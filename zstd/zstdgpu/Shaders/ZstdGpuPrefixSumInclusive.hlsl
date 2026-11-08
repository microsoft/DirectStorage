/**
 * ZstdGpuPrefixSumInclusive.hlsl
 *
 * The `ZstdGpuPrefixSum.hlsl` variant with `outputInclusive` hardcoded instead of read through
 * the root signature. The variant binds with the PrefixSum SRT but has its own pipeline state,
 * root signature and command signature objects.
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

#define kzstdgpu_PrefixSum_OutputInclusive 1
#include "ZstdGpuPrefixSum.hlsl"
