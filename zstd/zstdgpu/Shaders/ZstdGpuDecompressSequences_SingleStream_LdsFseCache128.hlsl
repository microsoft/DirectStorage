/**
 * ZstdGpuDecompressSequences_SingleStream_LdsFseCache128.hlsl
 *
 * A compute shader that decompresses single FSE-compressed sequences stream per TG by preloading
 * FSE tables into LDS and then sampling FSE tables from LDS for faster access comparing to L0/Scalar cache.
 *
 * The variant targeting hardware with maximal supported wave size of 128 lanes.
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

#define kzstdgpu_DecompressSequences_SingleStream_NoLdsFseCache 0
#define kzstdgpu_TgSizeX_DecompressSequences_SingleStream 128
#include "ZstdGpuDecompressSequences_SingleStream.hlsli"
