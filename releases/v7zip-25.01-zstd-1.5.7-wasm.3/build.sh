#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/wasm/dist"
EMSCRIPTEN_DIR="${EMSCRIPTEN_DIR:-${EMSDK:-${ROOT_DIR}/emsdk}/upstream/emscripten}"

mkdir -p "${OUT_DIR}"

EMCC_CMD=()
if command -v emcc >/dev/null 2>&1; then
  EMCC_CMD=("$(command -v emcc)")
elif [ -n "${EMSDK_PYTHON:-}" ] && [ -f "${EMSCRIPTEN_DIR}/emcc.py" ]; then
  # Git Bash on Windows often cannot resolve "emcc", but Python+emcc.py is reliable.
  EMCC_CMD=("${EMSDK_PYTHON}" "${EMSCRIPTEN_DIR}/emcc.py")
elif [ -f "${EMSCRIPTEN_DIR}/emcc.bat" ] && command -v cmd.exe >/dev/null 2>&1; then
  EMCC_CMD=("cmd.exe" "/c" "${EMSCRIPTEN_DIR}/emcc.bat")
else
  echo "Could not locate a working Emscripten compiler."
  echo "Tried: emcc on PATH, ${EMSCRIPTEN_DIR}/emcc.py with EMSDK_PYTHON, and ${EMSCRIPTEN_DIR}/emcc.bat."
  exit 1
fi

echo "Using Emscripten compiler command: ${EMCC_CMD[*]}"

"${EMCC_CMD[@]}" \
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
  "${ROOT_DIR}/wasm/7z_adapter.c" \
  "${ROOT_DIR}/C/7zArcIn.c" \
  "${ROOT_DIR}/C/7zStream.c" \
  "${ROOT_DIR}/C/7zFile.c" \
  "${ROOT_DIR}/C/7zBuf.c" \
  "${ROOT_DIR}/C/7zCrc.c" \
  "${ROOT_DIR}/C/7zCrcOpt.c" \
  "${ROOT_DIR}/C/Aes.c" \
  "${ROOT_DIR}/C/Sha256.c" \
  "${ROOT_DIR}/C/CpuArch.c" \
  "${ROOT_DIR}/C/Alloc.c" \
  "${ROOT_DIR}/C/7zDec.c" \
  "${ROOT_DIR}/C/7zAlloc.c" \
  "${ROOT_DIR}/C/zstd/zstd_preSplit.c" \
  "${ROOT_DIR}/C/zstd/zstdmt_compress.c" \
  "${ROOT_DIR}/C/hashes/xxhash.c" \
  "${ROOT_DIR}/C/LzmaDec.c" \
  "${ROOT_DIR}/C/Lzma2Dec.c" \
  "${ROOT_DIR}/C/LzmaEnc.c" \
  "${ROOT_DIR}/C/Lzma86Enc.c" \
  "${ROOT_DIR}/C/Bra.c" \
  "${ROOT_DIR}/C/Bra86.c" \
  "${ROOT_DIR}/C/Bcj2.c" \
  "${ROOT_DIR}/C/Delta.c" \
  -s EXPORTED_FUNCTIONS="['_zstd_wasm_compress','_zstd_wasm_compress_bound','_zstd_wasm_decompress','_zstd_wasm_get_frame_content_size','_zstd_wasm_is_error','_zstd_wasm_get_error_name','_wasm7z_open','_wasm7z_open_with_password','_wasm7z_close','_wasm7z_file_count','_wasm7z_fetch_name','_wasm7z_name_buffer','_wasm7z_name_length','_wasm7z_is_directory','_wasm7z_file_size','_wasm7z_extract','_wasm7z_has_encrypted_content','_malloc','_free']" \
  -s EXPORTED_RUNTIME_METHODS="['cwrap','getValue','setValue']" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=ZstdWasm \
  -s ENVIRONMENT=web,worker,node \
  -o "${OUT_DIR}/zstd_wasm.js"

echo "Built ${OUT_DIR}/zstd_wasm.js and ${OUT_DIR}/zstd_wasm.wasm"
