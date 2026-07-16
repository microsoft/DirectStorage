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

RWStructuredBuffer<zstdgpu_Counters>  ZstdCounters     : register(u0);
RWStructuredBuffer<uint32_t>          ZstdDispatchArgs : register(u1);
RWStructuredBuffer<uint32_t>          ZstdDispatchCnts : register(u2);
RWStructuredBuffer<uint32_t>          ZstdPredicate    : register(u3);

struct UpdateDispatchArgsConsts
{
    uint32_t decompressSequences_StreamsPerTG;
    uint32_t stage;

    uint32_t cmpBlockCountMax;
    uint32_t rawBlockCountMax;
    uint32_t rleBlockCountMax;
    uint32_t litByteCountMax;
    uint32_t seqElemCountMax;
};

ConstantBuffer<UpdateDispatchArgsConsts> Consts : register(b0);

[RootSignature("UAV(u0), UAV(u1), UAV(u2), UAV(u3), RootConstants(b0, num32BitConstants=7)")]
[numthreads(1, 1, 1)]
void main()
{
    if (Consts.stage == 0)
    {
        // Block-count dependent slots (valid after Stage 0 ParseFrames :: Count Blocks)
        const uint32_t cmpBlockCount = ZstdCounters[0].Blocks_CMP;
        const uint32_t rawBlockCount = ZstdCounters[0].Blocks_RAW;
        const uint32_t rleBlockCount = ZstdCounters[0].Blocks_RLE;
        const uint32_t allBlockCount = rawBlockCount
                                     + rleBlockCount
                                     + cmpBlockCount;


        // the arguments dependent on block counts/sizes -- these could be computed after ParseFrames
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_ComputePrefixSum,         cmpBlockCount,                            kzstdgpu_TgSizeX_PrefixSum_LiteralCount);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_PrefixBlockSizes,         allBlockCount,                            kzstdgpu_TgSizeX_PrefixSum);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_MemcpyRAW,                ZstdCounters[0].BlocksBytes_RAW,          kzstdgpu_TgSizeX_MemsetMemcpy);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_MemsetRLE,                ZstdCounters[0].BlocksBytes_RLE,          kzstdgpu_TgSizeX_MemsetMemcpy);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_ParseCompressedBlocks,    cmpBlockCount,                            kzstdgpu_TgSizeX_ParseCompressedBlocks);

        // Memset dispatch slots for InitResources Stage 1
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_Memset_CmpBlockLookback,    zstdgpu_GetLookbackBlockCount(cmpBlockCount),                    kzstdgpu_TgSizeX_Memset);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_Memset_AllBlockLookback,    zstdgpu_GetLookbackBlockCount(allBlockCount),                    kzstdgpu_TgSizeX_Memset);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_Memset_CmpBlockCount,       cmpBlockCount,                                                   kzstdgpu_TgSizeX_Memset);

        const uint32_t predicateMask = 0
                                     | (cmpBlockCount > Consts.cmpBlockCountMax ? (1u << 0u) : 0u)
                                     | (rawBlockCount > Consts.rawBlockCountMax ? (1u << 1u) : 0u)
                                     | (rleBlockCount > Consts.rleBlockCountMax ? (1u << 2u) : 0u);

        ZstdPredicate[0] = predicateMask; // lower 32-bits of Stage 1 predicate
        ZstdPredicate[2] = predicateMask; // lower 32-bits of Stage 2 predicate
    }
    else if (Consts.stage == 1)
    {
        const uint32_t litByteCount = ZstdCounters[0].HUF_Streams_DecodedBytes;
        const uint32_t seqElemCount = ZstdCounters[0].Seq_Streams_DecodedItems;

        // the arguments dependent on various streams counts that are part of compressed blocks -- these could be computed after ParseCompressedBlocks
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_FseHufW,                  ZstdCounters[0].FseHufW,                  1);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_FseLLen,                  ZstdCounters[0].FseLLen,                  1);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_FseOffs,                  ZstdCounters[0].FseOffs,                  1);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_FseMLen,                  ZstdCounters[0].FseMLen,                  1);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_HUF_WgtStreams,           ZstdCounters[0].HUF_WgtStreams,           1);

        // NOTE(pamartis): The number of groups running the decompression of Huffman weights depends on the number FSE tables
        // for Huffman weights because those numbers are the same because each FSE table decompresses its own Huffman weights' stream.
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_DecompressHuffmanWeights, ZstdCounters[0].FseHufW,                  kzstdgpu_TgSizeX_DecompressHuffmanWeights);

        // NOTE(pamartis): We also do decoding of uncompressed Huffman Weights stored as two nibbles per byte to make sure final representation
        // (a byte per weight) becomes identical, so identical representation simplfies initialisation of Huffman tables to use during literal decoding
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_DecodeHuffmanWeights,     ZstdCounters[0].HUF_WgtStreams,           kzstdgpu_TgSizeX_DecodeHuffmanWeights);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_DecompressSequences,      ZstdCounters[0].Seq_Streams,              Consts.decompressSequences_StreamsPerTG);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_FinaliseSequenceOffsets,  ZstdCounters[0].Seq_Streams_DecodedItems, kzstdgpu_TgSizeX_FinaliseSequenceOffsets);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_PrefixSequenceOffsets,    ZstdCounters[0].Seq_Streams,              kzstdgpu_TgSizeX_PrefixSequenceOffsets);
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_PropagateFseIndex,        ZstdCounters[0].Seq_Streams,              kzstdgpu_TgSizeX_PropagateFseIndex);

        const uint32_t predicateMask = 0
                                     | (litByteCount > Consts.litByteCountMax ? (1u << 3u) : 0u)
                                     | (seqElemCount > Consts.seqElemCountMax ? (1u << 4u) : 0u);

        ZstdPredicate[2] = ZstdPredicate[2] | predicateMask; // lower 32-bits of Stage 2 predicate
    }
    else
    {
        // The number of Groups required for `DecompressLiterals` is only calculated after `ComputePrefixSum`
        zstdgpu_EmitDispatch(ZstdDispatchArgs, ZstdDispatchCnts, kzstdgpu_DispatchSlot_DecompressLiterals,       ZstdCounters[0].DecompressLiteralsGroups, 1);
    }
}
