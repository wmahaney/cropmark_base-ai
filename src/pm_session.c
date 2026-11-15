/* Session files: a line-based, versioned text format recording the base
 * image and the applied op log. See docs/ARCHITECTURE.md, "Session file".
 * Parsing treats the file as untrusted input (it is fuzzed). */
#include "pm_internal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "pmark-session 1"
#define MAX_POINTS ((size_t)1 << 24)

/* ---------------------------------------------------------------- writing */

static void bprintf(pm_buf *b, const char *fmt, ...)
{
    char small[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = true; return; }
    if ((size_t)n < sizeof small) { pm_buf_put(b, small, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);
    if (!big) { b->oom = true; return; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    pm_buf_put(b, big, (size_t)n);
    free(big);
}

static void put_string(pm_buf *b, const char *s)
{
    pm_buf_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  pm_buf_put(b, "\\\"", 2); break;
        case '\\': pm_buf_put(b, "\\\\", 2); break;
        case '\n': pm_buf_put(b, "\\n", 2); break;
        case '\t': pm_buf_put(b, "\\t", 2); break;
        case '\r': pm_buf_put(b, "\\r", 2); break;
        default:
            if (*p < 0x20 || *p == 0x7F) bprintf(b, "\\x%02x", *p);
            else pm_buf_put(b, p, 1);
        }
    }
    pm_buf_put(b, "\"", 1);
}

static void put_color(pm_buf *b, pm_color c)
{
    bprintf(b, " %02x%02x%02x%02x", c.r, c.g, c.b, c.a);
}

/* Directory part of path ("." if none), malloc'd. */
static char *dir_of(const char *path)
{
    char *d = pm_strdup(path);
    if (!d) return NULL;
    char *slash = strrchr(d, '/');
#if defined(_WIN32)
    char *bs = strrchr(d, '\\');
    if (bs > slash) slash = bs;
#endif
    if (!slash) { free(d); return pm_strdup("."); }
    if (slash == d) slash[1] = 0; else *slash = 0;
    return d;
}

static bool is_absolute(const char *p)
{
#if defined(_WIN32)
    return p[0] == '\\' || p[0] == '/' || (p[0] && p[1] == ':');
#else
    return p[0] == '/';
#endif
}

pm_status pm_session_save(const pm_doc *doc, const char *path, bool overwrite)
{
    if (!doc || !path || !doc->base_path) return PM_ERR_ARG;

    /* Store the base relative to the session's directory when it lives at
     * or below it, so a folder of photo + session can be moved as a unit. */
    const char *base = doc->base_path;
    char *dir = dir_of(path), *dir_abs = dir ? pm_realpath(dir) : NULL;
    free(dir);
    if (dir_abs) {
        size_t dl = strlen(dir_abs);
        if (dl > 1 && strncmp(base, dir_abs, dl) == 0 && base[dl] == '/') base += dl + 1;
        else if (dl == 1 && base[0] == '/') base += 1;
    }

    pm_buf b = {0};
    bprintf(&b, MAGIC "\n");
    pm_buf_put(&b, "base ", 5);
    put_string(&b, base);
    bprintf(&b, " %016llx\n", (unsigned long long)doc->base_hash);
    free(dir_abs);

    for (size_t i = 0; i < doc->n_applied; i++) {
        const pm_op *op = &doc->ops[i];
        switch (op->kind) {
        case PM_OP_CROP:
            bprintf(&b, "crop %d %d %d %d\n", op->u.crop.x, op->u.crop.y,
                    op->u.crop.w, op->u.crop.h);
            break;
        case PM_OP_STROKE:
            pm_buf_put(&b, "stroke", 6);
            put_color(&b, op->color);
            bprintf(&b, " %.9g %zu", (double)op->u.stroke.width, op->u.stroke.n);
            for (size_t k = 0; k < op->u.stroke.n; k++)
                bprintf(&b, " %.9g %.9g", (double)op->u.stroke.pts[k].x,
                        (double)op->u.stroke.pts[k].y);
            pm_buf_put(&b, "\n", 1);
            break;
        case PM_OP_TEXT: {
            const pm_text_style *s = &op->u.text.style;
            const char *fp = pm_font_path(op->u.text.font);
            if (!fp) { free(b.p); return PM_ERR_FONT; }
            pm_buf_put(&b, "text", 4);
            put_color(&b, s->color);
            bprintf(&b, " %.9g %.9g %.9g %.9g", (double)s->size_px, (double)op->u.text.x,
                    (double)op->u.text.y, (double)s->outline_px);
            put_color(&b, s->outline);
            pm_buf_put(&b, " ", 1);
            put_string(&b, fp);
            pm_buf_put(&b, " ", 1);
            put_string(&b, op->u.text.utf8);
            pm_buf_put(&b, "\n", 1);
            break;
        }
        }
    }
    if (b.oom) { free(b.p); return PM_ERR_NOMEM; }
    pm_status st = pm_write_file_atomic(path, b.p, b.n, overwrite);
    free(b.p);
    return st;
}

