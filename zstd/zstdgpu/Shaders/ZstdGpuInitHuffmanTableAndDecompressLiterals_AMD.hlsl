/**
 * AMD stream-density variant of fused Huffman table construction and literal decoding.
 *
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

#define ZSTDGPU_AMD_LITERAL_STREAM_DENSITY 1
#include "ZstdGpuInitHuffmanTableAndDecompressLiterals.hlsl"
