/* 7zDec.c -- Decoding from 7z folder
: Igor Pavlov : Public domain */

#include "Precomp.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* #define Z7_PPMD_SUPPORT */

#include "7z.h"
#include "Aes.h"
#include "7zCrc.h"
#include "Sha256.h"

#include "Bcj2.h"
#include "Bra.h"
#include "CpuArch.h"
#include "Delta.h"
#include "LzmaDec.h"
#include "Lzma2Dec.h"
#include "zstd.h"
#ifdef Z7_PPMD_SUPPORT
#include "Ppmd7.h"
#endif

#define k_Copy 0
#ifndef Z7_NO_METHOD_LZMA2
#define k_LZMA2 0x21
#endif
#define k_LZMA  0x30101
#define k_BCJ2  0x303011B
#define k_ZSTD  0x4F71101
#define k_AES   0x6F10701
#define SZ_ERROR_WRONG_PASSWORD ((SRes)0x80100015)
#define WASM7Z_AES_DEBUG 1

#if WASM7Z_AES_DEBUG
#define AES_DBG(...) fprintf(stderr, "[wasm7z:aes] " __VA_ARGS__)
#else
#define AES_DBG(...)
#endif

#if !defined(Z7_NO_METHODS_FILTERS)
#define Z7_USE_BRANCH_FILTER
#endif

#if !defined(Z7_NO_METHODS_FILTERS) || \
     defined(Z7_USE_NATIVE_BRANCH_FILTER) && defined(MY_CPU_ARM64)
#define Z7_USE_FILTER_ARM64
#ifndef Z7_USE_BRANCH_FILTER
#define Z7_USE_BRANCH_FILTER
#endif
#define k_ARM64 0xa
#endif

#if !defined(Z7_NO_METHODS_FILTERS) || \
     defined(Z7_USE_NATIVE_BRANCH_FILTER) && defined(MY_CPU_ARMT)
#define Z7_USE_FILTER_ARMT
#ifndef Z7_USE_BRANCH_FILTER
#define Z7_USE_BRANCH_FILTER
#endif
#define k_ARMT  0x3030701
#endif

#ifndef Z7_NO_METHODS_FILTERS
#define k_Delta 3
#define k_RISCV 0xb
#define k_BCJ   0x3030103
#define k_PPC   0x3030205
#define k_IA64  0x3030401
#define k_ARM   0x3030501
#define k_SPARC 0x3030805
#endif

static Byte *g_7zPassword = NULL;
static size_t g_7zPasswordSize = 0;
static int g_7zCryptoReady = 0;

static SRes SzDecodeLzma(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain);
#ifndef Z7_NO_METHOD_LZMA2
static SRes SzDecodeLzma2(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain);
#endif
static SRes SzDecodeCopy(UInt64 inSize, ILookInStreamPtr inStream, Byte *outBuffer);
static SRes SzDecodeZstd(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain);
#ifdef Z7_PPMD_SUPPORT
static SRes SzDecodePpmd(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain);
#endif
static BoolInt IS_SUPPORTED_CODER(const CSzCoderInfo *c);
static BoolInt IS_AES_CODER(const CSzCoderInfo *c);

static void DebugDumpFolder(const CSzFolder *f)
{
  UInt32 i;
  AES_DBG("folder meta: coders=%u packStreams=%u bonds=%u unpackStream=%u\n",
      (unsigned)f->NumCoders, (unsigned)f->NumPackStreams, (unsigned)f->NumBonds, (unsigned)f->UnpackStream);
  for (i = 0; i < f->NumCoders; i++)
  {
    AES_DBG("  coder[%u]: method=%08X streams=%u props=%u\n",
        (unsigned)i, (unsigned)f->Coders[i].MethodID,
        (unsigned)f->Coders[i].NumStreams, (unsigned)f->Coders[i].PropsSize);
  }
  for (i = 0; i < f->NumPackStreams; i++)
  {
    AES_DBG("  pack[%u]=%u\n", (unsigned)i, (unsigned)f->PackStreams[i]);
  }
  for (i = 0; i < f->NumBonds; i++)
  {
    AES_DBG("  bond[%u]: in=%u out=%u\n",
        (unsigned)i, (unsigned)f->Bonds[i].InIndex, (unsigned)f->Bonds[i].OutIndex);
  }
}

void SzAr_SetPassword(const Byte *password, size_t passwordSize)
{
  if (g_7zPassword)
  {
    memset(g_7zPassword, 0, g_7zPasswordSize);
    free(g_7zPassword);
    g_7zPassword = NULL;
    g_7zPasswordSize = 0;
  }
  if (!password || passwordSize == 0)
    return;
  g_7zPassword = (Byte *)malloc(passwordSize);
  if (!g_7zPassword)
    return;
  memcpy(g_7zPassword, password, passwordSize);
  g_7zPasswordSize = passwordSize;
}

int SzAr_HasPassword(void)
{
  return g_7zPassword && g_7zPasswordSize != 0;
}

typedef struct
{
  ILookInStream vt;
  const Byte *data;
  size_t size;
  size_t pos;
} CBufLookInStream;

static SRes CBufLookInStream_Look(ILookInStreamPtr pp, const void **buf, size_t *size)
{
  CBufLookInStream *p = (CBufLookInStream *)pp;
  size_t avail = (p->pos <= p->size) ? (p->size - p->pos) : 0;
  if (*size > avail)
    *size = avail;
  *buf = p->data + p->pos;
  return SZ_OK;
}

static SRes CBufLookInStream_Skip(ILookInStreamPtr pp, size_t offset)
{
  CBufLookInStream *p = (CBufLookInStream *)pp;
  if (offset > p->size - p->pos)
    return SZ_ERROR_INPUT_EOF;
  p->pos += offset;
  return SZ_OK;
}

