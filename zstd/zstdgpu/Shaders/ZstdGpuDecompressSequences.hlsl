/**
 * ZstdGpuDecompressSequences.hlsl
 *
 * A compute shader that decompresses FSE-compressed Sequences.
 * The shader maps one stream of FSE-compressed sequences to a single thread.
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

#define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                  ZSTDGPU_RO_BUFFER(type)                    ZstdIn##name    : register(t##index);
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                  ZSTDGPU_RW_BUFFER(type)                    ZstdInOut##name : register(u##index);
#define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index) ZSTDGPU_RO_TYPED_BUFFER(hlsl_type, type)   ZstdIn##name    : register(t##index);
#define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index) ZSTDGPU_RW_TYPED_BUFFER(hlsl_type, type)   ZstdInOut##name : register(u##index);

ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()

#undef ZSTDGPU_RW_TYPED_BUFFER_DECL
#undef ZSTDGPU_RO_TYPED_BUFFER_DECL
#undef ZSTDGPU_RW_BUFFER_DECL
#undef ZSTDGPU_RO_BUFFER_DECL

#ifdef __XBOX_SCARLETT
#define __XBOX_ENABLE_WAVE32 1
#endif

#if kzstdgpu_TgSizeX_DecompressSequences == 1
#define USE_LDS_FSE_CACHE 1
#else
//#define USE_LDS_OUT_CACHE 1
#endif

#ifdef USE_LDS_FSE_CACHE
groupshared uint32_t Lds[kzstdgpu_FseElemMaxCount_LLen + kzstdgpu_FseElemMaxCount_MLen + kzstdgpu_FseElemMaxCount_Offs];
#define ZSTDGPU_LDS Lds
#include "../zstdgpu_lds_hlsl.h"
#endif

#ifdef USE_LDS_OUT_CACHE
#define SEQ_CACHE_LEN 128
groupshared uint32_t Lds[kzstdgpu_TgSizeX_DecompressSequences * (SEQ_CACHE_LEN + 1) * 3];
#define ZSTDGPU_LDS Lds
#include "../zstdgpu_lds_hlsl.h"
#endif

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=8), UAV(u0, numDescriptors=7))")]
#if defined(USE_LDS_FSE_CACHE) || defined(USE_LDS_OUT_CACHE)
#define NUM_THREADS 32
[numthreads(NUM_THREADS, 1, 1)]
void main(uint32_t groupId : SV_GroupId, uint i : SV_GroupThreadId)
#else
[numthreads(kzstdgpu_TgSizeX_DecompressSequences, 1, 1)]
void main(uint i : SV_DispatchThreadId)
#endif
{
    zstdgpu_DecompressSequences_SRT srt;

#define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                      srt.in##name    = ZstdIn##name;
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                      srt.inout##name = ZstdInOut##name;
#define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     srt.in##name    = ZstdIn##name;
#define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     srt.inout##name = ZstdInOut##name;
    ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()
#undef ZSTDGPU_RW_TYPED_BUFFER_DECL
#undef ZSTDGPU_RO_TYPED_BUFFER_DECL
#undef ZSTDGPU_RW_BUFFER_DECL
#undef ZSTDGPU_RO_BUFFER_DECL

#if defined(USE_LDS_FSE_CACHE)
    zstdgpu_ShaderEntry_DecompressSequences_LdsFseCache(srt, groupId, i, NUM_THREADS);
#elif defined(USE_LDS_OUT_CACHE)
    zstdgpu_ShaderEntry_DecompressSequences_LdsOutCache(srt, groupId, i);
#else
    zstdgpu_ShaderEntry_DecompressSequences(srt, i);
#endif
}
