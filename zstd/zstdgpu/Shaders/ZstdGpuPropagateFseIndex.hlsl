/**
 * ZstdGpuPropagateFseIndex.hlsl
 *
 * A compute shader that propagates a single FSE table index family
 * (LLen, Offs or MLen) across sequence streams via "Decoupled Lookback".
 *
 * Each invocation maps one sequence stream to a single thread. The shader
 * reads the un-propagated value from the bound `FseIds` buffer (indexed
 * directly by sequence stream index, since `SeqStreamTo*FseId` is compacted),
 * runs `zstdgpu_PropagateFseTableIndex` with the bound `Lookback` buffer,
 * and writes the propagated value back.
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
#include "../.generated/ZstdGpuSrt_PropagateFseIndex.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature(ZSTDGPU_SRT_RS_PropagateFseIndex)]
[numthreads(kzstdgpu_TgSizeX_PropagateFseIndex, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint threadId : SV_GroupThreadId)
{
    zstdgpu_PropagateFseIndex_SRT srt;

    zstdgpu_Srt_Fill(srt);

    const uint32_t i = zstdgpu_ConvertTo32BitGroupId(groupId, srt.tgOffset) * kzstdgpu_TgSizeX_PropagateFseIndex + threadId;

    if (i >= srt.workItemCount)
    {
        return;
    }
    srt.inoutFseIds[i] = zstdgpu_PropagateFseTableIndex(srt.inoutFseIndexLookback, srt.inoutFseIds[i], i);
}
