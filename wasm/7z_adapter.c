#include <emscripten/emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../C/7z.h"
#include "../C/7zCrc.h"
#include "../C/zstd/zstd.h"

#define LOOK_BUFFER_SIZE (1 << 16)
#define STREAM_IO_BUFFER_SIZE (1 << 16)
#define SZ_ERROR_WRONG_PASSWORD ((SRes)0x80100015)
#define SZ_ERROR_ENCRYPTION_UNSUPPORTED ((SRes)0x80100016)
#define METHOD_ID_7Z_AES ((UInt32)0x06F10701)
#define METHOD_ID_COPY ((UInt32)0x00000000)
#define METHOD_ID_ZSTD ((UInt32)0x04F71101)

#define WASM7Z_STREAM_ERR_INVALID_INDEX 10001
#define WASM7Z_STREAM_ERR_INVALID_STATE 10002
#define WASM7Z_STREAM_ERR_UNSUPPORTED_METHOD 10003
#define WASM7Z_STREAM_ERR_DECODE 10004
#define WASM7Z_STREAM_ERR_ALLOC 10005
#define WASM7Z_STREAM_ERR_BAD_ARGUMENT 10006

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
static int g_hasEncryptedContent = 0;
static char *g_passwordUtf8 = NULL;
static size_t g_passwordUtf8Len = 0;
static UInt16 *g_passwordUtf16 = NULL;
static size_t g_passwordUtf16Len = 0;

typedef enum {
  STREAM_METHOD_NONE = 0,
  STREAM_METHOD_COPY = 1,
  STREAM_METHOD_ZSTD = 2,
} StreamMethod;

typedef struct {
  int active;
  StreamMethod method;
  UInt32 fileIndex;
  UInt64 fileRemaining;
  UInt64 skipRemaining;
  UInt32 crcValue;
  int hasExpectedCrc;
  UInt32 expectedCrc;

  const Byte *src;
  size_t srcSize;
  size_t srcPos;

  ZSTD_DStream *zstd;
  Byte zstdSkipBuffer[STREAM_IO_BUFFER_SIZE];
} StreamExtractState;

static StreamExtractState g_streamState = {0};

EMSCRIPTEN_KEEPALIVE int wasm7z_open_with_password(const uint8_t *data, size_t size, const char *password);

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
  if (g_outBuffer) {
    free(g_outBuffer);
    g_outBuffer = NULL;
  }
  if (g_streamState.zstd) {
    ZSTD_freeDStream(g_streamState.zstd);
    g_streamState.zstd = NULL;
  }
  memset(&g_streamState, 0, sizeof(g_streamState));
  if (g_archiveBuffer) {
    free(g_archiveBuffer);
    g_archiveBuffer = NULL;
  }
  SzArEx_Free(&g_archive, &g_allocImp);
  g_outBufferSize = 0;
  g_blockIndex = (UInt32)(Int32)-1;
  g_isOpen = 0;
  g_hasEncryptedContent = 0;
}

static int StreamReset(void) {
  if (g_streamState.zstd) {
    ZSTD_freeDStream(g_streamState.zstd);
    g_streamState.zstd = NULL;
  }
  memset(&g_streamState, 0, sizeof(g_streamState));
  return SZ_OK;
}

static int LoadFolderForFile(UInt32 fileIndex, CSzFolder *folder) {
  const UInt32 folderIndex = g_archive.FileToFolder[fileIndex];
  const Byte *folderData;
  if (folderIndex == (UInt32)-1) {
    return WASM7Z_STREAM_ERR_INVALID_INDEX;
  }
  folderData = g_archive.db.CodersData + g_archive.db.FoCodersOffsets[folderIndex];
  {
    CSzData sd;
    sd.Data = folderData;
    sd.Size = g_archive.db.FoCodersOffsets[(size_t)folderIndex + 1] - g_archive.db.FoCodersOffsets[folderIndex];
    if (SzGetNextFolderItem(folder, &sd) != SZ_OK || sd.Size != 0) {
      return WASM7Z_STREAM_ERR_DECODE;
    }
  }
  return SZ_OK;
}

