/*
 * pmark -- photo crop / marker / text annotation core.
 *
 * Conventions (see docs/ARCHITECTURE.md):
 *   - Images are RGBA8, straight (non-premultiplied) alpha, row-major,
 *     stride = 4 * w, origin top-left.
 *   - Continuous coordinates: pixel (i, j) covers [i, i+1) x [j, j+1), so its
 *     centre is (i + 0.5, j + 0.5).
 *   - Every fallible function returns pm_status; PM_OK == 0.
 *   - The core never touches global state and never prints.
 */
#ifndef PMARK_H
#define PMARK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- basics */

typedef enum {
    PM_OK = 0,
    PM_ERR_IO,     /* cannot read / write file            */
    PM_ERR_FORMAT, /* undecodable or unsupported format   */
    PM_ERR_NOMEM,  /* allocation failed                   */
    PM_ERR_ARG,    /* bad argument (empty crop, NaN, ...) */
    PM_ERR_FONT,   /* no usable font                      */
    PM_ERR_TOO_BIG,/* image exceeds PM_MAX_DIM/PM_MAX_PIXELS */
    PM_ERR_EXISTS, /* refused to overwrite an existing file */
    PM_ERR_STALE   /* session's base image has changed     */
} pm_status;

const char *pm_strerror(pm_status st);

typedef struct { uint8_t r, g, b, a; } pm_color;
typedef struct { int x, y, w, h; } pm_rect;
typedef struct { float x, y; } pm_point;

bool    pm_rect_empty(pm_rect r);
pm_rect pm_rect_intersect(pm_rect a, pm_rect b);
pm_rect pm_rect_union(pm_rect a, pm_rect b); /* empty rects are ignored */

/* "RRGGBB" or "RRGGBBAA", optional leading '#'. */
bool pm_color_parse(const char *hex, pm_color *out);

/* ----------------------------------------------------------------- image */

typedef struct {
    int      w, h;
    uint8_t *px; /* owned; w * h * 4 bytes */
} pm_image;

/* Limits, checked from the file header before any pixel is decoded, so an
 * oversized or hostile file costs nothing. 128 MP is 512 MB per RGBA copy. */
#define PM_MAX_DIM    32768
#define PM_MAX_PIXELS (128u * 1024u * 1024u)

/* Metadata carried from input to output. Only the ICC colour profile is
 * kept; EXIF is deliberately dropped (see docs/ARCHITECTURE.md, Persistence). */
typedef struct {
    uint8_t *icc; /* owned, may be NULL */
    size_t   icc_len;
} pm_meta;

void      pm_meta_free(pm_meta *meta);
pm_status pm_meta_copy(pm_meta *out, const pm_meta *src);

typedef enum { PM_FMT_UNKNOWN, PM_FMT_PNG, PM_FMT_JPEG } pm_format;
pm_format pm_format_from_path(const char *path); /* by extension */

typedef struct {
    int            jpeg_quality; /* 1..100; <= 0 selects 92            */
    const pm_meta *meta;         /* embedded if non-NULL (ICC profile) */
    bool           overwrite;    /* false: PM_ERR_EXISTS if path exists */
} pm_save_opts;

pm_status pm_image_create(pm_image *out, int w, int h, pm_color fill);
pm_status pm_image_copy(pm_image *out, const pm_image *src);
/* Decodes JPEG/PNG. JPEG EXIF orientation is applied, so the result is
 * always upright. meta may be NULL. */
pm_status pm_image_load_ex(pm_image *out, pm_meta *meta, const char *path);
pm_status pm_image_decode(pm_image *out, pm_meta *meta, const uint8_t *data,
                          size_t len);
pm_status pm_image_load(pm_image *out, const char *path); /* meta discarded */
/* Encodes to memory. PNGs of fully opaque images are written as RGB.
 * JPEG flattens alpha on white. opts may be NULL. */
pm_status pm_image_encode(const pm_image *img, pm_format fmt,
                          const pm_save_opts *opts, uint8_t **out, size_t *len);
/* Atomic: the file at path is either the previous file or the complete new
 * one, never a partial write. Format chosen by extension. opts may be NULL
 * (which means overwrite = true, no metadata, default quality). */
