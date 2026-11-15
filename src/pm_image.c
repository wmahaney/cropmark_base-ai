#include "pm_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

const char *pm_strerror(pm_status st)
{
    switch (st) {
    case PM_OK:         return "ok";
    case PM_ERR_IO:     return "I/O error";
    case PM_ERR_FORMAT: return "unsupported or corrupt file format";
    case PM_ERR_NOMEM:  return "out of memory";
    case PM_ERR_ARG:    return "invalid argument";
    case PM_ERR_FONT:   return "no usable font (pass one, or set PMARK_FONT)";
    case PM_ERR_TOO_BIG: return "image too large";
    case PM_ERR_EXISTS: return "file exists (refusing to overwrite)";
    case PM_ERR_STALE:  return "the session's base image has changed since it was saved";
    }
    return "unknown error";
}

/* ----------------------------------------------------------------- rects */

bool pm_rect_empty(pm_rect r) { return r.w <= 0 || r.h <= 0; }

pm_rect pm_rect_intersect(pm_rect a, pm_rect b)
{
    /* long long so that huge-but-valid rects cannot overflow x + w */
    long long x0 = a.x > b.x ? a.x : b.x;
    long long y0 = a.y > b.y ? a.y : b.y;
    long long ax1 = (long long)a.x + a.w, bx1 = (long long)b.x + b.w;
    long long ay1 = (long long)a.y + a.h, by1 = (long long)b.y + b.h;
    long long x1 = ax1 < bx1 ? ax1 : bx1;
    long long y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0) return (pm_rect){0, 0, 0, 0};
    return (pm_rect){(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
}

pm_rect pm_rect_union(pm_rect a, pm_rect b)
{
    if (pm_rect_empty(a)) return pm_rect_empty(b) ? (pm_rect){0, 0, 0, 0} : b;
    if (pm_rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (pm_rect){x0, y0, x1 - x0, y1 - y0};
}

/* ---------------------------------------------------------------- colour */

bool pm_color_parse(const char *hex, pm_color *out)
{
    if (!hex || !out) return false;
    if (*hex == '#') hex++;
    size_t n = strlen(hex);
    if (n != 6 && n != 8) return false;
    uint8_t v[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < n; i++) {
        int c = tolower((unsigned char)hex[i]), d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;
        v[i / 2] = (i % 2 == 0) ? (uint8_t)(d << 4) : (uint8_t)(v[i / 2] | d);
    }
    *out = (pm_color){v[0], v[1], v[2], v[3]};
    return true;
}

/* ----------------------------------------------------------------- image */

static bool dims_ok(int w, int h)
{
    return w > 0 && h > 0 && w <= PM_MAX_DIM && h <= PM_MAX_DIM &&
           (uint64_t)w * (uint64_t)h <= PM_MAX_PIXELS;
}

void pm_meta_free(pm_meta *m)
{
    if (!m) return;
    free(m->icc);
    m->icc = NULL;
    m->icc_len = 0;
}

pm_status pm_meta_copy(pm_meta *out, const pm_meta *src)
{
    if (!out || !src) return PM_ERR_ARG;
    pm_meta m = {NULL, 0};
    if (src->icc && src->icc_len) {
        if (!(m.icc = malloc(src->icc_len))) return PM_ERR_NOMEM;
        memcpy(m.icc, src->icc, src->icc_len);
        m.icc_len = src->icc_len;
    }
    *out = m;
    return PM_OK;
}

pm_status pm_image_create(pm_image *out, int w, int h, pm_color fill)
{
    if (!out) return PM_ERR_ARG;
    if (!dims_ok(w, h)) return w > 0 && h > 0 ? PM_ERR_TOO_BIG : PM_ERR_ARG;
    size_t n = (size_t)w * (size_t)h;
    uint8_t *px = malloc(n * 4);
    if (!px) return PM_ERR_NOMEM;
    for (size_t i = 0; i < n; i++) memcpy(px + 4 * i, &fill, 4);
    *out = (pm_image){w, h, px};
    return PM_OK;
}

pm_status pm_image_copy(pm_image *out, const pm_image *src)
{
    if (!out || !src || !src->px) return PM_ERR_ARG;
    size_t bytes = (size_t)src->w * (size_t)src->h * 4;
    uint8_t *px = malloc(bytes);
    if (!px) return PM_ERR_NOMEM;
    memcpy(px, src->px, bytes);
    *out = (pm_image){src->w, src->h, px};
    return PM_OK;
}

pm_status pm_image_crop(pm_image *out, const pm_image *src, pm_rect r)
{
    if (!out || !src || !src->px) return PM_ERR_ARG;
    r = pm_rect_intersect(r, (pm_rect){0, 0, src->w, src->h});
    if (pm_rect_empty(r)) return PM_ERR_ARG;
    uint8_t *px = malloc((size_t)r.w * (size_t)r.h * 4);
    if (!px) return PM_ERR_NOMEM;
    for (int y = 0; y < r.h; y++)
        memcpy(px + (size_t)y * r.w * 4,
               src->px + ((size_t)(r.y + y) * src->w + r.x) * 4,
               (size_t)r.w * 4);
    *out = (pm_image){r.w, r.h, px};
    return PM_OK;
}

void pm_image_free(pm_image *img)
{
    if (!img) return;
    free(img->px);
    *img = (pm_image){0, 0, NULL};
}

/* ------------------------------------------------------------------- I/O */

static bool dims_ok_l(long w, long h)
{
    return w > 0 && h > 0 && w <= PM_MAX_DIM && h <= PM_MAX_DIM &&
           (uint64_t)w * (uint64_t)h <= PM_MAX_PIXELS;
}

/* Dimensions from a PNG IHDR or JPEG SOF header (error reporting only). */
static bool header_dims(const uint8_t *d, size_t len, long *w, long *h)
{
    if (len >= 24 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8) && !memcmp(d + 12, "IHDR", 4)) {
        *w = (long)((uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19]);
        *h = (long)((uint32_t)d[20] << 24 | (uint32_t)d[21] << 16 | (uint32_t)d[22] << 8 | d[23]);
        return true;
    }
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    for (size_t pos = 2; pos + 9 <= len;) {
        if (d[pos] != 0xFF) return false;
        unsigned m = d[pos + 1];
        if (m == 0xFF) { pos++; continue; }
        size_t seg = (size_t)d[pos + 2] << 8 | d[pos + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (long)d[pos + 5] << 8 | d[pos + 6];
            *w = (long)d[pos + 7] << 8 | d[pos + 8];
            return true;
        }
        if (seg < 2) return false;
        pos += 2 + seg;
    }
    return false;
}

pm_status pm_image_decode(pm_image *out, pm_meta *meta, const uint8_t *data,
                          size_t len)
{
    if (!out || !data) return PM_ERR_ARG;
    if (len > INT_MAX) return PM_ERR_TOO_BIG;

    /* Header-only probe first: a hostile or merely huge file is rejected
     * before stb allocates anything for its pixels. */
    int w, h, n;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &n)) {
        /* stb rejects some oversized headers itself, but its error reason is
         * overwritten as it tries the other formats; report them honestly */
        long hw, hh;
        return header_dims(data, len, &hw, &hh) && !dims_ok_l(hw, hh) ? PM_ERR_TOO_BIG
                                                                       : PM_ERR_FORMAT;
    }
    if (!dims_ok(w, h)) return PM_ERR_TOO_BIG;

    uint8_t *px = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    if (!px) return PM_ERR_FORMAT;
    if (!dims_ok(w, h)) { free(px); return PM_ERR_TOO_BIG; } /* paranoia */

    pm_image img = {w, h, px}; /* stb allocates with malloc, so free() is fine */
    int orient = pm_exif_orientation(data, len);
    pm_status st = pm_orient_apply(&img, orient);
    if (st != PM_OK) { pm_image_free(&img); return st; }
    if (meta) {
        meta->icc = pm_icc_extract(data, len, &meta->icc_len);
    }
    *out = img;
    return PM_OK;
}

