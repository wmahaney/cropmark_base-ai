/* Core unit tests: no framework, exit status = number of failures (capped). */
#define _POSIX_C_SOURCE 200809L
#include "pm_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

static int fails, checks;
#define CHECK(c) \
    do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static const pm_color WHITE = {255, 255, 255, 255}, RED = {255, 0, 0, 255};
static char tmpdir[64];

static const char *tmp(const char *name)
{
    static char buf[4][256];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 256, "%s/%s", tmpdir, name);
    return b;
}

static const uint8_t *at(const pm_image *im, int x, int y)
{
    return im->px + ((size_t)y * im->w + x) * 4;
}
static bool is(const pm_image *im, int x, int y, pm_color c)
{
    return memcmp(at(im, x, y), &c, 4) == 0;
}
static bool same(const pm_image *a, const pm_image *b)
{
    return a->w == b->w && a->h == b->h &&
           memcmp(a->px, b->px, (size_t)a->w * a->h * 4) == 0;
}
static pm_image photo(int w, int h) /* deterministic non-flat test image */
{
    pm_image im;
    pm_image_create(&im, w, h, WHITE);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = im.px + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(x * 7); p[1] = (uint8_t)(y * 5); p[2] = (uint8_t)(x ^ y);
        }
    return im;
}

/* ------------------------------------------------------------------ basics */

static void test_basics(void)
{
    pm_color c;
    CHECK(pm_color_parse("#1e88e5", &c) && c.r == 0x1e && c.g == 0x88 && c.b == 0xe5 && c.a == 255);
    CHECK(pm_color_parse("FFEB3B66", &c) && c.a == 0x66);
    CHECK(!pm_color_parse("12345", &c) && !pm_color_parse("gg0000", &c));

    pm_rect r = pm_rect_intersect((pm_rect){0, 0, 10, 10}, (pm_rect){5, -5, 10, 10});
    CHECK(r.x == 5 && r.y == 0 && r.w == 5 && r.h == 5);
    CHECK(pm_rect_empty(pm_rect_intersect((pm_rect){0, 0, 2, 2}, (pm_rect){2, 0, 2, 2})));
    r = pm_rect_union((pm_rect){0, 0, 0, 0}, (pm_rect){3, 4, 5, 6});
    CHECK(r.x == 3 && r.y == 4 && r.w == 5 && r.h == 6);

    pm_image im;
    CHECK(pm_image_create(&im, 0, 5, WHITE) == PM_ERR_ARG);
    CHECK(pm_image_create(&im, 20000, 20000, WHITE) == PM_ERR_TOO_BIG); /* 400 MP */
    CHECK(pm_image_create(&im, 8, 6, WHITE) == PM_OK);
    memcpy(im.px + (3 * 8 + 5) * 4, &RED, 4); /* (5,3) */
    pm_image cr;
    CHECK(pm_image_crop(&cr, &im, (pm_rect){4, 2, 100, 100}) == PM_OK); /* clipped */
    CHECK(cr.w == 4 && cr.h == 4 && is(&cr, 1, 1, RED) && is(&cr, 0, 0, WHITE));
    pm_image_free(&cr);
    CHECK(pm_image_crop(&cr, &im, (pm_rect){8, 0, 4, 4}) == PM_ERR_ARG);
    pm_image_free(&im);
    CHECK(pm_format_from_path("a.JPEG") == PM_FMT_JPEG && pm_format_from_path("a.png") == PM_FMT_PNG &&
          pm_format_from_path("a.bmp") == PM_FMT_UNKNOWN);
}

/* ----------------------------------------------------------------- strokes */

