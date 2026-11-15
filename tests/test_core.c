/* Core unit tests: no framework, exit status = number of failures (capped). */
#include "pm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) \
    do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static const pm_color WHITE = {255, 255, 255, 255}, RED = {255, 0, 0, 255};

static const uint8_t *at(const pm_image *im, int x, int y)
{
    return im->px + ((size_t)y * im->w + x) * 4;
}
static bool is(const pm_image *im, int x, int y, pm_color c)
{
    return memcmp(at(im, x, y), &c, 4) == 0;
}

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
    CHECK(pm_image_create(&im, 8, 6, WHITE) == PM_OK);
    memcpy(im.px + (3 * 8 + 5) * 4, &RED, 4); /* (5,3) */
    pm_image cr;
    CHECK(pm_image_crop(&cr, &im, (pm_rect){4, 2, 100, 100}) == PM_OK); /* clipped */
    CHECK(cr.w == 4 && cr.h == 4 && is(&cr, 1, 1, RED) && is(&cr, 0, 0, WHITE));
    pm_image_free(&cr);
    CHECK(pm_image_crop(&cr, &im, (pm_rect){8, 0, 4, 4}) == PM_ERR_ARG);
    pm_image_free(&im);
}

static void test_stroke(void)
{
    pm_image im;
    pm_image_create(&im, 100, 60, WHITE);
    pm_point line[] = {{10, 30.5f}, {90, 30.5f}};
    CHECK(pm_draw_stroke(&im, line, 2, 10, RED) == PM_OK);
    CHECK(is(&im, 50, 30, RED));   /* on the centre line          */
    CHECK(is(&im, 50, 26, RED));   /* inside the 10 px width      */
    CHECK(is(&im, 50, 40, WHITE)); /* outside                     */
    CHECK(is(&im, 7, 30, RED));    /* round cap extends past x=10 */
    CHECK(is(&im, 2, 30, WHITE));
    const uint8_t *edge = at(&im, 50, 25); /* centre is exactly r from the line: half covered */
    CHECK(edge[1] > 100 && edge[1] < 155);

    /* translucent stroke that doubles back: overlap must not darken */
    pm_image a, b;
    pm_image_create(&a, 100, 60, WHITE);
    pm_image_create(&b, 100, 60, WHITE);
    pm_color hl = {0, 0, 255, 128};
    pm_point back[] = {{10, 30.5f}, {90, 30.5f}, {10, 30.5f}};
    pm_draw_stroke(&a, line, 2, 10, hl);
    pm_draw_stroke(&b, back, 3, 10, hl);
    CHECK(memcmp(a.px, b.px, 100 * 60 * 4) == 0);

    /* off-canvas and degenerate input */
    pm_point off[] = {{-500, -500}, {-400, -400}};
    CHECK(pm_draw_stroke(&a, off, 2, 10, RED) == PM_OK);
    pm_point nan_pt[] = {{0.0f / 0.0f, 1}};
    CHECK(pm_draw_stroke(&a, nan_pt, 1, 10, RED) == PM_ERR_ARG);
    pm_point dot[] = {{50, 10}};
    CHECK(pm_draw_stroke(&a, dot, 1, 6, RED) == PM_OK && is(&a, 50, 10, RED));

    /* blending onto a transparent pixel yields the source colour */
    pm_image t;
    pm_image_create(&t, 20, 20, (pm_color){0, 0, 0, 0});
    pm_draw_stroke(&t, (pm_point[]){{10, 10}}, 1, 8, hl);
    CHECK(at(&t, 10, 10)[2] == 255 && at(&t, 10, 10)[3] == 128);

    pm_image_free(&t);
    pm_image_free(&a);
    pm_image_free(&b);
    pm_image_free(&im);
}

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
    CHECK(pm_doc_can_redo(&d));
    CHECK(pm_doc_redo(&d) == PM_OK && pm_doc_redo(&d) == PM_OK && d.canvas.w == 20);
    CHECK(pm_doc_redo(&d) == PM_ERR_ARG);

    /* a new op discards the redo tail */
    pm_doc_undo(&d);
    CHECK(pm_doc_stroke(&d, p, 1, 4, RED) == PM_OK);
    CHECK(d.n_ops == 2 && !pm_doc_can_redo(&d) && d.canvas.w == 64);
    pm_doc_close(&d);
}

