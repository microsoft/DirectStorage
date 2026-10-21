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
 * This header contains declaration of functions accessing into Local Data Share (LDS), such as
 * and load/store and atomics function, and macro to declare LDS space in a shader C++/HLSL function.
 *
 * This file aims to aid portability between C++/HLSL
 */

#pragma once

#ifndef ZSTDGPU_LDS_H
#define ZSTDGPU_LDS_H

#ifdef __hlsl_dx_compiler

// For HLSL this file contains only definitions that don't contain references to the LDS because
// HLSL doesn't support passing `groupshared` variables into functions

#pragma dxc push
#pragma dxc diagnostic ignored "-Wundefined-internal"
static uint32_t zstdgpu_LdsLoadU32(zstdgpu_lds_const_uintptr_t offsetInUInt32);
static void zstdgpu_LdsStoreU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x);
static void zstdgpu_LdsAtomicAddU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x);
static void zstdgpu_LdsAtomicMaxU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x);
static void zstdgpu_LdsAtomicMinU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x);
static void zstdgpu_LdsAtomicAndU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x);
static void zstdgpu_LdsAtomicOrU32(zstdgpu_lds_uintptr_t offsetInUInt32, uint32_t x);
#pragma dxc pop

#else

static inline uint32_t zstdgpu_LdsLoadU32(zstdgpu_lds_const_uintptr_t address)
{
    return *address;
}

static inline void zstdgpu_LdsStoreU32(zstdgpu_lds_uintptr_t address, uint32_t x)
{
    *address = x;
}

static inline void zstdgpu_LdsAtomicAddU32(zstdgpu_lds_uintptr_t address, uint32_t x)
{
    *address += x;
}

static inline void zstdgpu_LdsAtomicMaxU32(zstdgpu_lds_uintptr_t address, uint32_t x)
{
    *address = zstdgpu_MaxU32(*address, x);
}

static inline void zstdgpu_LdsAtomicMinU32(zstdgpu_lds_uintptr_t address, uint32_t x)
{
    *address = zstdgpu_MinU32(*address, x);
}

static inline void zstdgpu_LdsAtomicAndU32(zstdgpu_lds_uintptr_t address, uint32_t x)
{
    *address &= x;
}

static inline void zstdgpu_LdsAtomicOrU32(zstdgpu_lds_uintptr_t address, uint32_t x)
{
    *address |= x;
}

#endif /** #ifdef __hlsl_dx_compiler */

#endif /** ZSTDGPU_LDS_H */