static void test_stroke(void)
{
    pm_image im;
    pm_image_create(&im, 100, 60, WHITE);
    pm_point line[] = {{10, 30.5f}, {90, 30.5f}};
    CHECK(pm_draw_stroke(&im, line, 2, 10, RED) == PM_OK);
    CHECK(is(&im, 50, 30, RED) && is(&im, 50, 26, RED) && is(&im, 50, 40, WHITE));
    CHECK(is(&im, 7, 30, RED) && is(&im, 2, 30, WHITE)); /* round cap */
    const uint8_t *edge = at(&im, 50, 25); /* centre exactly r from the line */
    CHECK(edge[1] > 100 && edge[1] < 155);

    /* translucent stroke that doubles back: overlap must not darken */
    pm_image a, b;
    pm_image_create(&a, 100, 60, WHITE);
    pm_image_create(&b, 100, 60, WHITE);
    pm_color hl = {0, 0, 255, 128};
    pm_point back[] = {{10, 30.5f}, {90, 30.5f}, {10, 30.5f}};
    pm_draw_stroke(&a, line, 2, 10, hl);
    pm_draw_stroke(&b, back, 3, 10, hl);
    CHECK(same(&a, &b));

    pm_point off[] = {{-500, -500}, {-400, -400}};
    CHECK(pm_draw_stroke(&a, off, 2, 10, RED) == PM_OK);
    pm_point nan_pt[] = {{0.0f / 0.0f, 1}};
    CHECK(pm_draw_stroke(&a, nan_pt, 1, 10, RED) == PM_ERR_ARG);
    CHECK(pm_draw_stroke(&a, line, 2, 1.0e9f, RED) == PM_ERR_ARG); /* absurd width */
    pm_point dot[] = {{50, 10}};
    CHECK(pm_draw_stroke(&a, dot, 1, 6, RED) == PM_OK && is(&a, 50, 10, RED));

    pm_image t;
    pm_image_create(&t, 20, 20, (pm_color){0, 0, 0, 0});
    pm_draw_stroke(&t, (pm_point[]){{10, 10}}, 1, 8, hl);
    CHECK(at(&t, 10, 10)[2] == 255 && at(&t, 10, 10)[3] == 128);

    pm_image_free(&t); pm_image_free(&a); pm_image_free(&b); pm_image_free(&im);
}

/* The GUI previews with the incremental builder and commits with
 * pm_draw_stroke; the two must produce identical pixels. */
static void test_stroke_builder(void)
{
    pm_image base = photo(160, 120), whole, incr;
    pm_image_copy(&whole, &base);
    pm_image_copy(&incr, &base);
    pm_point pts[40];
    for (int i = 0; i < 40; i++) /* a wiggly path that leaves the canvas */
        pts[i] = (pm_point){-10.0f + (float)i * 4.7f, 60.0f + 50.0f * sinf((float)i * 0.4f)};
    pm_color c = {30, 200, 90, 150};
    CHECK(pm_draw_stroke(&whole, pts, 40, 9.5f, c) == PM_OK);

    pm_stroke *s;
    pm_rect canvas = {0, 0, 160, 120};
    CHECK(pm_stroke_begin(&s, canvas, 9.5f) == PM_OK);
    pm_rect dirty = {0, 0, 0, 0};
    for (int i = 0; i < 40; i++) dirty = pm_rect_union(dirty, pm_stroke_add(s, pts[i]));
    CHECK(!pm_rect_empty(dirty));
    pm_stroke_composite(s, &incr, 0, 0, dirty, c);
    CHECK(same(&whole, &incr));

    /* compositing a sub-rect into an offset scratch image (the GUI's path) */
    pm_rect sub = {40, 30, 50, 40};
    pm_image scratch;
    pm_image_crop(&scratch, &base, sub);
    pm_stroke_composite(s, &scratch, sub.x, sub.y, sub, c);
    bool ok = true;
    for (int y = 0; y < sub.h; y++)
        ok = ok && !memcmp(at(&scratch, 0, y), at(&whole, sub.x, sub.y + y), (size_t)sub.w * 4);
    CHECK(ok);
    CHECK(pm_rect_empty(pm_stroke_add(s, (pm_point){1.0f / 0.0f, 0})));
    pm_stroke_end(s);
    CHECK(pm_stroke_begin(&s, (pm_rect){0, 0, 0, 5}, 4) == PM_ERR_ARG);
    pm_image_free(&scratch); pm_image_free(&base); pm_image_free(&whole); pm_image_free(&incr);
}

