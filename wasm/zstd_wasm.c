#include <stddef.h>
#include <stdint.h>

#include "../C/zstd/zstd.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define ZSTD_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define ZSTD_WASM_EXPORT
#endif

ZSTD_WASM_EXPORT size_t zstd_wasm_compress(const uint8_t *src,
                                           size_t src_size,
                                           uint8_t *dst,
                                           size_t dst_capacity,
                                           int level) {
  return ZSTD_compress(dst, dst_capacity, src, src_size, level);
}

ZSTD_WASM_EXPORT size_t zstd_wasm_compress_bound(size_t src_size) {
  return ZSTD_compressBound(src_size);
}

ZSTD_WASM_EXPORT size_t zstd_wasm_decompress(const uint8_t *src,
                                             size_t src_size,
                                             uint8_t *dst,
                                             size_t dst_capacity) {
  return ZSTD_decompress(dst, dst_capacity, src, src_size);
}

ZSTD_WASM_EXPORT unsigned long long zstd_wasm_get_frame_content_size(
    const uint8_t *src,
    size_t src_size) {
  return ZSTD_getFrameContentSize(src, src_size);
}

ZSTD_WASM_EXPORT unsigned zstd_wasm_is_error(size_t code) {
  return ZSTD_isError(code);
}

ZSTD_WASM_EXPORT const char *zstd_wasm_get_error_name(size_t code) {
  return ZSTD_getErrorName(code);
}
