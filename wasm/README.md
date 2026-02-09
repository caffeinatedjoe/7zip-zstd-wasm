# WASM build for Zstandard

This folder provides a minimal WebAssembly build of the bundled Zstandard
library. It exposes a small C API intended for JavaScript/TypeScript bindings.

## Exposed functions

The `zstd_wasm.c` wrapper exports these symbols:

- `zstd_wasm_compress(src, src_size, dst, dst_capacity, level)`
- `zstd_wasm_decompress(src, src_size, dst, dst_capacity)`
- `zstd_wasm_get_frame_content_size(src, src_size)`
- `zstd_wasm_is_error(code)`
- `zstd_wasm_get_error_name(code)`

## Build

You need [Emscripten](https://emscripten.org/) installed and in your PATH.

```bash
./wasm/build.sh
```

The output artifacts are written to `wasm/dist/`:

- `zstd_wasm.js`
- `zstd_wasm.wasm`

## Usage (JavaScript)

```js
import ZstdWasm from "./dist/zstd_wasm.js";

const module = await ZstdWasm();
const compress = module.cwrap("zstd_wasm_compress", "number", [
  "number",
  "number",
  "number",
  "number",
  "number",
]);
```

Allocate buffers with `module._malloc` and free them with `module._free`.
Use `zstd_wasm_get_frame_content_size` to size the destination buffer when
possible and `zstd_wasm_is_error` + `zstd_wasm_get_error_name` for error
handling.

## Example page

Build the WASM bundle and serve the `wasm/` directory:

```bash
./wasm/build.sh
python3 -m http.server --directory wasm 8000
```

Then open `http://localhost:8000/example/` to try the demo UI.
