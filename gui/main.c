/*
 * pmark-gui -- interactive SDL2 frontend.
 *
 * This file owns the window, input and view transform and nothing else:
 * every pixel that ends up in the saved file is produced by the core
 * (pm_doc_* for commits, pm_op_apply for live previews).
 *
 *   M marker   H highlighter   C crop   T text      1-8 colour   [ ] size
 *   wheel zoom   middle-drag / space-drag pan   0 fit
 *   Ctrl+Z undo   Ctrl+Y / Ctrl+Shift+Z redo   Ctrl+S save   Esc cancel
 *   closing with unsaved edits asks once (title bar); close again to discard
 *   crop: drag, then Enter      text: click, type, Enter (Shift+Enter = newline)
 */
#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmark.h"

typedef enum { TOOL_MARKER, TOOL_CROP, TOOL_TEXT } tool_kind;

static const pm_color PALETTE[8] = {
    {0xe5, 0x39, 0x35, 0xff}, {0xfb, 0x8c, 0x00, 0xff}, {0xfd, 0xd8, 0x35, 0xff},
    {0x43, 0xa0, 0x47, 0xff}, {0x1e, 0x88, 0xe5, 0xff}, {0x8e, 0x24, 0xaa, 0xff},
    {0x00, 0x00, 0x00, 0xff}, {0xff, 0xff, 0xff, 0xff},
};
static const char *const PALETTE_NAME[8] = {
    "red", "orange", "yellow", "green", "blue", "purple", "black", "white"};

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex; /* mirrors doc.canvas, plus any live preview */

    pm_doc   doc;
    pm_font *font; /* may be NULL: the text tool is then disabled */

    float zoom, pan_x, pan_y; /* screen = image * zoom + pan */

    tool_kind tool;
    int       color_idx;
    bool      highlighter;
    float     marker_w, text_px;

    pm_point *pts; /* stroke being drawn */
    size_t    npts, pts_cap;
    bool      drawing;

    bool     crop_drag, crop_have;
    pm_point crop_a, crop_b;

    bool     typing;
    char     text[2048];
    size_t   text_len;
    pm_point text_at;

    bool    panning;
    pm_rect preview; /* texture region currently showing uncommitted pixels */

    char in_path[1024], out_path[1024];
    bool modified, running;
    bool quit_armed; /* a close request was already refused once */
} app_t;

/* ------------------------------------------------------------ view / texture */

static pm_rect canvas_rect(const app_t *a)
{
    return (pm_rect){0, 0, a->doc.canvas.w, a->doc.canvas.h};
}

static pm_point to_image(const app_t *a, int sx, int sy)
{
    return (pm_point){((float)sx - a->pan_x) / a->zoom, ((float)sy - a->pan_y) / a->zoom};
}

static void fit_view(app_t *a)
{
    int ww, wh;
    SDL_GetRendererOutputSize(a->ren, &ww, &wh);
    float zx = (float)ww / (float)a->doc.canvas.w, zy = (float)wh / (float)a->doc.canvas.h;
    a->zoom = fminf(fminf(zx, zy) * 0.96f, 1.0f);
    a->pan_x = ((float)ww - (float)a->doc.canvas.w * a->zoom) * 0.5f;
    a->pan_y = ((float)wh - (float)a->doc.canvas.h * a->zoom) * 0.5f;
}

/* Copies a region of the committed canvas into the texture. */
static void tex_restore(app_t *a, pm_rect r)
{
    r = pm_rect_intersect(r, canvas_rect(a));
    if (pm_rect_empty(r)) return;
    SDL_Rect sr = {r.x, r.y, r.w, r.h};
    SDL_UpdateTexture(a->tex, &sr,
                      a->doc.canvas.px + ((size_t)r.y * a->doc.canvas.w + r.x) * 4,
                      a->doc.canvas.w * 4);
}