pm_status pm_image_save_ex(const pm_image *img, const char *path,
                           const pm_save_opts *opts);
pm_status pm_image_save(const pm_image *img, const char *path, int jpeg_quality);
/* r is clipped to the image; an empty result is PM_ERR_ARG. */
pm_status pm_image_crop(pm_image *out, const pm_image *src, pm_rect r);
void      pm_image_free(pm_image *img);

/* ------------------------------------------------------------------ files */

/* Writes data to path via a temporary file in the same directory and a
 * rename (or, with overwrite == false, a link that fails if path exists). */
pm_status pm_write_file_atomic(const char *path, const uint8_t *data,
                               size_t len, bool overwrite);
bool      pm_path_exists(const char *path);
/* True if both paths name the same existing file (hard links included). */
bool      pm_path_same_file(const char *a, const char *b);

/* --------------------------------------------------------------- drawing */

/* Anti-aliased polyline with round caps and joins. The whole stroke is
 * composited once, so a translucent stroke that crosses itself does not
 * darken at the overlap (highlighter behaviour). n == 1 draws a dot. */
pm_status pm_draw_stroke(pm_image *img, const pm_point *pts, size_t n,
                         float width, pm_color color);
pm_rect   pm_stroke_bounds(const pm_point *pts, size_t n, float width);

/* Incremental form of the same rasteriser, for live drawing: each added
 * point rasterises one new segment into a coverage mask over `area`, and
 * the caller recomposites only the returned dirty rectangle. Coverage is a
 * per-pixel max, so the final mask -- and therefore the committed pixels --
 * is identical to pm_draw_stroke over the same points. */
typedef struct pm_stroke pm_stroke;
pm_status pm_stroke_begin(pm_stroke **out, pm_rect area, float width);
/* Returns the dirty rect (image coordinates, inside area); empty if the
 * point was rejected (non-finite) or the segment is entirely outside. */
pm_rect   pm_stroke_add(pm_stroke *s, pm_point p);
/* Blends colour through the mask over rect r (image coordinates) into img,
 * whose pixel (0,0) corresponds to image coordinate (ox, oy). */
void      pm_stroke_composite(const pm_stroke *s, pm_image *img, int ox, int oy,
                              pm_rect r, pm_color color);
void      pm_stroke_end(pm_stroke *s);

/* Shape helpers: they return malloc'd point lists to be drawn as one
 * stroke (so translucent shapes never double-blend where lines meet). */
pm_point *pm_shape_arrow(pm_point from, pm_point to, float width, size_t *n);
pm_point *pm_shape_rect(pm_rect r, size_t *n);
pm_point *pm_shape_ellipse(pm_rect r, size_t *n);

/* ------------------------------------------------------------------ text */

typedef struct pm_font pm_font;

pm_status pm_font_load(pm_font **out, const char *ttf_path);
/* Tries $PMARK_FONT, then a list of common system font locations. */
pm_status pm_font_load_default(pm_font **out);
/* Canonical (absolute when resolvable) path the font was loaded from. */
const char *pm_font_path(const pm_font *font);
void      pm_font_free(pm_font *font);

typedef struct {
    float    size_px;    /* pixel height, ascent to descent        */
    pm_color color;
    float    outline_px; /* 0 = none; drawn outside the glyphs     */
    pm_color outline;    /* outline colour                         */
} pm_text_style;

/* UTF-8, '\n' starts a new line. (x, y) is the top-left of the text box.
 * Glyph coverage is accumulated into one mask and composited once, so
 * translucent text does not darken where glyphs overlap. */
pm_status pm_text_draw(pm_image *img, const pm_font *font, const char *utf8,
                       float x, float y, const pm_text_style *style);
void      pm_text_measure(const pm_font *font, const char *utf8, float size_px,
                          float *w, float *h);

/* -------------------------------------------------------------- document */

typedef enum { PM_OP_CROP, PM_OP_STROKE, PM_OP_TEXT } pm_op_kind;

