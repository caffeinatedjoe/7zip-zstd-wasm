const statusEl = document.getElementById("status");
const runButton = document.getElementById("run");
const inputEl = document.getElementById("input");
const inputBytesEl = document.getElementById("input-bytes");
const compressedBytesEl = document.getElementById("compressed-bytes");
const ratioEl = document.getElementById("ratio");
const roundtripEl = document.getElementById("roundtrip");
const fileInput = document.getElementById("file-input");
const fileStatus = document.getElementById("file-status");
const fileList = document.getElementById("file-list");
const fileOutput = document.getElementById("file-output");
const downloadLink = document.getElementById("file-download");
const rezipButton = document.getElementById("rezip-button");
const rezipStatus = document.getElementById("rezip-status");
const rezipDownload = document.getElementById("rezip-download");
const passwordInput = document.getElementById("archive-password");
const passwordHint = document.getElementById("password-hint");

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const utf8Decoder = new TextDecoder();
const utf16Decoder = new TextDecoder("utf-16le");
const SZ_ERROR_WRONG_PASSWORD = 0x80100015;
const SZ_ERROR_ENCRYPTION_UNSUPPORTED = 0x80100016;

let moduleInstance;
let compressFn;
let compressBoundFn;
let decompressFn;
let frameSizeFn;
let isErrorFn;
let errorNameFn;
let wasm7z;
let heapBuffer = null;
let hasMemoryViews = false;
const archiveState = {
  ptr: 0,
  size: 0,
  name: "",
  entries: [],
  downloadUrl: null,
  rezipUrl: null,
};

function formatRatio(inputSize, compressedSize) {
  if (!inputSize || !compressedSize) {
    return "-";
  }
  return `${(compressedSize / inputSize).toFixed(2)}x`;
}

function setStatus(message) {
  statusEl.textContent = message;
}

function updatePasswordHint() {
  if (!passwordHint || !passwordInput) return;
  const trimmed = passwordInput.value.trim();
  passwordHint.textContent = trimmed
    ? "Using the supplied password to unlock the archive."
    : "Leave the field empty when opening unencrypted archives.";
}

function getWasmMemory() {
  if (!moduleInstance) {
    return null;
  }
  const direct =
    moduleInstance.wasmMemory ||
    (moduleInstance.asm && moduleInstance.asm.memory) ||
    (moduleInstance.exports && moduleInstance.exports.memory);
  if (direct) {
    return direct;
  }
  const keys = Object.getOwnPropertyNames(moduleInstance);
  for (const key of keys) {
    const value = moduleInstance[key];
    if (value && value.buffer instanceof ArrayBuffer) {
      return value;
    }
  }
  return null;
}

function ensureHeaps() {
  if (moduleInstance && moduleInstance.HEAPU8 && moduleInstance.HEAPU16) {
    hasMemoryViews = true;
    return true;
  }
  const memory = getWasmMemory();
  if (!memory || !memory.buffer) {
    hasMemoryViews = false;
    return false;
  }
  if (memory.buffer !== heapBuffer) {
    heapBuffer = memory.buffer;
    moduleInstance.HEAPU8 = new Uint8Array(heapBuffer);
    moduleInstance.HEAPU16 = new Uint16Array(heapBuffer);
    moduleInstance.HEAPU32 = new Uint32Array(heapBuffer);
  }
  hasMemoryViews = true;
  return true;
}

function writeBytes(ptr, bytes) {
  if (ensureHeaps()) {
    moduleInstance.HEAPU8.set(bytes, ptr);
    return;
  }
  for (let i = 0; i < bytes.length; i++) {
    moduleInstance.setValue(ptr + i, bytes[i], "i8");
  }
}

function readBytes(ptr, length) {
  const out = new Uint8Array(length);
  if (ensureHeaps()) {
    out.set(moduleInstance.HEAPU8.subarray(ptr, ptr + length));
    return out;
  }
  for (let i = 0; i < length; i++) {
    out[i] = moduleInstance.getValue(ptr + i, "i8") & 0xff;
  }
  return out;
}

