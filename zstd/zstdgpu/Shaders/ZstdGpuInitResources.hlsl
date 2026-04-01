/**
 * ZstdGpuInitResources.hlsl
 *
 * A compute shader that initializes various resources to their defaults.
 *      - Counters for various entities
 *      - Default FSE tables and RLE entries
 *      - Per-frame sequence stream min index
 *
 * The caller needs to call Dispatch(zstdgpu_InitResources_GetDispatchSizeX, 1, 1)
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
    uint32_t allBlockCount;
    uint32_t cmpBlockCount;
    uint32_t frameCount;
    uint32_t initResourcesStage;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_INIT_RESOURCES_SRT()
#include "../zstdgpu_srt_decl_undef.h"

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=1), UAV(u0, numDescriptors=4)), RootConstants(b0, num32BitConstants=4)")]
[numthreads(kzstdgpu_TgSizeX_InitCounters, 1, 1)]
void main(uint i : SV_DispatchThreadId)
{
    zstdgpu_InitResources_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_INIT_RESOURCES_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    srt.allBlockCount           = Constants.allBlockCount;
    srt.cmpBlockCount           = Constants.cmpBlockCount;
    srt.frameCount              = Constants.frameCount;
    srt.initResourcesStage      = Constants.initResourcesStage;
    zstdgpu_ShaderEntry_InitResources(srt, i);
}
