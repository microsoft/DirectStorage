/**
 * ZstdGpuPrefixSequenceOffsets.hlsl
 *
 * A compute shader producing a "prefix" (propagated result) of final 'offsets' per block
 * The computation is based on "Decoupled Lookback" approach.
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
    uint32_t frameCount;
};

ConstantBuffer<Consts>          Constants                           : register(b0);

RWStructuredBuffer<uint32_t>    ZstdPerSeqStreamFinalOffset1        : register(u0);
RWStructuredBuffer<uint32_t>    ZstdPerSeqStreamFinalOffset2        : register(u1);
RWStructuredBuffer<uint32_t>    ZstdPerSeqStreamFinalOffset3        : register(u2);

globallycoherent
RWStructuredBuffer<uint32_t>    ZstdPerSeqStreamFinalOffset1Lookback :register(u3);

globallycoherent
RWStructuredBuffer<uint32_t>    ZstdPerSeqStreamFinalOffset2Lookback :register(u4);

globallycoherent
RWStructuredBuffer<uint32_t>    ZstdPerSeqStreamFinalOffset3Lookback :register(u5);

StructuredBuffer<uint32_t>      ZstdPerFrameSeqStreamMinIdx         : register(t0);

StructuredBuffer<uint32_t>      ZstdFrameBlockCountAll              : register(t1);

StructuredBuffer<zstdgpu_SeqStreamInfo> ZstdSeqRefs                 : register(t2);

StructuredBuffer<zstdgpu_Counters>     ZstdCounters                 : register(t3);

#if defined(__XBOX_SCARLETT)
#   define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("UAV(u0), UAV(u1), UAV(u2), UAV(u3), UAV(u4), UAV(u5), SRV(t0), SRV(t1), SRV(t2), SRV(t3), RootConstants(b0, num32BitConstants=3)")]
[numthreads(kzstdgpu_TgSizeX_PrefixSequenceOffsets, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_PrefixSequenceOffsets + threadId;
    const uint32_t blockSize = min(kzstdgpu_TgSizeX_PrefixSequenceOffsets, WaveGetLaneCount());
    const uint32_t thisBlockIndex = WaveReadLaneFirst(i / blockSize);
    const uint32_t thisLocalIndex = i % blockSize;

    if (i >= ZstdCounters[0].Seq_Streams)
        return;

    // NOTE(pamartis): Given an exclusive prefix sum of compressed block counts per block (ZstdFrameBlockCountCMP)
    // Each sequence stream (threadId) does a binary search of its frame index (by using its (compressed) block id).
    // Then, when frame index is known each block fetches the index of the first compressed block in that frame
    // with non-zero sequence count and if current compressed block's index (threadId) matches that index --
    // it resolves its "repeat" offsets (if any) using "default" start offsets per frame.
    const uint32_t blockId = ZstdSeqRefs[i].blockId;
    const uint32_t frameId = zstdgpu_BinarySearch(ZstdFrameBlockCountAll, 0, Constants.frameCount, blockId);

    const uint32_t seqStreamIdxFirstInFrame = ZstdPerFrameSeqStreamMinIdx[frameId];

    uint32_t3 o = uint32_t3(
        ZstdPerSeqStreamFinalOffset1[i],
        ZstdPerSeqStreamFinalOffset2[i],
        ZstdPerSeqStreamFinalOffset3[i]
    );

    bool needsPropagationAfterLookback = false;


    // NOTE(pamartis): for every first in zstd frame compressed block that contains non-zero sequence count, convert "final" offsets
    // into "non-repeat" offsets (if they're re-encoded "repeat" offsets)
    const bool isFirstSequenceStreamInFrame = seqStreamIdxFirstInFrame != ~0u && seqStreamIdxFirstInFrame == i;
    if (isFirstSequenceStreamInFrame)
    {
        const uint32_t3 b = uint32_t3(1, 4, 8) + 3;

        zstdgpu_DecodeSeqRepeatOffsetsAndApplyPreviousOffsets(o.x, o.y, o.z, b.x, b.y, b.z);

        needsPropagationAfterLookback = true;
    }

    const uint32_t lastLocalIndex = WaveActiveCountBits(true) - 1u;

    #if 0
        #define LOOKBACK_STORE(name, prev, value) Zstd##name##Lookback[thisBlockIndex] = (value); DeviceMemoryBarrier();
    #else
        #define LOOKBACK_STORE(name, prev, value) InterlockedCompareStore(Zstd##name##Lookback[thisBlockIndex], prev, (value))
    #endif


    #define WAVE_SHUFFLE(v, and_mask, or_mask, xor_mask) WaveReadLaneAt(v, ((WaveGetLaneIndex() & (and_mask)) | (or_mask)) ^ (xor_mask))

    #define WAVE_BROADCAST(v, group_size, group_lane) WAVE_SHUFFLE(v, ~(group_size - 1u), group_lane, 0)

    #define WAVE_PROPAGATE_STEP(o, group_size)  \
        if (blockSize >= group_size /** this condition is expected to be a compile-time condition, so no real branch */) \
        {                                                                                                           \
            /** this branch is wave-uniform branch that executes only if any of lanes need propagation*/            \
            const uint32_t offsetOR = o.x | o.y | o.z;                                                              \
            if (WaveActiveAnyTrue(zstdgpu_DecodeSeqRepeatOffsetEncoded(offsetOR) > 0))                              \
            {                                                                                                       \
                /* for every group of `group_size` consecutive lanes, broadcast the value from the last lane of the "odd" sub-group of 2x smaller size) */\
                const uint32_t3 b = WAVE_BROADCAST(o, group_size, group_size / 2u - 1u);                            \
                /* for every group of `group_size` consecutive lanes */                                             \
                /* propagate element from the last lane of the "odd" sub-group of 2x smaller size  */               \
                /* into all elements of the "even" sub-group of 2x smaller size when propagated value makes sense */\
                [branch] if ((WaveGetLaneIndex() & (group_size / 2u)))                                              \
                {                                                                                                   \
                    zstdgpu_DecodeSeqRepeatOffsetsAndApplyPreviousOffsets(o.x, o.y, o.z, b.x, b.y, b.z);            \
                }                                                                                                   \
            }                                                                                                       \
        }

    // NOTE(pamartis):  STEP1 - populate "lookback" information early for current block
    //
    //  - All offsets in a given wave is zero (meaning "invalid", meaning this compressed block doesn't use sequences, and therefore this block "lookback" can be skipped by other blocks)
    //  - All offsets in the last lane of the current wave is not "encoded", meaning those are actual offsets, so can be stored immediately
    const uint32_t offsetOR = o.x | o.y | o.z;

    uint32_t3 lastLaneSelfResult = o;

    /** wave-uniform branch */  if (0 == zstdgpu_DecodeSeqRepeatOffsetEncoded(WaveReadLaneAt(offsetOR, lastLocalIndex)))
    {
        // if at least one offset is "encoded"
        // we need propagation step after "lookback" step to convert "encoded" offsets to actual offsets
        needsPropagationAfterLookback = zstdgpu_DecodeSeqRepeatOffsetEncoded(offsetOR) > 0;

        // NOTE (pamartis): it's important the last lane does the store, because only its VGPR lane contain valid value when non all offsets are zero
        if (WaveGetLaneIndex() == lastLocalIndex)
        {
            LOOKBACK_STORE(PerSeqStreamFinalOffset1, 0, zstdgpu_Encode30BitLookbackFull(o.x));
            LOOKBACK_STORE(PerSeqStreamFinalOffset2, 0, zstdgpu_Encode30BitLookbackFull(o.y));
            LOOKBACK_STORE(PerSeqStreamFinalOffset3, 0, zstdgpu_Encode30BitLookbackFull(o.z));
        }
    }
    //  - For all other cases, do the propagation and store the results
    else
    {
        uint32_t3 p = o; // p - for "propagated" offsets
        WAVE_PROPAGATE_STEP(p, 2);
        WAVE_PROPAGATE_STEP(p, 4);
        WAVE_PROPAGATE_STEP(p, 8);
        WAVE_PROPAGATE_STEP(p, 16);
        WAVE_PROPAGATE_STEP(p, 32);
#if kzstdgpu_TgSizeX_PrefixSequenceOffsets > 32
#   error TgSizeX with more than 32 elements requires enabling `WAVE_PROPAGATE_STEP(p, 64);` and `WAVE_PROPAGATE_STEP(p, 128);`
#endif

#if kzstdgpu_TgSizeX_PrefixSequenceOffsets > 128
#   error TgSizeX with more than 128 elements requires implementing cross-wave propagation in LDS
#endif

        // if at least one offset is "encoded" after propagation attempt
        // we need propagation step after "lookback" step to convert offset to actual offsets
        needsPropagationAfterLookback = zstdgpu_DecodeSeqRepeatOffsetEncoded(p.x | p.y | p.z) > 0;

        // if no lanes need propagation after reedback -- it means this propagation was successful, so we store actual block offsets
        if (WaveActiveAnyTrue(needsPropagationAfterLookback) == false)
        {
            ZstdPerSeqStreamFinalOffset1[i] = p.x;
            ZstdPerSeqStreamFinalOffset2[i] = p.y;
            ZstdPerSeqStreamFinalOffset3[i] = p.z;
        }

        // NOTE (pamartis): it's important the last lane does the store, because only its VGPR lane contain valid value for the block
        if (WaveGetLaneIndex() == lastLocalIndex)
        {
            if (needsPropagationAfterLookback)
            {
                lastLaneSelfResult.x = zstdgpu_Encode30BitLookbackSelf(p.x);
                lastLaneSelfResult.y = zstdgpu_Encode30BitLookbackSelf(p.y);
                lastLaneSelfResult.z = zstdgpu_Encode30BitLookbackSelf(p.z);

                LOOKBACK_STORE(PerSeqStreamFinalOffset1, 0, lastLaneSelfResult.x);
                LOOKBACK_STORE(PerSeqStreamFinalOffset2, 0, lastLaneSelfResult.y);
                LOOKBACK_STORE(PerSeqStreamFinalOffset3, 0, lastLaneSelfResult.z);
            }
            else
            {
                LOOKBACK_STORE(PerSeqStreamFinalOffset1, 0, zstdgpu_Encode30BitLookbackFull(p.x));
                LOOKBACK_STORE(PerSeqStreamFinalOffset2, 0, zstdgpu_Encode30BitLookbackFull(p.y));
                LOOKBACK_STORE(PerSeqStreamFinalOffset3, 0, zstdgpu_Encode30BitLookbackFull(p.z));
            }
        }
    }

    if (0 != thisBlockIndex && WaveActiveAnyTrue(needsPropagationAfterLookback))
    {
        uint32_t prevBlockIndex = thisBlockIndex - 1u;

        #if 0
            // BUG(pamartis): this varaint of code reads incorrect values on some HW, so it looks like `globallycoherent`
            // keyword doesn't work for reads
            #define LOOKBACK_READ(name)     \
                const uint32_t prev_##name = Zstd##name##Lookback[prevBlockIndex]
        #else
            #define LOOKBACK_READ(name)     \
                uint32_t prev_##name;       \
                InterlockedAdd(Zstd##name##Lookback[prevBlockIndex], 0, prev_##name)
        #endif

        if (WaveIsFirstLane())
        {
        for (;;)
        {
            LOOKBACK_READ(PerSeqStreamFinalOffset1);
            LOOKBACK_READ(PerSeqStreamFinalOffset2);
            LOOKBACK_READ(PerSeqStreamFinalOffset3);

            const uint32_t prevBlock_Bits = prev_PerSeqStreamFinalOffset1 & prev_PerSeqStreamFinalOffset2 & prev_PerSeqStreamFinalOffset3;

            // NOTE(pamartis): AND'ing 3 values and then checking for a specific flag ensures all values were atomically updated.
            const uint32_t prevBlock_Flags = zstdgpu_Decode30BitLookbackFlags(prevBlock_Bits);
            if (prevBlock_Flags > 0)
            {
                prev_PerSeqStreamFinalOffset1 = zstdgpu_Decode30BitLookbackValue(prev_PerSeqStreamFinalOffset1);
                prev_PerSeqStreamFinalOffset2 = zstdgpu_Decode30BitLookbackValue(prev_PerSeqStreamFinalOffset2);
                prev_PerSeqStreamFinalOffset3 = zstdgpu_Decode30BitLookbackValue(prev_PerSeqStreamFinalOffset3);
                {
                    zstdgpu_DecodeSeqRepeatOffsetsAndApplyPreviousOffsets(o.x, o.y, o.z, prev_PerSeqStreamFinalOffset1, prev_PerSeqStreamFinalOffset2, prev_PerSeqStreamFinalOffset3);

                    // NOTE(pamartis): if it's a "full" propagation result of some block --
                    // meaning we just encountered non-"encoded" offsets and eliminated all "encoded" offsets or if it's the first block, we break ..
                    if (zstdgpu_Check30BitLookbackFull(prevBlock_Flags) > 0 || prevBlockIndex == 0)
                    {
                        break;
                    }
                    else
                    {
                        // NOTE(pamartis): in it was "self" propagation result of some block --
                        // meaning we updated "encoded" offsets and we didn't reach the first block, so we go backward
                        --prevBlockIndex;
                    }
                }
            }
            /* else -- just loop waiting for the current block */
        }
        }
    }

    if (WaveActiveAnyTrue(needsPropagationAfterLookback))
    {
        uint32_t3 p = o; // p - for "propagated" offsets, the first lane of "o" - contains "lookback" results (non-"encoded" offsets)
        WAVE_PROPAGATE_STEP(p, 2);
        WAVE_PROPAGATE_STEP(p, 4);
        WAVE_PROPAGATE_STEP(p, 8);
        WAVE_PROPAGATE_STEP(p, 16);
        WAVE_PROPAGATE_STEP(p, 32);

#if kzstdgpu_TgSizeX_PrefixSequenceOffsets > 32
#   error TgSizeX with more than 32 elements requires enabling `WAVE_PROPAGATE_STEP(p, 64);` and `WAVE_PROPAGATE_STEP(p, 128);`
#endif

#if kzstdgpu_TgSizeX_PrefixSequenceOffsets > 128
#   error TgSizeX with more than 128 elements requires implementing cross-wave propagation in LDS
#endif

        if (needsPropagationAfterLookback)
        {
            // NOTE (pamartis): it's important the last lane does the store, because only its VGPR lane contain valid value for the block
            if (WaveGetLaneIndex() == lastLocalIndex)
            {
                LOOKBACK_STORE(PerSeqStreamFinalOffset1, lastLaneSelfResult.x, zstdgpu_Encode30BitLookbackFull(p.x));
                LOOKBACK_STORE(PerSeqStreamFinalOffset2, lastLaneSelfResult.y, zstdgpu_Encode30BitLookbackFull(p.y));
                LOOKBACK_STORE(PerSeqStreamFinalOffset3, lastLaneSelfResult.z, zstdgpu_Encode30BitLookbackFull(p.z));
            }
        }

        // WARN(pamartis): here we store final abosolute offsets propagated across wave, this changes "zero" offsets for blocks
        // which don't use use sequences, which may lead to "redundant" writes.
        // However, we choose to do this to simplify CPU-side validation which "propagates" valid absolute offset into block
        // which don't require sequences
        ZstdPerSeqStreamFinalOffset1[i] = p.x;
        ZstdPerSeqStreamFinalOffset2[i] = p.y;
        ZstdPerSeqStreamFinalOffset3[i] = p.z;
    }
    // NOTE(pamartis): Added this condition to make sure the first sequence stream ('i') in the frame
    // always updates its "final" offsets even if 'needsPropagationAfterLookback' is set to 'false'.
    // 'needsPropagationAfterLookback' can be set to 'false' by the condition at the top of the shader entry
    // checking that all offsets are "non-encoded", but at the same time offsets of the first sequence stream ('i')
    // in the frame could have been modified from "encoded" to "non-encoded", so they have to be stored back
    else if (isFirstSequenceStreamInFrame)
    {
        ZstdPerSeqStreamFinalOffset1[i] = o.x;
        ZstdPerSeqStreamFinalOffset2[i] = o.y;
        ZstdPerSeqStreamFinalOffset3[i] = o.z;
    }
}