function readUtf16(ptr, length) {
  const data = new Uint16Array(length);
  if (ensureHeaps()) {
    data.set(moduleInstance.HEAPU16.subarray(ptr >> 1, (ptr >> 1) + length));
    return utf16Decoder.decode(data);
  }
  for (let i = 0; i < length; i++) {
    data[i] = moduleInstance.getValue(ptr + i * 2, "i16") & 0xffff;
  }
  return utf16Decoder.decode(data);
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) {
    return "-";
  }
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  const units = ["KB", "MB", "GB"];
  let value = bytes / 1024;
  let unit = units.shift();
  while (value >= 1024 && units.length) {
    value /= 1024;
    unit = units.shift();
  }
  return `${value.toFixed(2)} ${unit}`;
}

function resetDownload() {
  if (archiveState.downloadUrl) {
    URL.revokeObjectURL(archiveState.downloadUrl);
    archiveState.downloadUrl = null;
  }
  downloadLink.hidden = true;
  downloadLink.href = "#";
  downloadLink.textContent = "Download extracted file";
}

function resetRezip() {
  if (archiveState.rezipUrl) {
    URL.revokeObjectURL(archiveState.rezipUrl);
    archiveState.rezipUrl = null;
  }
  if (rezipDownload) {
    rezipDownload.hidden = true;
    rezipDownload.href = "#";
    rezipDownload.textContent = "Download .7z";
  }
}

function resetArchiveState() {
  if (archiveState.ptr && wasm7z) {
    wasm7z.close();
    moduleInstance._free(archiveState.ptr);
  }
  archiveState.ptr = 0;
  archiveState.size = 0;
  archiveState.name = "";
  archiveState.entries = [];
  resetDownload();
  resetRezip();
  if (rezipStatus) {
    rezipStatus.textContent = "Repack requires a loaded archive.";
  }
  if (rezipButton) {
    rezipButton.disabled = true;
  }
}

function looksLikeText(bytes) {
  const sampleSize = Math.min(bytes.length, 4096);
  if (!sampleSize) {
    return true;
  }
  let weird = 0;
  for (let i = 0; i < sampleSize; i++) {
    const value = bytes[i];
    if (value === 0) {
      return false;
    }
    if (value < 9 || (value > 13 && value < 32)) {
      weird++;
    }
  }
  return weird / sampleSize < 0.1;
}

function renderArchiveList(entries) {
  fileList.textContent = "";
  if (!entries.length) {
    fileList.textContent = "Archive contains no entries.";
    return;
  }

  entries.forEach((entry) => {
    const row = document.createElement("div");
    row.className = `file-row${entry.isDir ? " is-dir" : ""}`;

    const name = document.createElement("div");
    name.className = "file-name";
    name.textContent = entry.name || "(unnamed)";

    const size = document.createElement("div");
    size.className = "file-size";
    size.textContent = entry.isDir ? "Directory" : formatBytes(entry.size);

    const button = document.createElement("button");
    button.type = "button";
    button.textContent = entry.isDir ? "Folder" : "Extract";
    button.disabled = entry.isDir;
    if (!entry.isDir) {
      button.addEventListener("click", () => extractEntry(entry));
    }

    row.appendChild(name);
    row.appendChild(size);
    row.appendChild(button);
    fileList.appendChild(row);
  });
}

function extractEntry(entry) {
  if (!moduleInstance || !wasm7z || !archiveState.ptr) {
    fileStatus.textContent = "No archive loaded.";
    return;
  }
  if (!ensureHeaps()) {
    fileStatus.textContent = "WASM memory views unavailable. Extraction may be slower.";
  }

  resetDownload();
  fileStatus.textContent = `Extracting ${entry.name}...`;

  const capacity = entry.size > 0 ? entry.size : 1;
  const dstPtr = moduleInstance._malloc(capacity);
  const sizePtr = moduleInstance._malloc(4);

  if (!dstPtr || !sizePtr) {
    fileStatus.textContent = "Unable to allocate memory for extraction.";
    if (dstPtr) moduleInstance._free(dstPtr);
    if (sizePtr) moduleInstance._free(sizePtr);
    return;
  }

  moduleInstance.setValue(sizePtr, 0, "i32");
  const result = wasm7z.extract(entry.index, dstPtr, capacity, sizePtr);
  if (result !== 0) {
    if (result === SZ_ERROR_ENCRYPTION_UNSUPPORTED) {
      fileStatus.textContent =
        "Extraction failed: encrypted 7z payload (7zAES) is not supported in this WASM build.";
    } else {
      fileStatus.textContent = `Extraction failed (${result}).`;
    }
    moduleInstance._free(dstPtr);
    moduleInstance._free(sizePtr);
    return;
  }

  const actualSize = moduleInstance.getValue(sizePtr, "i32") >>> 0;
  const resultBytes = readBytes(dstPtr, actualSize);

  moduleInstance._free(dstPtr);
  moduleInstance._free(sizePtr);

  const previewBytes = resultBytes.subarray(0, 64 * 1024);
  if (looksLikeText(previewBytes)) {
    const text = utf8Decoder.decode(previewBytes);
    fileOutput.textContent =
      previewBytes.length < resultBytes.length
        ? `${text}\n\n... (preview truncated)`
        : text;
  } else {
    fileOutput.textContent = "(binary file, use download to view)";
  }

  const blob = new Blob([resultBytes], { type: "application/octet-stream" });
  archiveState.downloadUrl = URL.createObjectURL(blob);
  downloadLink.href = archiveState.downloadUrl;
  downloadLink.download = entry.name.split("/").pop() || entry.name;
  downloadLink.hidden = false;
  downloadLink.textContent = `Download ${entry.name}`;
  fileStatus.textContent = `Extracted ${entry.name} (${formatBytes(actualSize)})`;
}

