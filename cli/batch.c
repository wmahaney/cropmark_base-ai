/* cropmark batch mode: apply edits from the command line, no window.
 * Edits are applied left to right. Reached through cli/main.c when the
 * arguments contain edits or output options. */
#include "cropmark.h"
#include "pmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cm_batch_usage(FILE *f)
{
    fputs(
"Batch mode: cropmark INPUT EDITS... [-o OUTPUT] [--save-session FILE.pmark]\n"
"\n"
"INPUT is a .jpg/.png photo, or a .pmark session to continue editing.\n"
"Edits run in the order given; coordinates are pixels in the image as it\n"
"is at that point (so after any earlier --crop).\n"
"\n"
"Edits:\n"
"  --crop X,Y,W,H         keep only this rectangle\n"
"  --stroke \"X,Y X,Y ...\" marker stroke through the points (1 point = dot)\n"
"  --arrow X0,Y0,X1,Y1    arrow pointing at X1,Y1\n"
"  --rect X,Y,W,H         rectangle outline\n"
"  --ellipse X,Y,W,H      ellipse outline inscribed in the rectangle\n"
"  --text X,Y STRING      text with its top-left corner at X,Y (\\n = newline)\n"
"\n"
"Settings (affect the edits that follow):\n"
"  --color RRGGBB[AA]     default e53935 (red); AA < ff gives a highlighter\n"
"  --width PX             line width, default 8\n"
"  --size PX              text height, default 48\n"
"  --outline PX           text outline width, default 0 (none)\n"
"  --outline-color HEX    text outline colour, default 000000\n"
"  --font FILE.ttf        default: $PMARK_FONT, then common system fonts\n"
"\n"
"Output:\n"
"  -o FILE                .png or .jpg/.jpeg (written atomically)\n"
"  --quality N            JPEG quality 1-100, default 92\n"
"  --no-clobber           fail rather than replace an existing output file\n"
"  --force                allow OUTPUT to be the input photo itself\n"
"  --strip-icc            do not carry the colour profile into OUTPUT\n"
"  --save-session FILE    write a .pmark session (base path + edit log)\n"
"  --lenient              open a session even if its base image changed\n"
"                         or one of its fonts is missing\n"
"  --info                 print the final dimensions\n", f);
}

static int fail(const char *what, pm_status st)
{
    fprintf(stderr, "cropmark: %s: %s\n", what, pm_strerror(st));
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

static bool parse_rect(const char *s, pm_rect *r)
{
    char tail;
    return sscanf(s, "%d,%d,%d,%d%c", &r->x, &r->y, &r->w, &r->h, &tail) == 4;
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

/* Pre-scan for flags that change how INPUT is opened. */
static bool has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], flag)) return true;
    return false;
}

