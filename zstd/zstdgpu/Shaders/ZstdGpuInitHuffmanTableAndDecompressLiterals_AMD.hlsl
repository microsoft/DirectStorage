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

#if defined(_GAMING_XBOX) || defined(_GAMING_XBOX_SCARLETT) || defined(_GAMING_XBOX_XBOXONE) \
    || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)

// On consoles the AMD stream-density variant would be byte-identical to the default kernel
// (kzstdgpu_StreamsPerGroup_DecompressLiterals_AMD == kzstdgpu_TgSizeX_DecompressLiterals there),
// so build a minimal placeholder instead of a redundant copy. The C++ runtime does not reference
// this shader on consoles (see ZSTDGPU_ENABLE_AMD_LITERAL_VARIANT).
[numthreads(1, 1, 1)]
void main()
{
}

#else

#define ZSTDGPU_AMD_LITERAL_STREAM_DENSITY 1
#include "ZstdGpuInitHuffmanTableAndDecompressLiterals.hlsl"

#endif