static SRes CBufLookInStream_Read(ILookInStreamPtr pp, void *buf, size_t *size)
{
  CBufLookInStream *p = (CBufLookInStream *)pp;
  size_t avail = (p->pos <= p->size) ? (p->size - p->pos) : 0;
  if (*size > avail)
    *size = avail;
  if (*size)
    memcpy(buf, p->data + p->pos, *size);
  p->pos += *size;
  return SZ_OK;
}

static SRes CBufLookInStream_Seek(ILookInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
  CBufLookInStream *p = (CBufLookInStream *)pp;
  Int64 cur = (Int64)p->pos;
  Int64 end = (Int64)p->size;
  Int64 next = cur;
  if (origin == SZ_SEEK_SET)
    next = *pos;
  else if (origin == SZ_SEEK_CUR)
    next = cur + *pos;
  else if (origin == SZ_SEEK_END)
    next = end + *pos;
  if (next < 0 || next > end)
    return SZ_ERROR_FAIL;
  p->pos = (size_t)next;
  *pos = next;
  return SZ_OK;
}

static void CBufLookInStream_Init(CBufLookInStream *p, const Byte *data, size_t size)
{
  p->vt.Look = CBufLookInStream_Look;
  p->vt.Skip = CBufLookInStream_Skip;
  p->vt.Read = CBufLookInStream_Read;
  p->vt.Seek = CBufLookInStream_Seek;
  p->data = data;
  p->size = size;
  p->pos = 0;
}

static SRes SzAes_DeriveKey(const Byte *salt, unsigned saltSize, unsigned numCyclesPower, Byte *key)
{
  if (!SzAr_HasPassword())
    return SZ_ERROR_WRONG_PASSWORD;

  if (numCyclesPower == 0x3F)
  {
    unsigned pos = 0;
    unsigned i;
    for (i = 0; i < saltSize && pos < SHA256_DIGEST_SIZE; i++)
      key[pos++] = salt[i];
    for (i = 0; i < g_7zPasswordSize && pos < SHA256_DIGEST_SIZE; i++)
      key[pos++] = g_7zPassword[i];
    while (pos < SHA256_DIGEST_SIZE)
      key[pos++] = 0;
    return SZ_OK;
  }

  if (numCyclesPower > 24)
    return SZ_ERROR_UNSUPPORTED;

  {
    CSha256 sha;
    const size_t baseSize = saltSize + g_7zPasswordSize;
    const size_t bufSize = baseSize + 8;
    Byte *buf = (Byte *)malloc(bufSize);
    UInt64 rounds = (UInt64)1 << numCyclesPower;
    UInt64 r;
    if (!buf)
      return SZ_ERROR_MEM;
    if (saltSize)
      memcpy(buf, salt, saltSize);
    if (g_7zPasswordSize)
      memcpy(buf + saltSize, g_7zPassword, g_7zPasswordSize);
    memset(buf + baseSize, 0, 8);
    Sha256_Init(&sha);
    for (r = 0; r < rounds; r++)
    {
      UInt64 v = r;
      unsigned i;
      for (i = 0; i < 8; i++)
      {
        buf[baseSize + i] = (Byte)(v & 0xFF);
        v >>= 8;
      }
      Sha256_Update(&sha, buf, bufSize);
    }
    Sha256_Final(&sha, key);
    memset(buf, 0, bufSize);
    free(buf);
  }
  return SZ_OK;
}

static SRes SzAes_DecodeBuf(const CSzCoderInfo *coder, const Byte *propsData, Byte *data, size_t size)
{
  unsigned numCyclesPower = 0;
  unsigned saltSize = 0;
  unsigned ivSize = 0;
  Byte salt[16];
  Byte iv[16];
  Byte key[SHA256_DIGEST_SIZE];
  unsigned i;

  if (size == 0)
    return SZ_OK;
  if ((size & (AES_BLOCK_SIZE - 1)) != 0)
    return SZ_ERROR_DATA;
  memset(salt, 0, sizeof(salt));
  memset(iv, 0, sizeof(iv));
  memset(key, 0, sizeof(key));

  if (coder->PropsSize == 0)
  {
    /* Upstream 7zAES decoder accepts empty props (all-zero defaults). */
    numCyclesPower = 0;
  }
  else
  {
    const Byte *props = propsData + coder->PropsOffset;
    const unsigned b0 = props[0];
    numCyclesPower = b0 & 0x3F;
    if ((b0 & 0xC0) == 0)
    {
      if (coder->PropsSize != 1)
        return SZ_ERROR_UNSUPPORTED;
    }
    else
    {
      const unsigned b1 = props[1];
      saltSize = ((b0 >> 7) & 1) + (b1 >> 4);
      ivSize = ((b0 >> 6) & 1) + (b1 & 0x0F);
      if (saltSize > sizeof(salt) || ivSize > sizeof(iv))
        return SZ_ERROR_UNSUPPORTED;
      if (coder->PropsSize != (UInt32)(2 + saltSize + ivSize))
        return SZ_ERROR_UNSUPPORTED;
      props += 2;
      for (i = 0; i < saltSize; i++)
        salt[i] = *props++;
      for (i = 0; i < ivSize; i++)
        iv[i] = *props++;
    }
  }

  AES_DBG("aes props: cycles=%u salt=%u iv=%u enc_size=%u\n",
      (unsigned)numCyclesPower, (unsigned)saltSize, (unsigned)ivSize, (unsigned)size);
  RINOK(SzAes_DeriveKey(salt, saltSize, numCyclesPower, key))
  if (!g_7zCryptoReady)
  {
    AesGenTables();
    Sha256Prepare();
    g_7zCryptoReady = 1;
  }
  {
    Byte stateSpace[AES_NUM_IVMRK_WORDS * sizeof(UInt32) + 15];
    UInt32 *ivAes = (UInt32 *)(void *)(((size_t)(stateSpace + 15)) & ~(size_t)15);
    Aes_SetKey_Dec(ivAes + 4, key, SHA256_DIGEST_SIZE);
    AesCbc_Init(ivAes, iv);
    g_AesCbc_Decode(ivAes, data, size / AES_BLOCK_SIZE);
  }
  memset(key, 0, sizeof(key));
  return SZ_OK;
}