function extractEntryBytes(entry) {
  const capacity = entry.size > 0 ? entry.size : 1;
  const dstPtr = moduleInstance._malloc(capacity);
  const sizePtr = moduleInstance._malloc(4);

  if (!dstPtr || !sizePtr) {
    if (dstPtr) moduleInstance._free(dstPtr);
    if (sizePtr) moduleInstance._free(sizePtr);
    throw new Error("Unable to allocate memory for extraction.");
  }

  moduleInstance.setValue(sizePtr, 0, "i32");
  const result = wasm7z.extract(entry.index, dstPtr, capacity, sizePtr);
  if (result !== 0) {
    moduleInstance._free(dstPtr);
    moduleInstance._free(sizePtr);
    if (result === SZ_ERROR_ENCRYPTION_UNSUPPORTED) {
      throw new Error("Extraction failed: encrypted 7z payload (7zAES) is not supported in this WASM build.");
    }
    throw new Error(`Extraction failed (${result}).`);
  }

  const actualSize = moduleInstance.getValue(sizePtr, "i32") >>> 0;
  const resultBytes = readBytes(dstPtr, actualSize);

  moduleInstance._free(dstPtr);
  moduleInstance._free(sizePtr);
  return resultBytes;
}

function compressZstd(srcBytes, level) {
  const srcPtr = moduleInstance._malloc(srcBytes.length || 1);
  if (!srcPtr) {
    throw new Error("Unable to allocate memory for compression.");
  }
  writeBytes(srcPtr, srcBytes);

  const bound = compressBoundFn
    ? compressBoundFn(srcBytes.length)
    : srcBytes.length + Math.ceil(srcBytes.length / 8) + 256;
  const dstPtr = moduleInstance._malloc(bound || 1);
  if (!dstPtr) {
    moduleInstance._free(srcPtr);
    throw new Error("Unable to allocate output buffer for compression.");
  }

  const compressedSize = compressFn(
    srcPtr,
    srcBytes.length,
    dstPtr,
    bound,
    level,
  );

  if (isErrorFn(compressedSize)) {
    const message = errorNameFn(compressedSize);
    moduleInstance._free(srcPtr);
    moduleInstance._free(dstPtr);
    throw new Error(`Compression failed: ${message}`);
  }

  const compressed = readBytes(dstPtr, compressedSize);
  moduleInstance._free(srcPtr);
  moduleInstance._free(dstPtr);
  return compressed;
}

let crcTable = null;
function makeCrcTable() {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let j = 0; j < 8; j++) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    table[i] = c >>> 0;
  }
  return table;
}