static void test_shapes(void)
{
    size_t n;
    pm_point *p = pm_shape_arrow((pm_point){0, 0}, (pm_point){100, 0}, 4, &n);
    CHECK(p && n == 5 && p[1].x == 100 && p[3].x == 100);
    CHECK(p[2].x < 100 && p[2].x > 60 && p[2].y != 0 && fabsf(p[2].y + p[4].y) < 1e-4f); /* symmetric barbs behind the tip */
    free(p);
    p = pm_shape_arrow((pm_point){5, 5}, (pm_point){5, 5}, 4, &n);
    CHECK(p && n == 1);
    free(p);
    p = pm_shape_rect((pm_rect){10, 20, 30, 40}, &n);
    CHECK(p && n == 5 && p[2].x == 40 && p[2].y == 60 && p[4].x == p[0].x && p[4].y == p[0].y);
    free(p);
    p = pm_shape_ellipse((pm_rect){0, 0, 200, 100}, &n);
    CHECK(p && n > 24 && p[0].x == p[n - 1].x && p[0].y == p[n - 1].y);
    bool on = true;
    for (size_t i = 0; i < n; i++) {
        float dx = (p[i].x - 100) / 100, dy = (p[i].y - 50) / 50;
        on = on && fabsf(dx * dx + dy * dy - 1.0f) < 1e-3f;
    }
    CHECK(on);
    free(p);
}

/* ---------------------------------------------------------------- document */

static void test_doc(void)
{
    pm_image base;
    pm_image_create(&base, 64, 48, WHITE);
    pm_doc d;
    CHECK(pm_doc_from_image(&d, &base) == PM_OK && base.px == NULL);
    CHECK(!pm_doc_can_undo(&d) && pm_doc_undo(&d) == PM_ERR_ARG);

    pm_point p[] = {{8, 8}, {40, 8}};
    CHECK(pm_doc_stroke(&d, p, 2, 4, RED) == PM_OK);
    CHECK(pm_doc_crop(&d, (pm_rect){4, 4, 20, 10}) == PM_OK);
    CHECK(d.canvas.w == 20 && d.canvas.h == 10 && is(&d.canvas, 10, 4, RED));
    CHECK(pm_doc_crop(&d, (pm_rect){50, 50, 5, 5}) == PM_ERR_ARG && d.n_ops == 2);

    CHECK(pm_doc_undo(&d) == PM_OK && d.canvas.w == 64 && is(&d.canvas, 20, 8, RED));
    CHECK(pm_doc_undo(&d) == PM_OK && is(&d.canvas, 20, 8, WHITE));
    CHECK(pm_doc_redo(&d) == PM_OK && pm_doc_redo(&d) == PM_OK && d.canvas.w == 20);
    CHECK(pm_doc_redo(&d) == PM_ERR_ARG);

    pm_doc_undo(&d);
    CHECK(pm_doc_stroke(&d, p, 1, 4, RED) == PM_OK);
    CHECK(d.n_ops == 2 && !pm_doc_can_redo(&d) && d.canvas.w == 64);
    pm_doc_close(&d);
}

/* Replays the first k ops from scratch: the reference for checkpointed undo. */
static pm_image reference(const pm_doc *d, size_t k)
{
    pm_image im;
    pm_image_copy(&im, &d->base);
    for (size_t i = 0; i < k; i++) pm_op_apply(&im, &d->ops[i]);
    return im;
}

static void test_checkpoints(void)
{
    pm_image base = photo(90, 70);
    pm_doc d;
    pm_doc_from_image(&d, &base);
    d.cp_budget = 3 * 90 * 70 * 4; /* room for three snapshots: forces eviction */
    for (int i = 0; i < 60; i++) {
        float y = (float)(i % 50) + 5.0f;
        pm_point seg[] = {{5, y}, {80, y + 3}};
        pm_doc_stroke(&d, seg, 2, 3, (pm_color){(uint8_t)(i * 4), 0, 255, 120});
        if (i == 30) pm_doc_crop(&d, (pm_rect){2, 2, 85, 66}); /* sizes differ across cps */
    }
    CHECK(d.n_cps > 0 && d.n_cps <= 3);
    size_t used = 0;
    for (size_t i = 0; i < d.n_cps; i++) used += (size_t)d.cps[i].img.w * d.cps[i].img.h * 4;
    CHECK(used <= d.cp_budget);

    bool ok = true;
    while (pm_doc_can_undo(&d)) { /* every intermediate state must be exact */
        pm_doc_undo(&d);
        pm_image ref = reference(&d, d.n_applied);
        ok = ok && same(&ref, &d.canvas);
        pm_image_free(&ref);
    }
    CHECK(ok && d.n_applied == 0);
    for (int i = 0; i < 20; i++) pm_doc_redo(&d);
    pm_point dot[] = {{40, 40}};
    pm_doc_stroke(&d, dot, 1, 9, RED); /* truncates the redo tail */
    bool tail_ok = true;
    for (size_t i = 0; i < d.n_cps; i++) tail_ok = tail_ok && d.cps[i].at <= d.n_applied;
    CHECK(tail_ok);
    pm_doc_undo(&d);
    pm_image ref = reference(&d, d.n_applied);
    CHECK(same(&ref, &d.canvas));
    pm_image_free(&ref);
    pm_doc_close(&d);
}