static SRes SzDecodeMainFromMem(const CSzCoderInfo *coder,
    const Byte *propsData, const Byte *src, size_t srcSize,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain)
{
  CBufLookInStream in;
  CBufLookInStream_Init(&in, src, srcSize);

  if (coder->MethodID == k_Copy)
  {
    if (srcSize != outSize)
      return SZ_ERROR_DATA;
    memcpy(outBuffer, src, srcSize);
    return SZ_OK;
  }
  if (coder->MethodID == k_LZMA)
    return SzDecodeLzma(propsData + coder->PropsOffset, coder->PropsSize, srcSize, &in.vt, outBuffer, outSize, allocMain);
#ifndef Z7_NO_METHOD_LZMA2
  if (coder->MethodID == k_LZMA2)
    return SzDecodeLzma2(propsData + coder->PropsOffset, coder->PropsSize, srcSize, &in.vt, outBuffer, outSize, allocMain);
#endif
  if (coder->MethodID == k_ZSTD)
    return SzDecodeZstd(propsData + coder->PropsOffset, coder->PropsSize, srcSize, &in.vt, outBuffer, outSize, allocMain);
#ifdef Z7_PPMD_SUPPORT
  if (coder->MethodID == k_PPMD)
    return SzDecodePpmd(propsData + coder->PropsOffset, coder->PropsSize, srcSize, &in.vt, outBuffer, outSize, allocMain);
#endif
  return SZ_ERROR_UNSUPPORTED;
}

static SRes SzFolder_DecodeAesMain(const CSzFolder *folder,
    const Byte *propsData,
    const UInt64 *unpackSizes,
    const UInt64 *packPositions,
    ILookInStreamPtr inStream, UInt64 startPos,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain)
{
  UInt32 aesCoderIndex = 0;
  UInt32 mainCoderIndex = 0;
  const CSzCoderInfo *aesCoder;
  const CSzCoderInfo *mainCoder;
  const UInt64 offset = packPositions[0];
  const UInt64 inSize64 = packPositions[1] - offset;
  UInt64 decryptedMainInputSize64;
  Byte *encBuf;
  SizeT inSize;
  SizeT decryptedMainInputSize;
  SRes res;

  if (folder->NumCoders != 2 || folder->NumPackStreams != 1 || folder->NumBonds != 1)
    return SZ_ERROR_UNSUPPORTED;

  if (IS_AES_CODER(&folder->Coders[0]) && IS_SUPPORTED_CODER(&folder->Coders[1]))
  {
    aesCoderIndex = 0;
    mainCoderIndex = 1;
  }
  else if (IS_AES_CODER(&folder->Coders[1]) && IS_SUPPORTED_CODER(&folder->Coders[0]))
  {
    aesCoderIndex = 1;
    mainCoderIndex = 0;
  }
  else
    return SZ_ERROR_UNSUPPORTED;

  aesCoder = &folder->Coders[aesCoderIndex];
  mainCoder = &folder->Coders[mainCoderIndex];
  AES_DBG("folder decode: coders=(%08X,%08X) aes_idx=%u main_idx=%u unpack_main=%llu unpack_aes=%llu pack=%llu\n",
      (unsigned)folder->Coders[0].MethodID,
      (unsigned)folder->Coders[1].MethodID,
      (unsigned)aesCoderIndex,
      (unsigned)mainCoderIndex,
      (unsigned long long)unpackSizes[mainCoderIndex],
      (unsigned long long)unpackSizes[aesCoderIndex],
      (unsigned long long)inSize64);

  decryptedMainInputSize64 = unpackSizes[aesCoderIndex];
  if (decryptedMainInputSize64 != (SizeT)decryptedMainInputSize64)
    return SZ_ERROR_MEM;
  decryptedMainInputSize = (SizeT)decryptedMainInputSize64;
  if (mainCoderIndex >= 4 || unpackSizes[mainCoderIndex] != outSize)
    return SZ_ERROR_DATA;
  if (inSize64 != (SizeT)inSize64)
    return SZ_ERROR_MEM;

  inSize = (SizeT)inSize64;
  encBuf = (Byte *)ISzAlloc_Alloc(allocMain, inSize ? inSize : 1);
  if (!encBuf && inSize != 0)
    return SZ_ERROR_MEM;

  res = LookInStream_SeekTo(inStream, startPos + offset);
  if (res == SZ_OK)
    res = SzDecodeCopy(inSize, inStream, encBuf);
  AES_DBG("after read pack: %d\n", (int)res);
  if (res == SZ_OK)
    res = SzAes_DecodeBuf(aesCoder, propsData, encBuf, inSize);
  AES_DBG("after aes decrypt: %d\n", (int)res);
  if (res == SZ_OK)
  {
    if (decryptedMainInputSize > inSize)
      res = SZ_ERROR_DATA;
    else
      res = SzDecodeMainFromMem(mainCoder, propsData, encBuf, decryptedMainInputSize, outBuffer, outSize, allocMain);
  }
  AES_DBG("after main decode: %d\n", (int)res);
  if (res == SZ_ERROR_DATA && decryptedMainInputSize > 0)
  {
    size_t tryBase = decryptedMainInputSize;
    unsigned trim;
    if (tryBase > inSize)
      tryBase = inSize;
    /* 7zAES payload can include up to 15 padding bytes at the tail. */
    for (trim = 1; trim <= 15 && tryBase > trim; trim++)
    {
      const SRes res2 = SzDecodeMainFromMem(
          mainCoder, propsData, encBuf, tryBase - trim, outBuffer, outSize, allocMain);
      if (res2 == SZ_OK)
      {
        AES_DBG("main decode succeeded with trim=%u\n", trim);
        res = SZ_OK;
        break;
      }
    }
  }
  AES_DBG("folder final result: %d\n", (int)res);
  if (res != SZ_OK && res != SZ_ERROR_UNSUPPORTED && res != SZ_ERROR_MEM && res != SZ_ERROR_INPUT_EOF)
    res = SZ_ERROR_WRONG_PASSWORD;

  ISzAlloc_Free(allocMain, encBuf);
  return res;
}