/* ---------------------------------------------------------------- parsing */

typedef struct { char *s; bool quoted; } token;
typedef struct { token *t; size_t n, cap; } tokens;

static void toks_clear(tokens *tk)
{
    for (size_t i = 0; i < tk->n; i++) free(tk->t[i].s);
    tk->n = 0;
}

static bool toks_push(tokens *tk, char *s, bool quoted)
{
    if (!s) return false;
    if (tk->n == tk->cap) {
        size_t cap = tk->cap ? tk->cap * 2 : 32;
        token *t = realloc(tk->t, cap * sizeof *t);
        if (!t) { free(s); return false; }
        tk->t = t;
        tk->cap = cap;
    }
    tk->t[tk->n++] = (token){s, quoted};
    return true;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Splits one line into tokens. Returns PM_OK, PM_ERR_FORMAT or PM_ERR_NOMEM. */
static pm_status tokenize(const char *p, const char *end, tokens *tk)
{
    while (p < end) {
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        if (*p == '"') {
            char *s = malloc((size_t)(end - p));
            if (!s) return PM_ERR_NOMEM;
            size_t n = 0;
            p++;
            for (;;) {
                if (p >= end) { free(s); return PM_ERR_FORMAT; }
                char c = *p++;
                if (c == '"') break;
                if (c != '\\') { s[n++] = c; continue; }
                if (p >= end) { free(s); return PM_ERR_FORMAT; }
                c = *p++;
                if (c == 'n') s[n++] = '\n';
                else if (c == 't') s[n++] = '\t';
                else if (c == 'r') s[n++] = '\r';
                else if (c == '"' || c == '\\') s[n++] = c;
                else if (c == 'x' && end - p >= 2 && hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
                    int v = hexval(p[0]) * 16 + hexval(p[1]);
                    if (v == 0) { free(s); return PM_ERR_FORMAT; } /* no embedded NUL */
                    s[n++] = (char)v;
                    p += 2;
                } else { free(s); return PM_ERR_FORMAT; }
            }
            s[n] = 0;
            if (!toks_push(tk, s, true)) return PM_ERR_NOMEM;
        } else {
            const char *q = p;
            while (q < end && *q != ' ' && *q != '\t' && *q != '\r' && *q != '"') q++;
            char *s = malloc((size_t)(q - p) + 1);
            if (!s) return PM_ERR_NOMEM;
            memcpy(s, p, (size_t)(q - p));
            s[q - p] = 0;
            if (memchr(s, 0, (size_t)(q - p))) { free(s); return PM_ERR_FORMAT; }
            if (!toks_push(tk, s, false)) return PM_ERR_NOMEM;
            p = q;
        }
    }
    return PM_OK;
}

static bool num(const token *t, float *out)
{
    if (t->quoted) return false;
    errno = 0;
    char *e;
    double v = strtod(t->s, &e);
    if (e == t->s || *e || errno || !isfinite(v) || fabs(v) > 1.0e7) return false;
    *out = (float)v;
    return true;
}

static bool integer(const token *t, long lo, long hi, long *out)
{
    if (t->quoted) return false;
    errno = 0;
    char *e;
    long v = strtol(t->s, &e, 10);
    if (e == t->s || *e || errno || v < lo || v > hi) return false;
    *out = v;
    return true;
}

static bool color(const token *t, pm_color *c)
{
    return !t->quoted && strlen(t->s) == 8 && pm_color_parse(t->s, c);
}

static pm_status add_op(pm_session_data *d, pm_op *op, char *font)
{
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 16;
        pm_op *ops = realloc(d->ops, cap * sizeof *ops);
        if (ops) d->ops = ops;
        char **f = ops ? realloc(d->fonts, cap * sizeof *f) : NULL;
        if (f) d->fonts = f;
        if (!ops || !f) { pm_op_free(op); free(font); return PM_ERR_NOMEM; }
        d->cap = cap;
    }
    d->ops[d->n] = *op;
    d->fonts[d->n++] = font;
    return PM_OK;
}

