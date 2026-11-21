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

#include "../srt_headers/ZstdGpuSrt_MemsetMemcpy.h"

[RootSignature(ZSTDGPU_SRT_RS_MemsetMemcpy)]
[numthreads(kzstdgpu_TgSizeX_MemsetMemcpy, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    // Read tgOffset and workItemCount through zstdgpu_Srt_Fill, which resolves workItemCount under executeIndirectWorkaround.
    zstdgpu_MemsetMemcpy_SRT srt;

    zstdgpu_Srt_Fill(srt);

    i += zstdgpu_ConvertTo32BitGroupId(groupId, srt.tgOffset) * kzstdgpu_TgSizeX_MemsetMemcpy;

    if (i >= srt.workItemCount)
    {
        return;
    }

    const uint32_t blockCnt = (ZstdConstants_MemsetMemcpy.flags & 0x1u) ? ZstdInCounters[0].Blocks_RAW : ZstdInCounters[0].Blocks_RLE;
    const uint32_t blockIdx = zstdgpu_BinarySearch(ZstdInBlockSizePrefixTyped, 0, blockCnt, i);

    const zstdgpu_OffsetAndSize blockRef = ZstdInBlocksRefsTyped[blockIdx];

    const uint32_t byteIdx = i - ZstdInBlockSizePrefixTyped[blockIdx];

    const uint32_t globalBlockIdx = ZstdInGlobalBlockIndexTyped[blockIdx];

    const uint32_t dstBlockOffset = ZstdInBlockDestOffs[globalBlockIdx];

    // `~0` marks a block past a partial decode's limit (see ZstdGpuComputeDestBlockOffsets.hlsl).
    if (byteIdx >= blockRef.size || ~0u == dstBlockOffset)
    {
        return;
    }

    [branch] if (ZstdConstants_MemsetMemcpy.flags & 0x1u)
    {
        const uint32_t byteOfs = blockRef.offs + byteIdx;

        ZstdInOutUnCompressedFramesData[dstBlockOffset + byteIdx] = (ZstdInCompressedData[byteOfs >> 2u] >> ((byteOfs & 3u) << 3u)) & 0xffu;
    }
    else
    {
        ZstdInOutUnCompressedFramesData[dstBlockOffset + byteIdx] = blockRef.offs;
    }
}