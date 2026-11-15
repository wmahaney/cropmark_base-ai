/* Fuzz target for every parser that sees untrusted bytes: the image
 * decoders (via stb), EXIF orientation, ICC extraction, and the session
 * parser.
 *
 *   clang:  clang -fsanitize=fuzzer,address -DPM_LIBFUZZER ... (make fuzz-libfuzzer)
 *   gcc:    built-in mutation driver below          (make fuzz-smoke)
 */
#include "pm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    pm_image im;
    pm_meta meta = {NULL, 0};
    if (pm_image_decode(&im, &meta, d, n) == PM_OK) {
        /* touch every pixel so ASan sees the whole buffer */
        unsigned acc = 0;
        for (size_t i = 0; i < (size_t)im.w * im.h * 4; i++) acc += im.px[i];
        if (acc == 0xdeadbeef) puts("");
        pm_image_free(&im);
    }
    pm_meta_free(&meta);

    size_t icc_len;
    free(pm_icc_extract(d, n, &icc_len));
    (void)pm_exif_orientation(d, n);

    pm_session_data s;
    if (pm_session_parse((const char *)d, n, &s) == PM_OK) pm_session_data_free(&s);
    return 0;
}

#ifndef PM_LIBFUZZER
/* Deterministic mutation driver: seeds are mutated with bit flips,
 * interesting bytes, truncation, splices and block duplication. */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static size_t mutate(uint8_t *b, size_t n, size_t cap)
{
    static const uint8_t interesting[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, '"', '\\', '\n', ' '};
    int rounds = 1 + (int)(rnd() % 8);
    for (int r = 0; r < rounds && n > 0; r++) {
        size_t i = (size_t)(rnd() % n);
        switch (rnd() % 7) {
        case 0: b[i] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: b[i] = interesting[rnd() % sizeof interesting]; break;
        case 2: b[i] = (uint8_t)rnd(); break;
        case 3: n = i + 1; break; /* truncate */
        case 4: { /* 16/32-bit field to an extreme: lengths, dimensions */
            size_t w = rnd() % 2 ? 2 : 4;
            if (i + w <= n) memset(b + i, rnd() % 2 ? 0xFF : 0x00, w);
            break;
        }
        case 5: { /* duplicate a block */
            size_t len = 1 + (size_t)(rnd() % 64), j = (size_t)(rnd() % n);
            if (j + len <= n && n + len <= cap) {
                memmove(b + i + len, b + i, n - i);
                memcpy(b + i, b + (j < i ? j : j + len), len);
                n += len;
            }
            break;
        }
        default: { /* delete a block */
            size_t len = 1 + (size_t)(rnd() % 64);
            if (i + len < n) { memmove(b + i, b + i + len, n - i - len); n -= len; }
        }
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 2000;
    if (argc < 3 || iters <= 0) {
        fprintf(stderr, "usage: fuzz ITERATIONS SEED...\n");
        return 2;
    }
    int nseeds = argc - 2;
    uint8_t **seed = calloc((size_t)nseeds, sizeof *seed);
    size_t *seed_len = calloc((size_t)nseeds, sizeof *seed_len), maxlen = 0;
    for (int i = 0; i < nseeds; i++) {
        if (pm_read_file(argv[i + 2], &seed[i], &seed_len[i]) != PM_OK) {
            fprintf(stderr, "fuzz: cannot read %s\n", argv[i + 2]);
            return 2;
        }
        LLVMFuzzerTestOneInput(seed[i], seed_len[i]);
        if (seed_len[i] > maxlen) maxlen = seed_len[i];
    }
    size_t cap = maxlen + 4096;
    uint8_t *buf = malloc(cap);
    for (long it = 0; it < iters; it++) {
        int k = (int)(rnd() % (uint64_t)nseeds);
        memcpy(buf, seed[k], seed_len[k]);
        size_t n = mutate(buf, seed_len[k], cap);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("fuzz: %ld mutated inputs from %d seeds, no crashes\n", iters, nseeds);
    for (int i = 0; i < nseeds; i++) free(seed[i]);
    free(seed); free(seed_len); free(buf);
    return 0;
}
#endif
