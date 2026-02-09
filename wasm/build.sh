#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/wasm/dist"

mkdir -p "${OUT_DIR}"

emcc \
  -O3 \
  -I"${ROOT_DIR}/C/zstd" \
  -DZSTD_MULTITHREAD=0 \
  -DZSTD_DISABLE_ASM \
  -DZSTD_LEGACY_SUPPORT=0 \
  "${ROOT_DIR}/wasm/zstd_wasm.c" \
  "${ROOT_DIR}/C/zstd/entropy_common.c" \
  "${ROOT_DIR}/C/zstd/error_private.c" \
  "${ROOT_DIR}/C/zstd/fse_compress.c" \
  "${ROOT_DIR}/C/zstd/fse_decompress.c" \
  "${ROOT_DIR}/C/zstd/hist.c" \
  "${ROOT_DIR}/C/zstd/huf_compress.c" \
  "${ROOT_DIR}/C/zstd/huf_decompress.c" \
  "${ROOT_DIR}/C/zstd/zstd_common.c" \
  "${ROOT_DIR}/C/zstd/zstd_compress.c" \
  "${ROOT_DIR}/C/zstd/zstd_compress_literals.c" \
  "${ROOT_DIR}/C/zstd/zstd_compress_sequences.c" \
  "${ROOT_DIR}/C/zstd/zstd_compress_superblock.c" \
  "${ROOT_DIR}/C/zstd/zstd_ddict.c" \
  "${ROOT_DIR}/C/zstd/zstd_decompress.c" \
  "${ROOT_DIR}/C/zstd/zstd_decompress_block.c" \
  "${ROOT_DIR}/C/zstd/zstd_double_fast.c" \
  "${ROOT_DIR}/C/zstd/zstd_fast.c" \
  "${ROOT_DIR}/C/zstd/zstd_lazy.c" \
  "${ROOT_DIR}/C/zstd/zstd_ldm.c" \
  "${ROOT_DIR}/C/zstd/pool.c" \
  "${ROOT_DIR}/C/zstd/zstd_opt.c" \
  "${ROOT_DIR}/C/zstd/zstd_preSplit.c" \
  "${ROOT_DIR}/C/zstd/zstdmt_compress.c" \
  "${ROOT_DIR}/C/hashes/xxhash.c" \
  -s EXPORTED_FUNCTIONS="['_zstd_wasm_compress','_zstd_wasm_decompress','_zstd_wasm_get_frame_content_size','_zstd_wasm_is_error','_zstd_wasm_get_error_name','_malloc','_free']" \
  -s EXPORTED_RUNTIME_METHODS="['cwrap','getValue','setValue']" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=ZstdWasm \
  -s ENVIRONMENT=web,worker,node \
  -o "${OUT_DIR}/zstd_wasm.js"

echo "Built ${OUT_DIR}/zstd_wasm.js and ${OUT_DIR}/zstd_wasm.wasm"