/* Full resync after anything that may have changed the canvas wholesale. */
static bool tex_sync(app_t *a)
{
    int tw = 0, th = 0;
    if (a->tex) SDL_QueryTexture(a->tex, NULL, NULL, &tw, &th);
    if (tw != a->doc.canvas.w || th != a->doc.canvas.h) {
        if (a->tex) SDL_DestroyTexture(a->tex);
        a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   a->doc.canvas.w, a->doc.canvas.h);
        if (!a->tex) {
            fprintf(stderr, "pmark-gui: texture %dx%d: %s\n", a->doc.canvas.w,
                    a->doc.canvas.h, SDL_GetError());
            return false;
        }
        SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
    }
    a->preview = (pm_rect){0, 0, 0, 0};
    tex_restore(a, canvas_rect(a));
    return true;
}

/* Shows `op` on screen without committing it. Only the union of the old and
 * new preview regions is re-rendered: that region of the canvas is copied to
 * a scratch image, the op is applied there (shifted into scratch coordinates;
 * integer shifts leave rasterisation bit-identical), and the result uploaded. */
static void preview_op(app_t *a, const pm_op *op)
{
    pm_rect nb = pm_rect_intersect(pm_op_bounds(op), canvas_rect(a));
    pm_rect region = pm_rect_union(a->preview, nb);
    if (pm_rect_empty(region)) return;

    pm_image scratch;
    if (pm_image_crop(&scratch, &a->doc.canvas, region) != PM_OK) return;

    pm_op t = *op;
    pm_point *shifted = NULL;
    if (op->kind == PM_OP_STROKE) {
        shifted = malloc(op->u.stroke.n * sizeof *shifted);
        if (shifted) {
            for (size_t i = 0; i < op->u.stroke.n; i++)
                shifted[i] = (pm_point){op->u.stroke.pts[i].x - (float)region.x,
                                        op->u.stroke.pts[i].y - (float)region.y};
            t.u.stroke.pts = shifted;
        }
    } else if (op->kind == PM_OP_TEXT) {
        t.u.text.x -= (float)region.x;
        t.u.text.y -= (float)region.y;
    }
    if (op->kind != PM_OP_STROKE || shifted) pm_op_apply(&scratch, &t);
    free(shifted);

    SDL_Rect sr = {region.x, region.y, region.w, region.h};
    SDL_UpdateTexture(a->tex, &sr, scratch.px, scratch.w * 4);
    pm_image_free(&scratch);
    a->preview = nb;
}

static void preview_clear(app_t *a)
{
    tex_restore(a, a->preview);
    a->preview = (pm_rect){0, 0, 0, 0};
}

/* ------------------------------------------------------------------- tools */

static pm_color cur_color(const app_t *a)
{
    pm_color c = PALETTE[a->color_idx];
    if (a->tool == TOOL_MARKER && a->highlighter) c.a = 0x66;
    return c;
}

static float cur_width(const app_t *a)
{
    return a->highlighter ? a->marker_w * 3.0f : a->marker_w;
}

static void report(const char *what, pm_status st)
{
    if (st != PM_OK) fprintf(stderr, "pmark-gui: %s: %s\n", what, pm_strerror(st));
}

static void stroke_add(app_t *a, pm_point p)
{
    if (a->npts == a->pts_cap) {
        size_t cap = a->pts_cap ? a->pts_cap * 2 : 256;
        pm_point *np = realloc(a->pts, cap * sizeof *np);
        if (!np) return;
        a->pts = np;
        a->pts_cap = cap;
    }
    a->pts[a->npts++] = p;
    pm_op op = {.kind = PM_OP_STROKE, .color = cur_color(a)};
    op.u.stroke.pts = a->pts;
    op.u.stroke.n = a->npts;
    op.u.stroke.width = cur_width(a);
    preview_op(a, &op);
}

static void stroke_commit(app_t *a)
{
    a->drawing = false;
    if (a->npts == 0) return;
    pm_status st = pm_doc_stroke(&a->doc, a->pts, a->npts, cur_width(a), cur_color(a));
    report("stroke", st);
    if (st == PM_OK) a->modified = true;
    preview_clear(a); /* the preview region now shows the committed canvas */
    a->npts = 0;
}

