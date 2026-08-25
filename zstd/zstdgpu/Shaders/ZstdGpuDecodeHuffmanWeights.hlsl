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
#include "../.generated/ZstdGpuSrt_DecodeHuffmanWeights.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature(ZSTDGPU_SRT_RS_DecodeHuffmanWeights)]
[numthreads(kzstdgpu_TgSizeX_DecodeHuffmanWeights, 1, 1)]
void main(uint2 groupId2 : SV_GroupID, uint32_t i : SV_GroupThreadId)
{
    zstdgpu_DecodeHuffmanWeights_SRT srt;
    zstdgpu_Srt_Fill(srt);

    i += zstdgpu_ConvertTo32BitGroupId(groupId2, srt.tgOffset) * kzstdgpu_TgSizeX_DecodeHuffmanWeights;

    zstdgpu_ShaderEntry_DecodeHuffmanWeights(srt, i);
}