function crc32(bytes) {
  if (!crcTable) {
    crcTable = makeCrcTable();
  }
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    const byte = bytes[i];
    crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function encodeUtf16LE(text) {
  const out = new Uint8Array((text.length + 1) * 2);
  let offset = 0;
  for (let i = 0; i < text.length; i++) {
    const code = text.charCodeAt(i);
    out[offset++] = code & 0xff;
    out[offset++] = (code >> 8) & 0xff;
  }
  out[offset++] = 0;
  out[offset++] = 0;
  return out.subarray(0, offset);
}

function writeNumber(value, out) {
  let v = BigInt(value);
  if (v < 0n) {
    v = 0n;
  }
  if (v < 0x80n) {
    out.push(Number(v));
    return;
  }
  let n = 1;
  for (; n < 9; n++) {
    const totalBits = 7n + 7n * BigInt(n);
    if (v < (1n << totalBits)) {
      break;
    }
  }
  const lowBits = v & ((1n << (8n * BigInt(n))) - 1n);
  const highPart = v >> (8n * BigInt(n));
  const prefix = (0xff << (8 - n)) & 0xff;
  out.push(prefix | Number(highPart));
  for (let i = 0; i < n; i++) {
    out.push(Number((lowBits >> (8n * BigInt(i))) & 0xffn));
  }
}

function writeUInt32LE(value, out, offset) {
  out[offset] = value & 0xff;
  out[offset + 1] = (value >>> 8) & 0xff;
  out[offset + 2] = (value >>> 16) & 0xff;
  out[offset + 3] = (value >>> 24) & 0xff;
}

function writeUInt64LE(value, out, offset) {
  let v = BigInt(value);
  for (let i = 0; i < 8; i++) {
    out[offset + i] = Number(v & 0xffn);
    v >>= 8n;
  }
}

function makeBitVector(numBits, predicate) {
  const numBytes = (numBits + 7) >> 3;
  const bytes = new Uint8Array(numBytes);
  for (let i = 0; i < numBits; i++) {
    if (predicate(i)) {
      bytes[i >> 3] |= 0x80 >> (i & 7);
    }
  }
  return bytes;
}

function build7zHeader(options) {
  const {
    packSizes,
    unpackSizes,
    fileNames,
    emptyStreamBits,
    emptyFileBits,
  } = options;
  const out = [];
  const pushByte = (b) => out.push(b & 0xff);
  const pushBytes = (bytes) => {
    for (let i = 0; i < bytes.length; i++) {
      out.push(bytes[i] & 0xff);
    }
  };

  const k7zIdEnd = 0x00;
  const k7zIdHeader = 0x01;
  const k7zIdMainStreamsInfo = 0x04;
  const k7zIdFilesInfo = 0x05;
  const k7zIdPackInfo = 0x06;
  const k7zIdUnpackInfo = 0x07;
  const k7zIdSize = 0x09;
  const k7zIdFolder = 0x0b;
  const k7zIdCodersUnpackSize = 0x0c;
  const k7zIdEmptyStream = 0x0e;
  const k7zIdEmptyFile = 0x0f;
  const k7zIdName = 0x11;

  pushByte(k7zIdHeader);
  pushByte(k7zIdMainStreamsInfo);

  pushByte(k7zIdPackInfo);
  writeNumber(0, out);
  writeNumber(packSizes.length, out);
  pushByte(k7zIdSize);
  packSizes.forEach((size) => writeNumber(size, out));
  pushByte(k7zIdEnd);

  pushByte(k7zIdUnpackInfo);
  pushByte(k7zIdFolder);
  writeNumber(unpackSizes.length, out);
  pushByte(0);

  for (let i = 0; i < unpackSizes.length; i++) {
    writeNumber(1, out);
    pushByte(0x24);
    pushBytes([0x04, 0xf7, 0x11, 0x01]);
    writeNumber(5, out);
    pushBytes([1, 5, 3, 0, 0]);
  }

  pushByte(k7zIdCodersUnpackSize);
  unpackSizes.forEach((size) => writeNumber(size, out));
  pushByte(k7zIdEnd);

  pushByte(k7zIdEnd);

  pushByte(k7zIdFilesInfo);
  writeNumber(fileNames.length, out);

  const nameChunks = fileNames.map(encodeUtf16LE);
  const namesSize = nameChunks.reduce((sum, chunk) => sum + chunk.length, 0);
  pushByte(k7zIdName);
  writeNumber(namesSize + 1, out);
  pushByte(0);
  nameChunks.forEach((chunk) => pushBytes(chunk));

  if (emptyStreamBits && emptyStreamBits.length) {
    pushByte(k7zIdEmptyStream);
    writeNumber(emptyStreamBits.length, out);
    pushBytes(emptyStreamBits);
    if (emptyFileBits && emptyFileBits.length) {
      pushByte(k7zIdEmptyFile);
      writeNumber(emptyFileBits.length, out);
      pushBytes(emptyFileBits);
    }
  }

  pushByte(k7zIdEnd);
  pushByte(k7zIdEnd);

  return Uint8Array.from(out);
}

async function rezipArchive() {
  if (!moduleInstance || !wasm7z || !archiveState.entries.length) {
    if (rezipStatus) {
      rezipStatus.textContent = "Repack requires a loaded archive.";
    }
    return;
  }
  if (!ensureHeaps() && rezipStatus) {
    rezipStatus.textContent = "WASM memory views unavailable. Repack may be slower.";
  }

  resetRezip();
  if (rezipButton) {
    rezipButton.disabled = true;
  }
  if (rezipStatus) {
    rezipStatus.textContent = "Extracting files for repack...";
  }

  const entries = archiveState.entries;
  const fileNames = entries.map((entry) => entry.name);
  const dataFiles = [];
  const emptyStreamEntries = [];

  for (const entry of entries) {
    if (entry.isDir || entry.size === 0) {
      emptyStreamEntries.push(entry);
      continue;
    }
    dataFiles.push(entry);
  }

  if (!dataFiles.length) {
    if (rezipStatus) {
      rezipStatus.textContent = "No non-empty files to repack.";
    }
    if (rezipButton) {
      rezipButton.disabled = false;
    }
    return;
  }

  const packSizes = [];
  const unpackSizes = [];
  const compressedChunks = [];

  for (let i = 0; i < dataFiles.length; i++) {
    const entry = dataFiles[i];
    if (rezipStatus) {
      rezipStatus.textContent = `Extracting ${entry.name}...`;
    }
    const bytes = extractEntryBytes(entry);
    if (rezipStatus) {
      rezipStatus.textContent = `Compressing ${entry.name} (Zstd lvl 3)...`;
    }
    const compressed = compressZstd(bytes, 3);
    compressedChunks.push(compressed);
    packSizes.push(compressed.length);
    unpackSizes.push(bytes.length);

    if (i % 2 === 0) {
      await new Promise((resolve) => setTimeout(resolve, 0));
    }
  }

  const totalPacked = packSizes.reduce((sum, size) => sum + size, 0);
  const packData = new Uint8Array(totalPacked);
  let cursor = 0;
  for (const chunk of compressedChunks) {
    packData.set(chunk, cursor);
    cursor += chunk.length;
  }

  const numFiles = fileNames.length;
  const emptyStreamBits =
    emptyStreamEntries.length > 0
      ? makeBitVector(numFiles, (i) => entries[i].isDir || entries[i].size === 0)
      : null;
  const emptyFileBits =
    emptyStreamEntries.length > 0
      ? makeBitVector(emptyStreamEntries.length, (i) => !emptyStreamEntries[i].isDir)
      : null;

  const header = build7zHeader({
    packSizes,
    unpackSizes,
    fileNames,
    emptyStreamBits,
    emptyFileBits,
  });

  const signature = Uint8Array.from([0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c]);
  const startHeader = new Uint8Array(32);
  startHeader.set(signature, 0);
  startHeader[6] = 0;
  startHeader[7] = 4;
  writeUInt64LE(packData.length, startHeader, 12);
  writeUInt64LE(header.length, startHeader, 20);
  const nextHeaderCrc = crc32(header);
  writeUInt32LE(nextHeaderCrc, startHeader, 28);
  const startCrc = crc32(startHeader.subarray(12, 32));
  writeUInt32LE(startCrc, startHeader, 8);

  const archiveBytes = new Uint8Array(startHeader.length + packData.length + header.length);
  archiveBytes.set(startHeader, 0);
  archiveBytes.set(packData, startHeader.length);
  archiveBytes.set(header, startHeader.length + packData.length);

  const blob = new Blob([archiveBytes], { type: "application/x-7z-compressed" });
  const baseName = archiveState.name ? archiveState.name.replace(/\.7z$/i, "") : "archive";
  const outName = `${baseName}-rezip.7z`;
  archiveState.rezipUrl = URL.createObjectURL(blob);
  if (rezipDownload) {
    rezipDownload.href = archiveState.rezipUrl;
    rezipDownload.download = outName;
    rezipDownload.hidden = false;
    rezipDownload.textContent = `Download ${outName}`;
  }
  if (rezipStatus) {
    rezipStatus.textContent = `Repacked ${dataFiles.length} file(s) into ${outName}.`;
  }
  if (rezipButton) {
    rezipButton.disabled = false;
  }
}

async function init() {
  if (typeof ZstdWasm !== "function") {
    setStatus("WASM loader missing: dist/zstd_wasm.js failed to load.");
    return;
  }
  if (window.location.protocol === "file:") {
    setStatus("WASM needs an HTTP server. Open this page via http://localhost.");
  }

  try {
    const baseUrl = new URL("../dist/", window.location.href);
    setStatus("Loading WASM...");
    const moduleConfig = {
      locateFile: (path) => new URL(path, baseUrl).toString(),
      print: (text) => console.log(`[wasm] ${text}`),
      printErr: (text) => console.error(`[wasm] ${text}`),
    };
    const maybePromise = ZstdWasm(moduleConfig);
    moduleInstance =
      maybePromise && typeof maybePromise.then === "function"
        ? await maybePromise
        : maybePromise;
    if (!moduleInstance) {
      throw new Error("Module factory did not return an instance.");
    }
  } catch (error) {
    setStatus(`WASM init failed: ${error.message}`);
    return;
  }

  if (!ensureHeaps()) {
    setStatus("WASM ready (no memory views). Using slower JS copy path.");
  }
  wasm7z = {
    open: moduleInstance.cwrap("wasm7z_open", "number", ["number", "number"]),
    openWithPassword: moduleInstance.cwrap("wasm7z_open_with_password", "number", [
      "number",
      "number",
      "string",
    ]),
    close: moduleInstance.cwrap("wasm7z_close", "void", []),
    fileCount: moduleInstance.cwrap("wasm7z_file_count", "number", []),
    fetchName: moduleInstance.cwrap("wasm7z_fetch_name", "number", ["number"]),
    nameBuffer: moduleInstance.cwrap("wasm7z_name_buffer", "number", []),
    nameLength: moduleInstance.cwrap("wasm7z_name_length", "number", []),
    isDirectory: moduleInstance.cwrap("wasm7z_is_directory", "number", ["number"]),
    fileSize: moduleInstance.cwrap("wasm7z_file_size", "number", ["number"]),
    extract: moduleInstance.cwrap("wasm7z_extract", "number", ["number", "number", "number", "number"]),
    hasEncryptedContent: moduleInstance.cwrap("wasm7z_has_encrypted_content", "number", []),
  };
  compressFn = moduleInstance.cwrap("zstd_wasm_compress", "number", [
    "number",
    "number",
    "number",
    "number",
    "number",
  ]);
  compressBoundFn = moduleInstance.cwrap("zstd_wasm_compress_bound", "number", [
    "number",
  ]);
  decompressFn = moduleInstance.cwrap("zstd_wasm_decompress", "number", [
    "number",
    "number",
    "number",
    "number",
  ]);
  frameSizeFn = moduleInstance.cwrap(
    "zstd_wasm_get_frame_content_size",
    "number",
    ["number", "number"],
  );
  isErrorFn = moduleInstance.cwrap("zstd_wasm_is_error", "number", ["number"]);
  errorNameFn = moduleInstance.cwrap("zstd_wasm_get_error_name", "string", [
    "number",
  ]);

  setStatus("Ready to compress.");
  runButton.disabled = false;
  if (rezipStatus) {
    rezipStatus.textContent = "Repack requires a loaded archive.";
  }
}

function runCompression() {
  if (!moduleInstance) {
    setStatus("WASM module is still loading.");
    return;
  }
  if (!ensureHeaps()) {
    setStatus("WASM memory views unavailable. Compression may be slower.");
  }
  const inputBytes = encoder.encode(inputEl.value);
  inputBytesEl.textContent = `${inputBytes.length}`;

  const inputPtr = moduleInstance._malloc(inputBytes.length);
  writeBytes(inputPtr, inputBytes);

  const outputCapacity = inputBytes.length + 1024;
  const outputPtr = moduleInstance._malloc(outputCapacity);

  const compressedSize = compressFn(
    inputPtr,
    inputBytes.length,
    outputPtr,
    outputCapacity,
    3,
  );

  if (isErrorFn(compressedSize)) {
    const message = errorNameFn(compressedSize);
    setStatus(`Compression failed: ${message}`);
    moduleInstance._free(inputPtr);
    moduleInstance._free(outputPtr);
    return;
  }

  compressedBytesEl.textContent = `${compressedSize}`;
  ratioEl.textContent = formatRatio(inputBytes.length, compressedSize);

  const frameSize = frameSizeFn(outputPtr, compressedSize);
  const outputSize = Number(frameSize);
  const decompressedPtr = moduleInstance._malloc(outputSize);

  const decompressedSize = decompressFn(
    outputPtr,
    compressedSize,
    decompressedPtr,
    outputSize,
  );

  if (isErrorFn(decompressedSize)) {
    const message = errorNameFn(decompressedSize);
    setStatus(`Decompression failed: ${message}`);
  } else {
    const decodedBytes = readBytes(decompressedPtr, decompressedSize);
    const decoded = decoder.decode(decodedBytes);
    roundtripEl.textContent = decoded === inputEl.value ? "OK" : "Mismatch";
    setStatus("Compression complete.");
  }

  moduleInstance._free(inputPtr);
  moduleInstance._free(outputPtr);
  moduleInstance._free(decompressedPtr);
}

async function openArchive(file) {
  if (!moduleInstance || !wasm7z) {
    fileStatus.textContent = "WASM module is still loading.";
    return;
  }
  if (!ensureHeaps()) {
    fileStatus.textContent = "WASM memory views unavailable. Extraction may be slower.";
  }

  resetArchiveState();
  fileStatus.textContent = `Loading ${file.name}...`;
  fileOutput.textContent = "Select a file to extract it.";
  fileList.textContent = "Reading archive...";

  try {
    const buffer = await file.arrayBuffer();
    const srcBytes = new Uint8Array(buffer);
    if (!srcBytes.length) {
      fileStatus.textContent = `${file.name} is empty.`;
      fileList.textContent = "Archive is empty.";
      return;
    }

    archiveState.ptr = moduleInstance._malloc(srcBytes.length);
    if (!archiveState.ptr) {
      fileStatus.textContent = "Unable to allocate memory for the archive.";
      fileList.textContent = "Archive could not be loaded.";
      return;
    }

    writeBytes(archiveState.ptr, srcBytes);
    archiveState.size = srcBytes.length;
    archiveState.name = file.name;

    const password = passwordInput?.value?.trim() || "";
    if (passwordHint) {
      passwordHint.textContent = password
        ? "Trying the supplied password..."
        : "Opening archive without a password.";
    }
    const openRes = password
      ? wasm7z.openWithPassword(archiveState.ptr, srcBytes.length, password)
      : wasm7z.open(archiveState.ptr, srcBytes.length);
    if (openRes !== 0) {
      if (openRes === SZ_ERROR_WRONG_PASSWORD) {
        fileStatus.textContent = "Incorrect password.";
        fileList.textContent = "Failed to unlock the archive payload.";
      } else {
        fileStatus.textContent = `Unable to open archive (${openRes}).`;
        fileList.textContent = "Archive could not be opened.";
      }
      resetArchiveState();
      return;
    }

    const files = [];
    const count = wasm7z.fileCount();
    for (let i = 0; i < count; i++) {
      wasm7z.fetchName(i);
      const ptr = wasm7z.nameBuffer();
      const len = wasm7z.nameLength();
      const name = readUtf16(ptr, len);
      files.push({
        index: i,
        name,
        size: wasm7z.fileSize(i),
        isDir: !!wasm7z.isDirectory(i),
      });
    }

    archiveState.entries = files;
    renderArchiveList(files);
    const hasEncryptedContent = !!wasm7z.hasEncryptedContent();
    if (hasEncryptedContent && !password) {
      fileStatus.textContent = `Loaded ${file.name} (${count} entries). Encrypted payload detected; enter a password and reopen to extract.`;
    } else {
      fileStatus.textContent = password
        ? `Loaded ${file.name} (${count} entries) using a password.`
        : `Loaded ${file.name} (${count} entries).`;
    }
    if (rezipStatus) {
      rezipStatus.textContent = "Ready to repack with Zstandard level 3.";
    }
    if (rezipButton) {
      rezipButton.disabled = false;
    }
  } catch (error) {
    fileStatus.textContent = `Unable to process file: ${error.message}`;
    fileList.textContent = "Archive could not be read.";
    resetArchiveState();
  }
}

runButton.addEventListener("click", runCompression);
runButton.disabled = true;
if (rezipButton) {
  rezipButton.addEventListener("click", () => {
    rezipArchive();
  });
  rezipButton.disabled = true;
}
if (passwordInput) {
  passwordInput.addEventListener("input", updatePasswordHint);
  updatePasswordHint();
}
fileInput.addEventListener("change", () => {
  if (!fileInput.files?.length) {
    fileStatus.textContent = "No archive loaded.";
    return;
  }
  const file = fileInput.files[0];
  openArchive(file);
});

init();
