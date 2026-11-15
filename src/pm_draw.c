#include "pm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* float -> int without UB for wild inputs */
static int to_int(float v)
{
    if (v < -1.0e7f) return -10000000;
    if (v > 1.0e7f) return 10000000;
    return (int)v;
}

static float radius_of(float width)
{
    return (width > 0.1f ? width : 0.1f) * 0.5f;
}

pm_rect pm_stroke_bounds(const pm_point *pts, size_t n, float width)
{
    if (!pts || n == 0) return (pm_rect){0, 0, 0, 0};
    float r = radius_of(width) + 1.0f; /* +1 for the anti-aliasing fringe */
    float x0 = pts[0].x, x1 = x0, y0 = pts[0].y, y1 = y0;
    for (size_t i = 1; i < n; i++) {
        x0 = fminf(x0, pts[i].x); x1 = fmaxf(x1, pts[i].x);
        y0 = fminf(y0, pts[i].y); y1 = fmaxf(y1, pts[i].y);
    }
    int ix0 = to_int(floorf(x0 - r)), iy0 = to_int(floorf(y0 - r));
    int ix1 = to_int(ceilf(x1 + r)), iy1 = to_int(ceilf(y1 + r));
    return (pm_rect){ix0, iy0, ix1 - ix0, iy1 - iy0};
}

static float seg_dist(float px, float py, pm_point a, pm_point b)
{
    float vx = b.x - a.x, vy = b.y - a.y;
    float wx = px - a.x, wy = py - a.y;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

void pm_blend_mask(pm_image *img, int ox, int oy, const uint8_t *mask,
                   pm_rect mrect, pm_rect r, pm_color c)
{
    r = pm_rect_intersect(r, mrect);
    r = pm_rect_intersect(r, (pm_rect){ox, oy, img->w, img->h});
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint8_t *m = mask + (size_t)(y - mrect.y) * mrect.w + (r.x - mrect.x);
        uint8_t *d = img->px + ((size_t)(y - oy) * img->w + (r.x - ox)) * 4;
        for (int x = 0; x < r.w; x++, d += 4)
            if (m[x]) pm_blend_px(d, c, m[x]);
    }
}

/* -------------------------------------------------------- stroke builder */

struct pm_stroke {
    pm_rect  area;
    float    width, r;
    uint8_t *mask; /* area.w * area.h, max coverage so far */
    pm_point last;
    bool     has_last;
};

pm_status pm_stroke_begin(pm_stroke **out, pm_rect area, float width)
{
    if (!out || pm_rect_empty(area) || !isfinite(width) || width > PM_MAX_STROKE_WIDTH)
        return PM_ERR_ARG;
    pm_stroke *s = calloc(1, sizeof *s);
    if (!s) return PM_ERR_NOMEM;
    s->mask = calloc((size_t)area.w * (size_t)area.h, 1);
    if (!s->mask) { free(s); return PM_ERR_NOMEM; }
    s->area = area;
    s->width = width;
    s->r = radius_of(width);
    *out = s;
    return PM_OK;
}

/* Rasterises the capsule a-b into the mask, visiting only its own box. */
static pm_rect raster_segment(pm_stroke *s, pm_point a, pm_point b)
{
    pm_rect sb = pm_rect_intersect(pm_stroke_bounds((pm_point[]){a, b}, 2, s->width), s->area);
    for (int y = sb.y; y < sb.y + sb.h; y++) {
        uint8_t *row = s->mask + (size_t)(y - s->area.y) * s->area.w - s->area.x;
        for (int x = sb.x; x < sb.x + sb.w; x++) {
            float c = s->r + 0.5f - seg_dist((float)x + 0.5f, (float)y + 0.5f, a, b);
            if (c <= 0.0f) continue;
            uint8_t cov = c >= 1.0f ? 255 : (uint8_t)(c * 255.0f + 0.5f);
            if (cov > row[x]) row[x] = cov;
        }
    }
    return sb;
}

pm_rect pm_stroke_add(pm_stroke *s, pm_point p)
{
    if (!s || !isfinite(p.x) || !isfinite(p.y)) return (pm_rect){0, 0, 0, 0};
    pm_point a = s->has_last ? s->last : p;
    s->last = p;
    s->has_last = true;
    return raster_segment(s, a, p);
}

