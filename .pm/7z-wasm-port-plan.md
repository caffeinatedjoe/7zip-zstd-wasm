# Porting 7-Zip Zstandard archive support to WebAssembly

## Goal
Enable the WASM demo to open `.7z` archives created by 7-Zip ZS with the **Zstandard** coder, list their entries, extract arbitrary files, and recompress them back into the same `.7z` format with the familiar Zstandard compression level/filters. The browser-side module must handle both the `.7z` container and the Zstd payloads so the UI can truly behave like a single-page 7-Zip client.

## What must be ported
1. **Archive container logic**
   * `C/7zArcIn.c`, `7zStream.c`, `7zBuf.c`, `7zTypes.h`, `7zDec.c` – these files parse the 7z headers, enumerate folders and coders, and expose the iterator state that the rest of the stack relies on. WASM needs the same data structures so it can list entries and understand how to feed compressed streams to Zstd.
   * `7zFile.c`, `7zCrc.c`, `7zAlloc.c/h` – these helpers provide stream reading/writing, CRC calculators, and allocation abstractions which the online runtime must reimplement or adapt for memory buffers instead of disk files.
   * Key data structures: `CSzAr`, `CArchiveDatabase`, `CFileItem`, `CFolder`, `CFileSize`, `CStream` (present in `7zArcIn/7zStream`).

2. **Coder pipeline and filters**

2. **Coder pipeline and filters**
   * `7zDec.c`/`7zEnc.c` orchestrate coder chains (BCJ/BCJ2, Delta, LZMA, Zstd). Even if only Zstd + BCJ are used, you need the pipeline logic plus any filters that might appear in archives (copied from `BsFilter`, `Bcj2`, etc.) so the browser can take a given folder’s coders and decode the correct stream.  
   * The existing Zstd WASM module fulfills the codec requirement, but it must be callable from the 7z pipeline as the last coder in each folder.
   * **Linking dependencies:** make sure to include `LzmaDec.c`, `Lzma2Dec.c`, `LzmaEnc.c`, `Lzma86Enc.c`, `Bcj2.c`, `Delta.c`, `LzmaAlloc.c`, and other support helpers so the decoder symbols resolve in the WASM build.

3. **Handler-level APIs**

3. **Handler-level APIs**
   * `CPP/Archive/7z/7zIn.c`, `7zOut.cpp`, `7zUpdate.cpp`, `7zHandlerOut.cpp` – these expose clean entry points (open archive, list entries, extract file, add files, write new archive) and handle metadata such as file properties (size, CRC, timestamps, attributes). WASM needs equivalents so the JS layer can call `listFiles()`, `extract(entry)`, and `createArchive({ files, settings })`.
   * Key functions:
     - `SzArEx_Open`: initializes `CSzArEx` from an `ILookInStream` and reads headers into `CArchiveDatabase`.
     - `SzArEx_Extract`: extracts one file/stream from `CSzArEx`, following folder coder chains to produce decompressed bytes.
     - `SzArEx_Free`: release memory (required for repeated use).
     - `SzAr_CreateStream`: reconstructs coders for encoding (used by `7zOut`/`7zUpdate`).
     - `SzArEx_Write`: there isn’t a single C function for writing, but `WzArchive_Impl` in `7zOut.cpp` drives `SzAr_Write`. Focus on the writer logic there.

4. **Compression settings + metadata**
   * `7zUpdate.cpp`, `7zOut.cpp` and associated property serialization logic determine how the `.7z` header records compression levels, filters, and dictionary sizes. The WASM rewriter must emit these exactly so re-zipping honors the original settings (including optional BCJ+Zstd combinations).

5. **Re-encoding pipeline**
   * The writer path (`7zOut`, `7zUpdate`, `Format7z`) must be ported so that, after editing files in JS, the module can build valid `.7z` structures, reapply coder chains, write header metadata, and include CRCs. Emitting the final byte stream will re-enable downloads of the reconstructed archive.

## How to use the ported pieces
1. Build the WASM module so it exposes APIs such as `openArchive(buffer)`, `listEntries()`, `extract(entryIndex)`, `updateArchive(fileList, settings)`, and `writeArchive()` (returning a `Uint8Array`). Keep the existing Zstd exports and wrap the new 7z logic around them.
2. Add JS helpers that:
   * Accept a `.7z` `File` object, feed it to `openArchive`, show the list of entries.
   * Allocate buffers for `extract`, decompress via the new pipeline, then display the contents.
   * Call `updateArchive` and `writeArchive` with UI-provided replacements, generating a downloadable `.7z`.

3. Keep the existing compression UI intact so the Zstd encoder is still reachable when users just want raw compression/decompression, but fold the `.7z` functionality into the same runtime so both workflows reuse the WASM heap/coders.

## Next steps for agentic completion
1. Create a new `wasm/7z/` build target that compiles the above C/C++ sources alongside the existing `zstd` files using Emscripten, exposing the new archive APIs.
2. Build JS bindings that translate between JS objects and the C structs used by `7zIn`/`7zOut`.
3. Extend `wasm/example` to:
   * Accept `.7z` uploads.
   * List entries with metadata.
   * Allow downloading extracted files and writing edited files back into a `.7z`.
4. Validate the result by opening a `.7z` archive created with 7-Zip ZS (Zstandard compression) and ensuring the archive can be read, modified, and regenerated with the same compression method.
