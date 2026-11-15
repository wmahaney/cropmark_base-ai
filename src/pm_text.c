#include "pm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

struct pm_font {
    uint8_t       *data; /* the font file; stbtt_fontinfo points into it */
    char          *path; /* canonical path, recorded in session files     */
    stbtt_fontinfo info;
};

pm_status pm_font_load(pm_font **out, const char *path)
{
    if (!out || !path) return PM_ERR_ARG;
    pm_font *f = calloc(1, sizeof *f);
    if (!f) return PM_ERR_NOMEM;
    size_t len;
    pm_status st = pm_read_file(path, &f->data, &len);
    if (st != PM_OK) { free(f); return st; }
    f->path = pm_realpath(path);
    if (!f->path) f->path = pm_strdup(path);
    if (!f->path) { pm_font_free(f); return PM_ERR_NOMEM; }
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    if (off < 0 || !stbtt_InitFont(&f->info, f->data, off)) {
        pm_font_free(f);
        return PM_ERR_FORMAT;
    }
    *out = f;
    return PM_OK;
}

pm_status pm_font_load_default(pm_font **out)
{
    static const char *const candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    const char *env = getenv("PMARK_FONT");
    if (env && *env && pm_font_load(out, env) == PM_OK) return PM_OK;
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++)
        if (pm_font_load(out, candidates[i]) == PM_OK) return PM_OK;
    return PM_ERR_FONT;
}

const char *pm_font_path(const pm_font *f) { return f ? f->path : NULL; }

void pm_font_free(pm_font *f)
{
    if (!f) return;
    free(f->data);
    free(f->path);
    free(f);
}

/* Decodes one UTF-8 scalar and advances *s; malformed input yields U+FFFD
 * and consumes one byte, so we always make progress. */
static int utf8_next(const unsigned char **s)
{
    const unsigned char *p = *s;
    int c = *p, n = 0, cp;
    if (c < 0x80) { *s = p + 1; return c; }
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { *s = p + 1; return 0xFFFD; }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s = p + 1; return 0xFFFD; }
        cp = cp << 6 | (p[i] & 0x3F);
    }
    *s = p + n + 1;
    return cp;
}

typedef struct {
    float scale, ascent, line_h;
} metrics;

static metrics font_metrics(const pm_font *f, float size_px)
{
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    float scale = stbtt_ScaleForPixelHeight(&f->info, size_px);
    return (metrics){scale, asc * scale, (asc - desc + gap) * scale};
}

/* Shared layout walk. With mask == NULL it only measures; otherwise glyph
 * coverage is max-accumulated into mask, which covers mrect. */
static pm_status layout(uint8_t *mask, pm_rect mrect, const pm_font *f,
                        const char *utf8, float x, float y, float size_px,
                        float *out_w, float *out_h)
{
    metrics m = font_metrics(f, size_px);
    const unsigned char *s = (const unsigned char *)utf8;
    uint8_t *buf = NULL;
    size_t buf_cap = 0;
    float pen = x, max_w = 0.0f;
    int lines = 1, prev = 0;
    int baseline = (int)floorf(y + m.ascent + 0.5f);

    while (*s) {
        int cp = utf8_next(&s);
        if (cp == '\n') {
            max_w = fmaxf(max_w, pen - x);
            pen = x;
            prev = 0;
            baseline = (int)floorf(y + m.ascent + (float)lines * m.line_h + 0.5f);
            lines++;
            continue;
        }
        int g = stbtt_FindGlyphIndex(&f->info, cp);
        if (prev) pen += (float)stbtt_GetGlyphKernAdvance(&f->info, prev, g) * m.scale;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);

        if (mask) {
            float fl = floorf(pen), shift = pen - fl;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, m.scale, m.scale,
                                            shift, 0.0f, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            pm_rect gr = {(int)fl + x0, baseline + y0, gw, gh};
            pm_rect dst = pm_rect_intersect(gr, mrect);
            if (!pm_rect_empty(dst)) {
                size_t need = (size_t)gw * (size_t)gh;
                if (need > buf_cap) {
                    uint8_t *nb = realloc(buf, need);
                    if (!nb) { free(buf); return PM_ERR_NOMEM; }
                    buf = nb;
                    buf_cap = need;
                }
                memset(buf, 0, need);
                stbtt_MakeGlyphBitmapSubpixel(&f->info, buf, gw, gh, gw,
                                              m.scale, m.scale, shift, 0.0f, g);
                for (int py = dst.y; py < dst.y + dst.h; py++) {
                    const uint8_t *src = buf + (size_t)(py - gr.y) * gw + (dst.x - gr.x);
                    uint8_t *d = mask + (size_t)(py - mrect.y) * mrect.w + (dst.x - mrect.x);
                    for (int px = 0; px < dst.w; px++)
                        if (src[px] > d[px]) d[px] = src[px];
                }
            }
        }
        pen += (float)adv * m.scale;
        prev = g;
    }
    max_w = fmaxf(max_w, pen - x);
    free(buf);
    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (float)lines * m.line_h;
    return PM_OK;
}

