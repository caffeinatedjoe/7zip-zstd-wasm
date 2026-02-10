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

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const utf8Decoder = new TextDecoder();
const utf16Decoder = new TextDecoder("utf-16le");

let moduleInstance;
let compressFn;
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
    fileStatus.textContent = `Extraction failed (${result}).`;
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
    close: moduleInstance.cwrap("wasm7z_close", "void", []),
    fileCount: moduleInstance.cwrap("wasm7z_file_count", "number", []),
    fetchName: moduleInstance.cwrap("wasm7z_fetch_name", "number", ["number"]),
    nameBuffer: moduleInstance.cwrap("wasm7z_name_buffer", "number", []),
    nameLength: moduleInstance.cwrap("wasm7z_name_length", "number", []),
    isDirectory: moduleInstance.cwrap("wasm7z_is_directory", "number", ["number"]),
    fileSize: moduleInstance.cwrap("wasm7z_file_size", "number", ["number"]),
    extract: moduleInstance.cwrap("wasm7z_extract", "number", ["number", "number", "number", "number"]),
  };
  compressFn = moduleInstance.cwrap("zstd_wasm_compress", "number", [
    "number",
    "number",
    "number",
    "number",
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

  clearTimeout(loadTimer);
  setStatus("Ready to compress.");
  runButton.disabled = false;
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

    const openRes = wasm7z.open(archiveState.ptr, srcBytes.length);
    if (openRes !== 0) {
      fileStatus.textContent = `Unable to open archive (${openRes}).`;
      fileList.textContent = "Archive could not be opened.";
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
    fileStatus.textContent = `Loaded ${file.name} (${count} entries).`;
  } catch (error) {
    fileStatus.textContent = `Unable to process file: ${error.message}`;
    fileList.textContent = "Archive could not be read.";
    resetArchiveState();
  }
}

runButton.addEventListener("click", runCompression);
runButton.disabled = true;
fileInput.addEventListener("change", () => {
  if (!fileInput.files?.length) {
    fileStatus.textContent = "No archive loaded.";
    return;
  }
  const file = fileInput.files[0];
  openArchive(file);
});

init();
