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
#include "../.generated/ZstdGpuSrt_ComputePrefixSum.h"

[RootSignature(ZSTDGPU_SRT_RS_ComputePrefixSum)]
[numthreads(kzstdgpu_TgSizeX_PrefixSum_LiteralCount, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    zstdgpu_ComputePrefixSum_SRT srt;

    zstdgpu_Srt_Fill(srt);

    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, srt.tgOffset) * kzstdgpu_TgSizeX_PrefixSum_LiteralCount + threadId;
    const uint32_t blockSize = min(kzstdgpu_TgSizeX_PrefixSum_LiteralCount, WaveGetLaneCount());
    const uint32_t thisBlockIndex = WaveReadLaneFirst(i / blockSize);
    const uint32_t thisLocalIndex = i % blockSize;

    if (i >= srt.workItemCount)
        return;

    const uint32_t lastLocalIndex = WaveActiveCountBits(true) - 1u;

    const uint32_t hufLitId = srt.inHufWIdToHufLitId[i];
    uint32_t hufLitStreamCount = 0;

    // NOTE(pamartis): ~0u marks unused Huffman table indices (no actual Huffman table exist for this index, no literal streams using such table exist)
    ZSTDGPU_BRANCH if (~0u != hufLitId)
    {
        const uint32_t hufLitStreamStart = srt.inHufLitIdToLitStreamId[hufLitId];

        // NOTE(pamartis): recompute the number of Huffman-compressed literal streams from prefix.
        ZSTDGPU_BRANCH if (hufLitId + 1u < srt.inoutCounters[0].HufLit)
        {
            hufLitStreamCount = srt.inHufLitIdToLitStreamId[hufLitId + 1u] - hufLitStreamStart;
        }
        else
        {
            hufLitStreamCount = srt.inoutCounters[0].HUF_Streams - hufLitStreamStart;
        }
    }
    const uint32_t groupCount = ZSTDGPU_TG_COUNT(hufLitStreamCount, srt.literalsPerGroup);
    const uint32_t waveExclusiveGroupPrefix = WavePrefixSum(groupCount);
    const uint32_t globalExclusiveGroupPrefix = zstdgpu_GlobalExclusivePrefixSum(srt.inoutLitGroupEndPerHuffmanTableLookback, waveExclusiveGroupPrefix, groupCount, i, kzstdgpu_TgSizeX_PrefixSum_LiteralCount);

    srt.inoutLitGroupEndPerHuffmanTable[i] = globalExclusiveGroupPrefix + groupCount;

    // NOTE(pamartis): the last thread writes its inclusive prefix -- the total number of threadgroups
    // to dispatch for literal decompression.
    if (i == srt.workItemCount - 1)
    {
        srt.inoutCounters[0].DecompressLiteralsGroups = srt.inoutLitGroupEndPerHuffmanTable[i];
    }
}
