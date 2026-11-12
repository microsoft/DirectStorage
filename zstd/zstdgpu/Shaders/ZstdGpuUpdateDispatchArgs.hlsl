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
#include "../srt_headers/ZstdGpuSrt_UpdateDispatchArgs.h"

/**
 *  Marks every frame in the batch as failed because the batch ran out of scratch.
 *
 *  The overflow is a per-DISPATCH condition computed from batch-global counters, so it cannot be
 *  attributed to one frame. Every frame is marked, because once predicated work is skipped no
 *  frame's output can be trusted.
 *
 *  `keepSpecificFailures` decides whether a frame that already carries a more specific failure
 *  (bad magic, dictionary, reserved bit, ...) keeps it. That is only meaningful once
 *  [Parse Frames :: Collect Blocks] has written this request's statuses (stage 1). Before that
 *  (stage 0) the buffer holds whatever an earlier request or the caller left there, so every frame
 *  must be overwritten: a stale code would otherwise survive, and stale non-negative garbage would
 *  read as success.
 *
 *  Single-threaded, and that is deliberate -- this loop only ever runs when the decode has already
 *  failed, so its cost is irrelevant. On the success path it is one comparison.
 */
static void zstdgpu_MarkAllFramesFailed(ZSTDGPU_RW_BUFFER(uint32_t) inoutFrameStatus, uint32_t frameCount, uint32_t status, uint32_t keepSpecificFailures)
{
    for (uint32_t frameIdx = 0; frameIdx < frameCount; ++frameIdx)
    {
        if (0 == keepSpecificFailures || kzstdgpu_FrameStatus_Success == inoutFrameStatus[frameIdx])
        {
            inoutFrameStatus[frameIdx] = status;
        }
    }
}

