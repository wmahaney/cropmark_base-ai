/*
 * Picture picker (see picker.h).
 *
 *   arrows / PgUp / PgDn / Home / End   move the selection
 *   Enter or double-click               open a picture or folder
 *   Backspace or Alt+Up                 parent folder
 *   typing                              filter by name; a typed path that
 *                                       contains '/' or starts with '~'
 *                                       opens on Enter
 *   Esc                                 clear the filter, then cancel
 */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _XOPEN_SOURCE 700 /* realpath, strdup */
#endif
#include "picker.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CM_PATH 4096
#define MAX_TEXTURES 240 /* thumbnails kept on the GPU; far-away ones are dropped */

typedef enum { E_UP, E_DIR, E_IMAGE, E_SESSION } entry_kind;
typedef enum { TH_NONE, TH_WANT, TH_LOADING, TH_READY, TH_DONE, TH_FAILED } thumb_state;

typedef struct {
    char        *name;
    entry_kind   kind;
    thumb_state  th;    /* guarded by picker.mx                          */
    pm_image     img;   /* TH_READY: decoded thumbnail awaiting upload    */
    SDL_Texture *tex;   /* TH_DONE; main thread only                      */
    bool         failed;/* copy of th == TH_FAILED for drawing; main only  */
    SDL_Texture *label; /* name, rendered for label_dpi                   */
    int          label_w, label_h;
} entry;

struct picker {
    SDL_Renderer  *ren;
    const pm_font *font;
    char   dir[CM_PATH];
    entry *e;
    size_t n;
    size_t *view, nview; /* indices into e that pass the filter */
    size_t sel;          /* index into view                     */
    char   filter[256];
    size_t filter_len;
    char   choice[CM_PATH];

    /* layout, renderer pixels, from the most recent render */
    float dpi, scroll;
    int   win_w, win_h, head_h, bar_h, gap, tile_w, tile_h, pad, cols, grid_x, grid_h;
    pm_rect btn_up, btn_home;
    SDL_Texture *head_tex, *bar_tex, *empty_tex;
    int   empty_w, empty_h;
    bool  ui_dirty, status_shown, reveal; /* reveal: scroll to the selection */
    char  status[512];
    Uint32 status_until;
    size_t n_tex, n_done;

    /* thumbnail worker */
    SDL_Thread *thread;
    SDL_mutex  *mx;
    SDL_cond   *cv;
    bool        quit;
    unsigned    gen;      /* bumped when the folder changes */
    int         thumb_px; /* longest side of a thumbnail    */
};

static Uint32 wake_event = (Uint32)-1;

static const pm_color C_BG = {0x26, 0x26, 0x2a, 0xff}, C_BAR = {0x24, 0x24, 0x28, 0xff},
                      C_TILE = {0x32, 0x32, 0x38, 0xff}, C_SEL = {0x4a, 0x6f, 0xa5, 0xff},
                      C_HOLE = {0x2a, 0x2a, 0x2f, 0xff}, C_TXT = {0xee, 0xee, 0xee, 0xff},
                      C_DIM = {0x9a, 0x9a, 0xa2, 0xff}, C_BTN = {0x38, 0x38, 0x3e, 0xff},
                      C_FOLDER = {0xd9, 0xa4, 0x41, 0xff}, C_WARN = {0xff, 0xd5, 0x4f, 0xff};

/* ------------------------------------------------------------- utilities */

static bool has_ext_ci(const char *name, const char *ext)
{
    size_t n = strlen(name), k = strlen(ext);
    return n > k && SDL_strcasecmp(name + n - k, ext) == 0;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t k = strlen(needle);
    for (; *hay; hay++)
        if (SDL_strncasecmp(hay, needle, k) == 0) return true;
    return k == 0;
}

/* dir/name; false if it does not fit. */
static bool join(char *out, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(out, cap, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
    return n >= 0 && (size_t)n < cap;
}

void picker_set_status(picker *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->status, sizeof p->status, fmt, ap);
    va_end(ap);
    p->status_until = SDL_GetTicks() + 6000;
    p->ui_dirty = true;
}

static void set_color(SDL_Renderer *r, pm_color c) { SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a); }

static void fill(SDL_Renderer *r, pm_color c, int x, int y, int w, int h)
{
    set_color(r, c);
    SDL_RenderFillRect(r, &(SDL_Rect){x, y, w, h});
}

