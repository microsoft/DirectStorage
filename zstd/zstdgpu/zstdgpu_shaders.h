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
 * Contains definitions of various routines shared between CPU and GPU.
 */

#pragma once

#ifndef ZSTDGPU_SHADERS_H
#define ZSTDGPU_SHADERS_H

#ifdef _MSC_VER
    // NOTE(pamartis): Make sure 4505 is not leaked into a file including this header. 4505 is fine because some non-inlined function may be unused
    __pragma(warning(push))
    __pragma(warning(disable : 4505))   /**< warning C4505: 'function name': unreferenced function with internal linkage has been removed */
#endif

#include "zstdgpu_structs.h"
#include "zstdgpu_lds.h"

static void zstdgpu_TypedStoreU8(ZSTDGPU_RW_TYPED_BUFFER(uint32_t, uint8_t) inoutBuffer, uint32_t index, uint32_t value)
{
#ifdef __hlsl_dx_compiler
    inoutBuffer[index] = value;
#else
    ZSTDGPU_ASSERT(value < (1u << 8u));
    inoutBuffer[index] = (uint8_t)value;
#endif
}

static void zstdgpu_TypedStoreU16(ZSTDGPU_RW_TYPED_BUFFER(uint32_t, uint16_t) inoutBuffer, uint32_t index, uint32_t value)
{
#ifdef __hlsl_dx_compiler
    inoutBuffer[index] = value;
#else
    ZSTDGPU_ASSERT(value < (1u << 16u));
    inoutBuffer[index] = (uint16_t)value;
#endif
}

static uint32_t zstdgpu_GlobalExclusivePrefixSum(ZSTDGPU_RW_BUFFER_GLC(uint32_t) lookback,
                                                 uint32_t vgprExclusivePrefix,
                                                 uint32_t vgprCount,
                                                 uint32_t globalThreadIdx,
                                                 uint32_t tgroupThreadCnt)
{
    const uint32_t lookbackBlockSize = zstdgpu_MinU32(tgroupThreadCnt, WaveGetLaneCount());

    const uint32_t thisBlockIndex = WaveReadLaneFirst(globalThreadIdx >> zstdgpu_FindFirstBitHiU32(lookbackBlockSize));
    const uint32_t lastLaneId = WaveActiveCountBits(true) - 1u;
    const uint32_t currLaneId = WavePrefixCountBits(true);

    const uint32_t vgprInclusivePrefix = vgprExclusivePrefix + vgprCount;

    uint32_t thisBlockExclusivePrefix = 0;

    // NOTE(pamartis): the last element writes the sum of the exclusive prefix and the it's own element --
    // which is the cross-block sum, this cuts down on WaveActiveSum and re-uses WavePrefixSum instead
    if (lastLaneId == currLaneId)
    {
        // NOTE(pamartis): we limit to size of total prefix to 30 bits because upper 2 bits are used
        // to mark to mark whether the value is actual "sum" or "prefix"
        const uint32_t vgprInclusivePrefixEncoded = zstdgpu_Encode30BitLookbackSelf(vgprInclusivePrefix);
        #if 0
            lookback[thisBlockIndex] = vgprInclusivePrefixEncoded;
        #else
            InterlockedCompareStore(lookback[thisBlockIndex], 0, vgprInclusivePrefixEncoded);
        #endif

        if (thisBlockIndex > 0)
        {
            uint32_t prevBlockIndex = thisBlockIndex - 1u;

            for (;;)
            {
                #if 0
                    // BUG(pamartis): this varaint of code reads incorrect values on some HW,
                    // so it looks like `globallycoherent` keyword doesn't work for reads
                    const uint32_t prevBlockPrefixOrSumEncoded = lookback[prevBlockIndex];
                #else
                    uint32_t prevBlockPrefixOrSumEncoded;
                    InterlockedAdd(lookback[prevBlockIndex], 0, prevBlockPrefixOrSumEncoded);
                #endif
                const uint32_t flags = zstdgpu_Decode30BitLookbackFlags(prevBlockPrefixOrSumEncoded);
                if (flags > 0)
                {
                    /* NOTE(pamartis): this is the sum of the previous block or prefix sum of all previous blocks, so we accumulate it */
                    thisBlockExclusivePrefix += zstdgpu_Decode30BitLookbackValue(prevBlockPrefixOrSumEncoded);

                    /* ... and if it's a prefix sum or we reached the first block, we break ...*/
                    if (zstdgpu_Check30BitLookbackFull(flags) > 0 || prevBlockIndex == 0)
                    {
                        break;
                    }
                    else
                    {
                        /* in case we encountered a sum -- we already accumulated it*/
                        /* at the same time we didn't reach the first block, so we go backward */
                        --prevBlockIndex;
                    }
                }
                /* else -- just loop waiting for the current block */
            }
            const uint32_t thisBlockInclusivePrefixEncoded = zstdgpu_Encode30BitLookbackFull(thisBlockExclusivePrefix + vgprInclusivePrefix);
            #if 0
                lookback[thisBlockIndex] = thisBlockInclusivePrefixEncoded;
            #else
                InterlockedCompareStore(lookback[thisBlockIndex], vgprInclusivePrefixEncoded, thisBlockInclusivePrefixEncoded);
            #endif
        }
    }
    return WaveReadLaneAt(thisBlockExclusivePrefix, lastLaneId) + vgprExclusivePrefix;
}

static inline uint32_t zstdgpu_OrderedAppendIndex(ZSTDGPU_RW_BUFFER_GLC(uint32_t) lookback,
                                                  bool     threadAppendCnd,
                                                  uint32_t globalThreadIdx,
                                                  uint32_t tgroupThreadCnt)
{
    return zstdgpu_GlobalExclusivePrefixSum(lookback, WavePrefixCountBits(threadAppendCnd), threadAppendCnd ? 1u : 0u, globalThreadIdx, tgroupThreadCnt);
}

static inline uint32_t zstdgpu_OrderedAppendIndex(ZSTDGPU_RW_BUFFER_GLC(uint32_t) lookback,
                                                  uint32_t threadAppendCnt,
                                                  uint32_t globalThreadIdx,
                                                  uint32_t tgroupThreadCnt)
{
    return zstdgpu_GlobalExclusivePrefixSum(lookback, WavePrefixSum(threadAppendCnt), threadAppendCnt, globalThreadIdx, tgroupThreadCnt);
}

static inline uint32_t zstdgpu_BinarySearch(ZSTDGPU_RO_BUFFER(uint32_t) sortedSequence, uint32_t start, uint32_t count, uint32_t threadId)
{
    uint32_t rangeBase = start;
    uint32_t rangeSize = count;

    while (rangeSize > 1)
    {
        const uint32_t rangeTest = rangeSize >> 1;
        const uint32_t rangeNext = rangeBase + rangeTest;

        const uint32_t value = sortedSequence[rangeNext];
        rangeBase = threadId < value ? rangeBase : rangeNext;
        rangeSize -= rangeTest;
    }

    return rangeBase;
}

static inline uint32_t zstdgpu_BinarySearchMasked(ZSTDGPU_RO_BUFFER(uint32_t) sortedSequence, uint32_t start, uint32_t count, uint32_t key, uint32_t tstMask)
{
    uint32_t rangeBase = start;
    uint32_t rangeSize = count;

    while (rangeSize > 1)
    {
        const uint32_t rangeTest = rangeSize >> 1;
        const uint32_t rangeNext = rangeBase + rangeTest;

        const uint32_t tst = sortedSequence[rangeNext]  & tstMask;
        rangeBase = key < tst ? rangeBase : rangeNext;
        rangeSize -= rangeTest;
    }

    return rangeBase;
}

static inline uint32_t zstdgpu_BinarySearchLds(ZSTDGPU_PARAM_LDS_IN(uint32_t) GS_Base, uint32_t start, uint32_t count, uint32_t key, uint32_t tstMask)
{
    uint32_t rangeBase = start;
    uint32_t rangeSize = count;

    while (rangeSize > 1)
    {
        const uint32_t rangeTest = rangeSize >> 1;
        const uint32_t rangeNext = rangeBase + rangeTest;

        const uint32_t tst = zstdgpu_LdsLoadU32(GS_Base + rangeNext) & tstMask;
        rangeBase = key < tst ? rangeBase : rangeNext;
        rangeSize -= rangeTest;
    }

    return rangeBase;
}

static inline void zstdgpu_GroupBallotLdsStore(uint32_t laneCnt, uint32_t VGPR, ZSTDGPU_PARAM_LDS_INOUT(uint32_t) ballot_GroupDwStart, uint32_t ballot_GroupDwCountPerBit, uint32_t waveId, uint32_t bitIdx)
{
    const uint32_t4 b = WaveActiveBallot((VGPR & (1u << bitIdx)) != 0);
    if (laneCnt <= 16)
    {
        uint32_t offsetInDw = ballot_GroupDwCountPerBit * (bitIdx);
        const uint32_t ballotMaskCntPerDw = 32 / laneCnt;
        offsetInDw += (waveId) / ballotMaskCntPerDw;
        const uint32_t ballotMaskIdInDw = (waveId) % ballotMaskCntPerDw;
        zstdgpu_LdsAtomicOrU32(ballot_GroupDwStart + offsetInDw, b.x << (ballotMaskIdInDw * laneCnt));
    }
    else if (laneCnt == 32)
    {
        const uint32_t offsetInDw = ballot_GroupDwCountPerBit * (bitIdx) + (waveId);
        zstdgpu_LdsStoreU32(ballot_GroupDwStart + offsetInDw + 0, b.x);
    }
    else if (laneCnt == 64)
    {
        const uint32_t offsetInDw = ballot_GroupDwCountPerBit * (bitIdx) + (waveId) * 2;
        zstdgpu_LdsStoreU32(ballot_GroupDwStart + offsetInDw + 0, b.x);
        zstdgpu_LdsStoreU32(ballot_GroupDwStart + offsetInDw + 1, b.y);
    }
    /* for some GPUs with 128-wide waves, store extra masks*/
    else
    {
        const uint32_t offsetInDw = ballot_GroupDwCountPerBit * (bitIdx) + (waveId) * 4;
        zstdgpu_LdsStoreU32(ballot_GroupDwStart + offsetInDw + 0, b.x);
        zstdgpu_LdsStoreU32(ballot_GroupDwStart + offsetInDw + 1, b.y);
        zstdgpu_LdsStoreU32(ballot_GroupDwStart + offsetInDw + 2, b.z);
        zstdgpu_LdsStoreU32(ballot_GroupDwStart + offsetInDw + 3, b.w);
    }
}

static inline void zstdgpu_ShaderEntry_ParseFrame(ZSTDGPU_PARAM_INOUT(zstdgpu_FrameInfo) outFrameInfo,
                                                  ZSTDGPU_RW_BUFFER(zstdgpu_OffsetAndSize) outBlocksRAWRefs,
                                                  ZSTDGPU_RW_BUFFER(zstdgpu_OffsetAndSize) outBlocksRLERefs,
                                                  ZSTDGPU_RW_BUFFER(zstdgpu_OffsetAndSize) outBlocksCMPRefs,
                                                  ZSTDGPU_RW_BUFFER(uint32_t) outPerBlockUncompressedSize,
                                                  ZSTDGPU_RW_BUFFER(uint32_t) outGlobalBlockIndexPerRawBlock,
                                                  ZSTDGPU_RW_BUFFER(uint32_t) outGlobalBlockIndexPerRleBlock,
                                                  ZSTDGPU_RW_BUFFER(uint32_t) outGlobalBlockIndexPerCmpBlock,
                                                  ZSTDGPU_RW_BUFFER(uint32_t) outRawBlockSizes,
                                                  ZSTDGPU_RW_BUFFER(uint32_t) outRleBlockSizes,
                                                  ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) bits,
                                                  uint32_t outputBlockInfo)
{
    // "A content compressed by Zstandard is transformed into a Zstandard frame.
    // Multiple frames can be appended into a single file or stream. A frame is
    // totally independent, has a defined beginning and end, and a set of
    // parameters which tells the decoder how to decompress it."
    outFrameInfo.windowSize = 0;
    outFrameInfo.uncompSize = 0;
    outFrameInfo.dictionary = 0;

    const uint32_t descriptor = zstdgpu_Forward_BitBuffer_Get(bits, 8);

    // check "Reserved_bit"
    // This bit is reserved for some future feature. Its value _must be zero_.
    // A decoder compliant with this specification version must ensure it is not set.
    // This bit may be used in a future revision, to signal a feature that must be interpreted to decode the frame correctly.
    if (0 != zstdgpu_BitFieldExtractU32(descriptor, 3, 1))
    {
        ZSTDGPU_BREAK();
    }

    const uint32_t singleSegmentFlag = zstdgpu_BitFieldExtractU32(descriptor, 5, 1);
    //
    if (0 == singleSegmentFlag)
    {
        const uint32_t windowDescriptor = zstdgpu_Forward_BitBuffer_Get(bits, 8);

        // "Provides guarantees on maximum back-reference distance that will be
        // used within compressed data. This information is important for
        // decoders to allocate enough memory.
        //
        // Bit numbers  7-3         2-0
        // Field name   Exponent    Mantissa"
        const uint32_t exponent = windowDescriptor >> 3;
        const uint32_t mantissa = windowDescriptor & 7;

        // Use the algorithm from the specification to compute window size
        // https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md#window_descriptor
        const uint64_t windowBase = 1ull << (10 + exponent);
        const uint64_t windowAdd = (windowBase / 8) * mantissa;
        outFrameInfo.windowSize = windowBase + windowAdd;
    }

    const uint32_t dictionaryIdFlag = zstdgpu_BitFieldExtractU32(descriptor, 0, 2);
    if (0 != dictionaryIdFlag)
    {
        // "This is a variable size field, which contains the ID of the
        // dictionary required to properly decode the frame. Note that this
        // field is optional. When it's not present, it's up to the caller to
        // make sure it uses the correct dictionary. Format is little-endian."
        const uint32_t byteCount[] = { 0u, 1u, 2u, 4u };
        const uint32_t bitCount = byteCount[dictionaryIdFlag] * 8u;

        outFrameInfo.dictionary = zstdgpu_Forward_BitBuffer_Get(bits, bitCount);
    }

    const uint32_t frameContentSizeFlag = zstdgpu_BitFieldExtractU32(descriptor, 6, 2);

    if (0 != singleSegmentFlag || 0 != frameContentSizeFlag)
    {
        // "This is the original (uncompressed) size. This information is
        // optional. The Field_Size is provided according to value of
        // Frame_Content_Size_flag. The Field_Size can be equal to 0 (not
        // present), 1, 2, 4 or 8 bytes. Format is little-endian."
        //
        // if frame_content_size_flag == 0 but single_segment_flag is set, we
        // still have a 1 byte field
        const uint32_t byteCount[] = { 1u, 2u, 4u, 8u };
        const uint32_t bitCount = byteCount[frameContentSizeFlag] * 8u;

        if (bitCount == 64)
        {
            outFrameInfo.uncompSize = zstdgpu_Forward_BitBuffer_Get(bits, 32);
            outFrameInfo.uncompSize |= (uint64_t)zstdgpu_Forward_BitBuffer_Get(bits, 32) << 32;
        }
        else
        {
            outFrameInfo.uncompSize = zstdgpu_Forward_BitBuffer_Get(bits, bitCount);
        }

        if (16u == bitCount)
        {
            // "When Field_Size is 2, the offset of 256 is added."
            outFrameInfo.uncompSize += 256;
        }
    }

    if (0 != singleSegmentFlag)
    {
        // "The Window_Descriptor byte is optional. It is absent when
        // Single_Segment_flag is set. In this case, the maximum back-reference
        // distance is the content size itself, which can be any value from 1 to
        // 2^64-1 bytes (16 EB)."
        outFrameInfo.windowSize = outFrameInfo.uncompSize;
    }

    //
    // "A frame encapsulates one or multiple blocks. Each block can be
    // compressed or not, and has a guaranteed maximum content size, which
    // depends on frame parameters. Unlike frames, each block depends on
    // previous blocks for proper decoding. However, each block can be
    // decompressed without waiting for its successor, allowing streaming
    // operations."
    uint32_t lastBlock = 0;
    do
    {
        // "Last_Block
        //
        // The lowest bit signals if this block is the last one. Frame ends
        // right after this block.
        //
        // Block_Type and Block_Size
        //
        // The next 2 bits represent the Block_Type, while the remaining 21 bits
        // represent the Block_Size. Format is little-endian."
        zstdgpu_Forward_BitBuffer_Refill(bits, 1 + 2 + 21);

        lastBlock = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 1);
        const uint32_t blockType = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 2);
        const uint32_t blockSize = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 21);

        const bool isRaw = 0 == blockType;
        const bool isRle = 1 == blockType;
        const bool isCmp = 2 == blockType;

        uint32_t blockOffs = 0;
        if (isRle)
        {
            blockOffs = zstdgpu_Forward_BitBuffer_Get(bits, 8);
        }
        else
        {
            blockOffs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);
            zstdgpu_Forward_BitBuffer_Skip(bits, blockSize);
        }

        if (0 != outputBlockInfo)
        {
            const uint32_t blockIndex = outFrameInfo.rawBlockStart
                                      + outFrameInfo.rleBlockStart
                                      + outFrameInfo.cmpBlockStart;

            // NOTE(pamartis): Without branch, there's out-of-bounds access detected by validation layer when outBlock{Type}Refs aren't bound
            // so, it's either DXC or IHV compiler not preserving branches with memory accesses.
            ZSTDGPU_BRANCH if (isRaw)
            {
                outBlocksRAWRefs[outFrameInfo.rawBlockStart].offs = blockOffs;
                outBlocksRAWRefs[outFrameInfo.rawBlockStart].size = blockSize;

                outRawBlockSizes[outFrameInfo.rawBlockStart] = outFrameInfo.rawBlockBytesStart;
                outGlobalBlockIndexPerRawBlock[outFrameInfo.rawBlockStart] = blockIndex;
            }

            ZSTDGPU_BRANCH if (isRle)
            {
                outBlocksRLERefs[outFrameInfo.rleBlockStart].offs = blockOffs;
                outBlocksRLERefs[outFrameInfo.rleBlockStart].size = 1u;

                outRleBlockSizes[outFrameInfo.rleBlockStart] = outFrameInfo.rleBlockBytesStart;
                outGlobalBlockIndexPerRleBlock[outFrameInfo.rleBlockStart] = blockIndex;
            }

            ZSTDGPU_BRANCH if (isCmp)
            {
                outBlocksCMPRefs[outFrameInfo.cmpBlockStart].offs = blockOffs;
                outBlocksCMPRefs[outFrameInfo.cmpBlockStart].size = blockSize;

                outGlobalBlockIndexPerCmpBlock[outFrameInfo.cmpBlockStart] = blockIndex;
            }

            outPerBlockUncompressedSize[blockIndex] = isCmp ? 0 : blockSize;
        }

        // `Raw_Block` - this is an uncompressed block. `Block_Content` contains `Block_Size` bytes.
        outFrameInfo.rawBlockStart += (isRaw) ? 1 : 0;

        // `RLE_Block` - this is a single byte, repeated `Block_Size` times. `Block_Content` consists of a single byte.
        // On the decompression side, this byte must be repeated `Block_Size` times.
        outFrameInfo.rleBlockStart += (isRle) ? 1 : 0;

        // `Compressed_Block` - this is a Zstandard compressed block. `Block_Size` is the length of `Block_Content`, the compressed data.
        // The decompressed size is not known, but its maximum possible value is guaranteed (see below).
        outFrameInfo.cmpBlockStart += (isCmp) ? 1 : 0;

        outFrameInfo.rawBlockBytesStart += (isRaw) ? blockSize : 0;
        outFrameInfo.rleBlockBytesStart += (isRle) ? blockSize : 0;
    }
    while (0 == lastBlock);

    const uint32_t contentCheckSumFlag = zstdgpu_BitFieldExtractU32(descriptor, 2, 1);
    if (0 != contentCheckSumFlag)
    {
        zstdgpu_Forward_BitBuffer_Refill(bits, 32);
        zstdgpu_Forward_BitBuffer_Pop(bits, 32);
    }
}

static inline void zstdgpu_ShaderEntry_ParseFrames(ZSTDGPU_PARAM_INOUT(zstdgpu_ParseFrames_SRT) srt, uint32_t threadId)
{
    if (threadId < srt.frameCount)
    {
        zstdgpu_Forward_BitBuffer bits;
        zstdgpu_Forward_BitBuffer_InitWithSegment(bits, srt.inCompressedData, srt.inFramesRefs[threadId], srt.compressedBufferSizeInBytes);

        uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);

        // TODO: need to find/inject some skippable frames to make sure this works
        while (magic >= 0x184D2A50 && magic <= 0x184D2A5F)
        {
            const uint32_t frameSize = zstdgpu_Forward_BitBuffer_Get(bits, 32);
            zstdgpu_Forward_BitBuffer_Skip(bits, frameSize);
            magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
        }

        if (magic == 0xFD2FB528U)
        {
            zstdgpu_FrameInfo frameInfo;

            if (srt.countBlocksOnly > 0)
            {
                frameInfo.rawBlockStart = 0;
                frameInfo.rleBlockStart = 0;
                frameInfo.cmpBlockStart = 0;
                frameInfo.rawBlockBytesStart = 0;
                frameInfo.rleBlockBytesStart = 0;
            }
            else
            {
                frameInfo.rawBlockStart = srt.inoutPerFrameBlockCountRAW[threadId];
                frameInfo.rleBlockStart = srt.inoutPerFrameBlockCountRLE[threadId];
                frameInfo.cmpBlockStart = srt.inoutPerFrameBlockCountCMP[threadId];

                frameInfo.rawBlockBytesStart = srt.inoutPerFrameBlockSizesRAW[threadId];
                frameInfo.rleBlockBytesStart = srt.inoutPerFrameBlockSizesRLE[threadId];
            }

            zstdgpu_ShaderEntry_ParseFrame(
                frameInfo,
                srt.inoutBlocksRAWRefs,
                srt.inoutBlocksRLERefs,
                srt.inoutBlocksCMPRefs,
                srt.inoutBlockSizePrefix,
                srt.inoutGlobalBlockIndexPerRawBlock,
                srt.inoutGlobalBlockIndexPerRleBlock,
                srt.inoutGlobalBlockIndexPerCmpBlock,
                srt.inoutRawBlockSizePrefix,
                srt.inoutRleBlockSizePrefix,
                bits,
                srt.countBlocksOnly > 0 ? 0u : 1u
            );
            srt.inoutFrames[threadId] = frameInfo;

            if (srt.countBlocksOnly > 0)
            {
                srt.inoutPerFrameBlockCountRAW[threadId] = frameInfo.rawBlockStart;
                srt.inoutPerFrameBlockCountRLE[threadId] = frameInfo.rleBlockStart;
                srt.inoutPerFrameBlockCountCMP[threadId] = frameInfo.cmpBlockStart;
                srt.inoutPerFrameBlockCountAll[threadId] = frameInfo.rawBlockStart
                                                         + frameInfo.rleBlockStart
                                                         + frameInfo.cmpBlockStart;

                srt.inoutPerFrameBlockSizesRAW[threadId] = frameInfo.rawBlockBytesStart;
                srt.inoutPerFrameBlockSizesRLE[threadId] = frameInfo.rleBlockBytesStart;

                const uint32_t rawBlockCount = WaveActiveSum(frameInfo.rawBlockStart);
                const uint32_t rleBlockCount = WaveActiveSum(frameInfo.rleBlockStart);
                const uint32_t cmpBlockCount = WaveActiveSum(frameInfo.cmpBlockStart);
                const uint32_t rawBlockByteCount = WaveActiveSum(frameInfo.rawBlockBytesStart);
                const uint32_t rleBlockByteCount = WaveActiveSum(frameInfo.rleBlockBytesStart);

                const uint32_t uncompSize = (uint32_t)WaveActiveSum(frameInfo.uncompSize);
                const uint32_t frameCount = WaveActiveCountBits(true);

                if (WaveIsFirstLane())
                {
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_Blocks_RAW], rawBlockCount);
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_Blocks_RLE], rleBlockCount);
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_Blocks_CMP], cmpBlockCount);
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_BlocksBytes_RAW], rawBlockByteCount);
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_BlocksBytes_RLE], rleBlockByteCount);
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_Frames], frameCount);
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_Frames_UncompressedByteSize], uncompSize);
                }
            }
        }
    }
}

