/*
 * pmark-gui -- interactive SDL2 frontend.
 *
 * This file owns the window, input, toolbar and view transform. Every pixel
 * that ends up in the saved file is produced by the core: pm_doc_* commits,
 * and previews use the same rasterisers (pm_op_apply, or the incremental
 * pm_stroke builder, which is tested to match pm_draw_stroke exactly).
 *
 *   M marker  H highlighter  A arrow  B box  E ellipse  C crop  T text
 *   1-8 colour   [ ] size   O text outline   0 fit   wheel zoom
 *   middle-drag / space-drag pan     Ctrl+Z undo   Ctrl+Y / Ctrl+Shift+Z redo
 *   Ctrl+S save    Ctrl+Shift+S save as    Esc cancel
 *   crop: drag, then Enter      text: click, type, Enter (Shift+Enter = newline);
 *         drag the box to move it before committing
 */
#include <SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmark.h"

typedef enum { T_MARKER, T_HIGHLIGHT, T_ARROW, T_RECT, T_ELLIPSE, T_CROP, T_TEXT, T__N } tool_kind;
static const char *const TOOL_NAME[T__N] = {"Marker", "Highlight", "Arrow", "Box", "Ellipse", "Crop", "Text"};
static const SDL_Keycode TOOL_KEY[T__N] = {SDLK_m, SDLK_h, SDLK_a, SDLK_b, SDLK_e, SDLK_c, SDLK_t};

static const pm_color PALETTE[8] = {
    {0xe5, 0x39, 0x35, 0xff}, {0xfb, 0x8c, 0x00, 0xff}, {0xfd, 0xd8, 0x35, 0xff},
    {0x43, 0xa0, 0x47, 0xff}, {0x1e, 0x88, 0xe5, 0xff}, {0x8e, 0x24, 0xaa, 0xff},
    {0x00, 0x00, 0x00, 0xff}, {0xff, 0xff, 0xff, 0xff},
};

typedef enum { A_TOOL, A_COLOR, A_SMALLER, A_BIGGER, A_OUTLINE, A_UNDO, A_REDO, A_SAVE, A_SAVE_AS } act_kind;
typedef struct { pm_rect r; act_kind kind; int arg; } button;

typedef enum { P_NONE, P_SAVE_AS, P_CONFIRM } prompt_kind;

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;    /* mirrors doc.canvas, plus any live preview */
    SDL_Texture  *ui_tex; /* toolbar */
    SDL_Texture  *bar_tex;/* status / prompt bar */
    float dpi;            /* renderer pixels per window coordinate */
    int   tb_h, bar_h;    /* in renderer pixels */
    bool  ui_dirty;
    button buttons[40];
    int    n_buttons;

    pm_doc   doc;
    pm_font *font; /* owned by doc; NULL disables text tool and labels */

    float zoom, pan_x, pan_y; /* screen = image * zoom + pan (renderer pixels) */

    tool_kind tool;
    int       color_idx;
    float     line_w;
    pm_text_style ts;

    /* marker / highlighter in progress (incremental) */
    pm_stroke *stroke;
    pm_point  *pts;
    size_t     npts, pts_cap;
    pm_rect    stroke_dirty;
    pm_color   stroke_color;
    float      stroke_w;

    /* arrow / box / ellipse / crop drag */
    bool     dragging, crop_have;
    pm_point drag_a, drag_b;

    /* text in progress */
    bool     typing, text_drag;
    char     text[2048];
    size_t   text_len;
    pm_point text_at, text_grab;

    bool    panning;
    pm_rect preview; /* texture region currently showing uncommitted pixels */

    /* saving */
    char in_path[1024], out_path[1024];
    int  quality;
    bool sessions;      /* write OUTPUT.pmark next to OUTPUT on save */
    bool out_confirmed; /* out_path is ours to overwrite without asking */
    bool modified, running, quit_armed;
    prompt_kind prompt;
    char prompt_buf[1024];
    size_t prompt_len;
    char status[512];
    Uint32 status_until;
} app_t;

/* ------------------------------------------------------------- utilities */

static void set_status(app_t *a, Uint32 ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
    a->status_until = ms ? SDL_GetTicks() + ms : 0xFFFFFFFFu;
    a->ui_dirty = true;
}