static pm_status parse_line(tokens *tk, pm_session_data *d)
{
    token *t = tk->t;
    size_t n = tk->n;
    const char *cmd = t[0].quoted ? "" : t[0].s;

    if (!strcmp(cmd, "base")) {
        if (n != 3 || d->base || !t[1].quoted || !*t[1].s || t[2].quoted ||
            strlen(t[2].s) != 16)
            return PM_ERR_FORMAT;
        uint64_t h = 0;
        for (int i = 0; i < 16; i++) {
            int v = hexval(t[2].s[i]);
            if (v < 0) return PM_ERR_FORMAT;
            h = h << 4 | (uint64_t)v;
        }
        d->base = t[1].s;
        t[1].s = NULL; /* moved */
        d->hash = h;
        return PM_OK;
    }
    if (!strcmp(cmd, "crop")) {
        long v[4];
        if (n != 5) return PM_ERR_FORMAT;
        for (int i = 0; i < 4; i++)
            if (!integer(&t[i + 1], -PM_MAX_DIM, PM_MAX_DIM, &v[i])) return PM_ERR_FORMAT;
        pm_op op = {.kind = PM_OP_CROP};
        op.u.crop = (pm_rect){(int)v[0], (int)v[1], (int)v[2], (int)v[3]};
        return add_op(d, &op, NULL);
    }
    if (!strcmp(cmd, "stroke")) {
        pm_op op = {.kind = PM_OP_STROKE};
        long np;
        if (n < 4 || !color(&t[1], &op.color) || !num(&t[2], &op.u.stroke.width) ||
            !integer(&t[3], 1, (long)MAX_POINTS, &np) || n != 4 + 2 * (size_t)np)
            return PM_ERR_FORMAT;
        op.u.stroke.pts = malloc((size_t)np * sizeof(pm_point));
        if (!op.u.stroke.pts) return PM_ERR_NOMEM;
        op.u.stroke.n = (size_t)np;
        for (size_t k = 0; k < (size_t)np; k++)
            if (!num(&t[4 + 2 * k], &op.u.stroke.pts[k].x) ||
                !num(&t[5 + 2 * k], &op.u.stroke.pts[k].y)) {
                pm_op_free(&op);
                return PM_ERR_FORMAT;
            }
        return add_op(d, &op, NULL);
    }
    if (!strcmp(cmd, "text")) {
        pm_op op = {.kind = PM_OP_TEXT};
        pm_text_style *s = &op.u.text.style;
        if (n != 9 || !color(&t[1], &s->color) || !num(&t[2], &s->size_px) ||
            !num(&t[3], &op.u.text.x) || !num(&t[4], &op.u.text.y) ||
            !num(&t[5], &s->outline_px) || !color(&t[6], &s->outline) ||
            !t[7].quoted || !*t[7].s || !t[8].quoted)
            return PM_ERR_FORMAT;
        op.color = s->color;
        op.u.text.utf8 = t[8].s;
        t[8].s = NULL;
        char *font = t[7].s;
        t[7].s = NULL;
        return add_op(d, &op, font);
    }
    return PM_ERR_FORMAT;
}

void pm_session_data_free(pm_session_data *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->n; i++) { pm_op_free(&d->ops[i]); free(d->fonts[i]); }
    free(d->ops);
    free(d->fonts);
    free(d->base);
    memset(d, 0, sizeof *d);
}