static void zstdgpu_ShaderEntry_InitResources(ZSTDGPU_PARAM_INOUT(zstdgpu_InitResources_SRT) srt, uint32_t threadId)
{
    ZSTDGPU_UNUSED(threadId);

    // "Per-frame" resources are initialized during right before "Frame Parsing"
    if (srt.initResourcesStage == 0)
    {
        ZSTDGPU_FOR_WORK_ITEMS(i, kzstdgpu_CounterIndex_Count, threadId, kzstdgpu_TgSizeX_InitCounters)
        {
            // Initialize default "Indirect Dispatch" arguments:
            //  - (0, 1, 1) for Huffman Weights' FSE Tables
            //  - (1, 1, 1) for Literal Lengths' FSE Tables
            //  - (1, 1, 1) for Offsets' FSE Tables
            //  - (1, 1, 1) for Match Lengths' FSE Tables
            //  - (1, 1, 1) for the number of threadgroups to decompress FSE-compressed Huffman Weights
            //  - (1, 1, 1) for the number of threadgroups to decompress UnCompressed Huffman Weights
            //  - (1, 1, 1) for The number of threadgroups to group Huffman-compressed literals by Huffman Table
            //  - (1, 1, 1) for The number of threadgroups to decompress Huffman-compressed literals
            //  - (1, 1, 1) for The number of threadgroups to decompress FSE-compressed sequences
            //
            //  - (0) for the Total HUF Weight Streams (uncompressed)
            //
            //  - (0) for the Total Sequence count in FSE-decompressed Seqeunce Streams
            //  - (0) for the Total Byte Size in HUF-decompressed Literal Streams
            //
            //  - (0) for the Total FSE-compressed Sequence Streams Counter
            //  - (0) for the Total HUF-compressed Literal Streams Counter
            //  - (0) for the Total RAW Streams Counter
            //  - (0) for the Total RLE Streams Counter
            //
            //  - (0) for the Total RAW Blocks Counter
            //  - (0) for the Total RLE Blocks Counter
            //  - (0) for the Total Compressed Blocks Counter
            //  - (0) for the Total Frames Counter
            //  - (0) for the Total Frames Uncompressed Size Counter
            // The reason we initialize the 1-st dimension of the last three "Indirect Dispatch" arguments to "1" is
            // because we always decode "Default" tables that get index "0", so at least 3 tables are always decoded.

            const bool needsZero = i == 0
                                || i == kzstdgpu_CounterIndex_DecompressHuffmanWeightsGroups
                                || i == kzstdgpu_CounterIndex_DecodeHuffmanWeightsGroups
                                || i == kzstdgpu_CounterIndex_GroupCompressedLiteralsGroups
                                || i == kzstdgpu_CounterIndex_DecompressLiteralsGroups
                                || i == kzstdgpu_CounterIndex_DecompressSequencesGroups
                                || i == kzstdgpu_CounterIndex_HUF_WgtStreams
                                || i >= kzstdgpu_CounterIndex_Seq_Streams_DecodedItems;

            if (needsZero)
                srt.inoutCounters[i] = 0;
            else
                srt.inoutCounters[i] = 1;

        }

        // NOTE(pamartis): here we initialize "lookback" regions in buffers containing prefix region + lookback region
        ZSTDGPU_FOR_WORK_ITEMS(i, zstdgpu_GetLookbackBlockCount(srt.frameCount), threadId, kzstdgpu_TgSizeX_InitCounters)
        {
            srt.inoutPerFrameBlockCountRAW[srt.frameCount + i] = 0;
            srt.inoutPerFrameBlockCountRLE[srt.frameCount + i] = 0;
            srt.inoutPerFrameBlockCountCMP[srt.frameCount + i] = 0;
            srt.inoutPerFrameBlockCountAll[srt.frameCount + i] = 0;
            srt.inoutPerFrameBlockSizesRAW[srt.frameCount + i] = 0;
            srt.inoutPerFrameBlockSizesRLE[srt.frameCount + i] = 0;
        }
        return;
    }

    const uint32_t lookbackUInt32Count = zstdgpu_GetHufFseTableIndexLookbackUInt32Count(srt.cmpBlockCount);

    ZSTDGPU_FOR_WORK_ITEMS(i, lookbackUInt32Count, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutTableIndexLookback[i] = 0;
    }

    ZSTDGPU_FOR_WORK_ITEMS(i, 1, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutFseInfos[kzstdgpu_FseRleTableCount + srt.cmpBlockCount * 1 + 0] = zstdgpu_CreateFseInfo(kzstdgpu_FseDefaultProbCount_LLen, kzstdgpu_FseDefaultProbAccuracy_LLen);
        srt.inoutFseInfos[kzstdgpu_FseRleTableCount + srt.cmpBlockCount * 2 + 1] = zstdgpu_CreateFseInfo(kzstdgpu_FseDefaultProbCount_Offs, kzstdgpu_FseDefaultProbAccuracy_Offs);
        srt.inoutFseInfos[kzstdgpu_FseRleTableCount + srt.cmpBlockCount * 3 + 2] = zstdgpu_CreateFseInfo(kzstdgpu_FseDefaultProbCount_MLen, kzstdgpu_FseDefaultProbAccuracy_MLen);
    }

    // Initialize 256 dense RLE entries at the beginning of FSE element buffers.
    // RLE table with symbol S uses FseElems[S] = PackFseElem(S, 0, 0).
    // FseInfos[S] = {0, 0}: accuracyLog=0 so decode reads 0 bits for initial state -> state=0 -> reads element[0].
    ZSTDGPU_FOR_WORK_ITEMS(i, kzstdgpu_FseRleTableCount, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutFseElems[i] = zstdgpu_PackFseElem(i, 0, 0);

        zstdgpu_FseInfo rleInfo;
        rleInfo.fseProbCountAndAccuracyLog2 = 0;
        srt.inoutFseInfos[i] = rleInfo;
    }

    // NOTE(pamartis): We initialize the buffer which is going to store (per frame) an index of the first compressed block
    // with non-zero sequence count, to initialize default offsets to 1, 4, 8
    //
    // The actual index of the compressed block is going to be determined during compressed block parsing where each
    // compressed block (thread) will store its index via atomic "min".
    ZSTDGPU_FOR_WORK_ITEMS(i, srt.frameCount, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutPerFrameSeqStreamMinIdx[i] = ~0u;
    }

    // NOTE(pamartis): We start from `srt.cmpBlockCount * kzstdgpu_MaxCount_FseProbs` because
    // it's the maximal size of FSE probabilities used for Huffman Weights FSE tables, but those don't have "default" probabilities,
    // and therefore not initialised.
    uint32_t dstStart = srt.cmpBlockCount * kzstdgpu_MaxCount_FseProbs;
    uint32_t srcStart = 0;

    // NOTE(pamartis): The reason we use extra `kzstdgpu_MaxCount_FseProbs` is to be able to store default probabilities
    const uint32_t dstTableStride = srt.cmpBlockCount * kzstdgpu_MaxCount_FseProbs + kzstdgpu_MaxCount_FseProbs;

    ZSTDGPU_FOR_WORK_ITEMS(i, kzstdgpu_FseDefaultProbCount_LLen, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutFseProbs[dstStart + i] = srt.inFseProbsDefault[srcStart + i];
    }

    dstStart += dstTableStride;
    srcStart += kzstdgpu_FseDefaultProbCount_LLen;

    ZSTDGPU_FOR_WORK_ITEMS(i, kzstdgpu_FseDefaultProbCount_Offs, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutFseProbs[dstStart + i] = srt.inFseProbsDefault[srcStart + i];
    }

    dstStart += dstTableStride;
    srcStart += kzstdgpu_FseDefaultProbCount_Offs;

    ZSTDGPU_FOR_WORK_ITEMS(i, kzstdgpu_FseDefaultProbCount_MLen, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutFseProbs[dstStart + i] = srt.inFseProbsDefault[srcStart + i];
    }

    ZSTDGPU_FOR_WORK_ITEMS(i, srt.cmpBlockCount + zstdgpu_GetLookbackBlockCount(srt.cmpBlockCount), threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutLitStreamEndPerHuffmanTable[i] = 0;
    }

#if 0 // NOTE: disabled because "zeros" are written by all compressed blocks without sequences in Compressed Block Parsing.

    ZSTDGPU_FOR_WORK_ITEMS(i, srt.cmpBlockCount, threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        // NOTE(pamartis): The motivation why we initialize the entire buffer with per-block "final" offsets to zero is somewhat non-trivial:
        //
        //  - The buffer contains two sub-buffers:
        //      1. the sub-buffer where the actual per block "final" offsets are stored
        //      2. the sub-buffer with the lookback information to propagate "final" offset between blocks
        //  - So, the lookback obviously need initialisation to zero because
        //      1. it doesn't store any data in lower 30 bits
        //      2. its upper 2-bit "flags" field of 32-bit integer have to store "0" meaning -- it's cleared
        //
        //  - The actual "repeat" offsets are initialized to "0" because it's invalid "offset value" which can't be written
        //    by sequence decompression pass which also outputs re-encoded "offset values", so writing "0" in this initialization pass,
        //    allows us to write default "repeat" offsets (1, 4, 8) for every first compressed block with non-zero sequence count in every frame,
        //    and also allows us to populate per-block final re-encoded "repeat" offsets from sequence decompression pass.
        //    So as a result, all zero offsets give us information during "repeat" offset propagation that they can't be used as "propagation source"
        //    and only can be used as "propagation destination" because corresponding blocks don't contain any sequences.
        srt.inoutPerSeqStreamFinalOffset1[i] = 3 + 1;
        srt.inoutPerSeqStreamFinalOffset2[i] = 3 + 4;
        srt.inoutPerSeqStreamFinalOffset3[i] = 3 + 8;
    }
#endif

    // NOTE(pamartis): initialize all "lookback" sub-buffers that depend on `srt.cmpBlockCount`
    ZSTDGPU_FOR_WORK_ITEMS(i, zstdgpu_GetLookbackBlockCount(srt.cmpBlockCount), threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        #ifdef __hlsl_dx_compiler
        srt.inoutLitGroupEndPerHuffmanTable[srt.cmpBlockCount + i] = 0;
        #endif
        srt.inoutPerSeqStreamFinalOffset1[srt.cmpBlockCount + i] = 0;
        srt.inoutPerSeqStreamFinalOffset2[srt.cmpBlockCount + i] = 0;
        srt.inoutPerSeqStreamFinalOffset3[srt.cmpBlockCount + i] = 0;
        srt.inoutSeqCountPrefixLookback[i] = 0;
        srt.inoutBlockSeqCountPrefixLookback[i] = 0;
    }

    // NOTE(pamartis): initialize "lookback" sub-buffer that depend on `srt.allBlockCount`
    ZSTDGPU_FOR_WORK_ITEMS(i, zstdgpu_GetLookbackBlockCount(srt.allBlockCount), threadId, kzstdgpu_TgSizeX_InitCounters)
    {
        srt.inoutBlockSizePrefix[srt.allBlockCount + i] = 0;
    }
}

static void zstdgpu_ParseFseHeader(ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) buffer,
                                   ZSTDGPU_RW_BUFFER(zstdgpu_FseInfo) outFseInfo,
                                   ZSTDGPU_RW_TYPED_BUFFER(int32_t, int16_t) outFseProb,
                                   uint32_t outFseTableIndex,
                                   uint32_t accuracyLog2Max)
{
    const uint32_t outFseProbTableOffset = outFseTableIndex * kzstdgpu_MaxCount_FseProbs - kzstdgpu_FseRleTableCount * kzstdgpu_MaxCount_FseProbs;
    if (accuracyLog2Max > 15)
    {
        ZSTDGPU_BREAK();
    }

    const uint32_t accuracyLog2 = 5u + zstdgpu_Forward_BitBuffer_Get(buffer, 4);
    if (accuracyLog2 > accuracyLog2Max)
    {
        ZSTDGPU_BREAK();
    }

    // `remain` is in range [32; 512]; we use uint32_t only due to HLSL.
    int32_t remain = 1u << accuracyLog2;
    uint32_t symbol = 0;

    while (remain > 0 && symbol < kzstdgpu_MaxCount_FseProbs)
    {
        uint32_t bits = zstdgpu_FindFirstBitHiU32(remain + 1u) + 1u;
        zstdgpu_Forward_BitBuffer_Refill(buffer, bits);

        uint32_t val = zstdgpu_Forward_BitBuffer_Top(buffer, bits);

        // Try to mask out the lower bits to see if it qualifies for the "small
        // value" threshold
        const uint32_t lowerMask = (1u << (bits - 1)) - 1u;
        const uint32_t threshold = (1u << bits) - 1u - (remain + 1u);

        if ((val & lowerMask) < threshold)
        {
            zstdgpu_Forward_BitBuffer_Pop(buffer, bits - 1);
            val = val & lowerMask;
        }
        else if (val > lowerMask)
        {
            zstdgpu_Forward_BitBuffer_Pop(buffer, bits);
            val = val - threshold;
        }
        else
        {
            zstdgpu_Forward_BitBuffer_Pop(buffer, bits);
        }

        // "Probability is obtained from Value decoded by following formula :
        // Proba = value - 1"
        const int32_t prob = (int32_t)val - 1;

        // "It means value 0 becomes negative probability -1. -1 is a special
        // probability, which means "less than 1". Its effect on distribution
        // table is described in next paragraph. For the purpose of calculating
        // cumulated distribution, it counts as one."
        remain -= prob < 0 ? -prob : prob;

#ifdef __hlsl_dx_compiler
        outFseProb[outFseProbTableOffset + symbol] = prob;
#else
        outFseProb[outFseProbTableOffset + symbol] = (int16_t)prob;
#endif
        symbol += 1;

        // "When a symbol has a probability of zero, it is followed by a 2-bits
        // repeat flag. This repeat flag tells how many probabilities of zeroes
        // follow the current one. It provides a number ranging from 0 to 3. If
        // it is a 3, another 2-bits repeat flag follows, and so on."
        if (prob == 0)
        {
            // Read the next two bits to see how many more 0s
            uint32_t repeat = zstdgpu_Forward_BitBuffer_Get(buffer, 2);

            do
            {
                for (uint32_t i = 0; i < repeat && symbol < kzstdgpu_MaxCount_FseProbs; ++i)
                {
                    outFseProb[outFseProbTableOffset + symbol] = 0;
                    symbol += 1;
                }
                if (repeat == 3)
                {
                    repeat = zstdgpu_Forward_BitBuffer_Get(buffer, 2);
                }
                else
                {
                    break;
                }
            }
            while (true);
        }
    }
    zstdgpu_Forward_BitBuffer_ByteAlign(buffer);
    if (remain != 0 || symbol >= kzstdgpu_MaxCount_FseProbs)
    {
        // "When last symbol reaches cumulated total of 1 << Accuracy_Log, decoding is complete.
        // If the last symbol makes cumulated total go above 1 << Accuracy_Log, distribution is considered corrupted."
        ZSTDGPU_BREAK(); // Corruption
    }
    outFseInfo[outFseTableIndex] = zstdgpu_CreateFseInfo(symbol, accuracyLog2);
}