static pm_op text_op(app_t *a)
{
    pm_op op = {.kind = PM_OP_TEXT, .color = PALETTE[a->color_idx]};
    op.u.text.utf8 = a->text;
    op.u.text.x = a->text_at.x;
    op.u.text.y = a->text_at.y;
    op.u.text.size_px = a->text_px;
    op.u.text.font = a->font;
    return op;
}

static void text_preview(app_t *a)
{
    pm_op op = text_op(a);
    preview_op(a, &op);
}

static void text_end(app_t *a, bool commit)
{
    if (!a->typing) return;
    a->typing = false;
    SDL_StopTextInput();
    if (commit && a->text_len > 0) {
        pm_status st = pm_doc_text(&a->doc, a->font, a->text, a->text_at.x,
                                   a->text_at.y, a->text_px, PALETTE[a->color_idx]);
        report("text", st);
        if (st == PM_OK) a->modified = true;
    }
    preview_clear(a);
    a->text_len = 0;
    a->text[0] = 0;
}

static void text_begin(app_t *a, pm_point at)
{
    if (!a->font) {
        fprintf(stderr, "pmark-gui: text tool unavailable: %s\n", pm_strerror(PM_ERR_FONT));
        return;
    }
    text_end(a, true);
    a->typing = true;
    a->text_at = (pm_point){floorf(at.x), floorf(at.y - a->text_px * 0.5f)};
    a->text_len = 0;
    a->text[0] = 0;
    SDL_StartTextInput();
}

static void text_append(app_t *a, const char *s)
{
    size_t n = strlen(s);
    if (a->text_len + n >= sizeof a->text) return;
    memcpy(a->text + a->text_len, s, n + 1);
    a->text_len += n;
    text_preview(a);
}

static void text_backspace(app_t *a)
{
    if (a->text_len == 0) return;
    do a->text_len--; /* drop UTF-8 continuation bytes with their lead byte */
    while (a->text_len > 0 && ((unsigned char)a->text[a->text_len] & 0xC0) == 0x80);
    a->text[a->text_len] = 0;
    text_preview(a);
}

static pm_rect crop_rect(const app_t *a)
{
    float x0 = fminf(a->crop_a.x, a->crop_b.x), x1 = fmaxf(a->crop_a.x, a->crop_b.x);
    float y0 = fminf(a->crop_a.y, a->crop_b.y), y1 = fmaxf(a->crop_a.y, a->crop_b.y);
    pm_rect r = {(int)floorf(x0 + 0.5f), (int)floorf(y0 + 0.5f), 0, 0};
    r.w = (int)floorf(x1 + 0.5f) - r.x;
    r.h = (int)floorf(y1 + 0.5f) - r.y;
    return pm_rect_intersect(r, canvas_rect(a));
}

static void crop_commit(app_t *a)
{
    pm_rect r = crop_rect(a);
    a->crop_have = a->crop_drag = false;
    if (pm_rect_empty(r)) return;
    pm_status st = pm_doc_crop(&a->doc, r);
    report("crop", st);
    if (st != PM_OK) return;
    a->modified = true;
    if (!tex_sync(a)) a->running = false;
    fit_view(a);
}

/* Abandons whatever is half-done (used by Esc and by tool switches). */
static void cancel_pending(app_t *a)
{
    text_end(a, false);
    if (a->drawing) { a->drawing = false; a->npts = 0; preview_clear(a); }
    a->crop_have = a->crop_drag = false;
}

static void history(app_t *a, bool redo)
{
    cancel_pending(a);
    int w = a->doc.canvas.w, h = a->doc.canvas.h;
    pm_status st = redo ? pm_doc_redo(&a->doc) : pm_doc_undo(&a->doc);
    if (st != PM_OK) return; /* nothing to undo / redo */
    a->modified = true;
    if (!tex_sync(a)) a->running = false;
    if (w != a->doc.canvas.w || h != a->doc.canvas.h) fit_view(a);
}