static SRes SzFolder_DecodeAesOnly(const CSzFolder *folder,
    const Byte *propsData,
    const UInt64 *packPositions,
    ILookInStreamPtr inStream, UInt64 startPos,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain)
{
  const CSzCoderInfo *aesCoder = &folder->Coders[0];
  const UInt64 offset = packPositions[0];
  const UInt64 inSize64 = packPositions[1] - offset;
  Byte *encBuf;
  SizeT inSize;
  SRes res;

  if (folder->NumCoders != 1
      || !IS_AES_CODER(aesCoder)
      || folder->NumPackStreams != 1
      || folder->PackStreams[0] != 0
      || folder->NumBonds != 0
      || folder->UnpackStream != 0)
    return SZ_ERROR_UNSUPPORTED;
  if (inSize64 != (SizeT)inSize64)
    return SZ_ERROR_MEM;

  inSize = (SizeT)inSize64;
  if (outSize > inSize)
    return SZ_ERROR_DATA;

  encBuf = (Byte *)ISzAlloc_Alloc(allocMain, inSize ? inSize : 1);
  if (!encBuf && inSize != 0)
    return SZ_ERROR_MEM;

  res = LookInStream_SeekTo(inStream, startPos + offset);
  if (res == SZ_OK)
    res = SzDecodeCopy(inSize, inStream, encBuf);
  if (res == SZ_OK)
    res = SzAes_DecodeBuf(aesCoder, propsData, encBuf, inSize);
  if (res == SZ_OK)
    memcpy(outBuffer, encBuf, outSize);

  ISzAlloc_Free(allocMain, encBuf);
  return res;
}

#ifdef Z7_PPMD_SUPPORT

#define k_PPMD 0x30401

typedef struct
{
  IByteIn vt;
  const Byte *cur;
  const Byte *end;
  const Byte *begin;
  UInt64 processed;
  BoolInt extra;
  SRes res;
  ILookInStreamPtr inStream;
} CByteInToLook;

static Byte ReadByte(IByteInPtr pp)
{
  Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(CByteInToLook)
  if (p->cur != p->end)
    return *p->cur++;
  if (p->res == SZ_OK)
  {
    size_t size = (size_t)(p->cur - p->begin);
    p->processed += size;
    p->res = ILookInStream_Skip(p->inStream, size);
    size = (1 << 25);
    p->res = ILookInStream_Look(p->inStream, (const void **)&p->begin, &size);
    p->cur = p->begin;
    p->end = p->begin + size;
    if (size != 0)
      return *p->cur++;
  }
  p->extra = True;
  return 0;
}

static SRes SzDecodePpmd(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain)
{
  CPpmd7 ppmd;
  CByteInToLook s;
  SRes res = SZ_OK;

  s.vt.Read = ReadByte;
  s.inStream = inStream;
  s.begin = s.end = s.cur = NULL;
  s.extra = False;
  s.res = SZ_OK;
  s.processed = 0;

  if (propsSize != 5)
    return SZ_ERROR_UNSUPPORTED;

  {
    unsigned order = props[0];
    UInt32 memSize = GetUi32(props + 1);
    if (order < PPMD7_MIN_ORDER ||
        order > PPMD7_MAX_ORDER ||
        memSize < PPMD7_MIN_MEM_SIZE ||
        memSize > PPMD7_MAX_MEM_SIZE)
      return SZ_ERROR_UNSUPPORTED;
    Ppmd7_Construct(&ppmd);
    if (!Ppmd7_Alloc(&ppmd, memSize, allocMain))
      return SZ_ERROR_MEM;
    Ppmd7_Init(&ppmd, order);
  }
  {
    ppmd.rc.dec.Stream = &s.vt;
    if (!Ppmd7z_RangeDec_Init(&ppmd.rc.dec))
      res = SZ_ERROR_DATA;
    else if (!s.extra)
    {
      Byte *buf = outBuffer;
      const Byte *lim = buf + outSize;
      for (; buf != lim; buf++)
      {
        int sym = Ppmd7z_DecodeSymbol(&ppmd);
        if (s.extra || sym < 0)
          break;
        *buf = (Byte)sym;
      }
      if (buf != lim)
        res = SZ_ERROR_DATA;
      else if (!Ppmd7z_RangeDec_IsFinishedOK(&ppmd.rc.dec))
      {
        /* if (Ppmd7z_DecodeSymbol(&ppmd) != PPMD7_SYM_END || !Ppmd7z_RangeDec_IsFinishedOK(&ppmd.rc.dec)) */
        res = SZ_ERROR_DATA;
      }
    }
    if (s.extra)
      res = (s.res != SZ_OK ? s.res : SZ_ERROR_DATA);
    else if (s.processed + (size_t)(s.cur - s.begin) != inSize)
      res = SZ_ERROR_DATA;
  }
  Ppmd7_Free(&ppmd, allocMain);
  return res;
}

