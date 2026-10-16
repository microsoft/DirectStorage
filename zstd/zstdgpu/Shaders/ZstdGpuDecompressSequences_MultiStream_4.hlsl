/**
 * ZstdGpuDecompressSequences_MultiStream_4.hlsl
 *
 * A compute shader that decompresses FSE-compressed Sequences.
 * The shader maps one stream of FSE-compressed sequences to a single thread.
 *
 * Shader variant processing 4 streams per TG:
 *  - only 4 threads are active to reduce the cost of scattered read/writes and
 *    average TG lifetime due to sequence count divergence)
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

#define kzstdgpu_DecompressSequences_StreamsPerTG 4
#include "ZstdGpuDecompressSequences_MultiStream.hlsli"