[RootSignature(ZSTDGPU_SRT_RS_UpdateDispatchArgs)]
[numthreads(1, 1, 1)]
void main()
{
    zstdgpu_UpdateDispatchArgs_SRT srt;

    zstdgpu_Srt_Fill(srt);

    const bool executeIndirectWorkaround = srt.executeIndirectWorkaround;

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
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_ComputePrefixSum,         cmpBlockCount,                            kzstdgpu_TgSizeX_PrefixSum_LiteralCount, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixBlockSizesAll,      allBlockCount,                            kzstdgpu_TgSizeX_PrefixSum, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixBlockSizesRLE,      rleBlockCount,                            kzstdgpu_TgSizeX_PrefixSum, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixBlockSizesRAW,      rawBlockCount,                            kzstdgpu_TgSizeX_PrefixSum, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_MemcpyRAW,                srt.inoutCounters[0].BlocksBytes_RAW,          kzstdgpu_TgSizeX_MemsetMemcpy, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_MemsetRLE,                srt.inoutCounters[0].BlocksBytes_RLE,          kzstdgpu_TgSizeX_MemsetMemcpy, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_ParseCompressedBlocks,    cmpBlockCount,                            kzstdgpu_TgSizeX_ParseCompressedBlocks, executeIndirectWorkaround);

        // Memset dispatch slots for InitResources Stage 1
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_RawBlockLookback,    zstdgpu_GetLookbackBlockCount(rawBlockCount),                    kzstdgpu_TgSizeX_Memset, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_RleBlockLookback,    zstdgpu_GetLookbackBlockCount(rleBlockCount),                    kzstdgpu_TgSizeX_Memset, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_CmpBlockLookback,    zstdgpu_GetLookbackBlockCount(cmpBlockCount),                    kzstdgpu_TgSizeX_Memset, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_AllBlockLookback,    zstdgpu_GetLookbackBlockCount(allBlockCount),                    kzstdgpu_TgSizeX_Memset, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_Memset_CmpBlockCount,       cmpBlockCount,                                                   kzstdgpu_TgSizeX_Memset, executeIndirectWorkaround);

        const uint32_t predicateMask = 0
                                     | (cmpBlockCount > srt.cmpBlockCountMax ? (1u << 0u) : 0u)
                                     | (rawBlockCount > srt.rawBlockCountMax ? (1u << 1u) : 0u)
                                     | (rleBlockCount > srt.rleBlockCountMax ? (1u << 2u) : 0u);

        // `reportsScratchOverflow` is set exactly when the host enforces this stage's checks with
        // predication. When it is clear, stage 0 needs a readback: the maxima above are not yet
        // known (they are typically zero), and the host sizes stage 1 from the counts it reads
        // back. The bits must then not reach the stage-2 predicate either, which IS applied when
        // stage 1 needs no readback (block constants supplied without frame constants) and would
        // otherwise skip stage 2 silently on a correctly sized request.
        const uint32_t enforcedMask = (0 != srt.reportsScratchOverflow) ? predicateMask : 0u;

        srt.inoutPredicate[0] = enforcedMask; // lower 32-bits of Stage 1 predicate
        srt.inoutPredicate[1] = 0;            // upper 32-bits of Stage 1 predicate
        srt.inoutPredicate[2] = enforcedMask; // lower 32-bits of Stage 2 predicate
        srt.inoutPredicate[3] = 0;            // upper 32-bits of Stage 2 predicate

        // Report the overflow instead of skipping silently.
        //
        // ORDERING, and it is load-bearing: [Parse Frames :: Collect Blocks] writes every frame's
        // status in stage 1, i.e. AFTER this pass, with an unconditional assignment that would
        // overwrite these marks. It cannot: that pass is predicated on exactly the mask written
        // above, so it runs only when this mask is zero and there was nothing to mark. For the same
        // reason no status of this request exists yet, so every frame is overwritten.
        if (0 != predicateMask && 0 != srt.reportsScratchOverflow)
        {
            zstdgpu_MarkAllFramesFailed(srt.inoutFrameStatus, srt.frameCount, kzstdgpu_FrameStatus_BlockCountExceeded, /* keepSpecificFailures */ 0u);
        }
    }
    else if (srt.stage == 1)
    {
        const uint32_t litByteCount = srt.inoutCounters[0].HUF_Streams_DecodedBytes;
        const uint32_t seqElemCount = srt.inoutCounters[0].Seq_Streams_DecodedItems;

        // the arguments dependent on various streams counts that are part of compressed blocks -- these could be computed after ParseCompressedBlocks
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseHufW,                  srt.inoutCounters[0].FseHufW,                  1, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseLLen,                  srt.inoutCounters[0].FseLLen,                  1, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseOffs,                  srt.inoutCounters[0].FseOffs,                  1, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FseMLen,                  srt.inoutCounters[0].FseMLen,                  1, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_HUF_WgtStreams,           srt.inoutCounters[0].HUF_WgtStreams,           1, executeIndirectWorkaround);

        // NOTE(pamartis): The number of groups running the decompression of Huffman weights depends on the number FSE tables
        // for Huffman weights because those numbers are the same because each FSE table decompresses its own Huffman weights' stream.
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecompressHuffmanWeights, srt.inoutCounters[0].FseHufW,                  kzstdgpu_TgSizeX_DecompressHuffmanWeights, executeIndirectWorkaround);

        // NOTE(pamartis): We also do decoding of uncompressed Huffman Weights stored as two nibbles per byte to make sure final representation
        // (a byte per weight) becomes identical, so identical representation simplfies initialisation of Huffman tables to use during literal decoding
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecodeHuffmanWeights,     srt.inoutCounters[0].HUF_WgtStreams,           kzstdgpu_TgSizeX_DecodeHuffmanWeights, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecompressSequences,      srt.inoutCounters[0].Seq_Streams,              srt.decompressSequences_StreamsPerTG, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_FinaliseSequenceOffsets,  srt.inoutCounters[0].Seq_Streams_DecodedItems, kzstdgpu_TgSizeX_FinaliseSequenceOffsets, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PrefixSequenceOffsets,    srt.inoutCounters[0].Seq_Streams,              kzstdgpu_TgSizeX_PrefixSequenceOffsets, executeIndirectWorkaround);
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_PropagateFseIndex,        srt.inoutCounters[0].Seq_Streams,              kzstdgpu_TgSizeX_PropagateFseIndex, executeIndirectWorkaround);

        // Bits 3 and 4 are canaries, not guards. Each maximum is individually provable (literal
        // bytes are output bytes; every sequence emits at least 3 output bytes), but the arena is
        // sized from the JOINT bound, because the two cannot both be maximal at once. So neither
        // check alone says the arena is big enough -- bit 5 does. Bits 3/4 are kept because they
        // are the only signal if one of those premises is ever violated.
        const uint32_t arenaBytesNeeded = zstdgpu_ArenaBytesForCounts(litByteCount, seqElemCount);

        const uint32_t predicateMask = 0
                                     | (litByteCount > srt.litByteCountMax ? (1u << 3u) : 0u)
                                     | (seqElemCount > srt.seqElemCountMax ? (1u << 4u) : 0u)
                                     | (arenaBytesNeeded > srt.arenaByteCount ? (1u << 5u) : 0u);

        srt.inoutPredicate[2] = srt.inoutPredicate[2] | predicateMask; // lower 32-bits of Stage 2 predicate

        // Report the overflow instead of skipping silently. Unlike the stage 0 case above, the
        // per-frame statuses have ALREADY been written by [Parse Frames :: Collect Blocks] at this
        // point, so a frame rejected on its own merits keeps that more specific reason.
        if (0 != predicateMask && 0 != srt.reportsScratchOverflow)
        {
            zstdgpu_MarkAllFramesFailed(srt.inoutFrameStatus, srt.frameCount, kzstdgpu_FrameStatus_ScratchExceeded, /* keepSpecificFailures */ 1u);
        }
    }
    else
    {
        // The number of Groups required for `DecompressLiterals` is only calculated after `ComputePrefixSum`
        zstdgpu_EmitDispatch(srt.inoutDispatchArgs, srt.inoutDispatchCnts, kzstdgpu_DispatchSlot_DecompressLiterals,       srt.inoutCounters[0].DecompressLiteralsGroups, 1, executeIndirectWorkaround);
    }
}
