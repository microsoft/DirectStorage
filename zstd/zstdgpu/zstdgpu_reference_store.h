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

#pragma once

#ifndef ZSTDGPU_REFERENCE_STORE_H
#define ZSTDGPU_REFERENCE_STORE_H

#include <stdint.h>

typedef enum zstdgpu_ReferenceStore_BlockType
{
    kzstdgpu_ReferenceStore_BlockRAW      = 0,
    kzstdgpu_ReferenceStore_BlockRLE      = 1,
    kzstdgpu_ReferenceStore_BlockCMP      = 2,
    kzstdgpu_ReferenceStore_BlockForceInt = 0x7fffffff
} zstdgpu_ReferenceStore_BlockType;

typedef enum zstdgpu_ReferenceStore_FseType
{
    kzstdgpu_ReferenceStore_FseHufW       = 0,
    kzstdgpu_ReferenceStore_FseLLen       = 1,
    kzstdgpu_ReferenceStore_FseOffs       = 2,
    kzstdgpu_ReferenceStore_FseMLen       = 3,
    kzstdgpu_ReferenceStore_FseForceInt   = 0x7fffffff
} zstdgpu_ReferenceStore_FseType;

typedef enum zstdgpu_Validate_Result
{
    kzstdgpu_Validate_Success       = 0,
    kzstdgpu_Validate_Failed        = 1,
    kzstdgpu_Validate_ForceInt      = 0x7fffffff
} zstdgpu_Validate_Result;

struct zstdgpu_ResourceDataCpu;

#ifdef __cplusplus
extern "C" {
#endif

void zstdgpu_ReferenceStore_Report_ChunkBase(const void *base);
void zstdgpu_ReferenceStore_Report_FrameAndBlockCount(uint32_t frameCount, uint32_t rawBlockCount, uint32_t rleBlockCount, uint32_t cmpBlockCount, uint32_t dataSize);

void zstdgpu_ReferenceStore_AllocateMemory(void);
void zstdgpu_ReferenceStore_FreeMemory(void);

void zstdgpu_ReferenceStore_Report_Block(const void *base, uint32_t size, zstdgpu_ReferenceStore_BlockType type);
void zstdgpu_ReferenceStore_Report_RawLiteral(const void *base, uint32_t size);
void zstdgpu_ReferenceStore_Report_RleLiteral(const void* base, uint32_t size);
void zstdgpu_ReferenceStore_Report_CmpLiteral(const void *base, uint32_t size);
void zstdgpu_ReferenceStore_Report_CompressedLiteral(const void *base, uint32_t size);
void zstdgpu_ReferenceStore_Report_CompressedLiteralDecompressedSizeAndStreamCount(uint32_t size, uint32_t streamCount);
void zstdgpu_ReferenceStore_Report_CompressedLiteralDecomressedSize(uint32_t size);
void zstdgpu_ReferenceStore_Report_DecompressedLiteral(const uint8_t *literal, uint32_t size);

void zstdgpu_ReferenceStore_Report_FseProbTableType(zstdgpu_ReferenceStore_FseType type);
void zstdgpu_ReferenceStore_Report_FseTable(const int16_t *probs, uint32_t symCount, const uint8_t *symbol, const uint8_t *bitcnt, const uint16_t *nstate, uint32_t accuracyLog);
void zstdgpu_ReferenceStore_Report_FseDefaultTable(const int16_t *probs, uint32_t symCount, const uint8_t *symbol, const uint8_t *bitcnt, const uint16_t *nstate, uint32_t accuracyLog);
void zstdgpu_ReferenceStore_Report_FseProbSymbol(uint32_t symbol);
void zstdgpu_ReferenceStore_Report_FseProbRepeatTable();

void zstdgpu_ReferenceStore_Report_FseCompressedHuffmanWeightSubBlock(const void* base, uint32_t size);
void zstdgpu_ReferenceStore_Report_HuffmanWeightSubBlock(const void *base, uint32_t size, uint32_t weightCount);

void zstdgpu_ReferenceStore_Report_DecompressedHuffmanWeights(const uint8_t *weights, uint32_t weightCount);
void zstdgpu_ReferenceStore_Report_UncompressedHuffmanWeights(const uint8_t *weights, uint32_t weightCount);

void zstdgpu_ReferenceStore_Report_CompressedSequences(const void *base, uint32_t size, uint32_t sequenceCount);
void zstdgpu_ReferenceStore_Report_DecompressedSequences(const uint32_t *sequences, uint32_t sequenceCount);

void zstdgpu_ReferenceStore_Report_ResolvedOffsetsResetFrame(void);
void zstdgpu_ReferenceStore_Report_ResolvedOffsetsBegin(void);
void zstdgpu_ReferenceStore_Report_ResolvedOffset(size_t offset);

zstdgpu_Validate_Result zstdgpu_ReferenceStore_Validate_Blocks(const struct zstdgpu_ResourceDataCpu *resourceDataCpu);
zstdgpu_Validate_Result zstdgpu_ReferenceStore_Validate_CompressedBlocksData(const struct zstdgpu_ResourceDataCpu * resourceDataCpu);
zstdgpu_Validate_Result zstdgpu_ReferenceStore_Validate_FseTables(const struct zstdgpu_ResourceDataCpu *resourceDataCpu);
zstdgpu_Validate_Result zstdgpu_ReferenceStore_Validate_DecompressedHuffmanWeights(const struct zstdgpu_ResourceDataCpu * resourceDataCpu);
zstdgpu_Validate_Result zstdgpu_ReferenceStore_Validate_DecodedHuffmanWeights(const struct zstdgpu_ResourceDataCpu * resourceDataCpu);
zstdgpu_Validate_Result zstdgpu_ReferenceStore_Validate_DecompressedLiterals(const struct zstdgpu_ResourceDataCpu *resourceDataCpu);
zstdgpu_Validate_Result zstdgpu_ReferenceStore_Validate_DecompressedSequences(const struct zstdgpu_ResourceDataCpu *resourceDataCpu);

#ifdef __cplusplus
}
#endif

#endif // ZSTDGPU_REFERENCE_STORE_H