int cm_batch_main(int argc, char **argv)
{
    if (argc < 2) {
        cm_batch_usage(stderr);
        return 2;
    }

    pm_doc doc;
    bool is_session = pm_is_session_file(argv[1]);
    pm_status st = is_session
        ? pm_session_load(&doc, argv[1], has_flag(argc, argv, "--lenient") ? PM_SESSION_LENIENT : 0)
        : pm_doc_open(&doc, argv[1]);
    if (st != PM_OK) return fail(argv[1], st);

    pm_color color = {0xe5, 0x39, 0x35, 0xff};
    float width = 8.0f;
    pm_text_style ts = {48.0f, color, 0.0f, {0, 0, 0, 255}};
    const pm_font *font = NULL; /* owned by doc */
    const char *out = NULL, *session_out = NULL;
    int quality = 0, rc = 0;
    bool info = false, no_clobber = false, force = false, strip_icc = false;

#define NEED(k) \
    if (i + (k) >= argc) { fprintf(stderr, "cropmark: %s needs %d argument(s)\n", a, (k)); rc = 2; break; }
#define BAD(msg) { fprintf(stderr, "cropmark: %s\n", msg); rc = 2; break; }

    for (int i = 2; i < argc && rc == 0; i++) {
        const char *a = argv[i];
        pm_point *pts = NULL;
        size_t n = 0;
        if (!strcmp(a, "--crop")) {
            NEED(1);
            pm_rect r;
            if (!parse_rect(argv[++i], &r)) BAD("--crop wants X,Y,W,H");
            if ((st = pm_doc_crop(&doc, r)) != PM_OK) rc = fail("--crop", st);
            continue;
        } else if (!strcmp(a, "--stroke")) {
            NEED(1);
            if (!(pts = parse_points(argv[++i], &n))) BAD("--stroke wants \"X,Y X,Y ...\"");
        } else if (!strcmp(a, "--arrow")) {
            NEED(1);
            float x0, y0, x1, y1;
            char tail;
            if (sscanf(argv[++i], "%f,%f,%f,%f%c", &x0, &y0, &x1, &y1, &tail) != 4)
                BAD("--arrow wants X0,Y0,X1,Y1");
            pts = pm_shape_arrow((pm_point){x0, y0}, (pm_point){x1, y1}, width, &n);
        } else if (!strcmp(a, "--rect") || !strcmp(a, "--ellipse")) {
            NEED(1);
            pm_rect r;
            if (!parse_rect(argv[++i], &r)) BAD("--rect/--ellipse want X,Y,W,H");
            pts = a[2] == 'r' ? pm_shape_rect(r, &n) : pm_shape_ellipse(r, &n);
        } else if (!strcmp(a, "--text")) {
            NEED(2);
            float x, y;
            char tail;
            if (sscanf(argv[++i], "%f,%f%c", &x, &y, &tail) != 2) BAD("--text wants X,Y STRING");
            if (!font) {
                pm_font *f;
                if ((st = pm_font_load_default(&f)) != PM_OK) { rc = fail("--text", st); break; }
                if ((st = pm_doc_own_font(&doc, f)) != PM_OK) { rc = fail("--text", st); break; }
                font = f;
            }
            unescape(argv[++i]);
            ts.color = color;
            if ((st = pm_doc_text(&doc, font, argv[i], x, y, &ts)) != PM_OK)
                rc = fail("--text", st);
            continue;
        } else if (!strcmp(a, "--color") || !strcmp(a, "--outline-color")) {
            NEED(1);
            pm_color *dst = a[2] == 'c' ? &color : &ts.outline;
            if (!pm_color_parse(argv[++i], dst)) BAD("colours are RRGGBB or RRGGBBAA");
            continue;
        } else if (!strcmp(a, "--width") || !strcmp(a, "--size") || !strcmp(a, "--outline")) {
            NEED(1);
            char *e;
            float v = strtof(argv[++i], &e);
            if (*e || !(v >= 0.0f)) BAD("sizes are non-negative numbers");
            if (a[2] == 'w') width = v; else if (a[2] == 's') ts.size_px = v; else ts.outline_px = v;
            continue;
        } else if (!strcmp(a, "--font")) {
            NEED(1);
            pm_font *f;
            if ((st = pm_font_load(&f, argv[++i])) != PM_OK) { rc = fail(argv[i], st); break; }
            if ((st = pm_doc_own_font(&doc, f)) != PM_OK) { rc = fail("--font", st); break; }
            font = f; /* earlier text keeps its font: the doc owns them all */
            continue;
        } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            NEED(1); out = argv[++i]; continue;
        } else if (!strcmp(a, "--save-session")) {
            NEED(1); session_out = argv[++i]; continue;
        } else if (!strcmp(a, "--quality")) {
            NEED(1);
            quality = atoi(argv[++i]);
            if (quality < 1 || quality > 100) BAD("--quality wants 1-100");
            continue;
        } else if (!strcmp(a, "--info"))       { info = true; continue; }
        else if (!strcmp(a, "--no-clobber"))   { no_clobber = true; continue; }
        else if (!strcmp(a, "--force"))        { force = true; continue; }
        else if (!strcmp(a, "--strip-icc"))    { strip_icc = true; continue; }
        else if (!strcmp(a, "--lenient"))      { continue; } /* handled above */
        else {
            fprintf(stderr, "cropmark: unknown option %s (try --help)\n", a);
            rc = 2;
            break;
        }
        /* the stroke-like edits fall through to here */
        if (!pts) { rc = fail(a, PM_ERR_NOMEM); break; }
        st = pm_doc_stroke(&doc, pts, n, width, color);
        free(pts);
        if (st != PM_OK) rc = fail(a, st);
    }

    if (rc == 0 && out) {
        if (pm_format_from_path(out) == PM_FMT_UNKNOWN) {
            rc = fail(out, PM_ERR_FORMAT);
        } else if (!force && doc.base_path && pm_path_same_file(out, doc.base_path)) {
            fprintf(stderr, "cropmark: %s is the input photo; refusing to replace it "
                            "(use --force, or write elsewhere)\n", out);
            rc = 1;
        } else {
            if (strip_icc) pm_meta_free(&doc.meta);
            if ((st = pm_doc_export(&doc, out, quality, !no_clobber)) != PM_OK)
                rc = fail(out, st);
        }
    }
    if (rc == 0 && session_out &&
        (st = pm_session_save(&doc, session_out, !no_clobber)) != PM_OK)
        rc = fail(session_out, st);
    if (rc == 0 && info) printf("%dx%d\n", doc.canvas.w, doc.canvas.h);
    if (rc == 0 && !out && !info && !session_out) {
        fprintf(stderr, "cropmark: nothing to do (no -o OUTPUT)\n");
        rc = 2;
    }
    pm_doc_close(&doc);
    return rc;
}
