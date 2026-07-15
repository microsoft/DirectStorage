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
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

struct ID3D12Device *zstdgpu_Demo_PlatformInit(uint32_t gpuVenId, uint32_t gpuDevId, bool d3dDbg, bool d3dGbv);

void zstdgpu_Demo_PlatformTerm(struct ID3D12Device *device);

uint32_t zstdgpu_Demo_PlatformTick(void);

#ifdef __cplusplus
}
#endif
