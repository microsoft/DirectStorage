/**
 * ZstdGpuDecompressSequences_MultiStream_4_LdsOutCache_32.hlsl
 *
 * A compute shader that decompresses multiple FSE-compressed sequences streams per TG by sampling
 * FSE tables from L0, but limiting the number of sequences streams per TG to avoid L0 cache thrashing
 * by FSE table sampling:
 *  - 3 FSE tables for LLen, MLen, Offs require 1280 dwords (5KB), so assuming the data where each
 *    sequences stream require a unique triple of tables of maximal size (non-RLE) 64KB L0 cache
 *    can fit tables for 12 streams.
 *
 * also, it stores decoded sequences into LDS first to avoid scattered writes, accumulates up to N
 * dwords for each of M streams, flushes them to memory by storing N sequential dwords per each of M
 * streams to help hardware coalescing.
 *
 * The variant accumulating M=32 sequences per sequences stream and processing N=`TgSizeX_DecompressSequences / 4` sequences streams
 * per TG of `TgSizeX_DecompressSequences` threads (some threads are inactive during decoding, and become active during memory writes).
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

#define kzstdgpu_DecompressSequences_ThreadsPerStream 4
#define kzstdgpu_DecompressSequences_LdsStoreCache_DwCount 32
#include "ZstdGpuDecompressSequences_MultiStream_LdsOutCache.hlsli"