/* Text on a transparent texture; NULL without a font. */
static SDL_Texture *text_tex(picker *p, const char *s, float px, pm_color c, int *w, int *h)
{
    *w = *h = 0;
    if (!p->font || !*s) return NULL;
    float tw, th;
    pm_text_measure(p->font, s, px, &tw, &th);
    pm_image im;
    if (pm_image_create(&im, (int)ceilf(tw) + 2, (int)ceilf(th) + 2, (pm_color){0, 0, 0, 0}) != PM_OK)
        return NULL;
    pm_text_style st = {px, c, 0, c};
    pm_text_draw(&im, p->font, s, 1, 1, &st);
    SDL_Texture *t = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im.w, im.h);
    if (t) {
        SDL_UpdateTexture(t, NULL, im.px, im.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        *w = im.w;
        *h = im.h;
    }
    pm_image_free(&im);
    return t;
}

/* Longest prefix of s (whole UTF-8 characters) that fits in max_w, plus "...". */
static void fit_text(const pm_font *font, const char *s, float px, float max_w, char *out, size_t cap)
{
    float w, h;
    snprintf(out, cap, "%s", s);
    pm_text_measure(font, out, px, &w, &h);
    if (w <= max_w) return;
    size_t n = strlen(out);
    while (n > 0) {
        do n--; while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80);
        snprintf(out + n, cap - n, "...");
        pm_text_measure(font, out, px, &w, &h);
        if (w <= max_w) return;
    }
}

/* ------------------------------------------------------------ thumbnails */

/* Area average; colour weighted by alpha so transparent pixels add no fringe. */
static pm_status downscale(const pm_image *s, int max, pm_image *out)
{
    int big = s->w > s->h ? s->w : s->h;
    if (big <= max) return pm_image_copy(out, s);
    int w = (int)((long long)s->w * max / big), h = (int)((long long)s->h * max / big);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    pm_status st = pm_image_create(out, w, h, (pm_color){0, 0, 0, 0});
    if (st != PM_OK) return st;
    for (int y = 0; y < h; y++) {
        int y0 = (int)((long long)y * s->h / h), y1 = (int)((long long)(y + 1) * s->h / h);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = (int)((long long)x * s->w / w), x1 = (int)((long long)(x + 1) * s->w / w);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *q = s->px + ((size_t)yy * s->w + x0) * 4;
                for (int xx = x0; xx < x1; xx++, q += 4) {
                    r += (uint64_t)q[0] * q[3];
                    g += (uint64_t)q[1] * q[3];
                    b += (uint64_t)q[2] * q[3];
                    a += q[3];
                    cnt++;
                }
            }
            uint8_t *d = out->px + ((size_t)y * w + x) * 4;
            if (a) { d[0] = (uint8_t)(r / a); d[1] = (uint8_t)(g / a); d[2] = (uint8_t)(b / a); }
            d[3] = (uint8_t)(a / cnt);
        }
    }
    return PM_OK;
}

static pm_status make_thumb(const char *path, bool session, int max, pm_image *out)
{
    pm_image full = {0};
    pm_status st;
    if (session) { /* show the edited picture, not the bare base photo */
        pm_doc d;
        st = pm_session_load(&d, path, PM_SESSION_LENIENT);
        if (st == PM_OK) {
            full = d.canvas;
            d.canvas = (pm_image){0};
            pm_doc_close(&d);
        }
    } else {
        st = pm_image_load(&full, path);
    }
    if (st != PM_OK) return st;
    st = downscale(&full, max, out);
    pm_image_free(&full);
    return st;
}

static int worker(void *ud)
{
    picker *p = ud;
    SDL_LockMutex(p->mx);
    while (!p->quit) {
        size_t i = 0;
        while (i < p->n && p->e[i].th != TH_WANT) i++; /* top of the folder first */
        if (i == p->n) { SDL_CondWait(p->cv, p->mx); continue; }
        char path[CM_PATH];
        if (!join(path, sizeof path, p->dir, p->e[i].name)) { p->e[i].th = TH_FAILED; continue; }
        bool session = p->e[i].kind == E_SESSION;
        unsigned gen = p->gen;
        int max = p->thumb_px;
        p->e[i].th = TH_LOADING;
        SDL_UnlockMutex(p->mx);

        pm_image t = {0};
        pm_status st = make_thumb(path, session, max, &t);

        SDL_LockMutex(p->mx);
        if (gen == p->gen && i < p->n) { /* the folder may have changed meanwhile */
            p->e[i].img = t;
            p->e[i].th = st == PM_OK ? TH_READY : TH_FAILED;
        } else {
            pm_image_free(&t);
        }
        SDL_UnlockMutex(p->mx);
        if (wake_event != (Uint32)-1) { /* let the main loop redraw */
            SDL_Event ev;
            memset(&ev, 0, sizeof ev);
            ev.type = wake_event;
            SDL_PushEvent(&ev);
        }
        SDL_LockMutex(p->mx);
    }
    SDL_UnlockMutex(p->mx);
    return 0;
}

/* --------------------------------------------------------------- folders */

