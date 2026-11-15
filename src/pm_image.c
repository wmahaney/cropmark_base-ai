#include "pm_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"
#include "stb_image_write.h"

const char *pm_strerror(pm_status st)
{
    switch (st) {
    case PM_OK:         return "ok";
    case PM_ERR_IO:     return "I/O error";
    case PM_ERR_FORMAT: return "unsupported or corrupt file format";
    case PM_ERR_NOMEM:  return "out of memory";
    case PM_ERR_ARG:    return "invalid argument";
    case PM_ERR_FONT:   return "no usable font (pass one, or set PMARK_FONT)";
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

pm_status pm_image_create(pm_image *out, int w, int h, pm_color fill)
{
    if (!out || w <= 0 || h <= 0 || w > PM_MAX_DIM || h > PM_MAX_DIM)
        return PM_ERR_ARG;
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

pm_status pm_read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return PM_ERR_IO;
    size_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { fclose(f); return PM_ERR_NOMEM; }
    for (;;) {
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap) break;
        uint8_t *nb = realloc(buf, cap *= 2);
        if (!nb) { free(buf); fclose(f); return PM_ERR_NOMEM; }
        buf = nb;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) { free(buf); return PM_ERR_IO; }
    *data = buf;
    *len = n;
    return PM_OK;
}

pm_status pm_image_load(pm_image *out, const char *path)
{
    if (!out || !path) return PM_ERR_ARG;
    uint8_t *data;
    size_t len;
    pm_status st = pm_read_file(path, &data, &len);
    if (st != PM_OK) return st;
    if (len > INT_MAX) { free(data); return PM_ERR_FORMAT; }

    int w, h, n;
    uint8_t *px = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    int orient = px ? pm_exif_orientation(data, len) : 1;
    free(data);
    if (!px) return PM_ERR_FORMAT;
    if (w > PM_MAX_DIM || h > PM_MAX_DIM) { free(px); return PM_ERR_FORMAT; }

    pm_image img = {w, h, px}; /* stb allocates with malloc, so free() is fine */
    if (orient != 1 && (st = pm_orient_apply(&img, orient)) != PM_OK) {
        pm_image_free(&img);
        return st;
    }
    *out = img;
    return PM_OK;
}

static bool has_ext(const char *path, const char *ext)
{
    size_t lp = strlen(path), le = strlen(ext);
    if (lp < le) return false;
    for (size_t i = 0; i < le; i++)
        if (tolower((unsigned char)path[lp - le + i]) != ext[i]) return false;
    return true;
}

pm_status pm_image_save(const pm_image *img, const char *path, int jpeg_quality)
{
    if (!img || !img->px || !path) return PM_ERR_ARG;

    if (has_ext(path, ".png"))
        return stbi_write_png(path, img->w, img->h, 4, img->px, img->w * 4)
                   ? PM_OK : PM_ERR_IO;

    if (has_ext(path, ".jpg") || has_ext(path, ".jpeg")) {
        if (jpeg_quality <= 0) jpeg_quality = 92;
        if (jpeg_quality > 100) jpeg_quality = 100;
        size_t n = (size_t)img->w * (size_t)img->h;
        uint8_t *rgb = malloc(n * 3);
        if (!rgb) return PM_ERR_NOMEM;
        for (size_t i = 0; i < n; i++) { /* JPEG has no alpha: flatten on white */
            const uint8_t *s = img->px + 4 * i;
            unsigned a = s[3], ia = 255u - a;
            rgb[3 * i + 0] = (uint8_t)((s[0] * a + 255u * ia + 127u) / 255u);
            rgb[3 * i + 1] = (uint8_t)((s[1] * a + 255u * ia + 127u) / 255u);
            rgb[3 * i + 2] = (uint8_t)((s[2] * a + 255u * ia + 127u) / 255u);
        }
        int ok = stbi_write_jpg(path, img->w, img->h, 3, rgb, jpeg_quality);
        free(rgb);
        return ok ? PM_OK : PM_ERR_IO;
    }
    return PM_ERR_FORMAT;
}