static void zstdgpu_ShaderEntry_ParseCompressedBlocks(ZSTDGPU_PARAM_INOUT(zstdgpu_ParseCompressedBlocks_SRT) srt, uint32_t threadId)
{
    if (threadId >= srt.compressedBlockCount)
        return;

    zstdgpu_Forward_BitBuffer buffer;
    zstdgpu_Forward_BitBuffer_InitWithSegment(buffer, srt.inCompressedData, srt.inBlocksCMPRefs[threadId], srt.compressedBufferSizeInBytes);

    zstdgpu_CompressedBlockData outBlockData;
    zstdgpu_Init_CompressedBlockData(outBlockData);

    zstdgpu_Forward_BitBuffer_Refill(buffer, 2 + 2);

    // Literals can be stored uncompressed or compressed using Huffman prefix codes.
    // When compressed, a tree description may optionally be present, followed by 1 or 4 streams.

    // Header is in charge of describing how literals are packed.
    // It's a byte-aligned variable-size bitfield, ranging from 1 to 5 bytes, using little-endian convention.
    //
    //  | `Literals_Block_Type` | `Size_Format` | `Regenerated_Size` | [`Compressed_Size`] |
    //  | --------------------- | ------------- | ------------------ | ------------------- |
    //  |       2 bits          |  1 - 2 bits   |    5 - 20 bits     |     0 - 18 bits     |
    //
    // `Literals_Block_Type`
    //
    //  This field uses 2 lowest bits of first byte, describing 4 different block types :
    //
    //      | `Literals_Block_Type`       | Value |
    //      | --------------------------- | ----- |
    //      | `Raw_Literals_Block`        |   0   |
    //      | `RLE_Literals_Block`        |   1   |
    //      | `Compressed_Literals_Block` |   2   |
    //      | `Treeless_Literals_Block`   |   3   |
    const uint32_t literalBlockType = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 2);

    //
    const uint32_t literalBlockSzFmt = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 2);

    uint32_t regeneratedSize = 0;
    // `Size_Format` is divided into 2 families :
    //      - For `Raw_Literals_Block` and `RLE_Literals_Block`, it's only necessary to decode `Regenerated_Size`.
    //        There is no `Compressed_Size` field.
    if (0x1 >= literalBlockType)
    {
        //  `Size_Format` uses 1 _or_ 2 bits. Its value is : `Size_Format = (Literals_Section_Header[0]>>2) & 3`
        //      - `Size_Format` == 00 or 10 : `Size_Format` uses 1 bit.
        if (0 == zstdgpu_BitFieldExtractU32(literalBlockSzFmt, 0, 1))
        {
            //  `Regenerated_Size` uses 5 bits (0-31).
            //  `Literals_Section_Header` uses 1 byte.
            //  `Regenerated_Size = Literals_Section_Header[0]>>3`
            regeneratedSize = (zstdgpu_Forward_BitBuffer_Get(buffer, 8 - 4) << 1) | zstdgpu_BitFieldExtractU32(literalBlockSzFmt, 1, 1);
        }
        // - `Size_Format` == 01 : `Size_Format` uses 2 bits.
        else if (0x1 == literalBlockSzFmt)
        {
            //  `Regenerated_Size` uses 12 bits (0-4095).
            //  `Literals_Section_Header` uses 2 bytes.
            //  `Regenerated_Size = (Literals_Section_Header[0]>>4) + (Literals_Section_Header[1]<<4)`
            regeneratedSize = zstdgpu_Forward_BitBuffer_Get(buffer, 16 - 4);
        }
        // - `Size_Format` == 11 : `Size_Format` uses 2 bits.
        else
        {
            //  `Regenerated_Size` uses 20 bits (0-1048575).
            //  `Literals_Section_Header` uses 3 bytes.
            //  `Regenerated_Size = (Literals_Section_Header[0]>>4) + (Literals_Section_Header[1]<<4) + (Literals_Section_Header[2]<<12)`
            regeneratedSize = zstdgpu_Forward_BitBuffer_Get(buffer, 24 - 4);
        }

        if (literalBlockType == 0)
        {
            outBlockData.literal.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
            outBlockData.literal.offs = zstdgpu_EncodeRawLitOffset(outBlockData.literal.offs);
            zstdgpu_Forward_BitBuffer_Skip(buffer, regeneratedSize);
        }
        else
        {
            outBlockData.literal.offs = zstdgpu_Forward_BitBuffer_Get(buffer, 8);
            outBlockData.literal.offs = zstdgpu_EncodeRleLitOffset(outBlockData.literal.offs);
        }
        outBlockData.literal.size = regeneratedSize;

        const uint32_t rawStreamCountPerWave = WaveActiveCountBits(literalBlockType == 0);
        const uint32_t rleStreamCountPerWave = WaveActiveCountBits(literalBlockType == 1);
        if (WaveIsFirstLane())
        {
            InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_RAW_Streams], rawStreamCountPerWave);
            InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_RLE_Streams], rleStreamCountPerWave);
        }
    }
    // ...
    //      - For `Compressed_Block` and `Treeless_Literals_Block`, it's required to decode both `Compressed_Size`
    //        and `Regenerated_Size` (the decompressed size). It's also necessary to decode the number of streams (1 or 4).
    else
    {
        //  `Size_Format` always uses 2 bits.
        //  - `Size_Format` == 00 : A single stream.
        //  - `Size_Format` == 01 : 4 streams.
        //  - `Size_Format` == 10 : 4 streams.
        //  - `Size_Format` == 11 : 4 streams.
        const uint32_t streamCount = (0x0 == literalBlockSzFmt) ? 1 : 4;

        uint32_t compressedSize = 0;

        //  - `Size_Format` == 00 : A single stream. Both `Regenerated_Size` and `Compressed_Size` use 10 bits (0-1023).
        //  - `Size_Format` == 01 : 4 streams. Both `Regenerated_Size` and `Compressed_Size` use 10 bits (0-1023).
        //      `Literals_Section_Header` uses 3 bytes.
        if (0x1 >= literalBlockSzFmt)
        {
            zstdgpu_Forward_BitBuffer_Refill(buffer, 10 + 10);
            regeneratedSize = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 10);
            compressedSize = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 10);
        }
        //  - `Size_Format` == 10 : 4 streams. Both `Regenerated_Size` and `Compressed_Size` use 14 bits (6-16383).
        //      `Literals_Section_Header` uses 4 bytes.
        else if (0x2 == literalBlockSzFmt)
        {
            zstdgpu_Forward_BitBuffer_Refill(buffer, 14 + 14);
            regeneratedSize = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 14);
            compressedSize = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 14);
        }
        // - `Size_Format` == 11 : 4 streams. Both `Regenerated_Size` and `Compressed_Size` use 18 bits (6-262143).
        //      `Literals_Section_Header` uses 5 bytes.
        else
        {
            // CONSIDER: [32; 64] - bit buffer reads
            regeneratedSize = zstdgpu_Forward_BitBuffer_Get(buffer, 18);
            compressedSize = zstdgpu_Forward_BitBuffer_Get(buffer, 18);
        }

        if (kzstdgpu_MaxCount_LiteralBytes < regeneratedSize)
        {
            ZSTDGPU_BREAK(); // Corruption
        }

        const uint32_t streamCountPerWave = WaveActiveSum(streamCount);
        const uint32_t regeneratedSizePerWave = WaveActiveSum(regeneratedSize);

        uint32_t streamOffsetPerWave = 0;
        uint32_t regeneratedOffsetPerWave = 0;
        if (WaveIsFirstLane())
        {
            InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_HUF_Streams], streamCountPerWave, streamOffsetPerWave);
            InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_HUF_Streams_DecodedBytes], regeneratedSizePerWave, regeneratedOffsetPerWave);
        }
        const uint32_t streamOffset = WaveReadLaneFirst(streamOffsetPerWave) + WavePrefixSum(streamCount);
        const uint32_t regeneratedOffset = WaveReadLaneFirst(regeneratedOffsetPerWave) + WavePrefixSum(regeneratedSize);

        outBlockData.literal.offs = zstdgpu_EncodeCmpLitOffset(regeneratedOffset);
        outBlockData.literal.size = regeneratedSize;
        outBlockData.litStreamIndex = streamOffset;

        //  Note: `Compressed_Size` includes the size of the Huffman Tree description when it is present.
        //  Note 2: `Compressed_Size` can never be `==0`.
        //      Even in single-stream scenario, assuming an empty content, it must be `>=1`, since it contains at least the final end bit flag.
        //      In 4-streams scenario, a valid `Compressed_Size` is necessarily `>= 10` (6 bytes for the jump table, + 4x1 bytes for the 4 streams).
        if (0x0 == literalBlockSzFmt)
        {
            if (1 > compressedSize)
            {
                ZSTDGPU_BREAK();
            }
        }
        else
        {
            if (10 > compressedSize)
            {
                ZSTDGPU_BREAK();
            }
        }

        //  `Huffman_Tree_Description`
        //      This section is only present when `Literals_Block_Type` type is `Compressed_Literals_Block` (`2`).
        //      The tree describes the weights of all literals symbols that can be present in the literals block, at least 2 and up to 256.
        //      The size of `Huffman_Tree_Description` is determined during decoding process, it must be used to determine where streams begin.
        //          `Total_Streams_Size = Compressed_Size - Huffman_Tree_Description_Size`.
        if (0x2 == literalBlockType)
        {
            //  `Huffman Tree header`
            //      This is a single byte value (0-255), which describes how the series of weights is encoded.
            const uint32_t headerByte = zstdgpu_Forward_BitBuffer_Get(buffer, 8);
            // - if `headerByte` < 128 : the series of weights is compressed using FSE
            //      The length of the FSE-compressed series is equal to `headerByte` (0-127).
            if (128 > headerByte)
            {
                const uint32_t fseProbOffs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);

                #define ALLOCATE_FSE_TABLE_INDEX(name)                                      \
                    const uint32_t fseWaveTableCount##name = WaveActiveCountBits(true);     \
                    uint32_t fseWaveTableStart##name = 0;                                   \
                    if (WaveIsFirstLane())                                                  \
                    {                                                                       \
                        InterlockedAdd(                                                     \
                            srt.inoutCounters[kzstdgpu_CounterIndex_Fse##name],             \
                            fseWaveTableCount##name,                                        \
                            fseWaveTableStart##name                                         \
                        );                                                                  \
                    }                                                                       \
                    outBlockData.fseTableIndex##name = zstdgpu_ComputeFseIndex##name(       \
                        WaveReadLaneFirst(fseWaveTableStart##name) + WavePrefixCountBits(true),\
                        srt.compressedBlockCount                                            \
                    );                                                                      \
                    zstdgpu_ParseFseHeader(buffer, srt.inoutFseInfos, srt.inoutFseProbs, outBlockData.fseTableIndex##name, kzstdgpu_FseProbMaxAccuracy_##name)

                ALLOCATE_FSE_TABLE_INDEX(HufW);

                outBlockData.fseTableIndexHufW -= zstdgpu_ComputeFseIndexHufW(0, srt.compressedBlockCount);

                zstdgpu_OffsetAndSize fseCompressedHuffmanWeights;
                fseCompressedHuffmanWeights.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
                fseCompressedHuffmanWeights.size = headerByte - (fseCompressedHuffmanWeights.offs - fseProbOffs);
                zstdgpu_Forward_BitBuffer_Skip(buffer, fseCompressedHuffmanWeights.size);

                srt.inoutHufRefs[outBlockData.fseTableIndexHufW] = fseCompressedHuffmanWeights;

                // NOTE(pamartis): We write zero here to initialize `counts` because the actual number
                // of Huffman Weights becomes known after they are decompressed (which happens in another kernel)
                srt.inoutDecompressedHuffmanWeightCount[outBlockData.fseTableIndexHufW] = 0;

                ZSTDGPU_ASSERT(fseCompressedHuffmanWeights.offs - fseProbOffs < headerByte);

                // +1: account for `headerByte`
                compressedSize -= headerByte + 1;
            }
            //  - if `headerByte` >= 128 :
            //      -  the series of weights uses a direct representation, where each `Weight` is encoded directly as a 4 bits field (0-15).
            //      - They are encoded forward, 2 weights to a byte, first weight taking the top four bits and second one taking the bottom four.
            //          * e.g. the following operations could be used to read the weights:
            //              `Weight[0] = (Byte[0] >> 4), Weight[1] = (Byte[0] & 0xf)`, etc.
            //      - The full representation occupies `Ceiling(Number_of_Weights/2)` bytes, meaning it uses only full bytes even if `Number_of_Weights` is odd.
            //      - `Number_of_Weights = headerByte - 127`.
            //          * Note that maximum `Number_of_Weights` is 255-127 = 128, therefore, only up to 128 `Weight` can be encoded using direct representation.
            //          * Since the last non-zero `Weight` is _not_ encoded, this scheme is compatible with alphabet sizes of up to 129 symbols, hence including literal symbol 128.
            //          * If any literal symbol > 128 has a non-zero `Weight`, direct representation is not possible. In such case, it's necessary to use FSE compression.
            else
            {
                const uint32_t uncompressedHuffmanWeightsCountPerWave = WaveActiveCountBits(true);
                uint32_t uncompressedHuffmanWeightsStartPerWave = 0;
                if (WaveIsFirstLane())
                {
                    InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_HUF_WgtStreams], uncompressedHuffmanWeightsCountPerWave, uncompressedHuffmanWeightsStartPerWave);
                }
                outBlockData.fseTableIndexHufW = WaveReadLaneFirst(uncompressedHuffmanWeightsStartPerWave) + WavePrefixCountBits(true);

                const uint32_t huffWeightCnt = headerByte - 127;

                zstdgpu_OffsetAndSize uncompressedHuffmanWeights;
                uncompressedHuffmanWeights.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
                uncompressedHuffmanWeights.size = (huffWeightCnt + 1) >> 1;
                zstdgpu_Forward_BitBuffer_Skip(buffer, uncompressedHuffmanWeights.size);

                // NOTE(pamartis): store the reference to the uncompressed weights at the end of the stream
                // to save memory and use `compressedBlockCount` references for both FSE-compressed and
                // uncompressed references
                outBlockData.fseTableIndexHufW = srt.compressedBlockCount - 1 - outBlockData.fseTableIndexHufW;

                srt.inoutHufRefs[outBlockData.fseTableIndexHufW] = uncompressedHuffmanWeights;
                zstdgpu_TypedStoreU8(srt.inoutDecompressedHuffmanWeightCount, outBlockData.fseTableIndexHufW, huffWeightCnt);

                // +1: account for `headerByte`
                compressedSize -= uncompressedHuffmanWeights.size + 1;
            }
        }
        else
        {
            outBlockData.fseTableIndexHufW = kzstdgpu_FseProbTableIndex_Repeat;
        }

        if (1 == streamCount)
        {
            srt.inoutLitRefs[streamOffset].src.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
            srt.inoutLitRefs[streamOffset].src.size = compressedSize;
            srt.inoutLitRefs[streamOffset].dst.offs = regeneratedOffset;
            srt.inoutLitRefs[streamOffset].dst.size = regeneratedSize;
            zstdgpu_Forward_BitBuffer_Skip(buffer, compressedSize);
        }
        else
        {
            zstdgpu_Forward_BitBuffer_Refill(buffer, 16 + 16);
            const uint32_t litStream0Size = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 16);
            const uint32_t litStream1Size = zstdgpu_Forward_BitBuffer_GetNoRefill(buffer, 16);
            const uint32_t litStream2Size = zstdgpu_Forward_BitBuffer_Get(buffer, 16);

            srt.inoutLitRefs[streamOffset + 0].src.size = litStream0Size;
            srt.inoutLitRefs[streamOffset + 1].src.size = litStream1Size;
            srt.inoutLitRefs[streamOffset + 2].src.size = litStream2Size;

            compressedSize -= 6;

            const uint32_t compressedSize3Streams = litStream0Size
                                                  + litStream1Size
                                                  + litStream2Size;

            ZSTDGPU_ASSERT(compressedSize >= compressedSize3Streams);
            const uint32_t litStream3Size = compressedSize - compressedSize3Streams;
            srt.inoutLitRefs[streamOffset + 3].src.size = litStream3Size;

            const uint32_t dstSize = (regeneratedSize + 3) / 4;
            uint32_t dstOffs = regeneratedOffset;

            srt.inoutLitRefs[streamOffset + 0].src.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
            srt.inoutLitRefs[streamOffset + 0].dst.offs = dstOffs;
            srt.inoutLitRefs[streamOffset + 0].dst.size = dstSize;
            dstOffs += dstSize;
            zstdgpu_Forward_BitBuffer_Skip(buffer, litStream0Size);

            srt.inoutLitRefs[streamOffset + 1].src.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
            srt.inoutLitRefs[streamOffset + 1].dst.offs = dstOffs;
            srt.inoutLitRefs[streamOffset + 1].dst.size = dstSize;
            dstOffs += dstSize;
            zstdgpu_Forward_BitBuffer_Skip(buffer, litStream1Size);

            srt.inoutLitRefs[streamOffset + 2].src.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
            srt.inoutLitRefs[streamOffset + 2].dst.offs = dstOffs;
            srt.inoutLitRefs[streamOffset + 2].dst.size = dstSize;
            dstOffs += dstSize;
            zstdgpu_Forward_BitBuffer_Skip(buffer, litStream2Size);

            srt.inoutLitRefs[streamOffset + 3].src.offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
            srt.inoutLitRefs[streamOffset + 3].dst.offs = dstOffs;
            srt.inoutLitRefs[streamOffset + 3].dst.size = regeneratedSize - dstSize * 3;
            zstdgpu_Forward_BitBuffer_Skip(buffer, litStream3Size);
        }
    }
    // NOTE(pamartis): The reason we output decompressed literal size into the buffer storing decompressed
    // block sizes for prefix computations (to know starting position where to write decompresed data):
    //      1. For compresssed blocks without sequences, the size (of content) of decompressed literal
    //         is the size of the decompressed block
    //      2. For compressed blocks with sequences, we need the size of decompressed literal to compute
    //         the size of decompressed block as `blockSize = SUM(mlen[i]) + literalSize`
    //         because literal is fully copied in chunks (of llen bytes per sequence) + via the final copy
    //         of the remaining portion of literals not copied via sequence execution.
    const uint32_t blockIndexInFrame = srt.inGlobalBlockIndexPerCmpBlock[threadId];

    srt.inoutBlockSizePrefix[blockIndexInFrame] = outBlockData.literal.size;

    // `Sequences_Section_Header`
    // Consists of 2 items:
    //      -`Number_of_Sequences`
    //      - Symbol compression modes
    //
    // `Number_of_Sequences`
    //
    // This is a variable size field using between 1 and 3 bytes. Let's call its first byte `byte0`.
    const uint32_t sequenceByte0 = zstdgpu_Forward_BitBuffer_Get(buffer, 8);

    // - `if (byte0 < 128)` : `Number_of_Sequences = byte0`.Uses 1 byte.
    uint32_t seqCount = 0;
    if (sequenceByte0 < 128)
    {
        seqCount = sequenceByte0;
    }
    // - `if (byte0 < 255)` : `Number_of_Sequences = ((byte0 - 0x80) << 8) + byte1`. Uses 2 bytes. Note that the 2 bytes format fully overlaps the 1 byte format.
    else if (sequenceByte0 < 255)
    {
        seqCount = ((sequenceByte0 - 128) << 8) + zstdgpu_Forward_BitBuffer_Get(buffer, 8);
    }
    // - `if (byte0 == 255)`: `Number_of_Sequences = byte1 + (byte2 << 8) + 0x7F00`. Uses 3 bytes.
    else
    {
        seqCount = zstdgpu_Forward_BitBuffer_Get(buffer, 16) + 0x7F00;
    }

    const bool seqStreamPresent = 0 != seqCount;

    const uint32_t waveSeqStreamCount = WaveActiveCountBits(seqStreamPresent);
    const uint32_t waveSeqCount = WaveActiveSum(seqCount);

    #ifndef __hlsl_dx_compiler
        const uint32_t seqStreamIndex = srt.inoutCounters[kzstdgpu_CounterIndex_Seq_Streams];
        const uint32_t seqIndex = srt.inoutCounters[kzstdgpu_CounterIndex_Seq_Streams_DecodedItems];
    #endif

    if (WaveIsFirstLane())
    {
        InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_Seq_Streams], waveSeqStreamCount);
        InterlockedAdd(srt.inoutCounters[kzstdgpu_CounterIndex_Seq_Streams_DecodedItems], waveSeqCount);
    }

    #ifdef __hlsl_dx_compiler
        const uint32_t seqStreamIndex = zstdgpu_OrderedAppendIndex(srt.inoutBlockSeqCountPrefixLookback, seqStreamPresent, threadId, kzstdgpu_TgSizeX_ParseCompressedBlocks);
        const uint32_t seqIndex = zstdgpu_OrderedAppendIndex(srt.inoutSeqCountPrefixLookback, seqCount, threadId, kzstdgpu_TgSizeX_ParseCompressedBlocks);
    #endif

    //  `if (Number_of_Sequences == 0)` : there are no sequences.
    //      The sequence section stops immediately,
    //      FSE tables used in `Repeat_Mode` aren't updated.
    //      Block's decompressed content is defined solely by the Literals Section content.
    //
    if (0 != seqCount)
    {
        // Symbol compression modes
        //
        // This is a single byte, defining the compression mode of each symbol type.
        //
        // | Bit number |       7 - 6             |       5 - 4    |         3 - 2        |    1 - 0   |
        // | -----------|-------------------------|----------------|----------------------|------------|
        // | Field name | `Literals_Lengths_Mode` | `Offsets_Mode` | `Match_Lengths_Mode` | `Reserved` |
        //
        // The last field, `Reserved`, must be all - zeroes.
        const uint32_t compressionModes = zstdgpu_Forward_BitBuffer_Get(buffer, 8);

        outBlockData.seqStreamIndex = seqStreamIndex;
        srt.inoutPerSeqStreamSeqStart[outBlockData.seqStreamIndex] = seqIndex;

        if (0 != zstdgpu_BitFieldExtractU32(compressionModes, 0, 2))
        {
            ZSTDGPU_BREAK();
        }

        #define PARSE_FSE_TABLE(startBit, name)                                             \
            const uint32_t mode##name = zstdgpu_BitFieldExtractU32(compressionModes, startBit, 2); \
            if (2 == mode##name)                                                            \
            {                                                                               \
                ALLOCATE_FSE_TABLE_INDEX(name);                                             \
            }                                                                               \
            else if (0 == mode##name)                                                       \
            {                                                                               \
                /** default FSE table is always at index '0' in a chunk of appropriate type*/\
                outBlockData.fseTableIndex##name = zstdgpu_ComputeFseIndex##name(0, srt.compressedBlockCount);\
            }                                                                               \
            else if (1 == mode##name)                                                       \
            {                                                                               \
                outBlockData.fseTableIndex##name = zstdgpu_Forward_BitBuffer_Get(buffer, 8); \
            }                                                                               \
            else/* if (3 == mode##name)*/                                                   \
            {                                                                               \
                outBlockData.fseTableIndex##name = kzstdgpu_FseProbTableIndex_Repeat;        \
            }

        PARSE_FSE_TABLE(6, LLen)
        PARSE_FSE_TABLE(4, Offs)
        PARSE_FSE_TABLE(2, MLen)

        #undef PARSE_FSE_TABLE
        #undef ALLOCATE_FSE_TABLE_INDEX

        const uint32_t offs = zstdgpu_Forward_BitBuffer_GetByteOffset(buffer);
        srt.inoutSeqRefs[outBlockData.seqStreamIndex].src.offs = offs;
        srt.inoutSeqRefs[outBlockData.seqStreamIndex].src.size = buffer.datasz - offs;

        // NOTE(pamartis): given the prefix sum (exclusive) of compressed block counts in each frame (srt.inPerFrameBlockCountCMP)
        // each threadId (compressed block index) does a binary search of its ZSTD frame index
        // when the frame index is found, the minimal index across all sequence streams in a frame is stored
        {
            const uint32_t frameIndex = zstdgpu_BinarySearch(srt.inPerFrameBlockCountCMP, 0, srt.frameCount, threadId);

            // TODO (pamartis): Add scalarisation loop if needed
            InterlockedMin(srt.inoutPerFrameSeqStreamMinIdx[frameIndex], outBlockData.seqStreamIndex);
        }
    }

#ifdef __hlsl_dx_compiler

    // Setup default FSE-indices we are going to propagate
    uint32_t4 fseTableIndices = uint32_t4(
        outBlockData.fseTableIndexHufW,
        outBlockData.fseTableIndexLLen,
        outBlockData.fseTableIndexOffs,
        outBlockData.fseTableIndexMLen
    );

    const uint32_t blockSize = min(WaveGetLaneCount(), kzstdgpu_TgSizeX_ParseCompressedBlocks);

    const uint32_t thisBlockIndex = WaveReadLaneFirst(threadId / blockSize);
    const uint32_t thisLocalIndex = threadId % blockSize;

    const uint32_t lastLocalIndex = WaveActiveCountBits(true) - 1u;

    #define WAVE_SHUFFLE(v, and_mask, or_mask, xor_mask) WaveReadLaneAt(v, ((WaveGetLaneIndex() & (and_mask)) | (or_mask)) ^ (xor_mask))

    #define WAVE_BROADCAST(v, group_size, group_lane) WAVE_SHUFFLE(v, ~(group_size - 1u), group_lane, 0)

    #define WAVE_PROPAGATE_STEP(p, group_size)  \
        if (blockSize >= group_size /** this condition is expected to be a compile-time condition, so no real branch */) \
        { \
            /* for every group of `group_size` consecutive lanes, broadcast the value from the last lane of the "odd" sub-group of 2x smaller size) */     \
            uint32_t b = WAVE_BROADCAST(p, group_size, group_size / 2u - 1u);                                   \
            /* for every group of `group_size` consecutive lanes */                                             \
            /* propagate element from the last lane of the "odd" sub-group of 2x smaller size  */               \
            /* into all elements of the "even" sub-group of 2x smaller size when propagated value makes sense */\
            [flatten] if ((WaveGetLaneIndex() & (group_size / 2u)))                                             \
            {                                                                                                   \
                /* We propagate only non-Repeat and not-Unused values to lanes containing Repeat/Unused values*/\
                if (p >= kzstdgpu_FseProbTableIndex_Repeat && b < kzstdgpu_FseProbTableIndex_Repeat)              \
                    p = b;                                                                                      \
            }                                                                                                   \
        }

    // To propagate FSE table indices, we use a variant of "Decoupled Lookback"
    //      1. Each block (a group of `blockSize` threads) looks at indices of each type of FSE table
    //         and checks for each of FSE table type if there's any FSE table "index" that is not `Unused`
    //         and is not `Repeat` which means the block can propagate its last not `Repeat` and not `Unused` value
    //         to the next block. If the block can propagate indices for all FSE table types, it does so and stores
    //         the result in the Lookback buffer (so the next block can read it sooner) and marks them as 'Ready'
    //      2. If the block can't propagate all indices to the succeeding blocks because it contains only `Repeat` and `Unused`
    //         indices, it also marks the buffer as 'Ready' (but stored indices are still `Unused` or `Repeat`)
    //      3. If the block needs previous block to complete (the first "index" that is not `Unused` has `Repeat` for any of the FSE table types)
    //         it does the lookback (it goes looking for the first `Ready' block, it skips `Ready` block with `Unused` values, and stops at `Ready' block with valid values)
    //      4. The block uses previous indices to do propagation and stores its own results, it also stores "lookback"
    //         indices for succeeding block if it didn't do so in Step 1.

    #if 0
        #define LOOKBACK_STORE(name, value) \
            srt.inoutTableIndexLookback[thisBlockIndex].fseTableIndex##name = (value)
    #else
        #define LOOKBACK_STORE(name, value) \
            InterlockedCompareStore(srt.inoutTableIndexLookback[thisBlockIndex].fseTableIndex##name, 0, (value))
    #endif

    #if 0
        // BUG(pamartis): this varaint of code reads incorrect values on some HW, so it looks like `globallycoherent`
        // keyword doesn't work for reads
        #define LOOKBACK_READ(name, index)  \
            const uint32_t lookbackIndex##name = srt.inoutTableIndexLookback[index].fseTableIndex##name
    #else
        #define LOOKBACK_READ(name, index)  \
            uint32_t lookbackIndex##name;   \
            InterlockedAdd(srt.inoutTableIndexLookback[index].fseTableIndex##name, 0, lookbackIndex##name)
    #endif

    const bool indexValidHufW = outBlockData.fseTableIndexHufW < kzstdgpu_FseProbTableIndex_Repeat;
    const bool indexValidLLen = outBlockData.fseTableIndexLLen < kzstdgpu_FseProbTableIndex_Repeat;
    const bool indexValidOffs = outBlockData.fseTableIndexOffs < kzstdgpu_FseProbTableIndex_Repeat;
    const bool indexValidMLen = outBlockData.fseTableIndexMLen < kzstdgpu_FseProbTableIndex_Repeat;

    #define LOOKBACK_STORE_EARLY_ALL_INVALID(name)                                                              \
        if (WaveActiveAllTrue(!indexValid##name))                                                               \
        {                                                                                                       \
            if (WaveIsFirstLane())                                                                              \
            {                                                                                                   \
                LOOKBACK_STORE(name, zstdgpu_Encode31BitLookbackFull(kzstdgpu_FseProbTableIndex_Unused));       \
            }                                                                                                   \
        }

    // NOTE(pamartis): STEP 2. Check that it's possible to prepare FSE/Huffman table index for the
    // succeeding blocks early (all lanes of each "index" are either `Repeat` or `Unused`)
    LOOKBACK_STORE_EARLY_ALL_INVALID(HufW)
    LOOKBACK_STORE_EARLY_ALL_INVALID(LLen)
    LOOKBACK_STORE_EARLY_ALL_INVALID(Offs)
    LOOKBACK_STORE_EARLY_ALL_INVALID(MLen)

    #undef LOOKBACK_STORE_EARLY_ALL_INVALID

    #define LOOKBACK_STORE_EARLY_ANY_VALID(name)            \
        if (WaveActiveAnyTrue(indexValid##name))            \
        {                                                   \
            uint32_t x = outBlockData.fseTableIndex##name;  \
            WAVE_PROPAGATE_STEP(x, 2)                       \
            WAVE_PROPAGATE_STEP(x, 4)                       \
            WAVE_PROPAGATE_STEP(x, 8)                       \
            WAVE_PROPAGATE_STEP(x, 16)                      \
            WAVE_PROPAGATE_STEP(x, 32)                      \
            const uint32_t xLast = WaveReadLaneAt(x, lastLocalIndex); \
            if (WaveIsFirstLane())                          \
            {                                               \
                LOOKBACK_STORE(name, zstdgpu_Encode31BitLookbackFull(xLast));\
            }                                               \
        }

    // NOTE(pamartis): STEP 1. Check that if it's possible to prepare FSE/Huffman table index for
    // the succeeding blocks early (at least one lane of each "index" is other than `Repeat` and `Unused`)
    LOOKBACK_STORE_EARLY_ANY_VALID(HufW)
    LOOKBACK_STORE_EARLY_ANY_VALID(LLen)
    LOOKBACK_STORE_EARLY_ANY_VALID(Offs)
    LOOKBACK_STORE_EARLY_ANY_VALID(MLen)

    #undef LOOKBACK_STORE_EARLY_ANY_VALID
    #undef LOOKBACK_STORE

    #define INIT_NEEDS_LOOKBACK(name)                                                                                       \
        bool needsLookback##name = false;                                                                                   \
        if (outBlockData.fseTableIndex##name != kzstdgpu_FseProbTableIndex_Unused)                                           \
        {                                                                                                                   \
            needsLookback##name = WaveReadLaneFirst(outBlockData.fseTableIndex##name) == kzstdgpu_FseProbTableIndex_Repeat;  \
        }                                                                                                                   \
        const bool needsLookbackUniform##name = WaveActiveAnyTrue(needsLookback##name)

    //  NOTE(pamartis): STEP 3. for each index type, check the first lane that doesn't contain
    //      `kzstdgpu_FseProbTableIndex_Unused` if it is kzstdgpu_FseProbTableIndex_Repeat
    //      (otherwise the index can be fully propagated)
    INIT_NEEDS_LOOKBACK(HufW);
    INIT_NEEDS_LOOKBACK(LLen);
    INIT_NEEDS_LOOKBACK(Offs);
    INIT_NEEDS_LOOKBACK(MLen);

    uint32_t fseTableIndexPropagatedHufW = outBlockData.fseTableIndexHufW;
    uint32_t fseTableIndexPropagatedLLen = outBlockData.fseTableIndexLLen;
    uint32_t fseTableIndexPropagatedOffs = outBlockData.fseTableIndexOffs;
    uint32_t fseTableIndexPropagatedMLen = outBlockData.fseTableIndexMLen;

    if (0 != thisBlockIndex && (needsLookbackUniformHufW || needsLookbackUniformLLen || needsLookbackUniformOffs || needsLookbackUniformMLen))
    {
        uint32_t fseTableIndexLookbackHufW = outBlockData.fseTableIndexHufW;
        uint32_t fseTableIndexLookbackLLen = outBlockData.fseTableIndexLLen;
        uint32_t fseTableIndexLookbackOffs = outBlockData.fseTableIndexOffs;
        uint32_t fseTableIndexLookbackMLen = outBlockData.fseTableIndexMLen;
        if (WaveIsFirstLane())
        {
            uint32_t prevBlockIndexHufW = thisBlockIndex - 1u;
            uint32_t prevBlockIndexLLen = thisBlockIndex - 1u;
            uint32_t prevBlockIndexOffs = thisBlockIndex - 1u;
            uint32_t prevBlockIndexMLen = thisBlockIndex - 1u;

            bool needsLookbackLoopHufW = needsLookbackUniformHufW;
            bool needsLookbackLoopLLen = needsLookbackUniformLLen;
            bool needsLookbackLoopOffs = needsLookbackUniformOffs;
            bool needsLookbackLoopMLen = needsLookbackUniformMLen;

            while (needsLookbackLoopHufW || needsLookbackLoopLLen || needsLookbackLoopOffs || needsLookbackLoopMLen)
            {
                #define UPDATA_LOOKBACK_STATE(name)                                                                             \
                    if (needsLookbackLoop##name)                                                                                \
                    {                                                                                                           \
                        LOOKBACK_READ(name, prevBlockIndex##name);                                                              \
                        if (zstdgpu_Decode31BitLookbackFlags(lookbackIndex##name) > 0)                                          \
                        {                                                                                                       \
                            const uint32_t maskedLookbackIndex##name = zstdgpu_Decode31BitLookbackValue(lookbackIndex##name);   \
                            if (maskedLookbackIndex##name == kzstdgpu_FseProbTableIndex_Unused)                                 \
                            {                                                                                                   \
                                /* NOTE(pamartis): There's no useful data in the current `lookback`, so proceed to the next one (backward)*/\
                                /* If we reach the first `lookback` which always contain the correct indices for propagation,   */\
                                /* we terminate early without going into the next iteration                                     */\
                                if (prevBlockIndex##name == 0)                                                                  \
                                {                                                                                               \
                                    needsLookbackLoop##name = false;                                                            \
                                }                                                                                               \
                                else                                                                                            \
                                {                                                                                               \
                                    --prevBlockIndex##name;                                                                     \
                                }                                                                                               \
                                /* NOTE(pamartis): We found the `lookback` index in `Ready` state that is `Unused`,     */      \
                                /* and decremented the block index for lookback, and continue into the next iteration   */      \
                            }                                                                                                   \
                            else                                                                                                \
                            {                                                                                                   \
                                /* NOTE(pamartis): We found the `lookback` index in `Ready` state other than `Unused`*/         \
                                needsLookbackLoop##name = false;                                                                \
                                fseTableIndexLookback##name = maskedLookbackIndex##name;                                        \
                            }                                                                                                   \
                        }                                                                                                       \
                    }

                UPDATA_LOOKBACK_STATE(HufW)
                UPDATA_LOOKBACK_STATE(LLen)
                UPDATA_LOOKBACK_STATE(Offs)
                UPDATA_LOOKBACK_STATE(MLen)
                #undef UPDATA_LOOKBACK_STATE
                #undef LOOKBACK_READ
            }
        }

        // replace the first lane's "Repeat" values with the values from the last compressed block
        #define SET_PROPAGATED_FSE_INDEX_LOOKBACK(name)                                                             \
            if (needsLookbackUniform##name)                                                                         \
            {                                                                                                       \
                const uint32_t fseTableIndexLookbackUniform##name = WaveReadLaneFirst(fseTableIndexLookback##name); \
                if (needsLookback##name)                                                                            \
                    if (WaveIsFirstLane())                                                                          \
                        fseTableIndexPropagated##name = fseTableIndexLookbackUniform##name;                         \
            }

        SET_PROPAGATED_FSE_INDEX_LOOKBACK(HufW)
        SET_PROPAGATED_FSE_INDEX_LOOKBACK(LLen)
        SET_PROPAGATED_FSE_INDEX_LOOKBACK(Offs)
        SET_PROPAGATED_FSE_INDEX_LOOKBACK(MLen)

        #undef SET_PROPAGATED_FSE_INDEX_LOOKBACK
    }

    // NOTE(pamartis): Because the first lane containining "non-Unused" index was set to something other than `Repeat`,
    // we can propagate indices across the wave (if needed of course, if the wave needs that -- contains any number of lanes with `Repeat` indices)
    #define PROPAGATE_ACROSS_WAVE_IF_NEEDED(name)                                                                       \
        const bool needPropagateAcrossWave##name = fseTableIndexPropagated##name == kzstdgpu_FseProbTableIndex_Repeat;   \
        if (WaveActiveAnyTrue(needPropagateAcrossWave##name))                                                           \
        {                                                                                                               \
            uint32_t x = fseTableIndexPropagated##name;                                                                 \
            WAVE_PROPAGATE_STEP(x, 2)                                                                                   \
            WAVE_PROPAGATE_STEP(x, 4)                                                                                   \
            WAVE_PROPAGATE_STEP(x, 8)                                                                                   \
            WAVE_PROPAGATE_STEP(x, 16)                                                                                  \
            WAVE_PROPAGATE_STEP(x, 32)                                                                                  \
            if (needPropagateAcrossWave##name)                                                                          \
            {                                                                                                           \
                fseTableIndexPropagated##name = x;                                                                      \
            }                                                                                                           \
        }

    PROPAGATE_ACROSS_WAVE_IF_NEEDED(HufW)
    PROPAGATE_ACROSS_WAVE_IF_NEEDED(LLen)
    PROPAGATE_ACROSS_WAVE_IF_NEEDED(Offs)
    PROPAGATE_ACROSS_WAVE_IF_NEEDED(MLen)

    #undef PROPAGATE_ACROSS_WAVE_IF_NEEDED

    outBlockData.fseTableIndexHufW = fseTableIndexPropagatedHufW;
    outBlockData.fseTableIndexLLen = fseTableIndexPropagatedLLen;
    outBlockData.fseTableIndexOffs = fseTableIndexPropagatedOffs;
    outBlockData.fseTableIndexMLen = fseTableIndexPropagatedMLen;

    #undef WAVE_PROPAGATE_STEP
    #undef WAVE_BROADCAST
    #undef WAVE_SHUFFLE

#else
    // use static variables on CPU because this function is expected to be called in a loop for all compressed blocks
    static uint32_t lastHufWIndex = kzstdgpu_FseProbTableIndex_Unused;
    static uint32_t lastLLenIndex = kzstdgpu_FseProbTableIndex_Unused;
    static uint32_t lastOffsIndex = kzstdgpu_FseProbTableIndex_Unused;
    static uint32_t lastMLenIndex = kzstdgpu_FseProbTableIndex_Unused;

    #define PROPAGATE_FSE_HUF_INDEX(name) \
        if (outBlockData.fseTableIndex##name < kzstdgpu_FseProbTableIndex_Repeat)    \
        {                                                                           \
            last##name##Index = outBlockData.fseTableIndex##name;                   \
        }                                                                           \
        else if (outBlockData.fseTableIndex##name == kzstdgpu_FseProbTableIndex_Repeat)\
        {                                                                           \
            outBlockData.fseTableIndex##name = last##name##Index;                   \
        }

    PROPAGATE_FSE_HUF_INDEX(HufW)
    PROPAGATE_FSE_HUF_INDEX(LLen)
    PROPAGATE_FSE_HUF_INDEX(Offs)
    PROPAGATE_FSE_HUF_INDEX(MLen)

    #undef PROPAGATE_FSE_HUF_INDEX
#endif

    if (0 != seqCount)
    {
        srt.inoutSeqRefs[outBlockData.seqStreamIndex].fseLLen = outBlockData.fseTableIndexLLen;
        srt.inoutSeqRefs[outBlockData.seqStreamIndex].fseOffs = outBlockData.fseTableIndexOffs;
        srt.inoutSeqRefs[outBlockData.seqStreamIndex].fseMLen = outBlockData.fseTableIndexMLen;
        srt.inoutSeqRefs[outBlockData.seqStreamIndex].blockId = blockIndexInFrame;
    }

    if (0x1 < literalBlockType)
    {
        const uint32_t literalStreamCount = (0x0 == literalBlockSzFmt) ? 1 : 4;

#ifdef __hlsl_dx_compiler
        #if 0
        uint32_t literalStreamCountPerHuffmanTablePerWave = 0;

#define ENABLE_DXC_RECONVERGENCE_BUG_WORKAROUND 1

#if ENABLE_DXC_RECONVERGENCE_BUG_WORKAROUND
        bool isLaneEnabled = true;
        do
#else
        for (;;)
#endif
        {
            const uint32_t huffmanTableIndexUniform = WaveReadLaneFirst(outBlockData.fseTableIndexHufW);
            [branch] if (huffmanTableIndexUniform == outBlockData.fseTableIndexHufW)
            {
                const uint32_t literalStreamCountPerWaveThisHuffmanTable = WaveActiveSum(literalStreamCount);
                if (WaveIsFirstLane())
                {
                    //InterlockedAdd(srt.literalStreamCountPerHuffmanTable[huffmanTableIndexUniform], literalStreamCountPerWaveThisHuffmanTable);
                    literalStreamCountPerHuffmanTablePerWave = literalStreamCountPerWaveThisHuffmanTable;
                }
#if ENABLE_DXC_RECONVERGENCE_BUG_WORKAROUND
                isLaneEnabled = false;
#else
                break;
#endif
            }
        }
#if ENABLE_DXC_RECONVERGENCE_BUG_WORKAROUND
        while (isLaneEnabled);
#endif

        if (literalStreamCountPerHuffmanTablePerWave > 0)
        {
            InterlockedAdd(srt.literalStreamCountPerHuffmanTable[outBlockData.fseTableIndexHufW], literalStreamCountPerHuffmanTablePerWave);
        }
        #else
        uint32_t huffmanBucketOffset = 0;
        InterlockedAdd(srt.inoutLitStreamEndPerHuffmanTable[outBlockData.fseTableIndexHufW], literalStreamCount, huffmanBucketOffset);
        #endif
#else
        uint32_t huffmanBucketOffset = srt.inoutLitStreamEndPerHuffmanTable[outBlockData.fseTableIndexHufW];
        srt.inoutLitStreamEndPerHuffmanTable[outBlockData.fseTableIndexHufW] = huffmanBucketOffset + literalStreamCount;
#endif
        // NOTE(pamartis): when Huffman table indices are valid, update them for every compressed literal stream
        if (0x0 == literalBlockSzFmt)
        {
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex].huffmanBucketIndex  = outBlockData.fseTableIndexHufW;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex].huffmanBucketOffset = huffmanBucketOffset;
        }
        else
        {
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 0].huffmanBucketIndex = outBlockData.fseTableIndexHufW;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 0].huffmanBucketOffset= huffmanBucketOffset + 0;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 1].huffmanBucketIndex = outBlockData.fseTableIndexHufW;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 1].huffmanBucketOffset= huffmanBucketOffset + 1;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 2].huffmanBucketIndex = outBlockData.fseTableIndexHufW;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 2].huffmanBucketOffset= huffmanBucketOffset + 2;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 3].huffmanBucketIndex = outBlockData.fseTableIndexHufW;
            srt.inoutLitStreamBuckets[outBlockData.litStreamIndex + 3].huffmanBucketOffset= huffmanBucketOffset + 3;
        }
    }

    srt.inoutCompressedBlocks[threadId] = outBlockData;
}

// LDS partitioning macro list for FSE Table Initialisation (default shader)
#define ZSTDGPU_INIT_FSE_TABLE_LDS_DEFAULT(base, size)                                                      \
    ZSTDGPU_LDS_SIZE(size)                                                                                  \
    ZSTDGPU_LDS_BASE(base)                                                                                  \
    ZSTDGPU_LDS_REGION(CompactedPositiveFrqPrefixSumAndSymbols  , kzstdgpu_MaxCount_FseProbs)               \
    ZSTDGPU_LDS_REGION(SymbolBitMasks                           , kzstdgpu_MaxCount_FseElemsAllDigitBits)

// LDS partitioning macro list for FSE Table Initialisation (experimental shader)
#define ZSTDGPU_INIT_FSE_TABLE_LDS_EXPERIMENTAL(base, size)                                                 \
    ZSTDGPU_LDS_SIZE(size)                                                                                  \
    ZSTDGPU_LDS_BASE(base)                                                                                  \
    ZSTDGPU_LDS_REGION(CompactedPositiveFrqPrefixSumAndSymbols  , kzstdgpu_MaxCount_FseElems)               \
    ZSTDGPU_LDS_REGION(SymbolShuffleScratch                     , kzstdgpu_MaxCount_FseElems)               \
    ZSTDGPU_LDS_REGION(SymbolBitMasks                           , kzstdgpu_MaxCount_FseElemsOneDigitBits * 2)

// LDS partitioning macro list tail for FSE Table Initialisation (when threadgroup contains multiple waves)
#define ZSTDGPU_INIT_FSE_TABLE_LDS_MULTI_WAVE()                                                             \
    ZSTDGPU_LDS_REGION(PerWaveDword0                            , kzstdgpu_WaveCountMax_InitFseTable)       \
    ZSTDGPU_LDS_REGION(PerWaveDword1                            , kzstdgpu_WaveCountMax_InitFseTable)       \
    ZSTDGPU_LDS_REGION(PerWaveDword2                            , kzstdgpu_WaveCountMax_InitFseTable)       \
    ZSTDGPU_LDS_REGION(PerGroupDword0                           , 1)                                        \
    ZSTDGPU_LDS_REGION(PerGroupDword1                           , 1)                                        \
    ZSTDGPU_LDS_REGION(PerGroupDword2                           , 1)

#ifndef IS_MULTI_WAVE
#define IS_MULTI_WAVE 0
#define IS_MULTI_WAVE_UNDEF 1
#endif

#include "zstdgpu_lds_decl_size.h"
ZSTDGPU_INIT_FSE_TABLE_LDS_DEFAULT(0, InitFseTable_Default)
#if IS_MULTI_WAVE
ZSTDGPU_INIT_FSE_TABLE_LDS_MULTI_WAVE()
#endif
;

ZSTDGPU_INIT_FSE_TABLE_LDS_EXPERIMENTAL(0, InitFseTable_Experimental)
#if IS_MULTI_WAVE
ZSTDGPU_INIT_FSE_TABLE_LDS_MULTI_WAVE()
#endif
;

#include "zstdgpu_lds_decl_undef.h"

static void zstdgpu_ShaderEntry_InitFseTable(ZSTDGPU_PARAM_INOUT(zstdgpu_InitFseTable_SRT) srt, uint32_t groupId, uint32_t i)
{

#ifndef ZSTD_BITCNT_NSTATE_METHOD_REFERENCE
#define ZSTD_BITCNT_NSTATE_METHOD_REFERENCE 0
#define ZSTD_BITCNT_NSTATE_METHOD_REFERENCE_UNDEF 1
#endif

#ifndef ZSTD_BITCNT_NSTATE_METHOD_DEFAULT
#define ZSTD_BITCNT_NSTATE_METHOD_DEFAULT 1
#define ZSTD_BITCNT_NSTATE_METHOD_DEFAULT_UNDEF 1
#endif

#ifndef ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL
#define ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL 2  // doesn't work on XBOX for now
#define ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL_UNDEF 1
#endif

#ifndef ZSTD_BITCNT_NSTATE_METHOD
#define ZSTD_BITCNT_NSTATE_METHOD ZSTD_BITCNT_NSTATE_METHOD_DEFAULT
#define ZSTD_BITCNT_NSTATE_METHOD_UNDEF 1
#endif

    const uint32_t laneCnt = zstdgpu_MinU32(kzstdgpu_TgSizeX_InitFseTable, WaveGetLaneCount());
    // NOTE(pamartis): the above is important because `kzstdgpu_TgSizeX_DecompressLiterals` is fixed,
    // so WaveGetLaneCount() could be > `kzstdgpu_TgSizeX_DecompressLiterals`
    #ifdef __hlsl_dx_compiler
    const uint32_t waveCnt = kzstdgpu_TgSizeX_InitFseTable / laneCnt;
    #else
    const uint32_t waveCnt = 1;
    #endif

    // NOTE(pamartis): Use `WaveReadLaneFirst` to make sure `waveIdx` is wave-uniform
    const uint32_t waveIdx = WaveReadLaneFirst(i / laneCnt);
    //const uint32_t laneIdx = i % laneCnt;

    #include "zstdgpu_lds_decl_base.h"
    #if (ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_DEFAULT) || (ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_REFERENCE)
        ZSTDGPU_INIT_FSE_TABLE_LDS_DEFAULT(0, InitFseTable_Default)
    #elif ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL
        ZSTDGPU_INIT_FSE_TABLE_LDS_EXPERIMENTAL(0, InitFseTable_Experimental)
    #endif
    #if IS_MULTI_WAVE
        ZSTDGPU_INIT_FSE_TABLE_LDS_MULTI_WAVE()
    #endif
    #include "zstdgpu_lds_decl_undef.h"

    const uint32_t tableIndex    = srt.tableStartIndex + groupId;
    const uint32_t frqDataOffset = tableIndex * kzstdgpu_MaxCount_FseProbs;
    const uint32_t tblDataOffset = srt.tableDataStart + groupId * srt.tableDataCount;

    const zstdgpu_FseInfo fseInfo = srt.inFseInfos[kzstdgpu_FseRleTableCount + tableIndex];

    const uint32_t accuracyLog2 = (fseInfo.fseProbCountAndAccuracyLog2 >> 8u);

    // clamp the number of symbols/per symbol frequencies (by the spec)
    const uint32_t frqDataCount  = zstdgpu_MinU32(fseInfo.fseProbCountAndAccuracyLog2 & 0xff, kzstdgpu_MaxCount_FseProbs);

    const uint32_t tblDataCount  = 1u << accuracyLog2;

    // make sure `tblDataCount` is never `> kzstdgpu_MaxCount_FseElems`
    //      9 - for Literal Length and Match Length FSEs
    //      8 - for Offsets FSE
    //      7 - for Huffman Weight FSE
    const uint32_t tblAllDataCount = zstdgpu_MinU32(tblDataCount, kzstdgpu_MaxCount_FseElems);

    // NB: compute the number of symbols/per symbol freqeuncies aligned up to threadgroup size.
    //      It's necessary to make sure sure `GroupMemoryBarrierWithGroupSync` are respected by the compiler
    //      and `groupshared` variable that are uniform across threadgroups -- remain uniform
    //      Otherwise, reading such variable (`PerGroupDwordN` for example) produces different results
    //      for different groups
    const uint32_t frqDataCountAligned = ZSTDGPU_TG_MULTIPLE(frqDataCount, kzstdgpu_TgSizeX_InitFseTable);

    const uint32_t tblAllDataCountAligned = ZSTDGPU_TG_MULTIPLE(tblAllDataCount, kzstdgpu_TgSizeX_InitFseTable);

    // initialize the index in the `table` array where symbols with negative frequencies are stored
    uint32_t negativeFrqSymStart = tblAllDataCount;
    uint32_t positiveFrqSymCount = 0;
    uint32_t positiveFrqSumTotal = 0;
    uint32_t positiveFrqSymIndexCount = 0;

#if IS_MULTI_WAVE
    if (i == 0)
    {
        zstdgpu_LdsStoreU32(GS_PerGroupDword0, negativeFrqSymStart);
        zstdgpu_LdsStoreU32(GS_PerGroupDword1, positiveFrqSymCount);
        zstdgpu_LdsStoreU32(GS_PerGroupDword2, positiveFrqSumTotal);
    }
#endif

    // distribute all symbols/per symbol frequencies to threads as evenly as possible
    //      iterations count per loop: `(frqDataCount + kzstdgpu_TgSizeX_InitFseTable - 1) / kzstdgpu_TgSizeX_InitFseTable`
    //      `frqDataCount % kzstdgpu_TgSizeX_InitFseTable` threads process `(frqDataCount + kzstdgpu_TgSizeX_InitFseTable - 1) / kzstdgpu_TgSizeX_InitFseTable` symbols/per symbol frequencies
    //      `kzstdgpu_TgSizeX_InitFseTable - frqDataCount % kzstdgpu_TgSizeX_InitFseTable` threads process `frqDataCount / kzstdgpu_TgSizeX_InitFseTable` symbols/per symbol frequencies
    ZSTDGPU_FOR_WORK_ITEMS(symbol, frqDataCountAligned, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        int frq = 0;
        if (symbol < frqDataCount)
        {
            frq = srt.inFseProbs[frqDataOffset + symbol];
        }

        const bool isNegativeFrq = frq == -1;
        const bool isPositiveFrq = frq > 0;

        // NOTE: we can cast to `uint32_t` type here because we check for `frq > 0`
        const uint32_t positiveFrq = isPositiveFrq ? uint32_t(frq) : 0;

        // compute "per wave" aggegates of desired quantities:
        //      - the number of `=-1` frequencies
        //      - the number of `> 0` frequencies
        //      - the sum of `> 0` frequencies
        const uint32_t WaveWide_NegativeFrqCnt = WaveActiveCountBits(isNegativeFrq);
        const uint32_t WaveWide_PositiveFrqCnt = WaveActiveCountBits(isPositiveFrq);
        const uint32_t WaveWide_PositiveFrqSum = WaveActiveSum(positiveFrq);

#if IS_MULTI_WAVE
        // NOTE: wait for `PerGroupDwordN` to be updated by either
        //      a. write from initialisation step before the loop
        //      b. write from the previous iteration
        GroupMemoryBarrierWithGroupSync();

        // store "per wave" aggregates of desired quantities to LDS
        if (WaveIsFirstLane())
        {
            zstdgpu_LdsStoreU32(GS_PerWaveDword0 + waveIdx, WaveWide_NegativeFrqCnt);
            zstdgpu_LdsStoreU32(GS_PerWaveDword1 + waveIdx, WaveWide_PositiveFrqCnt);
            zstdgpu_LdsStoreU32(GS_PerWaveDword2 + waveIdx, WaveWide_PositiveFrqSum);
        }

        // load "per group" aggregates from previous iteration of desired quantities from LDS
        uint32_t This_Wave_NegativeFrqSymStart = zstdgpu_LdsLoadU32(GS_PerGroupDword0);
        uint32_t This_Wave_PositiveFrqSymCount = zstdgpu_LdsLoadU32(GS_PerGroupDword1);
        uint32_t This_Wave_PositiveFrqSumStart = zstdgpu_LdsLoadU32(GS_PerGroupDword2);

        // wait for all waves to write "per-wave" data to LDS and read "per-group" data
        GroupMemoryBarrierWithGroupSync();

        // use "per group" aggregates from previous iteration and "per wave" aggregates from current iteration
        // to get "per wave" exclusive prefix
        // NOTE: every wave does a bit of repeating work to compute accumulate per-wave aggregates from prior waves
        for (uint32_t wave = 0; wave < waveIdx; ++wave)
        {
            This_Wave_NegativeFrqSymStart -= zstdgpu_LdsLoadU32(GS_PerWaveDword0 + wave);
            This_Wave_PositiveFrqSymCount += zstdgpu_LdsLoadU32(GS_PerWaveDword1 + wave);
            This_Wave_PositiveFrqSumStart += zstdgpu_LdsLoadU32(GS_PerWaveDword2 + wave);
        }

        const uint32_t Candidate_BlockWide_NegativeFrqSymStart = This_Wave_NegativeFrqSymStart - WaveWide_NegativeFrqCnt;
        const uint32_t Candidate_BlockWide_PositiveFrqSymCount = This_Wave_PositiveFrqSymCount + WaveWide_PositiveFrqCnt;
        const uint32_t Candidate_BlockWide_PositiveFrqSumStart = This_Wave_PositiveFrqSumStart + WaveWide_PositiveFrqSum;

        #if 0
        // NB: the Last Thread updates "per group" aggregates for the next iteration
        if (i == kzstdgpu_TgSizeX_InitFseTable - 1 || symbol == frqDataCount - 1)
        {
            PerGroupDword0 = Candidate_BlockWide_NegativeFrqSymStart;
            PerGroupDword1 = Candidate_BlockWide_PositiveFrqSymCount;
            PerGroupDword2 = Candidate_BlockWide_PositiveFrqSumStart;
        }
        #else
        // NB: the First Lane of each surviving lane updates "per group" aggregates for the next iteration
        //
        // We introduced this implementation to avoid computing proper index of the last active thread
        // after some of threads get disabled in `for` loop exit condition `symbol < frqDataCount`
        //
        // TODO: think whether HLSL can be improved. Surely it's would be simpler if we something like `WaveGetLastLaneIndex`
        //       but we would need to know the index of the last wave
        if (WaveIsFirstLane())
        {
            zstdgpu_LdsAtomicMinU32(GS_PerGroupDword0, Candidate_BlockWide_NegativeFrqSymStart);
            zstdgpu_LdsAtomicMaxU32(GS_PerGroupDword1, Candidate_BlockWide_PositiveFrqSymCount);
            zstdgpu_LdsAtomicMaxU32(GS_PerGroupDword2, Candidate_BlockWide_PositiveFrqSumStart);
        }
        #endif
#else
        // load "per group == per wave" aggregates from previous iteration of desired quantities from the first acitive lane
        // because reading it without
        uint32_t This_Wave_NegativeFrqSymStart = negativeFrqSymStart;
        uint32_t This_Wave_PositiveFrqSymCount = positiveFrqSymCount;
        uint32_t This_Wave_PositiveFrqSumStart = positiveFrqSumTotal;

        negativeFrqSymStart -= WaveWide_NegativeFrqCnt;
        positiveFrqSymCount += WaveWide_PositiveFrqCnt;
        positiveFrqSumTotal += WaveWide_PositiveFrqSum;
#endif

        // get "per-element" exclusive prefix by using by using "per-wave" exclusive prefix
        if (isNegativeFrq) // we don't check for `symbol < frqDataCount` because alignment tail contains` frq == 0`
        {
            const uint32_t negativeFrqSymIndex = This_Wave_NegativeFrqSymStart - WavePrefixCountBits(true) - 1;

            // NB: while there're might be few symbols per loop iteration,
            // we store them directly to the buffer instead of storing them to LDS temporally which would be the right
            // thing to do to help coalesce memory writes at a later point when LDS would be written to the buffer.
            //
            // This is to avoid temporary LDS memory use (up to 512 bytes or, rather, 512 dwords because HLSL doesn't have 8-bit types and we don't want to use atomics)
            // So on Scarlett it increases the occupancy which helps the performance
            srt.inoutFseElems[tblDataOffset + negativeFrqSymIndex] = zstdgpu_PackFseElem(symbol, 0, 0);

            // NOTE: below is mainly to make sure `frqDataCount` elements are valid
            //GS_CompactedPositiveFrqPrefixSumAndSymbols[negativeFrqSymIndex] = (symbol << 24) | 0xffffff;
        }

        if (isPositiveFrq) // we don't check for `symbol < frqDataCount` because alignment tail contains` frq == 0`
        {
            const uint32_t positiveFrqSymIndex = This_Wave_PositiveFrqSymCount + WavePrefixCountBits(true);
            const uint32_t positiveFrqPrefix = This_Wave_PositiveFrqSumStart + WavePrefixSum(positiveFrq);

            zstdgpu_LdsStoreU32(GS_CompactedPositiveFrqPrefixSumAndSymbols + positiveFrqSymIndex, (symbol << 24) | positiveFrqPrefix);
        }
    }

#if IS_MULTI_WAVE
    // wait for all waves to write "per-wave" data to LDS and read "per-group" data
    GroupMemoryBarrierWithGroupSync();

    negativeFrqSymStart = zstdgpu_LdsLoadU32(GS_PerGroupDword0);
    positiveFrqSymCount = zstdgpu_LdsLoadU32(GS_PerGroupDword1);
    positiveFrqSumTotal = zstdgpu_LdsLoadU32(GS_PerGroupDword2);

    GroupMemoryBarrierWithGroupSync();

    zstdgpu_LdsStoreU32(GS_PerGroupDword1, positiveFrqSymIndexCount);
#endif

    const uint32_t symIndexStep = (tblAllDataCount >> 1) + (tblAllDataCount >> 3) + 3;
    const uint32_t symIndexMask = tblAllDataCount - 1;

    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCountAligned, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t positiveFrqSymIndex = (workItemId * symIndexStep) & symIndexMask;
        const bool isPositiveFrqSymIndex = positiveFrqSymIndex < negativeFrqSymStart;

#if IS_MULTI_WAVE
        // compute per wave aggegates of desired quantities
        uint32_t WaveWide_PositiveFrqSymIndexCnt = WaveActiveCountBits(isPositiveFrqSymIndex);

        // NOTE: wait for `PerGroupDwordN` to be updated by either
        //      a. write from initialisation step before the loop
        //      b. write from the previous iteration
        GroupMemoryBarrierWithGroupSync();

        // store "per wave" aggregates of desired quantities to LDS
        if (WaveIsFirstLane())
        {
            zstdgpu_LdsStoreU32(GS_PerWaveDword1 + waveIdx, WaveWide_PositiveFrqSymIndexCnt);
        }

        // load "per group" aggregates from previous iteration of desired quantities from LDS
        uint32_t This_Wave_PositiveFrqSymIndexStart = zstdgpu_LdsLoadU32(GS_PerGroupDword1);

        // wait for all waves to write "per-wave" data to LDS and read "per-group" data
        GroupMemoryBarrierWithGroupSync();

        // use "per group" aggregates from previous iteration and "per wave" aggregates from current iteration
        // to get "per wave" exclusive prefix
        // NOTE: every wave does a bit of repeating work to compute accumulate per-wave aggregates from prior waves
        for (uint32_t wave = 0; wave < waveIdx; ++wave)
        {
            This_Wave_PositiveFrqSymIndexStart += zstdgpu_LdsLoadU32(GS_PerWaveDword1 + wave);
        }
#else
        // load "per group == per wave" aggregates from previous iteration of desired quantities from Lane 0
        uint32_t This_Wave_PositiveFrqSymIndexStart = positiveFrqSymIndexCount;
#endif
        const uint32_t positiveFrqSymIndexStart = This_Wave_PositiveFrqSymIndexStart + WavePrefixCountBits(isPositiveFrqSymIndex);

        positiveFrqSymIndexCount = This_Wave_PositiveFrqSymIndexStart + WaveActiveCountBits(isPositiveFrqSymIndex && positiveFrqSymIndexStart < positiveFrqSumTotal);

#if IS_MULTI_WAVE
        if (WaveIsFirstLane())
        {
            zstdgpu_LdsAtomicMaxU32(GS_PerGroupDword1, positiveFrqSymIndexCount);
        }
#endif
        if (isPositiveFrqSymIndex && positiveFrqSymIndexStart < positiveFrqSumTotal)
        {
            // NB: we expect high percentage of surviving threads/lanes in this branch, therefore we avoid storing `positiveFrqSymIndex`
            // to LDS temporaly:
            //      LDS[positiveFrqSymIndexStart] = positiveFrqSymIndex;
            // to avoid higher LDS usage
            //
            uint32_t compactedSymIndex = 0;
            uint32_t compactedSymCount = positiveFrqSymCount;
            // NOTE(pamartis): We have found a starting index in the final FSE table -- `positiveFrqSymIndexStart`
            // but we don't know what symbol to assign to it yet, so we do binary search to find the starting position of a symbol
            // that will be assigned to `positiveFrqSymIndexStart` in the final FSE table.
            while (compactedSymCount > 1)
            {
                const uint32_t compactedSymIndexOfs = compactedSymCount >> 1;
                const uint32_t compactedSymIndexNxt = compactedSymIndex + compactedSymIndexOfs;

                const uint32_t compactedSymFrqPrefixSum = zstdgpu_LdsLoadU32(GS_CompactedPositiveFrqPrefixSumAndSymbols + compactedSymIndexNxt) & 0x00ffffff;

                compactedSymIndex = positiveFrqSymIndexStart < compactedSymFrqPrefixSum ? compactedSymIndex : compactedSymIndexNxt;
                compactedSymCount -= compactedSymIndexOfs;
            }
            const uint32_t prefixAndSymbol = zstdgpu_LdsLoadU32(GS_CompactedPositiveFrqPrefixSumAndSymbols + compactedSymIndex);

            const uint32_t symbol = prefixAndSymbol >> 24;
            //const uint32_t prefix = prefixAndSymbol & 0x00ffffff;

            srt.inoutFseElems[tblDataOffset + positiveFrqSymIndex] = zstdgpu_PackFseElem(symbol, 0, 0);
        }
    }

    // Problem: After storing "symbols", we also need to compute, for all sequences with identical "symbols", indices of elements within every sequence.
    //
    // Example:
    //          Given a sequence of symbols:                1 2 3 1 2 2 1 1 4 3 3 1
    //              - after computing indices for "1" :     0 x x 1 x x 2 3 x x x 4
    //              - after computing indices for "2" :     x 0 x x 1 2 x x x x x x
    //
    // Option 1:    For every unique symbol, we can compute a cross-TG prefix sum of counts of symbols per wave, but we need a histogram storage per wave.
    //              NOT IMPLEMENTED.
    //
    // Issue:       We need 256 counters per wave. Because we have to reserve LDS space conservatively for worst-case number of waves
    //              which occurs when WaveSize=4 or 8 lanes, the total number of counters becomes 256 * MaxWaves[kzstdgpu_TgSizeX_InitFseTable / {4,8})]
    //              meaning for TG with 128 threads, we need {8192,4096} counters, for TG with 64 threads we need {4096,2048}
    //
    // Option 2:    REFERENCE. We can simply iterate over all symbols and check what symbol and update counter (per thread)
    //
    // Issue:       It's super slow because only 1 thread does all work in every iteration, and there're too many iterations (tblAllDataCount * (frqDataCount + NUM_THREAD - 1) / kzstdgpu_TgSizeX_InitFseTable)
    //
    // Option 3:    DEFAULT: We transpose binary representation of symbols and store them to LDS as bitmasks. The we compose bitmasks of previous in all ranks to get a global
    //              bitmask storing the locations of symbols in the final FSE table. We count bits in that bitmask to get the prefix sum for each symbol to get its position.
    //
    // Issue:       It's somewhat VALU heavy as of now as farther symbols need to go over a longer chain of bits to get the prefix.
    // Advantage:   Relatively low LDS consumption to store bitmasks and doesn't require a too much group barriers.
    //
    // Option 4:    EXPERIMENTAL. We do per bit radix sort.
    //
    // Issues:      Relatively large LDS consumption. A lot of group barrier.
    //

#if ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_REFERENCE
    GroupMemoryBarrierWithGroupSync();

    ZSTDGPU_FOR_WORK_ITEMS(symbol, frqDataCountAligned, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        int counter = 0;
        if (symbol < frqDataCount)
        {
            counter = srt.inFseProbs[frqDataOffset + symbol];

            if (counter == -1)
            {
                counter = 1;
            }

            for (uint32_t j = 0; j < tblAllDataCount; ++j)
            {
                if (symbol == zstdgpu_FseElem_Symbol(srt.inoutFseElems[tblDataOffset + j]))
                {
                    uint32_t nstate = counter ++;
                    uint32_t bitcnt = accuracyLog2 - zstdgpu_FindFirstBitHiU32(nstate);

                    nstate <<= bitcnt;
                    nstate  -= tblAllDataCount;

                    srt.inoutFseElems[tblDataOffset + j] = zstdgpu_PackFseElem(symbol, bitcnt, nstate);
                }
            }
        }
    }
#elif ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL

    GroupMemoryBarrierWithGroupSync(); // NOTE(pamartis): wait for all wave to finish reading `GS_CompactedPositiveFrqPrefixSumAndSymbols`

    const uint32_t tblAllDataMaskCount = (tblAllDataCount + 31u) >> 5u;
    uint32_t waveOffset = 0;

    #define StoreGroupBitMask(storage, waveId, bitIdx, VGPR)                       \
        uint32_t4 b = WaveActiveBallot((VGPR & (1u << bitIdx)) != 0);                  \
        if (WaveGetLaneCount() <= 16)                                              \
        {                                                                          \
            const uint32_t masksPerUInt = 32 / WaveGetLaneCount();                     \
            const uint32_t uintIdx = (waveId) / masksPerUInt;                          \
            const uint32_t uintOfs = (waveId) % masksPerUInt;                          \
            const uint32_t laneOfs = uintOfs * WaveGetLaneCount();                     \
            InterlockedOr(storage[uintIdx], b.x << laneOfs);                       \
        }                                                                          \
        else if (WaveGetLaneCount() == 32)                                         \
        {                                                                          \
            storage[(waveId)] = b.x;                                               \
        }                                                                          \
        else if (WaveGetLaneCount() == 64)                                         \
        {                                                                          \
            storage[(waveId) * 2 + 0] = b.x;                                       \
            storage[(waveId) * 2 + 1] = b.y;                                       \
        }                                                                          \
        /* for some GPUs with 128-wide waves, store extra masks*/                  \
        else                                                                       \
        {                                                                          \
            storage[(waveId) * 4 + 0] = b.x;                                       \
            storage[(waveId) * 4 + 1] = b.y;                                       \
            storage[(waveId) * 4 + 2] = b.z;                                       \
            storage[(waveId) * 4 + 3] = b.w;                                       \
        }


    #if kzstdgpu_TgSizeX_InitFseTable < kzstdgpu_MaxCount_FseElemsOneDigitBits
        #error This kernel requires at least `kzstdgpu_MaxCount_FseElemsOneDigitBits` threads per threadgroup.
    #endif

    #define COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, activeLaneCntPerWave, laneStart, remainMaskCount)            \
        if (remainMaskCount > 0)                                                                                        \
        {                                                                                                               \
        uint32_t maskCount = min(activeLaneCntPerWave, remainMaskCount);                                                    \
        uint32_t maskStart = laneIdx + laneStart;                                                                           \
        uint32_t oneCount_##idx = 0;                                                                                        \
        if (maskStart < maskCount)                                                                                        \
        {                                                                                                               \
            oneCount_##idx = zstdgpu_CountBitsU32(storage[maskStart]);                                                   \
        }                                                                                                               \
        const uint32_t oneCount_##idx##_Prefix = WavePrefixSum(oneCount_##idx);                                             \
        if (waveIdx == 0)                                                                                               \
        {                                                                                                               \
            storage[kzstdgpu_MaxCount_FseElemsOneDigitBits * 1 + maskStart] = oneCountPerTG + oneCount_##idx##_Prefix;       \
        }\
        oneCountPerTG += WaveReadLaneAt(oneCount_##idx##_Prefix, maskCount - 1) + WaveReadLaneAt(oneCount_##idx, maskCount - 1);  \
        remainMaskCount -= maskCount;   \
        laneStart += maskCount;         \
        }

    #define COMPUTE_SHARED_BIT_PREFIX(storage)                          \
        oneCountPerTG = 0;                                              \
        zeroCountPerTG = 0;                                             \
        if (WaveGetLaneCount() == 4)                                    \
        {                                                               \
            /* iterate 4 times processing 4 `uint32_t` masks at a time*/    \
            uint32_t remainMaskCount = tblAllDataMaskCount;                 \
            uint32_t laneStart = 0;                                         \
            COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, 4, laneStart, remainMaskCount);\
            COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, 4, laneStart, remainMaskCount);\
            COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, 4, laneStart, remainMaskCount);\
            COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, 4, laneStart, remainMaskCount);\
        }                                                               \
        else if (WaveGetLaneCount() == 8)                                    \
        {                                                               \
            uint32_t remainMaskCount = tblAllDataMaskCount;                 \
            uint32_t laneStart = 0;                                         \
            COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, 8, laneStart, remainMaskCount);\
            COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, 8, laneStart, remainMaskCount);\
        }                                                               \
        else if (WaveGetLaneCount() >= 16)                                   \
        {                                                               \
            uint32_t remainMaskCount = tblAllDataMaskCount;                 \
            uint32_t laneStart = 0;                                         \
            COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT(storage, kzstdgpu_MaxCount_FseElemsOneDigitBits, laneStart, remainMaskCount);\
        }

    #define SHUFFLE_DATA(dstDataStorage, srcDataStorage, srcSharedBitmask, bitIdx)  \
        ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)        \
        {                                                                                       \
            const uint32_t symbolAndIndex = srcDataStorage[workItemId];                             \
            const uint32_t bit = (symbolAndIndex >> bitIdx) & 0x1u;                                 \
            const uint32_t uintId = workItemId >> 5u;                                               \
            const uint32_t elemId = workItemId & 31u;                                               \
            const uint32_t uintMask = (1u << elemId) - 1u;                                          \
            const uint32_t oneCount = zstdgpu_CountBitsU32(srcSharedBitmask[kzstdgpu_MaxCount_FseElemsOneDigitBits * 0 + uintId] & uintMask);\
            const uint32_t onePrefix = srcSharedBitmask[kzstdgpu_MaxCount_FseElemsOneDigitBits * 1 + uintId];           \
            const uint32_t oneOffset = onePrefix + oneCount;                                        \
            uint32_t dstIndex = 0;                                                                  \
            if (bit)                                                                                \
            {                                                                                       \
                dstIndex = oneOffset;                                                               \
            }                                                                                       \
            else                                                                                    \
            {                                                                                       \
                dstIndex = oneCountPerTG + workItemId - oneOffset;                                  \
            }                                                                                       \
            dstDataStorage[dstIndex] = symbolAndIndex;                                              \
            /*DEBUG:dstDataStorage[workItemId] = symbolAndIndex | (dstIndex << 20);*/               \
        }

    #define COMPUTE_SHARED_BIT_MASK(dstSharedBitmask, srcDataStorage, bitIdx)                   \
        ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)  \
        {                                                                                       \
            const uint32_t symbolAndIndex = srcDataStorage[workItemId];                         \
            const uint32_t bit = (symbolAndIndex >> bitIdx) & 0x1u;                             \
            StoreGroupBitMask(dstSharedBitmask, waveIdx + waveOffset, bitIdx, symbolAndIndex);  \
        }

    uint32_t waveOffset = 0;
    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t symbol = zstdgpu_FseElem_Symbol(srt.inoutFseElems[tblDataOffset + workItemId]);

        const uint32_t symbolAndIndex = symbol | (workItemId << 8);
        GS_CompactedPositiveFrqPrefixSumAndSymbols[workItemId] = symbolAndIndex;
        StoreGroupBitMask(GS_SymbolBitMasks, waveIdx + waveOffset, 0, symbolAndIndex);
        waveOffset += waveCnt;
    }

    GroupMemoryBarrierWithGroupSync();

    uint32_t oneCountPerTG = 0;
    uint32_t zeroCountPerTG = 0;
    COMPUTE_SHARED_BIT_PREFIX(GS_SymbolBitMasks)
    GroupMemoryBarrierWithGroupSync();
    SHUFFLE_DATA(GS_SymbolShuffleScratch, GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolBitMasks, 0)
    GroupMemoryBarrierWithGroupSync(); // NOTE(pamartis): Finish reading destination positions from GS_SymbolBitMasks and writing GS_SymbolShuffleScratch

    #define SHUFFLE_STAGE(dstDataStorage, srcDataStorage, srcdstSharedBitmasks, bitIdx) \
        COMPUTE_SHARED_BIT_MASK(srcdstSharedBitmasks, srcDataStorage, bitIdx)           \
        GroupMemoryBarrierWithGroupSync();                                              \
        COMPUTE_SHARED_BIT_PREFIX(srcdstSharedBitmasks)                                 \
        GroupMemoryBarrierWithGroupSync();                                              \
        SHUFFLE_DATA(dstDataStorage, srcDataStorage, srcdstSharedBitmasks, bitIdx)      \
        GroupMemoryBarrierWithGroupSync();

    SHUFFLE_STAGE(GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolShuffleScratch, GS_SymbolBitMasks, 1)
    SHUFFLE_STAGE(GS_SymbolShuffleScratch, GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolBitMasks, 2)
    SHUFFLE_STAGE(GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolShuffleScratch, GS_SymbolBitMasks, 3)
    SHUFFLE_STAGE(GS_SymbolShuffleScratch, GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolBitMasks, 4)
    SHUFFLE_STAGE(GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolShuffleScratch, GS_SymbolBitMasks, 5)
    SHUFFLE_STAGE(GS_SymbolShuffleScratch, GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolBitMasks, 6)
    SHUFFLE_STAGE(GS_CompactedPositiveFrqPrefixSumAndSymbols, GS_SymbolShuffleScratch, GS_SymbolBitMasks, 7)

    #undef SHUFFLE_STAGE
    #undef COMPUTE_SHARED_BIT_MASK
    #undef SHUFFLE_DATA
    #undef COMPUTE_SHARED_BIT_PREFIX
    #undef COMPUTE_1_0_PREFIX_AND_UPDATE_1_COUNT
    #undef StoreGroupBitMask

    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t thisSymbol = GS_CompactedPositiveFrqPrefixSumAndSymbols[workItemId] & 0xffu;

        // NOTE(pamartis): store first workItemId for every symbol
        if (workItemId == 0)
        {
            GS_SymbolShuffleScratch[thisSymbol] = 0;
        }
        else
        {
            const uint32_t prevSymbol = GS_CompactedPositiveFrqPrefixSumAndSymbols[workItemId - 1] & 0xffu;
            if (prevSymbol != thisSymbol)
            {
                GS_SymbolShuffleScratch[thisSymbol] = workItemId;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t symbolAndIndex = GS_CompactedPositiveFrqPrefixSumAndSymbols[workItemId];
        const uint32_t symbol = symbolAndIndex & 0xffu;
        GS_CompactedPositiveFrqPrefixSumAndSymbols[workItemId] = symbolAndIndex | ((workItemId - GS_SymbolShuffleScratch[symbol]) << 20u);
    }
    GroupMemoryBarrierWithGroupSync();

    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t symbolAndIndex = GS_CompactedPositiveFrqPrefixSumAndSymbols[workItemId];
        ZstdTest[tblDataOffset + workItemId] = symbolAndIndex;
    }

    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t symbolAndIndex = GS_CompactedPositiveFrqPrefixSumAndSymbols[workItemId];
        const uint32_t oldIndex = (symbolAndIndex >> 8u) & 0x3ffu;
        GS_SymbolShuffleScratch[oldIndex] = symbolAndIndex;
    }
    GroupMemoryBarrierWithGroupSync();

    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t symbolAndIndex = GS_SymbolShuffleScratch[workItemId];
        const uint32_t symbol = symbolAndIndex & 0xffu;
        const uint32_t prefix = (symbolAndIndex >> 20u) & 0x3ffu;

        int prob = srt.inFseProbs[frqDataOffset + symbol];
        if (prob < 0)
            prob = 1;

        uint32_t nstate = prefix + (uint32_t)prob;
        uint32_t bitcnt = accuracyLog2 - zstdgpu_FindFirstBitHiU32(nstate);

        nstate <<= bitcnt;
        nstate  -= tblAllDataCount;

        srt.inoutFseElems[tblDataOffset + workItemId] = zstdgpu_PackFseElem(symbol, bitcnt, nstate);
    }

#elif ZSTD_BITCNT_NSTATE_METHOD == ZSTD_BITCNT_NSTATE_METHOD_DEFAULT

    // NOTE(pamartis): When the number of lanes in the wave <= 16, we need to initialize GS_SymbolBitMasks to zero
    // because we use InterlockedOr on each 32-bit GS_SymbolBitMasks[i] to store wave masks (see StoreGroupBitMask)...
    if (WaveGetLaneCount() <= 16)
    {
        ZSTDGPU_FOR_WORK_ITEMS(workItemId, kzstdgpu_MaxCount_FseElemsAllDigitBits, i, kzstdgpu_TgSizeX_InitFseTable)
        {
            zstdgpu_LdsStoreU32(GS_SymbolBitMasks + workItemId, 0);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // NOTE(pamartis): Transpose N=`tblAllDataCount` symbols (store only bits with index 0, then with index 1 and so on.)
    // and store the transposed representation in LDS (as eight `tblAllDataCount`-wide masks)
    uint32_t waveOfs = waveIdx;
    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t symbol = zstdgpu_FseElem_Symbol(srt.inoutFseElems[tblDataOffset + workItemId]);

        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 0);
        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 1);
        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 2);
        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 3);
        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 4);
        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 5);
        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 6);
        zstdgpu_GroupBallotLdsStore(laneCnt, symbol, GS_SymbolBitMasks, kzstdgpu_MaxCount_FseElemsOneDigitBits, waveOfs, 7);

        waveOfs += waveCnt;
    }
    GroupMemoryBarrierWithGroupSync();

    ZSTDGPU_FOR_WORK_ITEMS(workItemId, tblAllDataCount, i, kzstdgpu_TgSizeX_InitFseTable)
    {
        const uint32_t uintIdx = workItemId >> 5;
        const uint32_t uintOfs = workItemId & 0x1fu;

        const uint32_t symbol = zstdgpu_FseElem_Symbol(srt.inoutFseElems[tblDataOffset + workItemId]);

        #define FetchBitsAndAccumulateMask(mask, bits, storage, bitIdx, uintId) \
            bits = zstdgpu_LdsLoadU32(storage + kzstdgpu_MaxCount_FseElemsOneDigitBits * bitIdx + uintId);\
            /*mask &= (symbol & (1u << bitIdx)) != 0 ? bits : ~bits*/           \
            mask &= (((symbol >> bitIdx) & 0x1u) + ~0u) ^ bits;

        uint32_t prefix = 0;
        for (uint32_t uintId = 0; uintId < uintIdx; ++uintId)
        {
            uint32_t mask = ~0u;

            uint32_t bits = 0;
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 0, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 1, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 2, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 3, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 4, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 5, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 6, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 7, uintId);
            prefix += zstdgpu_CountBitsU32(mask);
        }
        uint32_t mask = (1u << uintOfs) - 1u;
        uint32_t bits = 0;
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 0, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 1, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 2, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 3, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 4, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 5, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 6, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_SymbolBitMasks, 7, uintIdx);
        #undef FetchBitsAndAccumulateMask
        prefix += zstdgpu_CountBitsU32(mask);

        int prob = srt.inFseProbs[frqDataOffset + symbol];
        if (prob < 0)
            prob = 1;

        uint32_t nstate = prefix + (uint32_t)prob;
        uint32_t bitcnt = accuracyLog2 - zstdgpu_FindFirstBitHiU32(nstate);

        nstate <<= bitcnt;
        nstate  -= tblAllDataCount;

        srt.inoutFseElems[tblDataOffset + workItemId] = zstdgpu_PackFseElem(symbol, bitcnt, nstate);
    }

