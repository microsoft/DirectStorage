/**
 * ZstdGpuParseFrames.hlsl
 *
 * A compute shader that parses Zstd frames.
 * The shader maps one frame to a single thread.
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
#include "../.generated/ZstdGpuSrt_ParseFrames.h"

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

[RootSignature(ZSTDGPU_SRT_RS_ParseFrames)]
[numthreads(kzstdgpu_TgSizeX_ParseCompressedBlocks, 1, 1)]
void main(uint i : SV_DispatchThreadId)
{
    zstdgpu_ParseFrames_SRT srt;

    zstdgpu_Srt_Fill(srt);

    zstdgpu_ShaderEntry_ParseFrames(srt, i);
}
