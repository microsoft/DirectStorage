/**
 * ZstdGpuMemsetMemcpy.hlsl
 *
 * A compute shader that does either a memset operation on the buffer or a memcpy.
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
    uint32_t blockCount;
    uint32_t frameCount;
    uint32_t flags;
};

ConstantBuffer<Consts>                  Constants                           : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_MEMSET_MEMCPY_SRT()
#include "../zstdgpu_srt_decl_undef.h"

StructuredBuffer<uint32_t>              ZstdInBlockSizePrefixTyped          : register(t4);

StructuredBuffer<uint32_t>              ZstdInPerFrameBlockSizePrefixTyped  : register(t5);

StructuredBuffer<zstdgpu_OffsetAndSize> ZstdInBlocksRefsTyped               : register(t6);

StructuredBuffer<uint32_t>              ZstdInGlobalBlockIndexTyped         : register(t7);

groupshared uint32_t lds[kzstdgpu_TgSizeX_MemsetMemcpy];

inline void GetDestinationInfo(uint32_t blockIdx,
                               uint32_t i,
                               // These are really just `out`:
                               ZSTDGPU_PARAM_INOUT(zstdgpu_OffsetAndSize) blockRef,
                               ZSTDGPU_PARAM_INOUT(uint32_t) byteIdx,
                               ZSTDGPU_PARAM_INOUT(zstdgpu_OffsetAndSize) dstFrameOffsetAndSize,
                               ZSTDGPU_PARAM_INOUT(uint32_t) dstBlockOffset)
{
    blockRef = ZstdInBlocksRefsTyped[blockIdx];

    byteIdx = i - ZstdInBlockSizePrefixTyped[blockIdx];

    const uint32_t globalBlockIdx = ZstdInGlobalBlockIndexTyped[blockIdx];

    uint32_t globalBlockGlobalOffset = 0;
    [branch] if (globalBlockIdx > 0)
    {
        globalBlockGlobalOffset = ZstdInBlockSizePrefix[globalBlockIdx - 1];
    }

    const uint32_t frameIdx = zstdgpu_BinarySearch(ZstdInPerFrameBlockCountAll, 0, Constants.frameCount, globalBlockIdx);

    const uint32_t frameFirstGlobalBlockIdx = ZstdInPerFrameBlockCountAll[frameIdx];

    uint32_t frameFirstBlockGlobalOffset = 0;
    [branch] if (frameFirstGlobalBlockIdx > 0)
    {
        frameFirstBlockGlobalOffset = ZstdInBlockSizePrefix[frameFirstGlobalBlockIdx - 1];
    }

    const uint32_t frameRelativeBlockOffset = globalBlockGlobalOffset - frameFirstBlockGlobalOffset;

    dstFrameOffsetAndSize = ZstdInUnCompressedFramesRefs[frameIdx];

    dstBlockOffset = dstFrameOffsetAndSize.offs + frameRelativeBlockOffset;
}

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=4), UAV(u0, numDescriptors=1)), SRV(t4), SRV(t5), SRV(t6), SRV(t7), RootConstants(b0, num32BitConstants=5)")]
[numthreads(kzstdgpu_TgSizeX_MemsetMemcpy, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadIdInGroup : SV_GroupThreadId)
{
    const uint32_t scaledGroupId = zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_MemsetMemcpy;
    const uint32_t i = scaledGroupId + threadIdInGroup;
    const uint32_t numActiveThreads = zstdgpu_MinU32(Constants.workItemCount - scaledGroupId, kzstdgpu_TgSizeX_MemsetMemcpy);

    // There are likely much fewer blocks this threadgroup will write than kzstdgpu_TgSizeX_MemsetMemcpy, and commonly a single block.
    // Do most of the work via scalar instructions.
    if (threadIdInGroup == 0)
    {
        const uint32_t groupLeaderBlockIdx = zstdgpu_BinarySearch(ZstdInBlockSizePrefixTyped, 0, Constants.blockCount, scaledGroupId + 0);
        lds[0] = groupLeaderBlockIdx;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint32_t groupLeaderBlockIdx = WaveReadLaneFirst(lds[0]);

    uint32_t iEndForGroupLeaderBlock = uint32_t(-1);
    [branch] if (Constants.blockCount >= 2)
    {
        iEndForGroupLeaderBlock = ZstdInBlockSizePrefixTyped[groupLeaderBlockIdx + 1];
    }

    zstdgpu_OffsetAndSize blockRef;
    uint32_t byteIdx;
    zstdgpu_OffsetAndSize dstFrameOffsetAndSize;
    uint32_t dstBlockOffset;

    // The else path can handle any case, but try to pass a uniform blockIdx to GetDestinationInfo.
    if (iEndForGroupLeaderBlock >= scaledGroupId + numActiveThreads)
    {
        if (i >= Constants.workItemCount)
            return;

        GetDestinationInfo(groupLeaderBlockIdx, i, blockRef, byteIdx, dstFrameOffsetAndSize, dstBlockOffset);
    }
    else
    {
        // After using SMEM to narrow down the block range, do one VMEM load per-wave into LDS,
        // which should be sufficient since we only kept nonzero-size RLE/Raw blocks.
        lds[threadIdInGroup] = ZstdInBlockSizePrefixTyped[zstdgpu_MinU32(groupLeaderBlockIdx + threadIdInGroup, Constants.blockCount - 1)];
        GroupMemoryBarrierWithGroupSync();
        if (i >= Constants.workItemCount)
            return;

        const uint32_t numActiveBlocks = zstdgpu_MinU32(Constants.blockCount - groupLeaderBlockIdx, numActiveThreads);
        uint32_t blockIdx;
        // Instead of a binary search or linear search from the end, this does a linear search from the beginning.
        // This has a more complicated loop exit test then linear search from end, but assuming block sizes are
        // large compared to kzstdgpu_TgSizeX_MemsetMemcpy, the loop should iterate much less times.
        for (uint32_t r = 0;; ++r)
        {
            if ((r == numActiveBlocks - 1) ||
                (i >= lds[r] && i < lds[r + 1]))
            {
                blockIdx = groupLeaderBlockIdx + r;
                break;
            }
        }

        GetDestinationInfo(blockIdx, i, blockRef, byteIdx, dstFrameOffsetAndSize, dstBlockOffset);
    }

    // Shouldn't be needed for valid data since already checked Constants.workItemCount?
    if (byteIdx >= dstFrameOffsetAndSize.size)
        return;

    [branch] if (Constants.flags & 0x1u)
    {
        const uint32_t byteOfs = blockRef.offs + byteIdx;

        ZstdInOutUnCompressedFramesData[dstBlockOffset + byteIdx] = (ZstdInCompressedData[byteOfs >> 2u] >> ((byteOfs & 3u) << 3u)) & 0xffu;
    }
    else
    {
        ZstdInOutUnCompressedFramesData[dstBlockOffset + byteIdx] = blockRef.offs;
    }
}