#endif

#ifdef IS_MULTI_WAVE_UNDEF
#undef IS_MULTI_WAVE_UNDEF
#undef IS_MULTI_WAVE
#endif

#ifdef ZSTD_BITCNT_NSTATE_METHOD_UNDEF
#undef ZSTD_BITCNT_NSTATE_METHOD_UNDEF
#undef ZSTD_BITCNT_NSTATE_METHOD
#endif

#ifdef ZSTD_BITCNT_NSTATE_METHOD_REFERENCE_UNDEF
#undef ZSTD_BITCNT_NSTATE_METHOD_REFERENCE_UNDEF
#undef ZSTD_BITCNT_NSTATE_METHOD_REFERENCE
#endif

#ifdef ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL_UNDEF
#undef ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL
#undef ZSTD_BITCNT_NSTATE_METHOD_EXPERIMENTAL
#endif

#ifdef ZSTD_BITCNT_NSTATE_METHOD_DEFAULT_UNDEF
#undef ZSTD_BITCNT_NSTATE_METHOD_DEFAULT_UNDEF
#undef ZSTD_BITCNT_NSTATE_METHOD_DEFAULT
#endif
}

static void zstdgpu_ShaderEntry_DecompressHuffmanWeights(ZSTDGPU_PARAM_INOUT(zstdgpu_DecompressHuffmanWeights_SRT) srt, uint32_t threadId)
{
    const uint32_t cmpBlockCnt = srt.inCounters[kzstdgpu_CounterIndex_Blocks_CMP];
    // NOTE(pamartis): We check the number of FSE tables for Huffman Weights, which gives us the number of FSE-compressed Huffman Weight streams
    if (threadId >= srt.inCounters[kzstdgpu_CounterIndex_FseHufW])
        return;

    zstdgpu_Backward_BitBuffer_V0 buffer;
    zstdgpu_Backward_BitBuffer_V0_InitWithSegment(buffer, srt.inCompressedData, srt.inHufRefs[threadId]);

    const uint32_t initialBitcnt = srt.inFseInfos[zstdgpu_ComputeFseIndexHufW(threadId, cmpBlockCnt)].fseProbCountAndAccuracyLog2 >> 8;

    uint32_t state0 = zstdgpu_Backward_BitBuffer_V0_Get(buffer, initialBitcnt);
    uint32_t state1 = zstdgpu_Backward_BitBuffer_V0_Get(buffer, initialBitcnt);

    uint32_t fseTableOffset = zstdgpu_ComputeFseDataStartHufW(threadId, cmpBlockCnt);
    uint32_t hufTableOffset = threadId * kzstdgpu_MaxCount_HuffmanWeights;

    uint32_t hufWeightIndex = hufTableOffset;
    // TODO IMPROVEMENT TEST 2: don't store decompresed Huffman weights immediately, one byte at a time per VMEM instruction,
    //                          but consider accumulating bytes into a single 32-bit value and store that,
    //                          furthermore, consider storing 32-bit values into LDS first(threadgroup needs TG_Size * 256 bytes of LDS)
    //                          to benefit from memory-coalescing on stores (read Huffman weights in 256-byte chunk from LDS and store to memory)
    //                          which seems acceptable given that TG_Size aims to be as minimal as possible,
    //                              TG_Size = (16-32 threads, 1 wave, potentially some lanes inactive on HW with large wave sizes)
    //                          to not introduce extra extra execution latency due to highly divergent work (decompressing FSE-compressed Huffman weights)
    //                          being executed, so a single lane can keep the entire group alive for a very long period of time
    //                          Q: do we want LDS consumption? if our workload on Async in low-pri mode, the user might prefer to not use LDS at all
    while (1)
    {
        uint32_t offsetState0 = fseTableOffset + state0;
        uint32_t offsetState1 = fseTableOffset + state1;

        const uint32_t fseElem0 = srt.inFseElems[offsetState0];
        // peek
        srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = ZSTDGPU_STATIC_CAST(uint8_t)zstdgpu_FseElem_Symbol(fseElem0);

        // update
        uint32_t bits0 = zstdgpu_FseElem_Bitcnt(fseElem0);
        uint32_t nextstate0 = zstdgpu_FseElem_NState(fseElem0);
        if (zstdgpu_Backward_BitBuffer_V0_CanRefill(buffer, bits0))
        {
            state0 = nextstate0 + zstdgpu_Backward_BitBuffer_V0_Get(buffer, bits0);
        }
        else
        {
            srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = ZSTDGPU_STATIC_CAST(uint8_t)zstdgpu_FseElem_Symbol(srt.inFseElems[offsetState1]);
            break;
        }

        offsetState0 = fseTableOffset + state0;

        const uint32_t fseElem1 = srt.inFseElems[offsetState1];
        // peek
        srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = ZSTDGPU_STATIC_CAST(uint8_t) zstdgpu_FseElem_Symbol(fseElem1);

        // update
        uint32_t bits1 = zstdgpu_FseElem_Bitcnt(fseElem1);
        uint32_t nextstate1 = zstdgpu_FseElem_NState(fseElem1);
        if (zstdgpu_Backward_BitBuffer_V0_CanRefill(buffer, bits1))
        {
            state1 = nextstate1 + zstdgpu_Backward_BitBuffer_V0_Get(buffer, bits1);
        }
        else
        {
            srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = ZSTDGPU_STATIC_CAST(uint8_t)zstdgpu_FseElem_Symbol(srt.inFseElems[offsetState0]);
            break;
        }
#if 0
        offsetState1 = fseTableOffset + state1;

        const uint32_t fseElem0 = srt.inFseElems[offsetState0];
        // peek
        srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = zstdgpu_FseElem_Symbol(fseElem0);

        // update
        bits0 = zstdgpu_FseElem_Bitcnt(fseElem0);
        nextstate0 = zstdgpu_FseElem_NState(fseElem0);
        if (zstdgpu_Backward_BitBuffer_V0_CanRefill(buffer, bits0))
        {
            state0 = nextstate0 + zstdgpu_Backward_BitBuffer_V0Get(buffer, bits0);
        }
        else
        {
            srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = zstdgpu_FseElem_Symbol(srt.inFseElems[offsetState1]);
            break;
        }

        offsetState0 = fseTableOffset + state0;

        const uint32_t fseElem1 = srt.inFseElems[offsetState1];
        // peek
        srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = zstdgpu_FseElem_Symbol(fseElem1);

        // update
        bits1 = zstdgpu_FseElem_Bitcnt(fseElem1);
        nextstate1 = zstdgpu_FseElem_NState(fseElem1);
        if (zstdgpu_Backward_BitBuffer_V0_CanRefill(buffer, bits1))
        {
            state1 = nextstate1 + zstdgpu_Backward_BitBuffer_V0Get(buffer, bits1);
        }
        else
        {
            srt.inoutDecompressedHuffmanWeights[hufWeightIndex++] = zstdgpu_FseElem_Symbol(srt.inFseElems[offsetState0]);
            break;
        }
#endif
    }
    zstdgpu_TypedStoreU8(srt.inoutDecompressedHuffmanWeightCount, threadId, hufWeightIndex - hufTableOffset);
}

