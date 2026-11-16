/* The single translation unit that instantiates the vendored stb libraries,
 * plus thin shims so no other file depends on stb's private entry points.
 * Built with warnings disabled (see Makefile). */
#include "pmark.h"
#include "pm_internal.h"

#include <stdlib.h>

#define STBI_NO_HDR
#define STBI_NO_LINEAR
/* stb checks this per dimension while parsing headers; the total-pixel
 * limit is enforced by pm_image_decode via stbi_info before decoding. */
#define STBI_MAX_DIMENSIONS PM_MAX_DIM
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

uint8_t *pm_stb_png(const uint8_t *px, int stride, int w, int h, int comp, int *len)
{
    return stbi_write_png_to_mem(px, stride, w, h, comp, len);
}

static void buf_sink(void *ctx, void *data, int size)
{
    pm_buf_put((pm_buf *)ctx, data, (size_t)size);
}

bool pm_stb_jpg(pm_buf *out, const uint8_t *rgb, int w, int h, int quality)
{
    return stbi_write_jpg_to_func(buf_sink, out, w, h, 3, rgb, quality) && !out->oom;
}

uint8_t *pm_stb_zlib_compress(const uint8_t *data, int len, int *out_len)
{
    return stbi_zlib_compress((unsigned char *)data, len, out_len, 8);
}

uint8_t *pm_stb_zlib_decompress(const uint8_t *data, int len, int *out_len)
{
    return (uint8_t *)stbi_zlib_decode_malloc((const char *)data, len, out_len);
}