#endif


static SRes SzDecodeLzma(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain)
{
  CLzmaDec state;
  SRes res = SZ_OK;

  LzmaDec_CONSTRUCT(&state)
  RINOK(LzmaDec_AllocateProbs(&state, props, propsSize, allocMain))
  state.dic = outBuffer;
  state.dicBufSize = outSize;
  LzmaDec_Init(&state);

  for (;;)
  {
    const void *inBuf = NULL;
    size_t lookahead = (1 << 18);
    if (lookahead > inSize)
      lookahead = (size_t)inSize;
    res = ILookInStream_Look(inStream, &inBuf, &lookahead);
    if (res != SZ_OK)
      break;

    {
      SizeT inProcessed = (SizeT)lookahead, dicPos = state.dicPos;
      ELzmaStatus status;
      res = LzmaDec_DecodeToDic(&state, outSize, (const Byte *)inBuf, &inProcessed, LZMA_FINISH_END, &status);
      lookahead -= inProcessed;
      inSize -= inProcessed;
      if (res != SZ_OK)
        break;

      if (status == LZMA_STATUS_FINISHED_WITH_MARK)
      {
        if (outSize != state.dicPos || inSize != 0)
          res = SZ_ERROR_DATA;
        break;
      }

      if (outSize == state.dicPos && inSize == 0 && status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)
        break;

      if (inProcessed == 0 && dicPos == state.dicPos)
      {
        res = SZ_ERROR_DATA;
        break;
      }

      res = ILookInStream_Skip(inStream, inProcessed);
      if (res != SZ_OK)
        break;
    }
  }

  LzmaDec_FreeProbs(&state, allocMain);
  return res;
}


#ifndef Z7_NO_METHOD_LZMA2

static SRes SzDecodeLzma2(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain)
{
  CLzma2Dec state;
  SRes res = SZ_OK;

  Lzma2Dec_CONSTRUCT(&state)
  if (propsSize != 1)
    return SZ_ERROR_DATA;
  RINOK(Lzma2Dec_AllocateProbs(&state, props[0], allocMain))
  state.decoder.dic = outBuffer;
  state.decoder.dicBufSize = outSize;
  Lzma2Dec_Init(&state);

  for (;;)
  {
    const void *inBuf = NULL;
    size_t lookahead = (1 << 18);
    if (lookahead > inSize)
      lookahead = (size_t)inSize;
    res = ILookInStream_Look(inStream, &inBuf, &lookahead);
    if (res != SZ_OK)
      break;

    {
      SizeT inProcessed = (SizeT)lookahead, dicPos = state.decoder.dicPos;
      ELzmaStatus status;
      res = Lzma2Dec_DecodeToDic(&state, outSize, (const Byte *)inBuf, &inProcessed, LZMA_FINISH_END, &status);
      lookahead -= inProcessed;
      inSize -= inProcessed;
      if (res != SZ_OK)
        break;

      if (status == LZMA_STATUS_FINISHED_WITH_MARK)
      {
        if (outSize != state.decoder.dicPos || inSize != 0)
          res = SZ_ERROR_DATA;
        break;
      }

      if (inProcessed == 0 && dicPos == state.decoder.dicPos)
      {
        res = SZ_ERROR_DATA;
        break;
      }

      res = ILookInStream_Skip(inStream, inProcessed);
      if (res != SZ_OK)
        break;
    }
  }

  Lzma2Dec_FreeProbs(&state, allocMain);
  return res;
}

#endif


static SRes SzDecodeCopy(UInt64 inSize, ILookInStreamPtr inStream, Byte *outBuffer)
{
  while (inSize > 0)
  {
    const void *inBuf;
    size_t curSize = (1 << 18);
    if (curSize > inSize)
      curSize = (size_t)inSize;
    RINOK(ILookInStream_Look(inStream, &inBuf, &curSize))
    if (curSize == 0)
      return SZ_ERROR_INPUT_EOF;
    memcpy(outBuffer, inBuf, curSize);
    outBuffer += curSize;
    inSize -= curSize;
    RINOK(ILookInStream_Skip(inStream, curSize))
  }
  return SZ_OK;
}

static SRes SzDecodeZstd(const Byte *props, unsigned propsSize, UInt64 inSize, ILookInStreamPtr inStream,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain)
{
  SRes res = SZ_OK;
  Byte *inBuf = NULL;
  UInt64 remaining = inSize;
  size_t inSizeT;

  (void)props;
  if (!(propsSize == 0 || propsSize == 1 || propsSize == 3 || propsSize == 5))
    return SZ_ERROR_UNSUPPORTED;
  if (inSize > (UInt64)(SizeT)-1)
    return SZ_ERROR_MEM;

  inSizeT = (size_t)inSize;
  inBuf = (Byte *)ISzAlloc_Alloc(allocMain, inSizeT);
  if (!inBuf && inSizeT != 0)
    return SZ_ERROR_MEM;

  {
    Byte *cur = inBuf;
    while (remaining > 0)
    {
      const void *buf;
      size_t curSize = (1 << 18);
      if (curSize > remaining)
        curSize = (size_t)remaining;
      RINOK(ILookInStream_Look(inStream, &buf, &curSize))
      if (curSize == 0)
      {
        res = SZ_ERROR_INPUT_EOF;
        break;
      }
      memcpy(cur, buf, curSize);
      cur += curSize;
      remaining -= curSize;
      res = ILookInStream_Skip(inStream, curSize);
      if (res != SZ_OK)
        break;
    }
  }

  if (res == SZ_OK)
  {
    size_t decoded = ZSTD_decompress(outBuffer, outSize, inBuf, inSizeT);
    if (ZSTD_isError(decoded) || decoded != outSize)
      res = SZ_ERROR_DATA;
  }

  ISzAlloc_Free(allocMain, inBuf);
  return res;
}

