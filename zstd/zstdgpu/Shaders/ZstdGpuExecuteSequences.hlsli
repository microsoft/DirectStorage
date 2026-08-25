/**
 * ZstdGpuExecuteSequences.hlsl
 *
 * A compute shader that executes sequences.
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

#include "../.generated/ZstdGpuSrt_ExecuteSequences.h"

[RootSignature(ZSTDGPU_SRT_RS_ExecuteSequences)]
[numthreads(MAX_COPY_SIZE, 1, 1)]
void main(uint groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    // Retire redundant waves (just recomputing the same result) when the GPU wave is narrower than the group.
    if (i >= WaveGetLaneCount())
        return;

    zstdgpu_ExecuteSequences_SRT srt;
    zstdgpu_Srt_Fill(srt);

    zstdgpu_ShaderEntry_ExecuteSequences(srt, groupId);
}
