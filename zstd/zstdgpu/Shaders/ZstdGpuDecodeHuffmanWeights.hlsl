/**
 * ZstdGpuDecodeHuffmanWeights.hlsl
 *
 * A compute shader that decode Huffman Weights from the original compressed block.
 * The shader maps one stream of Huffman Weights to a single thread.
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
    uint32_t ZstdCompressedBlockCount;
    uint32_t ZstdCompressedBufferSizeInBytes;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_DECODE_HUFFMAN_WEIGHTS_SRT()
#include "../zstdgpu_srt_decl_undef.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=3), UAV(u0, numDescriptors=2)), RootConstants(b0, num32BitConstants=4)")]
[numthreads(kzstdgpu_TgSizeX_DecodeHuffmanWeights, 1, 1)]
void main(uint2 groupId2 : SV_GroupID, uint32_t i : SV_GroupThreadId)
{
    i += zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset) * kzstdgpu_TgSizeX_DecodeHuffmanWeights;

    zstdgpu_DecodeHuffmanWeights_SRT srt;
    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_DECODE_HUFFMAN_WEIGHTS_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    srt.compressedBlockCount        = Constants.ZstdCompressedBlockCount;
    srt.compressedBufferSizeInBytes = Constants.ZstdCompressedBufferSizeInBytes;
    zstdgpu_ShaderEntry_DecodeHuffmanWeights(srt, i);
}