static void entry_free(entry *e)
{
    free(e->name);
    pm_image_free(&e->img);
    if (e->tex) SDL_DestroyTexture(e->tex);
    if (e->label) SDL_DestroyTexture(e->label);
}

static int entry_cmp(const void *pa, const void *pb)
{
    const entry *a = pa, *b = pb;
    bool da = a->kind == E_DIR, db = b->kind == E_DIR;
    if (da != db) return da ? -1 : 1; /* folders first */
    int c = SDL_strcasecmp(a->name, b->name);
    return c ? c : strcmp(a->name, b->name);
}

static void rebuild_view(picker *p)
{
    p->nview = 0;
    for (size_t i = 0; i < p->n; i++) {
        bool keep = p->e[i].kind == E_UP ? p->filter_len == 0 : contains_ci(p->e[i].name, p->filter);
        if (keep) p->view[p->nview++] = i;
    }
    if (p->sel >= p->nview) p->sel = p->nview ? p->nview - 1 : 0;
    p->ui_dirty = true;
}

static void ensure_visible(picker *p);

/* Lists dir (made absolute) and selects select_name if given, else the
 * first picture. On failure the current folder stays and a status says why. */
static bool load_dir(picker *p, const char *dir, const char *select_name)
{
    char real[CM_PATH];
    if (!realpath(dir, real)) {
        picker_set_status(p, "%s: %s", dir, strerror(errno));
        return false;
    }
    DIR *d = opendir(real);
    if (!d) {
        picker_set_status(p, "Cannot open %s: %s", real, strerror(errno));
        return false;
    }
    entry *list = NULL;
    size_t n = 0, cap = 0;
    bool ok = true, root = !strcmp(real, "/");
    for (struct dirent *de; ok && (de = readdir(d));) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue; /* hidden files, "." and ".." */
        char full[CM_PATH];
        if (!join(full, sizeof full, real, nm)) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue; /* follows symlinks; skips broken ones */
        entry_kind k;
        if (S_ISDIR(st.st_mode)) k = E_DIR;
        else if (!S_ISREG(st.st_mode)) continue;
        else if (has_ext_ci(nm, ".png") || has_ext_ci(nm, ".jpg") || has_ext_ci(nm, ".jpeg")) k = E_IMAGE;
        else if (has_ext_ci(nm, ".pmark")) k = E_SESSION;
        else continue;
        if (n + 1 >= cap) { /* + 1 keeps room for the ".." entry */
            size_t nc = cap ? cap * 2 : 64;
            entry *nl = realloc(list, nc * sizeof *nl);
            if (!nl) { ok = false; break; }
            list = nl;
            cap = nc;
        }
        memset(&list[n], 0, sizeof list[n]);
        list[n].kind = k;
        if (!(list[n].name = strdup(nm))) { ok = false; break; }
        n++;
    }
    closedir(d);
    size_t *view = ok ? malloc((n + 1) * sizeof *view) : NULL;
    if (!ok || !view || (!list && !(list = malloc(sizeof *list)))) {
        for (size_t i = 0; i < n; i++) entry_free(&list[i]);
        free(list);
        free(view);
        picker_set_status(p, "Out of memory listing %s", real);
        return false;
    }
    qsort(list, n, sizeof *list, entry_cmp);
    if (!root) { /* ".." goes first */
        memmove(list + 1, list, n * sizeof *list);
        memset(&list[0], 0, sizeof list[0]);
        list[0].kind = E_UP;
        list[0].name = strdup("..");
        if (!list[0].name) { memmove(list, list + 1, n * sizeof *list); root = true; }
        else n++;
    }

    SDL_LockMutex(p->mx); /* the worker reads e, n and dir */
    for (size_t i = 0; i < p->n; i++) entry_free(&p->e[i]);
    free(p->e);
    free(p->view);
    p->e = list;
    p->n = n;
    p->view = view;
    p->gen++;
    snprintf(p->dir, sizeof p->dir, "%s", real);
    SDL_UnlockMutex(p->mx);

    p->n_tex = p->n_done = 0;
    p->filter_len = 0;
    p->filter[0] = 0;
    p->scroll = 0;
    p->sel = 0;
    rebuild_view(p);
    for (size_t v = 0; v < p->nview; v++) {
        const entry *e = &p->e[p->view[v]];
        if (select_name ? !strcmp(e->name, select_name) : (e->kind == E_IMAGE || e->kind == E_SESSION)) {
            p->sel = v;
            break;
        }
    }
    ensure_visible(p);
    p->reveal = true; /* again once laid out, for the first render */
    return true;
}

