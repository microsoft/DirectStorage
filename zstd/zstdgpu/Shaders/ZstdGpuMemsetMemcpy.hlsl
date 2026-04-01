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

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=4), UAV(u0, numDescriptors=1)), SRV(t4), SRV(t5), SRV(t6), SRV(t7), RootConstants(b0, num32BitConstants=5)")]
[numthreads(kzstdgpu_TgSizeX_MemsetMemcpy, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    i += zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_MemsetMemcpy;

    if (i >= Constants.workItemCount)
        return;

    const uint32_t blockIdx = zstdgpu_BinarySearch(ZstdInBlockSizePrefixTyped, 0, Constants.blockCount, i);

    const zstdgpu_OffsetAndSize blockRef = ZstdInBlocksRefsTyped[blockIdx];

    const uint32_t byteIdx = i - ZstdInBlockSizePrefixTyped[blockIdx];

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

    const zstdgpu_OffsetAndSize dstFrameOffsetAndSize = ZstdInUnCompressedFramesRefs[frameIdx];

    const uint32_t dstBlockOffset = dstFrameOffsetAndSize.offs + frameRelativeBlockOffset;

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