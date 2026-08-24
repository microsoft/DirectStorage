/**
 * ZstdGpuFinaliseSequenceOffsets.hlsl
 *
 * A compute shader that converts sequence offsets from encoded representation
 * which includes "repeat" offsets into absolute offsets using information from
 * previous blocks such as final "repeat" offset and the prefix of block sizes
 *
 * The shader maps each sequence offset to a thread.
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

#include "../.generated/ZstdGpuSrt_FinaliseSequenceOffsets.h"

[RootSignature(ZSTDGPU_SRT_RS_FinaliseSequenceOffsets)]
[numthreads(kzstdgpu_TgSizeX_FinaliseSequenceOffsets, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_FinaliseSequenceOffsets_SRT srt;
    zstdgpu_Srt_Fill(srt);

    i += zstdgpu_ConvertTo32BitGroupId(groupId, srt.tgOffset) * kzstdgpu_TgSizeX_FinaliseSequenceOffsets;

    zstdgpu_ShaderEntry_FinaliseSequenceOffsets(srt, i);
}
