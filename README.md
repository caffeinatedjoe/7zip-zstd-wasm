# 7zip-zstd-wasm

This repository provides a WebAssembly build of the 7-Zip Zstandard Edition
(7-Zip ZS) core so you can:

- Open and extract `.7z` archives that use Zstandard compression.
- Repack files back into `.7z` with Zstandard compression.
- Use the bundled Zstandard codec directly from JS.

The WASM build lives in `wasm/`, with a working demo page in
`wasm/example/`.

## Releases

Release artifacts are stored in `releases/<version>/` and include:

- `zstd_wasm.js`
- `zstd_wasm.wasm`
- `build_info.json`
- `build.sh` (snapshot of the build script used)

Version naming follows:

`v7zip-<7zip_ver>-zstd-<zstd_ver>-wasm.<build>`

See `.pm/7z-wasm-release-versioning.md` for details.

## Example Page

The demo is in `wasm/example/index.html` and supports:

- Loading a `.7z` (Zstandard)
- Listing files and extracting individual entries
- Repacking into a new `.7z` using Zstandard level 3

Notes about the repack:

- Produces a non-solid archive (one pack stream per file)
- Preserves names and directory structure
- Does not preserve timestamps or file attributes

Serve it over HTTP (not `file://`). For example:

```bash
cd wasm/example
npx serve
```

Or use VSCode "Live Server" and open `wasm/example/index.html`.

## Using the WASM in Your App

The build output is in `wasm/dist/`.

Load the module and set `locateFile` so the WASM file resolves correctly:

```html
<script src="dist/zstd_wasm.js"></script>
<script>
  (async () => {
    const mod = await ZstdWasm({
      locateFile: (path) => `dist/${path}`
    });

    const compress = mod.cwrap('zstd_wasm_compress', 'number',
      ['number','number','number','number','number']);
    const decompress = mod.cwrap('zstd_wasm_decompress', 'number',
      ['number','number','number','number']);

    // Use mod._malloc / mod._free and HEAPU8 to pass buffers.
  })();
</script>
```

The example page in `wasm/example/app.js` shows the full end-to-end wiring
(including 7z archive open/extract and repack).

### Opening Encrypted Archives

- `wasm7z_open_with_password(ptr, len, password_utf8)` mirrors `wasm7z_open` but accepts a UTF-8 password string.  
- A return value of `0` still means success; the special error code `0x80100015` indicates an authentication failure (wrong password).
- `wasm7z_has_encrypted_content()` returns `1` when the archive contains 7z AES payload coders, so apps can enforce a password flow before extraction.

### Streaming Extraction API

For large entries, use the chunked extraction lifecycle:

- `wasm7z_extract_begin(index)`
- `wasm7z_extract_read(out_ptr, out_capacity, produced_ptr, done_ptr)`
- `wasm7z_extract_end()`

`read` writes up to `out_capacity` bytes per call, stores the count in `*produced_ptr`, and sets `*done_ptr` to `1` once the entry is fully emitted.

Deterministic streaming error codes:

- `10001`: invalid index
- `10002`: invalid extraction state
- `10003`: unsupported coder chain for streaming
- `10004`: decode failure
- `10005`: allocation failure
- `10006`: bad argument

Current streaming coverage is optimized for 7z folders with a single pack stream and a single main coder (`Copy` or `Zstd`). One-shot extraction (`wasm7z_extract`) remains unchanged for existing consumers.

## Build

### Prerequisites

- Emscripten SDK (emsdk)
- Bash (Git Bash on Windows is fine)

### Steps (Git Bash)

From repo root:

```bash
./wasm/run-build.sh
```

Output:

- `wasm/dist/zstd_wasm.js`
- `wasm/dist/zstd_wasm.wasm`

`run-build.sh` auto-installs/activates emsdk if needed and then calls the build.

If you do not have emsdk yet:

```bash
git clone https://github.com/emscripten-core/emsdk.git emsdk
cd emsdk
python emsdk.py install latest
python emsdk.py activate latest
cd ..
```

## Repository Layout

- `wasm/` - WASM build wrapper and 7z adapter glue
- `wasm/example/` - Demo page and UI
- `wasm/dist/` - Build output
- `releases/` - Versioned artifacts with build metadata

## License

Same as upstream 7-Zip (GNU LGPL v2.1-or-later). See `COPYING`.
