#include "pm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- ops */

pm_status pm_op_apply(pm_image *img, const pm_op *op)
{
    if (!img || !img->px || !op) return PM_ERR_ARG;
    switch (op->kind) {
    case PM_OP_CROP: {
        pm_image out;
        pm_status st = pm_image_crop(&out, img, op->u.crop);
        if (st != PM_OK) return st;
        pm_image_free(img);
        *img = out;
        return PM_OK;
    }
    case PM_OP_STROKE:
        return pm_draw_stroke(img, op->u.stroke.pts, op->u.stroke.n,
                              op->u.stroke.width, op->color);
    case PM_OP_TEXT:
        return pm_text_draw(img, op->u.text.font, op->u.text.utf8,
                            op->u.text.x, op->u.text.y, op->u.text.size_px,
                            op->color);
    }
    return PM_ERR_ARG;
}

pm_rect pm_op_bounds(const pm_op *op)
{
    if (!op) return (pm_rect){0, 0, 0, 0};
    switch (op->kind) {
    case PM_OP_CROP:
        return op->u.crop;
    case PM_OP_STROKE:
        return pm_stroke_bounds(op->u.stroke.pts, op->u.stroke.n,
                                op->u.stroke.width);
    case PM_OP_TEXT: {
        float w, h;
        pm_text_measure(op->u.text.font, op->u.text.utf8, op->u.text.size_px,
                        &w, &h);
        /* glyphs may overhang their advance box (italics, accents) */
        int pad = (int)ceilf(op->u.text.size_px * 0.5f) + 2;
        return (pm_rect){(int)floorf(op->u.text.x) - pad,
                         (int)floorf(op->u.text.y) - pad,
                         (int)ceilf(w) + 2 * pad + 1,
                         (int)ceilf(h) + 2 * pad + 1};
    }
    }
    return (pm_rect){0, 0, 0, 0};
}

static void op_free(pm_op *op)
{
    if (op->kind == PM_OP_STROKE) free(op->u.stroke.pts);
    if (op->kind == PM_OP_TEXT) free(op->u.text.utf8);
}

/* -------------------------------------------------------------- document */

pm_status pm_doc_from_image(pm_doc *doc, pm_image *base)
{
    if (!doc || !base || !base->px) return PM_ERR_ARG;
    memset(doc, 0, sizeof *doc);
    pm_status st = pm_image_copy(&doc->canvas, base);
    if (st != PM_OK) return st;
    doc->base = *base;
    *base = (pm_image){0, 0, NULL};
    return PM_OK;
}

pm_status pm_doc_open(pm_doc *doc, const char *path)
{
    if (!doc) return PM_ERR_ARG;
    pm_image img;
    pm_status st = pm_image_load(&img, path);
    if (st != PM_OK) return st;
    st = pm_doc_from_image(doc, &img);
    if (st != PM_OK) pm_image_free(&img);
    return st;
}

void pm_doc_close(pm_doc *doc)
{
    if (!doc) return;
    for (size_t i = 0; i < doc->n_ops; i++) op_free(&doc->ops[i]);
    free(doc->ops);
    pm_image_free(&doc->base);
    pm_image_free(&doc->canvas);
    memset(doc, 0, sizeof *doc);
}

/* Takes ownership of *op in every case (frees it on failure). The op is
 * applied before it is recorded, so a failed push leaves the log untouched;
 * stroke and text failures are detected before any pixel is written. */
static pm_status push(pm_doc *doc, pm_op *op)
{
    pm_status st = PM_OK;
    if (doc->n_applied == doc->cap) {
        size_t cap = doc->cap ? doc->cap * 2 : 16;
        pm_op *ops = realloc(doc->ops, cap * sizeof *ops);
        if (!ops) st = PM_ERR_NOMEM;
        else { doc->ops = ops; doc->cap = cap; }
    }
    if (st == PM_OK) st = pm_op_apply(&doc->canvas, op);
    if (st != PM_OK) { op_free(op); return st; }

    for (size_t i = doc->n_applied; i < doc->n_ops; i++) op_free(&doc->ops[i]);
    doc->ops[doc->n_applied++] = *op;
    doc->n_ops = doc->n_applied;
    return PM_OK;
}

pm_status pm_doc_crop(pm_doc *doc, pm_rect r)
{
    if (!doc || !doc->canvas.px) return PM_ERR_ARG;
    /* store the clipped rect so replay is independent of later edits */
    r = pm_rect_intersect(r, (pm_rect){0, 0, doc->canvas.w, doc->canvas.h});
    if (pm_rect_empty(r)) return PM_ERR_ARG;
    pm_op op = {.kind = PM_OP_CROP};
    op.u.crop = r;
    return push(doc, &op);
}

pm_status pm_doc_stroke(pm_doc *doc, const pm_point *pts, size_t n,
                        float width, pm_color color)
{
    if (!doc || !doc->canvas.px || !pts || n == 0) return PM_ERR_ARG;
    pm_op op = {.kind = PM_OP_STROKE, .color = color};
    op.u.stroke.pts = malloc(n * sizeof *pts);
    if (!op.u.stroke.pts) return PM_ERR_NOMEM;
    memcpy(op.u.stroke.pts, pts, n * sizeof *pts);
    op.u.stroke.n = n;
    op.u.stroke.width = width;
    return push(doc, &op);
}

pm_status pm_doc_text(pm_doc *doc, const pm_font *font, const char *utf8,
                      float x, float y, float size_px, pm_color color)
{
    if (!doc || !doc->canvas.px || !utf8) return PM_ERR_ARG;
    if (!font) return PM_ERR_FONT;
    pm_op op = {.kind = PM_OP_TEXT, .color = color};
    size_t len = strlen(utf8) + 1;
    op.u.text.utf8 = malloc(len);
    if (!op.u.text.utf8) return PM_ERR_NOMEM;
    memcpy(op.u.text.utf8, utf8, len);
    op.u.text.x = x;
    op.u.text.y = y;
    op.u.text.size_px = size_px;
    op.u.text.font = font;
    return push(doc, &op);
}

bool pm_doc_can_undo(const pm_doc *doc) { return doc && doc->n_applied > 0; }
bool pm_doc_can_redo(const pm_doc *doc) { return doc && doc->n_applied < doc->n_ops; }

/* Rebuilds the canvas from the base by replaying ops[0 .. n). The old canvas
 * is only replaced once the replay has fully succeeded. */
static pm_status replay(pm_doc *doc, size_t n)
{
    pm_image img = {0, 0, NULL};
    pm_status st = pm_image_copy(&img, &doc->base);
    for (size_t i = 0; st == PM_OK && i < n; i++)
        st = pm_op_apply(&img, &doc->ops[i]);
    if (st != PM_OK) { pm_image_free(&img); return st; }
    pm_image_free(&doc->canvas);
    doc->canvas = img;
    return PM_OK;
}

pm_status pm_doc_undo(pm_doc *doc)
{
    if (!pm_doc_can_undo(doc)) return PM_ERR_ARG;
    pm_status st = replay(doc, doc->n_applied - 1);
    if (st == PM_OK) doc->n_applied--;
    return st;
}

pm_status pm_doc_redo(pm_doc *doc)
{
    if (!pm_doc_can_redo(doc)) return PM_ERR_ARG;
    pm_status st = pm_op_apply(&doc->canvas, &doc->ops[doc->n_applied]);
    if (st == PM_OK) doc->n_applied++;
    return st;
}
