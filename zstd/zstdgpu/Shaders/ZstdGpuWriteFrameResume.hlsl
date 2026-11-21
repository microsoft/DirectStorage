/**
 * ZstdGpuWriteFrameResume.hlsl
 *
 * Writes a partial decode's resume record for the request's last frame: where the next request
 * continues it, the repeat offsets on entry there and the blocks whose entropy tables it may reuse.
 *
 * One thread does the work (see zstdgpu_ShaderEntry_WriteFrameResume).
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
