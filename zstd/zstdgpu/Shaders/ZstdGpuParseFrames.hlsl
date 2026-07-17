/**
 * ZstdGpuParseFrames.hlsl
 *
 * A compute shader that parses Zstd frames.
 * The shader maps one frame to a single thread.
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
    uint32_t frameCount;
    uint32_t compressedBufferSizeInBytes;
    uint32_t countBlocksOnly;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_PARSE_FRAMES_SRT()
#include "../zstdgpu_srt_decl_undef.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=2), UAV(u0, numDescriptors=14)), RootConstants(b0, num32BitConstants=3)")]
[numthreads(kzstdgpu_TgSizeX_ParseCompressedBlocks, 1, 1)]
void main(uint i : SV_DispatchThreadId)
{
    zstdgpu_ParseFrames_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_PARSE_FRAMES_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    srt.frameCount                  = Constants.frameCount;
    srt.compressedBufferSizeInBytes = Constants.compressedBufferSizeInBytes;
    srt.countBlocksOnly             = Constants.countBlocksOnly;

    zstdgpu_ShaderEntry_ParseFrames(srt, i);
}
