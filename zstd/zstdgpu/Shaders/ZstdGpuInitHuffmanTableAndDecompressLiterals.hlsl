/**
 * ZstdGpuInitHuffmanTableAndDecompressLiterals.hlsl
 *
 * A compute shader that initialises Huffman table given Huffman weights and decompresses
 * Huffman-compressed literals
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

#if !defined(__HLSL_VERSION) || (__HLSL_VERSION < 2021)
#pragma dxc diagnostic ignored "-Wfor-redefinition"
#endif

struct Consts
{
    uint32_t huffmanTableSlotCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_SRT()
#include "../zstdgpu_srt_decl_undef.h"

// WARN(pamartis): Wasteful, need only uint8_t but HLSL doesn't support it
groupshared uint32_t GS_Lds[kzstdgpu_InitHuffmanTableAndDecompressLiterals_LdsSize];
#define ZSTDGPU_LDS GS_Lds
#include "../zstdgpu_lds_hlsl.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=8), UAV(u0, numDescriptors=1)),RootConstants(b0, num32BitConstants=1)")]
[numthreads(kzstdgpu_TgSizeX_DecompressLiterals, 1, 1)]
void main(uint groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_SRT()
    #include "../zstdgpu_srt_decl_undef.h"
    srt.huffmanTableSlotCount   = Constants.huffmanTableSlotCount;

    if (groupId >= srt.inCounters[0].DecompressLiteralsGroups)
        return;

    zstdgpu_ShaderEntry_InitHuffmanTable_And_DecompressLiterals(srt, groupId, i);
}
