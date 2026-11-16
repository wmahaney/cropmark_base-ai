/* Minimal EXIF orientation support: phone photos are routinely stored
 * sideways with a tag saying how to turn them, and stb_image ignores it. */
#include "pm_internal.h"

#include <stdlib.h>
#include <string.h>

static unsigned rd16(const uint8_t *p, bool be)
{
    return be ? (unsigned)(p[0] << 8 | p[1]) : (unsigned)(p[1] << 8 | p[0]);
}

static unsigned long rd32(const uint8_t *p, bool be)
{
    return be ? ((unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
                 (unsigned long)p[2] << 8 | p[3])
              : ((unsigned long)p[3] << 24 | (unsigned long)p[2] << 16 |
                 (unsigned long)p[1] << 8 | p[0]);
}

static int tiff_orientation(const uint8_t *t, size_t len)
{
    if (len < 8) return 1;
    bool be;
    if (t[0] == 'M' && t[1] == 'M') be = true;
    else if (t[0] == 'I' && t[1] == 'I') be = false;
    else return 1;
    if (rd16(t + 2, be) != 42) return 1;
    unsigned long ifd = rd32(t + 4, be);
    if (ifd > len - 2) return 1;
    unsigned count = rd16(t + ifd, be);
    for (unsigned i = 0; i < count; i++) {
        size_t off = (size_t)ifd + 2 + (size_t)i * 12;
        if (off + 12 > len) return 1;
        if (rd16(t + off, be) == 0x0112) {
            unsigned v = rd16(t + off + 8, be);
            return (v >= 1 && v <= 8) ? (int)v : 1;
        }
    }
    return 1;
}

int pm_exif_orientation(const uint8_t *d, size_t len)
{
    if (!d || len < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1; /* not JPEG */
    size_t pos = 2;
    while (pos + 4 <= len) {
        if (d[pos] != 0xFF) return 1;
        unsigned marker = d[pos + 1];
        if (marker == 0xFF) { pos++; continue; }           /* fill byte    */
        if (marker == 0xDA || marker == 0xD9) return 1;    /* SOS / EOI    */
        size_t seg = (size_t)d[pos + 2] << 8 | d[pos + 3]; /* incl. itself */
        if (seg < 2 || pos + 2 + seg > len) return 1;
        if (marker == 0xE1 && seg >= 8 &&
            memcmp(d + pos + 4, "Exif\0\0", 6) == 0)
            return tiff_orientation(d + pos + 10, seg - 8);
        pos += 2 + seg;
    }
    return 1;
}

pm_status pm_orient_apply(pm_image *img, int o)
{
    if (!img || !img->px) return PM_ERR_ARG;
    if (o <= 1 || o > 8) return PM_OK;
    int w = img->w, h = img->h;
    int W = o >= 5 ? h : w, H = o >= 5 ? w : h;
    uint32_t *dst = malloc((size_t)W * (size_t)H * 4);
    if (!dst) return PM_ERR_NOMEM;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int sx, sy;
            switch (o) {
            case 2:  sx = w - 1 - x; sy = y;         break; /* mirror horizontal */
            case 3:  sx = w - 1 - x; sy = h - 1 - y; break; /* rotate 180        */
            case 4:  sx = x;         sy = h - 1 - y; break; /* mirror vertical   */
            case 5:  sx = y;         sy = x;         break; /* transpose         */
            case 6:  sx = y;         sy = h - 1 - x; break; /* rotate 90 cw      */
            case 7:  sx = w - 1 - y; sy = h - 1 - x; break; /* transverse        */
            default: sx = w - 1 - y; sy = x;         break; /* 8: rotate 90 ccw  */
            }
            memcpy(&dst[(size_t)y * W + x],
                   img->px + ((size_t)sy * w + sx) * 4, 4);
        }
    }
    free(img->px);
    *img = (pm_image){W, H, (uint8_t *)dst};
    return PM_OK;
}