static void zstdgpu_ShaderEntry_DecodeHuffmanWeights(ZSTDGPU_PARAM_INOUT(zstdgpu_DecodeHuffmanWeights_SRT) srt, uint32_t threadId)
{
    const uint32_t huffmanWeightsTableCount = srt.inCounters[kzstdgpu_CounterIndex_HUF_WgtStreams];
    if (threadId >= huffmanWeightsTableCount)
        return;

    const uint32_t hufTableIndex = srt.compressedBlockCount - 1 - threadId;

    zstdgpu_Forward_BitBuffer bitBuffer;
    zstdgpu_Forward_BitBuffer_InitWithSegment(bitBuffer, srt.inCompressedData, srt.inHufRefs[hufTableIndex], srt.compressedBufferSizeInBytes);

    const uint32_t hufTableOffset = hufTableIndex * kzstdgpu_MaxCount_HuffmanWeights;
    const uint32_t hufTableWeightCount = srt.inoutDecompressedHuffmanWeightCount[hufTableIndex];

    // TODO(pamartis): this is largely vanilla implementation, we don't need to do full loop
    // and limit ourselves to reading 4 bits one by one.
    for (uint32_t j = 0; j < (hufTableWeightCount >> 1u); ++j)
    {
        const uint32_t elem10 = zstdgpu_Forward_BitBuffer_Get(bitBuffer, 8);
        const uint32_t huffmanWeight0 = elem10 >> 4u;
        const uint32_t huffmanWeight1 = elem10 & 0xfu;
        zstdgpu_TypedStoreU8(srt.inoutDecompressedHuffmanWeights, hufTableOffset + 2 * j + 0, huffmanWeight0);
        zstdgpu_TypedStoreU8(srt.inoutDecompressedHuffmanWeights, hufTableOffset + 2 * j + 1, huffmanWeight1);
    }
    if (hufTableWeightCount & 1u)
    {
        const uint32_t huffmanWeight = zstdgpu_Forward_BitBuffer_Get(bitBuffer, 8) >> 4u;
        zstdgpu_TypedStoreU8(srt.inoutDecompressedHuffmanWeights, hufTableOffset + hufTableWeightCount - 1u, huffmanWeight);
    }
}

// LDS partitioning macro lists for Huffman Table pre-initialisation (sub-regions within PreInit).
#define ZSTDGPU_PRE_INIT_HUFFMAN_TABLE_LDS(base, size)                                  \
    ZSTDGPU_LDS_SIZE(size)                                                              \
    ZSTDGPU_LDS_BASE(base)                                                              \
    ZSTDGPU_LDS_REGION(Bits            , kzstdgpu_MaxCount_HuffmanWeights)              \
    ZSTDGPU_LDS_REGION(BitsMask        , kzstdgpu_MaxCount_HuffmanWeightsAllDigitBits)  \
    ZSTDGPU_LDS_REGION(RankCount       , kzstdgpu_MaxCount_HuffmanWeightRanks)          \
    ZSTDGPU_LDS_REGION(RankCountPrefix , kzstdgpu_MaxCount_HuffmanWeightRanks)          \
    ZSTDGPU_LDS_REGION(WeightSum       , 1)                                             \
    ZSTDGPU_LDS_REGION(BitsMax         , 1)


#include "zstdgpu_lds_decl_size.h"
ZSTDGPU_PRE_INIT_HUFFMAN_TABLE_LDS(0, PreInitHuffmanTable);
#include "zstdgpu_lds_decl_undef.h"

