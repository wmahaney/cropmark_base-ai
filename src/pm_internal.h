/* Private helpers shared between core translation units. */
#ifndef PM_INTERNAL_H
#define PM_INTERNAL_H

#include "pmark.h"

/* Input files larger than this are rejected before being read. */
#define PM_MAX_FILE_BYTES ((size_t)1 << 30)
/* ICC profiles larger than this are dropped (real ones are < 1 MB). */
#define PM_MAX_ICC_BYTES ((size_t)4 << 20)
/* Stroke widths and text outlines beyond these are rejected as PM_ERR_ARG. */
#define PM_MAX_STROKE_WIDTH 4096.0f
#define PM_MAX_OUTLINE_PX   64.0f

/* ------------------------------------------------------------ files etc. */

/* Reads a whole file; caller frees *data. */
pm_status pm_read_file(const char *path, uint8_t **data, size_t *len);
/* Canonical absolute path of an existing file, or NULL. malloc'd. */
char     *pm_realpath(const char *path);
char     *pm_strdup(const char *s);
uint64_t  pm_fnv1a64(const uint8_t *data, size_t len);
uint32_t  pm_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* Growable byte buffer used by the encoders. */
typedef struct { uint8_t *p; size_t n, cap; bool oom; } pm_buf;
void pm_buf_put(pm_buf *b, const void *data, size_t n);

/* ----------------------------------------------------------- stb shims */
/* (implemented in pm_stb.c so stb's internals stay in one TU) */

uint8_t *pm_stb_png(const uint8_t *px, int stride, int w, int h, int comp, int *len);
bool     pm_stb_jpg(pm_buf *out, const uint8_t *rgb, int w, int h, int quality);
uint8_t *pm_stb_zlib_compress(const uint8_t *data, int len, int *out_len);
uint8_t *pm_stb_zlib_decompress(const uint8_t *data, int len, int *out_len);

/* ------------------------------------------------------------- metadata */

/* EXIF orientation (1..8) of an in-memory JPEG, or 1 if absent/unparseable. */
int       pm_exif_orientation(const uint8_t *data, size_t len);
/* Rewrites img so that an image stored with the given orientation is upright. */
pm_status pm_orient_apply(pm_image *img, int orientation);
/* Extracts the ICC profile from a JPEG (APP2) or PNG (iCCP); NULL if none. */
uint8_t  *pm_icc_extract(const uint8_t *data, size_t len, size_t *icc_len);
/* Return a new encoded file with the profile embedded (input untouched). */
pm_status pm_icc_embed_png(pm_buf *out, const uint8_t *png, size_t len,
                           const uint8_t *icc, size_t icc_len);
pm_status pm_icc_embed_jpeg(pm_buf *out, const uint8_t *jpg, size_t len,
                            const uint8_t *icc, size_t icc_len);

/* -------------------------------------------------------------- drawing */

/* Blends colour c through an 8-bit coverage mask that covers mrect (image
 * coordinates, stride mrect.w), over region r, into img whose pixel (0,0)
 * is image coordinate (ox, oy). Everything is clipped. */
void pm_blend_mask(pm_image *img, int ox, int oy, const uint8_t *mask,
                   pm_rect mrect, pm_rect r, pm_color c);

/* Conservative pixel bounds of a text op (glyph overhang and outline). */
pm_rect pm_text_bounds(const pm_font *font, const char *utf8, float x, float y,
                       const pm_text_style *style);

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

/* -------------------------------------------------------------- session */

/* Parsed but not yet applied session (exposed for the fuzzer). Text ops
 * have font == NULL; fonts[i] holds op i's font path (NULL for non-text). */
typedef struct {
    char     *base;
    uint64_t  hash;
    pm_op    *ops;
    char    **fonts;
    size_t    n, cap;
} pm_session_data;

pm_status pm_session_parse(const char *text, size_t len, pm_session_data *out);
void      pm_session_data_free(pm_session_data *d);
void      pm_op_free(pm_op *op);

#endif /* PM_INTERNAL_H */