static void go_up(picker *p)
{
    if (!strcmp(p->dir, "/")) return;
    char parent[CM_PATH], here[CM_PATH];
    snprintf(parent, sizeof parent, "%s", p->dir);
    char *slash = strrchr(parent, '/');
    snprintf(here, sizeof here, "%s", slash + 1);
    if (slash == parent) slash[1] = 0; /* "/name" -> "/" */
    else *slash = 0;
    load_dir(p, parent, here); /* and select the folder we came from */
}

static void go_home(picker *p)
{
    const char *home = getenv("HOME");
    if (home && *home) load_dir(p, home, NULL);
}

/* ---------------------------------------------------------------- layout */

static void layout(picker *p)
{
    float d = p->dpi;
    SDL_GetRendererOutputSize(p->ren, &p->win_w, &p->win_h);
    p->head_h = (int)lroundf(48 * d);
    p->bar_h = (int)lroundf(28 * d);
    p->gap = (int)lroundf(14 * d);
    p->pad = (int)lroundf(8 * d);
    p->tile_w = (int)lroundf(168 * d);
    p->tile_h = p->tile_w + (int)lroundf(24 * d);
    p->cols = (p->win_w - p->gap) / (p->tile_w + p->gap);
    if (p->cols < 1) p->cols = 1;
    int used = p->cols * (p->tile_w + p->gap) - p->gap;
    p->grid_x = (p->win_w - used) / 2;
    p->grid_h = p->win_h - p->head_h - p->bar_h;
}

static pm_rect tile_rect(const picker *p, size_t k)
{
    int col = (int)(k % (size_t)p->cols), row = (int)(k / (size_t)p->cols);
    return (pm_rect){p->grid_x + col * (p->tile_w + p->gap),
                     p->head_h + p->gap + row * (p->tile_h + p->gap) - (int)p->scroll,
                     p->tile_w, p->tile_h};
}

static float max_scroll(const picker *p)
{
    size_t rows = (p->nview + (size_t)p->cols - 1) / (size_t)p->cols;
    float content = (float)rows * (float)(p->tile_h + p->gap) + (float)p->gap;
    return fmaxf(0.0f, content - (float)p->grid_h);
}

static void clamp_scroll(picker *p) { p->scroll = fminf(fmaxf(p->scroll, 0.0f), max_scroll(p)); }

static void ensure_visible(picker *p)
{
    if (!p->tile_w || !p->nview) return; /* before the first layout */
    pm_rect r = tile_rect(p, p->sel);
    int top = p->head_h, bottom = p->head_h + p->grid_h;
    if (r.y < top + p->gap) p->scroll -= (float)(top + p->gap - r.y);
    else if (r.y + r.h > bottom - p->gap) p->scroll += (float)(r.y + r.h - (bottom - p->gap));
    clamp_scroll(p);
}

static bool hit(pm_rect r, float x, float y)
{
    return x >= (float)r.x && x < (float)(r.x + r.w) && y >= (float)r.y && y < (float)(r.y + r.h);
}

/* ---------------------------------------------------------------- events */

static pick_result activate(picker *p, size_t k)
{
    if (k >= p->nview) return PICK_NONE;
    const entry *e = &p->e[p->view[k]];
    char path[CM_PATH];
    switch (e->kind) {
    case E_UP:  go_up(p); return PICK_NONE;
    case E_DIR:
        if (join(path, sizeof path, p->dir, e->name)) load_dir(p, path, NULL);
        else picker_set_status(p, "That path is too long");
        return PICK_NONE;
    default:
        if (join(p->choice, sizeof p->choice, p->dir, e->name)) return PICK_OPEN;
        picker_set_status(p, "That path is too long");
        return PICK_NONE;
    }
}

/* Enter on a filter that looks like a path: open that folder or picture. */
static pick_result open_typed_path(picker *p)
{
    char path[CM_PATH];
    const char *home = getenv("HOME");
    if (p->filter[0] == '~' && home) snprintf(path, sizeof path, "%s%s", home, p->filter + 1);
    else if (p->filter[0] == '/') snprintf(path, sizeof path, "%s", p->filter);
    else if (!join(path, sizeof path, p->dir, p->filter)) {
        picker_set_status(p, "That path is too long");
        return PICK_NONE;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        picker_set_status(p, "%s: %s", p->filter, strerror(errno));
        return PICK_NONE;
    }
    if (S_ISDIR(st.st_mode)) { load_dir(p, path, NULL); return PICK_NONE; }
    if (!has_ext_ci(path, ".pmark") && !has_ext_ci(path, ".jpg") && !has_ext_ci(path, ".jpeg") &&
        !has_ext_ci(path, ".png")) {
        picker_set_status(p, "%s is not a .jpg, .png or .pmark file", p->filter);
        return PICK_NONE;
    }
    snprintf(p->choice, sizeof p->choice, "%s", path);
    return PICK_OPEN;
}

