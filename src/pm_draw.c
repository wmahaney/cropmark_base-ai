#include "pm_internal.h"

#include <math.h>
#include <stdlib.h>

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
        if (pts[i].x < x0) x0 = pts[i].x;
        if (pts[i].x > x1) x1 = pts[i].x;
        if (pts[i].y < y0) y0 = pts[i].y;
        if (pts[i].y > y1) y1 = pts[i].y;
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

pm_status pm_draw_stroke(pm_image *img, const pm_point *pts, size_t n,
                         float width, pm_color color)
{
    if (!img || !img->px || !pts || n == 0) return PM_ERR_ARG;
    for (size_t i = 0; i < n; i++)
        if (!isfinite(pts[i].x) || !isfinite(pts[i].y)) return PM_ERR_ARG;
    if (!isfinite(width)) return PM_ERR_ARG;

    float r = radius_of(width);
    pm_rect bb = pm_rect_intersect(pm_stroke_bounds(pts, n, width),
                                   (pm_rect){0, 0, img->w, img->h});
    if (pm_rect_empty(bb)) return PM_OK; /* entirely off-canvas */

    /* Pass 1: coverage mask = max over segments of capsule coverage.
     * Each segment only visits its own bounding box, so cost is the sum of
     * segment boxes rather than (stroke box) x (segment count). */
    uint8_t *mask = calloc((size_t)bb.w * (size_t)bb.h, 1);
    if (!mask) return PM_ERR_NOMEM;

    size_t nseg = n > 1 ? n - 1 : 1;
    for (size_t s = 0; s < nseg; s++) {
        pm_point a = pts[s], b = pts[n > 1 ? s + 1 : s];
        pm_rect sb = pm_rect_intersect(pm_stroke_bounds((pm_point[]){a, b}, 2, width), bb);
        for (int y = sb.y; y < sb.y + sb.h; y++) {
            uint8_t *row = mask + (size_t)(y - bb.y) * bb.w;
            for (int x = sb.x; x < sb.x + sb.w; x++) {
                float c = r + 0.5f - seg_dist(x + 0.5f, y + 0.5f, a, b);
                if (c <= 0.0f) continue;
                uint8_t cov = c >= 1.0f ? 255 : (uint8_t)(c * 255.0f + 0.5f);
                if (cov > row[x - bb.x]) row[x - bb.x] = cov;
            }
        }
    }

    /* Pass 2: composite the colour through the mask exactly once. */
    for (int y = 0; y < bb.h; y++) {
        const uint8_t *m = mask + (size_t)y * bb.w;
        uint8_t *d = img->px + ((size_t)(bb.y + y) * img->w + bb.x) * 4;
        for (int x = 0; x < bb.w; x++, d += 4)
            if (m[x]) pm_blend_px(d, color, m[x]);
    }
    free(mask);
    return PM_OK;
}
