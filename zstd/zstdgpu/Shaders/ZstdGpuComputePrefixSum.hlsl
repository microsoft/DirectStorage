/**
 * ZstdGpuComputePrefixSum.hlsl
 *
 * A compute shader producing per Huffman table the number of threadgroups required
 * to decompress all Huffman-compressed literal streams the require this table,
 * and storing the prefix sum of those group counts used by [Decompress Literals]
 * to map a flat groupId back to a Huffman table.
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

#include "../zstdgpu_shaders.h"

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
    uint32_t literalsPerGroup;
};

ConstantBuffer<Consts>          Constants                           : register(b0);

StructuredBuffer<uint32_t>      ZstdHufWIdToHufLitId                : register(t0);
StructuredBuffer<uint32_t>      ZstdHufLitIdToLitStreamId           : register(t1);

RWStructuredBuffer<uint32_t>    ZstdLitGroupCountToPrefix           : register(u0);

globallycoherent
RWStructuredBuffer<uint32_t>    ZstdLitGroupCountToPrefixLookback   : register(u1);

RWStructuredBuffer<zstdgpu_Counters>  ZstdCounters                  : register(u2);

[RootSignature("SRV(t0), SRV(t1), UAV(u0), UAV(u1), UAV(u2), RootConstants(b0, num32BitConstants=3)")]
[numthreads(kzstdgpu_TgSizeX_PrefixSum_LiteralCount, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_PrefixSum_LiteralCount + threadId;
    const uint32_t blockSize = min(kzstdgpu_TgSizeX_PrefixSum_LiteralCount, WaveGetLaneCount());
    const uint32_t thisBlockIndex = WaveReadLaneFirst(i / blockSize);
    const uint32_t thisLocalIndex = i % blockSize;

    if (i >= Constants.workItemCount)
        return;

    const uint32_t lastLocalIndex = WaveActiveCountBits(true) - 1u;

    const uint32_t hufLitId = ZstdHufWIdToHufLitId[i];
    uint32_t hufLitStreamCount = 0;

    // NOTE(pamartis): ~0u marks unused Huffman table indices (no actual Huffman table exist for this index, no literal streams using such table exist)
    ZSTDGPU_BRANCH if (~0u != hufLitId)
    {
        const uint32_t hufLitStreamStart = ZstdHufLitIdToLitStreamId[hufLitId];

        // NOTE(pamartis): recompute the number of Huffman-compressed literal streams from prefix.
        ZSTDGPU_BRANCH if (hufLitId + 1u < ZstdCounters[0].HufLit)
        {
            hufLitStreamCount = ZstdHufLitIdToLitStreamId[hufLitId + 1u] - hufLitStreamStart;
        }
        else
        {
            hufLitStreamCount = ZstdCounters[0].HUF_Streams - hufLitStreamStart;
        }
    }
    const uint32_t groupCount = ZSTDGPU_TG_COUNT(hufLitStreamCount, Constants.literalsPerGroup);
    const uint32_t waveExclusiveGroupPrefix = WavePrefixSum(groupCount);
    const uint32_t globalExclusiveGroupPrefix = zstdgpu_GlobalExclusivePrefixSum(ZstdLitGroupCountToPrefixLookback, waveExclusiveGroupPrefix, groupCount, i, kzstdgpu_TgSizeX_PrefixSum_LiteralCount);

    ZstdLitGroupCountToPrefix[i] = globalExclusiveGroupPrefix + groupCount;

    // NOTE(pamartis): the last thread writes its inclusive prefix -- the total number of threadgroups
    // to dispatch for literal decompression.
    if (i == Constants.workItemCount - 1)
    {
        ZstdCounters[0].DecompressLiteralsGroups = ZstdLitGroupCountToPrefix[i];
    }
}
