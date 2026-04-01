/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 *
 * Advanced Technology Group (ATG)
 * Author(s):   Pavel Martishevsky (pamartis@microsoft.com)
 */

#include "zstdgpu_reference_store.h"

#include <memory.h>
#include <stdlib.h>
#include <assert.h>
#include "zstdgpu_structs.h"

#define ZSTDGPU_DISABLE_RESOURCE_DATA_GPU 1
#include "zstdgpu_resources.h"

static const uint8_t *GBase = 0;

typedef uint32_t (*zstdgpu_FseElemOffsetFn)(uint32_t fseTableIndex, uint32_t cmpBlockCount);

static uint32_t GFrameCount = 0;

static uint32_t GBlockCountRAW = 0;
static uint32_t GBlockCountRLE = 0;
static uint32_t GBlockCountCMP = 0;
static uint32_t GZstdDataSize  = 0;

static zstdgpu_ResourceDataCpu GZstd;

static uint32_t GBlockIndexRAW = 0;
static uint32_t GBlockIndexRLE = 0;
static uint32_t GBlockIndexCMP = 0;

static uint32_t GHufCompressedLiteralCount = 0;
static uint32_t GHufDecompressedLiteralOffset = 0;
static uint32_t GHufDecompressedLiteralSize = 0;
static uint32_t GHufDecompressedLiteralStreamCount = 0;
static uint32_t GHufDecompressedLiteralDataOffset = 0;

static uint32_t GFseProbTableIndexHufW = 0;
static uint32_t GFseProbTableIndexLLen = 1; //< initialise to "1" because we reserve index "0" for default table
static uint32_t GFseProbTableIndexOffs = 1; //< initialise to "1" because we reserve index "0" for default table
static uint32_t GFseProbTableIndexMLen = 1; //< initialise to "1" because we reserve index "0" for default table

static uint32_t GLastTableIndex_FseHufW = 0;
static uint32_t GLastTableIndex_FseLLen = 0;
static uint32_t GLastTableIndex_FseOffs = 0;
static uint32_t GLastTableIndex_FseMLen = 0;

static uint32_t GFseProbDefaultTableLLenStored = 0; //< initialise to "0" to indicate the tables weren't stored yet
static uint32_t GFseProbDefaultTableOffsStored = 0; //< initialise to "0" to indicate the tables weren't stored yet
static uint32_t GFseProbDefaultTableMLenStored = 0; //< initialise to "0" to indicate the tables weren't stored yet

static uint32_t GFseCompressedHuffmanWeightCount = 0;
static uint32_t GUnCompressedHuffmanWeightCount = 0;
static uint32_t GSequenceStreamCount = 0;
static uint32_t GSequenceCount = 0;

static ZSTDGPU_ENUM(ReferenceStore_FseType) GFseProbTableTypePending = ZSTDGPU_ENUM_CONST(ReferenceStore_FseForceInt);
static uint32_t GCompressedBlockLiteralStreamIndex = 0;

static inline uint32_t izstdgpu_ReferenceStore_PtrToOffs(const void *base)
{
    // Force cast to 32-bit because we never get offsets greater than 4GB (???)
    return (uint32_t)((const uint8_t *)base - GBase);
}

void zstdgpu_ReferenceStore_Report_ChunkBase(const void *base)
{
    GBase = (const uint8_t *)base;
}

void zstdgpu_ReferenceStore_Report_FrameAndBlockCount(uint32_t frameCount, uint32_t rawBlockCount, uint32_t rleBlockCount, uint32_t cmpBlockCount, uint32_t zstdDataSize)
{
    GFrameCount     = frameCount;
    GBlockCountRAW  = rawBlockCount;
    GBlockCountRLE  = rleBlockCount;
    GBlockCountCMP  = cmpBlockCount;
    GZstdDataSize   = zstdDataSize;
}

static zstdgpu_ResourceInfo GZstdInfo;

void zstdgpu_ReferenceStore_AllocateMemory(void)
{
    zstdgpu_ResourceInfo_Stage_0_Init(&GZstdInfo, GFrameCount, GZstdDataSize, 0);
    zstdgpu_ResourceInfo_Stage_1_Init(&GZstdInfo, GBlockCountRAW, GBlockCountRLE, GBlockCountCMP);
    zstdgpu_ResourceInfo_Stage_2_Init(&GZstdInfo, 4 * 1024 * 1024 /*literal count*/, 4 * 1024 * 1024 /*sequence count*/, 0, 0);

    zstdgpu_ResourceDataCpu_InitZero(&GZstd);
    zstdgpu_ResourceDataCpu_InitFromHeap(&GZstd, &GZstdInfo);

    // Pre-populate 256 dense RLE entries at the beginning of FSE element buffers
    for (uint32_t i = 0; i < kzstdgpu_FseRleTableCount; ++i)
    {
        GZstd.FseElems[i] = zstdgpu_PackFseElem(i, 0, 0);
        GZstd.FseInfos[i].fseProbCountAndAccuracyLog2 = 0;
    }
}

void zstdgpu_ReferenceStore_FreeMemory(void)
{
    zstdgpu_ResourceDataCpu_Term(&GZstd);
}

static uint32_t zstdgpu_GetLastBlockIndex(void)
{
    const uint32_t allBlockCount = GBlockIndexRAW
                                 + GBlockIndexRLE
                                 + GBlockIndexCMP;

    ZSTDGPU_ASSERT(allBlockCount >= 1u);

    return allBlockCount - 1u;
}

static uint32_t zstdgpu_SetLastBlockSize(uint32_t size)
{
    const uint32_t lastBlockIndex = zstdgpu_GetLastBlockIndex();
    GZstd.BlockSizePrefix[lastBlockIndex] = size;

    if (lastBlockIndex >= 1)
    {
        GZstd.BlockSizePrefix[lastBlockIndex] += GZstd.BlockSizePrefix[lastBlockIndex - 1];
    }
    return lastBlockIndex;
}

static void zstdgpu_AppendLastBlockSize(uint32_t size)
{
    GZstd.BlockSizePrefix[zstdgpu_GetLastBlockIndex()] += size;
}


