/* A C entry point for ZBIC decompression (Atmosphere's zstd with ZSTD_ZBIC_SUPPORT), loaded by nx2/zbic.py with
 * ctypes. Returns the decompressed size, or -1 (bad data or the size is not in the frame header), or -2 (the
 * output buffer is too small or the data is corrupt). */
#include <stddef.h>
#include "zstd.h"

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

API long long zbic_content_size(const void *src, size_t n) {
    unsigned long long s = ZSTD_getFrameContentSize(src, n);
    if (s == ZSTD_CONTENTSIZE_ERROR || s == ZSTD_CONTENTSIZE_UNKNOWN) return -1;
    return (long long)s;
}

API long long zbic_decompress(void *dst, size_t cap, const void *src, size_t n) {
    size_t r = ZSTD_decompress(dst, cap, src, n);
    if (ZSTD_isError(r)) return -2;
    return (long long)r;
}
