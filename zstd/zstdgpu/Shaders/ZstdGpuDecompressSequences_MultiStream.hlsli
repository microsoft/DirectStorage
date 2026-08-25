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

#include "../.generated/ZstdGpuSrt_DecompressSequences.h"

[RootSignature(ZSTDGPU_SRT_RS_DecompressSequences)]
[numthreads(kzstdgpu_DecompressSequences_StreamsPerTG, 1, 1)]
void main(uint32_t2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_DecompressSequences_SRT srt;
    zstdgpu_Srt_Fill(srt);
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, srt.tgOffset);

    zstdgpu_ShaderEntry_DecompressSequences_MultiStream(srt, groupId, i, kzstdgpu_DecompressSequences_StreamsPerTG);
}
