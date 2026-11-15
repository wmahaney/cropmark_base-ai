#include "pm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

struct pm_font {
    uint8_t       *data; /* the font file; stbtt_fontinfo points into it */
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

void pm_font_free(pm_font *f)
{
    if (!f) return;
    free(f->data);
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

/* Shared layout walk. With img == NULL it only measures. */
static pm_status layout(pm_image *img, const pm_font *f, const char *utf8,
                        float x, float y, float size_px, pm_color color,
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
            if (pen - x > max_w) max_w = pen - x;
            pen = x;
            prev = 0;
            baseline = (int)floorf(y + m.ascent + (float)lines * m.line_h + 0.5f);
            lines++;
            continue;
        }
        int g = stbtt_FindGlyphIndex(&f->info, cp);
        if (prev) pen += stbtt_GetGlyphKernAdvance(&f->info, prev, g) * m.scale;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);

        if (img) {
            float fl = floorf(pen), shift = pen - fl;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&f->info, g, m.scale, m.scale,
                                            shift, 0.0f, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
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
                pm_rect dst = pm_rect_intersect(
                    (pm_rect){(int)fl + x0, baseline + y0, gw, gh},
                    (pm_rect){0, 0, img->w, img->h});
                for (int py = dst.y; py < dst.y + dst.h; py++) {
                    const uint8_t *src = buf + (size_t)(py - baseline - y0) * gw
                                             + (dst.x - (int)fl - x0);
                    uint8_t *d = img->px + ((size_t)py * img->w + dst.x) * 4;
                    for (int px = 0; px < dst.w; px++, d += 4)
                        if (src[px]) pm_blend_px(d, color, src[px]);
                }
            }
        }
        pen += adv * m.scale;
        prev = g;
    }
    if (pen - x > max_w) max_w = pen - x;
    free(buf);
    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (float)lines * m.line_h;
    return PM_OK;
}

static bool text_args_ok(const pm_font *f, const char *utf8, float x, float y,
                         float size_px)
{
    return f && utf8 && isfinite(x) && isfinite(y) && isfinite(size_px) &&
           fabsf(x) < 1.0e6f && fabsf(y) < 1.0e6f &&
           size_px >= 1.0f && size_px <= 4096.0f;
}

pm_status pm_text_draw(pm_image *img, const pm_font *font, const char *utf8,
                       float x, float y, float size_px, pm_color color)
{
    if (!img || !img->px) return PM_ERR_ARG;
    if (!font) return PM_ERR_FONT;
    if (!text_args_ok(font, utf8, x, y, size_px)) return PM_ERR_ARG;
    return layout(img, font, utf8, x, y, size_px, color, NULL, NULL);
}

void pm_text_measure(const pm_font *font, const char *utf8, float size_px,
                     float *w, float *h)
{
    if (w) *w = 0.0f;
    if (h) *h = 0.0f;
    if (!text_args_ok(font, utf8, 0.0f, 0.0f, size_px)) return;
    layout(NULL, font, utf8, 0.0f, 0.0f, size_px, (pm_color){0, 0, 0, 0}, w, h);
}