static bool report(app_t *a, const char *what, pm_status st)
{
    if (st == PM_OK) return true;
    set_status(a, 6000, "%s: %s", what, pm_strerror(st));
    fprintf(stderr, "pmark-gui: %s: %s\n", what, pm_strerror(st));
    return false;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* path with its extension replaced (or appended) */
static void with_ext(char *out, size_t cap, const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    size_t stem = (dot && (!slash || dot > slash)) ? (size_t)(dot - path) : strlen(path);
    snprintf(out, cap, "%.*s%s", (int)stem, path, ext);
}

/* ------------------------------------------------------------ view / tex */

static pm_rect canvas_rect(const app_t *a)
{
    return (pm_rect){0, 0, a->doc.canvas.w, a->doc.canvas.h};
}

static pm_point to_image(const app_t *a, float sx, float sy)
{
    return (pm_point){(sx - a->pan_x) / a->zoom, (sy - a->pan_y) / a->zoom};
}

static void update_dpi(app_t *a)
{
    int ww, wh, pw, ph;
    SDL_GetWindowSize(a->win, &ww, &wh);
    SDL_GetRendererOutputSize(a->ren, &pw, &ph);
    float dpi = ww > 0 ? (float)pw / (float)ww : 1.0f;
    if (dpi < 0.5f) dpi = 1.0f;
    if (dpi != a->dpi) a->ui_dirty = true;
    a->dpi = dpi;
    a->tb_h = (int)lroundf(40.0f * dpi);
    a->bar_h = (int)lroundf(28.0f * dpi);
}

static void fit_view(app_t *a)
{
    int ww, wh;
    SDL_GetRendererOutputSize(a->ren, &ww, &wh);
    float avail_h = (float)(wh - a->tb_h - a->bar_h);
    float zx = (float)ww / (float)a->doc.canvas.w, zy = avail_h / (float)a->doc.canvas.h;
    a->zoom = fminf(fminf(zx, zy) * 0.96f, a->dpi); /* never upscale past 100% */
    a->pan_x = ((float)ww - (float)a->doc.canvas.w * a->zoom) * 0.5f;
    a->pan_y = (float)a->tb_h + (avail_h - (float)a->doc.canvas.h * a->zoom) * 0.5f;
}

static void tex_restore(app_t *a, pm_rect r)
{
    r = pm_rect_intersect(r, canvas_rect(a));
    if (pm_rect_empty(r)) return;
    SDL_Rect sr = {r.x, r.y, r.w, r.h};
    SDL_UpdateTexture(a->tex, &sr,
                      a->doc.canvas.px + ((size_t)r.y * a->doc.canvas.w + r.x) * 4,
                      a->doc.canvas.w * 4);
}

static void tex_upload(app_t *a, pm_rect r, const pm_image *scratch)
{
    SDL_Rect sr = {r.x, r.y, r.w, r.h};
    SDL_UpdateTexture(a->tex, &sr, scratch->px, scratch->w * 4);
}

static bool tex_sync(app_t *a)
{
    int tw = 0, th = 0;
    if (a->tex) SDL_QueryTexture(a->tex, NULL, NULL, &tw, &th);
    if (tw != a->doc.canvas.w || th != a->doc.canvas.h) {
        if (a->tex) SDL_DestroyTexture(a->tex);
        a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
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

/* Shows a whole op without committing it: the union of the old and new
 * preview regions is re-rendered from the canvas into a scratch image. */
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
    tex_upload(a, region, &scratch);
    pm_image_free(&scratch);
    a->preview = nb;
}

static void preview_clear(app_t *a)
{
    tex_restore(a, a->preview);
    a->preview = (pm_rect){0, 0, 0, 0};
}

/* ------------------------------------------------------------------ tools */

static pm_color tool_color(const app_t *a)
{
    pm_color c = PALETTE[a->color_idx];
    if (a->tool == T_HIGHLIGHT) c.a = 0x66;
    return c;
}

static float tool_width(const app_t *a)
{
    return a->tool == T_HIGHLIGHT ? a->line_w * 3.0f : a->line_w;
}

/* --- marker / highlighter: incremental preview */

static void marker_add(app_t *a, pm_point p)
{
    if (a->npts == a->pts_cap) {
        size_t cap = a->pts_cap ? a->pts_cap * 2 : 256;
        pm_point *np = realloc(a->pts, cap * sizeof *np);
        if (!np) return;
        a->pts = np;
        a->pts_cap = cap;
    }
    a->pts[a->npts++] = p;
    pm_rect r = pm_stroke_add(a->stroke, p);
    if (pm_rect_empty(r)) return;
    /* only the newest segment's box changes; composite the whole mask there */
    pm_image scratch;
    if (pm_image_crop(&scratch, &a->doc.canvas, r) != PM_OK) return;
    pm_stroke_composite(a->stroke, &scratch, r.x, r.y, r, a->stroke_color);
    tex_upload(a, r, &scratch);
    pm_image_free(&scratch);
    a->stroke_dirty = pm_rect_union(a->stroke_dirty, r);
}

static void marker_begin(app_t *a, pm_point p)
{
    a->stroke_w = tool_width(a);
    a->stroke_color = tool_color(a);
    if (!report(a, "stroke", pm_stroke_begin(&a->stroke, canvas_rect(a), a->stroke_w))) return;
    a->npts = 0;
    a->stroke_dirty = (pm_rect){0, 0, 0, 0};
    marker_add(a, p);
}

static void marker_finish(app_t *a, bool commit)
{
    if (!a->stroke) return;
    if (commit && a->npts > 0 &&
        report(a, "stroke", pm_doc_stroke(&a->doc, a->pts, a->npts, a->stroke_w, a->stroke_color)))
        a->modified = true;
    pm_stroke_end(a->stroke);
    a->stroke = NULL;
    a->npts = 0;
    tex_restore(a, a->stroke_dirty); /* now shows the committed canvas */
    a->ui_dirty = true;
}

/* --- arrow / box / ellipse */

static pm_rect drag_rect(const app_t *a)
{
    float x0 = fminf(a->drag_a.x, a->drag_b.x), x1 = fmaxf(a->drag_a.x, a->drag_b.x);
    float y0 = fminf(a->drag_a.y, a->drag_b.y), y1 = fmaxf(a->drag_a.y, a->drag_b.y);
    pm_rect r = {(int)floorf(x0 + 0.5f), (int)floorf(y0 + 0.5f), 0, 0};
    r.w = (int)floorf(x1 + 0.5f) - r.x;
    r.h = (int)floorf(y1 + 0.5f) - r.y;
    return r;
}

static pm_point *shape_points(const app_t *a, size_t *n)
{
    switch (a->tool) {
    case T_ARROW:   return pm_shape_arrow(a->drag_a, a->drag_b, a->line_w, n);
    case T_RECT:    return pm_shape_rect(drag_rect(a), n);
    case T_ELLIPSE: return pm_shape_ellipse(drag_rect(a), n);
    default:        return NULL;
    }
}

static void shape_preview(app_t *a)
{
    size_t n;
    pm_point *p = shape_points(a, &n);
    if (!p) return;
    pm_op op = {.kind = PM_OP_STROKE, .color = tool_color(a)};
    op.u.stroke.pts = p;
    op.u.stroke.n = n;
    op.u.stroke.width = tool_width(a);
    preview_op(a, &op);
    free(p);
}

static void shape_finish(app_t *a, bool commit)
{
    a->dragging = false;
    size_t n;
    pm_point *p = commit ? shape_points(a, &n) : NULL;
    if (p && report(a, "shape", pm_doc_stroke(&a->doc, p, n, tool_width(a), tool_color(a))))
        a->modified = true;
    free(p);
    preview_clear(a);
    a->ui_dirty = true;
}

/* --- text */

static pm_op text_op(app_t *a)
{
    pm_op op = {.kind = PM_OP_TEXT};
    op.u.text.utf8 = a->text;
    op.u.text.x = a->text_at.x;
    op.u.text.y = a->text_at.y;
    op.u.text.style = a->ts;
    op.u.text.style.color = PALETTE[a->color_idx];
    op.u.text.style.outline = a->color_idx == 6 ? PALETTE[7] : PALETTE[6]; /* contrast */
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
    a->typing = a->text_drag = false;
    if (a->prompt == P_NONE) SDL_StopTextInput();
    if (commit && a->text_len > 0) {
        pm_op op = text_op(a);
        if (report(a, "text", pm_doc_text(&a->doc, a->font, a->text, op.u.text.x, op.u.text.y,
                                          &op.u.text.style)))
            a->modified = true;
    }
    preview_clear(a);
    a->text_len = 0;
    a->text[0] = 0;
    a->ui_dirty = true;
}

static pm_rect text_box(const app_t *a)
{
    float tw, th;
    pm_text_measure(a->font, a->text_len ? a->text : " ", a->ts.size_px, &tw, &th);
    return (pm_rect){(int)a->text_at.x - 4, (int)a->text_at.y - 4, (int)tw + 8, (int)th + 8};
}

static bool in_rect(pm_rect r, pm_point p)
{
    return p.x >= (float)r.x && p.x < (float)(r.x + r.w) && p.y >= (float)r.y &&
           p.y < (float)(r.y + r.h);
}

static void text_begin(app_t *a, pm_point at)
{
    if (!a->font) { set_status(a, 4000, "Text tool unavailable: %s", pm_strerror(PM_ERR_FONT)); return; }
    text_end(a, true);
    a->typing = true;
    a->text_at = (pm_point){floorf(at.x), floorf(at.y - a->ts.size_px * 0.5f)};
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

static void utf8_pop(char *s, size_t *len)
{
    if (*len == 0) return;
    do (*len)--; /* drop UTF-8 continuation bytes with their lead byte */
    while (*len > 0 && ((unsigned char)s[*len] & 0xC0) == 0x80);
    s[*len] = 0;
}

/* --- crop */

static void crop_commit(app_t *a)
{
    pm_rect r = pm_rect_intersect(drag_rect(a), canvas_rect(a));
    a->crop_have = a->dragging = false;
    if (pm_rect_empty(r)) return;
    if (!report(a, "crop", pm_doc_crop(&a->doc, r))) return;
    a->modified = true;
    if (!tex_sync(a)) a->running = false;
    fit_view(a);
    a->ui_dirty = true;
}

static void cancel_pending(app_t *a)
{
    text_end(a, false);
    marker_finish(a, false);
    if (a->dragging && a->tool != T_CROP) shape_finish(a, false);
    a->crop_have = a->dragging = false;
}

static void set_tool(app_t *a, tool_kind t)
{
    if (t == a->tool) return;
    text_end(a, true); /* switching away keeps typed text */
    cancel_pending(a);
    a->tool = t;
    a->ui_dirty = true;
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
    a->ui_dirty = true;
}

static void resize(app_t *a, bool bigger)
{
    float f = bigger ? 1.25f : 0.8f;
    if (a->tool == T_TEXT) {
        a->ts.size_px = fminf(fmaxf(roundf(a->ts.size_px * f), 8.0f), 1024.0f);
        if (a->typing) text_preview(a);
    } else {
        a->line_w = fminf(fmaxf(roundf(a->line_w * f), 1.0f), 512.0f);
    }
    a->ui_dirty = true;
}

/* ------------------------------------------------------------------ saving */

static void sidecar_path(const app_t *a, char *out, size_t cap)
{
    with_ext(out, cap, a->out_path, ".pmark");
}

/* Save As changes the name only: the output keeps its format, so the
 * extension is fixed and shown after the prompt. */
static const char *out_ext(const app_t *a)
{
    const char *dot = strrchr(a->out_path, '.'), *slash = strrchr(a->out_path, '/');
    return (dot && (!slash || dot > slash)) ? dot : ".png";
}

static void prompt_open(app_t *a, prompt_kind k, const char *initial);

static void save_as_open(app_t *a)
{
    char stem[1024];
    with_ext(stem, sizeof stem, a->out_path, "");
    prompt_open(a, P_SAVE_AS, stem);
}

static void prompt_open(app_t *a, prompt_kind k, const char *initial)
{
    text_end(a, true);
    a->prompt = k;
    snprintf(a->prompt_buf, sizeof a->prompt_buf, "%s", initial ? initial : "");
    a->prompt_len = strlen(a->prompt_buf);
    if (k == P_SAVE_AS) SDL_StartTextInput();
    a->ui_dirty = true;
}

static void prompt_close(app_t *a)
{
    a->prompt = P_NONE;
    SDL_StopTextInput();
    a->ui_dirty = true;
}

/* The one save path. Asks before replacing a file this window did not
 * write; never writes a session for an output that replaced its own base. */
static void save(app_t *a, bool confirmed)
{
    cancel_pending(a);
    char side[1100];
    sidecar_path(a, side, sizeof side);
    bool replaces_input = a->doc.base_path && pm_path_same_file(a->out_path, a->doc.base_path);
    bool want_session = a->sessions && a->doc.base_path && !replaces_input;

    if (!confirmed && !a->out_confirmed) {
        bool img_exists = pm_path_exists(a->out_path);
        bool side_exists = want_session && pm_path_exists(side);
        if (replaces_input) {
            set_status(a, 0, "%s is the original photo. Enter replaces it (edits become permanent), Esc cancels",
                       base_name(a->out_path));
            prompt_open(a, P_CONFIRM, NULL);
            return;
        }
        if (img_exists || side_exists) {
            set_status(a, 0, "%s exists. Enter replaces it, Esc cancels",
                       base_name(img_exists ? a->out_path : side));
            prompt_open(a, P_CONFIRM, NULL);
            return;
        }
    }
    if (!report(a, base_name(a->out_path), pm_doc_export(&a->doc, a->out_path, a->quality, true)))
        return;
    a->out_confirmed = true;
    a->modified = false;
    a->quit_armed = false;
    bool sess_ok = want_session && pm_session_save(&a->doc, side, true) == PM_OK;
    if (want_session && !sess_ok)
        set_status(a, 6000, "Saved %s, but the session file could not be written",
                   base_name(a->out_path));
    else
        set_status(a, 4000, "Saved %s (%dx%d)%s%s", base_name(a->out_path), a->doc.canvas.w,
                   a->doc.canvas.h, sess_ok ? " + " : "", sess_ok ? base_name(side) : "");
    fprintf(stderr, "pmark-gui: saved %s\n", a->out_path);
}

static void prompt_submit(app_t *a)
{
    if (a->prompt == P_CONFIRM) {
        prompt_close(a);
        save(a, true);
        return;
    }
    /* P_SAVE_AS: the typed text is a name; the extension stays the output's.
     * A typed image extension is dropped rather than doubled ("x.jpg" ->
     * "x.png" for PNG output); other dots ("v1.2") are part of the name. */
    char name[1024];
    snprintf(name, sizeof name, "%s", a->prompt_buf);
    size_t n = strlen(name);
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t')) name[--n] = 0;
    if (pm_format_from_path(name) != PM_FMT_UNKNOWN) {
        with_ext(name, sizeof name, a->prompt_buf, "");
        n = strlen(name);
    }
    const char *base = strrchr(name, '/');
    if (n == 0 || (base && base[1] == 0) || !strcmp(base ? base + 1 : name, ".")) {
        set_status(a, 5000, "Type a file name");
        return;
    }
    char path[1100];
    int len = snprintf(path, sizeof path, "%s%s", name, out_ext(a));
    if (len < 0 || (size_t)len >= sizeof a->out_path) {
        set_status(a, 5000, "That name is too long");
        return;
    }
    prompt_close(a);
    if (strcmp(path, a->out_path) != 0) {
        memcpy(a->out_path, path, (size_t)len + 1); /* len checked above */
        a->out_confirmed = false;
    }
    save(a, false);
}

/* ---------------------------------------------------------------- toolbar */

static void fill(pm_image *im, pm_rect r, pm_color c)
{
    r = pm_rect_intersect(r, (pm_rect){0, 0, im->w, im->h});
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            memcpy(im->px + ((size_t)y * im->w + x) * 4, &c, 4);
}

static int label(app_t *a, pm_image *im, const char *s, int x, int h, pm_color c, bool draw)
{
    if (!a->font) return 0;
    float px = 15.0f * a->dpi, w, th;
    pm_text_measure(a->font, s, px, &w, &th);
    if (draw) {
        pm_text_style st = {px, c, 0, c};
        pm_text_draw(im, a->font, s, (float)x, ((float)h - th) * 0.5f, &st);
    }
    return (int)ceilf(w);
}

static const pm_color UI_BG = {0x24, 0x24, 0x28, 0xff}, UI_BTN = {0x38, 0x38, 0x3e, 0xff},
                      UI_ON = {0x4a, 0x6f, 0xa5, 0xff}, UI_TXT = {0xee, 0xee, 0xee, 0xff},
                      UI_DIM = {0x80, 0x80, 0x88, 0xff};

static void add_button(app_t *a, pm_image *im, int *x, const char *text, act_kind k, int arg, bool on,
                       bool enabled)
{
    int pad = (int)(10 * a->dpi), m = (int)(6 * a->dpi);
    int w = label(a, im, text, 0, 0, UI_TXT, false) + 2 * pad;
    if (!a->font) w = (int)(28 * a->dpi);
    pm_rect r = {*x, m, w, a->tb_h - 2 * m};
    fill(im, r, on ? UI_ON : UI_BTN);
    label(a, im, text, *x + pad, a->tb_h, enabled ? UI_TXT : UI_DIM, true);
    if (a->n_buttons < 40) a->buttons[a->n_buttons++] = (button){r, k, arg};
    *x += w + (int)(4 * a->dpi);
}

static void gap(app_t *a, int *x) { *x += (int)(10 * a->dpi); }

static void build_toolbar(app_t *a, int ww)
{
    pm_image im;
    if (pm_image_create(&im, ww, a->tb_h, UI_BG) != PM_OK) return;
    a->n_buttons = 0;
    int x = (int)(8 * a->dpi), m = (int)(6 * a->dpi);
    for (int t = 0; t < T__N; t++)
        add_button(a, &im, &x, TOOL_NAME[t], A_TOOL, t, (int)a->tool == t, t != T_TEXT || a->font);
    gap(a, &x);
    int sw = a->tb_h - 2 * m;
    for (int i = 0; i < 8; i++) {
        pm_rect r = {x, m, sw, sw};
        fill(&im, r, a->color_idx == i ? UI_TXT : UI_BTN);
        fill(&im, (pm_rect){r.x + 3, r.y + 3, r.w - 6, r.h - 6}, PALETTE[i]);
        a->buttons[a->n_buttons++] = (button){r, A_COLOR, i};
        x += sw + (int)(3 * a->dpi);
    }
    gap(a, &x);
    char size[48];
    snprintf(size, sizeof size, "%s %.0f", a->tool == T_TEXT ? "Size" : "Width",
             (double)(a->tool == T_TEXT ? a->ts.size_px : tool_width(a)));
    add_button(a, &im, &x, "-", A_SMALLER, 0, false, true);
    x += label(a, &im, size, x, a->tb_h, UI_TXT, true) + (int)(4 * a->dpi);
    add_button(a, &im, &x, "+", A_BIGGER, 0, false, true);
    add_button(a, &im, &x, "Outline", A_OUTLINE, 0, a->ts.outline_px > 0, true);
    gap(a, &x);
    add_button(a, &im, &x, "Undo", A_UNDO, 0, false, pm_doc_can_undo(&a->doc));
    add_button(a, &im, &x, "Redo", A_REDO, 0, false, pm_doc_can_redo(&a->doc));
    gap(a, &x);
    add_button(a, &im, &x, a->modified ? "Save*" : "Save", A_SAVE, 0, false, true);
    add_button(a, &im, &x, "Save as", A_SAVE_AS, 0, false, true);

    if (a->ui_tex) SDL_DestroyTexture(a->ui_tex);
    a->ui_tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im.w, im.h);
    if (a->ui_tex) SDL_UpdateTexture(a->ui_tex, NULL, im.px, im.w * 4);
    pm_image_free(&im);
}

static void build_bar(app_t *a, int ww)
{
    char line[2400];
    if (a->prompt == P_SAVE_AS)
        snprintf(line, sizeof line, "Save as: %s|%s   (Enter save, Esc cancel)", a->prompt_buf, out_ext(a));
    else if (a->quit_armed)
        snprintf(line, sizeof line, "Unsaved changes. Ctrl+S saves; close again to discard them");
    else if (a->status[0] && SDL_GetTicks() < a->status_until)
        snprintf(line, sizeof line, "%s", a->status);
    else
        snprintf(line, sizeof line, "%s  %dx%d  %.0f%%  ->  %s%s", base_name(a->in_path),
                 a->doc.canvas.w, a->doc.canvas.h, (double)(a->zoom / a->dpi * 100.0f),
                 base_name(a->out_path), a->modified ? "  (unsaved)" : "");
    pm_image im;
    if (pm_image_create(&im, ww, a->bar_h, UI_BG) != PM_OK) return;
    label(a, &im, line, (int)(10 * a->dpi), a->bar_h,
          a->prompt != P_NONE || a->quit_armed ? (pm_color){0xff, 0xd5, 0x4f, 0xff} : UI_TXT, true);
    if (a->bar_tex) SDL_DestroyTexture(a->bar_tex);
    a->bar_tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im.w, im.h);
    if (a->bar_tex) SDL_UpdateTexture(a->bar_tex, NULL, im.px, im.w * 4);
    pm_image_free(&im);
}

static void toolbar_click(app_t *a, float x, float y)
{
    for (int i = 0; i < a->n_buttons; i++) {
        const button *b = &a->buttons[i];
        if (!in_rect(b->r, (pm_point){x, y})) continue;
        switch (b->kind) {
        case A_TOOL:    set_tool(a, (tool_kind)b->arg); break;
        case A_COLOR:   a->color_idx = b->arg; if (a->typing) text_preview(a); break;
        case A_SMALLER: resize(a, false); break;
        case A_BIGGER:  resize(a, true); break;
        case A_OUTLINE:
            a->ts.outline_px = a->ts.outline_px > 0 ? 0 : fmaxf(2.0f, roundf(a->ts.size_px / 16.0f));
            if (a->typing) text_preview(a);
            break;
        case A_UNDO:    history(a, false); break;
        case A_REDO:    history(a, true); break;
        case A_SAVE:    save(a, false); break;
        case A_SAVE_AS: save_as_open(a); break;
        }
        a->ui_dirty = true;
        return;
    }
}

/* ------------------------------------------------------------------ events */

static void on_key(app_t *a, const SDL_KeyboardEvent *k)
{
    SDL_Keycode sym = k->keysym.sym;
    bool ctrl = (k->keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
    bool shift = (k->keysym.mod & KMOD_SHIFT) != 0;
    a->ui_dirty = true;

    if (a->prompt != P_NONE) {
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) prompt_submit(a);
        else if (sym == SDLK_ESCAPE) { prompt_close(a); set_status(a, 2000, "Cancelled"); }
        else if (sym == SDLK_BACKSPACE && a->prompt == P_SAVE_AS) utf8_pop(a->prompt_buf, &a->prompt_len);
        return;
    }
    if (a->typing) { /* everything else arrives as SDL_TEXTINPUT */
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
            if (shift) text_append(a, "\n"); else text_end(a, true);
        } else if (sym == SDLK_ESCAPE) text_end(a, false);
        else if (sym == SDLK_BACKSPACE) { utf8_pop(a->text, &a->text_len); text_preview(a); }
        else if (ctrl && sym == SDLK_s) { text_end(a, true); save(a, false); }
        return;
    }
    if (ctrl) {
        if (sym == SDLK_z) history(a, shift);
        else if (sym == SDLK_y) history(a, true);
        else if (sym == SDLK_s && shift) save_as_open(a);
        else if (sym == SDLK_s) save(a, false);
        return;
    }
    for (int t = 0; t < T__N; t++)
        if (sym == TOOL_KEY[t]) { set_tool(a, (tool_kind)t); return; }
    switch (sym) {
    case SDLK_0: fit_view(a); break;
    case SDLK_o:
        a->ts.outline_px = a->ts.outline_px > 0 ? 0 : fmaxf(2.0f, roundf(a->ts.size_px / 16.0f));
        break;
    case SDLK_ESCAPE: cancel_pending(a); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: if (a->crop_have) crop_commit(a); break;
    case SDLK_LEFTBRACKET: resize(a, false); break;
    case SDLK_RIGHTBRACKET: resize(a, true); break;
    default:
        if (sym >= SDLK_1 && sym <= SDLK_8) a->color_idx = (int)(sym - SDLK_1);
        break;
    }
}