static void move_sel(picker *p, long delta)
{
    if (!p->nview) return;
    long s = (long)p->sel + delta;
    if (s < 0) s = 0;
    if (s >= (long)p->nview) s = (long)p->nview - 1;
    p->sel = (size_t)s;
    ensure_visible(p);
    p->ui_dirty = true;
}

static void filter_changed(picker *p)
{
    p->sel = 0;
    rebuild_view(p);
    p->scroll = 0;
    ensure_visible(p);
}

static pick_result on_key(picker *p, const SDL_KeyboardEvent *k)
{
    SDL_Keycode sym = k->keysym.sym;
    bool alt = (k->keysym.mod & KMOD_ALT) != 0;
    int page = p->tile_h + p->gap > 0 ? p->grid_h / (p->tile_h + p->gap) : 1;
    if (page < 1) page = 1;
    switch (sym) {
    case SDLK_LEFT:     move_sel(p, -1); break;
    case SDLK_RIGHT:    move_sel(p, 1); break;
    case SDLK_UP:       if (alt) go_up(p); else move_sel(p, -p->cols); break;
    case SDLK_DOWN:     move_sel(p, p->cols); break;
    case SDLK_PAGEUP:   move_sel(p, -(long)page * p->cols); break;
    case SDLK_PAGEDOWN: move_sel(p, (long)page * p->cols); break;
    case SDLK_HOME:     move_sel(p, -(long)p->nview); break;
    case SDLK_END:      move_sel(p, (long)p->nview); break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (p->filter_len && (strchr(p->filter, '/') || p->filter[0] == '~')) return open_typed_path(p);
        return activate(p, p->sel);
    case SDLK_BACKSPACE:
        if (p->filter_len) {
            do p->filter_len--;
            while (p->filter_len > 0 && ((unsigned char)p->filter[p->filter_len] & 0xC0) == 0x80);
            p->filter[p->filter_len] = 0;
            filter_changed(p);
        } else {
            go_up(p);
        }
        break;
    case SDLK_ESCAPE:
        if (!p->filter_len) return PICK_CANCEL;
        p->filter_len = 0;
        p->filter[0] = 0;
        filter_changed(p);
        break;
    default: break;
    }
    return PICK_NONE;
}

pick_result picker_event(picker *p, const SDL_Event *e)
{
    layout(p);
    switch (e->type) {
    case SDL_KEYDOWN:
        return on_key(p, &e->key);
    case SDL_TEXTINPUT: {
        size_t n = strlen(e->text.text);
        if (p->filter_len + n < sizeof p->filter) {
            memcpy(p->filter + p->filter_len, e->text.text, n + 1);
            p->filter_len += n;
            filter_changed(p);
        }
        return PICK_NONE;
    }
    case SDL_MOUSEBUTTONDOWN: {
        if (e->button.button != SDL_BUTTON_LEFT) return PICK_NONE;
        float x = (float)e->button.x * p->dpi, y = (float)e->button.y * p->dpi;
        if (y < (float)p->head_h) {
            if (hit(p->btn_up, x, y)) go_up(p);
            else if (hit(p->btn_home, x, y)) go_home(p);
            return PICK_NONE;
        }
        if (y >= (float)(p->head_h + p->grid_h)) return PICK_NONE;
        for (size_t k = 0; k < p->nview; k++) {
            if (!hit(tile_rect(p, k), x, y)) continue;
            p->sel = k;
            p->ui_dirty = true;
            return e->button.clicks >= 2 ? activate(p, k) : PICK_NONE;
        }
        return PICK_NONE;
    }
    case SDL_MOUSEWHEEL:
        p->scroll -= (float)e->wheel.y * (float)(p->tile_h + p->gap) * 0.5f;
        clamp_scroll(p);
        return PICK_NONE;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { p->ui_dirty = true; clamp_scroll(p); }
        return PICK_NONE;
    default:
        return PICK_NONE; /* includes the worker's wake-up event: render() uploads */
    }
}

/* ---------------------------------------------------------------- render */

/* Marks on-screen thumbnails wanted (and off-screen ones not), uploads
 * finished ones and drops textures far from view. Main thread only. */
