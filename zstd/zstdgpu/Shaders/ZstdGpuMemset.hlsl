/**
 * ZstdGpuMemset.hlsl
 *
 * A generic compute shader that fills a buffer region with a constant uint32 value.
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
    uint32_t value;
};

ConstantBuffer<Consts> Constants : register(b0);

RWStructuredBuffer<uint32_t> ZstdBuffer : register(u0);

[RootSignature("UAV(u0), RootConstants(b0, num32BitConstants=3)")]
[numthreads(kzstdgpu_TgSizeX_Memset, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_Memset + threadId;
    if (i < Constants.workItemCount)
        ZstdBuffer[i] = Constants.value;
}