pm_status pm_session_parse(const char *text, size_t len, pm_session_data *out)
{
    if (!text || !out) return PM_ERR_ARG;
    pm_session_data d = {0};
    tokens tk = {0};
    pm_status st = PM_OK;
    bool seen_magic = false;
    const char *p = text, *end = text + len;

    while (st == PM_OK && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        const char *q = p;
        while (q < le && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
        if (q < le && *q != '#') { /* skip blank and comment lines */
            if (!seen_magic) {
                size_t ml = strlen(MAGIC);
                const char *e = le;
                while (e > q && (e[-1] == '\r' || e[-1] == ' ')) e--;
                if ((size_t)(e - q) != ml || memcmp(q, MAGIC, ml)) st = PM_ERR_FORMAT;
                seen_magic = true;
            } else {
                st = tokenize(q, le, &tk);
                if (st == PM_OK && tk.n) st = parse_line(&tk, &d);
                toks_clear(&tk);
            }
        }
        p = nl ? nl + 1 : end;
    }
    free(tk.t);
    if (st == PM_OK && (!seen_magic || !d.base)) st = PM_ERR_FORMAT;
    if (st != PM_OK) { pm_session_data_free(&d); return st; }
    *out = d;
    return PM_OK;
}

/* ---------------------------------------------------------------- loading */

bool pm_is_session_file(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return false;
    char head[16] = {0};
    size_t n = fread(head, 1, 14, f);
    fclose(f);
    return n == 14 && memcmp(head, "pmark-session ", 14) == 0;
}

/* Font for op i: shared between ops that name the same file. */
static pm_status font_for(pm_doc *doc, char **paths, const pm_font **fonts, size_t i,
                          unsigned flags)
{
    for (size_t j = 0; j < i; j++)
        if (paths[j] && fonts[j] && !strcmp(paths[j], paths[i])) {
            fonts[i] = fonts[j];
            return PM_OK;
        }
    pm_font *f = NULL;
    pm_status st = pm_font_load(&f, paths[i]);
    if (st != PM_OK && (flags & PM_SESSION_LENIENT)) st = pm_font_load_default(&f);
    if (st != PM_OK) return PM_ERR_FONT;
    fonts[i] = f;
    return pm_doc_own_font(doc, f);
}

pm_status pm_session_load(pm_doc *doc, const char *path, unsigned flags)
{
    if (!doc || !path) return PM_ERR_ARG;
    uint8_t *text;
    size_t len;
    pm_status st = pm_read_file(path, &text, &len);
    if (st != PM_OK) return st;
    pm_session_data d;
    st = pm_session_parse((const char *)text, len, &d);
    free(text);
    if (st != PM_OK) return st;

    char *base = NULL;
    if (is_absolute(d.base)) base = pm_strdup(d.base);
    else {
        char *dir = dir_of(path);
        if (dir) {
            size_t n = strlen(dir) + strlen(d.base) + 2;
            if ((base = malloc(n))) snprintf(base, n, "%s/%s", dir, d.base);
            free(dir);
        }
    }
    pm_doc tmp;
    st = base ? pm_doc_open(&tmp, base) : PM_ERR_NOMEM;
    free(base);
    if (st != PM_OK) { pm_session_data_free(&d); return st; }
    if (tmp.base_hash != d.hash && !(flags & PM_SESSION_LENIENT)) st = PM_ERR_STALE;

    const pm_font **fonts = st == PM_OK && d.n ? calloc(d.n, sizeof *fonts) : NULL;
    if (st == PM_OK && d.n && !fonts) st = PM_ERR_NOMEM;
    for (size_t i = 0; st == PM_OK && i < d.n; i++) {
        pm_op *op = &d.ops[i];
        switch (op->kind) {
        case PM_OP_CROP:
            st = pm_doc_crop(&tmp, op->u.crop);
            break;
        case PM_OP_STROKE:
            st = pm_doc_stroke(&tmp, op->u.stroke.pts, op->u.stroke.n,
                               op->u.stroke.width, op->color);
            break;
        case PM_OP_TEXT:
            st = font_for(&tmp, d.fonts, fonts, i, flags);
            if (st == PM_OK)
                st = pm_doc_text(&tmp, fonts[i], op->u.text.utf8, op->u.text.x,
                                 op->u.text.y, &op->u.text.style);
            break;
        }
    }
    free(fonts);
    pm_session_data_free(&d);
    if (st != PM_OK) { pm_doc_close(&tmp); return st; }
    *doc = tmp;
    return PM_OK;
}
