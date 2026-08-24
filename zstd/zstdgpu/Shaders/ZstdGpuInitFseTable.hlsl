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


#include "../.generated/ZstdGpuSrt_InitFseTable.h"

#if ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_DEFAULT
groupshared uint32_t Lds[kzstdgpu_InitFseTable_Default_LdsSize];
#elif ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL
groupshared uint32_t Lds[kzstdgpu_InitFseTable_Experimental_LdsSize];
#endif

#define ZSTDGPU_LDS Lds
#include "../zstdgpu_lds_hlsl.h"

[RootSignature(ZSTDGPU_SRT_RS_InitFseTable)]
[numthreads(kzstdgpu_TgSizeX_InitFseTable, 1, 1)]
void main(uint2 groupId2 : SV_GroupId, uint32_t i : SV_GroupThreadId)
{
    zstdgpu_InitFseTable_SRT srt;

    zstdgpu_Srt_Fill(srt);

    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, srt.tgOffset);

    const uint32_t cmpBlockCount = srt.inCounters[0].Blocks_CMP;
    if (srt.tableType == 1u) // LLen
    {
        zstdgpu_Srt_FillInline(srt, cmpBlockCount, zstdgpu_ComputeFseDataStartLLen(0, cmpBlockCount), kzstdgpu_FseElemMaxCount_LLen);
    }
    else if (srt.tableType == 2u) // Offs
    {
        zstdgpu_Srt_FillInline(srt, 2u * cmpBlockCount + 1u, zstdgpu_ComputeFseDataStartOffs(0, cmpBlockCount), kzstdgpu_FseElemMaxCount_Offs);
    }
    else if (srt.tableType == 3u) // MLen
    {
        zstdgpu_Srt_FillInline(srt, 3u * cmpBlockCount + 2u, zstdgpu_ComputeFseDataStartMLen(0, cmpBlockCount), kzstdgpu_FseElemMaxCount_MLen);
    }
    else // 0 == HufW
    {
        zstdgpu_Srt_FillInline(srt, 0u, zstdgpu_ComputeFseDataStartHufW(0, cmpBlockCount), kzstdgpu_FseElemMaxCount_HufW);
    }
    zstdgpu_ShaderEntry_InitFseTable(srt, groupId, i);
}