typedef struct {
    pm_op_kind kind;
    pm_color   color; /* stroke colour; unused for crop and text */
    union {
        pm_rect crop;
        struct { pm_point *pts; size_t n; float width; } stroke;
        struct { char *utf8; float x, y; pm_text_style style; const pm_font *font; } text;
    } u;
} pm_op;

/* Applies one op to an image in place. A crop replaces img's buffer.
 * Exposed so frontends can render not-yet-committed previews with exactly
 * the code path that commits will use. */
pm_status pm_op_apply(pm_image *img, const pm_op *op);
/* Conservative pixel bounds the op can touch (the crop rect for a crop). */
pm_rect   pm_op_bounds(const pm_op *op);

typedef struct {
    size_t   at;  /* state after ops[0 .. at) */
    pm_image img;
} pm_checkpoint;

/*
 * A document is an immutable base image plus an ordered op log.
 *   ops[0 .. n_applied)      are reflected in `canvas`
 *   ops[n_applied .. n_ops)  are the redo tail
 * Op coordinates are relative to the canvas as it was when the op was
 * pushed (i.e. after all earlier crops).
 *
 * Undo replays from the nearest checkpoint (a canvas snapshot taken every
 * PM_CHECKPOINT_EVERY ops while the snapshots fit in cp_budget bytes;
 * the oldest is evicted first), falling back to the base image.
 */
#define PM_CHECKPOINT_EVERY 8
#define PM_CHECKPOINT_BUDGET ((size_t)256 << 20)

typedef struct {
    pm_image base;
    pm_meta  meta;      /* from the base file; written back out on save */
    char    *base_path; /* NULL when built from an in-memory image      */
    uint64_t base_hash; /* FNV-1a 64 of the base file's bytes           */
    pm_image canvas;
    pm_op   *ops;
    size_t   n_applied, n_ops, cap;
    pm_checkpoint *cps;
    size_t   n_cps, cps_cap, cp_budget;
    pm_font **fonts;    /* fonts owned by the document (pm_doc_own_font) */
    size_t   n_fonts;
} pm_doc;

pm_status pm_doc_open(pm_doc *doc, const char *path);
/* Takes ownership of *base (which is zeroed). */
pm_status pm_doc_from_image(pm_doc *doc, pm_image *base);
void      pm_doc_close(pm_doc *doc);
/* Transfers ownership of font to the document (freed by pm_doc_close).
 * On failure the font is freed. */
pm_status pm_doc_own_font(pm_doc *doc, pm_font *font);

/* Each push deep-copies its inputs, discards the redo tail, and updates
 * the canvas. Fonts are borrowed and must outlive the document (or be
 * handed to it with pm_doc_own_font). */
pm_status pm_doc_crop(pm_doc *doc, pm_rect r);
pm_status pm_doc_stroke(pm_doc *doc, const pm_point *pts, size_t n,
                        float width, pm_color color);
pm_status pm_doc_text(pm_doc *doc, const pm_font *font, const char *utf8,
                      float x, float y, const pm_text_style *style);

bool pm_doc_can_undo(const pm_doc *doc);
bool pm_doc_can_redo(const pm_doc *doc);
/* PM_ERR_ARG if there is nothing to undo / redo. */
pm_status pm_doc_undo(pm_doc *doc);
pm_status pm_doc_redo(pm_doc *doc);

/* Saves the canvas with the document's metadata (ICC profile). */
pm_status pm_doc_export(const pm_doc *doc, const char *path, int jpeg_quality,
                        bool overwrite);

/* --------------------------------------------------------------- session */

/* A session file (.pmark) records the base image path and hash plus the
 * applied ops, so an edit can be reopened and continued. Format: see
 * docs/ARCHITECTURE.md. Requires doc->base_path. Written atomically. */
pm_status pm_session_save(const pm_doc *doc, const char *path, bool overwrite);
#define PM_SESSION_LENIENT 1u /* open even if the base file changed or a font
                                * had to be replaced by the default font */
pm_status pm_session_load(pm_doc *doc, const char *path, unsigned flags);
/* True if the file starts with the session magic line. */
bool      pm_is_session_file(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* PMARK_H */
