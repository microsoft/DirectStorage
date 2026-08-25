/**
 * ZstdGpuInitHuffmanTable.hlsl
 *
 * A compute shader that partially initializes Huffman table given weights.
 * The shader initializes single Huffman table per threadgroup.
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
#include "../.generated/ZstdGpuSrt_InitHuffmanTable.h"

// WARN(pamartis): Wasteful, need only uint8_t but HLSL doesn't support it
groupshared uint32_t GS_Lds[kzstdgpu_InitHuffmanTable_LdsSize];
#define ZSTDGPU_LDS GS_Lds
#include "../zstdgpu_lds_hlsl.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature(ZSTDGPU_SRT_RS_InitHuffmanTable)]
[numthreads(kzstdgpu_TgSizeX_DecompressLiterals, 1, 1)]
void main(uint2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_InitHuffmanTable_SRT srt;

    zstdgpu_Srt_Fill(srt);

    uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, srt.tgOffset);

    groupId = (srt.fseCompressed == 0u) ? (srt.inCounters[0].Blocks_CMP - 1u - groupId) : groupId;

    zstdgpu_ShaderEntry_InitHuffmanTable(srt, groupId, i, kzstdgpu_TgSizeX_DecompressLiterals);
}
