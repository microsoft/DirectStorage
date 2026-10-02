/**
 * ZstdGpuDecompressSequences_MultiStream_4_LdsOutCache_32_FseElems.hlsl
 *
 * The `ZstdGpuDecompressSequences_MultiStream_4_LdsOutCache_32.hlsl` variant that reads the LL/OF/ML decode
 * tables [Init FSE Tables] persisted in FseElems instead of building them in the FSE arena. It decodes the
 * batches whose compressed blocks fit the FSE arena capacity: their persisted tables take no more memory
 * than an arena sized to the batch, and skipping the in-kernel table build decodes them faster.
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

#define kzstdgpu_DecompressSequences_FseArena 0
#define kzstdgpu_DecompressSequences_ThreadsPerStream 4
#define kzstdgpu_DecompressSequences_LdsStoreCache_DwCount 32
#include "ZstdGpuDecompressSequences_MultiStream_LdsOutCache.hlsli"
