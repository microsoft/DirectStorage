/**
 * ZstdGpuInitFseTable.hlsl
 *
 * A compute shader that initializes FSE table given FSE probability distribution.
 * The shader initializes single FSE table per threadgroup.
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

#include "../zstdgpu_structs.h"

#define ZSTD_BITCNT_NSTATE_METHOD_REFERENCE 0
#define ZSTD_BITCNT_NSTATE_METHOD_DEFAULT 1
#define ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL 2  // doesn't work on XBOX for now
#define ZSTD_BITCNT_NSTATE_METHOD ZSTD_BITCNT_NSTATE_METHOD_DEFAULT

#if defined(__XBOX_SCARLETT)
 //#   define __XBOX_ENABLE_WAVE32 1
#   if !defined(__XBOX_ENABLE_WAVE32) || (__XBOX_ENABLE_WAVE32 == 0)
#       undef kzstdgpu_WaveSize_Min
#       define kzstdgpu_WaveSize_Min 64
#   endif
#endif

#define kzstdgpu_WaveCountMax_InitFseTable (kzstdgpu_TgSizeX_InitFseTable / kzstdgpu_WaveSize_Min)

#define PREFER_LDS 0

#if kzstdgpu_WaveCountMax_InitFseTable > 1 || PREFER_LDS == 1
#   define IS_MULTI_WAVE 1
#else
#   define IS_MULTI_WAVE 0
#endif

#include "../zstdgpu_shaders.h"

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
    uint32_t tableStartIndex;
    uint32_t tableDataStart;
    uint32_t tableDataCount;
};

ConstantBuffer<Consts> Constants : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_INIT_FSE_TABLE_SRT()
#include "../zstdgpu_srt_decl_undef.h"

#if ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_DEFAULT
groupshared uint32_t Lds[kzstdgpu_InitFseTable_Default_LdsSize];
#elif ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL
groupshared uint32_t Lds[kzstdgpu_InitFseTable_Experimental_LdsSize];
#endif

#define ZSTDGPU_LDS Lds
#include "../zstdgpu_lds_hlsl.h"

[RootSignature("DescriptorTable(SRV(t0, numDescriptors = 2), UAV(u0, numDescriptors=1)), RootConstants(b0, num32BitConstants=5)")]
[numthreads(kzstdgpu_TgSizeX_InitFseTable, 1, 1)]
void main(uint2 groupId2 : SV_GroupId, uint32_t i : SV_GroupThreadId)
{
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset);

    zstdgpu_InitFseTable_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_INIT_FSE_TABLE_SRT()
    #include "../zstdgpu_srt_decl_undef.h"
    srt.tableStartIndex = Constants.tableStartIndex;
    srt.tableDataStart  = Constants.tableDataStart;
    srt.tableDataCount  = Constants.tableDataCount;
    zstdgpu_ShaderEntry_InitFseTable(srt, groupId, i);
}