static int ConfigureStreamingForFile(UInt32 fileIndex) {
  UInt32 folderIndex;
  UInt64 fileStartPos;
  UInt64 folderStartPos;
  UInt64 fileSize;
  UInt64 folderPackOffset;
  UInt64 folderPackSize;
  UInt64 absolutePackOffset;
  const UInt64 *packPositions;
  CSzFolder folder;
  int result;

  result = StreamReset();
  if (result != SZ_OK) {
    return result;
  }

  fileStartPos = g_archive.UnpackPositions[fileIndex];
  fileSize = g_archive.UnpackPositions[(size_t)fileIndex + 1] - fileStartPos;
  g_streamState.active = 1;
  g_streamState.fileIndex = fileIndex;
  g_streamState.fileRemaining = fileSize;
  g_streamState.crcValue = CRC_INIT_VAL;
  g_streamState.hasExpectedCrc = 0;
  if (SzBitWithVals_Check(&g_archive.CRCs, fileIndex)) {
    g_streamState.hasExpectedCrc = 1;
    g_streamState.expectedCrc = g_archive.CRCs.Vals[fileIndex];
  }

  folderIndex = g_archive.FileToFolder[fileIndex];
  if (folderIndex == (UInt32)-1) {
    g_streamState.method = STREAM_METHOD_NONE;
    g_streamState.skipRemaining = 0;
    return SZ_OK;
  }

  result = LoadFolderForFile(fileIndex, &folder);
  if (result != SZ_OK) {
    StreamReset();
    return result;
  }

  if (folder.NumPackStreams != 1 || folder.NumCoders != 1 || folder.UnpackStream != 0 || folder.PackStreams[0] != 0) {
    StreamReset();
    return WASM7Z_STREAM_ERR_UNSUPPORTED_METHOD;
  }

  folderStartPos = g_archive.UnpackPositions[g_archive.FolderToFile[folderIndex]];
  g_streamState.skipRemaining = fileStartPos - folderStartPos;

  packPositions = g_archive.db.PackPositions + g_archive.db.FoStartPackStreamIndex[folderIndex];
  folderPackOffset = packPositions[0];
  folderPackSize = packPositions[1] - packPositions[0];
  absolutePackOffset = g_archive.dataPos + folderPackOffset;
  if (absolutePackOffset > g_archiveSize || folderPackSize > (UInt64)(g_archiveSize - (size_t)absolutePackOffset)) {
    StreamReset();
    return WASM7Z_STREAM_ERR_DECODE;
  }
  g_streamState.src = g_archiveBuffer + (size_t)absolutePackOffset;
  g_streamState.srcSize = (size_t)folderPackSize;
  g_streamState.srcPos = 0;

  if (folder.Coders[0].MethodID == METHOD_ID_COPY) {
    g_streamState.method = STREAM_METHOD_COPY;
    return SZ_OK;
  }
  if (folder.Coders[0].MethodID == METHOD_ID_ZSTD) {
    size_t initRes;
    g_streamState.zstd = ZSTD_createDStream();
    if (!g_streamState.zstd) {
      StreamReset();
      return WASM7Z_STREAM_ERR_ALLOC;
    }
    initRes = ZSTD_initDStream(g_streamState.zstd);
    if (ZSTD_isError(initRes)) {
      StreamReset();
      return WASM7Z_STREAM_ERR_DECODE;
    }
    g_streamState.method = STREAM_METHOD_ZSTD;
    return SZ_OK;
  }

  StreamReset();
  return WASM7Z_STREAM_ERR_UNSUPPORTED_METHOD;
}

static int StreamFinalizeIfDone(void) {
  if (!g_streamState.active || g_streamState.fileRemaining != 0) {
    return SZ_OK;
  }
  if (g_streamState.hasExpectedCrc && CRC_GET_DIGEST(g_streamState.crcValue) != g_streamState.expectedCrc) {
    StreamReset();
    return SZ_ERROR_CRC;
  }
  return SZ_OK;
}