static BoolInt IS_MAIN_METHOD(UInt32 m)
{
  switch (m)
  {
    case k_Copy:
    case k_LZMA:
  #ifndef Z7_NO_METHOD_LZMA2
    case k_LZMA2:
  #endif
    case k_ZSTD:
  #ifdef Z7_PPMD_SUPPORT
    case k_PPMD:
  #endif
      return True;
    default:
      return False;
  }
}

static BoolInt IS_SUPPORTED_CODER(const CSzCoderInfo *c)
{
  return
      c->NumStreams == 1
      /* && c->MethodID <= (UInt32)0xFFFFFFFF */
      && IS_MAIN_METHOD((UInt32)c->MethodID);
}

static BoolInt IS_AES_CODER(const CSzCoderInfo *c)
{
  return c->NumStreams == 1 && c->MethodID == k_AES;
}

#define IS_BCJ2(c) ((c)->MethodID == k_BCJ2 && (c)->NumStreams == 4)

static SRes CheckSupportedFolder(const CSzFolder *f)
{
  DebugDumpFolder(f);
  if (f->NumCoders < 1 || f->NumCoders > 4)
  {
    AES_DBG("unsupported reason: NumCoders range\n");
    return SZ_ERROR_UNSUPPORTED;
  }
  if (!IS_SUPPORTED_CODER(&f->Coders[0]))
  {
    if (f->NumCoders == 1 && IS_AES_CODER(&f->Coders[0]))
    {
      if (f->NumPackStreams != 1
          || f->PackStreams[0] != 0
          || f->NumBonds != 0
          || f->UnpackStream != 0)
      {
        AES_DBG("unsupported reason: AES-only topology\n");
        return SZ_ERROR_UNSUPPORTED;
      }
      return SZ_OK;
    }
    if (!(f->NumCoders == 2
        && IS_AES_CODER(&f->Coders[0])
        && IS_SUPPORTED_CODER(&f->Coders[1])))
    {
      AES_DBG("unsupported reason: first coder unsupported and not AES+main\n");
      return SZ_ERROR_UNSUPPORTED;
    }
  }
  if (f->NumCoders == 1)
  {
    if (f->NumPackStreams != 1 || f->PackStreams[0] != 0 || f->NumBonds != 0)
    {
      AES_DBG("unsupported reason: single-coder topology\n");
      return SZ_ERROR_UNSUPPORTED;
    }
    return SZ_OK;
  }
  
  
  #if defined(Z7_USE_BRANCH_FILTER)

  if (f->NumCoders == 2)
  {
    if ((IS_AES_CODER(&f->Coders[0]) && IS_SUPPORTED_CODER(&f->Coders[1]))
        || (IS_AES_CODER(&f->Coders[1]) && IS_SUPPORTED_CODER(&f->Coders[0])))
    {
      if (f->NumPackStreams != 1
          || f->NumBonds != 1)
      {
        AES_DBG("unsupported reason: AES pair topology\n");
        return SZ_ERROR_UNSUPPORTED;
      }
      return SZ_OK;
    }

    const CSzCoderInfo *c = &f->Coders[1];
    if (
        /* c->MethodID > (UInt32)0xFFFFFFFF || */
        c->NumStreams != 1
        || f->NumPackStreams != 1
        || f->PackStreams[0] != 0
        || f->NumBonds != 1
        || f->Bonds[0].InIndex != 1
        || f->Bonds[0].OutIndex != 0)
    {
      AES_DBG("unsupported reason: filter pair topology\n");
      return SZ_ERROR_UNSUPPORTED;
    }
    switch ((UInt32)c->MethodID)
    {
    #if !defined(Z7_NO_METHODS_FILTERS)
      case k_Delta:
      case k_BCJ:
      case k_PPC:
      case k_IA64:
      case k_SPARC:
      case k_ARM:
      case k_RISCV:
    #endif
    #ifdef Z7_USE_FILTER_ARM64
      case k_ARM64:
    #endif
    #ifdef Z7_USE_FILTER_ARMT
      case k_ARMT:
    #endif
        break;
      default:
        AES_DBG("unsupported reason: unknown secondary filter\n");
        return SZ_ERROR_UNSUPPORTED;
    }
    return SZ_OK;
  }

  #endif

  
  if (f->NumCoders == 4)
  {
    if (!IS_SUPPORTED_CODER(&f->Coders[1])
        || !IS_SUPPORTED_CODER(&f->Coders[2])
        || !IS_BCJ2(&f->Coders[3]))
    {
      AES_DBG("unsupported reason: bcj2 coder set\n");
      return SZ_ERROR_UNSUPPORTED;
    }
    if (f->NumPackStreams != 4
        || f->PackStreams[0] != 2
        || f->PackStreams[1] != 6
        || f->PackStreams[2] != 1
        || f->PackStreams[3] != 0
        || f->NumBonds != 3
        || f->Bonds[0].InIndex != 5 || f->Bonds[0].OutIndex != 0
        || f->Bonds[1].InIndex != 4 || f->Bonds[1].OutIndex != 1
        || f->Bonds[2].InIndex != 3 || f->Bonds[2].OutIndex != 2)
    {
      AES_DBG("unsupported reason: bcj2 topology\n");
      return SZ_ERROR_UNSUPPORTED;
    }
    return SZ_OK;
  }
  
  AES_DBG("unsupported reason: unmatched coder pattern\n");
  return SZ_ERROR_UNSUPPORTED;
}