static void on_mouse_down(app_t *a, const SDL_MouseButtonEvent *b)
{
    float x = (float)b->x * a->dpi, y = (float)b->y * a->dpi;
    if (a->prompt != P_NONE) return;
    if (y < (float)a->tb_h && b->button == SDL_BUTTON_LEFT) { toolbar_click(a, x, y); return; }
    bool space = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_SPACE] != 0;
    if (b->button == SDL_BUTTON_MIDDLE || (b->button == SDL_BUTTON_LEFT && space && !a->typing)) {
        a->panning = true;
        return;
    }
    if (b->button != SDL_BUTTON_LEFT) return;
    pm_point p = to_image(a, x, y);
    switch (a->tool) {
    case T_MARKER: case T_HIGHLIGHT:
        marker_begin(a, p);
        break;
    case T_ARROW: case T_RECT: case T_ELLIPSE: case T_CROP:
        a->dragging = true;
        a->crop_have = false;
        a->drag_a = a->drag_b = p;
        break;
    case T_TEXT:
        if (a->typing && in_rect(text_box(a), p)) { /* grab the box to move it */
            a->text_drag = true;
            a->text_grab = (pm_point){p.x - a->text_at.x, p.y - a->text_at.y};
        } else {
            text_begin(a, p);
        }
        break;
    default: break;
    }
}