void zstdgpu_ReferenceStore_Report_Block(const void *base, uint32_t size, ZSTDGPU_ENUM(ReferenceStore_BlockType) type)
{
#define APPEND(TYPE, type, base, size)                                  \
    if (type == kzstdgpu_ReferenceStore_Block##TYPE)                          \
    {                                                                   \
        GZstd.Blocks##TYPE##Refs[GBlockIndex##TYPE].offs = izstdgpu_ReferenceStore_PtrToOffs(base);\
        GZstd.Blocks##TYPE##Refs[GBlockIndex##TYPE].size = size;        \
        GBlockIndex##TYPE += 1;                                         \
    }

    APPEND(RAW, type, base, size);
    APPEND(RLE, type, base, size);
    APPEND(CMP, type, base, size);
#undef APPEND
    uint32_t lastBlockIndex = zstdgpu_SetLastBlockSize(type == ZSTDGPU_ENUM_CONST(ReferenceStore_BlockCMP) ? 0 : size);
    if (type == ZSTDGPU_ENUM_CONST(ReferenceStore_BlockCMP))
    {
        zstdgpu_Init_CompressedBlockData(GZstd.CompressedBlocks[GBlockIndexCMP - 1]);
        GCompressedBlockLiteralStreamIndex = 0;
        GZstd.GlobalBlockIndexPerCmpBlock[GBlockIndexCMP - 1] = lastBlockIndex;
    }
}

void zstdgpu_ReferenceStore_Report_RawLiteral(const void *base, uint32_t size)
{
    GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.offs = zstdgpu_EncodeRawLitOffset(izstdgpu_ReferenceStore_PtrToOffs(base));
    GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.size = size;
    GZstd.CompressedBlocks[GBlockIndexCMP - 1].litStreamIndex = ~0u;
    zstdgpu_AppendLastBlockSize(size);
}

void zstdgpu_ReferenceStore_Report_RleLiteral(const void *base, uint32_t size)
{
    GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.offs = zstdgpu_EncodeRleLitOffset(*(uint8_t *)base);
    GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.size = size;
    GZstd.CompressedBlocks[GBlockIndexCMP - 1].litStreamIndex = ~0u;
    zstdgpu_AppendLastBlockSize(size);
}

void zstdgpu_ReferenceStore_Report_CompressedLiteral(const void* base, uint32_t size)
{
    uint32_t hufCompressedLiteralIndex = GHufCompressedLiteralCount ++;
    if (GCompressedBlockLiteralStreamIndex == 0)
    {
        GZstd.CompressedBlocks[GBlockIndexCMP - 1].litStreamIndex = hufCompressedLiteralIndex;
        GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.offs = zstdgpu_EncodeCmpLitOffset(GHufDecompressedLiteralOffset);
        GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.size = GHufDecompressedLiteralSize;
    }

    GZstd.LitRefs[hufCompressedLiteralIndex].src.offs = izstdgpu_ReferenceStore_PtrToOffs(base);
    GZstd.LitRefs[hufCompressedLiteralIndex].src.size = size;

    GZstd.LitStreamBuckets[hufCompressedLiteralIndex].huffmanBucketIndex = GZstd.CompressedBlocks[GBlockIndexCMP - 1].fseTableIndexHufW;
    GZstd.LitStreamBuckets[hufCompressedLiteralIndex].huffmanBucketOffset = 0;

    if (GHufDecompressedLiteralStreamCount == 1)
    {
        GZstd.LitRefs[hufCompressedLiteralIndex].dst.offs = GHufDecompressedLiteralOffset;
        GZstd.LitRefs[hufCompressedLiteralIndex].dst.size = GHufDecompressedLiteralSize;
    }
    else
    {
        const uint32_t dstSizeOver4 = (GHufDecompressedLiteralSize + 3) / 4;
        GZstd.LitRefs[hufCompressedLiteralIndex].dst.offs = GHufDecompressedLiteralOffset + GCompressedBlockLiteralStreamIndex * dstSizeOver4;

        if (GCompressedBlockLiteralStreamIndex < GHufDecompressedLiteralStreamCount - 1)
        {
            GZstd.LitRefs[hufCompressedLiteralIndex].dst.size = dstSizeOver4;
        }
        else
        {
            GZstd.LitRefs[hufCompressedLiteralIndex].dst.size = GHufDecompressedLiteralSize - 3 * dstSizeOver4;
        }
    }

    if (GCompressedBlockLiteralStreamIndex == GHufDecompressedLiteralStreamCount - 1)
    {
        // when it's last literal, update the decompressed offset
        GHufDecompressedLiteralOffset += GHufDecompressedLiteralSize;
    }
    GCompressedBlockLiteralStreamIndex += 1;
}

void zstdgpu_ReferenceStore_Report_CompressedLiteralDecompressedSizeAndStreamCount(uint32_t size, uint32_t streamCount)
{
    GHufDecompressedLiteralSize = size;
    GHufDecompressedLiteralStreamCount = streamCount;
    zstdgpu_AppendLastBlockSize(size);
}

void zstdgpu_ReferenceStore_Report_CompressedLiteralDecomressedSize(uint32_t size)
{
    if (GHufDecompressedLiteralStreamCount == 1)
    {
        ZSTDGPU_ASSERT(GZstd.LitRefs[GHufCompressedLiteralCount - 1].dst.size == size);
    }
    else
    {
        ZSTDGPU_ASSERT(GZstd.LitRefs[GHufCompressedLiteralCount - 1].dst.size == size);
    }
}

void zstdgpu_ReferenceStore_Report_DecompressedLiteral(const uint8_t *literal, uint32_t size)
{
    ZSTDGPU_ASSERT(GHufDecompressedLiteralDataOffset + size == GHufDecompressedLiteralOffset);
    ZSTDGPU_ASSERT(size == GHufDecompressedLiteralSize);

    if (GHufDecompressedLiteralDataOffset + size > GZstdInfo.DecompressedLiterals_Count)
    {
        const uint32_t DecompressedLiterals_Count_New = (GHufDecompressedLiteralDataOffset + size) << 1u;
        void * DecompressedLiterals_New = alloc(DecompressedLiterals_Count_New);

        memcpy(DecompressedLiterals_New, GZstd.DecompressedLiterals, GHufDecompressedLiteralDataOffset);

        dealloc(GZstd.DecompressedLiterals);

        GZstd.DecompressedLiterals = (uint8_t *)DecompressedLiterals_New;
        GZstdInfo.DecompressedLiterals_Count = DecompressedLiterals_Count_New;
    }

    memcpy(GZstd.DecompressedLiterals + GHufDecompressedLiteralDataOffset, literal, size);
    ZSTDGPU_ASSERT(GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.offs == zstdgpu_EncodeCmpLitOffset(GHufDecompressedLiteralDataOffset));
    ZSTDGPU_ASSERT(GZstd.CompressedBlocks[GBlockIndexCMP - 1].literal.size == size);
    GHufDecompressedLiteralDataOffset += size;
}

void zstdgpu_ReferenceStore_Report_FseProbTableType(ZSTDGPU_ENUM(ReferenceStore_FseType) type)
{
    ZSTDGPU_ASSERT(type != ZSTDGPU_ENUM_CONST(ReferenceStore_FseForceInt));
    GFseProbTableTypePending = type;
}

#include <intrin.h>

void zstdgpu_ReferenceStore_Report_FseTable(const int16_t *probs, uint32_t symCount, const uint8_t *symbol, const uint8_t *bitcnt, const uint16_t *nstate, uint32_t accuracyLog2)
{
    ZSTDGPU_ASSERT(symCount < kzstdgpu_MaxCount_FseProbs);
    const uint32_t elemCount = 1u << accuracyLog2;

    #if 0
    uint8_t *testBitcnt = MALLOC_T(uint8_t, elemCount);
    uint32_t *bits[8];
    for (uint32_t i = 0; i < 8; ++i)
    {
        bits[i] = MALLOC_T(uint32_t, (elemCount + 31) / 32);
        memset(bits[i], 0, sizeof(uint32_t) * ((elemCount + 31) / 32));
    }
    for (uint32_t i = 0; i < elemCount; ++i)
    {
        const uint8_t s = symbol[i];
        for (uint32_t j = 0; j < 8; ++j)
        {
            bits[j][i >> 5] |= ((s >> j) & 0x1) << (i & 0x1fu);
        }
    }

    for (uint32_t i = 0; i < elemCount; ++i)
    {
        const uint32_t uintIdx = i >> 5;
        const uint32_t uintOfs = i & 0x1fu;

        const uint32_t s = symbol[i];


        uint32_t bitCnt = 0;

        for (uint32_t k = 0; k < uintIdx; ++k)
        {
            uint32_t mask = ~0u;

            for (uint32_t j = 0; j < 8; ++j)
            {
                const uint32_t *bitsJ = bits[j];

                mask &= ((s >> j) & 0x1) != 0 ? bitsJ[k] : ~bitsJ[k];
            }
            bitCnt += _mm_popcnt_u32(mask);
        }

        uint32_t mask = (1u << uintOfs) - 1;//~0u >> (32 - uintOfs);
        for (uint32_t j = 0; j < 8; ++j)
        {
            const uint32_t *bitsJ = bits[j];

            mask &= ((s >> j) & 0x1) != 0 ? bitsJ[uintIdx] : ~bitsJ[uintIdx];
        }
        bitCnt += _mm_popcnt_u32(mask);
        bitCnt += probs[s] > 0 ? (uint32_t)probs[s] : 1;

        bitCnt = accuracyLog2 - zstdgpu_FindFirstBitHiU32(bitCnt);
        ZSTDGPU_ASSERT(bitCnt == bitcnt[i]);
    }

    for (uint32_t i = 0; i < 8; ++i)
    {
        free(bits[i]);
    }
    free(testBitcnt);
    #endif

#define STORE(name, storeExpr)                                                                              \
    if (GFseProbTableTypePending == kzstdgpu_ReferenceStore_Fse##name)                                      \
    {                                                                                                       \
        const uint32_t localIdx = GFseProbTableIndex##name++;                                               \
        const uint32_t fseIndex = zstdgpu_ComputeFseIndex##name(localIdx, GBlockCountCMP);                  \
        const uint32_t fseElemStart = zstdgpu_ComputeFseDataStart##name(localIdx, GBlockCountCMP);          \
        const uint32_t fseProbStart = (fseIndex - kzstdgpu_FseRleTableCount) * kzstdgpu_MaxCount_FseProbs;  \
        GZstd.CompressedBlocks[GBlockIndexCMP - 1].fseTableIndex##name = storeExpr;                         \
        GLastTableIndex_Fse##name = storeExpr;                                                              \
        GZstd.FseInfos[fseIndex] = zstdgpu_CreateFseInfo(symCount, accuracyLog2);                           \
        memcpy(&GZstd.FseProbs[fseProbStart], probs, sizeof(probs[0]) * symCount);                          \
        for (uint32_t e = 0; e < elemCount; ++e)                                                            \
            GZstd.FseElems[fseElemStart + e] = zstdgpu_PackFseElem(symbol[e], bitcnt[e], nstate[e]);        \
    }

    STORE(HufW, localIdx)
    else
    STORE(LLen, fseIndex)
    else
    STORE(Offs, fseIndex)
    else
    STORE(MLen, fseIndex)
    else
    {
        __debugbreak();
    }
#undef STORE
}

void zstdgpu_ReferenceStore_Report_FseDefaultTable(const int16_t *probs, uint32_t symCount, const uint8_t *symbol, const uint8_t *bitcnt, const uint16_t *nstate, uint32_t accuracyLog2)
{
    ZSTDGPU_ASSERT(symCount < kzstdgpu_MaxCount_FseProbs);
    const uint32_t elemCount = 1u << accuracyLog2;

#define STORE(name)                                                                                         \
    if (GFseProbTableTypePending == kzstdgpu_ReferenceStore_Fse##name)                                      \
    {                                                                                                       \
        const uint32_t fseIndex = zstdgpu_ComputeFseIndex##name(0, GBlockCountCMP);                         \
        const uint32_t fseElemStart = zstdgpu_ComputeFseDataStart##name(0, GBlockCountCMP);                 \
        const uint32_t fseProbStart = (fseIndex - kzstdgpu_FseRleTableCount) * kzstdgpu_MaxCount_FseProbs;  \
        GZstd.CompressedBlocks[GBlockIndexCMP - 1].fseTableIndex##name = fseIndex;                          \
        GLastTableIndex_Fse##name = fseIndex;                                                               \
        if (GFseProbDefaultTable##name##Stored == 0)                                                        \
        {                                                                                                   \
            GZstd.FseInfos[fseIndex] = zstdgpu_CreateFseInfo(symCount, accuracyLog2);                       \
            memcpy(&GZstd.FseProbs[fseProbStart], probs, sizeof(probs[0]) * symCount);                      \
            for (uint32_t e = 0; e < elemCount; ++e)                                                        \
                GZstd.FseElems[fseElemStart + e] = zstdgpu_PackFseElem(symbol[e], bitcnt[e], nstate[e]);    \
            GFseProbDefaultTable##name##Stored = 1;                                                         \
        }                                                                                                   \
    }

    STORE(LLen)
    else
    STORE(Offs)
    else
    STORE(MLen)
    else
    {
        __debugbreak();
    }
#undef STORE
}

void zstdgpu_ReferenceStore_Report_FseProbSymbol(uint32_t symbol)
{
    ZSTDGPU_ASSERT(symbol < kzstdgpu_MaxCount_FseProbs);

#define STORE(name)                                                                 \
    if (GFseProbTableTypePending == kzstdgpu_ReferenceStore_Fse##name)              \
    {                                                                               \
        GZstd.CompressedBlocks[GBlockIndexCMP - 1].fseTableIndex##name = symbol;    \
        GLastTableIndex_Fse##name = symbol;                                         \
    }

    STORE(LLen)
    else
    STORE(Offs)
    else
    STORE(MLen)
    else
    {
        __debugbreak();
    }
#undef STORE
}

void zstdgpu_ReferenceStore_Report_FseProbRepeatTable()
{
#define STORE(name)                                                     \
    if (GFseProbTableTypePending == kzstdgpu_ReferenceStore_Fse##name)        \
    {                                                                   \
        GZstd.CompressedBlocks[GBlockIndexCMP - 1].fseTableIndex##name = GLastTableIndex_Fse##name;   \
    }
    STORE(HufW)
    else
    STORE(LLen)
    else
    STORE(Offs)
    else
    STORE(MLen)
    else
    {
        __debugbreak();
    }
#undef STORE
}

void zstdgpu_ReferenceStore_Report_FseCompressedHuffmanWeightSubBlock(const void* base, uint32_t size)
{
    uint32_t index = GFseCompressedHuffmanWeightCount ++;

    GZstd.HufRefs[index].offs = izstdgpu_ReferenceStore_PtrToOffs(base);
    GZstd.HufRefs[index].size = size;

    GZstd.DecompressedHuffmanWeightCount[index] = 0;
}

void zstdgpu_ReferenceStore_Report_HuffmanWeightSubBlock(const void *base, uint32_t size, uint32_t weightCount)
{
    uint32_t index = GUnCompressedHuffmanWeightCount ++;
    index = GBlockCountCMP - 1u - index;

    GZstd.HufRefs[index].offs = izstdgpu_ReferenceStore_PtrToOffs(base);
    GZstd.HufRefs[index].size = size;

    ZSTDGPU_ASSERT(weightCount < kzstdgpu_MaxCount_HuffmanWeights);
    GZstd.DecompressedHuffmanWeightCount[index] = (uint8_t)weightCount;

    GZstd.CompressedBlocks[GBlockIndexCMP - 1].fseTableIndexHufW = index;
    GLastTableIndex_FseHufW = index;
}

void zstdgpu_ReferenceStore_Report_DecompressedHuffmanWeights(const uint8_t *weights, uint32_t weightCount)
{
    uint32_t index = GFseCompressedHuffmanWeightCount - 1;

    memcpy(&GZstd.DecompressedHuffmanWeights[index * kzstdgpu_MaxCount_HuffmanWeights], weights, weightCount);

    ZSTDGPU_ASSERT(GZstd.DecompressedHuffmanWeightCount[index] == 0);
    ZSTDGPU_ASSERT(weightCount < kzstdgpu_MaxCount_HuffmanWeights);
    GZstd.DecompressedHuffmanWeightCount[index] = (uint8_t)weightCount;
}

void zstdgpu_ReferenceStore_Report_UncompressedHuffmanWeights(const uint8_t *weights, uint32_t weightCount)
{
    uint32_t index = GUnCompressedHuffmanWeightCount - 1;
    index = GBlockCountCMP - 1u - index;

    memcpy(&GZstd.DecompressedHuffmanWeights[index * kzstdgpu_MaxCount_HuffmanWeights], weights, weightCount);

    ZSTDGPU_ASSERT(GZstd.DecompressedHuffmanWeightCount[index] == weightCount);
    ZSTDGPU_ASSERT(weightCount < kzstdgpu_MaxCount_HuffmanWeights);
    GZstd.DecompressedHuffmanWeightCount[index] = (uint8_t)weightCount;
}

void zstdgpu_ReferenceStore_Report_CompressedSequences(const void *base, uint32_t size, uint32_t sequenceCount)
{
    ZSTDGPU_UNUSED(sequenceCount);
    const uint32_t i = GSequenceStreamCount ++;
    GZstd.SeqRefs[i].src.offs = izstdgpu_ReferenceStore_PtrToOffs(base);
    GZstd.SeqRefs[i].src.size = size;

    GZstd.PerSeqStreamSeqStart[i] = GSequenceCount;

    const uint32_t blockId = zstdgpu_GetLastBlockIndex();
    const uint32_t cmpBlockId = GBlockIndexCMP - 1;
    GZstd.SeqRefs[i].fseLLen = GZstd.CompressedBlocks[cmpBlockId].fseTableIndexLLen;
    GZstd.SeqRefs[i].fseOffs = GZstd.CompressedBlocks[cmpBlockId].fseTableIndexOffs;
    GZstd.SeqRefs[i].fseMLen = GZstd.CompressedBlocks[cmpBlockId].fseTableIndexMLen;
    GZstd.SeqRefs[i].blockId = blockId;
    GZstd.CompressedBlocks[cmpBlockId].seqStreamIndex = i;
}

__pragma(warning(push))
__pragma(warning(disable : 4505))
static uint32_t zstdgpu_SequenceOffsets_Update(ZSTDGPU_PARAM_INOUT(uint32_t) offset1,
                                               ZSTDGPU_PARAM_INOUT(uint32_t) offset2,
                                               ZSTDGPU_PARAM_INOUT(uint32_t) offset3,
                                               uint32_t offset,
                                               uint32_t llen)
{
    uint32_t actualOffset = offset;
    if (offset > 3u)
    {
        offset3 = offset2;
        offset2 = offset1;
        offset1 = offset;
    }
    else
    {
        if (llen != 0)
        {
            if (offset == 1u)
            {
                actualOffset = offset1;
                //offset3 = offset3
                //offset2 = offset2
                //offset1 = actualOffset1
            }
            else if (offset == 2u)
            {
                actualOffset = offset2;
                //offset3 = offset3
                offset2 = offset1;
                offset1 = actualOffset;
            }
            else
            {
                actualOffset = offset3;
                offset3 = offset2;
                offset2 = offset1;
                offset1 = actualOffset;
            }
        }
        else
        {
            if (offset == 1u)
            {
                actualOffset = offset2;
                //offset3 = offset3
                offset2 = offset1;
                offset1 = actualOffset;
            }
            else if (offset == 2u)
            {
                actualOffset = offset3;
                offset3 = offset2;
                offset2 = offset1;
                offset1 = actualOffset;
            }
            else
            {
                if (offset1 > 3u)
                {
                    // case 1: if we have actual offset (bit 31 is 0) -- we subtract a byte
                    // case 2: if the offset is "repeat offset" (bit 31 is 1, bits 29 and 30 encode previous "repeat offset")
                    //         which depending on previous block. We keep subtracting bytes
                    actualOffset = offset1 - 1u;
                }
                else if (offset1 > 0) // we don't have valid offset, but we have to subtract one byte, so we re-encode "repeat offset"
                {
                    // in the encoding
                    //      - we set offs[31] bit to 1 to mark this uint32_t as "encoded"
                    //      - we set offs[30:29] to the "repeated offset"
                    //      - we set all other bits to 1, so it behaves as -1 (which is a starting bit)
                    actualOffset = zstdgpu_EncodeSeqRepeatOffset(offset1);
                }
                else
                {
#ifndef __hlsl_dx_compiler
                    // offset must no be zero
                    __debugbreak();
#endif
                }
                offset3 = offset2;
                offset2 = offset1;
                offset1 = actualOffset;
            }
        }
    }
    return actualOffset;
}

__pragma(warning(pop))

static uint32_t zstdgpu_SequenceOffsets_Update2(ZSTDGPU_PARAM_INOUT(uint32_t) offset1,
                                                ZSTDGPU_PARAM_INOUT(uint32_t) offset2,
                                                ZSTDGPU_PARAM_INOUT(uint32_t) offset3,
                                                uint32_t offset,
                                                uint32_t llen)
{
    uint32_t encodedOffset = offset;
    if (offset <= 3u)
    {
        offset += llen == 0u ? 1u : 0u;
#if 0
        if (offset == 4u)
        {
            // case 1: if we have an actual offset (bit 31 is 0) -- we subtract a byte
            // case 2: if the offset is "repeat offset" (bit 31 is 1, bits 29 and 30 encode previous "repeat offset")
            //         which depending on previous block. We keep subtracting bytes
            encodedOffset = offset1 - 1u;
        }
        else
        {
            encodedOffset = (offset == 3u) ? offset3 : ((offset == 2u) ? offset2 : offset1);
        }
#else
        encodedOffset = (offset < 3u) ? (offset < 2u ? offset1 : offset2) : (offset < 4u ? offset3 : (offset1 - 1u));
#endif
    }

    offset3 = offset >= 3u ? offset2 : offset3;
    offset2 = offset >= 2u ? offset1 : offset2;
    offset1 = encodedOffset;
    return encodedOffset;
}

static uint32_t GResolvedOffsetResetFrame = 0;

static uint32_t GRecentOffset1 = 0u; // initialize to "invalid" offset that can't happen
static uint32_t GRecentOffset2 = 0u; // initialize to "invalid" offset that can't happen
static uint32_t GRecentOffset3 = 0u; // initialize to "invalid" offset that can't happen

void zstdgpu_ReferenceStore_Report_DecompressedSequences(const uint32_t *sequences, uint32_t sequenceCount)
{
    const uint32_t i = GSequenceStreamCount - 1u;
    //ZSTDGPU_ASSERT(GSequenceCount == sequenceCount);
    const uint32_t seqOffs = GZstd.PerSeqStreamSeqStart[i];

#define ENABLE_OFFSET_PROPAGATION 1

#if ENABLE_OFFSET_PROPAGATION
    // these are recent offsets "per-block"
    uint32_t recent1 = zstdgpu_EncodeSeqRepeatOffset(1u);
    uint32_t recent2 = zstdgpu_EncodeSeqRepeatOffset(2u);
    uint32_t recent3 = zstdgpu_EncodeSeqRepeatOffset(3u);

    // these are recent offsets "cross-block" and at the same time "cross-frame"
    if (GResolvedOffsetResetFrame > 0u)
    {
        GRecentOffset1 = 1u + 3u;
        GRecentOffset2 = 4u + 3u;
        GRecentOffset3 = 8u + 3u;
        GResolvedOffsetResetFrame = 0u;
    }
#endif

    if (seqOffs + sequenceCount > GZstdInfo.DecompressedSequenceLLen_Count)
    {
        const uint32_t DecompressedSequence_Count_New = (seqOffs + sequenceCount) << 1u;
        uint32_t *DecompressedSequenceLLen_New = (uint32_t *)alloc(DecompressedSequence_Count_New * sizeof(GZstd.DecompressedSequenceLLen[0]));
        uint32_t *DecompressedSequenceOffs_New = (uint32_t *)alloc(DecompressedSequence_Count_New * sizeof(GZstd.DecompressedSequenceOffs[0]));
        uint32_t *DecompressedSequenceMLen_New = (uint32_t *)alloc(DecompressedSequence_Count_New * sizeof(GZstd.DecompressedSequenceMLen[0]));

        memcpy(DecompressedSequenceLLen_New, GZstd.DecompressedSequenceLLen, seqOffs * sizeof(GZstd.DecompressedSequenceLLen[0]));
        memcpy(DecompressedSequenceOffs_New, GZstd.DecompressedSequenceOffs, seqOffs * sizeof(GZstd.DecompressedSequenceOffs[0]));
        memcpy(DecompressedSequenceMLen_New, GZstd.DecompressedSequenceMLen, seqOffs * sizeof(GZstd.DecompressedSequenceMLen[0]));

        dealloc(GZstd.DecompressedSequenceLLen);
        dealloc(GZstd.DecompressedSequenceOffs);
        dealloc(GZstd.DecompressedSequenceMLen);

        GZstd.DecompressedSequenceLLen = DecompressedSequenceLLen_New;
        GZstd.DecompressedSequenceOffs = DecompressedSequenceOffs_New;
        GZstd.DecompressedSequenceMLen = DecompressedSequenceMLen_New;

        GZstdInfo.DecompressedSequenceLLen_Count = DecompressedSequence_Count_New;
        GZstdInfo.DecompressedSequenceOffs_Count = DecompressedSequence_Count_New;
        GZstdInfo.DecompressedSequenceMLen_Count = DecompressedSequence_Count_New;
    }

    uint32_t mlenSum = 0;
    for (uint32_t seqId = 0; seqId < sequenceCount; ++seqId)
    {
        const uint32_t llen = sequences[seqId * 3 + 0];
        const uint32_t mlen = sequences[seqId * 3 + 1];
        const uint32_t offs = sequences[seqId * 3 + 2];

        mlenSum += mlen;

        GZstd.DecompressedSequenceLLen[seqOffs + seqId] = llen;
        GZstd.DecompressedSequenceMLen[seqOffs + seqId] = mlen;
        GZstd.DecompressedSequenceOffs[seqOffs + seqId] = offs;

#if ENABLE_OFFSET_PROPAGATION
        GZstd.DecompressedSequenceOffs[seqOffs + seqId] = zstdgpu_SequenceOffsets_Update2(recent1, recent2, recent3, offs, llen);
#endif
    }
    // NOTE(pamartis): accumulated match length are used to update the uncompressed size of compressed block
    zstdgpu_AppendLastBlockSize(mlenSum);
    ZSTDGPU_ASSERT(GRecentOffset1 != 0);
    ZSTDGPU_ASSERT(GRecentOffset2 != 0);
    ZSTDGPU_ASSERT(GRecentOffset3 != 0);
    zstdgpu_DecodeSeqRepeatOffsetsAndApplyPreviousOffsets(recent1, recent2, recent3, GRecentOffset1, GRecentOffset2, GRecentOffset3);
    ZSTDGPU_ASSERT(zstdgpu_DecodeSeqRepeatOffsetEncoded(recent1) == 0);
    ZSTDGPU_ASSERT(zstdgpu_DecodeSeqRepeatOffsetEncoded(recent2) == 0);
    ZSTDGPU_ASSERT(zstdgpu_DecodeSeqRepeatOffsetEncoded(recent3) == 0);

    // on GPU these offsets are available after cross-block propagation
    GZstd.PerSeqStreamFinalOffset1[i] = recent1;
    GZstd.PerSeqStreamFinalOffset2[i] = recent2;
    GZstd.PerSeqStreamFinalOffset3[i] = recent3;

    GSequenceCount += sequenceCount;

#if ENABLE_OFFSET_PROPAGATION
    for (uint32_t seqId = 0; seqId < sequenceCount; ++seqId)
    {
        uint32_t offset = GZstd.DecompressedSequenceOffs[seqOffs + seqId];
        if (zstdgpu_DecodeSeqRepeatOffsetEncoded(offset))
        {
            GZstd.DecompressedSequenceOffs[seqOffs + seqId] = zstdgpu_DecodeSeqRepeatOffsetAndApplyPreviousOffsets(offset, GRecentOffset1, GRecentOffset2, GRecentOffset3);
        }
        GZstd.DecompressedSequenceOffs[seqOffs + seqId] -= 3u;
    }
#endif
    GRecentOffset1 = recent1;
    GRecentOffset2 = recent2;
    GRecentOffset3 = recent3;
}


static uint32_t GResolvedOffsetIndex = 0;

void zstdgpu_ReferenceStore_Report_ResolvedOffsetsResetFrame(void)
{
    GResolvedOffsetResetFrame = 1u;
}

void zstdgpu_ReferenceStore_Report_ResolvedOffsetsBegin(void)
{
    GResolvedOffsetIndex = 0;
}

void zstdgpu_ReferenceStore_Report_ResolvedOffset(size_t offset)
{
    const uint32_t i = GSequenceStreamCount - 1u;
    const uint32_t seqOffs = GZstd.PerSeqStreamSeqStart[i];
#if ENABLE_OFFSET_PROPAGATION
    ZSTDGPU_ASSERT(GZstd.DecompressedSequenceOffs[seqOffs + GResolvedOffsetIndex] == offset);
#else
    (void)offset;
#endif
    GResolvedOffsetIndex += 1;
}

static ZSTDGPU_ENUM(Validate_Result) izstdgpu_ReferenceStore_Validate_OffsetAndSize(const zstdgpu_OffsetAndSize *ref, const zstdgpu_OffsetAndSize *tst)
{
    if (ref->offs == tst->offs && ref->size == tst->size)
        return ZSTDGPU_ENUM_CONST(Validate_Success);
    else
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
}

static ZSTDGPU_ENUM(Validate_Result) izstdgpu_ReferenceStore_Validate_OffsetsAndSizes(const zstdgpu_OffsetAndSize *ref, uint32_t refCount, const zstdgpu_OffsetAndSize *tst, uint32_t tstCount)
{
    ZSTDGPU_ASSERT(refCount == tstCount);

    if (0 != memcmp(ref, tst, sizeof(tst[0]) * tstCount))
    {
        // NOTE(pamartis): We iterate to find which reference is invalid.
        for (uint32_t i = 0; i < refCount; ++i)
        {
            if (ZSTDGPU_ENUM_CONST(Validate_Success) != izstdgpu_ReferenceStore_Validate_OffsetAndSize(&ref[i], &tst[i]))
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
        }
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

ZSTDGPU_ENUM(Validate_Result) zstdgpu_ReferenceStore_Validate_Blocks(const zstdgpu_ResourceDataCpu *resourceDataCpu)
{
    if (resourceDataCpu->Counters->Blocks_RAW != GBlockIndexRAW)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (resourceDataCpu->Counters->Blocks_RLE != GBlockIndexRLE)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (resourceDataCpu->Counters->Blocks_CMP != GBlockIndexCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    #define VALIDATE_BLOCKS(name) \
        izstdgpu_ReferenceStore_Validate_OffsetsAndSizes(GZstd.Blocks##name##Refs, GBlockCount##name, resourceDataCpu->Blocks##name##Refs, resourceDataCpu->Counters->Blocks_##name)

        if (ZSTDGPU_ENUM_CONST(Validate_Success) != VALIDATE_BLOCKS(RAW))
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        //VALIDATE_BLOCKS(RLE);
        if (ZSTDGPU_ENUM_CONST(Validate_Success) != VALIDATE_BLOCKS(CMP))
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

    #undef VALIDATE_BLOCKS
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

static ZSTDGPU_ENUM(Validate_Result) izstdgpu_ReferenceStore_Validate_FseTable(uint32_t refFseTableIndex,
                                                                         uint32_t tstFseTableIndex,
                                                                         uint32_t fseInfoOffset,
                                                                         uint32_t cmpBlockCount,
                                                                         zstdgpu_FseElemOffsetFn elemOffsetFn,
                                                                         const zstdgpu_FseInfo *refFseInfos,
                                                                         const zstdgpu_FseInfo *tstFseInfos,
                                                                         const int16_t *refFseProbs,
                                                                         const int16_t *tstFseProbs,
                                                                         const uint32_t *refFseElems,
                                                                         const uint32_t *tstFseElems)
{
    // All table indices (including RLE 0-255) are valid real indices now
    if (refFseTableIndex < kzstdgpu_FseProbTableIndex_Repeat)
    {
        if (tstFseTableIndex >= kzstdgpu_FseProbTableIndex_Repeat)
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        // For HufW: fseInfoOffset=256 maps localIdx to FseInfos[256+localIdx]
        // For LLen/Offs/MLen: fseInfoOffset=0, fseTableIndex already includes +256 (or is RLE 0-255)
        const uint32_t refFseInfoIndex = fseInfoOffset + refFseTableIndex;
        const uint32_t tstFseInfoIndex = fseInfoOffset + tstFseTableIndex;

        if (refFseInfos[refFseInfoIndex].fseProbCountAndAccuracyLog2 != tstFseInfos[tstFseInfoIndex].fseProbCountAndAccuracyLog2)
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        const uint32_t probCount = refFseInfos[refFseInfoIndex].fseProbCountAndAccuracyLog2 & 0xff;
        const uint32_t symbolCount = 1u << (refFseInfos[refFseInfoIndex].fseProbCountAndAccuracyLog2 >> 8u);

        // FseProbs are indexed at (fseInfoIndex - 256) * MaxCount_FseProbs
        // For RLE (fseInfoIndex < 256), probCount is 0 so no comparison is needed
        if (probCount > 0)
        {
            const uint32_t refProbStart = (refFseInfoIndex - kzstdgpu_FseRleTableCount) * kzstdgpu_MaxCount_FseProbs;
            const uint32_t tstProbStart = (tstFseInfoIndex - kzstdgpu_FseRleTableCount) * kzstdgpu_MaxCount_FseProbs;

            if (0 != memcmp(&refFseProbs[refProbStart], &tstFseProbs[tstProbStart], probCount * sizeof(refFseProbs[0])))
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
        }

        if (NULL != tstFseElems)
        {
            const uint32_t refElemStart = elemOffsetFn(refFseTableIndex, cmpBlockCount);
            const uint32_t tstElemStart = elemOffsetFn(tstFseTableIndex, cmpBlockCount);

            if (0 != memcmp(&refFseElems[refElemStart], &tstFseElems[tstElemStart], symbolCount * sizeof(refFseElems[0])))
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

        }
    }
    // if reference FSE/Huffman table index is an actual index -- the test index must be identical
    else
    {
        if (refFseTableIndex == kzstdgpu_FseProbTableIndex_Repeat)
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        if (refFseTableIndex != tstFseTableIndex)
            return ZSTDGPU_ENUM_CONST(Validate_Failed);
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

ZSTDGPU_ENUM(Validate_Result) zstdgpu_ReferenceStore_Validate_FseTables(const zstdgpu_ResourceDataCpu *resourceDataCpu)
{
    const zstdgpu_ResourceDataCpu *ref = &GZstd;
    const zstdgpu_ResourceDataCpu *tst = resourceDataCpu;

    if (tst->Counters->Blocks_CMP != GBlockCountCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (tst->Counters->Blocks_CMP != GBlockIndexCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (tst->Counters->FseHufW != GFseProbTableIndexHufW)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (tst->Counters->FseLLen != GFseProbTableIndexLLen)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (tst->Counters->FseOffs != GFseProbTableIndexOffs)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (tst->Counters->FseMLen != GFseProbTableIndexMLen)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (tst->Counters->FseHufW != GFseCompressedHuffmanWeightCount)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    for (uint32_t i = 0; i < GBlockCountCMP; ++i)
    {
         // Validate Referred FSE Tables
        #define VALIDATE_FSE_TABLE_CONTENT(name, infoOfs, elemFn) \
            izstdgpu_ReferenceStore_Validate_FseTable(          \
                ref->CompressedBlocks[i].fseTableIndex##name,   \
                tst->CompressedBlocks[i].fseTableIndex##name,   \
                infoOfs, GBlockCountCMP, elemFn,                \
                ref->FseInfos,                                  \
                tst->FseInfos,                                  \
                ref->FseProbs,                                  \
                tst->FseProbs,                                  \
                ref->FseElems,                                  \
                tst->FseElems                                   \
            )

        // NOTE(pamartis): When FSE HufW table index is less than the total number of FSE table count --
        // it's an actual FSE index, when it's greater or equal --
        // it encodes the index of an uncompressed Huffman Weights Stream
        if (ref->CompressedBlocks[i].fseTableIndexHufW < GFseProbTableIndexHufW)
        {
            if (!(tst->CompressedBlocks[i].fseTableIndexHufW < GFseProbTableIndexHufW))
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            if (ZSTDGPU_ENUM_CONST(Validate_Success) != VALIDATE_FSE_TABLE_CONTENT(HufW, kzstdgpu_FseRleTableCount, zstdgpu_ComputeFseDataStartHufW))
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
        }

        if (ZSTDGPU_ENUM_CONST(Validate_Success) != VALIDATE_FSE_TABLE_CONTENT(LLen, 0, zstdgpu_ComputeFseDataStartFromFseIndexLLen))
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        if (ZSTDGPU_ENUM_CONST(Validate_Success) != VALIDATE_FSE_TABLE_CONTENT(Offs, 0, zstdgpu_ComputeFseDataStartFromFseIndexOffs))
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        if (ZSTDGPU_ENUM_CONST(Validate_Success) != VALIDATE_FSE_TABLE_CONTENT(MLen, 0, zstdgpu_ComputeFseDataStartFromFseIndexMLen))
            return ZSTDGPU_ENUM_CONST(Validate_Failed);


        #undef VALIDATE_FSE_TABLE_CONTENT
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

ZSTDGPU_ENUM(Validate_Result) zstdgpu_ReferenceStore_Validate_CompressedBlocksData(const zstdgpu_ResourceDataCpu *resourceDataCpu)
{
    if (resourceDataCpu->Counters->Blocks_CMP != GBlockCountCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (resourceDataCpu->Counters->Blocks_CMP != GBlockIndexCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (resourceDataCpu->Counters->FseHufW != GFseProbTableIndexHufW)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (resourceDataCpu->Counters->FseLLen != GFseProbTableIndexLLen)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (resourceDataCpu->Counters->FseOffs != GFseProbTableIndexOffs)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);
    if (resourceDataCpu->Counters->FseMLen != GFseProbTableIndexMLen)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (resourceDataCpu->Counters->FseHufW != GFseCompressedHuffmanWeightCount)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    const zstdgpu_CompressedBlockData *refCmpBlocks = GZstd.CompressedBlocks;
    const zstdgpu_CompressedBlockData *tstCmpBlocks = resourceDataCpu->CompressedBlocks;

    //if (0 != memcmp(tstCmpBlocks, refCmpBlocks, sizeof(refCmpBlocks[0]) * GBlockCountCMP))
    {
        for (uint32_t i = 0; i < GBlockCountCMP; ++i)
        {
            if (tstCmpBlocks[i].litStreamIndex == ~0u)
            {
                if (ZSTDGPU_ENUM_CONST(Validate_Success) != izstdgpu_ReferenceStore_Validate_OffsetAndSize(&refCmpBlocks[i].literal, &resourceDataCpu->CompressedBlocks[i].literal))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
            }
            else
            {
                // Only the sizes must be identical, offset can be arbitrary due to randomness of allocation
                if (refCmpBlocks[i].literal.size != tstCmpBlocks[i].literal.size)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                uint32_t refLiteralOffset = refCmpBlocks[i].litStreamIndex;
                uint32_t tstLitaralOffset = tstCmpBlocks[i].litStreamIndex;

                const zstdgpu_LitStreamInfo *refHufLiteral = &GZstd.LitRefs[refLiteralOffset];
                const zstdgpu_LitStreamInfo *tstHufLiteral = &resourceDataCpu->LitRefs[tstLitaralOffset];

                // Disabled because "block data" contains offset as "compressed block data size + offset"
                //ZSTDGPU_ASSERT(refCompressedBlockData.literal.offs == refHufLiteral[0].dstOffs);
                //ZSTDGPU_ASSERT(tstCmpBlocks[i].literal.offs == tstHufLiteral[0].dstOffs);

                if (refCmpBlocks[i].literal.size == refHufLiteral[0].dst.size)
                {
                    if (refHufLiteral[0].dst.size != tstHufLiteral[0].dst.size)
                        return ZSTDGPU_ENUM_CONST(Validate_Failed);
                }
                else
                {
                    const uint32_t refDstSize = refHufLiteral[0].dst.size
                                              + refHufLiteral[1].dst.size
                                              + refHufLiteral[2].dst.size
                                              + refHufLiteral[3].dst.size;

                    const uint32_t tstDstSize = tstHufLiteral[0].dst.size
                                              + tstHufLiteral[1].dst.size
                                              + tstHufLiteral[2].dst.size
                                              + tstHufLiteral[3].dst.size;

                    if (refCmpBlocks[i].literal.size != refDstSize)
                        return ZSTDGPU_ENUM_CONST(Validate_Failed);
                    if (tstCmpBlocks[i].literal.size != tstDstSize)
                        return ZSTDGPU_ENUM_CONST(Validate_Failed);
                    if (refCmpBlocks[i].literal.size != tstCmpBlocks[i].literal.size)
                        return ZSTDGPU_ENUM_CONST(Validate_Failed);
                }

                if (ZSTDGPU_ENUM_CONST(Validate_Success) != izstdgpu_ReferenceStore_Validate_OffsetAndSize(&refHufLiteral[0].src, &tstHufLiteral[0].src))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
            }

            // Validate offsets of Fse-compressed Huffman Weights
            uint32_t tstFseCompressedHuffmanWeightsIndex = tstCmpBlocks[i].fseTableIndexHufW;
            uint32_t refFseCompressedHuffmanWeightsIndex = refCmpBlocks[i].fseTableIndexHufW;
            if (refFseCompressedHuffmanWeightsIndex != kzstdgpu_FseProbTableIndex_Unused)
            {
                if (refFseCompressedHuffmanWeightsIndex == kzstdgpu_FseProbTableIndex_Repeat)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
                if (tstFseCompressedHuffmanWeightsIndex == kzstdgpu_FseProbTableIndex_Repeat)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
                if (tstFseCompressedHuffmanWeightsIndex == kzstdgpu_FseProbTableIndex_Unused)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (ZSTDGPU_ENUM_CONST(Validate_Success) != izstdgpu_ReferenceStore_Validate_OffsetAndSize(&GZstd.HufRefs[refFseCompressedHuffmanWeightsIndex], &resourceDataCpu->HufRefs[tstFseCompressedHuffmanWeightsIndex]))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
            }
        }
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

ZSTDGPU_ENUM(Validate_Result) zstdgpu_ReferenceStore_Validate_DecompressedHuffmanWeights(const zstdgpu_ResourceDataCpu *resourceDataCpu)
{
    if (resourceDataCpu->Counters->Blocks_CMP != GBlockCountCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (resourceDataCpu->Counters->Blocks_CMP != GBlockIndexCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    const zstdgpu_ResourceDataCpu *ref = &GZstd;
    const zstdgpu_ResourceDataCpu *tst = resourceDataCpu;

    for (uint32_t i = 0; i < GBlockCountCMP; ++i)
    {
        // Validate decompressed Fse-compressed Huffman Weights and their counts
        uint32_t tstHuffmanWeightStreamIndex = tst->CompressedBlocks[i].fseTableIndexHufW;
        uint32_t refHuffmanWeightStreamIndex = ref->CompressedBlocks[i].fseTableIndexHufW;

        if (refHuffmanWeightStreamIndex == kzstdgpu_FseProbTableIndex_Repeat)
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        if (refHuffmanWeightStreamIndex != kzstdgpu_FseProbTableIndex_Unused)
        {
            if (tstHuffmanWeightStreamIndex == kzstdgpu_FseProbTableIndex_Repeat)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            if (tstHuffmanWeightStreamIndex == kzstdgpu_FseProbTableIndex_Unused)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            if (refHuffmanWeightStreamIndex < GFseCompressedHuffmanWeightCount)
            {
                if (tstHuffmanWeightStreamIndex >= GFseCompressedHuffmanWeightCount)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                uint32_t tstCount = tst->DecompressedHuffmanWeightCount[tstHuffmanWeightStreamIndex];
                uint32_t refCount = ref->DecompressedHuffmanWeightCount[refHuffmanWeightStreamIndex];

                const uint8_t *tstStart = &tst->DecompressedHuffmanWeights[tstHuffmanWeightStreamIndex * kzstdgpu_MaxCount_HuffmanWeights];
                const uint8_t *refStart = &ref->DecompressedHuffmanWeights[refHuffmanWeightStreamIndex * kzstdgpu_MaxCount_HuffmanWeights];

                if (refCount != tstCount)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (tstCount >= kzstdgpu_MaxCount_HuffmanWeights)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (0 != memcmp(refStart, tstStart, refCount))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
            }
        }
        else
        {
            // NOTE(pamartis): because ref is Unused, tst must be Unused too.
            if (tstHuffmanWeightStreamIndex != kzstdgpu_FseProbTableIndex_Unused)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
        }
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

ZSTDGPU_ENUM(Validate_Result) zstdgpu_ReferenceStore_Validate_DecodedHuffmanWeights(const zstdgpu_ResourceDataCpu * resourceDataCpu)
{
    if (resourceDataCpu->Counters->Blocks_CMP != GBlockCountCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);

    if (resourceDataCpu->Counters->Blocks_CMP != GBlockIndexCMP)
        return ZSTDGPU_ENUM_CONST(Validate_Failed);


    const zstdgpu_ResourceDataCpu *ref = &GZstd;
    const zstdgpu_ResourceDataCpu *tst = resourceDataCpu;

    for (uint32_t i = 0; i < GBlockCountCMP; ++i)
    {
        // Validate decoded uncompressed Huffman Weights and their counts
        uint32_t tstHuffmanWeightStreamIndex = tst->CompressedBlocks[i].fseTableIndexHufW;
        uint32_t refHuffmanWeightStreamIndex = ref->CompressedBlocks[i].fseTableIndexHufW;

        if (refHuffmanWeightStreamIndex == kzstdgpu_FseProbTableIndex_Repeat)
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        if (refHuffmanWeightStreamIndex != kzstdgpu_FseProbTableIndex_Unused)
        {
            if (tstHuffmanWeightStreamIndex == kzstdgpu_FseProbTableIndex_Repeat)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            if (tstHuffmanWeightStreamIndex == kzstdgpu_FseProbTableIndex_Unused)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);


            if (refHuffmanWeightStreamIndex >= GFseCompressedHuffmanWeightCount)
            {
                if (tstHuffmanWeightStreamIndex < GFseCompressedHuffmanWeightCount)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                uint32_t tstCount = tst->DecompressedHuffmanWeightCount[tstHuffmanWeightStreamIndex];
                uint32_t refCount = ref->DecompressedHuffmanWeightCount[refHuffmanWeightStreamIndex];

                const uint8_t *tstStart = &tst->DecompressedHuffmanWeights[tstHuffmanWeightStreamIndex * kzstdgpu_MaxCount_HuffmanWeights];
                const uint8_t* refStart = &ref->DecompressedHuffmanWeights[refHuffmanWeightStreamIndex * kzstdgpu_MaxCount_HuffmanWeights];

                if (refCount != tstCount)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (tstCount >= kzstdgpu_MaxCount_HuffmanWeights)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (0 != memcmp(refStart, tstStart, refCount))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
            }
        }
        else
        {
            // NOTE(pamartis): because ref is Unused, tst must be Unused too.
            if (tstHuffmanWeightStreamIndex != kzstdgpu_FseProbTableIndex_Unused)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
        }
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

ZSTDGPU_ENUM(Validate_Result) zstdgpu_ReferenceStore_Validate_DecompressedLiterals(const zstdgpu_ResourceDataCpu *resourceDataCpu)
{
    const zstdgpu_ResourceDataCpu *tstData = resourceDataCpu;
    const zstdgpu_ResourceDataCpu *refData = &GZstd;

    for (uint32_t i = 0; i < GBlockCountCMP; ++i)
    {
        const zstdgpu_OffsetAndSize *refLit = &refData->CompressedBlocks[i].literal;
        const zstdgpu_OffsetAndSize *tstLit = &tstData->CompressedBlocks[i].literal;

        const uint32_t refLitT = zstdgpu_DecodeLitOffsetType(refLit->offs);
        const uint32_t tstLitT = zstdgpu_DecodeLitOffsetType(tstLit->offs);

        if (refLitT != tstLitT)
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        // NOTE(pamartis): if the type of the offset not "compressed literal", then it's a non-compressed literal stored in the source data, so offsets are identical
        if (zstdgpu_CheckLitOffsetTypeCmp(refLitT) == 0)
        {
            if (ZSTDGPU_ENUM_CONST(Validate_Success) != izstdgpu_ReferenceStore_Validate_OffsetAndSize(refLit, tstLit))
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
        }
        else
        // NOTE(pamartis): if the type of the offset is "compressed literal", then it's a decompressed literal stored in `DecompressedLiterals`, so offsets are NOT identical
        {
            if (refLit->size != tstLit->size)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            const uint8_t *refDecLit = refData->DecompressedLiterals + zstdgpu_DecodeLitOffset(refLit->offs);
            const uint8_t *tstDecLit = tstData->DecompressedLiterals + zstdgpu_DecodeLitOffset(tstLit->offs);
            if (0 != memcmp(refDecLit, tstDecLit, refLit->size))
            {
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
            }
        }
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}

ZSTDGPU_ENUM(Validate_Result) zstdgpu_ReferenceStore_Validate_DecompressedSequences(const struct zstdgpu_ResourceDataCpu *resourceDataCpu)
{
    const zstdgpu_ResourceDataCpu *tstData = resourceDataCpu;
    const zstdgpu_ResourceDataCpu *refData = &GZstd;

    for (uint32_t i = 0; i < GBlockCountCMP; ++i)
    {
        if (refData->GlobalBlockIndexPerCmpBlock[i] != tstData->GlobalBlockIndexPerCmpBlock[i])
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        const uint32_t cmpBlockIndex = refData->GlobalBlockIndexPerCmpBlock[i];

        if (refData->BlockSizePrefix[cmpBlockIndex] != tstData->BlockSizePrefix[cmpBlockIndex])
            return ZSTDGPU_ENUM_CONST(Validate_Failed);

        if (refData->CompressedBlocks[i].seqStreamIndex == ~0u)
        {
            if (tstData->CompressedBlocks[i].seqStreamIndex != ~0u)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);
        }
        else
        {
            const uint32_t refSeqSteamIndex = refData->CompressedBlocks[i].seqStreamIndex;
            const uint32_t tstSeqSteamIndex = tstData->CompressedBlocks[i].seqStreamIndex;

            if (tstSeqSteamIndex == ~0u)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            if (refData->PerSeqStreamFinalOffset1[refSeqSteamIndex] != tstData->PerSeqStreamFinalOffset1[tstSeqSteamIndex])
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            if (refData->PerSeqStreamFinalOffset2[refSeqSteamIndex] != tstData->PerSeqStreamFinalOffset2[tstSeqSteamIndex])
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            if (refData->PerSeqStreamFinalOffset3[refSeqSteamIndex] != tstData->PerSeqStreamFinalOffset3[tstSeqSteamIndex])
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            const zstdgpu_SeqStreamInfo *refSeqRefs = &refData->SeqRefs[refSeqSteamIndex];
            const zstdgpu_SeqStreamInfo *tstSeqRefs = &tstData->SeqRefs[tstSeqSteamIndex];

            const uint32_t refSeqOffs = refData->PerSeqStreamSeqStart[refSeqSteamIndex];
            const uint32_t tstSeqOffs = tstData->PerSeqStreamSeqStart[tstSeqSteamIndex];

            if (tstSeqOffs != refSeqOffs)
                return ZSTDGPU_ENUM_CONST(Validate_Failed);

            // NOTE: dst.offs can't be the same due to non-deterministic allocation
            //izstdgpu_ReferenceStore_Validate_OffsetAndSize(&refSeqRefs[refSeqSteamIndex].dst, &tstSeqRefs[tstSeqSteamIndex].dst);
            {
                const uint32_t refSeqOffsNext = (i == GBlockCountCMP - 1u) ? GSequenceCount                                                     : refData->PerSeqStreamSeqStart[refSeqSteamIndex + 1];
                const uint32_t tstSeqOffsNext = (i == GBlockCountCMP - 1u) ? tstData->Counters->Seq_Streams_DecodedItems  : tstData->PerSeqStreamSeqStart[tstSeqSteamIndex + 1];
                const uint32_t refSeqCount = refSeqOffsNext - refSeqOffs;
                const uint32_t tstSeqCount = tstSeqOffsNext - tstSeqOffs;

                if (refSeqCount != tstSeqCount)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (0 != memcmp(&refData->DecompressedSequenceLLen[refSeqOffs], &tstData->DecompressedSequenceLLen[tstSeqOffs], refSeqCount * sizeof(refData->DecompressedSequenceLLen[0])))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (0 != memcmp(&refData->DecompressedSequenceMLen[refSeqOffs], &tstData->DecompressedSequenceMLen[tstSeqOffs], refSeqCount * sizeof(refData->DecompressedSequenceMLen[0])))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (0 != memcmp(&refData->DecompressedSequenceOffs[refSeqOffs], &tstData->DecompressedSequenceOffs[tstSeqOffs], refSeqCount * sizeof(refData->DecompressedSequenceOffs[0])))
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);
            }

            // Validate FSE tables accessed through references are the same
            {
                // NOTE: can't be the same due to non-deterministic allocation
                //ZSTDGPU_ASSERT(refSeqRefs[refSeqSteamIndex].fseLLen == tstSeqRefs[tstSeqSteamIndex].fseLLen);
                //ZSTDGPU_ASSERT(refSeqRefs[refSeqSteamIndex].fseOffs == tstSeqRefs[tstSeqSteamIndex].fseOffs);
                //ZSTDGPU_ASSERT(refSeqRefs[refSeqSteamIndex].fseMLen == tstSeqRefs[tstSeqSteamIndex].fseMLen);

                if (refSeqRefs->fseLLen != refData->CompressedBlocks[i].fseTableIndexLLen)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (refSeqRefs->fseOffs != refData->CompressedBlocks[i].fseTableIndexOffs)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (refSeqRefs->fseMLen != refData->CompressedBlocks[i].fseTableIndexMLen)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);


                if (tstSeqRefs->fseLLen != tstData->CompressedBlocks[i].fseTableIndexLLen)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (tstSeqRefs->fseOffs != tstData->CompressedBlocks[i].fseTableIndexOffs)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

                if (tstSeqRefs->fseMLen != tstData->CompressedBlocks[i].fseTableIndexMLen)
                    return ZSTDGPU_ENUM_CONST(Validate_Failed);

            }
        }
    }
    return ZSTDGPU_ENUM_CONST(Validate_Success);
}
