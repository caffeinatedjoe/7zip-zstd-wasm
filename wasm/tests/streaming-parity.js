const fs = require("node:fs");
const path = require("node:path");

async function loadModule() {
  const distDir = path.resolve(__dirname, "..", "dist");
  const factory = require(path.join(distDir, "zstd_wasm.js"));
  return factory({
    locateFile: (name) => path.join(distDir, name),
  });
}

function assertEqualBytes(a, b, message) {
  if (a.length !== b.length) {
    throw new Error(`${message}: length mismatch ${a.length} != ${b.length}`);
  }
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      throw new Error(`${message}: byte mismatch at index ${i}`);
    }
  }
}

function openArchive(mod, wasm7z, archiveBytes) {
  const ptr = mod._malloc(archiveBytes.length || 1);
  if (!ptr) {
    throw new Error("malloc failed for archive");
  }
  for (let i = 0; i < archiveBytes.length; i++) {
    mod.setValue(ptr + i, archiveBytes[i], "i8");
  }
  const res = wasm7z.open(ptr, archiveBytes.length);
  if (res !== 0) {
    mod._free(ptr);
    throw new Error(`wasm7z_open failed: ${res}`);
  }
  return ptr;
}

function getFirstFileEntry(wasm7z) {
  const count = wasm7z.fileCount();
  for (let i = 0; i < count; i++) {
    if (!wasm7z.isDirectory(i)) {
      return { index: i, size: wasm7z.fileSize(i) };
    }
  }
  throw new Error("archive contains no file entries");
}

function oneShotExtract(mod, wasm7z, entry) {
  const capacity = entry.size > 0 ? entry.size : 1;
  const dstPtr = mod._malloc(capacity);
  const outSizePtr = mod._malloc(4);
  if (!dstPtr || !outSizePtr) {
    if (dstPtr) mod._free(dstPtr);
    if (outSizePtr) mod._free(outSizePtr);
    throw new Error("malloc failed for one-shot extraction");
  }
  mod.setValue(outSizePtr, 0, "i32");
  const res = wasm7z.extract(entry.index, dstPtr, capacity, outSizePtr);
  if (res !== 0) {
    mod._free(dstPtr);
    mod._free(outSizePtr);
    throw new Error(`wasm7z_extract failed: ${res}`);
  }
  const outSize = mod.getValue(outSizePtr, "i32") >>> 0;
  const out = new Uint8Array(outSize);
  for (let i = 0; i < outSize; i++) {
    out[i] = mod.getValue(dstPtr + i, "i8") & 0xff;
  }
  mod._free(dstPtr);
  mod._free(outSizePtr);
  return out;
}

function streamingExtract(mod, wasm7z, entry) {
  const chunkSize = 64 * 1024;
  const chunkPtr = mod._malloc(chunkSize);
  const producedPtr = mod._malloc(4);
  const donePtr = mod._malloc(4);
  const chunks = [];
  let started = false;
  if (!chunkPtr || !producedPtr || !donePtr) {
    if (chunkPtr) mod._free(chunkPtr);
    if (producedPtr) mod._free(producedPtr);
    if (donePtr) mod._free(donePtr);
    throw new Error("malloc failed for streaming extraction");
  }
  try {
    const beginRes = wasm7z.extractBegin(entry.index);
    if (beginRes !== 0) {
      throw new Error(`wasm7z_extract_begin failed: ${beginRes}`);
    }
    started = true;
    for (;;) {
      mod.setValue(producedPtr, 0, "i32");
      mod.setValue(donePtr, 0, "i32");
      const readRes = wasm7z.extractRead(chunkPtr, chunkSize, producedPtr, donePtr);
      if (readRes !== 0) {
        throw new Error(`wasm7z_extract_read failed: ${readRes}`);
      }
      const produced = mod.getValue(producedPtr, "i32") >>> 0;
      if (produced > 0) {
        const chunk = new Uint8Array(produced);
        for (let i = 0; i < produced; i++) {
          chunk[i] = mod.getValue(chunkPtr + i, "i8") & 0xff;
        }
        chunks.push(chunk);
      }
      const done = mod.getValue(donePtr, "i32") | 0;
      if (done !== 0) {
        break;
      }
    }
    const total = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
    const out = new Uint8Array(total);
    let cursor = 0;
    for (const chunk of chunks) {
      out.set(chunk, cursor);
      cursor += chunk.length;
    }
    return out;
  } finally {
    if (started) {
      const endRes = wasm7z.extractEnd();
      if (endRes !== 0) {
        throw new Error(`wasm7z_extract_end failed: ${endRes}`);
      }
    }
    mod._free(chunkPtr);
    mod._free(producedPtr);
    mod._free(donePtr);
  }
}

async function verifyFixture(mod, wasm7z, fixturePath) {
  const archiveBytes = new Uint8Array(fs.readFileSync(fixturePath));
  const archivePtr = openArchive(mod, wasm7z, archiveBytes);
  try {
    const entry = getFirstFileEntry(wasm7z);
    const expected = oneShotExtract(mod, wasm7z, entry);
    const streamed = streamingExtract(mod, wasm7z, entry);
    assertEqualBytes(expected, streamed, `parity failed for ${path.basename(fixturePath)}`);
    for (let i = 0; i < 3; i++) {
      const repeated = streamingExtract(mod, wasm7z, entry);
      assertEqualBytes(expected, repeated, `repeat cycle ${i + 1} failed for ${path.basename(fixturePath)}`);
    }
    console.log(`OK: ${path.basename(fixturePath)} (${expected.length} bytes)`);
  } finally {
    wasm7z.close();
    mod._free(archivePtr);
  }
}

async function main() {
  const mod = await loadModule();
  const wasm7z = {
    open: mod.cwrap("wasm7z_open", "number", ["number", "number"]),
    close: mod.cwrap("wasm7z_close", "void", []),
    fileCount: mod.cwrap("wasm7z_file_count", "number", []),
    isDirectory: mod.cwrap("wasm7z_is_directory", "number", ["number"]),
    fileSize: mod.cwrap("wasm7z_file_size", "number", ["number"]),
    extract: mod.cwrap("wasm7z_extract", "number", ["number", "number", "number", "number"]),
    extractBegin: mod.cwrap("wasm7z_extract_begin", "number", ["number"]),
    extractRead: mod.cwrap("wasm7z_extract_read", "number", ["number", "number", "number", "number"]),
    extractEnd: mod.cwrap("wasm7z_extract_end", "number", []),
  };

  const fixtures = [
    path.resolve(__dirname, "..", "..", "tests", "regr-arc", "test.txt.zstd.7z"),
    path.resolve(__dirname, "..", "..", "tests", "regr-arc", "test-sol.zstd.7z"),
  ];
  for (const fixture of fixtures) {
    await verifyFixture(mod, wasm7z, fixture);
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