static void update_thumbs(picker *p, size_t first, size_t last)
{
    bool wake = false;
    SDL_LockMutex(p->mx);
    if (!p->thumb_px) p->thumb_px = p->tile_w - 2 * p->pad;
    size_t margin = (size_t)p->cols; /* one row of read-ahead each way */
    size_t lo = first > margin ? first - margin : 0, hi = last + margin;
    for (size_t k = 0; k < p->nview; k++) {
        entry *e = &p->e[p->view[k]];
        if (e->kind != E_IMAGE && e->kind != E_SESSION) continue;
        bool on_screen = k >= lo && k <= hi;
        if (on_screen && e->th == TH_NONE) { e->th = TH_WANT; wake = true; }
        else if (!on_screen && e->th == TH_WANT) e->th = TH_NONE;
        if (e->th == TH_READY) {
            e->tex = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                       e->img.w, e->img.h);
            if (e->tex) {
                SDL_UpdateTexture(e->tex, NULL, e->img.px, e->img.w * 4);
                SDL_SetTextureBlendMode(e->tex, SDL_BLENDMODE_BLEND);
                p->n_tex++;
            }
            pm_image_free(&e->img);
            e->th = e->tex ? TH_DONE : TH_FAILED;
            p->n_done++;
        }
        e->failed = e->th == TH_FAILED;
    }
    if (p->n_tex > MAX_TEXTURES) /* drop off-screen thumbnails; they reload if needed */
        for (size_t k = 0; k < p->nview && p->n_tex > MAX_TEXTURES / 2; k++) {
            entry *e = &p->e[p->view[k]];
            if ((k >= lo && k <= hi) || e->th != TH_DONE) continue;
            SDL_DestroyTexture(e->tex);
            e->tex = NULL;
            e->th = TH_NONE;
            p->n_tex--;
        }
    if (wake) SDL_CondSignal(p->cv);
    SDL_UnlockMutex(p->mx);
}

static void draw_folder(picker *p, int x, int y, int s, bool up)
{
    int w = s * 3 / 5, h = w * 3 / 4, fx = x + (s - w) / 2, fy = y + (s - h) / 2;
    fill(p->ren, C_FOLDER, fx, fy - h / 8, w * 2 / 5, h / 6);           /* tab  */
    fill(p->ren, C_FOLDER, fx, fy, w, h);                               /* body */
    if (up) { /* an upward arrow on the folder */
        pm_color c = C_HOLE;
        int ax = fx + w / 2, t = w / 10 > 1 ? w / 10 : 2;
        fill(p->ren, c, ax - t / 2, fy + h / 4, t, h / 2);
        for (int i = 0; i < h / 4; i++) fill(p->ren, c, ax - i, fy + h / 4 + i, 2 * i + 1, 1);
    }
}

static void draw_tile(picker *p, size_t k)
{
    entry *e = &p->e[p->view[k]];
    pm_rect r = tile_rect(p, k);
    fill(p->ren, k == p->sel ? C_SEL : C_TILE, r.x, r.y, r.w, r.h);
    int s = r.w - 2 * p->pad, bx = r.x + p->pad, by = r.y + p->pad;
    if (e->kind == E_UP || e->kind == E_DIR) {
        draw_folder(p, bx, by, s, e->kind == E_UP);
    } else if (e->tex) { /* th is the worker's: drawing reads only main-thread fields */
        int tw, th;
        SDL_QueryTexture(e->tex, NULL, NULL, &tw, &th);
        float z = fminf((float)s / (float)tw, (float)s / (float)th);
        SDL_FRect d = {(float)bx + ((float)s - (float)tw * z) * 0.5f,
                       (float)by + ((float)s - (float)th * z) * 0.5f, (float)tw * z, (float)th * z};
        SDL_RenderCopyF(p->ren, e->tex, NULL, &d);
    } else {
        fill(p->ren, C_HOLE, bx, by, s, s); /* loading, or unreadable */
        if (e->failed) {
            int cx = bx + s / 2, cy = by + s / 2, a = s / 6;
            set_color(p->ren, C_DIM);
            SDL_RenderDrawLine(p->ren, cx - a, cy - a, cx + a, cy + a);
            SDL_RenderDrawLine(p->ren, cx - a, cy + a, cx + a, cy - a);
        }
    }
    if (!e->label && p->font) {
        char shown[512];
        const char *nm = e->kind == E_UP ? "..  (up)" : e->name;
        float px = 13.0f * p->dpi;
        fit_text(p->font, nm, px, (float)(r.w - 2 * p->pad), shown, sizeof shown);
        if (e->kind == E_DIR && strlen(shown) + 1 < sizeof shown) strcat(shown, "/");
        e->label = text_tex(p, shown, px, e->kind == E_SESSION ? C_WARN : C_TXT, &e->label_w, &e->label_h);
    }
    if (e->label) {
        SDL_Rect d = {r.x + (r.w - e->label_w) / 2, r.y + r.w + (r.h - r.w - e->label_h) / 2,
                      e->label_w, e->label_h};
        SDL_RenderCopy(p->ren, e->label, NULL, &d);
    }
}

