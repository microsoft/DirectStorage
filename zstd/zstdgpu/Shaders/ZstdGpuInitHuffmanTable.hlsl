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

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
    uint32_t HuffmanTableBase;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_INIT_HUFFMAN_TABLE_SRT()
#include "../zstdgpu_srt_decl_undef.h"

// WARN(pamartis): Wasteful, need only uint8_t but HLSL doesn't support it
groupshared uint32_t GS_Lds[kzstdgpu_InitHuffmanTable_LdsSize];
#define ZSTDGPU_LDS GS_Lds
#include "../zstdgpu_lds_hlsl.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0, numDescriptors=3)), RootConstants(b0, num32BitConstants=3)")]
[numthreads(kzstdgpu_TgSizeX_DecompressLiterals, 1, 1)]
void main(uint2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_InitHuffmanTable_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_INIT_HUFFMAN_TABLE_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset);

    groupId = (Constants.HuffmanTableBase != 0) ? (Constants.HuffmanTableBase - 1 - groupId) : groupId;

    zstdgpu_ShaderEntry_InitHuffmanTable(srt, groupId, i, kzstdgpu_TgSizeX_DecompressLiterals);
}
