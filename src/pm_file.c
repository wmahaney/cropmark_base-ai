/* File-system helpers: whole-file reads, atomic writes, path identity. */
#if !defined(_WIN32)
#define _XOPEN_SOURCE 700 /* POSIX.1-2008 plus XSI: realpath is XSI */
#endif
#include "pm_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

char *pm_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

uint64_t pm_fnv1a64(const uint8_t *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 0x100000001b3ull; }
    return h;
}

uint32_t pm_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static bool ready;
    if (!ready) { /* benign race: every thread computes identical values */
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void pm_buf_put(pm_buf *b, const void *data, size_t n)
{
    if (b->oom || n == 0) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) { b->oom = true; return; }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

pm_status pm_read_file(const char *path, uint8_t **data, size_t *len)
{
    if (!path || !data || !len) return PM_ERR_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return PM_ERR_IO;
    size_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { fclose(f); return PM_ERR_NOMEM; }
    for (;;) {
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap) break;
        if (cap >= PM_MAX_FILE_BYTES) { free(buf); fclose(f); return PM_ERR_TOO_BIG; }
        uint8_t *nb = realloc(buf, cap *= 2);
        if (!nb) { free(buf); fclose(f); return PM_ERR_NOMEM; }
        buf = nb;
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) { free(buf); return PM_ERR_IO; }
    *data = buf;
    *len = n;
    return PM_OK;
}

#if defined(_WIN32) /* -------------------------------------------- Windows */

char *pm_realpath(const char *path)
{
    char *r = _fullpath(NULL, path, 0);
    if (r && GetFileAttributesA(r) == INVALID_FILE_ATTRIBUTES) { free(r); r = NULL; }
    return r;
}

bool pm_path_exists(const char *path)
{
    return path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool pm_path_same_file(const char *a, const char *b)
{
    char *ra = pm_realpath(a), *rb = pm_realpath(b);
    bool same = ra && rb && _stricmp(ra, rb) == 0;
    free(ra);
    free(rb);
    return same;
}

pm_status pm_write_file_atomic(const char *path, const uint8_t *data, size_t len,
                               bool overwrite)
{
    if (!path || (!data && len)) return PM_ERR_ARG;
    if (!overwrite && pm_path_exists(path)) return PM_ERR_EXISTS;
    static unsigned counter;
    size_t pl = strlen(path);
    char *tmp = malloc(pl + 48);
    if (!tmp) return PM_ERR_NOMEM;
    snprintf(tmp, pl + 48, "%s.pmtmp-%lu-%u", path, GetCurrentProcessId(), counter++);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return PM_ERR_IO; }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (_commit(_fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    pm_status st = PM_OK;
    if (!ok) st = PM_ERR_IO;
    else if (!MoveFileExA(tmp, path, flags))
        st = GetLastError() == ERROR_ALREADY_EXISTS ? PM_ERR_EXISTS : PM_ERR_IO;
    if (st != PM_OK) remove(tmp);
    free(tmp);
    return st;
}

#else /* ----------------------------------------------------------- POSIX */

char *pm_realpath(const char *path)
{
    return path ? realpath(path, NULL) : NULL;
}

bool pm_path_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

bool pm_path_same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (!a || !b || stat(a, &sa) != 0 || stat(b, &sb) != 0) return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void fsync_parent_dir(const char *path)
{
    char *dir = pm_strdup(path);
    if (!dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { (void)fsync(fd); close(fd); } /* best effort */
    free(dir);
}

pm_status pm_write_file_atomic(const char *path, const uint8_t *data, size_t len,
                               bool overwrite)
{
    if (!path || !*path || (!data && len)) return PM_ERR_ARG;
    size_t pl = strlen(path);
    char *tmp = malloc(pl + 16);
    if (!tmp) return PM_ERR_NOMEM;
    memcpy(tmp, path, pl);
    memcpy(tmp + pl, ".pmtmp-XXXXXX", 14);

    int fd = mkstemp(tmp); /* same directory, so rename() stays atomic */
    if (fd < 0) { free(tmp); return PM_ERR_IO; }
    mode_t um = umask(0);  /* mkstemp creates 0600; use the normal 0666 & ~umask */
    umask(um);
    (void)fchmod(fd, 0666 & ~um);

    bool ok = true;
    for (size_t off = 0; ok && off < len;) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) ok = false;
        else off += (size_t)w;
    }
    ok = (fsync(fd) == 0) && ok;
    ok = (close(fd) == 0) && ok;

    pm_status st = ok ? PM_OK : PM_ERR_IO;
    if (st == PM_OK && overwrite) {
        if (rename(tmp, path) != 0) st = PM_ERR_IO;
    } else if (st == PM_OK) {
        /* link() fails with EEXIST if path appeared, with no race window */
        if (link(tmp, path) == 0) unlink(tmp);
        else if (errno == EEXIST) st = PM_ERR_EXISTS;
        else if (pm_path_exists(path)) st = PM_ERR_EXISTS; /* FS without hard links */
        else if (rename(tmp, path) != 0) st = PM_ERR_IO;
    }
    if (st != PM_OK) unlink(tmp);
    else fsync_parent_dir(path);
    free(tmp);
    return st;
}

#endif
