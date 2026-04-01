/**
 * ZstdGpuDecompressHuffmanWeights.hlsl
 *
 * A compute shader that decompresses FSE-compressed Huffman Weights.
 * The shader maps one stream of FSE-compressed Huffman Weights to a single thread.
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

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_DECOMPRESS_HUFFMAN_WEIGHTS_SRT()
#include "../zstdgpu_srt_decl_undef.h"

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=5), UAV(u0, numDescriptors=2)), RootConstants(b0, num32BitConstants=2)")]
[numthreads(kzstdgpu_TgSizeX_DecompressHuffmanWeights, 1, 1)]
void main(uint2 groupId : SV_GroupID, uint32_t i : SV_GroupThreadId)
{
    i += zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_DecompressHuffmanWeights;

    zstdgpu_DecompressHuffmanWeights_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_DECOMPRESS_HUFFMAN_WEIGHTS_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    zstdgpu_ShaderEntry_DecompressHuffmanWeights(srt, i);
}
