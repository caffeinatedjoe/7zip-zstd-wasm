#include <emscripten/emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../C/7z.h"
#include "../C/7zCrc.h"

#define LOOK_BUFFER_SIZE (1 << 16)
#define SZ_ERROR_WRONG_PASSWORD ((SRes)0x80100015)
#define SZ_ERROR_ENCRYPTION_UNSUPPORTED ((SRes)0x80100016)
#define METHOD_ID_7Z_AES ((UInt32)0x06F10701)

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
  if (g_archiveBuffer) {
    free(g_archiveBuffer);
    g_archiveBuffer = NULL;
  }
  SzArEx_Free(&g_archive, &g_allocImp);
  g_outBuffer = NULL;
  g_outBufferSize = 0;
  g_blockIndex = (UInt32)(Int32)-1;
  g_isOpen = 0;
  g_hasEncryptedContent = 0;
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

EMSCRIPTEN_KEEPALIVE int wasm7z_has_encrypted_content(void) {
  return g_hasEncryptedContent;
}