static void test_io(void)
{
    pm_image im, back;
    pm_image_create(&im, 31, 17, (pm_color){10, 200, 30, 255});
    pm_draw_stroke(&im, (pm_point[]){{3, 3}, {25, 12}}, 2, 3, (pm_color){0, 0, 255, 90});

    CHECK(pm_image_save(&im, "/tmp/pmark_test.png", 0) == PM_OK);
    CHECK(pm_image_load(&back, "/tmp/pmark_test.png") == PM_OK);
    CHECK(back.w == 31 && back.h == 17 && memcmp(im.px, back.px, 31 * 17 * 4) == 0);
    pm_image_free(&back);

    CHECK(pm_image_save(&im, "/tmp/pmark_test.JPG", 90) == PM_OK);
    CHECK(pm_image_load(&back, "/tmp/pmark_test.JPG") == PM_OK);
    CHECK(back.w == 31 && back.h == 17 && abs(at(&back, 28, 2)[1] - 200) < 12);
    pm_image_free(&back);

    CHECK(pm_image_save(&im, "/tmp/pmark_test.bmp", 0) == PM_ERR_FORMAT);
    CHECK(pm_image_load(&back, "/nonexistent/x.png") == PM_ERR_IO);
    pm_image_free(&im);
    remove("/tmp/pmark_test.png");
    remove("/tmp/pmark_test.JPG");
}

static void test_exif(void)
{
    /* SOI, APP1{Exif, big-endian TIFF, IFD0 with Orientation=6} */
    const uint8_t jpg[] = {
        0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
        'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,
        0x00, 0x01,
        0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0xFF, 0xD9};
    CHECK(pm_exif_orientation(jpg, sizeof jpg) == 6);
    CHECK(pm_exif_orientation(jpg, 20) == 1);           /* truncated */
    CHECK(pm_exif_orientation((const uint8_t *)"\x89PNG", 4) == 1);

    /* [A B] stored with orientation 6 (rotate 90 cw to view): A ends on top */
    pm_image im;
    pm_image_create(&im, 2, 1, WHITE);
    memcpy(im.px, &RED, 4);
    CHECK(pm_orient_apply(&im, 6) == PM_OK);
    CHECK(im.w == 1 && im.h == 2 && is(&im, 0, 0, RED) && is(&im, 0, 1, WHITE));
    CHECK(pm_orient_apply(&im, 3) == PM_OK && is(&im, 0, 1, RED));
    pm_image_free(&im);
}

static void test_text(void)
{
    pm_font *f;
    if (pm_font_load_default(&f) != PM_OK) {
        fprintf(stderr, "note: no system font found, text tests skipped\n");
        return;
    }
    float w1, h1, w2, h2;
    pm_text_measure(f, "Hello", 32, &w1, &h1);
    pm_text_measure(f, "Hello\nHello, world", 32, &w2, &h2);
    CHECK(w1 > 40 && w1 < 160 && h1 >= 32 && w2 > w1 && h2 > 1.9f * h1);

    pm_image im;
    pm_image_create(&im, 300, 120, WHITE);
    pm_op op = {.kind = PM_OP_TEXT, .color = RED};
    op.u.text.utf8 = "H\xC3\xA9llo";
    op.u.text.x = 40.5f; op.u.text.y = 30; op.u.text.size_px = 32; op.u.text.font = f;
    CHECK(pm_op_apply(&im, &op) == PM_OK);
    pm_rect b = pm_rect_intersect(pm_op_bounds(&op), (pm_rect){0, 0, im.w, im.h});
    int inside = 0, outside = 0;
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++)
            if (!is(&im, x, y, WHITE)) {
                if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) inside++;
                else outside++;
            }
    CHECK(inside > 200 && outside == 0); /* ink exists and stays in its bounds */

    CHECK(pm_text_draw(&im, f, "clipped", 280, 110, 40, RED) == PM_OK);
    CHECK(pm_text_draw(&im, f, "x", 0, 0, 0, RED) == PM_ERR_ARG);
    CHECK(pm_text_draw(&im, NULL, "x", 0, 0, 20, RED) == PM_ERR_FONT);
    pm_image_free(&im);
    pm_font_free(f);
}

int main(void)
{
    test_basics();
    test_stroke();
    test_doc();
    test_io();
    test_exif();
    test_text();
    printf("%d checks, %d failed\n", checks, fails);
    return fails > 100 ? 100 : fails;
}
