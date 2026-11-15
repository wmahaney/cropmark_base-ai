/* pmark -- scriptable frontend. Edits are applied left to right. */
#include "pmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f)
{
    fputs(
"usage: pmark INPUT [edits...] -o OUTPUT\n"
"\n"
"Edits run in the order given; coordinates are pixels in the image as it\n"
"is at that point (so after any earlier --crop).\n"
"\n"
"  --crop X,Y,W,H         keep only this rectangle\n"
"  --stroke \"X,Y X,Y ...\" marker stroke through the points (1 point = dot)\n"
"  --text X,Y STRING      text with its top-left corner at X,Y (\\n = newline)\n"
"\n"
"Settings (affect the edits that follow):\n"
"  --color RRGGBB[AA]     default e53935 (red); AA < ff gives a highlighter\n"
"  --width PX             marker width, default 8\n"
"  --size PX              text height, default 48\n"
"  --font FILE.ttf        default: $PMARK_FONT, then common system fonts\n"
"\n"
"Output:\n"
"  -o FILE                .png or .jpg/.jpeg\n"
"  --quality N            JPEG quality 1-100, default 92\n"
"  --info                 print the final dimensions\n", f);
}

static int fail(const char *what, pm_status st)
{
    fprintf(stderr, "pmark: %s: %s\n", what, pm_strerror(st));
    return 1;
}

/* Parses "x,y x,y ..." (any mix of spaces/commas between numbers). */
static pm_point *parse_points(const char *s, size_t *n_out)
{
    size_t cap = 16, n = 0, k = 0;
    float xy[2];
    pm_point *pts = malloc(cap * sizeof *pts);
    if (!pts) return NULL;
    for (;;) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!*s) break;
        char *end;
        xy[k++] = strtof(s, &end);
        if (end == s) { free(pts); return NULL; }
        s = end;
        if (k == 2) {
            if (n == cap) {
                pm_point *np = realloc(pts, (cap *= 2) * sizeof *pts);
                if (!np) { free(pts); return NULL; }
                pts = np;
            }
            pts[n++] = (pm_point){xy[0], xy[1]};
            k = 0;
        }
    }
    if (k != 0 || n == 0) { free(pts); return NULL; }
    *n_out = n;
    return pts;
}

/* Turns the two-character sequence \n into a newline, in place. */
static void unescape(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') { *w++ = '\n'; s++; }
        else *w++ = *s;
    }
    *w = 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2;
    }

    pm_doc doc;
    pm_status st = pm_doc_open(&doc, argv[1]);
    if (st != PM_OK) return fail(argv[1], st);

    pm_color color = {0xe5, 0x39, 0x35, 0xff};
    float width = 8.0f, size = 48.0f;
    pm_font *font = NULL;
    const char *out = NULL;
    int quality = 0, rc = 0;
    bool info = false;

#define NEED(k) \
    if (i + (k) >= argc) { fprintf(stderr, "pmark: %s needs %d argument(s)\n", a, (k)); rc = 2; break; }

    for (int i = 2; i < argc && rc == 0; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--crop")) {
            NEED(1);
            pm_rect r;
            if (sscanf(argv[++i], "%d,%d,%d,%d", &r.x, &r.y, &r.w, &r.h) != 4) {
                fprintf(stderr, "pmark: --crop wants X,Y,W,H\n"); rc = 2; break;
            }
            if ((st = pm_doc_crop(&doc, r)) != PM_OK) rc = fail("--crop", st);
        } else if (!strcmp(a, "--stroke")) {
            NEED(1);
            size_t n;
            pm_point *pts = parse_points(argv[++i], &n);
            if (!pts) { fprintf(stderr, "pmark: --stroke wants \"X,Y X,Y ...\"\n"); rc = 2; break; }
            st = pm_doc_stroke(&doc, pts, n, width, color);
            free(pts);
            if (st != PM_OK) rc = fail("--stroke", st);
        } else if (!strcmp(a, "--text")) {
            NEED(2);
            float x, y;
            if (sscanf(argv[++i], "%f,%f", &x, &y) != 2) {
                fprintf(stderr, "pmark: --text wants X,Y STRING\n"); rc = 2; break;
            }
            if (!font && (st = pm_font_load_default(&font)) != PM_OK) { rc = fail("--text", st); break; }
            unescape(argv[++i]);
            if ((st = pm_doc_text(&doc, font, argv[i], x, y, size, color)) != PM_OK)
                rc = fail("--text", st);
        } else if (!strcmp(a, "--color")) {
            NEED(1);
            if (!pm_color_parse(argv[++i], &color)) {
                fprintf(stderr, "pmark: --color wants RRGGBB or RRGGBBAA\n"); rc = 2;
            }
        } else if (!strcmp(a, "--width")) {
            NEED(1);
            width = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--size")) {
            NEED(1);
            size = strtof(argv[++i], NULL);
        } else if (!strcmp(a, "--font")) {
            NEED(1);
            /* Earlier text ops borrow the previous font, so it has to stay
             * alive until the document closes; fonts are tiny, and this is a
             * one-shot process, so the old one is simply left allocated. */
            pm_font *nf;
            if ((st = pm_font_load(&nf, argv[++i])) != PM_OK) rc = fail(argv[i], st);
            else font = nf;
        } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            NEED(1);
            out = argv[++i];
        } else if (!strcmp(a, "--quality")) {
            NEED(1);
            quality = atoi(argv[++i]);
        } else if (!strcmp(a, "--info")) {
            info = true;
        } else {
            fprintf(stderr, "pmark: unknown option %s (try --help)\n", a);
            rc = 2;
        }
    }

    if (rc == 0 && info) printf("%dx%d\n", doc.canvas.w, doc.canvas.h);
    if (rc == 0 && out && (st = pm_image_save(&doc.canvas, out, quality)) != PM_OK)
        rc = fail(out, st);
    if (rc == 0 && !out && !info) {
        fprintf(stderr, "pmark: nothing to do (no -o OUTPUT)\n");
        rc = 2;
    }

    pm_doc_close(&doc);
    pm_font_free(font);
    return rc;
}