pm_status pm_image_load_ex(pm_image *out, pm_meta *meta, const char *path)
{
    if (!out || !path) return PM_ERR_ARG;
    uint8_t *data;
    size_t len;
    pm_status st = pm_read_file(path, &data, &len);
    if (st != PM_OK) return st;
    st = pm_image_decode(out, meta, data, len);
    free(data);
    return st;
}

pm_status pm_image_load(pm_image *out, const char *path)
{
    return pm_image_load_ex(out, NULL, path);
}

static bool has_ext(const char *path, const char *ext)
{
    size_t lp = strlen(path), le = strlen(ext);
    if (lp < le) return false;
    for (size_t i = 0; i < le; i++)
        if (tolower((unsigned char)path[lp - le + i]) != ext[i]) return false;
    return true;
}

pm_format pm_format_from_path(const char *path)
{
    if (!path) return PM_FMT_UNKNOWN;
    if (has_ext(path, ".png")) return PM_FMT_PNG;
    if (has_ext(path, ".jpg") || has_ext(path, ".jpeg")) return PM_FMT_JPEG;
    return PM_FMT_UNKNOWN;
}

static bool fully_opaque(const pm_image *img)
{
    size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; i++)
        if (img->px[4 * i + 3] != 255) return false;
    return true;
}

