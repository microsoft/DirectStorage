/**
 * ZstdGpuDecompressLiterals.hlsl
 *
 * A compute shader that decompresses Huffman-compressed literals
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
    uint32_t huffmanTableSlotCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#define ZSTDGPU_RO_RAW_BUFFER_DECL(type, name, index)              ZSTDGPU_RO_RAW_BUFFER(type)                ZstdIn##name    : register(t##index);
#define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                  ZSTDGPU_RO_BUFFER(type)                    ZstdIn##name    : register(t##index);
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                  ZSTDGPU_RW_BUFFER(type)                    ZstdInOut##name : register(u##index);
#define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index) ZSTDGPU_RO_TYPED_BUFFER(hlsl_type, type)   ZstdIn##name    : register(t##index);
#define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index) ZSTDGPU_RW_TYPED_BUFFER(hlsl_type, type)   ZstdInOut##name : register(u##index);

ZSTDGPU_DECOMPRESS_LITERALS_SRT()

#undef ZSTDGPU_RW_TYPED_BUFFER_DECL
#undef ZSTDGPU_RO_TYPED_BUFFER_DECL
#undef ZSTDGPU_RW_BUFFER_DECL
#undef ZSTDGPU_RO_BUFFER_DECL
#undef ZSTDGPU_RO_RAW_BUFFER_DECL

// WARN(pamartis): Wasteful, need only uint8_t but HLSL doesn't support it
groupshared uint32_t GS_Lds[kzstdgpu_MaxCount_HuffmanTableExpandedUInts];
#define ZSTDGPU_LDS GS_Lds
#include "../zstdgpu_lds_hlsl.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=9), UAV(u0, numDescriptors=1)),RootConstants(b0, num32BitConstants=1)")]
[numthreads(kzstdgpu_TgSizeX_DecompressLiterals, 1, 1)]
void main(uint groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_DecompressLiterals_SRT srt;

#define ZSTDGPU_RO_RAW_BUFFER_DECL(type, name, index)                  srt.in##name    = ZstdIn##name;
#define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                      srt.in##name    = ZstdIn##name;
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                      srt.inout##name = ZstdInOut##name;
#define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     srt.in##name    = ZstdIn##name;
#define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     srt.inout##name = ZstdInOut##name;
    ZSTDGPU_DECOMPRESS_LITERALS_SRT()

#undef ZSTDGPU_RW_TYPED_BUFFER_DECL
#undef ZSTDGPU_RO_TYPED_BUFFER_DECL
#undef ZSTDGPU_RW_BUFFER_DECL
#undef ZSTDGPU_RO_BUFFER_DECL
#undef ZSTDGPU_RO_RAW_BUFFER_DECL
    srt.huffmanTableSlotCount   = Constants.huffmanTableSlotCount;

    if (groupId >= srt.inCounters[kzstdgpu_CounterIndex_DecompressLiteralsGroups])
        return;

    zstdgpu_ShaderEntry_DecompressLiterals(srt, groupId, i, kzstdgpu_TgSizeX_DecompressLiterals);
}