static void on_mouse_move(app_t *a, const SDL_MouseMotionEvent *m)
{
    if (a->panning) {
        a->pan_x += (float)m->xrel * a->dpi;
        a->pan_y += (float)m->yrel * a->dpi;
        return;
    }
    pm_point p = to_image(a, (float)m->x * a->dpi, (float)m->y * a->dpi);
    if (a->stroke && a->npts > 0) {
        pm_point q = a->pts[a->npts - 1];
        /* thin out samples closer than ~1 screen pixel or 0.75 image pixel */
        if (hypotf(p.x - q.x, p.y - q.y) >= fmaxf(0.75f, 1.0f / a->zoom)) marker_add(a, p);
    } else if (a->dragging) {
        a->drag_b = p;
        if (a->tool != T_CROP) shape_preview(a);
    } else if (a->text_drag) {
        a->text_at = (pm_point){roundf(p.x - a->text_grab.x), roundf(p.y - a->text_grab.y)};
        text_preview(a);
    }
}

static void on_mouse_up(app_t *a, const SDL_MouseButtonEvent *b)
{
    if (a->panning && (b->button == SDL_BUTTON_MIDDLE || b->button == SDL_BUTTON_LEFT)) {
        a->panning = false;
        return;
    }
    if (b->button != SDL_BUTTON_LEFT) return;
    if (a->stroke) marker_finish(a, true);
    if (a->text_drag) a->text_drag = false;
    if (a->dragging) {
        if (a->tool == T_CROP) {
            a->dragging = false;
            a->crop_have = !pm_rect_empty(pm_rect_intersect(drag_rect(a), canvas_rect(a)));
            if (a->crop_have) set_status(a, 0, "Enter applies the crop, Esc cancels");
        } else {
            shape_finish(a, true);
        }
    }
}