static void zstdgpu_PreInitHuffmanTableToLds(ZSTDGPU_RO_TYPED_BUFFER(uint32_t, uint8_t) HuffmanWeights,
                                             ZSTDGPU_RO_TYPED_BUFFER(uint32_t, uint8_t) HuffmanWeightCount,
                                             uint32_t tableId,
                                             uint32_t threadId,
                                             uint32_t threadCnt,
                                             ZSTDGPU_PARAM_INOUT(uint32_t) outBitsMax,
                                             ZSTDGPU_PARAM_INOUT(uint32_t) outCodeTableSize,
                                             /**
                                              * NOTE(pamartis): Since HLSL doesn't allow us to declare 'groupshared' memory region within function
                                              * or pass such region as a parameter, we resort to passing an integer 'GS_Region' (and later) variable(s) controlling the start
                                              * of region of LDS memory allocated externally.
                                              *
                                              * HLSL also doesn't give us any means of enforcing 'GS_Region' to be a compile-time constant.
                                              */
                                             ZSTDGPU_PARAM_LDS_INOUT(uint32_t) GS_Region,
                                             /**
                                              * NOTE(pamartis): Since HLSL is doesn't give us way to specify callbacks/lambdas, in this case
                                              * to either implement storing pre-initialised Huffman Table to either LDS or Memory, we have
                                              * to pass both:
                                              *     - LDS region as 'GS_OutCodeAndSymbol'
                                              *     - Memory region as 'MEM_OutCodeAndSymbol'
                                              * and supply additional 'bool isMemStore' controlling whether to use LDS or Memory as destination
                                              */
                                             ZSTDGPU_PARAM_LDS_INOUT(uint32_t) GS_RankIndex,
                                             ZSTDGPU_PARAM_LDS_INOUT(uint32_t) GS_CodeAndSymbol,
                                             ZSTDGPU_RW_BUFFER(uint32_t) MEM_CodeAndSymbol,
                                             bool isMemStore)
{
    const uint32_t weightCntMinusOne = HuffmanWeightCount[tableId];

    #include "zstdgpu_lds_decl_base.h"
    ZSTDGPU_PRE_INIT_HUFFMAN_TABLE_LDS(GS_Region, PreInitHuffmanTable)
    #include "zstdgpu_lds_decl_undef.h"

    ZSTDGPU_FOR_WORK_ITEMS(i, 1, threadId, threadCnt)
    {
        zstdgpu_LdsStoreU32(GS_WeightSum, 0);
        zstdgpu_LdsStoreU32(GS_BitsMax, 0);
    }

    ZSTDGPU_FOR_WORK_ITEMS(i, kzstdgpu_MaxCount_HuffmanWeightRanks, threadId, threadCnt)
    {
        zstdgpu_LdsStoreU32(GS_RankCount + i, 0);
        zstdgpu_LdsStoreU32(GS_RankIndex + i, 0);
        zstdgpu_LdsStoreU32(GS_RankCountPrefix + i, 0);
    }

    #ifdef __hlsl_dx_compiler
    // NOTE(pamartis): When the number of lanes in the wave <= 16, we need to initialize GS_BitsMask to zero
    // because we use InterlockedOr on each 32-bit GS_BitsMask[i] to store wave masks (see StoreGroupBitMask)...
    if (WaveGetLaneCount() <= 16)
        #else
        // always in non-HLSL case
        #endif
    {
        ZSTDGPU_FOR_WORK_ITEMS(i, kzstdgpu_MaxCount_HuffmanWeightsAllDigitBits, threadId, threadCnt)
        {
            zstdgpu_LdsStoreU32(GS_BitsMask + i, 0);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    ZSTDGPU_FOR_WORK_ITEMS(i, weightCntMinusOne, threadId, threadCnt)
    {
        const uint32_t weight = HuffmanWeights[tableId * kzstdgpu_MaxCount_HuffmanWeights + i];

        // weights are in range [0;16], so the maximim accumulated value is 0x8000
        const uint32_t weightSumPerWave = WaveActiveSum(weight > 0u ? (1u << (weight - 1u)) : 0u);

        if (WaveIsFirstLane())
        {
            zstdgpu_LdsAtomicAddU32(GS_WeightSum, weightSumPerWave);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // because the maximal accumulated value is 0x8000 and there're only 255 weights possible (because the last weight isn't stored)
    // the maximum value of `GS_WeightSum` is 0x8000 * 255 = 0x7f8000, which means the largest possible highest bit index is 22, and
    // the largest possible `maxBitsUniform` is `23`

    const uint32_t weightSumUniform = zstdgpu_LdsLoadU32(GS_WeightSum);

    const uint32_t maxBitsUniform = zstdgpu_FindFirstBitHiU32(weightSumUniform) + 1u;
    const uint32_t leftoverBitsUniform = (1u << maxBitsUniform) - weightSumUniform;
    const uint32_t lastWeightUniform = zstdgpu_FindFirstBitHiU32(leftoverBitsUniform) + 1u;

    const uint32_t laneCnt = zstdgpu_MinU32(threadCnt, WaveGetLaneCount());
    // NOTE(pamartis): the above is important because `threadCnt` is fixed,
    // so WaveGetLaneCount() could be > `threadCnt`
    #ifdef __hlsl_dx_compiler
    const uint32_t waveCnt = threadCnt / laneCnt;
    #else
    const uint32_t waveCnt = 1;
    #endif

    // NOTE(pamartis): Use `WaveReadLaneFirst` to make sure `waveIdx` is wave-uniform
    const uint32_t waveIdx = WaveReadLaneFirst(threadId / laneCnt);

    uint32_t waveOfs = waveIdx;
    const uint32_t weightCnt = weightCntMinusOne + 1;
    ZSTDGPU_FOR_WORK_ITEMS(weightId, weightCnt, threadId, threadCnt)
    {
        // TODO(pamartis): consider if we can avoid re-loading weights from memory (it's only 256 bytes worst case)
        const uint32_t weight = HuffmanWeights[tableId * kzstdgpu_MaxCount_HuffmanWeights + weightId];

        uint32_t bits = 0u;
        if (weightId < weightCntMinusOne)
        {
            bits = weight > 0u ? (maxBitsUniform + 1u - weight) : 0u;
        }
        else
        {
            bits = maxBitsUniform + 1u - lastWeightUniform;
        }

        zstdgpu_LdsStoreU32(GS_Bits + weightId, bits);

        zstdgpu_GroupBallotLdsStore(laneCnt, bits, GS_BitsMask, kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits, waveOfs, 0);
        zstdgpu_GroupBallotLdsStore(laneCnt, bits, GS_BitsMask, kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits, waveOfs, 1);
        zstdgpu_GroupBallotLdsStore(laneCnt, bits, GS_BitsMask, kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits, waveOfs, 2);
        zstdgpu_GroupBallotLdsStore(laneCnt, bits, GS_BitsMask, kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits, waveOfs, 3);
        zstdgpu_GroupBallotLdsStore(laneCnt, bits, GS_BitsMask, kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits, waveOfs, 4);

        const uint32_t bitsMaxPerWave = WaveActiveMax(bits);

        if (WaveIsFirstLane())
        {
            zstdgpu_LdsAtomicMaxU32(GS_BitsMax, bitsMaxPerWave);
        }
        zstdgpu_LdsAtomicAddU32(GS_RankCount + bits, 1);

        waveOfs += waveCnt;
    }
    GroupMemoryBarrierWithGroupSync();

    outBitsMax = zstdgpu_LdsLoadU32(GS_BitsMax);

    // NOTE(pamartis): initialize RankIndex which is used to determine the starting number of bits
    #ifndef __hlsl_dx_compiler
    uint32_t prevRankIndex = 0;
    uint32_t prevRankCountPrefix = 0;
    #endif
    ZSTDGPU_FOR_WORK_ITEMS(workItemId, outBitsMax + 1u, threadId, threadCnt)
    {
        const uint32_t rankCountId = outBitsMax - workItemId;
        const uint32_t rankCount= zstdgpu_LdsLoadU32(GS_RankCount + rankCountId);
        #ifdef __hlsl_dx_compiler
        uint32_t rankIndexInWave = WavePrefixSum(rankCount << workItemId);
        uint32_t rankCountPrefixInWave = WavePrefixSum(rankCount);

        zstdgpu_LdsAtomicAddU32(GS_RankIndex + workItemId, rankIndexInWave);
        zstdgpu_LdsAtomicAddU32(GS_RankCountPrefix + workItemId, rankCountPrefixInWave);

        // NOTE(pamartis): In case if the number of lanes is smaller than `bitsMax + 1`, every wave needs to update local prefix sums of other waves
        if (laneCnt < outBitsMax + 1)
        {
            const uint32_t lastLaneIndex = WaveActiveCountBits(true) - 1;
            const uint32_t rankIndexWaveSum = WaveReadLaneAt(rankIndexInWave + (rankCount << workItemId), lastLaneIndex);
            const uint32_t rankCountPrefixWaveSum = WaveReadLaneAt(rankCountPrefixInWave + rankCount, lastLaneIndex);

            for (uint32_t workItemNxt = workItemId + laneCnt; workItemNxt < outBitsMax + 1; workItemNxt += laneCnt)
            {
                zstdgpu_LdsAtomicAddU32(GS_RankIndex + workItemNxt, rankIndexWaveSum);
                zstdgpu_LdsAtomicAddU32(GS_RankCountPrefix + workItemNxt, rankCountPrefixWaveSum);
            }
        }

        #else
        zstdgpu_LdsStoreU32(GS_RankIndex + workItemId, prevRankIndex);
        prevRankIndex += rankCount << workItemId;

        zstdgpu_LdsStoreU32(GS_RankCountPrefix + workItemId, prevRankCountPrefix);
        prevRankCountPrefix += rankCount;
        #endif
    }

    GroupMemoryBarrierWithGroupSync();

    ZSTDGPU_FOR_WORK_ITEMS(weightId, weightCnt, threadId, threadCnt)
    {
        const uint32_t uintIdx = weightId >> 5;
        const uint32_t uintOfs = weightId & 0x1fu;

        const uint32_t bits = zstdgpu_LdsLoadU32(GS_Bits + weightId);

        #define FetchBitsAndAccumulateMask(mask, bits, storage, bitIdx, uintId)     \
        const uint32_t bit##bitIdx = zstdgpu_LdsLoadU32(storage + (kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits * bitIdx) + uintId);    \
            mask &= (((bits >> bitIdx) & 0x1u) + ~0u) ^ bit##bitIdx;

        uint32_t prefix = 0;
        for (uint32_t uintId = 0; uintId < uintIdx; ++uintId)
        {
            uint32_t mask = ~0u;

            FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 0, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 1, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 2, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 3, uintId);
            FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 4, uintId);
            prefix += zstdgpu_CountBitsU32(mask);
        }
        uint32_t mask = (1u << uintOfs) - 1u;
        FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 0, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 1, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 2, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 3, uintIdx);
        FetchBitsAndAccumulateMask(mask, bits, GS_BitsMask, 4, uintIdx);
        #undef FetchBitsAndAccumulateMask
        prefix += zstdgpu_CountBitsU32(mask);

        const uint32_t bitsInv = outBitsMax - bits;
        const uint32_t code = zstdgpu_LdsLoadU32(GS_RankIndex + bitsInv) + (prefix << bitsInv);
        const uint32_t codePos = zstdgpu_LdsLoadU32(GS_RankCountPrefix + bitsInv) + prefix;

        ZSTDGPU_ASSERT(codePos < kzstdgpu_MaxCount_HuffmanWeights);

        const uint32_t weightIdAndCodePacked = (weightId << 24) | (code & 0x00ffffffu);

        if (isMemStore) /** NOTE(pamartis): This must be a compile-time constant, see the comment at the top of the function */
        {
            MEM_CodeAndSymbol[tableId * kzstdgpu_MaxCount_HuffmanWeights + codePos] = weightIdAndCodePacked;
        }
        else
        {
            zstdgpu_LdsStoreU32(GS_CodeAndSymbol + codePos, weightIdAndCodePacked);
        }

    }
    outCodeTableSize = zstdgpu_LdsLoadU32(GS_RankCountPrefix + outBitsMax);
}

// LDS partitioning macro lists for Huffman Table Initialisation (standalone shader)
#define ZSTDGPU_INIT_HUFFMAN_TABLE_LDS(base, size)                              \
    ZSTDGPU_LDS_SIZE(size)                                                      \
    ZSTDGPU_LDS_BASE(base)                                                      \
    ZSTDGPU_LDS_REGION(PreInit         , kzstdgpu_PreInitHuffmanTable_LdsSize)   \
    ZSTDGPU_LDS_REGION(RankIndex       , kzstdgpu_MaxCount_HuffmanWeightRanks)  \
    ZSTDGPU_LDS_REGION(CodeAndSymbol   , 1)

#include "zstdgpu_lds_decl_size.h"
ZSTDGPU_INIT_HUFFMAN_TABLE_LDS(0, InitHuffmanTable);
#include "zstdgpu_lds_decl_undef.h"

static void zstdgpu_ShaderEntry_InitHuffmanTable(ZSTDGPU_PARAM_INOUT(zstdgpu_InitHuffmanTable_SRT) srt, uint32_t groupId, uint32_t threadId, uint32_t tgSize)
{
    #include "zstdgpu_lds_decl_base.h"
    ZSTDGPU_INIT_HUFFMAN_TABLE_LDS(0, InitHuffmanTable);
    #include "zstdgpu_lds_decl_undef.h"

    uint32_t bitsMax = 0;
    uint32_t codeTableSize = 0;
    zstdgpu_PreInitHuffmanTableToLds(
        srt.inDecompressedHuffmanWeights,
        srt.inDecompressedHuffmanWeightCount,
        groupId,
        threadId,
        tgSize,
        bitsMax,
        codeTableSize,
        GS_PreInit,
        GS_RankIndex,
        GS_CodeAndSymbol,
        srt.inoutHuffmanTableCodeAndSymbol,
        true
    );
    ZSTDGPU_FOR_WORK_ITEMS(workItemId, bitsMax + 1u, threadId, tgSize)
    {
        srt.inoutHuffmanTableRankIndex[groupId * kzstdgpu_MaxCount_HuffmanWeightRanks + workItemId] = zstdgpu_LdsLoadU32(GS_RankIndex + workItemId);
    }
    if (threadId == 0)
    {
        srt.inoutHuffmanTableInfo[groupId] = (bitsMax << 16) | codeTableSize;
    }
}

static inline void zstdgpu_DecompressHuffmanCompressedLiterals(ZSTDGPU_RO_RAW_BUFFER(uint32_t) CompressedData,
                                                               ZSTDGPU_RO_BUFFER(uint32_t) LitStreamRemap,
                                                               ZSTDGPU_RO_BUFFER(zstdgpu_LitStreamInfo) LitRefs,
                                                               ZSTDGPU_RW_TYPED_BUFFER(uint32_t, uint8_t) DecompressedLiterals,
                                                               ZSTDGPU_PARAM_LDS_IN(uint32_t) GS_HuffmanTable,
                                                               uint32_t groupId,
                                                               uint32_t threadId,
                                                               uint32_t htGroupStart,
                                                               uint32_t htLiteralStart,
                                                               uint32_t htLiteralCount,
                                                               uint32_t bitsMax,
                                                               uint32_t tgSize);


static void zstdgpu_ConvertThreadgroupIdToDecompressLiteralsInputs(ZSTDGPU_RO_BUFFER(uint32_t) LitGroupEndPerHuffmanTable,
                                                                   ZSTDGPU_RO_BUFFER(uint32_t) LitStreamEndPerHuffmanTable,
                                                                   uint32_t htCount,
                                                                   uint32_t groupId,
                                                                   ZSTDGPU_PARAM_INOUT(uint32_t) htIndex,
                                                                   ZSTDGPU_PARAM_INOUT(uint32_t) htGroupStart,
                                                                   ZSTDGPU_PARAM_INOUT(uint32_t) htLiteralStart,
                                                                   ZSTDGPU_PARAM_INOUT(uint32_t) htLiteralCount)
{
    htIndex = 0;
    {
        uint32_t rangeBase = 0;

        // NOTE(pamartis): This number is the number of compressedBlocks
        uint32_t rangeSize = htCount;

        // NOTE(pamartis): Binary search for the right Huffman Table Slot (the actual index is different) */
        while (rangeSize > 1)
        {
            const uint32_t rangeTest = rangeSize >> 1;
            const uint32_t rangeNext = rangeBase + rangeTest;

            uint32_t rangeLoad = 0;
            if (rangeNext > 0)
                rangeLoad = rangeNext - 1;

            const uint32_t groupIdStart = LitGroupEndPerHuffmanTable[rangeLoad];

            rangeBase = groupId < groupIdStart ? rangeBase : rangeNext;
            rangeSize -= rangeTest;
        }

        htIndex = rangeBase;
    }

    htLiteralCount = LitStreamEndPerHuffmanTable[htIndex];
    htLiteralStart = 0;
    //uint32_t htGroupCount = ZstdLitGroupStartPerHuffmanTable[rangeBase];
    htGroupStart = 0;
    if (htIndex > 0)
    {
        htLiteralStart = LitStreamEndPerHuffmanTable[htIndex - 1];
        htLiteralCount -= htLiteralStart;

        htGroupStart = LitGroupEndPerHuffmanTable[htIndex - 1];
        //htGroupCount -= htGroupStart;
    }
}

// LDS partitioning macro lists for Huffman Table Initialisation (standalone shader)
#define ZSTDGPU_DECOMPRESS_LITERALS_LDS(base, size) \
    ZSTDGPU_LDS_SIZE(size)                          \
    ZSTDGPU_LDS_BASE(base)                          \
    ZSTDGPU_LDS_REGION(HuffmanTable, 1u << (kzstdgpu_MaxCount_HuffmanWeightBits - 1))

#include "zstdgpu_lds_decl_size.h"
ZSTDGPU_DECOMPRESS_LITERALS_LDS(0, DecompressLiterals);
#include "zstdgpu_lds_decl_undef.h"

static void zstdgpu_ShaderEntry_DecompressLiterals(ZSTDGPU_PARAM_INOUT(zstdgpu_DecompressLiterals_SRT) srt, uint32_t groupId, uint32_t threadId, uint32_t tgSize)
{
    uint32_t htIndex = 0;
    uint32_t htGroupStart = 0;
    uint32_t htLiteralStart = 0;
    uint32_t htLiteralCount= 0;

    zstdgpu_ConvertThreadgroupIdToDecompressLiteralsInputs(
        srt.inLitGroupEndPerHuffmanTable,
        srt.inLitStreamEndPerHuffmanTable,
        srt.huffmanTableSlotCount,
        groupId,
        htIndex,
        htGroupStart,
        htLiteralStart,
        htLiteralCount
    );


    #include "zstdgpu_lds_decl_base.h"
    ZSTDGPU_DECOMPRESS_LITERALS_LDS(0, DecompressLiterals);
    #include "zstdgpu_lds_decl_undef.h"

    const uint32_t htInfo = WaveReadLaneFirst(srt.inHuffmanTableInfo[htIndex]);
    const uint32_t bitsMax = htInfo >> 16;
    const uint32_t codeTableSize = htInfo & 0xffffu;
    const uint32_t stateCnt = WaveReadLaneFirst(srt.inHuffmanTableRankIndex[htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks + bitsMax]);
    const uint32_t statePairCnt = stateCnt >> 1u;

    // Expand Huffman Table, pack 2 entries into single dword
    ZSTDGPU_FOR_WORK_ITEMS(statePairId, statePairCnt, threadId, tgSize)
    {
        const uint32_t stateId0 = statePairId << 1u;
        const uint32_t stateId1 = stateId0 + 1u;

        const uint32_t symbolIndex0 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableCodeAndSymbol, htIndex * kzstdgpu_MaxCount_HuffmanWeights, codeTableSize, stateId0, 0x00ffffffu);
        const uint32_t symbolIndex1 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableCodeAndSymbol, htIndex * kzstdgpu_MaxCount_HuffmanWeights, codeTableSize, stateId1, 0x00ffffffu);

        const uint32_t bitcntIndex0 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableRankIndex, htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks, bitsMax + 1, stateId0, 0xffffffffu)
                                    - htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks;

        const uint32_t bitcntIndex1 = zstdgpu_BinarySearchMasked(srt.inHuffmanTableRankIndex, htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks, bitsMax + 1, stateId1, 0xffffffffu)
                                    - htIndex * kzstdgpu_MaxCount_HuffmanWeightRanks;

        const uint32_t symbol0 = srt.inHuffmanTableCodeAndSymbol[symbolIndex0] >> 24;
        const uint32_t bitcnt0 = bitsMax - bitcntIndex0;

        const uint32_t symbol1 = srt.inHuffmanTableCodeAndSymbol[symbolIndex1] >> 24;
        const uint32_t bitcnt1 = bitsMax - bitcntIndex1;

        const uint32_t symbolAndBitcnt0 = (symbol0 << 8) | bitcnt0;
        const uint32_t symbolAndBitcnt1 = (symbol1 << 8) | bitcnt1;

        zstdgpu_LdsStoreU32(GS_HuffmanTable + statePairId, (symbolAndBitcnt1 << 16) | symbolAndBitcnt0);
    }
    GroupMemoryBarrierWithGroupSync();

    zstdgpu_DecompressHuffmanCompressedLiterals(
        srt.inCompressedData,
        srt.inLitStreamRemap,
        srt.inLitRefs,
        srt.inoutDecompressedLiterals,
        GS_HuffmanTable,
        groupId,
        threadId,
        htGroupStart,
        htLiteralStart,
        htLiteralCount,
        bitsMax,
        tgSize
    );

}

// LDS partitioning macro lists for combined Huffman Table Initialisation + Literal Decompression
#define ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_LDS(base, size)          \
    ZSTDGPU_LDS_SIZE(size)                                                          \
    ZSTDGPU_LDS_BASE(base)                                                          \
    ZSTDGPU_LDS_REGION(CodeAndSymbol   , kzstdgpu_MaxCount_HuffmanWeights)          \
    ZSTDGPU_LDS_REGION(PreInit         , kzstdgpu_PreInitHuffmanTable_LdsSize)      \
    ZSTDGPU_LDS_REGION(RankIndex       , kzstdgpu_MaxCount_HuffmanWeightRanks)      \
    ZSTDGPU_LDS_REGION(HuffmanTable    , 1u << (kzstdgpu_MaxCount_HuffmanWeightBits - 1u))

#include "zstdgpu_lds_decl_size.h"
ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_LDS(0, InitHuffmanTableAndDecompressLiterals);
#include "zstdgpu_lds_decl_undef.h"

static void zstdgpu_ShaderEntry_InitHuffmanTable_And_DecompressLiterals(ZSTDGPU_PARAM_INOUT(zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT) srt, uint32_t groupId, uint32_t threadId)
{
    uint32_t htIndex = 0;
    uint32_t htGroupStart = 0;
    uint32_t htLiteralStart = 0;
    uint32_t htLiteralCount= 0;

    zstdgpu_ConvertThreadgroupIdToDecompressLiteralsInputs(
        srt.inLitGroupEndPerHuffmanTable,
        srt.inLitStreamEndPerHuffmanTable,
        srt.huffmanTableSlotCount,
        groupId,
        htIndex,
        htGroupStart,
        htLiteralStart,
        htLiteralCount
    );

    //
    // The start of the Huffman Table initialisation
    //
    #include "zstdgpu_lds_decl_base.h"
    ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_LDS(0, InitHuffmanTableAndDecompressLiterals);
    #include "zstdgpu_lds_decl_undef.h"

    ZSTDGPU_RW_BUFFER(uint32_t) dummyBuffer;
    #ifndef __hlsl_dx_compiler
    dummyBuffer = 0;
    #endif

    uint32_t bitsMax = 0;
    uint32_t codeTableSize = 0;
    zstdgpu_PreInitHuffmanTableToLds(
        srt.inDecompressedHuffmanWeights,
        srt.inDecompressedHuffmanWeightCount,
        htIndex,
        threadId,
        kzstdgpu_TgSizeX_DecompressLiterals,
        bitsMax,
        codeTableSize,
        GS_PreInit,
        GS_RankIndex,
        GS_CodeAndSymbol,
        dummyBuffer,
        false
    );

    GroupMemoryBarrierWithGroupSync();

    const uint32_t stateCnt = zstdgpu_LdsLoadU32(GS_RankIndex + bitsMax);
    const uint32_t statePairCnt = stateCnt >> 1u;

    // Expand Huffman Table
    ZSTDGPU_FOR_WORK_ITEMS(statePairId, statePairCnt, threadId, kzstdgpu_TgSizeX_DecompressLiterals)
    {
        const uint32_t stateId0 = statePairId << 1u;
        const uint32_t stateId1 = stateId0 + 1u;

        const uint32_t symbolIndex0 = zstdgpu_BinarySearchLds(GS_CodeAndSymbol, 0, codeTableSize, stateId0, 0x00ffffffu);
        const uint32_t symbolIndex1 = zstdgpu_BinarySearchLds(GS_CodeAndSymbol, 0, codeTableSize, stateId1, 0x00ffffffu);

        const uint32_t bitcntIndex0 = zstdgpu_BinarySearchLds(GS_RankIndex, 0, bitsMax + 1, stateId0, 0xffffffffu);
        const uint32_t bitcntIndex1 = zstdgpu_BinarySearchLds(GS_RankIndex, 0, bitsMax + 1, stateId1, 0xffffffffu);

        const uint32_t symbol0 = zstdgpu_LdsLoadU32(GS_CodeAndSymbol + symbolIndex0) >> 24;
        const uint32_t symbol1 = zstdgpu_LdsLoadU32(GS_CodeAndSymbol + symbolIndex1) >> 24;

        const uint32_t bitcnt0 = bitsMax - bitcntIndex0;
        const uint32_t bitcnt1 = bitsMax - bitcntIndex1;

        const uint32_t symbolAndBitcnt0 = (symbol0 << 8) | bitcnt0;
        const uint32_t symbolAndBitcnt1 = (symbol1 << 8) | bitcnt1;

        zstdgpu_LdsStoreU32(GS_HuffmanTable + statePairId, (symbolAndBitcnt1 << 16) | symbolAndBitcnt0);
    }
    GroupMemoryBarrierWithGroupSync();

    zstdgpu_DecompressHuffmanCompressedLiterals(
        srt.inCompressedData,
        srt.inLitStreamRemap,
        srt.inLitRefs,
        srt.inoutDecompressedLiterals,
        GS_HuffmanTable,
        groupId,
        threadId,
        htGroupStart,
        htLiteralStart,
        htLiteralCount,
        bitsMax,
        kzstdgpu_TgSizeX_DecompressLiterals
    );
}

static inline void zstdgpu_SampleHuffmanSymbolAndBitcnt(ZSTDGPU_PARAM_INOUT(uint32_t) symbol,
                                                        ZSTDGPU_PARAM_INOUT(uint32_t) bitcnt,
                                                        ZSTDGPU_PARAM_IN(uint32_t) state,
                                                        ZSTDGPU_PARAM_LDS_IN(uint32_t) GS_HuffmanTable)
{
    const uint32_t statePairId = state >> 1;
    const uint32_t stateIdInPair = state & 0x1u;
    const uint32_t symbolAndBitcnt = zstdgpu_LdsLoadU32(GS_HuffmanTable + statePairId) >> (stateIdInPair << 4u);
    symbol = (symbolAndBitcnt >> 8) & 0xffu;
    bitcnt = symbolAndBitcnt & 0xffu;
}

void zstdgpu_DecompressHuffmanCompressedLiterals(ZSTDGPU_RO_RAW_BUFFER(uint32_t) CompressedData,
                                                 ZSTDGPU_RO_BUFFER(uint32_t) LitStreamRemap,
                                                 ZSTDGPU_RO_BUFFER(zstdgpu_LitStreamInfo) LitRefs,
                                                 ZSTDGPU_RW_TYPED_BUFFER(uint32_t, uint8_t) DecompressedLiterals,
                                                 ZSTDGPU_PARAM_LDS_IN(uint32_t) GS_HuffmanTable,
                                                 uint32_t groupId,
                                                 uint32_t threadId,
                                                 uint32_t htGroupStart,
                                                 uint32_t htLiteralStart,
                                                 uint32_t htLiteralCount,
                                                 uint32_t bitsMax,
                                                 uint32_t tgSize)
{
    ZSTDGPU_UNUSED(threadId);
    const uint32_t maxBitcntMask = (1u << bitsMax) - 1u;
    ZSTDGPU_UNUSED(maxBitcntMask);
    //
    // The start of decompression of Huffman-compressed literals
    //
    const uint32_t thisGroupLiteralStart = (groupId - htGroupStart) * tgSize;
    const uint32_t thisGroupLiteralRemain = zstdgpu_MinU32(htLiteralCount - thisGroupLiteralStart, tgSize);
    ZSTDGPU_FOR_WORK_ITEMS(literalIndex, thisGroupLiteralRemain, threadId, tgSize)
    {
        const uint32_t literalStreamId = LitStreamRemap[htLiteralStart + thisGroupLiteralStart + literalIndex];

        zstdgpu_LitStreamInfo compressedLiteral = LitRefs[literalStreamId];
        uint32_t decodedByteCnt = 0;
#if 0
        zstdgpu_Backward_BitBuffer_V0 bitBuffer;
        zstdgpu_Backward_BitBuffer_V0_InitWithSegment(bitBuffer, CompressedData, compressedLiteral.src);

        uint32_t state = zstdgpu_Backward_BitBuffer_V0_Get_Huffman(bitBuffer, bitsMax, bitsMax);
        for (;;)
        {
            uint32_t symbol = 0;
            uint32_t bitcnt = 0;
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);

            // FIXME/TODO(pamartis): Experiment with storing data to LDS first (we have some allocated but unused)
            // and then to memory. At least try small LDS cache of 32-dwords per literal
            zstdgpu_TypedStoreU8(DecompressedLiterals, compressedLiteral.dst.offs + decodedByteCnt++, symbol);

            if (decodedByteCnt == compressedLiteral.dst.size)
            {
                break;
            }

            const uint32_t rest = zstdgpu_Backward_BitBuffer_V0_Get_Huffman(bitBuffer, bitcnt, bitsMax);
            state = ((state << bitcnt) + rest) & maxBitcntMask;
        }
#else
        zstdgpu_HuffmanStream stream;
        zstdgpu_HuffmanStream_InitWithSegment(stream, CompressedData, compressedLiteral.src, bitsMax);
        do
        {
            const uint32_t state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
            uint32_t symbol = 0;
            uint32_t bitcnt = 0;
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);

            // FIXME/TODO(pamartis): Experiment with storing data to LDS first (we have some allocated but unused)
            // and then to memory. At least try small LDS cache of 32-dwords per literal
            zstdgpu_TypedStoreU8(DecompressedLiterals, compressedLiteral.dst.offs + decodedByteCnt++, symbol);
            // It could make sense to mid-break on (decodedByteCnt == compressedLiteral.dst.size) instead.
            zstdgpu_HuffmanStream_Consume(stream, bitcnt);
        } while (decodedByteCnt < compressedLiteral.dst.size);
#endif
    }
}

static void zstdgpu_DecompressHuffmanCompressedLiterals_StoreLdsCache(ZSTDGPU_RO_RAW_BUFFER(uint32_t) CompressedData,
                                                                      ZSTDGPU_RO_BUFFER(uint32_t) LitStreamRemap,
                                                                      ZSTDGPU_RO_BUFFER(zstdgpu_LitStreamInfo) LitRefs,
                                                                      ZSTDGPU_RW_TYPED_BUFFER(uint32_t, uint8_t) DecompressedLiterals,
                                                                      ZSTDGPU_RW_BUFFER(uint32_t) DecompressedLiteralsAsDwords,
                                                                      ZSTDGPU_PARAM_LDS_IN(uint32_t) GS_HuffmanTable,
                                                                      ZSTDGPU_PARAM_LDS_INOUT(uint32_t) GS_LiteralStoreCache,
                                                                      uint32_t groupId,
                                                                      uint32_t threadId,
                                                                      uint32_t htGroupStart,
                                                                      uint32_t htLiteralStart,
                                                                      uint32_t htLiteralCount,
                                                                      uint32_t bitsMax,
                                                                      uint32_t tgSize,
                                                                      uint32_t streamsPerGroup,
                                                                      uint32_t cacheDwordsPerStream)
{
    ZSTDGPU_UNUSED(threadId);
    const uint32_t maxBitcntMask = (1u << bitsMax) - 1u;
    ZSTDGPU_UNUSED(maxBitcntMask);
    //
    // The start of decompression of Huffman-compressed literals
    //
    const uint32_t thisGroupLiteralStart = (groupId - htGroupStart) * streamsPerGroup;
    const uint32_t thisGroupLiteralRemain = zstdgpu_MinU32(htLiteralCount - thisGroupLiteralStart, streamsPerGroup);

    zstdgpu_HuffmanStream stream;

    uint32_t byteAlignedBeg = 0;
    uint32_t byteAlignedEnd = 0;
    uint32_t dwordAlignedEnd = 0;
    uint32_t dwordAlignedBeg = 0;

    ZSTDGPU_BRANCH if (threadId < thisGroupLiteralRemain)
    {
        const uint32_t literalStreamId = LitStreamRemap[htLiteralStart + thisGroupLiteralStart + threadId];
        zstdgpu_LitStreamInfo compressedLiteral = LitRefs[literalStreamId];
        if (compressedLiteral.dst.size != 0) // derived from block Regenerated_Size
        {
            zstdgpu_HuffmanStream_InitWithSegment(stream, CompressedData, compressedLiteral.src, bitsMax);
        }

        byteAlignedBeg = compressedLiteral.dst.offs;
        byteAlignedEnd = byteAlignedBeg + compressedLiteral.dst.size;
        dwordAlignedEnd = zstdgpu_MaxU32(byteAlignedBeg, byteAlignedEnd & ~3u);
        dwordAlignedBeg = zstdgpu_MinU32((byteAlignedBeg + 3u) & ~3u, dwordAlignedEnd);
    }

    uint32_t symbol = 0;
    uint32_t bitcnt = 0;
    uint32_t state = 0;

    // Handle head bytes (up to 3 bytes before the first dword-aligned address)
    ZSTDGPU_BRANCH if (byteAlignedBeg < dwordAlignedBeg)
    {
        state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
        zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
        zstdgpu_TypedStoreU8(DecompressedLiterals, byteAlignedBeg ++, symbol);
        zstdgpu_HuffmanStream_Consume(stream, bitcnt);

        ZSTDGPU_BRANCH if (byteAlignedBeg < dwordAlignedBeg)
        {
            state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
            zstdgpu_TypedStoreU8(DecompressedLiterals, byteAlignedBeg ++, symbol);
            zstdgpu_HuffmanStream_Consume(stream, bitcnt);

            ZSTDGPU_BRANCH if (byteAlignedBeg < dwordAlignedBeg)
            {
                state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
                zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
                zstdgpu_TypedStoreU8(DecompressedLiterals, byteAlignedBeg ++, symbol);
                zstdgpu_HuffmanStream_Consume(stream, bitcnt);
            }
        }
    }

    const uint32_t kStoreCacheBankCount = 32;
    const uint32_t kStoreCacheBankMask = kStoreCacheBankCount - 1u;

    const uint32_t storeCacheThreadOffset = threadId * cacheDwordsPerStream;

    const uint32_t dwordIdxBeg = dwordAlignedBeg >> 2;
    const uint32_t dwordIdxEnd = dwordAlignedEnd >> 2;
    uint32_t dwordIdx = dwordIdxBeg;

    const uint32_t laneCnt = zstdgpu_MinU32(WaveGetLaneCount(), tgSize);
    const uint32_t waveIdx = WaveReadLaneFirst(threadId / laneCnt);
    const uint32_t streamBeg = waveIdx * laneCnt;

    do
    {
        const uint32_t dwordIdxBatchBeg = dwordIdx;
        const uint32_t dwordIdxBatchEnd = zstdgpu_MinU32(dwordIdx + cacheDwordsPerStream, dwordIdxEnd);

        // Populate LDS cache of at most 'cacheDwordsPerStream' dwords per stream
        for (; dwordIdx < dwordIdxBatchEnd; ++dwordIdx)
        {
            uint32_t dword = 0;
            state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
            dword |= symbol;
            zstdgpu_HuffmanStream_Consume(stream, bitcnt);

            state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
            dword |= symbol << 8;
            zstdgpu_HuffmanStream_Consume(stream, bitcnt);

            state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
            dword |= symbol << 16;
            zstdgpu_HuffmanStream_Consume(stream, bitcnt);

            state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
            dword |= symbol << 24;
            zstdgpu_HuffmanStream_Consume(stream, bitcnt);

            const uint32_t dwordIdxInBatch = dwordIdx - dwordIdxBatchBeg;

            // add 'threadId' to 'dwordIdx' to make sure there's no bank conflicts.
            const uint32_t dwordIdxInCache = (dwordIdxInBatch & ~kStoreCacheBankMask) + ((dwordIdxInBatch + threadId) & kStoreCacheBankMask);
            zstdgpu_LdsStoreU32(GS_LiteralStoreCache + storeCacheThreadOffset + dwordIdxInCache, dword);
        }

        uint32_t i = streamBeg;
        const uint32_t streamEnd = zstdgpu_MinU32(i + laneCnt, thisGroupLiteralRemain);

        ZSTDGPU_LOOP for (; i < streamEnd; ++i)
        {
            const uint32_t dwordCntInBatch = WaveReadLaneAt(dwordIdxBatchEnd - dwordIdxBatchBeg, i - streamBeg);
            const uint32_t dstDwordIdx = WaveReadLaneAt(dwordIdxBatchBeg, i - streamBeg);

            ZSTDGPU_FOR_WORK_ITEMS(dwordIdxToStore, dwordCntInBatch, WaveGetLaneIndex(), laneCnt)
            {
                // Inverse of the store swizzle: thread i stored logical dword d at cache position ((d + i) & mask)
                const uint32_t dwordIdxInCache = (dwordIdxToStore & ~kStoreCacheBankMask) + ((dwordIdxToStore + i) & kStoreCacheBankMask);
                const uint32_t dword = zstdgpu_LdsLoadU32(GS_LiteralStoreCache + ZSTDGPU_INTENDED_MUL32(i * cacheDwordsPerStream) + dwordIdxInCache);

                DecompressedLiteralsAsDwords[dstDwordIdx + dwordIdxToStore] = dword;
            }
        }
    }
    while (WaveActiveAnyTrue(dwordIdx < dwordIdxEnd));

    // Handle tail bytes (up to 3 bytes after the last dword-aligned address)
    ZSTDGPU_BRANCH if (dwordAlignedEnd < byteAlignedEnd)
    {
        uint32_t tailByte = dwordAlignedEnd;

        state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
        zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
        zstdgpu_TypedStoreU8(DecompressedLiterals, tailByte ++, symbol);
        zstdgpu_HuffmanStream_Consume(stream, bitcnt);

        ZSTDGPU_BRANCH if (tailByte < byteAlignedEnd)
        {
            state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
            zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
            zstdgpu_TypedStoreU8(DecompressedLiterals, tailByte ++, symbol);
            zstdgpu_HuffmanStream_Consume(stream, bitcnt);

            ZSTDGPU_BRANCH if (tailByte < byteAlignedEnd)
            {
                state = zstdgpu_HuffmanStream_RefillAndPeek(stream);
                zstdgpu_SampleHuffmanSymbolAndBitcnt(symbol, bitcnt, state, GS_HuffmanTable);
                zstdgpu_TypedStoreU8(DecompressedLiterals, tailByte ++, symbol);
                zstdgpu_HuffmanStream_Consume(stream, bitcnt);
            }
        }
    }
}

