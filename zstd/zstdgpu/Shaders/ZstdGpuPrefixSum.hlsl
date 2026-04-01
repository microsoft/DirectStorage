/**
 * ZstdGpuPrefixSum.hlsl
 *
 * A compute shader producing a prefix sum of unsigned value with total sum fitting 30 bits.
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
    uint32_t outputInclusive;
};

ConstantBuffer<Consts>          Constants                           : register(b0);

RWStructuredBuffer<uint32_t>    ZstdInCountsOutPrefix               : register(u0);

globallycoherent
RWStructuredBuffer<uint32_t>    ZstdInCountsOutPrefixLookback       : register(u1);


[RootSignature("UAV(u0), UAV(u1), RootConstants(b0, num32BitConstants=3)")]
[numthreads(kzstdgpu_TgSizeX_PrefixSum, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, Constants.tgOffset) * kzstdgpu_TgSizeX_PrefixSum + threadId;
    if (i >= Constants.workItemCount)
        return;

    const uint32_t count = ZstdInCountsOutPrefix[i];

    // NOTE(pamartis): can increase threadgroup size and do threadgroup-wide prefix sum to save memory
    // but we don't do this currently to increase parallelism
    const uint32_t countExclusiveBlockPrefix = WavePrefixSum(count);

    const uint32_t exclusivePrefixSum = zstdgpu_GlobalExclusivePrefixSum(ZstdInCountsOutPrefixLookback, countExclusiveBlockPrefix, count, i, kzstdgpu_TgSizeX_PrefixSum);

    ZstdInCountsOutPrefix[i] = exclusivePrefixSum + (Constants.outputInclusive > 0 ? count : 0);
}
