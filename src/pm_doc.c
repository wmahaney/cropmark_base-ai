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
                            op->u.text.x, op->u.text.y, &op->u.text.style);
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
        return pm_stroke_bounds(op->u.stroke.pts, op->u.stroke.n, op->u.stroke.width);
    case PM_OP_TEXT:
        return pm_text_bounds(op->u.text.font, op->u.text.utf8, op->u.text.x,
                              op->u.text.y, &op->u.text.style);
    }
    return (pm_rect){0, 0, 0, 0};
}

void pm_op_free(pm_op *op)
{
    if (op->kind == PM_OP_STROKE) free(op->u.stroke.pts);
    if (op->kind == PM_OP_TEXT) free(op->u.text.utf8);
    memset(op, 0, sizeof *op);
}

static size_t img_bytes(const pm_image *im) { return (size_t)im->w * (size_t)im->h * 4; }

/* ----------------------------------------------------------- checkpoints */

static void cp_remove(pm_doc *doc, size_t i)
{
    pm_image_free(&doc->cps[i].img);
    memmove(doc->cps + i, doc->cps + i + 1, (doc->n_cps - i - 1) * sizeof *doc->cps);
    doc->n_cps--;
}

/* Checkpoints past n describe a redo tail that no longer exists. */
static void cp_drop_after(pm_doc *doc, size_t n)
{
    while (doc->n_cps && doc->cps[doc->n_cps - 1].at > n) cp_remove(doc, doc->n_cps - 1);
}

/* Snapshots the canvas if PM_CHECKPOINT_EVERY ops have passed since the
 * newest snapshot. Over budget, the oldest snapshot goes first: recent
 * undos are the common case. Failure to snapshot is harmless (slower undo). */
static void cp_maybe_take(pm_doc *doc)
{
    size_t last = doc->n_cps ? doc->cps[doc->n_cps - 1].at : 0;
    if (last >= doc->n_applied || doc->n_applied - last < PM_CHECKPOINT_EVERY) return;
    size_t need = img_bytes(&doc->canvas);
    if (need > doc->cp_budget) return;
    size_t used = 0;
    for (size_t i = 0; i < doc->n_cps; i++) used += img_bytes(&doc->cps[i].img);
    while (doc->n_cps && used + need > doc->cp_budget) {
        used -= img_bytes(&doc->cps[0].img);
        cp_remove(doc, 0);
    }
    if (doc->n_cps == doc->cps_cap) {
        size_t cap = doc->cps_cap ? doc->cps_cap * 2 : 8;
        pm_checkpoint *c = realloc(doc->cps, cap * sizeof *c);
        if (!c) return;
        doc->cps = c;
        doc->cps_cap = cap;
    }
    pm_image snap;
    if (pm_image_copy(&snap, &doc->canvas) != PM_OK) return;
    doc->cps[doc->n_cps++] = (pm_checkpoint){doc->n_applied, snap};
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
    doc->cp_budget = PM_CHECKPOINT_BUDGET;
    return PM_OK;
}

pm_status pm_doc_open(pm_doc *doc, const char *path)
{
    if (!doc || !path) return PM_ERR_ARG;
    uint8_t *data;
    size_t len;
    pm_status st = pm_read_file(path, &data, &len);
    if (st != PM_OK) return st;
    pm_image img;
    pm_meta meta = {NULL, 0};
    st = pm_image_decode(&img, &meta, data, len);
    uint64_t hash = pm_fnv1a64(data, len);
    free(data);
    if (st != PM_OK) return st;
    char *canon = pm_realpath(path);
    if (!canon) canon = pm_strdup(path);
    if (!canon) st = PM_ERR_NOMEM;
    if (st == PM_OK) st = pm_doc_from_image(doc, &img);
    if (st != PM_OK) {
        pm_image_free(&img);
        pm_meta_free(&meta);
        free(canon);
        return st;
    }
    doc->meta = meta;
    doc->base_path = canon;
    doc->base_hash = hash;
    return PM_OK;
}

void pm_doc_close(pm_doc *doc)
{
    if (!doc) return;
    for (size_t i = 0; i < doc->n_ops; i++) pm_op_free(&doc->ops[i]);
    free(doc->ops);
    for (size_t i = 0; i < doc->n_cps; i++) pm_image_free(&doc->cps[i].img);
    free(doc->cps);
    for (size_t i = 0; i < doc->n_fonts; i++) pm_font_free(doc->fonts[i]);
    free(doc->fonts);
    pm_image_free(&doc->base);
    pm_image_free(&doc->canvas);
    pm_meta_free(&doc->meta);
    free(doc->base_path);
    memset(doc, 0, sizeof *doc);
}

pm_status pm_doc_own_font(pm_doc *doc, pm_font *font)
{
    if (!doc || !font) { pm_font_free(font); return PM_ERR_ARG; }
    pm_font **f = realloc(doc->fonts, (doc->n_fonts + 1) * sizeof *f);
    if (!f) { pm_font_free(font); return PM_ERR_NOMEM; }
    doc->fonts = f;
    doc->fonts[doc->n_fonts++] = font;
    return PM_OK;
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
    if (st != PM_OK) { pm_op_free(op); return st; }

    for (size_t i = doc->n_applied; i < doc->n_ops; i++) pm_op_free(&doc->ops[i]);
    cp_drop_after(doc, doc->n_applied);
    doc->ops[doc->n_applied++] = *op;
    doc->n_ops = doc->n_applied;
    cp_maybe_take(doc);
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
                      float x, float y, const pm_text_style *style)
{
    if (!doc || !doc->canvas.px || !utf8 || !style) return PM_ERR_ARG;
    if (!font) return PM_ERR_FONT;
    pm_op op = {.kind = PM_OP_TEXT, .color = style->color};
    op.u.text.utf8 = pm_strdup(utf8);
    if (!op.u.text.utf8) return PM_ERR_NOMEM;
    op.u.text.x = x;
    op.u.text.y = y;
    op.u.text.style = *style;
    op.u.text.font = font;
    return push(doc, &op);
}

bool pm_doc_can_undo(const pm_doc *doc) { return doc && doc->n_applied > 0; }
bool pm_doc_can_redo(const pm_doc *doc) { return doc && doc->n_applied < doc->n_ops; }

/* Rebuilds the canvas for ops[0 .. n) from the nearest checkpoint at or
 * before n (or the base). The old canvas is replaced only on success. */
static pm_status replay(pm_doc *doc, size_t n)
{
    const pm_image *from = &doc->base;
    size_t start = 0;
    for (size_t i = doc->n_cps; i-- > 0;)
        if (doc->cps[i].at <= n) { from = &doc->cps[i].img; start = doc->cps[i].at; break; }

    pm_image img = {0, 0, NULL};
    pm_status st = pm_image_copy(&img, from);
    for (size_t i = start; st == PM_OK && i < n; i++)
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
    if (st == PM_OK) { doc->n_applied++; cp_maybe_take(doc); }
    return st;
}

pm_status pm_doc_export(const pm_doc *doc, const char *path, int jpeg_quality,
                        bool overwrite)
{
    if (!doc || !doc->canvas.px) return PM_ERR_ARG;
    pm_save_opts o = {jpeg_quality, &doc->meta, overwrite};
    return pm_image_save_ex(&doc->canvas, path, &o);
}