static void on_wheel(app_t *a, const SDL_MouseWheelEvent *w)
{
    int mx, my;
    SDL_GetMouseState(&mx, &my);
    float sx = (float)mx * a->dpi, sy = (float)my * a->dpi;
    pm_point p = to_image(a, sx, sy); /* keep the pixel under the cursor fixed */
    a->zoom = fminf(fmaxf(a->zoom * powf(1.15f, (float)w->y), 0.02f), 64.0f);
    a->pan_x = sx - p.x * a->zoom;
    a->pan_y = sy - p.y * a->zoom;
    a->ui_dirty = true;
}

static void handle_event(app_t *a, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: /* first close with unsaved edits only warns */
        if (a->modified && !a->quit_armed) { a->quit_armed = true; a->ui_dirty = true; }
        else a->running = false;
        return;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { update_dpi(a); a->ui_dirty = true; }
        return; /* window events do not disarm the close guard */
    case SDL_KEYDOWN:         on_key(a, &e->key); break;
    case SDL_TEXTINPUT:
        if (a->prompt == P_SAVE_AS) {
            size_t n = strlen(e->text.text);
            if (a->prompt_len + n < sizeof a->prompt_buf) {
                memcpy(a->prompt_buf + a->prompt_len, e->text.text, n + 1);
                a->prompt_len += n;
            }
            a->ui_dirty = true;
        } else if (a->typing) text_append(a, e->text.text);
        break;
    case SDL_MOUSEBUTTONDOWN: on_mouse_down(a, &e->button); break;
    case SDL_MOUSEMOTION:     on_mouse_move(a, &e->motion); return;
    case SDL_MOUSEBUTTONUP:   on_mouse_up(a, &e->button); break;
    case SDL_MOUSEWHEEL:      on_wheel(a, &e->wheel); break;
    default: return;
    }
    if (a->quit_armed) a->ui_dirty = true;
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
    static bool status_shown;
    bool status_live = a->status[0] && SDL_GetTicks() < a->status_until;
    if (status_shown != status_live) a->ui_dirty = true; /* let messages expire */
    status_shown = status_live;
    if (a->ui_dirty) { build_toolbar(a, ww); build_bar(a, ww); a->ui_dirty = false; }

    SDL_SetRenderDrawBlendMode(a->ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(a->ren, 38, 38, 42, 255);
    SDL_RenderClear(a->ren);

    SDL_FRect dst = to_screen(a, 0, 0, (float)a->doc.canvas.w, (float)a->doc.canvas.h);
    SDL_SetRenderDrawColor(a->ren, 190, 190, 190, 255); /* shows through transparency */
    SDL_RenderFillRectF(a->ren, &dst);
    SDL_RenderCopyF(a->ren, a->tex, NULL, &dst);

    if ((a->dragging && a->tool == T_CROP) || a->crop_have) {
        pm_rect r = pm_rect_intersect(drag_rect(a), canvas_rect(a));
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
    if (a->typing) { /* box outline: shows where text goes and is the drag handle */
        pm_rect b = text_box(a);
        SDL_FRect s = to_screen(a, (float)b.x, (float)b.y, (float)b.w, (float)b.h);
        SDL_SetRenderDrawColor(a->ren, 255, 255, 255, 200);
        SDL_RenderDrawRectF(a->ren, &s);
    }
    if (a->ui_tex) SDL_RenderCopy(a->ren, a->ui_tex, NULL, &(SDL_Rect){0, 0, ww, a->tb_h});
    if (a->bar_tex) SDL_RenderCopy(a->ren, a->bar_tex, NULL, &(SDL_Rect){0, wh - a->bar_h, ww, a->bar_h});
    SDL_RenderPresent(a->ren);
}

static void update_title(app_t *a)
{
    char buf[1200];
    snprintf(buf, sizeof buf, "%s%s - pmark", a->modified ? "* " : "", base_name(a->in_path));
    SDL_SetWindowTitle(a->win, buf);
}

/* ---------------------------------------------------------------- selftest */

/* Drives the real event handlers with synthetic input (no display needed:
 * run with SDL_VIDEODRIVER=dummy). Coordinates are fractions of the canvas,
 * converted through the live view transform as a user's clicks would be. */
static void st_send(app_t *a, SDL_Event *e) { handle_event(a, e); render(a); }

static void st_mouse(app_t *a, Uint32 type, float fx, float fy)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    int x = (int)((fx * (float)a->doc.canvas.w * a->zoom + a->pan_x) / a->dpi);
    int y = (int)((fy * (float)a->doc.canvas.h * a->zoom + a->pan_y) / a->dpi);
    e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x = x; e.motion.y = y; }
    else { e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y; }
    st_send(a, &e);
}