static void zstdgpu_SequenceOffsets_Init(ZSTDGPU_PARAM_INOUT(uint32_t) offset1,
                                         ZSTDGPU_PARAM_INOUT(uint32_t) offset2,
                                         ZSTDGPU_PARAM_INOUT(uint32_t) offset3)
{
    // NOTE (pamartis): the meaning of "1, 2, 3" here is:
    //  - we initialize offset1 to "final" (resulting offset after executing all sequences) offset1 from previous block
    //  - we initialize offset2 to "final" (resulting offset after executing all sequences) offset2 from previous block
    //  - we initialize offset3 to "final" (resulting offset after executing all sequences) offset3 from previous block
    //
    // but we apply special encoding, so they could be either replaced by "non-repeat" offsets or used multiple times
    // with "-1 byte" operation (when llen == 0 and "repeat" offset is 3), see `zstdgpu_SequenceOffsets_Update2`
    offset1 = zstdgpu_EncodeSeqRepeatOffset(1);
    offset2 = zstdgpu_EncodeSeqRepeatOffset(2);
    offset3 = zstdgpu_EncodeSeqRepeatOffset(3);
}

static uint32_t zstdgpu_SequenceOffsets_Update(ZSTDGPU_PARAM_INOUT(uint32_t) offset1,
                                               ZSTDGPU_PARAM_INOUT(uint32_t) offset2,
                                               ZSTDGPU_PARAM_INOUT(uint32_t) offset3,
                                               uint32_t offset,
                                               uint32_t llen)
{
    uint32_t actualOffset = offset;
    if (offset > 3u)
    {
        offset3 = offset2;
        offset2 = offset1;
        offset1 = offset;
    }
    else
    {
        if (llen != 0)
        {
            if (offset == 1u)
            {
                actualOffset = offset1;
                //offset3 = offset3
                //offset2 = offset2
                //offset1 = actualOffset1
            }
            else if (offset == 2u)
            {
                actualOffset = offset2;
                //offset3 = offset3
                offset2 = offset1;
                offset1 = actualOffset;
            }
            else
            {
                actualOffset = offset3;
                offset3 = offset2;
                offset2 = offset1;
                offset1 = actualOffset;
            }
        }
        else
        {
            if (offset == 1u)
            {
                actualOffset = offset2;
                //offset3 = offset3
                offset2 = offset1;
                offset1 = actualOffset;
            }
            else if (offset == 2u)
            {
                actualOffset = offset3;
                offset3 = offset2;
                offset2 = offset1;
                offset1 = actualOffset;
            }
            else
            {
                if (offset1 > 3u)
                {
                    // case 1: if we have actual offset (bit 31 is 0) -- we subtract a byte
                    // case 2: if the offset is "repeat offset" (bit 31 is 1, bits 29 and 30 encode previous "repeat offset")
                    //         which depending on previous block. We keep subtracting bytes
                    actualOffset = offset1 - 1u;
                }
                else if (offset1 > 0) // we don't have valid offset, but we have to subtract one byte, so we re-encode "repeat offset"
                {
                    // in the encoding
                    //      - we set offs[31] bit to 1 to mark this uint32_t as "encoded"
                    //      - we set offs[30:29] to the "repeated offset"
                    //      - we set all other bits to 1, so it behaves as -1 (which is a starting bit)
                    actualOffset = zstdgpu_EncodeSeqRepeatOffset(offset1);
                }
                else
                {
#ifndef __hlsl_dx_compiler
                    // offset must no be zero
                    __debugbreak();
#endif
                }
                offset3 = offset2;
                offset2 = offset1;
                offset1 = actualOffset;
            }
        }
    }
    return actualOffset;
}

static uint32_t zstdgpu_SequenceOffsets_Update2(ZSTDGPU_PARAM_INOUT(uint32_t) offset1,
                                                ZSTDGPU_PARAM_INOUT(uint32_t) offset2,
                                                ZSTDGPU_PARAM_INOUT(uint32_t) offset3,
                                                uint32_t offset,
                                                uint32_t llen)
{
    uint32_t encodedOffset = offset;
    if (offset <= 3u)
    {
        offset += llen == 0u ? 1u : 0u;
#if 0
        if (offset == 4u)
        {
            // case 1: if we have an actual offset (bit 31 is 0) -- we subtract a byte
            // case 2: if the offset is "repeat offset" (bit 31 is 1, bits 29 and 30 encode previous "repeat offset")
            //         which depending on previous block. We keep subtracting bytes
            encodedOffset = offset1 - 1u;
        }
        else
        {
            encodedOffset = (offset == 3u) ? offset3 : ((offset == 2u) ? offset2 : offset1);
        }
#else
        encodedOffset = (offset < 3u) ? (offset < 2u ? offset1 : offset2) : (offset < 4u ? offset3 : (offset1 - 1u));
#endif
    }

    offset3 = offset >= 3u ? offset2 : offset3;
    offset2 = offset >= 2u ? offset1 : offset2;
    offset1 = encodedOffset;
    return encodedOffset;
}

static zstdgpu_OffsetAndSize zstdgpu_GetSequenceStartAndCount(ZSTDGPU_PARAM_INOUT(zstdgpu_DecompressSequences_SRT) srt, uint32_t seqStreamIdx, uint32_t seqStreamCnt)
{
    zstdgpu_OffsetAndSize ref;
    ref.offs = srt.inPerSeqStreamSeqStart[seqStreamIdx];

    ZSTDGPU_BRANCH if (seqStreamIdx + 1u == seqStreamCnt)
    {
        ref.size = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams_DecodedItems];
    }
    else
    {
        ref.size = srt.inPerSeqStreamSeqStart[seqStreamIdx + 1u];
    }
    ref.size -= ref.offs;
    return ref;
}

static void zstdgpu_ReadSeqBitsAndDecompress(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer_V0) bitBuffer,
                                             ZSTDGPU_PARAM_IN(uint32_t) symbolLLen,
                                             ZSTDGPU_PARAM_IN(uint32_t) symbolOffs,
                                             ZSTDGPU_PARAM_IN(uint32_t) symbolMLen,
                                             ZSTDGPU_PARAM_INOUT(uint32_t) outLLen,
                                             ZSTDGPU_PARAM_INOUT(uint32_t) outOffs,
                                             ZSTDGPU_PARAM_INOUT(uint32_t) outMLen)
{
    // Extra bits are low 5 bits, rest are baseline.
    // It could be better/simpler to not bitpack (use uint32_t2), but the LLVM-SROA pass in DXC might split that up; a single load is desired.
    static const uint32_t SEQ_LITERAL_LENGTH_EXTRA_BITS_AND_BASELINES[36] =
    {
            0 << 5 |  0,     1 << 5 |  0,     2 << 5 |  0,     3 << 5 |  0,     4 << 5 |  0,     5 << 5 |  0,     6 << 5 |  0,     7 << 5 |  0,
            8 << 5 |  0,     9 << 5 |  0,    10 << 5 |  0,    11 << 5 |  0,    12 << 5 |  0,    13 << 5 |  0,    14 << 5 |  0,    15 << 5 |  0,
           16 << 5 |  1,    18 << 5 |  1,    20 << 5 |  1,    22 << 5 |  1,    24 << 5 |  2,    28 << 5 |  2,    32 << 5 |  3,    40 << 5 |  3,
           48 << 5 |  4,    64 << 5 |  6,   128 << 5 |  7,   256 << 5 |  8,   512 << 5 |  9,  1024 << 5 | 10,  2048 << 5 | 11,  4096 << 5 | 12,
         8192 << 5 | 13, 16384 << 5 | 14, 32768 << 5 | 15, 65536 << 5 | 16
    };

    static const uint32_t SEQ_MATCH_LENGTH_EXTRA_BITS_AND_BASELINES[53] =
    {
            3 << 5 |  0,     4 << 5 |  0,     5 << 5 |  0,     6 << 5 |  0,     7 << 5 |  0,     8 << 5 |  0,     9 << 5 |  0,    10 << 5 |  0,
           11 << 5 |  0,    12 << 5 |  0,    13 << 5 |  0,    14 << 5 |  0,    15 << 5 |  0,    16 << 5 |  0,    17 << 5 |  0,    18 << 5 |  0,
           19 << 5 |  0,    20 << 5 |  0,    21 << 5 |  0,    22 << 5 |  0,    23 << 5 |  0,    24 << 5 |  0,    25 << 5 |  0,    26 << 5 |  0,
           27 << 5 |  0,    28 << 5 |  0,    29 << 5 |  0,    30 << 5 |  0,    31 << 5 |  0,    32 << 5 |  0,    33 << 5 |  0,    34 << 5 |  0,
           35 << 5 |  1,    37 << 5 |  1,    39 << 5 |  1,    41 << 5 |  1,    43 << 5 |  2,    47 << 5 |  2,    51 << 5 |  3,    59 << 5 |  3,
           67 << 5 |  4,    83 << 5 |  4,    99 << 5 |  5,   131 << 5 |  7,   259 << 5 |  8,   515 << 5 |  9,  1027 << 5 | 10,  2051 << 5 | 11,
         4099 << 5 | 12,  8195 << 5 | 13, 16387 << 5 | 14, 32771 << 5 | 15, 65539 << 5 | 16
    };

    ZSTDGPU_ASSERT(symbolLLen < 36);
    ZSTDGPU_ASSERT(symbolMLen < 53);

    const uint32_t llenInfo = SEQ_LITERAL_LENGTH_EXTRA_BITS_AND_BASELINES[symbolLLen];
    const uint32_t mlenInfo = SEQ_MATCH_LENGTH_EXTRA_BITS_AND_BASELINES[symbolMLen];
    const uint32_t bitcntLLen = llenInfo & 31;
    const uint32_t bitcntOffs = symbolOffs;
    const uint32_t bitcntMLen = mlenInfo & 31;

    const uint32_t bitsOffs = zstdgpu_Backward_BitBuffer_V0_Get(bitBuffer, bitcntOffs);
    const uint32_t bitsMLen = zstdgpu_Backward_BitBuffer_V0_Get(bitBuffer, bitcntMLen);
    const uint32_t bitsLLen = zstdgpu_Backward_BitBuffer_V0_Get(bitBuffer, bitcntLLen);

    outOffs = (1u << symbolOffs) + bitsOffs;
    outMLen = (mlenInfo >> 5) + bitsMLen;
    outLLen = (llenInfo >> 5) + bitsLLen;
}

static void zstdgpu_ReadExtraBitsAndUpdateState(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer_V0) bitBuffer,
                                                uint32_t fseElemLLen,
                                                uint32_t fseElemOffs,
                                                uint32_t fseElemMLen,
                                                ZSTDGPU_PARAM_INOUT(uint32_t) stateLLen,
                                                ZSTDGPU_PARAM_INOUT(uint32_t) stateOffs,
                                                ZSTDGPU_PARAM_INOUT(uint32_t) stateMLen)
{
    const uint32_t restbitcntLLen = zstdgpu_FseElem_Bitcnt(fseElemLLen);
    const uint32_t restbitcntMLen = zstdgpu_FseElem_Bitcnt(fseElemMLen);
    const uint32_t restbitcntOffs = zstdgpu_FseElem_Bitcnt(fseElemOffs);

    #if 0
        // NOTE(pamartis): this version of extra bits reading is left for debugging purpose in case if restbitcnt are wrong somehow
        const uint32_t restLLen = zstdgpu_Backward_BitBuffer_V0_Get(bitBuffer, restbitcntLLen);
        const uint32_t restMLen = zstdgpu_Backward_BitBuffer_V0_Get(bitBuffer, restbitcntMLen);
        const uint32_t restOffs = zstdgpu_Backward_BitBuffer_V0_Get(bitBuffer, restbitcntOffs);
    #else
        // NOTE(pamartis): bit counts stored in FSE tables are equal to accuracy_log in worst case
        // so it's 9 for LLen/MLen and 8 for offset, so we are not extracting more than 26 bits at once
        uint32_t packedBits = zstdgpu_Backward_BitBuffer_V0_Get(bitBuffer, restbitcntLLen + restbitcntMLen + restbitcntOffs);

        const uint32_t restOffs = packedBits & ((1u << restbitcntOffs) - 1u);
        packedBits >>= restbitcntOffs;

        const uint32_t restMLen = packedBits & ((1u << restbitcntMLen) - 1u);
        packedBits >>= restbitcntMLen;

        const uint32_t restLLen = packedBits;
    #endif

    stateLLen = zstdgpu_FseElem_NState(fseElemLLen) + restLLen;
    stateMLen = zstdgpu_FseElem_NState(fseElemMLen) + restMLen;
    stateOffs = zstdgpu_FseElem_NState(fseElemOffs) + restOffs;
}

static void zstdgpu_ShaderEntry_DecompressSequences(ZSTDGPU_PARAM_INOUT(zstdgpu_DecompressSequences_SRT) srt, uint32_t threadId)
{
    const uint32_t seqStreamIdx = threadId;
    const uint32_t seqStreamCnt = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams];
    const uint32_t cmpBlockCnt = srt.inCounters[kzstdgpu_CounterIndex_Blocks_CMP];

    if (seqStreamIdx >= seqStreamCnt)
        return;

    const zstdgpu_SeqStreamInfo seqRef = srt.inSeqRefs[seqStreamIdx];

    #ifdef ZSTDGPU_BACKWARD_BITBUF
    #   error `ZSTDGPU_BACKWARD_BITBUF` must not be defined.
    #endif

    zstdgpu_Backward_BitBuffer_V0 bitBuffer;
    #define ZSTDGPU_BACKWARD_BITBUF(method) zstdgpu_Backward_BitBuffer_V0_##method

    //zstdgpu_Backward_CmpBitBuffer bitBuffer;
    ZSTDGPU_BACKWARD_BITBUF(InitWithSegment)(bitBuffer, srt.inCompressedData, seqRef.src);

    // NOTE: the final block size will be computed as SUM(literalSize, totalMLen)
    const uint32_t literalSize = srt.inoutBlockSizePrefix[seqRef.blockId];
    uint32_t totalSize = 0;
    uint32_t totalMLen = 0;

    uint32_t offset1, offset2, offset3;
    zstdgpu_SequenceOffsets_Init(offset1, offset2, offset3);

    const uint32_t startLLen = zstdgpu_ComputeFseDataStartFromFseIndexLLen(seqRef.fseLLen, cmpBlockCnt);
    const uint32_t startOffs = zstdgpu_ComputeFseDataStartFromFseIndexOffs(seqRef.fseOffs, cmpBlockCnt);
    const uint32_t startMLen = zstdgpu_ComputeFseDataStartFromFseIndexMLen(seqRef.fseMLen, cmpBlockCnt);

    const zstdgpu_OffsetAndSize dst = zstdgpu_GetSequenceStartAndCount(srt, seqStreamIdx, seqStreamCnt);
    const uint32_t outputStart = dst.offs;
    const uint32_t outputEnd = outputStart + dst.size;

    {
        const uint32_t initBitcntLLen = srt.inFseInfos[seqRef.fseLLen].fseProbCountAndAccuracyLog2 >> 8;
        const uint32_t initBitcntOffs = srt.inFseInfos[seqRef.fseOffs].fseProbCountAndAccuracyLog2 >> 8;
        const uint32_t initBitcntMLen = srt.inFseInfos[seqRef.fseMLen].fseProbCountAndAccuracyLog2 >> 8;

        ZSTDGPU_BACKWARD_BITBUF(Refill)(bitBuffer, initBitcntLLen + initBitcntOffs + initBitcntMLen);

        uint32_t stateLLen = ZSTDGPU_BACKWARD_BITBUF(GetNoRefill)(bitBuffer, initBitcntLLen);
        uint32_t stateOffs = ZSTDGPU_BACKWARD_BITBUF(GetNoRefill)(bitBuffer, initBitcntOffs);
        uint32_t stateMLen = ZSTDGPU_BACKWARD_BITBUF(GetNoRefill)(bitBuffer, initBitcntMLen);

        for (uint32_t i = outputStart; i < outputEnd; ++i)
        {
            stateLLen += startLLen;
            stateOffs += startOffs;
            stateMLen += startMLen;

            const uint32_t fseElemLLen = srt.inFseElems[stateLLen];
            const uint32_t fseElemOffs = srt.inFseElems[stateOffs];
            const uint32_t fseElemMLen = srt.inFseElems[stateMLen];

            uint32_t llen = 0, offs = 0, mlen = 0;
            zstdgpu_ReadSeqBitsAndDecompress(
                bitBuffer,
                zstdgpu_FseElem_Symbol(fseElemLLen),
                zstdgpu_FseElem_Symbol(fseElemOffs),
                zstdgpu_FseElem_Symbol(fseElemMLen),
                llen, offs, mlen
            );
            offs = zstdgpu_SequenceOffsets_Update2(offset1, offset2, offset3, offs, llen);

            // TODO: output totalSize per iteration to automatically compute prefix
            totalSize += llen + mlen;
            totalMLen += mlen;

            srt.inoutDecompressedSequenceLLen[i] = llen;
            srt.inoutDecompressedSequenceMLen[i] = mlen;
            srt.inoutDecompressedSequenceOffs[i] = offs;

            if (i == outputEnd - 1u)
            {
                break;
            }

            zstdgpu_ReadExtraBitsAndUpdateState(bitBuffer, fseElemLLen, fseElemOffs, fseElemMLen, stateLLen, stateOffs, stateMLen);
        }
    }

    // NOTE(pamartis): update block size adding `totalMLen` bytes on top
    srt.inoutBlockSizePrefix[seqRef.blockId] = totalMLen + literalSize;

    srt.inoutPerSeqStreamFinalOffset1[seqStreamIdx] = offset1;
    srt.inoutPerSeqStreamFinalOffset2[seqStreamIdx] = offset2;
    srt.inoutPerSeqStreamFinalOffset3[seqStreamIdx] = offset3;

    #undef ZSTDGPU_BACKWARD_BITBUF
    //ZSTDGPU_ASSERT(bitBuffer.hadlastrefill && bitBuffer.bitcnt == 0);
}

// LDS partitioning macro lists for sequence decompression with in-LDS FSE caching
#ifndef ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE
#define ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE 1
#define ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE_UNDEF 1
#endif

#if !ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE
#define ZSTDGPU_DECOMPRESS_SEQUENCES_LDS_FSE_CACHE_LDS(base, size)      \
    ZSTDGPU_LDS_SIZE(size)                                              \
    ZSTDGPU_LDS_BASE(base)                                              \
    ZSTDGPU_LDS_REGION(FsePackedLLen   , kzstdgpu_FseElemMaxCount_LLen) \
    ZSTDGPU_LDS_REGION(FsePackedMLen   , kzstdgpu_FseElemMaxCount_MLen) \
    ZSTDGPU_LDS_REGION(FsePackedOffs   , kzstdgpu_FseElemMaxCount_Offs)

#include "zstdgpu_lds_decl_size.h"
ZSTDGPU_DECOMPRESS_SEQUENCES_LDS_FSE_CACHE_LDS(0, DecompressSequences_LdsFseCache);
#include "zstdgpu_lds_decl_undef.h"
#endif

static void zstdgpu_ShaderEntry_DecompressSequences_LdsFseCache(ZSTDGPU_PARAM_INOUT(zstdgpu_DecompressSequences_SRT) srt, uint32_t groupId, uint32_t threadId, uint32_t tgSize)
{
#if ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE
    ZSTDGPU_UNUSED(threadId);
    ZSTDGPU_UNUSED(tgSize);
#endif

    const uint32_t seqStreamIdx = groupId;
    const uint32_t seqStreamCnt = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams];
    const uint32_t cmpBlockCnt = srt.inCounters[kzstdgpu_CounterIndex_Blocks_CMP];

    if (seqStreamIdx >= seqStreamCnt)
        return;

    const zstdgpu_SeqStreamInfo seqRef = srt.inSeqRefs[seqStreamIdx];

     // NOTE: the final block size will be computed as SUM(literalSize, totalMLen)
    const uint32_t literalSize = srt.inoutBlockSizePrefix[seqRef.blockId];
    // uint32_t totalSize = 0;
    uint32_t totalMLen = 0;

    uint32_t offset1, offset2, offset3;
    zstdgpu_SequenceOffsets_Init(offset1, offset2, offset3);

    const uint32_t startLLen = zstdgpu_ComputeFseDataStartFromFseIndexLLen(seqRef.fseLLen, cmpBlockCnt);
    const uint32_t startOffs = zstdgpu_ComputeFseDataStartFromFseIndexOffs(seqRef.fseOffs, cmpBlockCnt);
    const uint32_t startMLen = zstdgpu_ComputeFseDataStartFromFseIndexMLen(seqRef.fseMLen, cmpBlockCnt);

    const zstdgpu_OffsetAndSize dst = zstdgpu_GetSequenceStartAndCount(srt, seqStreamIdx, seqStreamCnt);