static int button(picker *p, pm_image *im, int right, const char *s, pm_rect *out)
{
    float px = 14.0f * p->dpi, w = 30.0f * p->dpi, h;
    if (p->font) pm_text_measure(p->font, s, px, &w, &h);
    int pad = (int)lroundf(10 * p->dpi), m = (int)lroundf(9 * p->dpi);
    pm_rect r = {right - (int)ceilf(w) - 2 * pad, m, (int)ceilf(w) + 2 * pad, p->head_h - 2 * m};
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x < 0 ? 0 : r.x; x < r.x + r.w && x < im->w; x++)
            memcpy(im->px + ((size_t)y * im->w + x) * 4, &C_BTN, 4);
    if (p->font) {
        pm_text_style st = {px, C_TXT, 0, C_TXT};
        float tw, th;
        pm_text_measure(p->font, s, px, &tw, &th);
        pm_text_draw(im, p->font, s, (float)(r.x + pad), (float)p->head_h * 0.5f - th * 0.5f, &st);
    }
    *out = r;
    return r.x - (int)lroundf(6 * p->dpi);
}

static SDL_Texture *upload(picker *p, SDL_Texture *old, pm_image *im)
{
    if (old) SDL_DestroyTexture(old);
    SDL_Texture *t = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im->w, im->h);
    if (t) SDL_UpdateTexture(t, NULL, im->px, im->w * 4);
    pm_image_free(im);
    return t;
}

static void build_ui(picker *p)
{
    pm_image im;
    /* header: title, folder, Up and Home buttons */
    if (pm_image_create(&im, p->win_w, p->head_h, C_BAR) == PM_OK) {
        int right = p->win_w - (int)lroundf(10 * p->dpi);
        right = button(p, &im, right, "Home", &p->btn_home);
        right = button(p, &im, right, "Up", &p->btn_up);
        if (p->font) {
            float px = 15.0f * p->dpi, tw, th, x = 12.0f * p->dpi;
            pm_text_style st = {px, C_TXT, 0, C_TXT};
            pm_text_measure(p->font, "Open a picture", px, &tw, &th);
            float y = (float)p->head_h * 0.5f - th * 0.5f;
            pm_text_draw(&im, p->font, "Open a picture", x, y, &st);
            x += tw + 16.0f * p->dpi;
            char shown[CM_PATH];
            fit_text(p->font, p->dir, px, (float)right - x - 8.0f * p->dpi, shown, sizeof shown);
            st.color = C_DIM;
            pm_text_draw(&im, p->font, shown, x, y, &st);
        }
        p->head_tex = upload(p, p->head_tex, &im);
    }

    /* status bar */
    size_t n_pics = 0, n_dirs = 0;
    for (size_t v = 0; v < p->nview; v++) {
        entry_kind k = p->e[p->view[v]].kind;
        n_pics += k == E_IMAGE || k == E_SESSION;
        n_dirs += k == E_DIR;
    }
    char line[1024];
    bool live = p->status[0] && SDL_GetTicks() < p->status_until;
    if (live)
        snprintf(line, sizeof line, "%s", p->status);
    else if (p->filter_len)
        snprintf(line, sizeof line, "Filter: %s_   (%zu match%s; Enter opens, Esc clears)", p->filter,
                 n_pics + n_dirs, n_pics + n_dirs == 1 ? "" : "es");
    else
        snprintf(line, sizeof line, "%zu picture%s, %zu folder%s   Enter or double-click opens   "
                 "Backspace: parent folder   type to filter, or a path", n_pics, n_pics == 1 ? "" : "s",
                 n_dirs, n_dirs == 1 ? "" : "s");
    if (pm_image_create(&im, p->win_w, p->bar_h, C_BAR) == PM_OK) {
        if (p->font) {
            float px = 14.0f * p->dpi, tw, th;
            pm_text_measure(p->font, line, px, &tw, &th);
            pm_text_style st = {px, live || p->filter_len ? C_WARN : C_TXT, 0, C_TXT};
            pm_text_draw(&im, p->font, line, 10.0f * p->dpi, (float)p->bar_h * 0.5f - th * 0.5f, &st);
        }
        p->bar_tex = upload(p, p->bar_tex, &im);
    }

    /* message for a folder with nothing to show */
    if (p->empty_tex) SDL_DestroyTexture(p->empty_tex);
    p->empty_tex = NULL;
    if (n_pics == 0)
        p->empty_tex = text_tex(p, p->filter_len ? "Nothing matches the filter"
                                                 : "No pictures here (.jpg, .png, .pmark)",
                                16.0f * p->dpi, C_DIM, &p->empty_w, &p->empty_h);
    p->ui_dirty = false;
}