static void st_drag(app_t *a, float x0, float y0, float x1, float y1)
{
    st_mouse(a, SDL_MOUSEBUTTONDOWN, x0, y0);
    for (int i = 1; i <= 24; i++) {
        float t = (float)i / 24.0f;
        st_mouse(a, SDL_MOUSEMOTION, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
    }
    st_mouse(a, SDL_MOUSEBUTTONUP, x1, y1);
}

static void st_key(app_t *a, SDL_Keycode sym, Uint16 mod)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = sym;
    e.key.keysym.mod = mod;
    st_send(a, &e);
}

static void st_type(app_t *a, const char *s)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_TEXTINPUT;
    snprintf(e.text.text, sizeof e.text.text, "%s", s);
    st_send(a, &e);
}

static bool st_click_button(app_t *a, act_kind k, int arg)
{
    for (int i = 0; i < a->n_buttons; i++)
        if (a->buttons[i].kind == k && a->buttons[i].arg == arg) {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_MOUSEBUTTONDOWN;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.x = (int)((float)(a->buttons[i].r.x + 2) / a->dpi);
            e.button.y = (int)((float)(a->buttons[i].r.y + 2) / a->dpi);
            st_send(a, &e);
            e.type = SDL_MOUSEBUTTONUP;
            st_send(a, &e);
            return true;
        }
    return false;
}

