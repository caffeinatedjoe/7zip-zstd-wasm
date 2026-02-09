const statusEl = document.getElementById("status");
const runButton = document.getElementById("run");
const inputEl = document.getElementById("input");
const inputBytesEl = document.getElementById("input-bytes");
const compressedBytesEl = document.getElementById("compressed-bytes");
const ratioEl = document.getElementById("ratio");
const roundtripEl = document.getElementById("roundtrip");

const encoder = new TextEncoder();
const decoder = new TextDecoder();

let moduleInstance;
let compressFn;
let decompressFn;
let frameSizeFn;
let isErrorFn;
let errorNameFn;

function formatRatio(inputSize, compressedSize) {
  if (!inputSize || !compressedSize) {
    return "-";
  }
  return `${(compressedSize / inputSize).toFixed(2)}x`;
}

function setStatus(message) {
  statusEl.textContent = message;
}

async function init() {
  moduleInstance = await ZstdWasm();
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

  setStatus("Ready to compress.");
  runButton.disabled = false;
}

function runCompression() {
  const inputBytes = encoder.encode(inputEl.value);
  inputBytesEl.textContent = `${inputBytes.length}`;

  const inputPtr = moduleInstance._malloc(inputBytes.length);
  moduleInstance.HEAPU8.set(inputBytes, inputPtr);

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
    const decoded = decoder.decode(
      moduleInstance.HEAPU8.subarray(
        decompressedPtr,
        decompressedPtr + decompressedSize,
      ),
    );
    roundtripEl.textContent = decoded === inputEl.value ? "OK" : "Mismatch";
    setStatus("Compression complete.");
  }

  moduleInstance._free(inputPtr);
  moduleInstance._free(outputPtr);
  moduleInstance._free(decompressedPtr);
}

runButton.addEventListener("click", runCompression);
runButton.disabled = true;

init();