/* Packs RGBA to RGB; with flatten, composites onto white first. */
static uint8_t *to_rgb(const pm_image *img, bool flatten)
{
    size_t n = (size_t)img->w * (size_t)img->h;
    uint8_t *rgb = malloc(n * 3);
    if (!rgb) return NULL;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *s = img->px + 4 * i;
        unsigned a = flatten ? s[3] : 255u, ia = 255u - a;
        for (int c = 0; c < 3; c++)
            rgb[3 * i + c] = (uint8_t)((s[c] * a + 255u * ia + 127u) / 255u);
    }
    return rgb;
}

pm_status pm_image_encode(const pm_image *img, pm_format fmt,
                          const pm_save_opts *opts, uint8_t **out, size_t *len)
{
    if (!img || !img->px || !out || !len) return PM_ERR_ARG;
    const pm_meta *meta = opts ? opts->meta : NULL;
    bool icc = meta && meta->icc && meta->icc_len;
    pm_buf enc = {0};

    if (fmt == PM_FMT_PNG) {
        bool opaque = fully_opaque(img); /* RGB PNGs are ~25% smaller */
        uint8_t *rgb = opaque ? to_rgb(img, false) : NULL;
        if (opaque && !rgb) return PM_ERR_NOMEM;
        int n = 0;
        uint8_t *png = opaque ? pm_stb_png(rgb, img->w * 3, img->w, img->h, 3, &n)
                              : pm_stb_png(img->px, img->w * 4, img->w, img->h, 4, &n);
        free(rgb);
        if (!png) return PM_ERR_NOMEM;
        enc = (pm_buf){png, (size_t)n, (size_t)n, false};
    } else if (fmt == PM_FMT_JPEG) {
        int q = opts ? opts->jpeg_quality : 0;
        if (q <= 0) q = 92;
        if (q > 100) q = 100;
        uint8_t *rgb = to_rgb(img, true); /* JPEG has no alpha */
        if (!rgb) return PM_ERR_NOMEM;
        bool ok = pm_stb_jpg(&enc, rgb, img->w, img->h, q);
        free(rgb);
        if (!ok) { free(enc.p); return PM_ERR_NOMEM; }
    } else {
        return PM_ERR_FORMAT;
    }

    if (icc) {
        pm_buf with = {0};
        pm_status st = fmt == PM_FMT_PNG
            ? pm_icc_embed_png(&with, enc.p, enc.n, meta->icc, meta->icc_len)
            : pm_icc_embed_jpeg(&with, enc.p, enc.n, meta->icc, meta->icc_len);
        free(enc.p);
        if (st != PM_OK) { free(with.p); return st; }
        enc = with;
    }
    *out = enc.p;
    *len = enc.n;
    return PM_OK;
}

pm_status pm_image_save_ex(const pm_image *img, const char *path,
                           const pm_save_opts *opts)
{
    if (!img || !img->px || !path) return PM_ERR_ARG;
    pm_format fmt = pm_format_from_path(path);
    if (fmt == PM_FMT_UNKNOWN) return PM_ERR_FORMAT;
    bool overwrite = opts ? opts->overwrite : true;
    if (!overwrite && pm_path_exists(path)) return PM_ERR_EXISTS; /* fail fast */
    uint8_t *data;
    size_t len;
    pm_status st = pm_image_encode(img, fmt, opts, &data, &len);
    if (st != PM_OK) return st;
    st = pm_write_file_atomic(path, data, len, overwrite);
    free(data);
    return st;
}

pm_status pm_image_save(const pm_image *img, const char *path, int jpeg_quality)
{
    pm_save_opts o = {jpeg_quality, NULL, true};
    return pm_image_save_ex(img, path, &o);
}
