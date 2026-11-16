/* ICC colour profiles: read from JPEG APP2 / PNG iCCP, written back out.
 * Phones tag photos with wide-gamut profiles (e.g. Display P3); dropping the
 * profile makes saved images look washed out, so it is carried through. */
#include "pm_internal.h"

#include <stdlib.h>
#include <string.h>

static const char ICC_SIG[12] = "ICC_PROFILE"; /* includes the trailing NUL */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(pm_buf *b, uint32_t v)
{
    uint8_t q[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    pm_buf_put(b, q, 4);
}

/* ---------------------------------------------------------------- JPEG */

static uint8_t *icc_from_jpeg(const uint8_t *d, size_t len, size_t *out_len)
{
    /* APP2 chunks: "ICC_PROFILE\0", seq (1-based), count, data */
    const uint8_t *part[256] = {0};
    size_t part_len[256] = {0};
    unsigned count = 0;
    size_t pos = 2;
    while (pos + 4 <= len) {
        if (d[pos] != 0xFF) break;
        unsigned marker = d[pos + 1];
        if (marker == 0xFF) { pos++; continue; }
        if (marker == 0xDA || marker == 0xD9) break;
        size_t seg = (size_t)d[pos + 2] << 8 | d[pos + 3];
        if (seg < 2 || pos + 2 + seg > len) break;
        if (marker == 0xE2 && seg >= 16 && memcmp(d + pos + 4, ICC_SIG, 12) == 0) {
            unsigned seq = d[pos + 16], cnt = d[pos + 17];
            if (seq == 0 || cnt == 0 || seq > cnt || (count && cnt != count)) return NULL;
            count = cnt;
            part[seq] = d + pos + 18;
            part_len[seq] = seg - 16;
        }
        pos += 2 + seg;
    }
    if (!count) return NULL;
    size_t total = 0;
    for (unsigned i = 1; i <= count; i++) {
        if (!part[i]) return NULL; /* incomplete profile: ignore it */
        total += part_len[i];
    }
    if (total == 0 || total > PM_MAX_ICC_BYTES) return NULL;
    uint8_t *icc = malloc(total);
    if (!icc) return NULL;
    size_t off = 0;
    for (unsigned i = 1; i <= count; i++) {
        memcpy(icc + off, part[i], part_len[i]);
        off += part_len[i];
    }
    *out_len = total;
    return icc;
}

pm_status pm_icc_embed_jpeg(pm_buf *out, const uint8_t *jpg, size_t len,
                            const uint8_t *icc, size_t icc_len)
{
    enum { CHUNK = 65535 - 2 - 14 };
    if (len < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8 || !icc || !icc_len) return PM_ERR_ARG;
    size_t count = (icc_len + CHUNK - 1) / CHUNK;
    if (count > 255) return PM_ERR_ARG;

    size_t at = 2; /* keep a leading JFIF APP0 first, as readers expect */
    if (len >= 6 && jpg[2] == 0xFF && jpg[3] == 0xE0) {
        size_t seg = (size_t)jpg[4] << 8 | jpg[5];
        if (4 + seg <= len) at = 2 + 2 + seg;
    }
    pm_buf_put(out, jpg, at);
    for (size_t i = 0; i < count; i++) {
        size_t n = icc_len - i * CHUNK < CHUNK ? icc_len - i * CHUNK : CHUNK;
        size_t seg = n + 16;
        uint8_t hdr[4] = {0xFF, 0xE2, (uint8_t)(seg >> 8), (uint8_t)seg};
        uint8_t seqcnt[2] = {(uint8_t)(i + 1), (uint8_t)count};
        pm_buf_put(out, hdr, 4);
        pm_buf_put(out, ICC_SIG, 12);
        pm_buf_put(out, seqcnt, 2);
        pm_buf_put(out, icc + i * CHUNK, n);
    }
    pm_buf_put(out, jpg + at, len - at);
    return out->oom ? PM_ERR_NOMEM : PM_OK;
}

/* ----------------------------------------------------------------- PNG */

static const uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

static uint8_t *icc_from_png(const uint8_t *d, size_t len, size_t *out_len)
{
    size_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t n = be32(d + pos);
        if (n > len - pos - 12) return NULL;
        const uint8_t *type = d + pos + 4, *data = d + pos + 8;
        if (!memcmp(type, "IDAT", 4) || !memcmp(type, "IEND", 4)) return NULL;
        if (!memcmp(type, "iCCP", 4)) {
            const uint8_t *nul = memchr(data, 0, n < 80 ? n : 80);
            if (!nul || nul == data || (size_t)(nul - data) + 2 > n || nul[1] != 0) return NULL;
            size_t zlen = n - (size_t)(nul - data) - 2;
            int ilen;
            uint8_t *icc = pm_stb_zlib_decompress(nul + 2, (int)zlen, &ilen);
            if (!icc || ilen <= 0 || (size_t)ilen > PM_MAX_ICC_BYTES) { free(icc); return NULL; }
            *out_len = (size_t)ilen;
            return icc;
        }
        pos += 12 + n;
    }
    return NULL;
}

pm_status pm_icc_embed_png(pm_buf *out, const uint8_t *png, size_t len,
                           const uint8_t *icc, size_t icc_len)
{
    /* the chunk goes right after IHDR (8 sig + 4 len + 4 type + 13 + 4 crc) */
    if (len < 33 || memcmp(png, PNG_SIG, 8) || memcmp(png + 12, "IHDR", 4) ||
        !icc || !icc_len || icc_len > PM_MAX_ICC_BYTES)
        return PM_ERR_ARG;
    int zlen;
    uint8_t *z = pm_stb_zlib_compress(icc, (int)icc_len, &zlen);
    if (!z) return PM_ERR_NOMEM;
    static const char name[] = "ICC Profile"; /* + NUL, then method 0 */
    uint8_t chunk_hdr[4 + sizeof name + 1];
    memcpy(chunk_hdr, "iCCP", 4);
    memcpy(chunk_hdr + 4, name, sizeof name);
    chunk_hdr[4 + sizeof name] = 0;
    uint32_t crc = pm_crc32(0, chunk_hdr, sizeof chunk_hdr);
    crc = pm_crc32(crc, z, (size_t)zlen);

    pm_buf_put(out, png, 33);
    put_be32(out, (uint32_t)(sizeof chunk_hdr - 4 + (size_t)zlen));
    pm_buf_put(out, chunk_hdr, sizeof chunk_hdr);
    pm_buf_put(out, z, (size_t)zlen);
    put_be32(out, crc);
    pm_buf_put(out, png + 33, len - 33);
    free(z);
    return out->oom ? PM_ERR_NOMEM : PM_OK;
}

uint8_t *pm_icc_extract(const uint8_t *d, size_t len, size_t *icc_len)
{
    *icc_len = 0;
    if (!d) return NULL;
    if (len >= 4 && d[0] == 0xFF && d[1] == 0xD8) return icc_from_jpeg(d, len, icc_len);
    if (len >= 8 && memcmp(d, PNG_SIG, 8) == 0) return icc_from_png(d, len, icc_len);
    return NULL;
}
