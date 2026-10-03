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
 * Contains zstdgpu-specific assert macro definition
 */

#pragma once

#ifndef ZSTDGPU_ASSERT_H
#define ZSTDGPU_ASSERT_H

#define ZSTDGPU_ASSERT_BACKEND_NONE 0
#define ZSTDGPU_ASSERT_BACKEND_TTA  1

#ifndef ZSTDGPU_ASSERT_BACKEND
    /*
     *  TTA remains the default for existing zstdgpu builds. Consumers that
     *  don't require host assertions can define ZSTDGPU_ASSERT_BACKEND as
     *  ZSTDGPU_ASSERT_BACKEND_NONE for every zstdgpu translation unit.
     */
#   define ZSTDGPU_ASSERT_BACKEND ZSTDGPU_ASSERT_BACKEND_TTA
#endif

#ifndef ZSTDGPU_ASSERT
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_ASSERT(cond)
#       define ZSTDGPU_ASSERT_MSG(cond, msg, ...)
#   elif ZSTDGPU_ASSERT_BACKEND == ZSTDGPU_ASSERT_BACKEND_NONE
#       define ZSTDGPU_ASSERT(cond) ((void)sizeof((cond) != 0))
#       define ZSTDGPU_ASSERT_MSG(cond, msg, ...) ((void)sizeof((cond) != 0))
#   elif ZSTDGPU_ASSERT_BACKEND == ZSTDGPU_ASSERT_BACKEND_TTA
#       include "zstdgpu_assert_tta.h"
#   else
#       error Unknown ZSTDGPU_ASSERT_BACKEND.
#   endif
#endif

#ifndef ZSTDGPU_BREAK
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_BREAK()
#   else
#       define ZSTDGPU_BREAK() ZSTDGPU_ASSERT(0)
#   endif
#endif

#endif /* #define ZSTDGPU_ASSERT_H */
