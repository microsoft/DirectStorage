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

#define NUM_THREADS 32

#include "../zstdgpu_shaders.h"

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()
#include "../zstdgpu_srt_decl_undef.h"

groupshared uint32_t Lds[kzstdgpu_DecompressSequences_MultiStream_LdsOutCache_LdsSize];
#define ZSTDGPU_LDS Lds
#include "../zstdgpu_lds_hlsl.h"

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=6), UAV(u0, numDescriptors=7)), RootConstants(b0, num32BitConstants=2)")]
[numthreads(NUM_THREADS, 1, 1)]
void main(uint32_t2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset);
    zstdgpu_DecompressSequences_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    zstdgpu_ShaderEntry_DecompressSequences_MultiStream_LdsOutCache(srt, groupId, i, NUM_THREADS, kzstdgpu_DecompressSequences_StreamsPerTG, kzstdgpu_DecompressSequences_LdsStoreCache_DwCount);
}
