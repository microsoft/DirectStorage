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
 * zstdgpu_lds_decl_size.h
 *
 * Defines ZSTDGPU_LDS_* macros that help to declare the total size of LDS partition defined by a macrolist
 * Include this file before invoking a macrolist defining LDS partition,
 * then include zstdgpu_lds_decl_undef.h afterwards to clean up.
 */

#define ZSTDGPU_LDS_SIZE(size) static const uint32_t kzstdgpu_##size##_LdsSize =
#define ZSTDGPU_LDS_BASE(base) (base)
#define ZSTDGPU_LDS_REGION(name, size) + (size)