static int StreamReadCopy(Byte *out, uint32_t outCapacity, uint32_t *produced, int *done) {
  uint32_t written = 0;
  while (written < outCapacity && g_streamState.fileRemaining > 0) {
    size_t available = g_streamState.srcSize - g_streamState.srcPos;
    size_t take;
    if (available == 0) {
      return WASM7Z_STREAM_ERR_DECODE;
    }
    if (g_streamState.skipRemaining > 0) {
      size_t skip = available;
      if ((UInt64)skip > g_streamState.skipRemaining) {
        skip = (size_t)g_streamState.skipRemaining;
      }
      g_streamState.srcPos += skip;
      g_streamState.skipRemaining -= skip;
      continue;
    }
    take = outCapacity - written;
    if ((UInt64)take > g_streamState.fileRemaining) {
      take = (size_t)g_streamState.fileRemaining;
    }
    if (take > available) {
      take = available;
    }
    if (take == 0) {
      break;
    }
    memcpy(out + written, g_streamState.src + g_streamState.srcPos, take);
    g_streamState.crcValue = CrcUpdate(g_streamState.crcValue, out + written, take);
    g_streamState.srcPos += take;
    written += (uint32_t)take;
    g_streamState.fileRemaining -= take;
  }
  *produced = written;
  *done = (g_streamState.fileRemaining == 0) ? 1 : 0;
  return StreamFinalizeIfDone();
}

static int StreamReadZstd(Byte *out, uint32_t outCapacity, uint32_t *produced, int *done) {
  uint32_t written = 0;
  while (written < outCapacity && g_streamState.fileRemaining > 0) {
    ZSTD_inBuffer inBuf;
    ZSTD_outBuffer outBuf;
    size_t beforeIn;
    size_t decodeRes;
    if (g_streamState.srcPos > g_streamState.srcSize) {
      return WASM7Z_STREAM_ERR_DECODE;
    }
    inBuf.src = g_streamState.src;
    inBuf.size = g_streamState.srcSize;
    inBuf.pos = g_streamState.srcPos;

    if (g_streamState.skipRemaining > 0) {
      size_t skipCap = sizeof(g_streamState.zstdSkipBuffer);
      if ((UInt64)skipCap > g_streamState.skipRemaining) {
        skipCap = (size_t)g_streamState.skipRemaining;
      }
      outBuf.dst = g_streamState.zstdSkipBuffer;
      outBuf.size = skipCap;
      outBuf.pos = 0;
    } else {
      size_t targetCap = outCapacity - written;
      if ((UInt64)targetCap > g_streamState.fileRemaining) {
        targetCap = (size_t)g_streamState.fileRemaining;
      }
      outBuf.dst = out + written;
      outBuf.size = targetCap;
      outBuf.pos = 0;
    }

    beforeIn = inBuf.pos;
    decodeRes = ZSTD_decompressStream(g_streamState.zstd, &outBuf, &inBuf);
    if (ZSTD_isError(decodeRes)) {
      return WASM7Z_STREAM_ERR_DECODE;
    }
    g_streamState.srcPos = inBuf.pos;

    if (g_streamState.skipRemaining > 0) {
      g_streamState.skipRemaining -= outBuf.pos;
    } else if (outBuf.pos > 0) {
      g_streamState.crcValue = CrcUpdate(g_streamState.crcValue, out + written, outBuf.pos);
      written += (uint32_t)outBuf.pos;
      g_streamState.fileRemaining -= outBuf.pos;
    }

    if (outBuf.pos == 0 && inBuf.pos == beforeIn) {
      return WASM7Z_STREAM_ERR_DECODE;
    }
  }
  *produced = written;
  *done = (g_streamState.fileRemaining == 0) ? 1 : 0;
  return StreamFinalizeIfDone();
}

