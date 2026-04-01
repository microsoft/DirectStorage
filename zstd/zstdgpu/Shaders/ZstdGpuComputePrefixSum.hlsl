/**
 * ZstdGpuComputePrefixSum.hlsl
 *
 * A compute shader producing a prefix sum (of the number of Huffman-compressed literals and of the number
 * of threadgroups required to decompress them) from elements stored in the supplied buffer,
 * and storing the prefix sum back to the supplied buffer. The computation is based on
 * "Decoupled Lookback" approach.
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

RWStructuredBuffer<uint32_t>    ZstdLitStreamCountToPrefix          : register(u0);
RWStructuredBuffer<uint32_t>    ZstdLitGroupCountToPrefix           : register(u1);

globallycoherent
RWStructuredBuffer<uint32_t>    ZstdLitStreamCountToPrefixLookback  : register(u2);

globallycoherent
RWStructuredBuffer<uint32_t>    ZstdLitGroupCountToPrefixLookback   : register(u3);

RWStructuredBuffer<zstdgpu_Counters>  ZstdCounters                 : register(u4);

[RootSignature("UAV(u0), UAV(u1), UAV(u2), UAV(u3), UAV(u4), RootConstants(b0, num32BitConstants=3)")]
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

    const uint32_t streamCount = ZstdLitStreamCountToPrefix[i];
    const uint32_t groupCount = ZSTDGPU_TG_COUNT(streamCount, Constants.literalsPerGroup);

    const uint32_t streamPrefix = WavePrefixSum(streamCount);
    const uint32_t groupPrefix = WavePrefixSum(groupCount);

    uint32_t prevBlockTotalStreamPrefix = 0;
    uint32_t prevBlockTotalGroupPrefix = 0;

    // NOTE(pamartis): the last element writes the sum of the exclusive prefix and the it's own element -- which is the cross-block sum
    // this cuts down on WaveActiveSum and re-uses WavePrefixSum instead
    if (thisLocalIndex == lastLocalIndex)
    {
        #if 0
            #define LOOKBACK_STORE(name, prev, value) ZstdLit##name##CountToPrefixLookback[thisBlockIndex] = (value)
        #else
            #define LOOKBACK_STORE(name, prev, value) InterlockedCompareStore(ZstdLit##name##CountToPrefixLookback[thisBlockIndex], prev, (value))
        #endif

        // NOTE(pamartis): 0x40000000u is used to mark that the data we're storing is the sum
        const uint32_t lookbackStreamSum = ((streamPrefix + streamCount) & 0x3fffffffu) | 0x40000000u;
        const uint32_t lookbackGroupSum = ((groupPrefix + groupCount) & 0x3fffffffu) | 0x40000000u;

        LOOKBACK_STORE(Stream, 0, lookbackStreamSum);
        LOOKBACK_STORE(Group, 0, lookbackGroupSum);

        if (thisBlockIndex > 0)
        {
            uint32_t prevBlockIndex = thisBlockIndex - 1u;
            #if 0
                // BUG(pamartis): this varaint of code reads incorrect values on some HW, so it looks like `globallycoherent`
                // keyword doesn't work for reads
                #define LOOKBACK_READ(name)    \
                    const uint32_t prevBlock##name##PrfxOrSum = ZstdLit##name##CountToPrefixLookback[prevBlockIndex]
            #else
                #define LOOKBACK_READ(name)              \
                    uint32_t prevBlock##name##PrfxOrSum; \
                    InterlockedAdd(ZstdLit##name##CountToPrefixLookback[prevBlockIndex], 0, prevBlock##name##PrfxOrSum)
            #endif

            #define LOOKBACK_LOOP(name)                                                             \
                for (;;)                                                                            \
                {                                                                                   \
                    LOOKBACK_READ(name);                                                            \
                    const uint32_t flags = prevBlock##name##PrfxOrSum & 0xc0000000u;                \
                    if (flags > 0)                                                                  \
                    {                                                                               \
                        /* NOTE(pamartis): this is the sum of the previous block or prefix sum of all previous blocks, so we accumulate it */\
                        prevBlockTotal##name##Prefix += prevBlock##name##PrfxOrSum & 0x3fffffffu;   \
                        /* ... and if it's a prefix sum or we reached the first block, we break ...*/\
                        if (flags == 0x80000000u || prevBlockIndex == 0)                            \
                        {                                                                           \
                            break;                                                                  \
                        }                                                                           \
                        else                                                                        \
                        {                                                                           \
                            /* in case we encountered a sum -- we already accumulated it*/          \
                            /* at the same time we didn't reach the first block, so we go backward */\
                            --prevBlockIndex;                                                       \
                        }                                                                           \
                    }                                                                               \
                    /* else -- just loop waiting for the current block */                           \
                }

            LOOKBACK_LOOP(Stream);

            // NOTE(pamartis): 0x80000000u is used to mark that the data we're storing is the prefix sum
            const uint32_t lookbackStreamPrfx = ((prevBlockTotalStreamPrefix + streamPrefix + streamCount) & 0x3fffffffu) | 0x80000000u;
            LOOKBACK_STORE(Stream, lookbackStreamSum, lookbackStreamPrfx);

            prevBlockIndex = thisBlockIndex - 1u;

            LOOKBACK_LOOP(Group);

            const uint32_t lookbackGroupPrfx = ((prevBlockTotalGroupPrefix + groupPrefix + groupCount) & 0x3fffffffu) | 0x80000000u;
            LOOKBACK_STORE(Group, lookbackGroupSum, lookbackGroupPrfx);
        }
    }
    ZstdLitStreamCountToPrefix[i] = WaveReadLaneAt(prevBlockTotalStreamPrefix, lastLocalIndex) + streamPrefix + streamCount;
    ZstdLitGroupCountToPrefix[i] = WaveReadLaneAt(prevBlockTotalGroupPrefix, lastLocalIndex) + groupPrefix + groupCount;

    // NOTE(pamartis): the last thread in the dispatch writes its "inclusive" prefix sum because it's the total number of threadgroups
    // to be dispatched for literal decompression
    if (i == Constants.workItemCount - 1)
    {
        ZstdCounters[0].DecompressLiteralsGroups = ZstdLitGroupCountToPrefix[i];
    }
}