static void save(app_t *a)
{
    pm_status st = pm_image_save(&a->doc.canvas, a->out_path, 0);
    report(a->out_path, st);
    if (st == PM_OK) {
        a->modified = false;
        fprintf(stderr, "pmark-gui: saved %s (%dx%d)\n", a->out_path,
                a->doc.canvas.w, a->doc.canvas.h);
    }
}

static void update_title(app_t *a)
{
    char buf[2304];
    const char *tool = a->tool == TOOL_CROP ? "crop"
                     : a->tool == TOOL_TEXT ? "text"
                     : a->highlighter ? "highlighter" : "marker";
    float size = a->tool == TOOL_TEXT ? a->text_px : cur_width(a);
    snprintf(buf, sizeof buf, "%s%s  |  %s  %s  %.0fpx  |  %dx%d @ %.0f%%  ->  %s",
             a->modified ? "* " : "", a->in_path, tool, PALETTE_NAME[a->color_idx],
             (double)size, a->doc.canvas.w, a->doc.canvas.h, (double)(a->zoom * 100.0f),
             a->out_path);
    SDL_SetWindowTitle(a->win, a->quit_armed
        ? "Unsaved changes: Ctrl+S to save, or close again to discard" : buf);
}

/* ------------------------------------------------------------------ events */

static void set_tool(app_t *a, tool_kind t, bool highlighter)
{
    cancel_pending(a);
    a->tool = t;
    a->highlighter = highlighter;
}

static void on_key(app_t *a, const SDL_KeyboardEvent *k)
{
    SDL_Keycode sym = k->keysym.sym;
    bool ctrl = (k->keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
    bool shift = (k->keysym.mod & KMOD_SHIFT) != 0;

    if (a->typing) { /* everything else is text and arrives as SDL_TEXTINPUT */
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (shift) text_append(a, "\n"); else text_end(a, true);
        } else if (sym == SDLK_ESCAPE) text_end(a, false);
        else if (sym == SDLK_BACKSPACE) text_backspace(a);
        return;
    }
    if (ctrl) {
        if (sym == SDLK_z) history(a, shift);
        else if (sym == SDLK_y) history(a, true);
        else if (sym == SDLK_s) save(a);
        return;
    }
    switch (sym) {
    case SDLK_m: set_tool(a, TOOL_MARKER, false); break;
    case SDLK_h: set_tool(a, TOOL_MARKER, true); break;
    case SDLK_c: set_tool(a, TOOL_CROP, false); break;
    case SDLK_t: set_tool(a, TOOL_TEXT, false); break;
    case SDLK_0: fit_view(a); break;
    case SDLK_ESCAPE: cancel_pending(a); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (a->crop_have) crop_commit(a);
        break;
    case SDLK_LEFTBRACKET: case SDLK_RIGHTBRACKET: {
        float f = sym == SDLK_RIGHTBRACKET ? 1.25f : 0.8f;
        if (a->tool == TOOL_TEXT) a->text_px = fminf(fmaxf(a->text_px * f, 8.0f), 1024.0f);
        else a->marker_w = fminf(fmaxf(a->marker_w * f, 1.0f), 256.0f);
        break;
    }
    default:
        if (sym >= SDLK_1 && sym <= SDLK_8) a->color_idx = (int)(sym - SDLK_1);
        break;
    }
}

