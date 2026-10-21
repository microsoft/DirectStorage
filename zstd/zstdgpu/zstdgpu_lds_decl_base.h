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
 * zstdgpu_lds_decl_base.h
 *
 * Defines ZSTDGPU_LDS_* macros that help to declare the base of LDS partition defined by a macrolist
 * and the base for all subregions.
 * Include this file before invoking a macrolist defining LDS partition,
 * then include zstdgpu_lds_decl_undef.h afterwards to clean up.
 */

#ifndef __hlsl_dx_compiler
#define ZSTDGPU_LDS_SIZE(size)          uint32_t GS_Storage[kzstdgpu_##size##_LdsSize];
#define ZSTDGPU_LDS_BASE(base)          zstdgpu_lds_uintptr_t GS_Base = (0 != (base)) ? (base) : &GS_Storage[0];
#else
#define ZSTDGPU_LDS_SIZE(size)
#define ZSTDGPU_LDS_BASE(base)          zstdgpu_lds_uintptr_t GS_Base = base;
#endif

#define ZSTDGPU_LDS_REGION(name, size)  zstdgpu_lds_uintptr_t GS_##name = GS_Base;  \
                                        GS_Base += (size);

