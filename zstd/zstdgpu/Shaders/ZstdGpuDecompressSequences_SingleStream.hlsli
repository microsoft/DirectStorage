/**
 * ZstdGpuDecompressSequences_SingleStream.hlsli
 *
 * A compute shader that decompresses single FSE-compressed sequences stream per TG by preloading
 * FSE tables into LDS and then sampling FSE tables from LDS for faster access comparing to L0/Scalar cache.
 *
 * Because of this, this shader must be compiled as a small threadgroup as possible.
 *
 * However, because D3D12 doesn't allow to query on C++ side the size of the actual wave the compiler
 * have chosen for a threadgroup, on hardware with multiple supported wave sizes -- it's not possible
 * to make exact choice of the permutation with the 'numthreads' matching wave size.
 * We can't use WaveSize in HLSL either because it would require SM 6.5.
 *
 * Therefore 'kzstdgpu_TgSizeX_DecompressSequences_SingleStream' must be defined before including this file,
 * It controls threadgroup size which on hardware with multiple wave size equals to the size of the largest
 * wave that could be created. We determine the actual wave size in the threadgroup by using `WaveLaneCount()`
 * intrinsic and early out waves that are no longer needed.
 * This has an potential overhead of launching unused waves/more constrained (multiple waves) threadgroups
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

#if !kzstdgpu_DecompressSequences_SingleStream_NoLdsFseCache
groupshared uint32_t Lds[kzstdgpu_DecompressSequences_SingleStream_LdsFseCache_LdsSize];
#define ZSTDGPU_LDS Lds
#include "../zstdgpu_lds_hlsl.h"
#endif

#ifndef kzstdgpu_TgSizeX_DecompressSequences_SingleStream
#   error 'kzstdgpu_TgSizeX_DecompressSequences_SingleStream' must be defined before including this '.hlsli'
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=6), UAV(u0, numDescriptors=7)), RootConstants(b0, num32BitConstants=2)")]
[numthreads(kzstdgpu_TgSizeX_DecompressSequences_SingleStream, 1, 1)]
void main(uint32_t2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset);
    zstdgpu_DecompressSequences_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    zstdgpu_ShaderEntry_DecompressSequences_SingleStream(srt, groupId, i, kzstdgpu_TgSizeX_DecompressSequences_SingleStream);
}