static void on_mouse_down(app_t *a, const SDL_MouseButtonEvent *b)
{
    bool space = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_SPACE] != 0;
    if (b->button == SDL_BUTTON_MIDDLE || (b->button == SDL_BUTTON_LEFT && space && !a->typing)) {
        a->panning = true;
        return;
    }
    if (b->button != SDL_BUTTON_LEFT) return;
    pm_point p = to_image(a, b->x, b->y);
    switch (a->tool) {
    case TOOL_MARKER:
        a->drawing = true;
        a->npts = 0;
        stroke_add(a, p);
        break;
    case TOOL_CROP:
        a->crop_drag = true;
        a->crop_have = false;
        a->crop_a = a->crop_b = p;
        break;
    case TOOL_TEXT:
        text_begin(a, p); /* commits any text already being typed */
        break;
    }
}

static void on_mouse_move(app_t *a, const SDL_MouseMotionEvent *m)
{
    if (a->panning) {
        a->pan_x += (float)m->xrel;
        a->pan_y += (float)m->yrel;
        return;
    }
    pm_point p = to_image(a, m->x, m->y);
    if (a->drawing && a->npts > 0) {
        pm_point q = a->pts[a->npts - 1];
        /* thin out samples closer than ~1 screen pixel or 0.75 image pixel */
        float min_d = fmaxf(0.75f, 1.0f / a->zoom);
        if (hypotf(p.x - q.x, p.y - q.y) >= min_d) stroke_add(a, p);
    } else if (a->crop_drag) {
        a->crop_b = p;
    }
}

static void on_mouse_up(app_t *a, const SDL_MouseButtonEvent *b)
{
    if (a->panning && (b->button == SDL_BUTTON_MIDDLE || b->button == SDL_BUTTON_LEFT)) {
        a->panning = false;
        return;
    }
    if (b->button != SDL_BUTTON_LEFT) return;
    if (a->drawing) stroke_commit(a);
    if (a->crop_drag) {
        a->crop_drag = false;
        a->crop_have = !pm_rect_empty(crop_rect(a));
    }
}

static void on_wheel(app_t *a, const SDL_MouseWheelEvent *w)
{
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    pm_point p = to_image(a, mx, my); /* keep the pixel under the cursor fixed */
    a->zoom = fminf(fmaxf(a->zoom * powf(1.15f, (float)w->y), 0.02f), 64.0f);
    a->pan_x = (float)mx - p.x * a->zoom;
    a->pan_y = (float)my - p.y * a->zoom;
}

static void handle_event(app_t *a, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: /* first close with unsaved edits only warns (see title) */
        if (a->modified && !a->quit_armed) a->quit_armed = true;
        else a->running = false;
        return;
    case SDL_KEYDOWN:         on_key(a, &e->key); break;
    case SDL_TEXTINPUT:       if (a->typing) text_append(a, e->text.text); break;
    case SDL_MOUSEBUTTONDOWN: on_mouse_down(a, &e->button); break;
    case SDL_MOUSEMOTION:     on_mouse_move(a, &e->motion); break;
    case SDL_MOUSEBUTTONUP:   on_mouse_up(a, &e->button); break;
    case SDL_MOUSEWHEEL:      on_wheel(a, &e->wheel); break;
    default: return; /* window events etc. do not disarm the close guard */
    }
    a->quit_armed = false;
}

/* ------------------------------------------------------------------ render */

static SDL_FRect to_screen(const app_t *a, float x, float y, float w, float h)
{
    return (SDL_FRect){x * a->zoom + a->pan_x, y * a->zoom + a->pan_y, w * a->zoom, h * a->zoom};
}

