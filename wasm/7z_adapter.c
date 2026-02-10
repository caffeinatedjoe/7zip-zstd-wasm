#include <emscripten/emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../C/7z.h"
#include "../C/7zCrc.h"

#define LOOK_BUFFER_SIZE (1 << 16)

typedef struct {
  ISeekInStream vt;
  const Byte *data;
  size_t size;
  UInt64 pos;
} CMemInStream;

static CSzArEx g_archive;
static CMemInStream g_memStream;
static CLookToRead2 g_lookStream;
static Byte g_lookBuffer[LOOK_BUFFER_SIZE];
static Byte *g_archiveBuffer = NULL;
static size_t g_archiveSize = 0;
static UInt16 g_nameBuf[2048];
static size_t g_nameLen = 0;
static Byte *g_outBuffer = NULL;
static size_t g_outBufferSize = 0;
static UInt32 g_blockIndex = (UInt32)(Int32)-1;
static int g_isOpen = 0;
static int g_crcReady = 0;

static void *SzAllocFunc(ISzAllocPtr p, size_t size) {
  (void)p;
  return malloc(size);
}

static void SzFreeFunc(ISzAllocPtr p, void *address) {
  (void)p;
  free(address);
}

static ISzAlloc g_allocImp = { SzAllocFunc, SzFreeFunc };
static ISzAlloc g_allocTempImp = { SzAllocFunc, SzFreeFunc };

static SRes MemInStream_Read(ISeekInStreamPtr p, void *buf, size_t *size) {
  if (!p || !buf || !size)
    return SZ_ERROR_FAIL;
  CMemInStream *self = (CMemInStream *)p;
  size_t toRead = *size;
  if (self->pos + toRead > self->size)
    toRead = (size_t)(self->size - self->pos);
  memcpy(buf, self->data + self->pos, toRead);
  self->pos += toRead;
  *size = toRead;
  return SZ_OK;
}

static SRes MemInStream_Seek(ISeekInStreamPtr p, Int64 *pos, ESzSeek origin) {
  if (!p || !pos)
    return SZ_ERROR_FAIL;
  CMemInStream *self = (CMemInStream *)p;
  Int64 newPos = (Int64)self->pos;
  switch (origin) {
    case SZ_SEEK_SET: newPos = *pos; break;
    case SZ_SEEK_CUR: newPos += *pos; break;
    case SZ_SEEK_END: newPos = (Int64)self->size + *pos; break;
  }
  if (newPos < 0 || (UInt64)newPos > self->size)
    return SZ_ERROR_FAIL;
  self->pos = (UInt64)newPos;
  *pos = newPos;
  return SZ_OK;
}

static void MemInStream_Init(CMemInStream *self, const Byte *data, size_t size) {
  self->vt.Read = MemInStream_Read;
  self->vt.Seek = MemInStream_Seek;
  self->data = data;
  self->size = size;
  self->pos = 0;
}

extern void LookToRead2_CreateVTable(CLookToRead2 *p, int lookahead);

static void ResetArchiveState(void) {
  if (g_archiveBuffer) {
    free(g_archiveBuffer);
    g_archiveBuffer = NULL;
  }
  SzArEx_Free(&g_archive, &g_allocImp);
  g_outBuffer = NULL;
  g_outBufferSize = 0;
  g_blockIndex = (UInt32)(Int32)-1;
  g_isOpen = 0;
}

EMSCRIPTEN_KEEPALIVE int wasm7z_open(const uint8_t *data, size_t size) {
  if (!data || size == 0)
    return SZ_ERROR_PARAM;
  if (!g_crcReady) {
    CrcGenerateTable();
    g_crcReady = 1;
  }
  ResetArchiveState();
  g_archiveBuffer = (Byte *)malloc(size);
  if (!g_archiveBuffer)
    return SZ_ERROR_MEM;
  memcpy(g_archiveBuffer, data, size);
  g_archiveSize = size;
  MemInStream_Init(&g_memStream, g_archiveBuffer, size);
  LookToRead2_CreateVTable(&g_lookStream, 0);
  LookToRead2_INIT(&g_lookStream);
  g_lookStream.realStream = &g_memStream.vt;
  g_lookStream.buf = g_lookBuffer;
  g_lookStream.bufSize = LOOK_BUFFER_SIZE;
  SzArEx_Init(&g_archive);
  const SRes res = SzArEx_Open(&g_archive, &g_lookStream.vt, &g_allocImp, &g_allocTempImp);
  if (res == SZ_OK)
    g_isOpen = 1;
  return res;
}

EMSCRIPTEN_KEEPALIVE void wasm7z_close(void) {
  ResetArchiveState();
}

EMSCRIPTEN_KEEPALIVE size_t wasm7z_file_count(void) {
  return g_isOpen ? g_archive.NumFiles : 0;
}

EMSCRIPTEN_KEEPALIVE size_t wasm7z_fetch_name(int index) {
  if (!g_isOpen || index < 0 || (size_t)index >= g_archive.NumFiles)
    return 0;
  g_nameLen = SzArEx_GetFileNameUtf16(&g_archive, index, g_nameBuf);
  if (g_nameLen > 0 && g_nameBuf[g_nameLen - 1] == 0)
    g_nameLen -= 1;
  return g_nameLen;
}

EMSCRIPTEN_KEEPALIVE const UInt16 *wasm7z_name_buffer(void) {
  return g_nameBuf;
}

EMSCRIPTEN_KEEPALIVE size_t wasm7z_name_length(void) {
  return g_nameLen;
}

EMSCRIPTEN_KEEPALIVE int wasm7z_is_directory(int index) {
  if (!g_isOpen || index < 0 || (size_t)index >= g_archive.NumFiles)
    return 0;
  if (!g_archive.IsDirs)
    return 0;
  return SzArEx_IsDir(&g_archive, (UInt32)index);
}

EMSCRIPTEN_KEEPALIVE size_t wasm7z_file_size(int index) {
  if (!g_isOpen || index < 0 || (size_t)index >= g_archive.NumFiles)
    return 0;
  if (!g_archive.UnpackPositions)
    return 0;
  const size_t len = g_archive.UnpackPositions[index + 1] - g_archive.UnpackPositions[index];
  return len;
}

EMSCRIPTEN_KEEPALIVE int wasm7z_extract(int index, uint8_t *dst, size_t dstCapacity, size_t *outSize) {
  if (!g_isOpen || index < 0 || (size_t)index >= g_archive.NumFiles)
    return SZ_ERROR_ARCHIVE;
  UInt32 blockIndex = g_blockIndex;
  size_t offset = 0;
  SRes res = SzArEx_Extract(
    &g_archive,
    &g_lookStream.vt,
    (UInt32)index,
    &blockIndex,
    &g_outBuffer,
    &g_outBufferSize,
    &offset,
    outSize,
    &g_allocImp,
    &g_allocTempImp);
  if (res == SZ_OK) {
    if (dstCapacity < *outSize)
      return SZ_ERROR_OUTPUT_EOF;
    memcpy(dst, g_outBuffer + offset, *outSize);
    g_blockIndex = blockIndex;
  }
  return res;
}
