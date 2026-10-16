/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 *
 * Advanced Technology Group (ATG)
 * Author(s):   Pavel Martishevsky (pamartis@microsoft.com)
 * 
 * This file contains definitions of functions accessing into Local Data Share (LDS), such as
 * and load/store and atomics function.
 *
 * It must be included after declaring `groupshared uint32_t YourLdsRegion[YourLdsRegionSize]`
 * and prior to including `#define ZSTDGPU_LDS YourLdsRegion` must be defined:
 *
 * ```
 * groupshared uint32_t MyLdsRegion[MyLdsSize]
 * #define ZSTDGPU_LDS MyLdsRegion
 * #include <zstdgpu_lds_hlsl.h>
 * ```
 *
 * This is largely workaround for HLSL not allowing to pass `groupshared` variables
 * into function as references and not allowing to declare `groupshared` variables within functions.
 */

#pragma once

#ifndef ZSTDGPU_LDS_HLSL_H
#define ZSTDGPU_LDS_HLSL_H

#ifndef __hlsl_dx_compiler
#   error Must be included only by HLSL compiler
#endif

#ifndef ZSTDGPU_LDS
#   error ZSTDGPU_LDS must be defined as `#define ZSTDGPU_LDS YourLdsRegion`
#endif

static uint32_t zstdgpu_LdsLoadU32(zstdgpu_lds_const_uintptr_t offsetInUInt32)
{
    return ZSTDGPU_LDS[offsetInUInt32];
}

static void zstdgpu_LdsStoreU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x)
{
    ZSTDGPU_LDS[offsetInUInt32] = x;
}

static void zstdgpu_LdsAtomicAddU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x)
{
    InterlockedAdd(ZSTDGPU_LDS[offsetInUInt32], x);
}

static void zstdgpu_LdsAtomicMaxU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x)
{
    InterlockedMax(ZSTDGPU_LDS[offsetInUInt32], x);
}

static void zstdgpu_LdsAtomicMinU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x)
{
    InterlockedMin(ZSTDGPU_LDS[offsetInUInt32], x);
}

static void zstdgpu_LdsAtomicAndU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x)
{
    InterlockedAnd(ZSTDGPU_LDS[offsetInUInt32], x);
}

static void zstdgpu_LdsAtomicOrU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x)
{
    InterlockedOr(ZSTDGPU_LDS[offsetInUInt32], x);
}

#endif /** ZSTDGPU_LDS_HLSL_H */
