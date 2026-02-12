#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export EMSDK_DIR="${ROOT_DIR}/emsdk"
EMSDK_PY="${EMSDK_DIR}/emsdk"
EMSDK_ENV="${EMSDK_DIR}/emsdk_env.sh"
EMSDK_TOOLCHAIN="latest"
EMCC_BAT="${EMSDK_DIR}/upstream/emscripten/emcc.bat"
export EMSDK_QUIET="${EMSDK_QUIET:-1}"

if [ ! -f "${EMSDK_ENV}" ]; then
  echo "emsdk environment script missing at ${EMSDK_ENV}"
  exit 1
fi

if [ ! -x "${EMSDK_PY}" ]; then
  echo "emsdk launcher missing or not executable: ${EMSDK_PY}"
  exit 1
fi

if [ ! -f "${EMCC_BAT}" ]; then
  echo "emsdk toolchain missing; installing ${EMSDK_TOOLCHAIN}."
  "${EMSDK_PY}" install "${EMSDK_TOOLCHAIN}"
  "${EMSDK_PY}" activate "${EMSDK_TOOLCHAIN}"
fi

source "${EMSDK_ENV}"

exec "${ROOT_DIR}/wasm/build.sh"
