/**
 * ZstdGpuDecompressLiterals_LdsStoreCache.hlsli
 *
 * A compute shader that decompresses Huffman-compressed literals using
 * an LDS store cache for cooperative dword-aligned writes.
 *
 * The Huffman table is packed: two symbol+bitcnt pairs per dword (each pair
 * is 16 bits: 8-bit symbol | 8-bit bitcnt). Decoded literals are first
 * accumulated into dwords, staged in an LDS cache, and then cooperatively
 * flushed to device memory via a dword-typed UAV.
 *
 * The following must be defined before including this file:
 *   'kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache'
 *                                                  -- threadgroup size, also used as
 *                                                     the number of dwords cached in LDS
 *                                                     per decoded literal stream.
 *   'kzstdgpu_DecompressLiterals_StreamsPerGroup'  -- number of literal streams processed
 *                                                     per threadgroup.
 *
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

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

#ifndef kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache
#   error 'kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache' must be defined before including this '.hlsli'
#endif

#ifndef kzstdgpu_DecompressLiterals_StreamsPerGroup
#   error 'kzstdgpu_DecompressLiterals_StreamsPerGroup' must be defined before including this '.hlsli'
#endif

#include "../zstdgpu_shaders.h"

// LDS layout for the LdsStoreCache variant: Huffman table + per-stream store cache.
// Cache size per stream equals the threadgroup size (kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache).
#define ZSTDGPU_DECOMPRESS_LITERALS_LDS_STORE_CACHE(base, size) \
    ZSTDGPU_LDS_SIZE(size)                                      \
    ZSTDGPU_LDS_BASE(base)                                      \
    ZSTDGPU_LDS_REGION(HuffmanTable, 1u << (kzstdgpu_MaxCount_HuffmanWeightBits - 1))   \
    ZSTDGPU_LDS_REGION(LiteralStoreCache, kzstdgpu_DecompressLiterals_StreamsPerGroup * kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache)

#include "../zstdgpu_lds_decl_size.h"
ZSTDGPU_DECOMPRESS_LITERALS_LDS_STORE_CACHE(0, DecompressLiterals_LdsStoreCache);
#include "../zstdgpu_lds_decl_undef.h"

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
    uint32_t huffmanTableSlotCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_DECOMPRESS_LITERALS_SRT()
#include "../zstdgpu_srt_decl_undef.h"

groupshared uint32_t GS_Lds[kzstdgpu_DecompressLiterals_LdsStoreCache_LdsSize];
#define ZSTDGPU_LDS GS_Lds
#include "../zstdgpu_lds_hlsl.h"

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=9), UAV(u0, numDescriptors=2)), RootConstants(b0, num32BitConstants=3)")]
[numthreads(kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache, 1, 1)]
void main(uint2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset);

    zstdgpu_DecompressLiterals_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_DECOMPRESS_LITERALS_SRT()
    #include "../zstdgpu_srt_decl_undef.h"
    srt.huffmanTableSlotCount   = Constants.huffmanTableSlotCount;

    if (groupId >= srt.inCounters[0].DecompressLiteralsGroups)
        return;

    uint32_t htIndex = 0;
    uint32_t htGroupStart = 0;
    uint32_t htLiteralStart = 0;
    uint32_t htLiteralCount = 0;

    zstdgpu_ConvertThreadgroupIdToDecompressLiteralsInputs(
        srt.inLitGroupEndPerHuffmanTable,
        srt.inLitStreamEndPerHuffmanTable,
        srt.huffmanTableSlotCount,
        groupId,
        htIndex,
        htGroupStart,
        htLiteralStart,
        htLiteralCount
    );

    #include "../zstdgpu_lds_decl_base.h"
    ZSTDGPU_DECOMPRESS_LITERALS_LDS_STORE_CACHE(0, DecompressLiterals_LdsStoreCache);
    #include "../zstdgpu_lds_decl_undef.h"

    const uint32_t htInfo = WaveReadLaneFirst(srt.inHuffmanTableInfo[htIndex]);
    const uint32_t bitsMax = htInfo >> 16;
    const uint32_t codeTableSize = htInfo & 0xffffu;
    const uint32_t stateCnt = WaveReadLaneFirst(srt.inHuffmanTableRankIndex[htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks + bitsMax]);
    const uint32_t statePairCnt = stateCnt >> 1u;

    // Expand Huffman Table -- pack two symbol+bitcnt pairs per dword
    ZSTDGPU_FOR_WORK_ITEMS(statePairId, statePairCnt, i, kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache)
    {
        const uint32_t stateId0 = statePairId << 1u;
        const uint32_t stateId1 = stateId0 + 1u;

        const uint32_t symbolIndex0 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableCodeAndSymbol, htIndex * kzstdgpu_MaxCount_HuffmanWeights, codeTableSize, stateId0, 0x00ffffffu);
        const uint32_t symbolIndex1 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableCodeAndSymbol, htIndex * kzstdgpu_MaxCount_HuffmanWeights, codeTableSize, stateId1, 0x00ffffffu);

        const uint32_t bitcntIndex0 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableRankIndex, htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks, bitsMax + 1, stateId0, 0xffffffffu)
                                    - htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks;

        const uint32_t bitcntIndex1 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableRankIndex, htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks, bitsMax + 1, stateId1, 0xffffffffu)
                                    - htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks;

        const uint32_t symbol0 = srt.inHuffmanTableCodeAndSymbol[symbolIndex0] >> 24;
        const uint32_t bitcnt0 = bitsMax - bitcntIndex0;

        const uint32_t symbol1 = srt.inHuffmanTableCodeAndSymbol[symbolIndex1] >> 24;
        const uint32_t bitcnt1 = bitsMax - bitcntIndex1;

        const uint32_t symbolAndBitcnt0 = (symbol0 << 8) | bitcnt0;
        const uint32_t symbolAndBitcnt1 = (symbol1 << 8) | bitcnt1;

        zstdgpu_LdsStoreU32(GS_HuffmanTable + statePairId, (symbolAndBitcnt1 << 16) | symbolAndBitcnt0);
    }
    GroupMemoryBarrierWithGroupSync();

    zstdgpu_DecompressHuffmanCompressedLiterals_StoreLdsCache(
        srt.inCompressedData,
        srt.inLitStreamRemap,
        srt.inLitRefs,
        srt.inoutDecompressedLiterals,
        srt.inoutDecompressedLiterals_Dwords,
        GS_HuffmanTable,
        GS_LiteralStoreCache,
        groupId,
        i,
        htGroupStart,
        htLiteralStart,
        htLiteralCount,
        bitsMax,
        kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache,
        kzstdgpu_DecompressLiterals_StreamsPerGroup,
        kzstdgpu_TgSizeX_DecompressLiterals_LdsStoreCache
    );
}
