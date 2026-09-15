/**
 * ZstdGpuDecompressSequences_SingleStream_ScalarFseLoad32.hlsl
 *
 * A variant of the compute shader decompressessing FSE-compressed sequences
 * without pre-caching FSE tables into LDS, reading directly from the buffer.
 * Targets hardware with maximal supported wave size of 32 lanes.
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

#define kzstdgpu_DecompressSequences_SingleStream_NoLdsFseCache 1
#define kzstdgpu_TgSizeX_DecompressSequences_SingleStream 32
#if 0
#define SEQ_CODE_INFO_USE_READLANE_UNIFORM_INDEX_WAVE32_PLUS 1
#else
#define SEQ_CODE_INFO_USE_LDS 1
#endif
#include "ZstdGpuDecompressSequences_SingleStream.hlsli"