static void render(app_t *a)
{
    int ww, wh;
    SDL_GetRendererOutputSize(a->ren, &ww, &wh);
    SDL_SetRenderDrawBlendMode(a->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(a->ren, 38, 38, 42, 255);
    SDL_RenderClear(a->ren);

    SDL_FRect dst = to_screen(a, 0, 0, (float)a->doc.canvas.w, (float)a->doc.canvas.h);
    SDL_SetRenderDrawColor(a->ren, 190, 190, 190, 255); /* shows through transparency */
    SDL_RenderFillRectF(a->ren, &dst);
    SDL_RenderCopyF(a->ren, a->tex, NULL, &dst);

    if (a->crop_drag || a->crop_have) {
        pm_rect r = crop_rect(a);
        SDL_FRect s = to_screen(a, (float)r.x, (float)r.y, (float)r.w, (float)r.h);
        SDL_FRect shade[4] = {
            {0, 0, (float)ww, s.y},
            {0, s.y + s.h, (float)ww, (float)wh - s.y - s.h},
            {0, s.y, s.x, s.h},
            {s.x + s.w, s.y, (float)ww - s.x - s.w, s.h},
        };
        SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 150);
        SDL_RenderFillRectsF(a->ren, shade, 4);
        SDL_SetRenderDrawColor(a->ren, 255, 255, 255, 255);
        SDL_RenderDrawRectF(a->ren, &s);
    }
    if (a->typing) { /* outline of the text box doubles as the caret */
        float tw, th;
        pm_text_measure(a->font, a->text_len ? a->text : " ", a->text_px, &tw, &th);
        SDL_FRect s = to_screen(a, a->text_at.x - 3, a->text_at.y - 3, tw + 6, th + 6);
        SDL_SetRenderDrawColor(a->ren, 255, 255, 255, 200);
        SDL_RenderDrawRectF(a->ren, &s);
    }
    SDL_RenderPresent(a->ren);
}

/* ---------------------------------------------------------------- selftest */

/* Drives the real event handlers with synthetic input (no display needed:
 * run with SDL_VIDEODRIVER=dummy). Coordinates are fractions of the canvas,
 * converted through the live view transform exactly as a user's would be. */
static void st_mouse(app_t *a, Uint32 type, float fx, float fy)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    int x = (int)(fx * (float)a->doc.canvas.w * a->zoom + a->pan_x);
    int y = (int)(fy * (float)a->doc.canvas.h * a->zoom + a->pan_y);
    e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x = x; e.motion.y = y; }
    else { e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y; }
    handle_event(a, &e);
}

static void st_drag(app_t *a, float x0, float y0, float x1, float y1)
{
    st_mouse(a, SDL_MOUSEBUTTONDOWN, x0, y0);
    for (int i = 1; i <= 24; i++) {
        float t = (float)i / 24.0f;
        st_mouse(a, SDL_MOUSEMOTION, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
    }
    st_mouse(a, SDL_MOUSEBUTTONUP, x1, y1);
    render(a);
}

static void st_key(app_t *a, SDL_Keycode sym, Uint16 mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = sym;
    e.key.keysym.mod = mod;
    handle_event(a, &e);
    render(a);
}

static void st_type(app_t *a, const char *s)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof e.text.text, "%s", s);
    handle_event(a, &e);
    render(a);
}

static int selftest(app_t *a)
{
    int w0 = a->doc.canvas.w;
    st_drag(a, 0.15f, 0.20f, 0.60f, 0.30f);             /* red marker        */
    st_key(a, SDLK_h, 0); st_key(a, SDLK_3, 0);
    st_drag(a, 0.15f, 0.62f, 0.75f, 0.62f);             /* yellow highlighter */
    st_key(a, SDLK_m, 0); st_key(a, SDLK_5, 0);
    st_drag(a, 0.90f, 0.90f, 0.95f, 0.95f);             /* blue: to be undone */
    st_key(a, SDLK_z, KMOD_CTRL);
    st_key(a, SDLK_t, 0); st_key(a, SDLK_8, 0);
    st_mouse(a, SDL_MOUSEBUTTONDOWN, 0.17f, 0.62f);
    st_mouse(a, SDL_MOUSEBUTTONUP, 0.17f, 0.62f);
    st_type(a, "typed in the GUIx");
    st_key(a, SDLK_BACKSPACE, 0);
    st_key(a, SDLK_RETURN, 0);
    st_key(a, SDLK_c, 0);
    st_drag(a, 0.05f, 0.05f, 0.85f, 0.85f);
    st_key(a, SDLK_RETURN, 0);                          /* apply crop         */
    st_key(a, SDLK_z, KMOD_CTRL);                       /* undo crop ...      */
    if (a->doc.canvas.w != w0) { fprintf(stderr, "selftest: undo crop failed\n"); return 1; }
    st_key(a, SDLK_y, KMOD_CTRL);                       /* ... and redo it    */
    st_key(a, SDLK_s, KMOD_CTRL);
    if (a->doc.n_applied != 4 || a->modified || a->doc.canvas.w >= w0) {
        fprintf(stderr, "selftest: unexpected end state (%zu ops)\n", a->doc.n_applied);
        return 1;
    }
    fprintf(stderr, "selftest ok\n");
    return 0;
}

