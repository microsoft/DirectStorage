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

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_EXECUTE_SEQUENCES_SRT()
#include "../zstdgpu_srt_decl_undef.h"

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=12), UAV(u0, numDescriptors=1))")]
[numthreads(MAX_COPY_SIZE, 1, 1)]
void main(uint groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    // Retire redundant waves (just recomputing the same result) when the GPU wave is narrower than the group.
    if (i >= WaveGetLaneCount())
        return;

    zstdgpu_ExecuteSequences_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_EXECUTE_SEQUENCES_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    zstdgpu_ShaderEntry_ExecuteSequences(srt, groupId);
}