#if !ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE
    #include "zstdgpu_lds_decl_base.h"
    ZSTDGPU_DECOMPRESS_SEQUENCES_LDS_FSE_CACHE_LDS(0, DecompressSequences_LdsFseCache);
    #include "zstdgpu_lds_decl_undef.h"

    #define ZSTDGPU_PRELOAD_FSE_INTO_LDS(name)                                                                  \
    {                                                                                                           \
        const uint32_t fseAccuracyLog2 = srt.inFseInfos[seqRef.fse##name].fseProbCountAndAccuracyLog2 >> 8;     \
        const uint32_t fseElemCount = 1u << fseAccuracyLog2;                                                    \
        ZSTDGPU_FOR_WORK_ITEMS(i, fseElemCount, threadId, tgSize)                                               \
        {                                                                                                       \
            zstdgpu_LdsStoreU32(GS_FsePacked##name + i, srt.inFseElems[start##name + i]);                       \
        }                                                                                                       \
    }

    ZSTDGPU_PRELOAD_FSE_INTO_LDS(LLen)
    ZSTDGPU_PRELOAD_FSE_INTO_LDS(Offs)
    ZSTDGPU_PRELOAD_FSE_INTO_LDS(MLen)
    #undef ZSTDGPU_PRELOAD_FSE_INTO_LDS

    #if !defined(__XBOX_SCARLETT)
    GroupMemoryBarrierWithGroupSync();
    #endif
#endif

    // The rest of the shader should be scalar. Ideally the compiler should emit mostly scalar instructions,
    // but this may help it, or deactivate unnecessary lanes for instructions with no scalar counterpart (LDS loads).
    if (threadId != 0)
    {
        return;
    }

        #ifdef ZSTDGPU_BACKWARD_BITBUF
        #   error `ZSTDGPU_BACKWARD_BITBUF` must not be defined.
        #endif

    zstdgpu_Backward_BitBuffer_V0 bitBuffer;
    #define ZSTDGPU_BACKWARD_BITBUF(method) zstdgpu_Backward_BitBuffer_V0_##method
    ZSTDGPU_BACKWARD_BITBUF(InitWithSegment)(bitBuffer, srt.inCompressedData, seqRef.src);

    #define ZSTDGPU_INIT_FSE_STATE(name)                                                                    \
        uint32_t state##name = 0;                                                                           \
        {                                                                                                   \
            const uint32_t initBitcnt = srt.inFseInfos[seqRef.fse##name].fseProbCountAndAccuracyLog2 >> 8;  \
            state##name = ZSTDGPU_BACKWARD_BITBUF(Get)(bitBuffer, initBitcnt);                              \
        }

    ZSTDGPU_INIT_FSE_STATE(LLen)
    ZSTDGPU_INIT_FSE_STATE(Offs)
    ZSTDGPU_INIT_FSE_STATE(MLen)
    #undef ZSTDGPU_INIT_FSE_STATE

        uint32_t i         = dst.offs;
    const uint32_t outputEnd = dst.offs + dst.size;
    for (;;)
    {
        #if !ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE
        const uint32_t packedFseElemLLen = WaveReadLaneFirst(zstdgpu_LdsLoadU32(GS_FsePackedLLen + stateLLen));
        const uint32_t packedFseElemOffs = WaveReadLaneFirst(zstdgpu_LdsLoadU32(GS_FsePackedOffs + stateOffs));
        const uint32_t packedFseElemMLen = WaveReadLaneFirst(zstdgpu_LdsLoadU32(GS_FsePackedMLen + stateMLen));
        #else
        const uint32_t packedFseElemLLen = WaveReadLaneFirst(srt.inFseElems[startLLen + stateLLen]);
        const uint32_t packedFseElemOffs = WaveReadLaneFirst(srt.inFseElems[startOffs + stateOffs]);
        const uint32_t packedFseElemMLen = WaveReadLaneFirst(srt.inFseElems[startMLen + stateMLen]);
        #endif

        uint32_t llen = 0, offs = 0, mlen = 0;
        zstdgpu_ReadSeqBitsAndDecompress(bitBuffer,
            zstdgpu_FseElem_Symbol(packedFseElemLLen),
            zstdgpu_FseElem_Symbol(packedFseElemOffs),
            zstdgpu_FseElem_Symbol(packedFseElemMLen),
            llen, offs, mlen);
        offs = zstdgpu_SequenceOffsets_Update2(offset1, offset2, offset3, offs, llen);

        /*totalSize += llen + mlen;*/
        totalMLen += mlen;

        srt.inoutDecompressedSequenceLLen[i] = llen;
        srt.inoutDecompressedSequenceMLen[i] = mlen;
        srt.inoutDecompressedSequenceOffs[i] = offs;

        if (++i == outputEnd)
        {
            break;
        }

        zstdgpu_ReadExtraBitsAndUpdateState(bitBuffer, packedFseElemLLen, packedFseElemOffs, packedFseElemMLen, stateLLen, stateOffs, stateMLen);
    }
    ZSTDGPU_ASSERT(bitBuffer.hadlastrefill && bitBuffer.bitcnt == 0);
    #undef ZSTDGPU_BACKWARD_BITBUF

    // NOTE(pamartis): update block size adding `totalMLen` bytes on top
    srt.inoutBlockSizePrefix[seqRef.blockId] = totalMLen + literalSize;
    srt.inoutPerSeqStreamFinalOffset1[seqStreamIdx] = offset1;
    srt.inoutPerSeqStreamFinalOffset2[seqStreamIdx] = offset2;
    srt.inoutPerSeqStreamFinalOffset3[seqStreamIdx] = offset3;
}

#ifdef ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE_UNDEF
#undef ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE_UNDEF
#undef ZSTDGPU_DECOMPRESS_SEQUENCES_NO_LDS_FSE_CACHE
#endif

// LDS partitioning macro lists for sequence decompression with in-LDS caching
#define ZSTDGPU_DECOMPRESS_SEQUENCES_LDS_OUT_CACHE_LDS(base, size)      \
    ZSTDGPU_LDS_SIZE(size)                                              \
    ZSTDGPU_LDS_BASE(base)                                              \
    ZSTDGPU_LDS_REGION(OutputCache,     kzstdgpu_TgSizeX_DecompressSequences * (SEQ_CACHE_LEN + 1) * 3)

#ifndef SEQ_CACHE_LEN
#define SEQ_CACHE_LEN 128
#define SEQ_CACHE_LEN_UNDEF 1
#endif

#include "zstdgpu_lds_decl_size.h"
ZSTDGPU_DECOMPRESS_SEQUENCES_LDS_OUT_CACHE_LDS(0, DecompressSequences_LdsOutCache);
#include "zstdgpu_lds_decl_undef.h"

static void zstdgpu_ShaderEntry_DecompressSequences_LdsOutCache(ZSTDGPU_PARAM_INOUT(zstdgpu_DecompressSequences_SRT) srt, uint32_t groupId, uint32_t threadId)
{
    const uint32_t globalSeqBase = groupId * kzstdgpu_TgSizeX_DecompressSequences;
    const uint32_t globalSeqEnd = zstdgpu_MinU32(globalSeqBase + kzstdgpu_TgSizeX_DecompressSequences, srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams]);
    const uint32_t groupSeqCount = globalSeqEnd - globalSeqBase;

    const uint32_t seqIdPerThread = zstdgpu_MinU32(threadId, groupSeqCount - 1u);
    const uint32_t seqStreamIdx = groupId * kzstdgpu_TgSizeX_DecompressSequences + seqIdPerThread;
    const uint32_t seqStreamCnt = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams];
    const uint32_t cmpBlockCnt = srt.inCounters[kzstdgpu_CounterIndex_Blocks_CMP];

    const zstdgpu_OffsetAndSize seqRefDst = zstdgpu_GetSequenceStartAndCount(srt, seqStreamIdx, seqStreamCnt);

    const zstdgpu_SeqStreamInfo seqRef = srt.inSeqRefs[seqStreamIdx];

    uint32_t offset1, offset2, offset3;
    zstdgpu_SequenceOffsets_Init(offset1, offset2, offset3);

    #ifndef NUM_THREADS
    #define NUM_THREADS 32
    #else
    #error `NUM_THREADS` must not be defined.
    #endif

    #ifndef SEQ_CACHE_LEN
    #define SEQ_CACHE_LEN 128
    #endif

    #include "zstdgpu_lds_decl_base.h"
    ZSTDGPU_DECOMPRESS_SEQUENCES_LDS_OUT_CACHE_LDS(0, DecompressSequences_LdsOutCache);
    #include "zstdgpu_lds_decl_undef.h"

    const uint32_t seqLdsLLenStart = 0;
    const uint32_t seqLdsMLenStart = kzstdgpu_TgSizeX_DecompressSequences * (SEQ_CACHE_LEN + 1);
    const uint32_t seqLdsOffsStart = kzstdgpu_TgSizeX_DecompressSequences * (SEQ_CACHE_LEN + 1) * 2;

    #ifdef ZSTDGPU_BACKWARD_BITBUF
    #   error `ZSTDGPU_BACKWARD_BITBUF` must not be defined.
    #endif

    zstdgpu_Backward_BitBuffer_V0 bitBuffer;
    #define ZSTDGPU_BACKWARD_BITBUF(method) zstdgpu_Backward_BitBuffer_V0_##method

    //zstdgpu_Backward_CmpBitBuffer bitBuffer;
    ZSTDGPU_BACKWARD_BITBUF(InitWithSegment)(bitBuffer, srt.inCompressedData, seqRef.src);

    // NOTE: the final block size will be computed as SUM(literalSize, totalMLen)
    const uint32_t literalSize = srt.inoutBlockSizePrefix[seqRef.blockId];
    uint32_t totalSize = 0;
    uint32_t totalMLen = 0;

    const uint32_t startLLen = zstdgpu_ComputeFseDataStartFromFseIndexLLen(seqRef.fseLLen, cmpBlockCnt);
    const uint32_t startOffs = zstdgpu_ComputeFseDataStartFromFseIndexOffs(seqRef.fseOffs, cmpBlockCnt);
    const uint32_t startMLen = zstdgpu_ComputeFseDataStartFromFseIndexMLen(seqRef.fseMLen, cmpBlockCnt);

    {
        const uint32_t initBitcntLLen = srt.inFseInfos[seqRef.fseLLen].fseProbCountAndAccuracyLog2 >> 8;
        const uint32_t initBitcntOffs = srt.inFseInfos[seqRef.fseOffs].fseProbCountAndAccuracyLog2 >> 8;
        const uint32_t initBitcntMLen = srt.inFseInfos[seqRef.fseMLen].fseProbCountAndAccuracyLog2 >> 8;

        ZSTDGPU_BACKWARD_BITBUF(Refill)(bitBuffer, initBitcntLLen + initBitcntOffs + initBitcntMLen);

        uint32_t stateLLen = ZSTDGPU_BACKWARD_BITBUF(GetNoRefill)(bitBuffer, initBitcntLLen);
        uint32_t stateOffs = ZSTDGPU_BACKWARD_BITBUF(GetNoRefill)(bitBuffer, initBitcntOffs);
        uint32_t stateMLen = ZSTDGPU_BACKWARD_BITBUF(GetNoRefill)(bitBuffer, initBitcntMLen);

        const uint32_t sequenceCountMax = WaveActiveMax(seqRefDst.size);

        const uint32_t seqMemStart = seqRefDst.offs;
        const uint32_t seqLdsStart = seqIdPerThread * (SEQ_CACHE_LEN + 1);

        for (uint32_t i = 0; i < sequenceCountMax; ++i)
        {
            uint32_t fseElemLLen = 0, fseElemOffs = 0, fseElemMLen = 0;

            if (threadId < groupSeqCount && i < seqRefDst.size)
            {
                stateLLen += startLLen;
                stateOffs += startOffs;
                stateMLen += startMLen;

                fseElemLLen = srt.inFseElems[stateLLen];
                fseElemOffs = srt.inFseElems[stateOffs];
                fseElemMLen = srt.inFseElems[stateMLen];

                uint32_t llen = 0, offs = 0, mlen = 0;
                zstdgpu_ReadSeqBitsAndDecompress(
                    bitBuffer,
                    zstdgpu_FseElem_Symbol(fseElemLLen),
                    zstdgpu_FseElem_Symbol(fseElemOffs),
                    zstdgpu_FseElem_Symbol(fseElemMLen),
                    llen, offs, mlen
                );
                offs = zstdgpu_SequenceOffsets_Update2(offset1, offset2, offset3, offs, llen);

                const uint32_t seqLdsCurrIndex = i & (SEQ_CACHE_LEN - 1u);

                zstdgpu_LdsStoreU32((GS_OutputCache + seqLdsLLenStart) + seqLdsStart + seqLdsCurrIndex, llen);
                zstdgpu_LdsStoreU32((GS_OutputCache + seqLdsMLenStart) + seqLdsStart + seqLdsCurrIndex, mlen);
                zstdgpu_LdsStoreU32((GS_OutputCache + seqLdsOffsStart) + seqLdsStart + seqLdsCurrIndex, offs);

                // TODO: computing sum might be cheaper in a cross-wave fashion (depends on WaveActiveSum implementation + complexity of cross-group reduction)
                totalMLen += mlen;
                totalSize += llen + mlen;
            }

            const bool seqLdsCacheFull = (i & (SEQ_CACHE_LEN - 1u)) == (SEQ_CACHE_LEN - 1u) && (i < seqRefDst.size);

            if (WaveActiveAnyTrue(seqLdsCacheFull))
            {
                // NOTE(pamartis): we check for `ldsElemIndex == 0` because this an indicator that
                // LDS cache for a particular sequence is full, so we need to start offloading it
                // to memory
                const uint32_t seqMask = seqLdsCacheFull ? (1u << seqIdPerThread) : 0;
                uint32_t seqWaveMask = WaveActiveBitOr(seqMask);
                while(seqWaveMask)
                {
                    const uint32_t offloadSeqId = zstdgpu_FindFirstBitHiU32(seqWaveMask);

                    const uint32_t seqElemStart = WaveReadLaneAt(i, offloadSeqId) & ~(SEQ_CACHE_LEN - 1u);
                    const uint32_t seqMemStoreBase = (WaveReadLaneAt(seqMemStart, offloadSeqId) + seqElemStart);

                    const uint32_t seqLdsStartUniform = WaveReadLaneAt(seqLdsStart, offloadSeqId);
                    const uint32_t seqLLenLdsStartUniform = seqLdsStartUniform + seqLdsLLenStart;
                    const uint32_t seqMLenLdsStartUniform = seqLdsStartUniform + seqLdsMLenStart;
                    const uint32_t seqOffsLdsStartUniform = seqLdsStartUniform + seqLdsOffsStart;

                    ZSTDGPU_FOR_WORK_ITEMS(seqLdsElemIndex, SEQ_CACHE_LEN, threadId, NUM_THREADS)
                    {
                        const uint32_t seqMemStoreIndex = seqLdsElemIndex + seqMemStoreBase;

                        const uint32_t llen = zstdgpu_LdsLoadU32(GS_OutputCache + seqLLenLdsStartUniform + seqLdsElemIndex);
                        const uint32_t mlen = zstdgpu_LdsLoadU32(GS_OutputCache + seqMLenLdsStartUniform + seqLdsElemIndex);
                        const uint32_t offs = zstdgpu_LdsLoadU32(GS_OutputCache + seqOffsLdsStartUniform + seqLdsElemIndex);

                        // TODO: we need SoA streams here.
                        srt.inoutDecompressedSequenceLLen[seqMemStoreIndex] = llen;
                        srt.inoutDecompressedSequenceMLen[seqMemStoreIndex] = mlen;
                        srt.inoutDecompressedSequenceOffs[seqMemStoreIndex] = offs;
                    }
                    seqWaveMask &= ~(1u << offloadSeqId);
                }
            }

            if (threadId < groupSeqCount && (i + 1u < seqRefDst.size))
            {
                zstdgpu_ReadExtraBitsAndUpdateState(bitBuffer, fseElemLLen, fseElemOffs, fseElemMLen, stateLLen, stateOffs, stateMLen);
            }
        }

        const bool seqLdsCacheTail = (seqRefDst.size & (SEQ_CACHE_LEN - 1u)) != 0 && threadId < groupSeqCount;

        if (WaveActiveAnyTrue(seqLdsCacheTail))
        {
            const uint32_t seqMask = seqLdsCacheTail ? (1u << seqIdPerThread) : 0;
            uint32_t seqWaveMask = WaveActiveBitOr(seqMask);
            while(seqWaveMask)
            {
                const uint32_t offloadSeqId = zstdgpu_FindFirstBitHiU32(seqWaveMask);

                const uint32_t seqElemCount = WaveReadLaneAt(seqRefDst.size, offloadSeqId);
                const uint32_t seqElemTailCount = seqElemCount & (SEQ_CACHE_LEN - 1u);

                const uint32_t seqElemStart = (seqElemCount - 1u) & ~(SEQ_CACHE_LEN - 1u);
                const uint32_t seqMemStoreBase = (WaveReadLaneAt(seqMemStart, offloadSeqId) + seqElemStart);

                const uint32_t seqLdsStartUniform = WaveReadLaneAt(seqLdsStart, offloadSeqId);
                const uint32_t seqLLenLdsStartUniform = seqLdsStartUniform + seqLdsLLenStart;
                const uint32_t seqMLenLdsStartUniform = seqLdsStartUniform + seqLdsMLenStart;
                const uint32_t seqOffsLdsStartUniform = seqLdsStartUniform + seqLdsOffsStart;

                ZSTDGPU_FOR_WORK_ITEMS(seqLdsElemIndex, seqElemTailCount, threadId, NUM_THREADS)
                {
                    const uint32_t seqMemStoreIndex = seqLdsElemIndex + seqMemStoreBase;

                    const uint32_t llen = zstdgpu_LdsLoadU32(GS_OutputCache + seqLLenLdsStartUniform + seqLdsElemIndex);
                    const uint32_t mlen = zstdgpu_LdsLoadU32(GS_OutputCache + seqMLenLdsStartUniform + seqLdsElemIndex);
                    const uint32_t offs = zstdgpu_LdsLoadU32(GS_OutputCache + seqOffsLdsStartUniform + seqLdsElemIndex);

                    srt.inoutDecompressedSequenceLLen[seqMemStoreIndex] = llen;
                    srt.inoutDecompressedSequenceMLen[seqMemStoreIndex] = mlen;
                    srt.inoutDecompressedSequenceOffs[seqMemStoreIndex] = offs;
                }
                seqWaveMask &= ~(1u << offloadSeqId);
            }
        }
        #undef SEQ_CACHE_LEN
        #undef NUM_THREADS
    }

    if (threadId < groupSeqCount)
    {
        // NOTE(pamartis): update block size adding `totalMLen` bytes on top
        srt.inoutBlockSizePrefix[seqRef.blockId] = totalMLen + literalSize;

        srt.inoutPerSeqStreamFinalOffset1[seqStreamIdx] = offset1;
        srt.inoutPerSeqStreamFinalOffset2[seqStreamIdx] = offset2;
        srt.inoutPerSeqStreamFinalOffset3[seqStreamIdx] = offset3;
    }

    #undef ZSTDGPU_BACKWARD_BITBUF
    //ZSTDGPU_ASSERT(bitBuffer.hadlastrefill && bitBuffer.bitcnt == 0);
}

static void zstdgpu_ShaderEntry_FinaliseSequenceOffsets(ZSTDGPU_PARAM_INOUT(zstdgpu_FinaliseSequenceOffsets_SRT) srt, uint32_t threadId)
{
    const uint32_t seqIdx = threadId;
    const uint32_t seqCnt = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams_DecodedItems];
    const uint32_t frameCnt = srt.inCounters[kzstdgpu_CounterIndex_Frames];
    if (seqIdx >= seqCnt)
        return;

    uint32_t offset = srt.inoutDecompressedSequenceOffs[seqIdx];

    // NOTE(pamartis): during "Sequence Decoding" we encode offsets so that they are either:
    //      - actual relative offsets (offset "N" means "-N" bytes relative to the current position in the output stream) with extra "+3" encoding.
    //      - "repeat" offsets relative to previous block's last sequence (not relative to previous sequence as original) minus some number of bytes
    //        accumulated due to "-1" bytes encoding when literal length is zero
    // so here we check if they are actually "repeat" offsets relative to previous block's last sequence
    if (zstdgpu_DecodeSeqRepeatOffsetEncoded(offset) > 0)
    {
        const uint32_t seqStreamCnt = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams];
        const uint32_t seqStreamIdx = zstdgpu_BinarySearch(srt.inPerSeqStreamSeqStart, 0, seqStreamCnt, seqIdx);
        const uint32_t blockIdx     = srt.inSeqRefs[seqStreamIdx].blockId;
        const uint32_t frameIdx     = zstdgpu_BinarySearch(srt.inPerFrameBlockCountAll, 0, frameCnt, blockIdx);
        const uint32_t seqStreamIdxFirstInFrame = srt.inPerFrameSeqStreamMinIdx[frameIdx];

        // NOTE(pamartis): initialise offsets to "default" values with offset
        uint32_t offset1 = 1u + 3u;
        uint32_t offset2 = 4u + 3u;
        uint32_t offset3 = 8u + 3u;
        ZSTDGPU_BRANCH if (seqStreamIdxFirstInFrame != seqStreamIdx)
        {
            const uint32_t prevSeqStreamIdx = seqStreamIdx - 1u;
            offset1 = srt.inPerSeqStreamFinalOffset1[prevSeqStreamIdx];
            offset2 = srt.inPerSeqStreamFinalOffset2[prevSeqStreamIdx];
            offset3 = srt.inPerSeqStreamFinalOffset3[prevSeqStreamIdx];
        }
        offset = zstdgpu_DecodeSeqRepeatOffsetAndApplyPreviousOffsets(offset, offset1, offset2, offset3);
    }
    offset -= 3u;
    srt.inoutDecompressedSequenceOffs[seqIdx] = offset;
}

static uint32_t zstdgpu_MatchLengthCopy(ZSTDGPU_RW_TYPED_BUFFER(uint32_t, uint8_t) outData, uint32_t outByteIdx, uint32_t outByteEnd, uint32_t seqOffs, uint32_t seqMLen, uint32_t i, uint32_t seqIdx, uint32_t maxCopySize, ZSTDGPU_PARAM_INOUT(uint32_t) readableOutputGlobalEnd)
{
    ZSTDGPU_UNUSED(i);
    ZSTDGPU_UNUSED(seqIdx);
    ZSTDGPU_UNUSED(maxCopySize); //< NOTE(pamartis): Unused only on CPP side.

    // NOTE(jweinste): Avoid waits on UAV stores when llen and mlen are small and match offset is large.
    // This check may be inaccurate, but it should be conservative.
    const uint32_t toReadNowGlobalEnd = (outByteIdx - seqOffs) + seqMLen;
    if (readableOutputGlobalEnd < toReadNowGlobalEnd) // these values should be workgroup-uniform
    {
        DeviceMemoryBarrierWithGroupSync();
        readableOutputGlobalEnd = outByteIdx;
    }

    // NOTE(pamartis): when offset is large enough to fit the entire length, we do as wide copy as possible
    if (seqOffs >= seqMLen)
    {
        const uint32_t copyLen = zstdgpu_MinU32(outByteEnd - outByteIdx, seqMLen);
        ZSTDGPU_FOR_WORK_ITEMS(byteIdx, copyLen, i, maxCopySize)
        {
            outData[outByteIdx + byteIdx] = outData[outByteIdx + byteIdx - seqOffs];
        }

        outByteIdx += copyLen;
    }
    else
    {
        // NOTE(pamartis): when offset is short, so match length overlaps with the destination start
        // we copy 'offs' bytes at a time, except the last copy which copes `mlen % offs` bytes.
        //
        // We don't compute `mlen % offs` to avoid expensive integer divsion and instead use `len` variable to track
        // the start of the copy and use `min(mlen - len, offs)` to make sure last copy is exact.
        uint32_t len = 0;
        do
        {
            const uint32_t copyLen = zstdgpu_MinU32(outByteEnd - outByteIdx, zstdgpu_MinU32(seqMLen - len, seqOffs));
            ZSTDGPU_FOR_WORK_ITEMS(byteIdx, copyLen, i, maxCopySize)
            {
                outData[outByteIdx + byteIdx] = outData[outByteIdx + byteIdx - seqOffs];
            }
            DeviceMemoryBarrierWithGroupSync();

            outByteIdx += copyLen;
            len += copyLen;
        }
        while (len < seqMLen);
        readableOutputGlobalEnd = outByteIdx; // see barrier in do...while
    }

    return outByteIdx;
}

static void zstdgpu_ShaderEntry_ExecuteSequences(ZSTDGPU_PARAM_INOUT(zstdgpu_ExecuteSequences_SRT) srt, uint32_t groupId, uint32_t i, uint32_t maxCopySize)
{
    const uint32_t seqStreamCnt = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams];

    const uint32_t frameCnt = srt.inCounters[kzstdgpu_CounterIndex_Frames];
    const uint32_t frameIdx = groupId;

    const uint32_t cmpBlockBeg = srt.inPerFrameBlockCountCMP[frameIdx];
    const uint32_t cmpBlockEnd = (frameIdx + 1u < frameCnt)
                               ? srt.inPerFrameBlockCountCMP[frameIdx + 1u]
                               : srt.inCounters[kzstdgpu_CounterIndex_Blocks_CMP];

    const uint32_t firstFrameBlockIdx = srt.inPerFrameBlockCountAll[frameIdx];

    uint32_t firstFrameBlockOfs = 0;
    // NOTE(pamartis): Without `ZSTDGPU_BRANCH`, there's out-of-bounds `ZstdInBlockSizePrefix` access detected by validation layer when
    // DXC used: "Version: dxcompiler.dll: 1.6 - 1.6.2112.16 (e8295973c); dxil.dll: 1.6(101.6.2112.13)"
    ZSTDGPU_BRANCH if (firstFrameBlockIdx > 0)
    {
        firstFrameBlockOfs = srt.inBlockSizePrefix[firstFrameBlockIdx - 1];
    }

    const zstdgpu_OffsetAndSize dstFrameOffsAndSize = srt.inUnCompressedFramesRefs[frameIdx];

    uint32_t readableOutputGlobalEnd = 0;

    for (uint32_t cmpBlockIdx = cmpBlockBeg; cmpBlockIdx < cmpBlockEnd; ++cmpBlockIdx)
    {
        const uint32_t blockIdx = srt.inGlobalBlockIndexPerCmpBlock[cmpBlockIdx];

        uint32_t blockOfs = 0;
        // NOTE(pamartis): Without `ZSTDGPU_BRANCH`, there's out-of-bounds `ZstdInBlockSizePrefix` access detected by validation layer when
        // DXC used: "Version: dxcompiler.dll: 1.6 - 1.6.2112.16 (e8295973c); dxil.dll: 1.6(101.6.2112.13)"
        ZSTDGPU_BRANCH if (blockIdx > 0)
        {
            blockOfs = srt.inBlockSizePrefix[blockIdx - 1];
        }

        const uint32_t blockByteBeg = dstFrameOffsAndSize.offs + (blockOfs - firstFrameBlockOfs);
        const uint32_t blockByteEnd = dstFrameOffsAndSize.offs + (srt.inBlockSizePrefix[blockIdx] - firstFrameBlockOfs);

#if 0
        for (uint32_t blockByteIdx = blockByteBeg + i; blockByteIdx < blockByteEnd; blockByteIdx += maxCopySize)
        {
            srt.inoutUnCompressedFramesData[blockByteIdx] = cmpBlockIdx & 255;
        }

#else
        const zstdgpu_OffsetAndSize literal = srt.inCompressedBlocks[cmpBlockIdx].literal;

        const uint32_t seqStreamIdx = srt.inCompressedBlocks[cmpBlockIdx].seqStreamIndex;

        uint32_t seqOfs = 0;
        uint32_t seqEnd = 0;
        // NOTE(pamartis): BRANCH is used to make sure validation layer doesn't complain about accessing `inPerSeqStreamSeqStart`
        ZSTDGPU_BRANCH if (seqStreamIdx != ~0u)
        {
            // NOTE(pamartis): Because `PerSeqStreamSeqStart` contains a prefix, we load the element corresponding to the current stream
            // to get sequence offset and then load either the offset of the next stream or the total number of sequences.
            // and take the difference to calculate the actual number of sequences.
            seqOfs = srt.inPerSeqStreamSeqStart[seqStreamIdx];

            ZSTDGPU_BRANCH if (seqStreamIdx + 1u == seqStreamCnt)
            {
                seqEnd = srt.inCounters[kzstdgpu_CounterIndex_Seq_Streams_DecodedItems];
            }
            else
            {
                seqEnd = srt.inPerSeqStreamSeqStart[seqStreamIdx + 1u];
            }
        }

        const uint32_t litType = zstdgpu_DecodeLitOffsetType(literal.offs);
        const uint32_t litOffs = zstdgpu_DecodeLitOffset(literal.offs);
        const uint32_t litSize = literal.size;

        if (zstdgpu_CheckLitOffsetTypeCmp(litType))
        {
            uint32_t blockByteCur = blockByteBeg;
            uint32_t litCur = litOffs;

                // NOTE(pamartis): LOOP is used to make sure validation layer doesn't complain about accessing `inDecompressedSequence*`
                ZSTDGPU_LOOP for (uint32_t seqIdx = seqOfs; seqIdx < seqEnd; ++seqIdx)
                {
                    // NOTE(pamartis): these are still uniform variables HLSL has no way of enforcing....
                    const uint32_t mlen = srt.inDecompressedSequenceMLen[seqIdx];
                    const uint32_t llen = srt.inDecompressedSequenceLLen[seqIdx];
                    const uint32_t offs = srt.inDecompressedSequenceOffs[seqIdx];

                    const uint32_t copyLen = zstdgpu_MinU32(blockByteEnd - blockByteCur, llen);
                    ZSTDGPU_FOR_WORK_ITEMS(byteIdx, copyLen, i, maxCopySize)
                    {
                        srt.inoutUnCompressedFramesData[blockByteCur + byteIdx] = srt.inDecompressedLiterals[litCur + byteIdx];
                    }
                    blockByteCur += copyLen;
                    litCur += copyLen;

                    blockByteCur = zstdgpu_MatchLengthCopy(srt.inoutUnCompressedFramesData, blockByteCur, blockByteEnd, offs, mlen, i, seqIdx, maxCopySize, readableOutputGlobalEnd);
                }

            // NOTE(pamartis): copy remaining literals. If above condtion `seqStreamIdx == ~0u` is true,
            // it means meaning there's no sequences, we copy the entire literal block.
            ZSTDGPU_ASSERT(litOffs + litSize - litCur == blockByteEnd - blockByteCur);
            ZSTDGPU_FOR_WORK_ITEMS(byteIdx, zstdgpu_MinU32(litOffs + litSize - litCur, blockByteEnd - blockByteCur), i, maxCopySize)
            {
                srt.inoutUnCompressedFramesData[blockByteCur + byteIdx] = srt.inDecompressedLiterals[litCur + byteIdx];
            }
        }
        else if (zstdgpu_CheckLitOffsetTypeRaw(litType))
        {
            uint32_t blockByteCur = blockByteBeg;
            uint32_t litCur = litOffs;

                // NOTE(pamartis): LOOP is used to make sure validation layer doesn't complain about accessing `inDecompressedSequence*`
                ZSTDGPU_LOOP for (uint32_t seqIdx = seqOfs; seqIdx < seqEnd; ++seqIdx)
                {
                    // NOTE(pamartis): these are still uniform variables HLSL has no way of enforcing....
                    const uint32_t mlen = srt.inDecompressedSequenceMLen[seqIdx];
                    const uint32_t llen = srt.inDecompressedSequenceLLen[seqIdx];
                    const uint32_t offs = srt.inDecompressedSequenceOffs[seqIdx];
                    const uint32_t copyLen = zstdgpu_MinU32(blockByteEnd - blockByteCur, llen);

                    ZSTDGPU_FOR_WORK_ITEMS(byteIdx, copyLen, i, maxCopySize)
                    {
                        const uint32_t byteOfs = litCur + byteIdx;
                        srt.inoutUnCompressedFramesData[blockByteCur + byteIdx] = (srt.inCompressedData[byteOfs >> 2] >> ((byteOfs & 3u) << 3u)) & 0xffu;
                    }
                    blockByteCur += copyLen;
                    litCur += copyLen;

                    blockByteCur = zstdgpu_MatchLengthCopy(srt.inoutUnCompressedFramesData, blockByteCur, blockByteEnd, offs, mlen, i, seqIdx, maxCopySize, readableOutputGlobalEnd);
                }

            // NOTE(pamartis): copy remaining literals. If above condtion `seqStreamIdx == ~0u` is true,
            // it means meaning there's no sequences, we copy the entire literal block.
            ZSTDGPU_ASSERT(litOffs + litSize - litCur == blockByteEnd - blockByteCur);
            ZSTDGPU_FOR_WORK_ITEMS(byteIdx, zstdgpu_MinU32(litOffs + litSize - litCur, blockByteEnd - blockByteCur), i, maxCopySize)
            {
                const uint32_t byteOfs = litCur + byteIdx;
                srt.inoutUnCompressedFramesData[blockByteCur + byteIdx] = (srt.inCompressedData[byteOfs >> 2] >> ((byteOfs & 3u) << 3u)) & 0xffu;
            }
        }
        else if (zstdgpu_CheckLitOffsetTypeRle(litType))
        {
            // NOTE(pamartis): RLE literals contain actual symbol instead of offset, so we set the offsets to zero.
            const uint32_t symbol = litOffs;
            uint32_t blockByteCur = blockByteBeg;
            uint32_t litCur = 0;

                // NOTE(pamartis): LOOP is used to make sure validation layer doesn't complain about accessing `inDecompressedSequence*`
                ZSTDGPU_LOOP for (uint32_t seqIdx = seqOfs; seqIdx < seqEnd; ++seqIdx)
                {
                    // NOTE(pamartis): these are still uniform variables HLSL has no way of enforcing....
                    const uint32_t mlen = srt.inDecompressedSequenceMLen[seqIdx];
                    const uint32_t llen = srt.inDecompressedSequenceLLen[seqIdx];
                    const uint32_t offs = srt.inDecompressedSequenceOffs[seqIdx];
                    const uint32_t copyLen = zstdgpu_MinU32(blockByteEnd - blockByteCur, llen);

                    ZSTDGPU_FOR_WORK_ITEMS(byteIdx, copyLen, i, maxCopySize)
                    {
                        zstdgpu_TypedStoreU8(srt.inoutUnCompressedFramesData, blockByteCur + byteIdx, symbol);
                    }
                    blockByteCur += copyLen;
                    litCur += copyLen;

                    blockByteCur = zstdgpu_MatchLengthCopy(srt.inoutUnCompressedFramesData, blockByteCur, blockByteEnd, offs, mlen, i, seqIdx, maxCopySize, readableOutputGlobalEnd);
                }

            // NOTE(pamartis): copy remaining literals. If above condtion `seqStreamIdx == ~0u` is true,
            // it means meaning there's no sequences, we copy the entire literal block.
            ZSTDGPU_ASSERT(litOffs + litSize - litCur == blockByteEnd - blockByteCur);
            ZSTDGPU_FOR_WORK_ITEMS(byteIdx, zstdgpu_MinU32(litOffs + litSize - litCur, blockByteEnd - blockByteCur), i, maxCopySize)
            {
                zstdgpu_TypedStoreU8(srt.inoutUnCompressedFramesData, blockByteCur + byteIdx, symbol);
            }
        }
#endif

    }
}

#ifdef _MSC_VER
    __pragma(warning(pop))
#endif

#endif // ZSTDGPU_SHADERS_H
