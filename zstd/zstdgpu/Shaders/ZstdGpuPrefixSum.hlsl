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
#include "../.generated/ZstdGpuSrt_PrefixSum.h"

[RootSignature(ZSTDGPU_SRT_RS_PrefixSum)]
[numthreads(kzstdgpu_TgSizeX_PrefixSum, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    zstdgpu_PrefixSum_SRT srt;

    zstdgpu_Srt_Fill(srt);

    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, srt.tgOffset) * kzstdgpu_TgSizeX_PrefixSum + threadId;
    if (i >= srt.workItemCount)
        return;

    const uint32_t count = srt.inoutInCountsOutPrefix[i];

    // NOTE(pamartis): can increase threadgroup size and do threadgroup-wide prefix sum to save memory
    // but we don't do this currently to increase parallelism
    const uint32_t countExclusiveBlockPrefix = WavePrefixSum(count);

    const uint32_t exclusivePrefixSum = zstdgpu_GlobalExclusivePrefixSum(srt.inoutInCountsOutPrefixLookback, countExclusiveBlockPrefix, count, i, kzstdgpu_TgSizeX_PrefixSum);

    srt.inoutInCountsOutPrefix[i] = exclusivePrefixSum + (srt.outputInclusive > 0 ? count : 0);
}