/* --------------------------------------------------------------------- I/O */

static bool dir_has_temp(void)
{
    DIR *dir = opendir(tmpdir);
    bool found = false;
    for (struct dirent *e; dir && (e = readdir(dir));)
        if (strstr(e->d_name, ".pmtmp-")) found = true;
    if (dir) closedir(dir);
    return found;
}

static void test_io(void)
{
    pm_image im, back;
    pm_image_create(&im, 31, 17, (pm_color){10, 200, 30, 255});
    pm_draw_stroke(&im, (pm_point[]){{3, 3}, {25, 12}}, 2, 3, (pm_color){0, 0, 255, 90});

    CHECK(pm_image_save(&im, tmp("t.png"), 0) == PM_OK);
    CHECK(pm_image_load(&back, tmp("t.png")) == PM_OK);
    CHECK(same(&im, &back));
    pm_image_free(&back);

    CHECK(pm_image_save(&im, tmp("t.JPG"), 90) == PM_OK);
    CHECK(pm_image_load(&back, tmp("t.JPG")) == PM_OK);
    CHECK(back.w == 31 && back.h == 17 && abs(at(&back, 28, 2)[1] - 200) < 12);
    pm_image_free(&back);

    CHECK(pm_image_save(&im, tmp("t.bmp"), 0) == PM_ERR_FORMAT);
    CHECK(pm_image_load(&back, "/nonexistent/x.png") == PM_ERR_IO);

    /* opaque images are written as RGB PNGs (colour type 2), others RGBA (6) */
    uint8_t *png; size_t len;
    CHECK(pm_image_encode(&im, PM_FMT_PNG, NULL, &png, &len) == PM_OK && png[25] == 2);
    free(png);
    im.px[3] = 200;
    CHECK(pm_image_encode(&im, PM_FMT_PNG, NULL, &png, &len) == PM_OK && png[25] == 6);
    CHECK(pm_image_decode(&back, NULL, png, len) == PM_OK && same(&im, &back));
    pm_image_free(&back);
    free(png);
    pm_image_free(&im);
}

static void test_atomic(void)
{
    char p[256];
    snprintf(p, sizeof p, "%s", tmp("atomic.png"));
    pm_image a, b, back;
    pm_image_create(&a, 10, 10, RED);
    pm_image_create(&b, 12, 12, WHITE);
    remove(p);
    pm_save_opts keep = {0, NULL, false};
    CHECK(pm_image_save_ex(&a, p, &keep) == PM_OK);
    CHECK(pm_image_save_ex(&b, p, &keep) == PM_ERR_EXISTS);
    CHECK(pm_write_file_atomic(p, (const uint8_t *)"x", 1, false) == PM_ERR_EXISTS);
    CHECK(pm_image_load(&back, p) == PM_OK && same(&a, &back)); /* untouched */
    pm_image_free(&back);
    CHECK(pm_image_save_ex(&b, p, NULL) == PM_OK);              /* overwrite */
    CHECK(pm_image_load(&back, p) == PM_OK && same(&b, &back));
    pm_image_free(&back);
    CHECK(pm_image_save(&a, tmp("no/such/dir.png"), 0) == PM_ERR_IO);
    CHECK(!dir_has_temp()); /* failed or not, nothing is left behind */

    CHECK(link(p, tmp("hardlink.png")) == 0);
    CHECK(pm_path_same_file(p, tmp("hardlink.png")));
    CHECK(!pm_path_same_file(p, tmp("t.png")) && !pm_path_same_file(p, tmp("missing")));
    pm_image_free(&a);
    pm_image_free(&b);
}