static int ArchiveHas7zAes(const CSzArEx *archive) {
  UInt32 folderIndex;
  for (folderIndex = 0; folderIndex < archive->db.NumFolders; folderIndex++) {
    CSzFolder folder;
    CSzData sd;
    UInt32 coderIndex;
    const Byte *data = archive->db.CodersData + archive->db.FoCodersOffsets[folderIndex];
    sd.Data = data;
    sd.Size = archive->db.FoCodersOffsets[(size_t)folderIndex + 1] - archive->db.FoCodersOffsets[folderIndex];
    if (SzGetNextFolderItem(&folder, &sd) != SZ_OK) {
      continue;
    }
    for (coderIndex = 0; coderIndex < folder.NumCoders; coderIndex++) {
      if ((UInt32)folder.Coders[coderIndex].MethodID == METHOD_ID_7Z_AES) {
        return 1;
      }
    }
  }
  return 0;
}

static size_t Utf8ToUtf16(const char *src, UInt16 *dst, size_t dstCapacity) {
  const unsigned char *cur = (const unsigned char *)src;
  size_t wrote = 0;
  while (*cur && wrote < dstCapacity - 1) {
    uint32_t codepoint;
    unsigned char byte = *cur++;
    if ((byte & 0x80) == 0) {
      codepoint = byte;
    } else if ((byte & 0xE0) == 0xC0 && (cur[0] & 0xC0) == 0x80) {
      codepoint = ((byte & 0x1F) << 6) | ((*cur++ & 0x3F));
    } else if ((byte & 0xF0) == 0xE0 && (cur[0] & 0xC0) == 0x80 && (cur[1] & 0xC0) == 0x80) {
      codepoint = ((byte & 0x0F) << 12) | (((uint32_t)cur[0] & 0x3F) << 6) | ((uint32_t)cur[1] & 0x3F);
      cur += 2;
    } else if ((byte & 0xF8) == 0xF0 && (cur[0] & 0xC0) == 0x80 && (cur[1] & 0xC0) == 0x80 && (cur[2] & 0xC0) == 0x80) {
      codepoint =
          ((byte & 0x07) << 18) | (((uint32_t)cur[0] & 0x3F) << 12) | (((uint32_t)cur[1] & 0x3F) << 6) |
          ((uint32_t)cur[2] & 0x3F);
      cur += 3;
    } else {
      codepoint = 0xFFFD;
    }
    if (codepoint <= 0xFFFF) {
      dst[wrote++] = (UInt16)codepoint;
    } else {
      if (wrote + 1 >= dstCapacity - 1) {
        break;
      }
      codepoint -= 0x10000;
      dst[wrote++] = (UInt16)(0xD800 + (codepoint >> 10));
      dst[wrote++] = (UInt16)(0xDC00 + (codepoint & 0x3FF));
    }
  }
  dst[wrote++] = 0;
  return wrote - 1;
}

static void ClearStoredPassword(void) {
  if (g_passwordUtf8) {
    free(g_passwordUtf8);
    g_passwordUtf8 = NULL;
  }
  g_passwordUtf8Len = 0;
  if (g_passwordUtf16) {
    free(g_passwordUtf16);
    g_passwordUtf16 = NULL;
  }
  g_passwordUtf16Len = 0;
}

static int PreservePassword(const char *password) {
  if (!password || !password[0]) {
    ClearStoredPassword();
    return SZ_OK;
  }
  size_t len = strlen(password);
  char *copy = (char *)malloc(len + 1);
  if (!copy)
    return SZ_ERROR_MEM;
  memcpy(copy, password, len + 1);
  UInt16 *buf = (UInt16 *)malloc((len + 4) * sizeof(UInt16));
  if (!buf) {
    free(copy);
    return SZ_ERROR_MEM;
  }
  size_t u16 = Utf8ToUtf16(password, buf, len + 4);
  ClearStoredPassword();
  g_passwordUtf8 = copy;
  g_passwordUtf8Len = len;
  g_passwordUtf16 = buf;
  g_passwordUtf16Len = u16;
  return SZ_OK;
}

EMSCRIPTEN_KEEPALIVE int wasm7z_open(const uint8_t *data, size_t size) {
  return wasm7z_open_with_password(data, size, NULL);
}

