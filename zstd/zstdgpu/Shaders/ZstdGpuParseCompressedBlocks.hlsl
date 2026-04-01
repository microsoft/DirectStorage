/**
 * ZstdGpuParseCompressedBlocks.hlsl
 *
 * A compute shader that parses compressed Zstd blocks, extracts locations of
 * other sub-blocks such as literals, Huffman Weights and FSE tables, and
 * executes FSE table index propagation via "Decoupled Lookback",
 * so each block refer its FSE tables directly.
 * The shader maps one compressed block to a single thread.
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
    uint32_t compressedBufferSizeInBytes;
    uint32_t frameCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_PARSE_COMPRESSED_BLOCKS_SRT()
#include "../zstdgpu_srt_decl_undef.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=4), UAV(u0, numDescriptors=16)), RootConstants(b0, num32BitConstants=4)")]
[numthreads(kzstdgpu_TgSizeX_ParseCompressedBlocks, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_ParseCompressedBlocks + threadId;
    zstdgpu_ParseCompressedBlocks_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_PARSE_COMPRESSED_BLOCKS_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    srt.compressedBlockCount        = Constants.workItemCount;
    srt.compressedBufferSizeInBytes = Constants.compressedBufferSizeInBytes;
    srt.frameCount                  = Constants.frameCount;

    zstdgpu_ShaderEntry_ParseCompressedBlocks(srt, i);
}
