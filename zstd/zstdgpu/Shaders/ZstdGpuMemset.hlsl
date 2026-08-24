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
#include "../.generated/ZstdGpuSrt_Memset.h"

[RootSignature(ZSTDGPU_SRT_RS_Memset)]
[numthreads(kzstdgpu_TgSizeX_Memset, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    zstdgpu_Memset_SRT srt;

    zstdgpu_Srt_Fill(srt);

    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, srt.tgOffset) * kzstdgpu_TgSizeX_Memset + threadId;
    if (i < srt.workItemCount)
    {
        srt.inoutDest[i] = srt.value;
    }
}