/* Builds a PNG whose header claims w x h (no pixel data needed). */
static size_t fake_png(uint8_t *out, uint32_t w, uint32_t h)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[17] = {'I', 'H', 'D', 'R',
                        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                        8, 2, 0, 0, 0};
    uint32_t crc = pm_crc32(0, ihdr, 17);
    memcpy(out, sig, 8);
    memcpy(out + 8, "\0\0\0\x0d", 4);
    memcpy(out + 12, ihdr, 17);
    uint8_t c[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    memcpy(out + 29, c, 4);
    memcpy(out + 33, "\0\0\0\0IEND\xae\x42\x60\x82", 12);
    return 45;
}

static void test_limits(void)
{
    uint8_t buf[64];
    pm_image im;
    /* rejected from the header alone: these would need 1.6-6.4 GB */
    CHECK(pm_image_decode(&im, NULL, buf, fake_png(buf, 40000, 10)) == PM_ERR_TOO_BIG);
    CHECK(pm_image_decode(&im, NULL, buf, fake_png(buf, 16000, 9000)) == PM_ERR_TOO_BIG); /* 144 MP */
    CHECK(pm_image_decode(&im, NULL, buf, fake_png(buf, 20000, 20000)) == PM_ERR_TOO_BIG);
    CHECK(pm_image_decode(&im, NULL, buf, fake_png(buf, 4, 4)) == PM_ERR_FORMAT); /* no IDAT */
    CHECK(pm_image_decode(&im, NULL, (const uint8_t *)"junk", 4) == PM_ERR_FORMAT);
}

static void test_icc(void)
{
    pm_image im = photo(40, 30), back;
    pm_meta m = {NULL, 70000}; /* > one JPEG APP2 segment: exercises chunking */
    m.icc = malloc(m.icc_len);
    for (size_t i = 0; i < m.icc_len; i++) m.icc[i] = (uint8_t)(i * 31 + (i >> 9));
    pm_save_opts o = {85, &m, true};

    const char *files[] = {tmp("icc.png"), tmp("icc.jpg")};
    for (int k = 0; k < 2; k++) {
        pm_meta got = {NULL, 0};
        CHECK(pm_image_save_ex(&im, files[k], &o) == PM_OK);
        CHECK(pm_image_load_ex(&back, &got, files[k]) == PM_OK);
        CHECK(got.icc_len == m.icc_len && got.icc && !memcmp(got.icc, m.icc, m.icc_len));
        CHECK(back.w == 40 && back.h == 30);
        pm_image_free(&back);
        pm_meta_free(&got);
    }
    pm_meta none = {NULL, 0};
    CHECK(pm_image_save(&im, tmp("plain.jpg"), 85) == PM_OK);
    CHECK(pm_image_load_ex(&back, &none, tmp("plain.jpg")) == PM_OK && none.icc == NULL);
    pm_image_free(&back);
    pm_meta_free(&m);
    pm_image_free(&im);
}

static void test_exif(void)
{
    const uint8_t jpg[] = {
        0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
        'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x01,
        0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0xFF, 0xD9};
    CHECK(pm_exif_orientation(jpg, sizeof jpg) == 6);
    CHECK(pm_exif_orientation(jpg, 20) == 1);
    CHECK(pm_exif_orientation((const uint8_t *)"\x89PNG", 4) == 1);

    pm_image im;
    pm_image_create(&im, 2, 1, WHITE);
    memcpy(im.px, &RED, 4);
    CHECK(pm_orient_apply(&im, 6) == PM_OK);
    CHECK(im.w == 1 && im.h == 2 && is(&im, 0, 0, RED) && is(&im, 0, 1, WHITE));
    CHECK(pm_orient_apply(&im, 3) == PM_OK && is(&im, 0, 1, RED));
    pm_image_free(&im);
}

/* -------------------------------------------------------------------- text */

static int ink_outside(const pm_image *im, pm_rect b)
{
    int out = 0;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
            if (!is(im, x, y, WHITE) && !(x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h))
                out++;
    return out;
}
static int ink(const pm_image *im)
{
    int n = 0;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++) n += !is(im, x, y, WHITE);
    return n;
}