#define ST_CHECK(c) do { if (!(c)) { fprintf(stderr, "selftest: failed: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int selftest(app_t *a, const char *save_as)
{
    int w0 = a->doc.canvas.w;
    st_drag(a, 0.15f, 0.20f, 0.60f, 0.30f);                    /* red marker (keys)    */
    ST_CHECK(st_click_button(a, A_TOOL, T_HIGHLIGHT));         /* toolbar clicks       */
    ST_CHECK(st_click_button(a, A_COLOR, 2) && a->color_idx == 2);
    st_drag(a, 0.15f, 0.62f, 0.75f, 0.62f);                    /* yellow highlighter   */
    st_key(a, SDLK_a, 0); st_key(a, SDLK_5, 0);
    st_drag(a, 0.30f, 0.90f, 0.62f, 0.45f);                    /* blue arrow           */
    st_key(a, SDLK_e, 0); st_key(a, SDLK_1, 0);
    st_drag(a, 0.66f, 0.05f, 0.95f, 0.50f);                    /* red ellipse          */
    st_key(a, SDLK_b, 0);
    st_drag(a, 0.90f, 0.90f, 0.95f, 0.95f);                    /* box: to be undone    */
    ST_CHECK(st_click_button(a, A_UNDO, 0) && a->doc.n_applied == 4);
    st_key(a, SDLK_t, 0); st_key(a, SDLK_8, 0); st_key(a, SDLK_o, 0);
    st_mouse(a, SDL_MOUSEBUTTONDOWN, 0.05f, 0.80f);
    st_mouse(a, SDL_MOUSEBUTTONUP, 0.05f, 0.80f);
    st_type(a, "typed in the GUIx");
    st_key(a, SDLK_BACKSPACE, 0);
    pm_point before = a->text_at;                              /* drag the box         */
    float gx = (a->text_at.x + 6) / (float)a->doc.canvas.w, gy = (a->text_at.y + 6) / (float)a->doc.canvas.h;
    st_drag(a, gx, gy, gx + 0.10f, gy - 0.05f);
    ST_CHECK(a->typing && a->text_at.x > before.x + 10 && a->text_at.y < before.y - 5);
    st_key(a, SDLK_RETURN, 0);
    ST_CHECK(a->doc.n_applied == 5);
    st_key(a, SDLK_c, 0);
    st_drag(a, 0.04f, 0.04f, 0.96f, 0.96f);
    st_key(a, SDLK_RETURN, 0);                                 /* apply crop           */
    st_key(a, SDLK_z, KMOD_CTRL);                              /* undo crop ...        */
    ST_CHECK(a->doc.canvas.w == w0);
    st_key(a, SDLK_y, KMOD_CTRL);                              /* ... and redo it      */
    ST_CHECK(a->doc.canvas.w < w0 && a->doc.n_applied == 6);

    /* Save: the output already exists, so the first attempt must ask */
    st_key(a, SDLK_s, KMOD_CTRL);
    ST_CHECK(a->prompt == P_CONFIRM && a->modified);
    st_key(a, SDLK_RETURN, 0);
    ST_CHECK(a->prompt == P_NONE && !a->modified && a->out_confirmed);
    char side[1100];
    sidecar_path(a, side, sizeof side);
    ST_CHECK(pm_path_exists(side));

    /* the session reopens to the same pixels */
    pm_doc re;
    ST_CHECK(pm_session_load(&re, side, 0) == PM_OK);
    ST_CHECK(re.canvas.w == a->doc.canvas.w && re.canvas.h == a->doc.canvas.h &&
             !memcmp(re.canvas.px, a->doc.canvas.px, (size_t)re.canvas.w * re.canvas.h * 4));
    pm_doc_close(&re);

    /* Save as changes the name, never the format: the prompt opens on the
     * name without its extension, an empty name is refused, and a typed
     * image extension is replaced by the output's own. */
    char ext[16], want[1100], typed[1100];
    snprintf(ext, sizeof ext, "%s", out_ext(a));
    snprintf(want, sizeof want, "%s%s", save_as, ext);
    snprintf(typed, sizeof typed, "%s.jpg", save_as);
    st_key(a, SDLK_s, KMOD_CTRL | KMOD_SHIFT);
    ST_CHECK(a->prompt == P_SAVE_AS && !strstr(a->prompt_buf, ext));
    while (a->prompt_len) st_key(a, SDLK_BACKSPACE, 0);
    st_key(a, SDLK_RETURN, 0);
    ST_CHECK(a->prompt == P_SAVE_AS); /* empty: refused, prompt stays open */
    st_type(a, typed);
    st_key(a, SDLK_RETURN, 0);
    ST_CHECK(a->prompt == P_NONE && pm_path_exists(want) && !strcmp(a->out_path, want));
    st_key(a, SDLK_m, 0);
    st_drag(a, 0.2f, 0.2f, 0.3f, 0.3f);
    st_key(a, SDLK_s, KMOD_CTRL);
    ST_CHECK(a->prompt == P_NONE && !a->modified);

    /* trying to save over the original photo asks; Esc keeps it */
    snprintf(a->out_path, sizeof a->out_path, "%s", a->doc.base_path);
    a->out_confirmed = false;
    st_key(a, SDLK_s, KMOD_CTRL);
    ST_CHECK(a->prompt == P_CONFIRM);
    st_key(a, SDLK_ESCAPE, 0);
    ST_CHECK(a->prompt == P_NONE);
    /* optional screenshot of the final frame, for eyeballing the UI */
    const char *shot = getenv("PMARK_SELFTEST_SHOT");
    if (shot) {
        int ww, wh;
        SDL_GetRendererOutputSize(a->ren, &ww, &wh);
        pm_image im;
        if (pm_image_create(&im, ww, wh, (pm_color){0, 0, 0, 255}) == PM_OK) {
            if (SDL_RenderReadPixels(a->ren, NULL, SDL_PIXELFORMAT_RGBA32, im.px, ww * 4) == 0)
                pm_image_save(&im, shot, 0);
            pm_image_free(&im);
        }
    }
    fprintf(stderr, "selftest ok\n");
    return 0;
}