void pm_stroke_composite(const pm_stroke *s, pm_image *img, int ox, int oy,
                         pm_rect r, pm_color color)
{
    if (!s || !img || !img->px) return;
    pm_blend_mask(img, ox, oy, s->mask, s->area, r, color);
}

void pm_stroke_end(pm_stroke *s)
{
    if (!s) return;
    free(s->mask);
    free(s);
}

pm_status pm_draw_stroke(pm_image *img, const pm_point *pts, size_t n,
                         float width, pm_color color)
{
    if (!img || !img->px || !pts || n == 0) return PM_ERR_ARG;
    for (size_t i = 0; i < n; i++)
        if (!isfinite(pts[i].x) || !isfinite(pts[i].y)) return PM_ERR_ARG;
    if (!isfinite(width) || width > PM_MAX_STROKE_WIDTH) return PM_ERR_ARG;

    pm_rect bb = pm_rect_intersect(pm_stroke_bounds(pts, n, width),
                                   (pm_rect){0, 0, img->w, img->h});
    if (pm_rect_empty(bb)) return PM_OK; /* entirely off-canvas */

    pm_stroke *s;
    pm_status st = pm_stroke_begin(&s, bb, width);
    if (st != PM_OK) return st;
    for (size_t i = 0; i < n; i++) pm_stroke_add(s, pts[i]);
    pm_stroke_composite(s, img, 0, 0, bb, color);
    pm_stroke_end(s);
    return PM_OK;
}

/* ----------------------------------------------------------------- shapes */

pm_point *pm_shape_arrow(pm_point from, pm_point to, float width, size_t *n)
{
    if (!n) return NULL;
    float dx = to.x - from.x, dy = to.y - from.y, len = hypotf(dx, dy);
    pm_point *p = malloc(5 * sizeof *p);
    if (!p) return NULL;
    if (!(len > 0.0f)) { p[0] = to; *n = 1; return p; }
    /* head scales with the line width but never exceeds 60% of the shaft */
    float head = fminf(fmaxf(4.0f * width, 12.0f), 0.6f * len);
    float ux = dx / len, uy = dy / len;
    const float c = 0.8829476f, s = 0.4694716f; /* cos, sin of 28 degrees */
    pm_point l = {to.x - head * (ux * c - uy * s), to.y - head * (uy * c + ux * s)};
    pm_point r = {to.x - head * (ux * c + uy * s), to.y - head * (uy * c - ux * s)};
    /* one polyline that retraces the tip: max coverage makes that free */
    p[0] = from; p[1] = to; p[2] = l; p[3] = to; p[4] = r;
    *n = 5;
    return p;
}

pm_point *pm_shape_rect(pm_rect r, size_t *n)
{
    if (!n) return NULL;
    pm_point *p = malloc(5 * sizeof *p);
    if (!p) return NULL;
    float x0 = (float)r.x, y0 = (float)r.y, x1 = x0 + (float)r.w, y1 = y0 + (float)r.h;
    p[0] = (pm_point){x0, y0}; p[1] = (pm_point){x1, y0};
    p[2] = (pm_point){x1, y1}; p[3] = (pm_point){x0, y1};
    p[4] = p[0];
    *n = 5;
    return p;
}

pm_point *pm_shape_ellipse(pm_rect r, size_t *n)
{
    if (!n) return NULL;
    float rx = (float)r.w * 0.5f, ry = (float)r.h * 0.5f;
    float cx = (float)r.x + rx, cy = (float)r.y + ry;
    /* ~1 vertex per 4 px of circumference keeps the chord error sub-pixel */
    float per = 6.2831853f * sqrtf((rx * rx + ry * ry) * 0.5f);
    size_t k = (size_t)fminf(fmaxf(per / 4.0f, 24.0f), 720.0f);
    pm_point *p = malloc((k + 1) * sizeof *p);
    if (!p) return NULL;
    for (size_t i = 0; i < k; i++) {
        float t = 6.2831853f * (float)i / (float)k;
        p[i] = (pm_point){cx + rx * cosf(t), cy + ry * sinf(t)};
    }
    p[k] = p[0];
    *n = k + 1;
    return p;
}
