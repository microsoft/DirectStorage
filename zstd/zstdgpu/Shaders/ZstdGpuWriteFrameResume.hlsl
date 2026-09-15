/**
 * ZstdGpuWriteFrameResume.hlsl
 *
 * Publishes the per-frame state a subsequent intra-frame slice needs in order to continue:
 * the output cursor and the repeat offsets left by the frame's last sequence stream.
 *
 * The shader maps each frame to a thread.
 *
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 *
 * Advanced Technology Group (ATG)
 */

#include "../zstdgpu_shaders.h"

#include "../srt_headers/ZstdGpuSrt_WriteFrameResume.h"

[RootSignature(ZSTDGPU_SRT_RS_WriteFrameResume)]
[numthreads(kzstdgpu_TgSizeX_WriteFrameResume, 1, 1)]
void main(uint2 groupId : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_WriteFrameResume_SRT srt;
    zstdgpu_Srt_Fill(srt);

    i += zstdgpu_ConvertTo32BitGroupId(groupId, 0) * kzstdgpu_TgSizeX_WriteFrameResume;

    zstdgpu_ShaderEntry_WriteFrameResume(srt, i);
}