/* -------------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
"usage: pmark-gui INPUT [OUTPUT] [--quality N] [--no-session]\n"
"  INPUT is a photo or a .pmark session. OUTPUT defaults to INPUT-marked.png\n"
"  (for a session, the session's name with .png). Ctrl+S saves OUTPUT and,\n"
"  unless --no-session, OUTPUT.pmark next to it so the edit can be reopened.\n");
}

int main(int argc, char **argv)
{
    static app_t app; /* zero-initialised */
    app_t *a = &app;
    const char *pos[2] = {NULL, NULL}, *selftest_out = NULL;
    int npos = 0;
    a->sessions = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftest_out = argv[++i];
        else if (!strcmp(argv[i], "--no-session")) a->sessions = false;
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) a->quality = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) { usage(); return 2; }
        else if (npos < 2) pos[npos++] = argv[i];
    }
    if (npos == 0) { usage(); return 2; }

    snprintf(a->in_path, sizeof a->in_path, "%s", pos[0]);
    bool is_session = pm_is_session_file(a->in_path);
    if (pos[1]) snprintf(a->out_path, sizeof a->out_path, "%s", pos[1]);
    else if (is_session) with_ext(a->out_path, sizeof a->out_path, a->in_path, ".png");
    else with_ext(a->out_path, sizeof a->out_path, a->in_path, "-marked.png");

    pm_status st;
    if (is_session) {
        st = pm_session_load(&a->doc, a->in_path, 0);
        if (st == PM_ERR_STALE || st == PM_ERR_FONT) {
            pm_status st2 = pm_session_load(&a->doc, a->in_path, PM_SESSION_LENIENT);
            if (st2 == PM_OK) set_status(a, 8000, "Warning: %s; opened anyway", pm_strerror(st));
            st = st2;
        }
    } else {
        st = pm_doc_open(&a->doc, a->in_path);
    }
    if (st != PM_OK) {
        fprintf(stderr, "pmark-gui: %s: %s\n", a->in_path, pm_strerror(st));
        return 1;
    }
    if (pm_format_from_path(a->out_path) == PM_FMT_UNKNOWN) {
        fprintf(stderr, "pmark-gui: %s: output must be .png, .jpg or .jpeg\n", a->out_path);
        pm_doc_close(&a->doc);
        return 2;
    }
    pm_font *f;
    if (pm_font_load_default(&f) == PM_OK && pm_doc_own_font(&a->doc, f) == PM_OK) a->font = f;
    else fprintf(stderr, "pmark-gui: warning: %s; text tool disabled\n", pm_strerror(PM_ERR_FONT));

    /* scale default sizes to the photo so they are usable on 12 MP shots */
    float ref = (float)(a->doc.canvas.w > a->doc.canvas.h ? a->doc.canvas.w : a->doc.canvas.h);
    a->line_w = fmaxf(3.0f, roundf(ref / 160.0f));
    a->ts = (pm_text_style){fmaxf(16.0f, roundf(ref / 28.0f)), PALETTE[0], 0, PALETTE[6]};

    int rc = 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "pmark-gui: SDL_Init: %s\n", SDL_GetError());
        goto done;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    a->win = SDL_CreateWindow("pmark", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 820,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (a->win) a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!a->win || !a->ren) {
        fprintf(stderr, "pmark-gui: window: %s\n", SDL_GetError());
        goto done;
    }
    SDL_StopTextInput(); /* SDL starts with it on; we only want it while typing */
    update_dpi(a);
    if (!tex_sync(a)) goto done;
    fit_view(a);
    a->ui_dirty = true;
    a->running = true;

    if (selftest_out) {
        render(a);
        rc = selftest(a, selftest_out);
        goto done;
    }
    update_title(a);
    render(a);
    while (a->running) {
        SDL_Event e;
        /* wake up at least twice a second so status messages can expire */
        if (SDL_WaitEventTimeout(&e, 500)) {
            do handle_event(a, &e); while (SDL_PollEvent(&e)); /* drain, then draw once */
        }
        update_title(a);
        render(a);
    }
    rc = 0;

done:
    marker_finish(a, false);
    if (a->tex) SDL_DestroyTexture(a->tex);
    if (a->ui_tex) SDL_DestroyTexture(a->ui_tex);
    if (a->bar_tex) SDL_DestroyTexture(a->bar_tex);
    if (a->ren) SDL_DestroyRenderer(a->ren);
    if (a->win) SDL_DestroyWindow(a->win);
    SDL_Quit();
    free(a->pts);
    pm_doc_close(&a->doc);
    return rc;
}