static bool style_ok(const pm_text_style *st)
{
    return st && isfinite(st->size_px) && st->size_px >= 1.0f && st->size_px <= 4096.0f &&
           isfinite(st->outline_px) && st->outline_px >= 0.0f &&
           st->outline_px <= PM_MAX_OUTLINE_PX;
}

static bool text_args_ok(const pm_font *f, const char *utf8, float x, float y)
{
    return f && utf8 && isfinite(x) && isfinite(y) && fabsf(x) < 1.0e6f && fabsf(y) < 1.0e6f;
}

void pm_text_measure(const pm_font *font, const char *utf8, float size_px,
                     float *w, float *h)
{
    if (w) *w = 0.0f;
    if (h) *h = 0.0f;
    if (!text_args_ok(font, utf8, 0, 0) || !isfinite(size_px) || size_px < 1.0f ||
        size_px > 4096.0f)
        return;
    layout(NULL, (pm_rect){0, 0, 0, 0}, font, utf8, 0.0f, 0.0f, size_px, w, h);
}

pm_rect pm_text_bounds(const pm_font *font, const char *utf8, float x, float y,
                       const pm_text_style *st)
{
    if (!text_args_ok(font, utf8, x, y) || !style_ok(st)) return (pm_rect){0, 0, 0, 0};
    float w, h;
    pm_text_measure(font, utf8, st->size_px, &w, &h);
    /* glyphs may overhang their advance box (italics, accents) */
    int pad = (int)ceilf(st->size_px * 0.5f) + 2 + (int)ceilf(st->outline_px) + 1;
    return (pm_rect){(int)floorf(x) - pad, (int)floorf(y) - pad,
                     (int)ceilf(w) + 2 * pad + 1, (int)ceilf(h) + 2 * pad + 1};
}

/* Anti-aliased disk dilation, scattered from inked pixels only:
 * out = max over offsets o with |o| <= r of in(p - o) * w(|o|). */
static void dilate(const uint8_t *in, uint8_t *out, int w, int h, float r)
{
    int R = (int)ceilf(r + 0.5f);
    int side = 2 * R + 1;
    uint8_t *wt = malloc((size_t)side * (size_t)side);
    if (!wt) return;
    for (int j = -R; j <= R; j++)
        for (int i = -R; i <= R; i++) {
            float c = r + 0.5f - sqrtf((float)(i * i + j * j));
            wt[(j + R) * side + (i + R)] = c <= 0 ? 0 : c >= 1 ? 255 : (uint8_t)(c * 255.0f + 0.5f);
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned v = in[(size_t)y * w + x];
            if (!v) continue;
            for (int j = -R; j <= R; j++) {
                int yy = y + j;
                if (yy < 0 || yy >= h) continue;
                uint8_t *row = out + (size_t)yy * w;
                const uint8_t *wr = wt + (j + R) * side + R;
                for (int i = -R; i <= R; i++) {
                    int xx = x + i;
                    if (xx < 0 || xx >= w || !wr[i]) continue;
                    unsigned c = (v * wr[i] + 127u) / 255u;
                    if (c > row[xx]) row[xx] = (uint8_t)c;
                }
            }
        }
    free(wt);
}

pm_status pm_text_draw(pm_image *img, const pm_font *font, const char *utf8,
                       float x, float y, const pm_text_style *st)
{
    if (!img || !img->px) return PM_ERR_ARG;
    if (!font) return PM_ERR_FONT;
    if (!text_args_ok(font, utf8, x, y) || !style_ok(st)) return PM_ERR_ARG;

    pm_rect box = pm_rect_intersect(pm_text_bounds(font, utf8, x, y, st),
                                    (pm_rect){0, 0, img->w, img->h});
    if (pm_rect_empty(box)) return PM_OK;

    size_t area = (size_t)box.w * (size_t)box.h;
    uint8_t *mask = calloc(area, 1);
    if (!mask) return PM_ERR_NOMEM;
    pm_status res = layout(mask, box, font, utf8, x, y, st->size_px, NULL, NULL);
    if (res == PM_OK && st->outline_px > 0.0f) {
        uint8_t *ol = calloc(area, 1);
        if (!ol) res = PM_ERR_NOMEM;
        else {
            dilate(mask, ol, box.w, box.h, st->outline_px);
            pm_blend_mask(img, 0, 0, ol, box, box, st->outline);
            free(ol);
        }
    }
    if (res == PM_OK) pm_blend_mask(img, 0, 0, mask, box, box, st->color);
    free(mask);
    return res;
}