static void test_text(void)
{
    pm_font *f;
    if (pm_font_load_default(&f) != PM_OK) {
        fprintf(stderr, "note: no system font found, text tests skipped\n");
        return;
    }
    CHECK(pm_font_path(f) && pm_font_path(f)[0] == '/');
    float w1, h1, w2, h2;
    pm_text_measure(f, "Hello", 32, &w1, &h1);
    pm_text_measure(f, "Hello\nHello, world", 32, &w2, &h2);
    CHECK(w1 > 40 && w1 < 160 && h1 >= 32 && w2 > w1 && h2 > 1.9f * h1);

    pm_image im, ol;
    pm_image_create(&im, 300, 120, WHITE);
    pm_image_create(&ol, 300, 120, WHITE);
    pm_op op = {.kind = PM_OP_TEXT};
    op.u.text.utf8 = "H\xC3\xA9llo";
    op.u.text.x = 40.5f; op.u.text.y = 30; op.u.text.font = f;
    op.u.text.style = (pm_text_style){32, RED, 0, {0, 0, 0, 255}};
    CHECK(pm_op_apply(&im, &op) == PM_OK);
    CHECK(ink(&im) > 200 && ink_outside(&im, pm_op_bounds(&op)) == 0);
    op.u.text.style.outline_px = 4;
    CHECK(pm_op_apply(&ol, &op) == PM_OK);
    CHECK(ink(&ol) > ink(&im) * 2 && ink_outside(&ol, pm_op_bounds(&op)) == 0);

    /* translucent text is composited once: overlapping glyph boxes ("fi",
     * kerning pairs) cannot exceed the colour's own alpha */
    pm_image tr;
    pm_image_create(&tr, 200, 80, WHITE);
    pm_text_style half = {48, {0, 0, 0, 128}, 0, {0, 0, 0, 0}};
    CHECK(pm_text_draw(&tr, f, "AVAWfij", 5, 5, &half) == PM_OK);
    int darkest = 255;
    for (int i = 0; i < 200 * 80; i++) if (tr.px[4 * i] < darkest) darkest = tr.px[4 * i];
    CHECK(darkest >= 126); /* 255 * (1 - 128/255) = 127, +-1 rounding */

    CHECK(pm_text_draw(&im, f, "clipped", 280, 110, &(pm_text_style){40, RED, 0, RED}) == PM_OK);
    CHECK(pm_text_draw(&im, f, "x", 0, 0, &(pm_text_style){0, RED, 0, RED}) == PM_ERR_ARG);
    CHECK(pm_text_draw(&im, f, "x", 0, 0, &(pm_text_style){20, RED, 999, RED}) == PM_ERR_ARG);
    CHECK(pm_text_draw(&im, NULL, "x", 0, 0, &half) == PM_ERR_FONT);
    pm_image_free(&tr); pm_image_free(&im); pm_image_free(&ol);
    pm_font_free(f);
}

/* ----------------------------------------------------------------- session */

static void test_session(void)
{
    pm_image src = photo(120, 90);
    CHECK(pm_image_save(&src, tmp("base.png"), 0) == PM_OK);
    pm_image_free(&src);

    pm_doc d, e;
    CHECK(pm_doc_open(&d, tmp("base.png")) == PM_OK && d.base_path && d.base_hash);
    pm_font *f = NULL;
    bool have_font = pm_font_load_default(&f) == PM_OK;
    if (have_font) CHECK(pm_doc_own_font(&d, f) == PM_OK);
    pm_point pts[] = {{10.25f, 20.125f}, {100.5f, 60.75f}, {30, 80}};
    pm_doc_stroke(&d, pts, 3, 7.5f, (pm_color){1, 2, 3, 200});
    pm_doc_crop(&d, (pm_rect){5, 5, 100, 70});
    if (have_font)
        pm_doc_text(&d, f, "quote \" back\\slash\nline2\t\x01 caf\xC3\xA9", 8.5f, 9,
                    &(pm_text_style){20, RED, 2, {0, 0, 0, 255}});
    pm_doc_stroke(&d, pts, 1, 3, RED);
    pm_doc_undo(&d); /* the redo tail is not saved */

    CHECK(pm_session_save(&d, tmp("s.pmark"), true) == PM_OK);
    CHECK(pm_session_save(&d, tmp("s.pmark"), false) == PM_ERR_EXISTS);
    CHECK(pm_is_session_file(tmp("s.pmark")) && !pm_is_session_file(tmp("base.png")));
    CHECK(pm_session_load(&e, tmp("s.pmark"), 0) == PM_OK);
    CHECK(same(&d.canvas, &e.canvas) && e.n_ops == d.n_applied && !pm_doc_can_redo(&e));
    CHECK(pm_doc_can_undo(&e)); /* reopened edits are still undoable */
    pm_doc_close(&e);

    /* base stored relative to the session: the folder can move as a unit */
    uint8_t *text; size_t len;
    pm_read_file(tmp("s.pmark"), &text, &len);
    CHECK(len > 20 && strstr((char *)text, "base \"base.png\"") != NULL);
    free(text);

    /* the base changed underneath: refused unless lenient */
    pm_image other = photo(120, 90);
    other.px[0] ^= 1;
    pm_image_save(&other, tmp("base.png"), 0);
    pm_image_free(&other);
    CHECK(pm_session_load(&e, tmp("s.pmark"), 0) == PM_ERR_STALE);
    CHECK(pm_session_load(&e, tmp("s.pmark"), PM_SESSION_LENIENT) == PM_OK);
    pm_doc_close(&e);
    pm_doc_close(&d);

    pm_image im;
    pm_image_create(&im, 4, 4, WHITE);
    CHECK(pm_doc_from_image(&d, &im) == PM_OK);
    CHECK(pm_session_save(&d, tmp("x.pmark"), true) == PM_ERR_ARG); /* no base file */
    pm_doc_close(&d);
}

