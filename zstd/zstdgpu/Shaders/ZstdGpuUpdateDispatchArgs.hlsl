/**
 * ZstdGpuUpdateDispatchArgs.hlsl
 *
 * A compute shader that reads source counters from the Counters and writes dispatch arguments
 * into the DispatchArgs buffer. The shader also updates derived counter fields in the Counters buffer.
 * The shader needs to be dispatched as single threadgroup.
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

#include "../zstdgpu_shaders.h"
#include "../.generated/ZstdGpuSrt_UpdateDispatchArgs.h"

[RootSignature(ZSTDGPU_SRT_RS_UpdateDispatchArgs)]
[numthreads(1, 1, 1)]
void main()
{
    zstdgpu_UpdateDispatchArgs_SRT srt;

    zstdgpu_Srt_Fill(srt);

    if (srt.stage == 0)
    {
        // Block-count dependent slots (valid after Stage 0 ParseFrames :: Count Blocks)
        const uint32_t cmpBlockCount = srt.inoutCounters[0].Blocks_CMP;
        const uint32_t rawBlockCount = srt.inoutCounters[0].Blocks_RAW;
        const uint32_t rleBlockCount = srt.inoutCounters[0].Blocks_RLE;
        const uint32_t allBlockCount = rawBlockCount
                                     + rleBlockCount
                                     + cmpBlockCount;


        // the arguments dependent on block counts/sizes -- these could be computed after ParseFrames
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_ComputePrefixSum,         cmpBlockCount,                            kzstdgpu_TgSizeX_PrefixSum_LiteralCount);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixBlockSizesAll,      allBlockCount,                            kzstdgpu_TgSizeX_PrefixSum);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixBlockSizesRLE,      rleBlockCount,                            kzstdgpu_TgSizeX_PrefixSum);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixBlockSizesRAW,      rawBlockCount,                            kzstdgpu_TgSizeX_PrefixSum);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_MemcpyRAW,                srt.inoutCounters[0].BlocksBytes_RAW,          kzstdgpu_TgSizeX_MemsetMemcpy);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_MemsetRLE,                srt.inoutCounters[0].BlocksBytes_RLE,          kzstdgpu_TgSizeX_MemsetMemcpy);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_ParseCompressedBlocks,    cmpBlockCount,                            kzstdgpu_TgSizeX_ParseCompressedBlocks);

        // Memset dispatch slots for InitResources Stage 1
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_RawBlockLookback,    zstdgpu_GetLookbackBlockCount(rawBlockCount),                    kzstdgpu_TgSizeX_Memset);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_RleBlockLookback,    zstdgpu_GetLookbackBlockCount(rleBlockCount),                    kzstdgpu_TgSizeX_Memset);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_CmpBlockLookback,    zstdgpu_GetLookbackBlockCount(cmpBlockCount),                    kzstdgpu_TgSizeX_Memset);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_AllBlockLookback,    zstdgpu_GetLookbackBlockCount(allBlockCount),                    kzstdgpu_TgSizeX_Memset);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_CmpBlockCount,       cmpBlockCount,                                                   kzstdgpu_TgSizeX_Memset);

        const uint32_t predicateMask = 0
                                     | (cmpBlockCount > srt.cmpBlockCountMax ? (1u << 0u) : 0u)
                                     | (rawBlockCount > srt.rawBlockCountMax ? (1u << 1u) : 0u)
                                     | (rleBlockCount > srt.rleBlockCountMax ? (1u << 2u) : 0u);

        srt.inoutPredicate[0] = predicateMask; // lower 32-bits of Stage 1 predicate
        srt.inoutPredicate[2] = predicateMask; // lower 32-bits of Stage 2 predicate
    }
    else if (srt.stage == 1)
    {
        const uint32_t litByteCount = srt.inoutCounters[0].HUF_Streams_DecodedBytes;
        const uint32_t seqElemCount = srt.inoutCounters[0].Seq_Streams_DecodedItems;

        // the arguments dependent on various streams counts that are part of compressed blocks -- these could be computed after ParseCompressedBlocks
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseHufW,                  srt.inoutCounters[0].FseHufW,                  1);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseLLen,                  srt.inoutCounters[0].FseLLen,                  1);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseOffs,                  srt.inoutCounters[0].FseOffs,                  1);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseMLen,                  srt.inoutCounters[0].FseMLen,                  1);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_HUF_WgtStreams,           srt.inoutCounters[0].HUF_WgtStreams,           1);

        // NOTE(pamartis): The number of groups running the decompression of Huffman weights depends on the number FSE tables
        // for Huffman weights because those numbers are the same because each FSE table decompresses its own Huffman weights' stream.
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecompressHuffmanWeights, srt.inoutCounters[0].FseHufW,                  kzstdgpu_TgSizeX_DecompressHuffmanWeights);

        // NOTE(pamartis): We also do decoding of uncompressed Huffman Weights stored as two nibbles per byte to make sure final representation
        // (a byte per weight) becomes identical, so identical representation simplfies initialisation of Huffman tables to use during literal decoding
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecodeHuffmanWeights,     srt.inoutCounters[0].HUF_WgtStreams,           kzstdgpu_TgSizeX_DecodeHuffmanWeights);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecompressSequences,      srt.inoutCounters[0].Seq_Streams,              srt.decompressSequences_StreamsPerTG);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FinaliseSequenceOffsets,  srt.inoutCounters[0].Seq_Streams_DecodedItems, kzstdgpu_TgSizeX_FinaliseSequenceOffsets);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixSequenceOffsets,    srt.inoutCounters[0].Seq_Streams,              kzstdgpu_TgSizeX_PrefixSequenceOffsets);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PropagateFseIndex,        srt.inoutCounters[0].Seq_Streams,              kzstdgpu_TgSizeX_PropagateFseIndex);

        const uint32_t predicateMask = 0
                                     | (litByteCount > srt.litByteCountMax ? (1u << 3u) : 0u)
                                     | (seqElemCount > srt.seqElemCountMax ? (1u << 4u) : 0u);

        srt.inoutPredicate[2] = srt.inoutPredicate[2] | predicateMask; // lower 32-bits of Stage 2 predicate
    }
    else
    {
        // The number of Groups required for `DecompressLiterals` is only calculated after `ComputePrefixSum`
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecompressLiterals,       srt.inoutCounters[0].DecompressLiteralsGroups, 1);
    }
}
