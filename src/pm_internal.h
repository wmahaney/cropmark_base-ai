/* Private helpers shared between core translation units. */
#ifndef PM_INTERNAL_H
#define PM_INTERNAL_H

#include "pmark.h"

/* Reads a whole file; caller frees *data. */
pm_status pm_read_file(const char *path, uint8_t **data, size_t *len);

/* EXIF orientation (1..8) of an in-memory JPEG, or 1 if absent/unparseable. */
int       pm_exif_orientation(const uint8_t *data, size_t len);
/* Rewrites img so that an image stored with the given orientation is upright. */
pm_status pm_orient_apply(pm_image *img, int orientation);

/* Straight-alpha "source over" of colour c at coverage cov (0..255). */
static inline void pm_blend_px(uint8_t *d, pm_color c, unsigned cov)
{
    unsigned sa = (c.a * cov + 127u) / 255u;
    if (sa == 0) return;
    unsigned da = d[3];
    if (da == 255) { /* opaque destination: the common photo case */
        unsigned ia = 255u - sa;
        d[0] = (uint8_t)((c.r * sa + d[0] * ia + 127u) / 255u);
        d[1] = (uint8_t)((c.g * sa + d[1] * ia + 127u) / 255u);
        d[2] = (uint8_t)((c.b * sa + d[2] * ia + 127u) / 255u);
        return;
    }
    unsigned db = da * (255u - sa); /* dst weight, scaled by 255 */
    unsigned oa = sa * 255u + db;   /* out alpha, scaled by 255  */
    d[0] = (uint8_t)((c.r * sa * 255u + d[0] * db + oa / 2) / oa);
    d[1] = (uint8_t)((c.g * sa * 255u + d[1] * db + oa / 2) / oa);
    d[2] = (uint8_t)((c.b * sa * 255u + d[2] * db + oa / 2) / oa);
    d[3] = (uint8_t)((oa + 127u) / 255u);
}

#endif /* PM_INTERNAL_H */