static SRes SzFolder_Decode2(const CSzFolder *folder,
    const Byte *propsData,
    const UInt64 *unpackSizes,
    const UInt64 *packPositions,
    ILookInStreamPtr inStream, UInt64 startPos,
    Byte *outBuffer, SizeT outSize, ISzAllocPtr allocMain,
    Byte *tempBuf[])
{
  UInt32 ci;
  SizeT tempSizes[3] = { 0, 0, 0};
  SizeT tempSize3 = 0;
  Byte *tempBuf3 = 0;

  RINOK(CheckSupportedFolder(folder))

  if (folder->NumCoders == 2
      && ((IS_AES_CODER(&folder->Coders[0]) && IS_SUPPORTED_CODER(&folder->Coders[1]))
          || (IS_AES_CODER(&folder->Coders[1]) && IS_SUPPORTED_CODER(&folder->Coders[0]))))
    return SzFolder_DecodeAesMain(folder, propsData, unpackSizes, packPositions, inStream, startPos, outBuffer, outSize, allocMain);
  if (folder->NumCoders == 1 && IS_AES_CODER(&folder->Coders[0]))
    return SzFolder_DecodeAesOnly(folder, propsData, packPositions, inStream, startPos, outBuffer, outSize, allocMain);

  for (ci = 0; ci < folder->NumCoders; ci++)
  {
    const CSzCoderInfo *coder = &folder->Coders[ci];

    if (IS_MAIN_METHOD((UInt32)coder->MethodID))
    {
      UInt32 si = 0;
      UInt64 offset;
      UInt64 inSize;
      Byte *outBufCur = outBuffer;
      SizeT outSizeCur = outSize;
      if (folder->NumCoders == 4)
      {
        const UInt32 indices[] = { 3, 2, 0 };
        const UInt64 unpackSize = unpackSizes[ci];
        si = indices[ci];
        if (ci < 2)
        {
          Byte *temp;
          outSizeCur = (SizeT)unpackSize;
          if (outSizeCur != unpackSize)
            return SZ_ERROR_MEM;
          temp = (Byte *)ISzAlloc_Alloc(allocMain, outSizeCur);
          if (!temp && outSizeCur != 0)
            return SZ_ERROR_MEM;
          outBufCur = tempBuf[1 - ci] = temp;
          tempSizes[1 - ci] = outSizeCur;
        }
        else if (ci == 2)
        {
          if (unpackSize > outSize) /* check it */
            return SZ_ERROR_PARAM;
          tempBuf3 = outBufCur = outBuffer + (outSize - (size_t)unpackSize);
          tempSize3 = outSizeCur = (SizeT)unpackSize;
        }
        else
          return SZ_ERROR_UNSUPPORTED;
      }
      offset = packPositions[si];
      inSize = packPositions[(size_t)si + 1] - offset;
      RINOK(LookInStream_SeekTo(inStream, startPos + offset))

      if (coder->MethodID == k_Copy)
      {
        if (inSize != outSizeCur) /* check it */
          return SZ_ERROR_DATA;
        RINOK(SzDecodeCopy(inSize, inStream, outBufCur))
      }
      else if (coder->MethodID == k_LZMA)
      {
        RINOK(SzDecodeLzma(propsData + coder->PropsOffset, coder->PropsSize, inSize, inStream, outBufCur, outSizeCur, allocMain))
      }
    #ifndef Z7_NO_METHOD_LZMA2
      else if (coder->MethodID == k_LZMA2)
      {
        RINOK(SzDecodeLzma2(propsData + coder->PropsOffset, coder->PropsSize, inSize, inStream, outBufCur, outSizeCur, allocMain))
      }
    #endif
      else if (coder->MethodID == k_ZSTD)
      {
        RINOK(SzDecodeZstd(propsData + coder->PropsOffset, coder->PropsSize, inSize, inStream, outBufCur, outSizeCur, allocMain))
      }
    #ifdef Z7_PPMD_SUPPORT
      else if (coder->MethodID == k_PPMD)
      {
        RINOK(SzDecodePpmd(propsData + coder->PropsOffset, coder->PropsSize, inSize, inStream, outBufCur, outSizeCur, allocMain))
      }
    #endif
      else
        return SZ_ERROR_UNSUPPORTED;
    }
    else if (coder->MethodID == k_BCJ2)
    {
      const UInt64 offset = packPositions[1];
      const UInt64 s3Size = packPositions[2] - offset;
      
      if (ci != 3)
        return SZ_ERROR_UNSUPPORTED;
      
      tempSizes[2] = (SizeT)s3Size;
      if (tempSizes[2] != s3Size)
        return SZ_ERROR_MEM;
      tempBuf[2] = (Byte *)ISzAlloc_Alloc(allocMain, tempSizes[2]);
      if (!tempBuf[2] && tempSizes[2] != 0)
        return SZ_ERROR_MEM;
      
      RINOK(LookInStream_SeekTo(inStream, startPos + offset))
      RINOK(SzDecodeCopy(s3Size, inStream, tempBuf[2]))

      if ((tempSizes[0] & 3) != 0 ||
          (tempSizes[1] & 3) != 0 ||
          tempSize3 + tempSizes[0] + tempSizes[1] != outSize)
        return SZ_ERROR_DATA;

      {
        CBcj2Dec p;
        
        p.bufs[0] = tempBuf3;   p.lims[0] = tempBuf3 + tempSize3;
        p.bufs[1] = tempBuf[0]; p.lims[1] = tempBuf[0] + tempSizes[0];
        p.bufs[2] = tempBuf[1]; p.lims[2] = tempBuf[1] + tempSizes[1];
        p.bufs[3] = tempBuf[2]; p.lims[3] = tempBuf[2] + tempSizes[2];
        
        p.dest = outBuffer;
        p.destLim = outBuffer + outSize;
        
        Bcj2Dec_Init(&p);
        RINOK(Bcj2Dec_Decode(&p))

        {
          unsigned i;
          for (i = 0; i < 4; i++)
            if (p.bufs[i] != p.lims[i])
              return SZ_ERROR_DATA;
          if (p.dest != p.destLim || !Bcj2Dec_IsMaybeFinished(&p))
            return SZ_ERROR_DATA;
        }
      }
    }
#if defined(Z7_USE_BRANCH_FILTER)
    else if (ci == 1)
    {
#if !defined(Z7_NO_METHODS_FILTERS)
      if (coder->MethodID == k_Delta)
      {
        if (coder->PropsSize != 1)
          return SZ_ERROR_UNSUPPORTED;
        {
          Byte state[DELTA_STATE_SIZE];
          Delta_Init(state);
          Delta_Decode(state, (unsigned)(propsData[coder->PropsOffset]) + 1, outBuffer, outSize);
        }
        continue;
      }
#endif
     
#ifdef Z7_USE_FILTER_ARM64
      if (coder->MethodID == k_ARM64)
      {
        UInt32 pc = 0;
        if (coder->PropsSize == 4)
        {
          pc = GetUi32(propsData + coder->PropsOffset);
          if (pc & 3)
            return SZ_ERROR_UNSUPPORTED;
        }
        else if (coder->PropsSize != 0)
          return SZ_ERROR_UNSUPPORTED;
        z7_BranchConv_ARM64_Dec(outBuffer, outSize, pc);
        continue;
      }
#endif

#if !defined(Z7_NO_METHODS_FILTERS)
      if (coder->MethodID == k_RISCV)
      {
        UInt32 pc = 0;
        if (coder->PropsSize == 4)
        {
          pc = GetUi32(propsData + coder->PropsOffset);
          if (pc & 1)
            return SZ_ERROR_UNSUPPORTED;
        }
        else if (coder->PropsSize != 0)
          return SZ_ERROR_UNSUPPORTED;
        z7_BranchConv_RISCV_Dec(outBuffer, outSize, pc);
        continue;
      }
#endif
      
#if !defined(Z7_NO_METHODS_FILTERS) || defined(Z7_USE_FILTER_ARMT)
      {
        if (coder->PropsSize != 0)
          return SZ_ERROR_UNSUPPORTED;
       #define CASE_BRA_CONV(isa) case k_ ## isa: Z7_BRANCH_CONV_DEC(isa)(outBuffer, outSize, 0); break; // pc = 0;
        switch (coder->MethodID)
        {
         #if !defined(Z7_NO_METHODS_FILTERS)
          case k_BCJ:
          {
            UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
            z7_BranchConvSt_X86_Dec(outBuffer, outSize, 0, &state); // pc = 0
            break;
          }
          case k_PPC: Z7_BRANCH_CONV_DEC_2(BranchConv_PPC)(outBuffer, outSize, 0); break; // pc = 0;
          // CASE_BRA_CONV(PPC)
          CASE_BRA_CONV(IA64)
          CASE_BRA_CONV(SPARC)
          CASE_BRA_CONV(ARM)
         #endif
         #if !defined(Z7_NO_METHODS_FILTERS) || defined(Z7_USE_FILTER_ARMT)
          CASE_BRA_CONV(ARMT)
         #endif
          default:
            return SZ_ERROR_UNSUPPORTED;
        }
        continue;
      }
#endif
    } // (c == 1)
#endif // Z7_USE_BRANCH_FILTER
    else
      return SZ_ERROR_UNSUPPORTED;
  }

  return SZ_OK;
}


