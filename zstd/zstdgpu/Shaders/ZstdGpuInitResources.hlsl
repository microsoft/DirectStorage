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

#include "../.generated/ZstdGpuSrt_InitResources.h"

[RootSignature(ZSTDGPU_SRT_RS_InitResources)]
[numthreads(kzstdgpu_TgSizeX_InitCounters, 1, 1)]
void main(uint i : SV_DispatchThreadId)
{
    zstdgpu_InitResources_SRT srt;
    zstdgpu_Srt_Fill(srt);

    zstdgpu_ShaderEntry_InitResources(srt, i);
}