void picker_render(picker *p, float dpi)
{
    if (dpi != p->dpi) { /* labels are rasterised for one scale */
        for (size_t i = 0; i < p->n; i++)
            if (p->e[i].label) { SDL_DestroyTexture(p->e[i].label); p->e[i].label = NULL; }
        p->dpi = dpi;
        p->ui_dirty = true;
    }
    int ow = p->win_w, oh = p->win_h;
    layout(p);
    if (ow != p->win_w || oh != p->win_h) p->ui_dirty = true;
    clamp_scroll(p);
    if (p->reveal) { ensure_visible(p); p->reveal = false; }
    bool live = p->status[0] && SDL_GetTicks() < p->status_until;
    if (live != p->status_shown) p->ui_dirty = true; /* let messages expire */
    p->status_shown = live;
    if (p->ui_dirty) build_ui(p);

    SDL_SetRenderDrawBlendMode(p->ren, SDL_BLENDMODE_BLEND);
    set_color(p->ren, C_BG);
    SDL_RenderClear(p->ren);

    size_t row_h = (size_t)(p->tile_h + p->gap);
    size_t first_row = (size_t)(p->scroll / (float)row_h);
    size_t last_row = (size_t)((p->scroll + (float)p->grid_h) / (float)row_h);
    size_t first = first_row * (size_t)p->cols, last = (last_row + 1) * (size_t)p->cols - 1;
    if (last >= p->nview) last = p->nview ? p->nview - 1 : 0;
    update_thumbs(p, first, last);

    SDL_RenderSetClipRect(p->ren, &(SDL_Rect){0, p->head_h, p->win_w, p->grid_h});
    for (size_t k = first; k <= last && k < p->nview; k++) draw_tile(p, k);
    if (p->empty_tex) {
        int y = p->head_h + p->grid_h / 2;
        if (p->nview) { /* below the folders, if any */
            pm_rect r = tile_rect(p, p->nview - 1);
            y = r.y + r.h + p->gap * 2 + p->empty_h;
        }
        SDL_RenderCopy(p->ren, p->empty_tex, NULL,
                       &(SDL_Rect){(p->win_w - p->empty_w) / 2, y - p->empty_h / 2, p->empty_w, p->empty_h});
    }
    SDL_RenderSetClipRect(p->ren, NULL);

    if (p->head_tex) SDL_RenderCopy(p->ren, p->head_tex, NULL, &(SDL_Rect){0, 0, p->win_w, p->head_h});
    if (p->bar_tex)
        SDL_RenderCopy(p->ren, p->bar_tex, NULL, &(SDL_Rect){0, p->win_h - p->bar_h, p->win_w, p->bar_h});
    SDL_RenderPresent(p->ren);
}

/* ------------------------------------------------------------- lifecycle */

picker *picker_new(SDL_Renderer *ren, const pm_font *font, const char *dir)
{
    picker *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->ren = ren;
    p->font = font;
    p->dpi = 1.0f;
    if (wake_event == (Uint32)-1) wake_event = SDL_RegisterEvents(1);
    p->mx = SDL_CreateMutex();
    p->cv = SDL_CreateCond();
    if (!p->mx || !p->cv || !load_dir(p, dir && *dir ? dir : ".", NULL)) {
        if (p->status[0]) fprintf(stderr, "cropmark: %s\n", p->status);
        picker_free(p);
        return NULL;
    }
    p->thread = SDL_CreateThread(worker, "cropmark-thumbs", p);
    if (!p->thread) fprintf(stderr, "cropmark: no thumbnail thread: %s\n", SDL_GetError());
    p->status[0] = 0;
    p->ui_dirty = true;
    return p;
}

void picker_free(picker *p)
{
    if (!p) return;
    if (p->thread) {
        SDL_LockMutex(p->mx);
        p->quit = true;
        SDL_CondSignal(p->cv);
        SDL_UnlockMutex(p->mx);
        SDL_WaitThread(p->thread, NULL);
    }
    for (size_t i = 0; i < p->n; i++) entry_free(&p->e[i]);
    free(p->e);
    free(p->view);
    if (p->head_tex) SDL_DestroyTexture(p->head_tex);
    if (p->bar_tex) SDL_DestroyTexture(p->bar_tex);
    if (p->empty_tex) SDL_DestroyTexture(p->empty_tex);
    if (p->cv) SDL_DestroyCond(p->cv);
    if (p->mx) SDL_DestroyMutex(p->mx);
    free(p);
}

bool picker_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

const char *picker_choice(const picker *p) { return p->choice; }
const char *picker_dir(const picker *p) { return p->dir; }
size_t picker_shown(const picker *p) { return p->nview; }
size_t picker_thumbs_done(const picker *p) { return p->n_done; }

const char *picker_selected(const picker *p)
{
    return p->sel < p->nview ? p->e[p->view[p->sel]].name : NULL;
}
