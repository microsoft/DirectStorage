/**
 * ZstdGpuDecompressSequences_MultiStream_LdsOutCache.hlsli
 *
 * A compute shader that decompresses multiple FSE-compressed sequences streams per TG by sampling
 * FSE tables from L0, but limiting the number of sequences streams per TG to avoid L0 cache thrashing
 * by FSE table sampling:
 *  - 3 FSE tables for LLen, MLen, Offs require 1280 dwords (5KB), so assuming the data where each
 *    sequences stream require a unique triple of tables of maximal size (non-RLE) 64KB L0 cache
 *    can fit tables for 12 streams.
 *
 * also, it stores decoded sequences into LDS first to avoid scattered writes, accumulates up to N
 * dwords for each of M streams, flushes them to memory by storing N sequential dwords per each of M
 * streams to help hardware coalescing.
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

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

#ifndef kzstdgpu_DecompressSequences_LdsStoreCache_DwCount
#   error 'kzstdgpu_DecompressSequences_LdsStoreCache_DwCount' must be defined before including this '.hlsli'
#endif

#ifndef kzstdgpu_DecompressSequences_ThreadsPerStream
#   error 'kzstdgpu_DecompressSequences_ThreadsPerStream' must be defined before including this '.hlsli'
#endif

#if kzstdgpu_DecompressSequences_ThreadsPerStream < 1 || kzstdgpu_DecompressSequences_ThreadsPerStream > 8
#   error 'kzstdgpu_DecompressSequences_ThreadsPerStream' must be in range [1, 8]
#endif

#if (kzstdgpu_DecompressSequences_ThreadsPerStream & (kzstdgpu_DecompressSequences_ThreadsPerStream - 1)) != 0
#   error 'kzstdgpu_DecompressSequences_ThreadsPerStream' must be a power of 2
#endif

#include "../zstdgpu_shaders.h"

#include "../.generated/ZstdGpuSrt_DecompressSequences.h"

groupshared uint32_t Lds[kzstdgpu_DecompressSequences_MultiStream_LdsOutCache_LdsSize];
#define ZSTDGPU_LDS Lds
#include "../zstdgpu_lds_hlsl.h"

[RootSignature(ZSTDGPU_SRT_RS_DecompressSequences)]
[numthreads(kzstdgpu_TgSizeX_DecompressSequences, 1, 1)]
void main(uint32_t2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    zstdgpu_DecompressSequences_SRT srt;
    zstdgpu_Srt_Fill(srt);
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, srt.tgOffset);

    zstdgpu_ShaderEntry_DecompressSequences_MultiStream_LdsOutCache(
        srt,
        groupId,
        i,
        kzstdgpu_TgSizeX_DecompressSequences,
        kzstdgpu_TgSizeX_DecompressSequences / kzstdgpu_DecompressSequences_ThreadsPerStream,
        kzstdgpu_DecompressSequences_LdsStoreCache_DwCount);
}
