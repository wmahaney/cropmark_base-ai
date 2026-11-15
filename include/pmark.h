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
    PM_ERR_FONT    /* no usable font                      */
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

#define PM_MAX_DIM 32768

pm_status pm_image_create(pm_image *out, int w, int h, pm_color fill);
pm_status pm_image_copy(pm_image *out, const pm_image *src);
/* Decodes JPEG/PNG (and whatever else stb_image handles). JPEG EXIF
 * orientation is applied, so the result is always upright. */
pm_status pm_image_load(pm_image *out, const char *path);
/* Format chosen by extension: .png, .jpg/.jpeg (alpha flattened on white).
 * jpeg_quality in 1..100; <= 0 selects the default (92). */
pm_status pm_image_save(const pm_image *img, const char *path, int jpeg_quality);
/* r is clipped to the image; an empty result is PM_ERR_ARG. */
pm_status pm_image_crop(pm_image *out, const pm_image *src, pm_rect r);
void      pm_image_free(pm_image *img);

/* --------------------------------------------------------------- drawing */

/* Anti-aliased polyline with round caps and joins. The whole stroke is
 * composited once, so a translucent stroke that crosses itself does not
 * darken at the overlap (highlighter behaviour). n == 1 draws a dot. */
pm_status pm_draw_stroke(pm_image *img, const pm_point *pts, size_t n,
                         float width, pm_color color);
pm_rect   pm_stroke_bounds(const pm_point *pts, size_t n, float width);

/* ------------------------------------------------------------------ text */

typedef struct pm_font pm_font;

pm_status pm_font_load(pm_font **out, const char *ttf_path);
/* Tries $PMARK_FONT, then a list of common system font locations. */
pm_status pm_font_load_default(pm_font **out);
void      pm_font_free(pm_font *font);

/* UTF-8, '\n' starts a new line. (x, y) is the top-left of the text box;
 * size_px is the font's pixel height (ascent to descent). */
pm_status pm_text_draw(pm_image *img, const pm_font *font, const char *utf8,
                       float x, float y, float size_px, pm_color color);
void      pm_text_measure(const pm_font *font, const char *utf8, float size_px,
                          float *w, float *h);

/* -------------------------------------------------------------- document */

typedef enum { PM_OP_CROP, PM_OP_STROKE, PM_OP_TEXT } pm_op_kind;

typedef struct {
    pm_op_kind kind;
    pm_color   color; /* unused for crop */
    union {
        pm_rect crop;
        struct { pm_point *pts; size_t n; float width; } stroke;
        struct { char *utf8; float x, y, size_px; const pm_font *font; } text;
    } u;
} pm_op;

/* Applies one op to an image in place. A crop replaces img's buffer.
 * Exposed so frontends can render not-yet-committed previews with exactly
 * the code path that commits will use. */
pm_status pm_op_apply(pm_image *img, const pm_op *op);
/* Conservative pixel bounds the op can touch (the crop rect for a crop). */
pm_rect   pm_op_bounds(const pm_op *op);

/*
 * A document is an immutable base image plus an ordered op log.
 *   ops[0 .. n_applied)      are reflected in `canvas`
 *   ops[n_applied .. n_ops)  are the redo tail
 * Op coordinates are relative to the canvas as it was when the op was
 * pushed (i.e. after all earlier crops).
 */
typedef struct {
    pm_image base;
    pm_image canvas;
    pm_op   *ops;
    size_t   n_applied, n_ops, cap;
} pm_doc;

pm_status pm_doc_open(pm_doc *doc, const char *path);
/* Takes ownership of *base (which is zeroed). */
pm_status pm_doc_from_image(pm_doc *doc, pm_image *base);
void      pm_doc_close(pm_doc *doc);

/* Each push deep-copies its inputs, discards the redo tail, and updates
 * the canvas. Fonts are borrowed and must outlive the document. */
pm_status pm_doc_crop(pm_doc *doc, pm_rect r);
pm_status pm_doc_stroke(pm_doc *doc, const pm_point *pts, size_t n,
                        float width, pm_color color);
pm_status pm_doc_text(pm_doc *doc, const pm_font *font, const char *utf8,
                      float x, float y, float size_px, pm_color color);

bool pm_doc_can_undo(const pm_doc *doc);
bool pm_doc_can_redo(const pm_doc *doc);
/* PM_ERR_ARG if there is nothing to undo / redo. */
pm_status pm_doc_undo(pm_doc *doc);
pm_status pm_doc_redo(pm_doc *doc);

#ifdef __cplusplus
}
#endif
#endif /* PMARK_H */
