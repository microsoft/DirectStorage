/**
 * ZstdGpuDecompressSequences_MultiStream.hlsli
 *
 * A compute shader that decompresses FSE-compressed Sequences.
 * The shader maps one stream of FSE-compressed sequences to a single thread.
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

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

#ifndef kzstdgpu_DecompressSequences_StreamsPerTG
#   error 'kzstdgpu_DecompressSequences_StreamsPerTG' must be defined before including this '.hlsli'
#endif

#include "../zstdgpu_shaders.h"

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()
#include "../zstdgpu_srt_decl_undef.h"

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=6), UAV(u0, numDescriptors=7)), RootConstants(b0, num32BitConstants=2)")]
[numthreads(kzstdgpu_DecompressSequences_StreamsPerTG, 1, 1)]
void main(uint32_t2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset);
    zstdgpu_DecompressSequences_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    zstdgpu_ShaderEntry_DecompressSequences_MultiStream(srt, groupId, i, kzstdgpu_DecompressSequences_StreamsPerTG);
}