static int wasm7z_open_internal(const uint8_t *data, size_t size, int useStoredPassword) {
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
  if (useStoredPassword && g_passwordUtf16 && g_passwordUtf16Len) {
    SzAr_SetPassword((const Byte *)g_passwordUtf16, g_passwordUtf16Len * sizeof(UInt16));
  } else {
    SzAr_SetPassword(NULL, 0);
  }
  const SRes res = SzArEx_Open(&g_archive, &g_lookStream.vt, &g_allocImp, &g_allocTempImp);
  if (res == SZ_OK) {
    g_isOpen = 1;
    g_hasEncryptedContent = ArchiveHas7zAes(&g_archive);
  }
  return res;
}

EMSCRIPTEN_KEEPALIVE int wasm7z_open_with_password(const uint8_t *data, size_t size, const char *password) {
  if (!password) {
    return wasm7z_open_internal(data, size, 0);
  }
  const int pwdRes = PreservePassword(password);
  if (pwdRes != SZ_OK)
    return pwdRes;
  {
    const int res = wasm7z_open_internal(data, size, 1);
    if (res == SZ_ERROR_DATA || res == SZ_ERROR_CRC)
      return SZ_ERROR_WRONG_PASSWORD;
    return res;
  }
}

EMSCRIPTEN_KEEPALIVE void wasm7z_close(void) {
  ResetArchiveState();
  ClearStoredPassword();
  SzAr_SetPassword(NULL, 0);
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
  } else if (res == SZ_ERROR_UNSUPPORTED && g_hasEncryptedContent) {
    return SZ_ERROR_ENCRYPTION_UNSUPPORTED;
  }
  return res;
}

EMSCRIPTEN_KEEPALIVE int wasm7z_extract_begin(int index) {
  if (!g_isOpen)
    return WASM7Z_STREAM_ERR_INVALID_STATE;
  if (g_streamState.active)
    return WASM7Z_STREAM_ERR_INVALID_STATE;
  if (index < 0 || (size_t)index >= g_archive.NumFiles)
    return WASM7Z_STREAM_ERR_INVALID_INDEX;
  return ConfigureStreamingForFile((UInt32)index);
}

EMSCRIPTEN_KEEPALIVE int wasm7z_extract_read(int out_ptr, uint32_t out_capacity, uint32_t *produced, int *done) {
  Byte *outBuffer;
  int res;
  if (!produced || !done)
    return WASM7Z_STREAM_ERR_BAD_ARGUMENT;
  *produced = 0;
  *done = 0;
  if (!g_isOpen || !g_streamState.active)
    return WASM7Z_STREAM_ERR_INVALID_STATE;
  if (out_capacity > 0 && out_ptr == 0)
    return WASM7Z_STREAM_ERR_BAD_ARGUMENT;
  if (g_streamState.fileRemaining == 0) {
    *done = 1;
    return StreamFinalizeIfDone();
  }
  if (out_capacity == 0)
    return SZ_OK;

  outBuffer = (Byte *)(uintptr_t)out_ptr;
  if (g_streamState.method == STREAM_METHOD_NONE) {
    *done = 1;
    return SZ_OK;
  }
  if (g_streamState.method == STREAM_METHOD_COPY) {
    res = StreamReadCopy(outBuffer, out_capacity, produced, done);
  } else if (g_streamState.method == STREAM_METHOD_ZSTD) {
    res = StreamReadZstd(outBuffer, out_capacity, produced, done);
  } else {
    return WASM7Z_STREAM_ERR_INVALID_STATE;
  }
  if (res != SZ_OK) {
    StreamReset();
  }
  return res;
}

EMSCRIPTEN_KEEPALIVE int wasm7z_extract_end(void) {
  if (!g_isOpen)
    return WASM7Z_STREAM_ERR_INVALID_STATE;
  if (!g_streamState.active)
    return WASM7Z_STREAM_ERR_INVALID_STATE;
  return StreamReset();
}

EMSCRIPTEN_KEEPALIVE int wasm7z_has_encrypted_content(void) {
  return g_hasEncryptedContent;
}