static void test_session_parse(void)
{
    static const char *const bad[] = {
        "",
        "pmark-session 2\nbase \"a\" 0000000000000000\n",
        "pmark-session 1\n",                                         /* no base */
        "pmark-session 1\nbase a 0000000000000000\n",                /* unquoted */
        "pmark-session 1\nbase \"a\" 00\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\nbase \"b\" 0000000000000000\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\ncrop 1 2 3\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\ncrop 1 2 3 99999999\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\nstroke ff0000ff 4 2 1 1 2\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\nstroke ff0000ff 4 1 nan 1\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\nstroke ff0000 4 1 1 1\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\nstroke ff0000ff 4 99999999999 1 1\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\ntext ff0000ff 20 1 1 0 000000ff \"f\" \"open\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\ntext ff0000ff 20 1 1 0 000000ff \"f\" \"\\x00\"\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\ntext ff0000ff 20 1 1 0 000000ff \"f\" \"\\q\"\n",
        "pmark-session 1\nbase \"a\" 0000000000000000\nfrobnicate\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        pm_session_data d;
        pm_status st = pm_session_parse(bad[i], strlen(bad[i]), &d);
        if (st == PM_OK) { fprintf(stderr, "  accepted bad session #%zu\n", i); pm_session_data_free(&d); }
        CHECK(st == PM_ERR_FORMAT);
    }
    const char *good =
        "# comment\r\n\npmark-session 1\r\n"
        "base \"dir/a b.png\" 00000000000000ff\n"
        "  # indented comment\n"
        "stroke ff0000ff 4 2 1 1 2.5 -3e2\n"
        "text 00ff00ff 20 1 1 0 000000ff \"/f.ttf\" \"a\\\"b\\\\c\\nd\\x7f\"\n";
    pm_session_data d;
    CHECK(pm_session_parse(good, strlen(good), &d) == PM_OK);
    CHECK(d.n == 2 && d.hash == 0xff && !strcmp(d.base, "dir/a b.png"));
    CHECK(d.ops[0].u.stroke.n == 2 && d.ops[0].u.stroke.pts[1].y == -300.0f);
    CHECK(!strcmp(d.ops[1].u.text.utf8, "a\"b\\c\nd\x7f") && !strcmp(d.fonts[1], "/f.ttf"));
    pm_session_data_free(&d);
}

int main(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/pmark_test_XXXXXX");
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }
    test_basics();
    test_stroke();
    test_stroke_builder();
    test_shapes();
    test_doc();
    test_checkpoints();
    test_io();
    test_atomic();
    test_limits();
    test_icc();
    test_exif();
    test_text();
    test_session();
    test_session_parse();
    printf("%d checks, %d failed\n", checks, fails);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) fprintf(stderr, "note: could not remove %s\n", tmpdir);
    return fails > 100 ? 100 : fails;
}