/* -------------------------------------------------------------------- main */

static void default_out_path(app_t *a)
{
    const char *dot = strrchr(a->in_path, '.');
    const char *slash = strrchr(a->in_path, '/');
    size_t stem = (dot && (!slash || dot > slash)) ? (size_t)(dot - a->in_path)
                                                   : strlen(a->in_path);
    snprintf(a->out_path, sizeof a->out_path, "%.*s-marked.png", (int)stem, a->in_path);
}

int main(int argc, char **argv)
{
    bool want_selftest = false;
    const char *pos[2] = {NULL, NULL};
    int npos = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) want_selftest = true;
        else if (npos < 2) pos[npos++] = argv[i];
    }
    if (npos == 0) {
        fprintf(stderr, "usage: pmark-gui INPUT [OUTPUT]\n"
                        "  OUTPUT defaults to INPUT-marked.png; Ctrl+S writes it.\n");
        return 2;
    }

    static app_t app; /* zero-initialised */
    app_t *a = &app;
    a->marker_w = 8.0f;
    a->text_px = 48.0f;
    a->running = true;
    snprintf(a->in_path, sizeof a->in_path, "%s", pos[0]);
    if (pos[1]) snprintf(a->out_path, sizeof a->out_path, "%s", pos[1]);
    else default_out_path(a);

    pm_status st = pm_doc_open(&a->doc, a->in_path);
    if (st != PM_OK) {
        fprintf(stderr, "pmark-gui: %s: %s\n", a->in_path, pm_strerror(st));
        return 1;
    }
    if (pm_font_load_default(&a->font) != PM_OK)
        fprintf(stderr, "pmark-gui: warning: %s; text tool disabled\n", pm_strerror(PM_ERR_FONT));
    /* scale default tool sizes to the photo so they are usable on 12 MP shots */
    float ref = (float)(a->doc.canvas.w > a->doc.canvas.h ? a->doc.canvas.w : a->doc.canvas.h);
    a->marker_w = fmaxf(4.0f, roundf(ref / 160.0f));
    a->text_px = fmaxf(16.0f, roundf(ref / 28.0f));

    int rc = 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "pmark-gui: SDL_Init: %s\n", SDL_GetError());
        goto done;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    a->win = SDL_CreateWindow("pmark", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              1280, 800, SDL_WINDOW_RESIZABLE);
    if (a->win) a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!a->win || !a->ren) {
        fprintf(stderr, "pmark-gui: window: %s\n", SDL_GetError());
        goto done;
    }
    SDL_StopTextInput(); /* SDL starts with it on; we only want it while typing */
    if (!tex_sync(a)) goto done;
    fit_view(a);

    if (want_selftest) {
        rc = selftest(a);
        goto done;
    }

    update_title(a);
    render(a);
    while (a->running) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) break;
        do handle_event(a, &e); while (SDL_PollEvent(&e)); /* drain, then draw once */
        update_title(a);
        render(a);
    }
    rc = 0;

done:
    if (a->tex) SDL_DestroyTexture(a->tex);
    if (a->ren) SDL_DestroyRenderer(a->ren);
    if (a->win) SDL_DestroyWindow(a->win);
    SDL_Quit();
    free(a->pts);
    pm_doc_close(&a->doc);
    pm_font_free(a->font);
    return rc;
}