SRes SzAr_DecodeFolder(const CSzAr *p, UInt32 folderIndex,
    ILookInStreamPtr inStream, UInt64 startPos,
    Byte *outBuffer, size_t outSize,
    ISzAllocPtr allocMain)
{
  SRes res;
  CSzFolder folder;
  CSzData sd;
  
  const Byte *data = p->CodersData + p->FoCodersOffsets[folderIndex];
  sd.Data = data;
  sd.Size = p->FoCodersOffsets[(size_t)folderIndex + 1] - p->FoCodersOffsets[folderIndex];
  
  res = SzGetNextFolderItem(&folder, &sd);
  
  if (res != SZ_OK)
    return res;

  if (sd.Size != 0
      || folder.UnpackStream != p->FoToMainUnpackSizeIndex[folderIndex]
      || outSize != SzAr_GetFolderUnpackSize(p, folderIndex))
    return SZ_ERROR_FAIL;
  {
    unsigned i;
    Byte *tempBuf[3] = { 0, 0, 0};

    res = SzFolder_Decode2(&folder, data,
        &p->CoderUnpackSizes[p->FoToCoderUnpackSizes[folderIndex]],
        p->PackPositions + p->FoStartPackStreamIndex[folderIndex],
        inStream, startPos,
        outBuffer, (SizeT)outSize, allocMain, tempBuf);
    
    for (i = 0; i < 3; i++)
      ISzAlloc_Free(allocMain, tempBuf[i]);

    if (res == SZ_OK)
      if (SzBitWithVals_Check(&p->FolderCRCs, folderIndex))
        if (CrcCalc(outBuffer, outSize) != p->FolderCRCs.Vals[folderIndex])
          res = SZ_ERROR_CRC;

    return res;
  }
}
