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
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#define ZSTDGPU_ENABLE_TIMESTAMPS 1

#include <stdint.h>
#include <stdio.h>

#include "zstdgpu.h"
#include "zstdgpu_shaders.h"

#if defined(_GAMING_XBOX_SCARLETT)
#   include <d3d12_xs.h>
#elif defined(_GAMING_XBOX_XBOXONE)
#   include <d3d12_x.h>
#else
#   include <d3d12.h>
#   include <dxgi1_6.h>
#   include <dxgidebug.h>
#endif

#include "zstdgpu_assert.h"

#define D3D12AID_CHECK(call)                            \
    do                                                  \
    {                                                   \
        HRESULT hr = call;                              \
        ZSTDGPU_ASSERT_MSG(S_OK == hr, "S_OK != 0x%08lx " #call "\n", hr); \
    }                                                   \
    while(0)

#define D3D12AID_ASSERT(cond) ZSTDGPU_ASSERT(cond)

#define D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT 2
#define D3D12AID_API_STATIC 1
#include "d3d12aid.h"
#include <pix3.h>

#include "zstdgpu_resources.h"

#include "ZstdGpuComputeDestBlockOffsets.h"
#include "ZstdGpuComputeDestSequenceOffsets.h"
#include "ZstdGpuComputePrefixSum.h"
#include "ZstdGpuDecodeHuffmanWeights.h"
#include "ZstdGpuDecompressHuffmanWeights.h"
#include "ZstdGpuDecompressLiterals.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache128_8.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache64_16.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache64_8.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache32_32.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache32_16.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache32_8.h"
#include "ZstdGpuDecompressSequences_MultiStream_4.h"
#include "ZstdGpuDecompressSequences_MultiStream_8.h"
#include "ZstdGpuDecompressSequences_MultiStream_8_LdsOutCache_128.h"
#include "ZstdGpuDecompressSequences_MultiStream_8_LdsOutCache_64.h"
#include "ZstdGpuDecompressSequences_MultiStream_4_LdsOutCache_64.h"
#include "ZstdGpuDecompressSequences_MultiStream_4_LdsOutCache_32.h"
#include "ZstdGpuDecompressSequences_MultiStream_2_LdsOutCache_32.h"
#include "ZstdGpuDecompressSequences_SingleStream_LdsFseCache128.h"
#include "ZstdGpuDecompressSequences_SingleStream_LdsFseCache64.h"
#include "ZstdGpuDecompressSequences_SingleStream_LdsFseCache32.h"
#include "ZstdGpuDecompressSequences_SingleStream_ScalarFseLoad128.h"
#include "ZstdGpuDecompressSequences_SingleStream_ScalarFseLoad64.h"
#include "ZstdGpuDecompressSequences_SingleStream_ScalarFseLoad32.h"
#include "ZstdGpuExecuteSequences128.h"
#include "ZstdGpuExecuteSequences64.h"
#include "ZstdGpuExecuteSequences32.h"
#include "ZstdGpuFinaliseSequenceOffsets.h"
#include "ZstdGpuInitFseTable.h"
#include "ZstdGpuInitHuffmanTable.h"
#include "ZstdGpuInitHuffmanTableAndDecompressLiterals.h"
#include "ZstdGpuInitResources.h"
#include "ZstdGpuMemset.h"
#include "ZstdGpuMemsetMemcpy.h"
#include "ZstdGpuParseCompressedBlocks.h"
#include "ZstdGpuParseFrames.h"
#include "ZstdGpuPrefixSequenceOffsets.h"
#include "ZstdGpuPrefixSum.h"
#include "ZstdGpuPropagateFseIndex.h"
#include "ZstdGpuUpdateDispatchArgs.h"

static const int16_t kzstdgpuFseProbsDefault[] =
{
    // SEQ_LITERAL_LENGTH_DEFAULT_DIST
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,  1,  2,  2,
    2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1,

    // SEQ_OFFSET_DEFAULT_DIST
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1,

    // SEQ_MATCH_LENGTH_DEFAULT_DIST
    1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1,  1,  1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  1,  1,  1,  1,  1,  1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1
};

struct zstdgpu_BlockInfo
{
    uint32_t litStreamCount;
    uint32_t seqStreamCount;
    uint32_t litByteCount;
    uint32_t seqElemCount;
};

/**
 *  This is copy and paste of `zstdgpu_ShaderEntry_ParseFrame` mostly due to a divergency of parameters
 */
static inline void zstdgpu_ParseFrame(zstdgpu_FrameInfo *outFrameInfo,
                                      zstdgpu_BlockInfo *outBlockInfo,
                                      zstdgpu_OffsetAndSize *outBlocksRAWRefs,
                                      zstdgpu_OffsetAndSize *outBlocksRLERefs,
                                      zstdgpu_OffsetAndSize *outBlocksCMPRefs,
                                      zstdgpu_Forward_BitBuffer & bits)
{
    ZSTDGPU_ASSERT(NULL != outFrameInfo);

    // "A content compressed by Zstandard is transformed into a Zstandard frame.
    // Multiple frames can be appended into a single file or stream. A frame is
    // totally independent, has a defined beginning and end, and a set of
    // parameters which tells the decoder how to decompress it."

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
        outFrameInfo->windowSize = windowBase + windowAdd;
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

        outFrameInfo->dictionary = zstdgpu_Forward_BitBuffer_Get(bits, bitCount);
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
            outFrameInfo->uncompSize = zstdgpu_Forward_BitBuffer_Get(bits, 32);
            outFrameInfo->uncompSize |= (uint64_t)zstdgpu_Forward_BitBuffer_Get(bits, 32) << 32;
        }
        else
        {
            outFrameInfo->uncompSize = zstdgpu_Forward_BitBuffer_Get(bits, bitCount);
        }

        if (16u == bitCount)
        {
            // "When Field_Size is 2, the offset of 256 is added."
            outFrameInfo->uncompSize += 256;
        }
    }

    if (0 != singleSegmentFlag)
    {
        // "The Window_Descriptor byte is optional. It is absent when
        // Single_Segment_flag is set. In this case, the maximum back-reference
        // distance is the content size itself, which can be any value from 1 to
        // 2^64-1 bytes (16 EB)."
        outFrameInfo->windowSize = outFrameInfo->uncompSize;
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
        const uint32_t blockBase = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);

        if (/* Compressed block */ 2 == blockType)
        {
            if (outBlocksCMPRefs)
            {
                outBlocksCMPRefs[outFrameInfo->cmpBlockStart].offs = blockBase;
                outBlocksCMPRefs[outFrameInfo->cmpBlockStart].size = blockSize;
            }
            outFrameInfo->cmpBlockStart += 1;

            if (NULL == outBlockInfo)
            {
                zstdgpu_Forward_BitBuffer_Skip(bits, blockSize);
            }
            else
            {
                // Parse literal section header
                zstdgpu_Forward_BitBuffer_Refill(bits, 32);
                const uint32_t litBlockType  = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 2);
                const uint32_t litBlockSzFmt = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 2);

                uint32_t regeneratedSize = 0;
                uint32_t compressedSize  = 0;

                if (litBlockType <= 1)
                {
                    if (0 == zstdgpu_BitFieldExtractU32(litBlockSzFmt, 0, 1))
                    {
                        regeneratedSize = (zstdgpu_Forward_BitBuffer_Get(bits, 8 - 4) << 1) | zstdgpu_BitFieldExtractU32(litBlockSzFmt, 1, 1);
                    }
                    else if (litBlockSzFmt == 1)
                    {
                        regeneratedSize = zstdgpu_Forward_BitBuffer_Get(bits, 16 - 4);
                    }
                    else
                    {
                        regeneratedSize = zstdgpu_Forward_BitBuffer_Get(bits, 24 - 4);
                    }

                    compressedSize = (litBlockType == 0) ? regeneratedSize : 1;
                }
                else
                {
                    if (litBlockSzFmt <= 1)
                    {
                        zstdgpu_Forward_BitBuffer_Refill(bits, 10 + 10);
                        regeneratedSize = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 10);
                        compressedSize  = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 10);
                    }
                    else if (litBlockSzFmt == 2)
                    {
                        zstdgpu_Forward_BitBuffer_Refill(bits, 14 + 14);
                        regeneratedSize = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 14);
                        compressedSize  = zstdgpu_Forward_BitBuffer_GetNoRefill(bits, 14);
                    }
                    else
                    {
                        regeneratedSize = zstdgpu_Forward_BitBuffer_Get(bits, 18);
                        compressedSize  = zstdgpu_Forward_BitBuffer_Get(bits, 18);
                    }

                    outBlockInfo->litByteCount += regeneratedSize;
                    outBlockInfo->litStreamCount += 1;
                }

                // Skip past literal section data to reach sequence section header
                if (compressedSize > 0)
                {
                    zstdgpu_Forward_BitBuffer_Skip(bits, compressedSize);
                }

                // Parse sequence count
                const uint32_t seqByte0 = zstdgpu_Forward_BitBuffer_Get(bits, 8);
                uint32_t seqCount = 0;
                if (seqByte0 < 128)
                {
                    seqCount = seqByte0;
                }
                else if (seqByte0 < 255)
                {
                    seqCount = ((seqByte0 - 128) << 8) + zstdgpu_Forward_BitBuffer_Get(bits, 8);
                }
                else
                {
                    seqCount = zstdgpu_Forward_BitBuffer_Get(bits, 16) + 0x7F00;
                }

                outBlockInfo->seqElemCount += seqCount;
                outBlockInfo->seqStreamCount += seqCount > 0 ? 1 : 0;

                // Skip to the end of block
                const uint32_t blockCur = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);
                const uint32_t blockEnd = blockBase + blockSize;
                if (blockEnd >= blockCur)
                {
                    zstdgpu_Forward_BitBuffer_Skip(bits, blockEnd - blockCur);
                }
                else
                {
                    ZSTDGPU_BREAK();
                }
            }
        }
        else if (/* RAW */ 0 == blockType)
        {
            if (outBlocksRAWRefs)
            {
                outBlocksRAWRefs[outFrameInfo->rawBlockStart].offs = blockBase;
                // `Raw_Block` - this is an uncompressed block. `Block_Content` contains `Block_Size` bytes.
                outBlocksRAWRefs[outFrameInfo->rawBlockStart].size = blockSize;
            }
            outFrameInfo->rawBlockStart += 1;
            outFrameInfo->rawBlockBytesStart += blockSize;

            zstdgpu_Forward_BitBuffer_Skip(bits, blockSize);
        }
        else if (/* RLE */ 1 == blockType)
        {
            if (outBlocksRLERefs)
            {
                outBlocksRLERefs[outFrameInfo->rleBlockStart].offs = zstdgpu_Forward_BitBuffer_Get(bits, 8);
                // `RLE_Block` - this is a single byte, repeated `Block_Size` times. `Block_Content` consists of a single byte.
                // On the decompression side, this byte must be repeated `Block_Size` times.
                outBlocksRLERefs[outFrameInfo->rleBlockStart].size = blockSize;
            }
            else
            {
                zstdgpu_Forward_BitBuffer_Skip(bits, 1);
            }
            outFrameInfo->rleBlockStart += 1;
            outFrameInfo->rleBlockBytesStart += blockSize;
        }
        else
        {
            ZSTDGPU_BREAK();
        }
    }
    while (0 == lastBlock);

    const uint32_t contentCheckSumFlag = zstdgpu_BitFieldExtractU32(descriptor, 2, 1);
    if (0 != contentCheckSumFlag)
    {
        zstdgpu_Forward_BitBuffer_Refill(bits, 32);
        zstdgpu_Forward_BitBuffer_Pop(bits, 32);
    }
}

void zstdgpu_CountFramesAndBlocks(zstdgpu_CountFramesAndBlocksInfo *outInfo, const void *memoryBlock, uint32_t memoryBlockSizeInBytes, uint32_t contentSizeInBytes)
{
    outInfo->rawBlockCount  = 0;
    outInfo->rleBlockCount  = 0;
    outInfo->cmpBlockCount  = 0;
    outInfo->frameCount     = 0;
    outInfo->frameByteCount = 0;

    uint32_t byteOfs = 0;

    zstdgpu_Forward_BitBuffer bits;
    zstdgpu_Forward_BitBuffer_Init(bits, (uint32_t *)memoryBlock, contentSizeInBytes, memoryBlockSizeInBytes);

    while (byteOfs < bits.datasz)
    {
        uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);

        /** skip over skipable frames */
        while (magic >= 0x184D2A50 && magic <= 0x184D2A5F)
        {
            const uint32_t frameSize = zstdgpu_Forward_BitBuffer_Get(bits, 32);
            zstdgpu_Forward_BitBuffer_Skip(bits, frameSize);
            byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);
            magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
        }

        if (magic == 0xFD2FB528U)
        {
            zstdgpu_FrameInfo frameInfo = {};
            zstdgpu_ParseFrame(&frameInfo, NULL, NULL, NULL, NULL, bits);

            byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);

            outInfo->rawBlockCount  += frameInfo.rawBlockStart;
            outInfo->rleBlockCount  += frameInfo.rleBlockStart;
            outInfo->cmpBlockCount  += frameInfo.cmpBlockStart;
            outInfo->frameCount     += 1u;
            outInfo->frameByteCount += frameInfo.uncompSize;
        }
        else
        {
            break;
        }
    }
}

void zstdgpu_CollectFrames(zstdgpu_OffsetAndSize *outFrames, zstdgpu_FrameInfo *outFrameInfos, uint32_t frameCount, const void *memoryBlock, uint32_t memoryBlockSizeInBytes, uint32_t contentSizeInBytes)
{
    uint32_t byteOfs = 0;

    zstdgpu_Forward_BitBuffer bits;
    zstdgpu_FrameInfo frameInfo = {};

    zstdgpu_Forward_BitBuffer_Init(bits, (uint32_t*)memoryBlock, contentSizeInBytes, memoryBlockSizeInBytes);

    for (uint32_t frameId = 0; frameId < frameCount; ++frameId)
    {
        uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);

        /** skip over skipable frames */
        while (magic >= 0x184D2A50 && magic <= 0x184D2A5F)
        {
            const uint32_t frameSize = zstdgpu_Forward_BitBuffer_Get(bits, 32);
            zstdgpu_Forward_BitBuffer_Skip(bits, frameSize);
            byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);
            magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
        }

        if (magic == 0xFD2FB528U)
        {
            outFrames[frameId].offs = byteOfs;

            // store prefix
            outFrameInfos[frameId].rawBlockStart      = frameInfo.rawBlockStart;
            outFrameInfos[frameId].rleBlockStart      = frameInfo.rleBlockStart;
            outFrameInfos[frameId].cmpBlockStart      = frameInfo.cmpBlockStart;
            outFrameInfos[frameId].rawBlockBytesStart = frameInfo.rawBlockBytesStart;
            outFrameInfos[frameId].rleBlockBytesStart = frameInfo.rleBlockBytesStart;

            frameInfo.windowSize         = 0;
            frameInfo.uncompSize         = 0;
            frameInfo.dictionary         = 0;
            frameInfo.rawBlockStart      = 0;
            frameInfo.rleBlockStart      = 0;
            frameInfo.cmpBlockStart      = 0;
            frameInfo.rawBlockBytesStart = 0;
            frameInfo.rleBlockBytesStart = 0;
            zstdgpu_ParseFrame(&frameInfo, NULL, NULL, NULL, NULL, bits);

            // store just retrieved data
            outFrameInfos[frameId].windowSize = frameInfo.windowSize;
            outFrameInfos[frameId].uncompSize = frameInfo.uncompSize;
            outFrameInfos[frameId].dictionary = frameInfo.dictionary;

            byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);
            outFrames[frameId].size = byteOfs - outFrames[frameId].offs;

            // accumulate previous prefix onto current frame's block counts
            frameInfo.rawBlockStart      += outFrameInfos[frameId].rawBlockStart;
            frameInfo.rleBlockStart      += outFrameInfos[frameId].rleBlockStart;
            frameInfo.cmpBlockStart      += outFrameInfos[frameId].cmpBlockStart;
            frameInfo.rawBlockBytesStart += outFrameInfos[frameId].rawBlockBytesStart;
            frameInfo.rleBlockBytesStart += outFrameInfos[frameId].rleBlockBytesStart;
        }
        else
        {
            break;
        }
    }
}

void zstdgpu_CollectBlocks(zstdgpu_OffsetAndSize *outBlocksRaw, zstdgpu_OffsetAndSize *outBlocksRLE, zstdgpu_OffsetAndSize *outBlocksCmp, const zstdgpu_OffsetAndSize *frames, const zstdgpu_FrameInfo *frameInfos, uint32_t frameIndex, uint32_t frameCount, const void *memoryBlock, uint32_t memoryBlockSizeInBytes, uint32_t contentSizeInBytes)
{
    uint32_t byteOfs = 0;

    zstdgpu_Forward_BitBuffer bits;
    zstdgpu_Forward_BitBuffer_InitWithSegment(bits, (uint32_t *)memoryBlock, frames[frameIndex], memoryBlockSizeInBytes);

    uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);

    /** skip over skipable frames */
    while (magic >= 0x184D2A50 && magic <= 0x184D2A5F)
    {
        const uint32_t frameSize = zstdgpu_Forward_BitBuffer_Get(bits, 32);
        zstdgpu_Forward_BitBuffer_Skip(bits, frameSize);
        magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
    }

    if (magic == 0xFD2FB528U)
    {
        zstdgpu_FrameInfo frameInfo = {};

        const uint32_t rawBlockStart = frameInfos[frameIndex].rawBlockStart;
        const uint32_t rleBlockStart = frameInfos[frameIndex].rleBlockStart;
        const uint32_t cmpBlockStart = frameInfos[frameIndex].cmpBlockStart;

        const uint32_t byteEnd = frameIndex < frameCount - 1u ? frames[frameIndex + 1u].offs : contentSizeInBytes;

        zstdgpu_ParseFrame(&frameInfo, NULL, &outBlocksRaw[rawBlockStart], &outBlocksRLE[rleBlockStart], &outBlocksCmp[cmpBlockStart], bits);
        byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);

        ZSTDGPU_ASSERT(byteOfs == byteEnd);
    }
}

void zstdgpu_CountCompressedLiteralsAndSequences(zstdgpu_CountLiteralAndSequenceInfo *outInfo, const zstdgpu_OffsetAndSize *frames, uint32_t frameCount, const void *memoryBlock, uint32_t memoryBlockSizeInBytes)
{
    outInfo->decodedLiteralsByteCount = 0;
    outInfo->sequenceCount              = 0;

    uint32_t byteOfs = 0;

    for (uint32_t frameIdx = 0; frameIdx < frameCount; ++frameIdx)
    {
        zstdgpu_Forward_BitBuffer bits;
        zstdgpu_Forward_BitBuffer_InitWithSegment(bits, (uint32_t *)memoryBlock, frames[frameIdx], memoryBlockSizeInBytes);

        uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);

        /** skip over skipable frames */
        while (magic >= 0x184D2A50 && magic <= 0x184D2A5F)
        {
            const uint32_t frameSize = zstdgpu_Forward_BitBuffer_Get(bits, 32);
            zstdgpu_Forward_BitBuffer_Skip(bits, frameSize);
            magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
        }

        if (magic == 0xFD2FB528U)
        {
            zstdgpu_FrameInfo frameInfo = {};
            zstdgpu_BlockInfo blockInfo = {};

            zstdgpu_ParseFrame(&frameInfo, &blockInfo, NULL, NULL, NULL, bits);
            byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);

            ZSTDGPU_ASSERT(byteOfs == frames[frameIdx].offs + frames[frameIdx].size);

            outInfo->decodedLiteralsByteCount    += blockInfo.litByteCount;
            outInfo->sequenceCount               += blockInfo.seqElemCount;
        }
    }
}

typedef struct zstdgpu_SRTs
{
    ID3D12DescriptorHeap *heap;
    uint32_t              heapOffset;

    #define ZSTDGPU_SRT(name, SRT) \
        D3D12_GPU_DESCRIPTOR_HANDLE name##GpuHandle;

        ZSTDGPU_SRT_LIST()
    #undef  ZSTDGPU_SRT
} zstdgpu_SRTs;

static uint32_t zstdgpu_Count_SRTs_Stage(uint32_t stageIndex)
{
    uint32_t descCount = 0;

    #define ZSTDGPU_RO_RAW_BUFFER_DECL(type, name, index)                               descCount += 1;
    #define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                                   descCount += 1;
    #define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                                   descCount += 1;
    #define ZSTDGPU_RW_BUFFER_DECL_GLC(type, name, index)                               descCount += 1;

    #define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  descCount += 1;
    #define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  descCount += 1;
    #define ZSTDGPU_RW_TYPED_BUFFER_DECL_GLC(hlsl_type, type, name, index)              descCount += 1;

    #define ZSTDGPU_RO_BUFFER_ALIAS_DECL(type, name, alias, index)                      descCount += 1;
    #define ZSTDGPU_RW_BUFFER_ALIAS_DECL(type, name, alias, index)                      descCount += 1;
    #define ZSTDGPU_RW_BUFFER_ALIAS_DECL_GLC(type, name, alias, index)                  descCount += 1;

    #define ZSTDGPU_RO_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     descCount += 1;
    #define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     descCount += 1;
    #define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL_GLC(hlsl_type, type, name, alias, index) descCount += 1;

    #define ZSTDGPU_SRT(name, SRT) SRT
        if (stageIndex == 0)
        {
            ZSTDGPU_SRT_LIST_STAGE0()
        }
        else if (stageIndex == 1)
        {
            ZSTDGPU_SRT_LIST_STAGE1()
        }
        else
        {
            ZSTDGPU_SRT_LIST_STAGE2()
        }
    #undef  ZSTDGPU_SRT

    #include "zstdgpu_srt_decl_undef.h"

    return descCount;
}

static void zstdgpu_CreateByteAddressBufferSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpuDest, ID3D12Device* device, ID3D12Resource* resource, uint32_t byteSize)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc =
    {
        DXGI_FORMAT_R32_TYPELESS,
        D3D12_SRV_DIMENSION_BUFFER,
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    desc.Buffer.NumElements = byteSize / sizeof(uint32_t);
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(resource, &desc, cpuDest);
}

static void zstdgpu_ReCreate_SRTs(zstdgpu_SRTs & srts, ID3D12Device *device, const zstdgpu_ResourceInfo & resInfo, const zstdgpu_ResourceDataGpu & gpuResData, uint32_t stageIndex)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = d3d12aid_DescriptorHeap_GetGpuStart(srts.heap);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = d3d12aid_DescriptorHeap_GetCpuStart(srts.heap);
    const uint32_t descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpuDest = { cpuStart.ptr + (SIZE_T)srts.heapOffset * descSize };
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDest = { gpuStart.ptr + (SIZE_T)srts.heapOffset * descSize };

    D3D12_SHADER_RESOURCE_VIEW_DESC SRV;
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAV;
    const DXGI_FORMAT DXGI_FORMAT_uint8_t = DXGI_FORMAT_R8_UINT;
    //const DXGI_FORMAT DXGI_FORMAT_uint16_t = DXGI_FORMAT_R16_UINT;
    const DXGI_FORMAT DXGI_FORMAT_int16_t = DXGI_FORMAT_R16_SINT;

    #define ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, viewType) \
        d3d12aid_##viewType##_Create(cpuDest, device,                                                       \
            gpuResData.gpuOnly.name,                                                                        \
            d3d12aid_##viewType##_InitAsStructBuffer(&viewType, resInfo.name##_ByteSize - resInfo.name##_ByteSize % sizeof(type), sizeof(type))      \
        );                                                                                                  \
        cpuDest.ptr += descSize;                                                                            \
        gpuDest.ptr += descSize;

    #define ZSTDGPU_PUSH_TYPED_BUFFER(type, name, viewType)                                                \
        d3d12aid_##viewType##_Create(cpuDest, device,                                                       \
            gpuResData.gpuOnly.name,                                                                        \
            d3d12aid_##viewType##_InitAsTypedBuffer(&viewType, resInfo.name##_ByteSize, DXGI_FORMAT_##type, sizeof(type))\
        );                                                                                                  \
        cpuDest.ptr += descSize;                                                                            \
        gpuDest.ptr += descSize;


    #define ZSTDGPU_PUSH_RAW_BUFFER(name)                                                                           \
        (zstdgpu_CreateByteAddressBufferSrv(cpuDest, device, gpuResData.gpuOnly.name, resInfo.name##_ByteSize),     \
         cpuDest.ptr += descSize,                                                                                   \
         gpuDest.ptr += descSize);

    #define ZSTDGPU_RO_RAW_BUFFER_DECL(type, name, index)                               ZSTDGPU_PUSH_RAW_BUFFER(name)

    #define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                                   ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, SRV)
    #define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                                   ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, UAV)
    #define ZSTDGPU_RW_BUFFER_DECL_GLC(type, name, index)                               ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, UAV)

    #define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  ZSTDGPU_PUSH_TYPED_BUFFER(type, name, SRV)
    #define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  ZSTDGPU_PUSH_TYPED_BUFFER(type, name, UAV)
    #define ZSTDGPU_RW_TYPED_BUFFER_DECL_GLC(hlsl_type, type, name, index)              ZSTDGPU_PUSH_TYPED_BUFFER(type, name, UAV)

    #define ZSTDGPU_RO_BUFFER_ALIAS_DECL(type, name, alias, index)                      ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, SRV)
    #define ZSTDGPU_RW_BUFFER_ALIAS_DECL(type, name, alias, index)                      ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, UAV)
    #define ZSTDGPU_RW_BUFFER_ALIAS_DECL_GLC(type, name, alias, index)                  ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, UAV)

    #define ZSTDGPU_RO_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     ZSTDGPU_PUSH_TYPED_BUFFER(type, name, SRV)
    #define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     ZSTDGPU_PUSH_TYPED_BUFFER(type, name, UAV)
    #define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL_GLC(hlsl_type, type, name, alias, index) ZSTDGPU_PUSH_TYPED_BUFFER(type, name, UAV)

    #define ZSTDGPU_SRT(name, SRT) srts.name##GpuHandle = gpuDest; SRT
        if (stageIndex == 0)
        {
            ZSTDGPU_SRT_LIST_STAGE0()
        }
        else if (stageIndex == 1)
        {
            ZSTDGPU_SRT_LIST_STAGE1()
        }
        else
        {
            ZSTDGPU_SRT_LIST_STAGE2()
        }
    #undef  ZSTDGPU_SRT

    #include "zstdgpu_srt_decl_undef.h"
}

#define ZSTDGPU_KERNEL_LIST()                                                                                                           \
    ZSTDGPU_KERNEL(ComputeDestBlockOffsets                          ,   L"Compute Destination Block Offsets")                                   \
    ZSTDGPU_KERNEL(ComputeDestSequenceOffsets                       ,   L"Compute Destination Sequence Offsets")                                \
    ZSTDGPU_KERNEL(ComputePrefixSum                                 ,   L"Compute Prefix of Literal and TG Count for Literal Decompression")    \
    ZSTDGPU_KERNEL(DecodeHuffmanWeights                             ,   L"Decode (from nibbles) Uncompressed Huffman Weights")                  \
    ZSTDGPU_KERNEL(DecompressHuffmanWeights                         ,   L"Decompress FSE-compressed Huffman Weights")                           \
    ZSTDGPU_KERNEL(DecompressLiterals                               ,   L"Decompress Literals")                                                 \
    ZSTDGPU_KERNEL(DecompressLiterals_LdsStoreCache128_8            ,   L"Decompress Literals (LDS Store Cache=128 Dwords, Stream Count= 8)")   \
    ZSTDGPU_KERNEL(DecompressLiterals_LdsStoreCache64_16            ,   L"Decompress Literals (LDS Store Cache= 64 Dwords, Stream Count=16)")   \
    ZSTDGPU_KERNEL(DecompressLiterals_LdsStoreCache64_8             ,   L"Decompress Literals (LDS Store Cache= 64 Dwords, Stream Count= 8)")   \
    ZSTDGPU_KERNEL(DecompressLiterals_LdsStoreCache32_32            ,   L"Decompress Literals (LDS Store Cache= 32 Dwords, Stream Count=32)")   \
    ZSTDGPU_KERNEL(DecompressLiterals_LdsStoreCache32_16            ,   L"Decompress Literals (LDS Store Cache= 32 Dwords, Stream Count=16)")   \
    ZSTDGPU_KERNEL(DecompressLiterals_LdsStoreCache32_8             ,   L"Decompress Literals (LDS Store Cache= 32 Dwords, Stream Count= 8)")   \
    ZSTDGPU_KERNEL(DecompressSequences_SingleStream_LdsFseCache128  ,   L"Decompress Sequences (Single-Stream, LDS FSE Cache, TG Size=128)")    \
    ZSTDGPU_KERNEL(DecompressSequences_SingleStream_LdsFseCache64   ,   L"Decompress Sequences (Single-Stream, LDS FSE Cache, TG Size= 64)")    \
    ZSTDGPU_KERNEL(DecompressSequences_SingleStream_LdsFseCache32   ,   L"Decompress Sequences (Single-Stream, LDS FSE Cache, TG Size= 32)")    \
    ZSTDGPU_KERNEL(DecompressSequences_SingleStream_ScalarFseLoad128,   L"Decompress Sequences (Single-Stream, Scalar FSE Load, TG Size=128)")  \
    ZSTDGPU_KERNEL(DecompressSequences_SingleStream_ScalarFseLoad64 ,   L"Decompress Sequences (Single-Stream, Scalar FSE Load, TG Size= 64)")  \
    ZSTDGPU_KERNEL(DecompressSequences_SingleStream_ScalarFseLoad32 ,   L"Decompress Sequences (Single-Stream, Scalar FSE Load, TG Size= 32)")  \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_4                ,   L"Decompress Sequences (Multi-Stream, Streams= 4)")                     \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_8                ,   L"Decompress Sequences (Multi-Stream, Streams= 8)")                     \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_8_LdsOutCache_128,   L"Decompress Sequences (Multi-Stream, Threads Per Stream=8, LDS Out Cache=128 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_8_LdsOutCache_64 ,   L"Decompress Sequences (Multi-Stream, Threads Per Stream=8, LDS Out Cache= 64 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_4_LdsOutCache_64 ,   L"Decompress Sequences (Multi-Stream, Threads Per Stream=4, LDS Out Cache= 64 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_4_LdsOutCache_32 ,   L"Decompress Sequences (Multi-Stream, Threads Per Stream=4, LDS Out Cache= 32 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_2_LdsOutCache_32 ,   L"Decompress Sequences (Multi-Stream, Threads Per Stream=2, LDS Out Cache= 32 Sequences)")    \
    ZSTDGPU_KERNEL(ExecuteSequences128                              ,   L"Execute Sequences 128")                                               \
    ZSTDGPU_KERNEL(ExecuteSequences64                               ,   L"Execute Sequences 64")                                                \
    ZSTDGPU_KERNEL(ExecuteSequences32                               ,   L"Execute Sequences 32")                                                \
    ZSTDGPU_KERNEL(FinaliseSequenceOffsets                          ,   L"Finalise Sequence Offsets")                                           \
    ZSTDGPU_KERNEL(InitFseTable                                     ,   L"Init Fse Table")                                                      \
    ZSTDGPU_KERNEL(InitHuffmanTable                                 ,   L"Init Huffman Table")                                                  \
    ZSTDGPU_KERNEL(InitHuffmanTableAndDecompressLiterals            ,   L"Init Huffman Table and Decompress Literals")                          \
    ZSTDGPU_KERNEL(InitResources                                    ,   L"Init Resources")                                                      \
    ZSTDGPU_KERNEL(Memset                                           ,   L"Memset")                                                              \
    ZSTDGPU_KERNEL(MemsetMemcpy                                     ,   L"Memset-Memcpy")                                                       \
    ZSTDGPU_KERNEL(ParseCompressedBlocks                            ,   L"Parse Compressed Blocks")                                             \
    ZSTDGPU_KERNEL(ParseFrames                                      ,   L"Parse Frames")                                                        \
    ZSTDGPU_KERNEL(PrefixSequenceOffsets                            ,   L"Prefix Sequence Offsets")                                             \
    ZSTDGPU_KERNEL(PrefixSum                                        ,   L"Prefix Sum")                                                          \
    ZSTDGPU_KERNEL(PropagateFseIndex                                ,   L"Propagate FSE Index")                                                 \
    ZSTDGPU_KERNEL(UpdateDispatchArgs                               ,   L"Update Dispatch Args")

typedef enum zstdgpu_CompiledShaderId
{
#define ZSTDGPU_KERNEL(name, desc) kzstdgpu_CompiledShaderId_##name,
    ZSTDGPU_KERNEL_LIST()
#undef ZSTDGPU_KERNEL
    kzstdgpu_CompiledShaderId_Count,
    kzstdgpu_CompiledShaderId_MaxInt = 0x7fffffff
} zstdgpu_CompiledShaderId;

typedef struct zstdgpu_CompiledShader
{
    const void    *code;
    const uint32_t size;
    const wchar_t *desc;
} zstdgpu_CompiledShader;

static const zstdgpu_CompiledShader kzstdgpu_CompiledShaders [] =
{
#define ZSTDGPU_KERNEL(name, desc) { g_ZstdGpu##name, sizeof(g_ZstdGpu##name), desc },
    ZSTDGPU_KERNEL_LIST()
#undef ZSTDGPU_KERNEL
};

#define ZSTDGPU_DISPATCH32_CMD_SIG_LIST()                       \
    ZSTDGPU_DISPATCH32_CMD_SIG(ComputeDestBlockOffsets  , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(DecodeHuffmanWeights     , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(DecompressHuffmanWeights , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(DecompressLiterals       , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(DecompressSequences      , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(FinaliseSequenceOffsets  , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(InitFseTable             , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(InitHuffmanTable         , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(Memset                   , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(MemsetMemcpy             , 4)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(PrefixSequenceOffsets    , 10)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(PrefixSum                , 2)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(ComputePrefixSum         , 5)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(ParseCompressedBlocks    , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(PropagateFseIndex        , 0)

#define ZSTDGPU_RUNTIME_KERNEL_LIST_SHARED()        \
    ZSTDGPU_KERNEL(ComputeDestBlockOffsets)         \
    ZSTDGPU_KERNEL(ComputeDestSequenceOffsets)      \
    ZSTDGPU_KERNEL(ComputePrefixSum)                \
    ZSTDGPU_KERNEL(DecodeHuffmanWeights)            \
    ZSTDGPU_KERNEL(DecompressHuffmanWeights)        \
    ZSTDGPU_KERNEL(FinaliseSequenceOffsets)         \
    ZSTDGPU_KERNEL(InitFseTable)                    \
    ZSTDGPU_KERNEL(InitHuffmanTable)                \
    ZSTDGPU_KERNEL(InitResources)                   \
    ZSTDGPU_KERNEL(Memset)                          \
    ZSTDGPU_KERNEL(MemsetMemcpy)                    \
    ZSTDGPU_KERNEL(ParseCompressedBlocks)           \
    ZSTDGPU_KERNEL(ParseFrames)                     \
    ZSTDGPU_KERNEL(PrefixSequenceOffsets)           \
    ZSTDGPU_KERNEL(PrefixSum)                       \
    ZSTDGPU_KERNEL(PropagateFseIndex)               \
    ZSTDGPU_KERNEL(UpdateDispatchArgs)

#define ZSTDGPU_RUNTIME_KERNEL_LIST_SPECIALISED()   \
    ZSTDGPU_KERNEL(DecompressLiterals)              \
    ZSTDGPU_KERNEL(DecompressSequences)             \
    ZSTDGPU_KERNEL(ExecuteSequences)

#define ZSTDGPU_RUNTIME_KERNEL_LIST()           \
    ZSTDGPU_RUNTIME_KERNEL_LIST_SHARED()        \
    ZSTDGPU_RUNTIME_KERNEL_LIST_SPECIALISED()

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0() \
    ZSTDGPU_KERNEL_SCOPE_X(InitResources_CountBlocks            , L"Init Resources"             )   \
    ZSTDGPU_KERNEL_SCOPE_X(ParseFrames_CountBlocks              , L"Parse Frames"               )   \
    ZSTDGPU_KERNEL_SCOPE_X(PrefixSum                            , L"Prefix Sums"                )   \
    ZSTDGPU_KERNEL_SCOPE_X(UpdateDispatchArgs_Stage0            , L"UpdateDispatchArgs:: Stage0")

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1() \
    ZSTDGPU_KERNEL_SCOPE_X(InitResources                        , L"Init Resources"             )   \
    ZSTDGPU_KERNEL_SCOPE_X(ParseFrames                          , L"Parse Frames"               )   \
    ZSTDGPU_KERNEL_SCOPE_X(ParseCompressedBlocks                , L"Parse Compressed Blocks"    )   \
    ZSTDGPU_KERNEL_SCOPE_X(PropagateFseIndex                    , L"Propagate FSE Index"        )   \
    ZSTDGPU_KERNEL_SCOPE_X(UpdateDispatchArgs_Stage1            , L"UpdateDispatchArgs:: Stage1")

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2() \
    ZSTDGPU_KERNEL_SCOPE_X(UpdateDispatchArgs_DecompressLiterals, L"UpdateDispatchArgs:: Stage2")   \
    ZSTDGPU_KERNEL_SCOPE_X(ComputePrefixSum                     , L"Compute Prefix Sums"        )   \
    ZSTDGPU_KERNEL_SCOPE_X(InitFseTable                         , L"Init FSE Tables"            )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecompressHuffmanWeights             , L"Decompress Huffman Weights" )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecodeHuffmanWeights                 , L"Decode Huffman Weights"     )   \
    ZSTDGPU_KERNEL_SCOPE_X(InitHuffmanTable                     , L"Init Huffman Table"         )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecompressLiterals                   , L"Decompress Literals"        )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecompressSequences                  , L"Decompress Sequences"       )   \
    ZSTDGPU_KERNEL_SCOPE_X(PrefixSequenceOffsets                , L"Propagate Sequence Offsets" )   \
    ZSTDGPU_KERNEL_SCOPE_X(FinaliseSequenceOffsets              , L"Finalise Sequence Offsets"  )   \
    ZSTDGPU_KERNEL_SCOPE_X(ComputeDestBlockOffsets              , L"Compute Dest Block Offsets" )   \
    ZSTDGPU_KERNEL_SCOPE_X(ExecuteSequences                     , L"ExecuteSequences"           )   \
    ZSTDGPU_KERNEL_SCOPE_X(MemcpyRAW_MemsetRLE                  , L"Memcpy Raw/Memset RLE Blocks")  \
    ZSTDGPU_KERNEL_SCOPE_X(PrefixBlockSizes                     , L"Prefix Block Sizes"         )

#define ZSTDGPU_KERNEL_SCOPE_LIST()     \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0() \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1() \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2()

enum zstdgpu_KernelScopeId
{
    kzstdgpu_KernelScope_Stage0_Start,
    kzstdgpu_KernelScope_Stage0_End,
    kzstdgpu_KernelScope_Stage1_Start,
    kzstdgpu_KernelScope_Stage1_End,
    kzstdgpu_KernelScope_Stage2_Start,
    kzstdgpu_KernelScope_Stage2_End,

#define ZSTDGPU_KERNEL_SCOPE_X(name, desc) kzstdgpu_KernelScope_##name,
    ZSTDGPU_KERNEL_SCOPE_LIST()
#undef  ZSTDGPU_KERNEL_SCOPE_X

    kzstdgpu_KernelScope_Count,

#define ZSTDGPU_KERNEL_SCOPE_X(name, desc) + 1
    kzstdgpu_KernelScope_Stage0Count = 0 + (ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0()),
    kzstdgpu_KernelScope_Stage1Count = 0 + (ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1()),
    kzstdgpu_KernelScope_Stage2Count = 0 + (ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2()),
#undef  ZSTDGPU_KERNEL_SCOPE_X

    kzstdgpu_KernelScope_StageSlotCount = kzstdgpu_KernelScope_Stage2_End + 1,
    kzstdgpu_KernelScope_Stage0Start = kzstdgpu_KernelScope_StageSlotCount,
    kzstdgpu_KernelScope_Stage1Start = kzstdgpu_KernelScope_Stage0Start + kzstdgpu_KernelScope_Stage0Count,
    kzstdgpu_KernelScope_Stage2Start = kzstdgpu_KernelScope_Stage1Start + kzstdgpu_KernelScope_Stage1Count,

    kzstdgpu_KernelScope_ForceInt    = 0x3fffffff
};

static const wchar_t * kzstdgpu_KernelScopeDesc[] =
{
#define ZSTDGPU_KERNEL_SCOPE_X(name, desc) desc,
    ZSTDGPU_KERNEL_SCOPE_LIST()
#undef  ZSTDGPU_KERNEL_SCOPE_X
};

struct zstdgpu_PersistentContextImpl
{
    void                    *thisMemoryBlock;
    ID3D12Device            *device;
    ID3D12CommandSignature  *dispatchCmdSig;

    #define ZSTDGPU_DISPATCH32_CMD_SIG(name, rootParamIdx) ID3D12CommandSignature *name##_CmdSig;
        ZSTDGPU_DISPATCH32_CMD_SIG_LIST()
    #undef ZSTDGPU_DISPATCH32_CMD_SIG

    #define ZSTDGPU_KERNEL(name) d3d12aid_ComputeRsPs name;
        ZSTDGPU_RUNTIME_KERNEL_LIST()
    #undef ZSTDGPU_KERNEL
    uint32_t                DecompressLiterals_LdsStoreCache_StreamsPerGroup;
    uint32_t                DecompressSequences_StreamsPerGroup;
};

static const uint32_t kzstdgpu_SetupFlags_InputsCpuMemory       = (1u << 0);
static const uint32_t kzstdgpu_SetupFlags_InputsGpuMemory       = (1u << 1);
static const uint32_t kzstdgpu_SetupFlags_HasFrameInfoConstants = (1u << 2);
static const uint32_t kzstdgpu_SetupFlags_HasBlockInfoConstants = (1u << 3);
static const uint32_t kzstdgpu_SetupFlags_HasSingleSubmission   = (1u << 4);

static const uint32_t kzstdgpu_SetupFlags_InputsMask = kzstdgpu_SetupFlags_InputsCpuMemory | kzstdgpu_SetupFlags_InputsGpuMemory;

static uint32_t zstdgpu_HasFlag(uint32_t flags, uint32_t flag) { return (flags & flag) != 0; }

struct zstdgpu_PerRequestContextImpl
{
    void                    *thisMemoryBlock;
    ID3D12Device            *device;
    ID3D12CommandSignature  *dispatchCmdSig;

    #define ZSTDGPU_DISPATCH32_CMD_SIG(name, rootParamIdx) ID3D12CommandSignature *name##_CmdSig;
        ZSTDGPU_DISPATCH32_CMD_SIG_LIST()
    #undef ZSTDGPU_DISPATCH32_CMD_SIG

    #define ZSTDGPU_KERNEL(name) d3d12aid_ComputeRsPs name;
        ZSTDGPU_RUNTIME_KERNEL_LIST()
    #undef ZSTDGPU_KERNEL
    uint32_t                DecompressLiterals_LdsStoreCache_StreamsPerGroup;
    uint32_t                DecompressSequences_StreamsPerGroup;

    zstdgpu_SRTs            srts;
    zstdgpu_ResourceDataGpu resData;
    zstdgpu_ResourceInfo    resInfo;

    zstdgpu_UploadFrames   *uploadCallback;
    void                   *uploadUserdata;
    ID3D12Resource         *compressedFramesData;
    ID3D12Resource         *compressedFramesRefs;
    ID3D12Resource         *uncompressedFramesData;
    ID3D12Resource         *uncompressedFramesRefs;

    d3d12aid_Timestamps     timestamps;

    uint32_t                timestampSlot[kzstdgpu_KernelScope_Count];

    uint32_t                zstdFrameCount;
    uint32_t                zstdCompressedFramesByteCount;
    uint32_t                zstdUncompressedFrameCount;
    uint32_t                zstdUncompressedFramesByteCount;

    /**
     *  These the maximal number of blocks of each kind the context can process
     *
     *  Option A:
     *  If `zstdgpu_SetupFrameInfoConstants` was called on the context and these numbers
     *  were provided by the calling code, these numbers are used internally to conservatively
     *  allocate memory (so that specified number of blocks fit) and partition it statically
     *  into buffers/views.
     *
     *  Option B:
     *  If `zstdgpu_SetupFrameInfoConstants` was NOT called, these numbers are originally
     *  set to zero in `zstdgpu_CreatePerRequestContext`, meaning they can't be used to allocate
     *  memory until Zstd frames are parsed and the number of blocks read back to CPU
     *  either through `zstdgpu_SubmitWithInteralMemory` or `zstdgpu_GetGpuMemoryRequirement`
     */
    uint32_t                zstdRawBlockCountMax;
    uint32_t                zstdRleBlockCountMax;
    uint32_t                zstdCmpBlockCountMax;

    uint32_t                zstdUncompressedLitByteCountMax;
    uint32_t                zstdUncompressedSeqElemCountMax;

    uint32_t                setupFlags;
};


uint32_t zstdgpu_GetPersistentContextRequiredMemorySizeInBytes(void)
{
    return sizeof(zstdgpu_PersistentContextImpl);
}

uint32_t zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes(void)
{
    return sizeof(zstdgpu_PerRequestContextImpl);
}

ZSTDGPU_ENUM(Status) zstdgpu_CreatePersistentContext(zstdgpu_PersistentContext *outPersistentContext, ID3D12Device *device, void *memoryBlock, uint32_t memoryBlockSizeInBytes)
{
    uint32_t proceed = 1;

    proceed = proceed && (NULL != outPersistentContext);
    proceed = proceed && (NULL != device);
    proceed = proceed && (NULL != memoryBlock);
    proceed = proceed && (memoryBlockSizeInBytes >= zstdgpu_GetPersistentContextRequiredMemorySizeInBytes());

    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed > 0)
    {
        zstdgpu_PersistentContextImpl *context = (zstdgpu_PersistentContextImpl *)memoryBlock;
        context->thisMemoryBlock = memoryBlock;

        context->device = device;
        device->AddRef();

        D3D12_INDIRECT_ARGUMENT_DESC dispatchArgDesc[2];
        dispatchArgDesc[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

        D3D12_COMMAND_SIGNATURE_DESC cmdSigDesc;
        cmdSigDesc.ByteStride       = sizeof(uint32_t) * 3;
        cmdSigDesc.NumArgumentDescs = 1;
        cmdSigDesc.pArgumentDescs   = dispatchArgDesc;
        cmdSigDesc.NodeMask         = 0x1;
        D3D12AID_CHECK(device->CreateCommandSignature(&cmdSigDesc, NULL, D3D12AID_IID_PPV_ARGS(&context->dispatchCmdSig)));

        dispatchArgDesc[0].Type                              = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        dispatchArgDesc[0].Constant.DestOffsetIn32BitValues  = 0;
        dispatchArgDesc[0].Constant.Num32BitValuesToSet      = 2;
        dispatchArgDesc[1].Type                              = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

        cmdSigDesc.ByteStride        = sizeof(uint32_t) * kzstdgpu_DispatchSlot_CmdStrideInUInt32;
        cmdSigDesc.NumArgumentDescs  = 2;

        #define ZSTDGPU_KERNEL_GET(name) &kzstdgpu_CompiledShaders[kzstdgpu_CompiledShaderId_##name];
        #define ZSTDGPU_KERNEL_MAP(runtime, compiled) shader##runtime = ZSTDGPU_KERNEL_GET(compiled)

        #define ZSTDGPU_KERNEL(name) const zstdgpu_CompiledShader *shader##name = ZSTDGPU_KERNEL_GET(name);
            ZSTDGPU_RUNTIME_KERNEL_LIST_SHARED()
        #undef ZSTDGPU_KERNEL

        #define ZSTDGPU_KERNEL(name) const zstdgpu_CompiledShader *shader##name = NULL;
            ZSTDGPU_RUNTIME_KERNEL_LIST_SPECIALISED()
        #undef ZSTDGPU_KERNEL

#if defined(_GAMING_XBOX_SCARLETT)
        ZSTDGPU_KERNEL_MAP(DecompressLiterals, DecompressLiterals_LdsStoreCache32_16);
        context->DecompressLiterals_LdsStoreCache_StreamsPerGroup = 16;
        ZSTDGPU_KERNEL_MAP(DecompressSequences, DecompressSequences_SingleStream_LdsFseCache32);
        context->DecompressSequences_StreamsPerGroup = 1;
        ZSTDGPU_KERNEL_MAP(ExecuteSequences, ExecuteSequences64);
#else
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 featureOptions1;
        D3D12AID_CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, ZSTDGPU_WARN_DISABLE_MSVC(6001, &featureOptions1), sizeof(featureOptions1)));
        ZSTDGPU_ASSERT(featureOptions1.Int64ShaderOps); // 64-bit integer shader ops required for Forward_BitBuffer

        const LUID luid = device->GetAdapterLuid();

        IDXGIAdapter* adapter = NULL;
        IDXGIFactory4 *factory = NULL;
        CreateDXGIFactory2(0, D3D12AID_IID_PPV_ARGS(&factory));
        D3D12AID_CHECK(factory->EnumAdapterByLuid(luid, D3D12AID_IID_PPV_ARGS(&adapter)));
        D3D12AID_SAFE_RELEASE(factory);

        DXGI_ADAPTER_DESC desc;
        D3D12AID_CHECK(adapter->GetDesc(&desc));
        D3D12AID_SAFE_RELEASE(adapter);

        if (desc.VendorId == 0x1002)
        {
            ZSTDGPU_KERNEL_MAP(DecompressLiterals, DecompressLiterals_LdsStoreCache64_16);
            context->DecompressLiterals_LdsStoreCache_StreamsPerGroup = 16;
            ZSTDGPU_KERNEL_MAP(DecompressSequences, DecompressSequences_SingleStream_ScalarFseLoad32);
            context->DecompressSequences_StreamsPerGroup = 1;
            ZSTDGPU_KERNEL_MAP(ExecuteSequences, ExecuteSequences64);
        }
        else if (desc.VendorId == 0x10de)
        {
            // Nvidia
            ZSTDGPU_KERNEL_MAP(DecompressLiterals, DecompressLiterals_LdsStoreCache32_16);
            context->DecompressLiterals_LdsStoreCache_StreamsPerGroup = 16;

            // NOTE(pamartis): Enable multi-stream variant by default. This variant outperforms single-stream
            // variant in cases when the number of sequence streams large enough so GPU becomes fully saturated
            // with threadgroups/waves running single-stream shader. On the other side, because single-stream
            // variant waves/threadgroups are shorter individually, workloads not saturating GPU would perform
            // faster with single-stream variant and multi-stream version would be a pessimisation.
            //
            // But we choose "throughput" maximising kernel.
            ZSTDGPU_KERNEL_MAP(DecompressSequences, DecompressSequences_MultiStream_4_LdsOutCache_32);
            context->DecompressSequences_StreamsPerGroup = kzstdgpu_TgSizeX_DecompressSequences / 4u;

            ZSTDGPU_KERNEL_MAP(ExecuteSequences, ExecuteSequences64);
        }
        else if (featureOptions1.WaveLaneCountMax == 128)
        {
            ZSTDGPU_KERNEL_MAP(DecompressLiterals, DecompressLiterals_LdsStoreCache128_8);
            context->DecompressLiterals_LdsStoreCache_StreamsPerGroup = 8;
            ZSTDGPU_KERNEL_MAP(DecompressSequences, DecompressSequences_SingleStream_LdsFseCache128);
            context->DecompressSequences_StreamsPerGroup = 1;
            ZSTDGPU_KERNEL_MAP(ExecuteSequences, ExecuteSequences128);
        }
        else //if (desc.VendorId == 0x8086 || featureOptions1.WaveLaneCountMax == 32)
        {
            ZSTDGPU_KERNEL_MAP(DecompressLiterals, DecompressLiterals_LdsStoreCache32_16);
            context->DecompressLiterals_LdsStoreCache_StreamsPerGroup = 16;
            ZSTDGPU_KERNEL_MAP(DecompressSequences, DecompressSequences_SingleStream_LdsFseCache32);
            context->DecompressSequences_StreamsPerGroup = 1;
            ZSTDGPU_KERNEL_MAP(ExecuteSequences, ExecuteSequences32);
        }
#endif
        #undef ZSTDGPU_KERNEL_GET
        #undef ZSTDGPU_KERNEL_MAP

        /** NOTE(pamartis): generate PipelineState / RootSignature initialisation through macro list */
        #define ZSTDGPU_KERNEL(name) \
            d3d12aid_ComputeRsPs_Create(&context->name, device, shader##name->code, shader##name->size);\
            context->name.rs->SetName(shader##name->desc);\
            context->name.ps->SetName(shader##name->desc);
            ZSTDGPU_RUNTIME_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL

        /** NOTE(pamartis): generate CommandSignatures through macro list specifying what kernels/root signatures need command signatures for indirect dispatch */
        #define ZSTDGPU_DISPATCH32_CMD_SIG(name, rootParamIdx)              \
            dispatchArgDesc[0].Constant.RootParameterIndex = rootParamIdx;  \
            D3D12AID_CHECK(device->CreateCommandSignature(&cmdSigDesc, context->name.rs, D3D12AID_IID_PPV_ARGS(&context->name##_CmdSig)));

            ZSTDGPU_DISPATCH32_CMD_SIG_LIST()
        #undef ZSTDGPU_DISPATCH32_CMD_SIG

        *outPersistentContext = context;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}


ZSTDGPU_ENUM(Status) zstdgpu_DestroyPersistentContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PersistentContext inPersistentContext)
{
    const uint32_t proceed = inPersistentContext->thisMemoryBlock == (void *)inPersistentContext;
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed > 0)
    {
        #define ZSTDGPU_KERNEL(name) d3d12aid_ComputeRsPs_Release(&inPersistentContext->name);
            ZSTDGPU_RUNTIME_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL

        D3D12AID_SAFE_RELEASE(inPersistentContext->dispatchCmdSig);

        #define ZSTDGPU_DISPATCH32_CMD_SIG(name, rootParamIdx) D3D12AID_SAFE_RELEASE(inPersistentContext->name##_CmdSig);
            ZSTDGPU_DISPATCH32_CMD_SIG_LIST()
        #undef ZSTDGPU_DISPATCH32_CMD_SIG

        D3D12AID_SAFE_RELEASE(inPersistentContext->device);

        if (NULL != outMemoryBlock)
            *outMemoryBlock = inPersistentContext->thisMemoryBlock;

        if (NULL != outMemoryBlockSizeInBytes)
            *outMemoryBlockSizeInBytes = zstdgpu_GetPersistentContextRequiredMemorySizeInBytes();

        inPersistentContext->thisMemoryBlock = NULL;

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_CreatePerRequestContext(zstdgpu_PerRequestContext *outPerRequestContext, zstdgpu_PersistentContext persistentContext, void *memoryBlock, uint32_t memoryBlockSizeInBytes)
{
    uint32_t proceed = 1;

    proceed = proceed && (NULL != outPerRequestContext);
    proceed = proceed && (NULL != persistentContext);
    proceed = proceed && (NULL != memoryBlock);
    proceed = proceed && (memoryBlockSizeInBytes >= zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes());
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed > 0)
    {
        zstdgpu_PerRequestContextImpl *context = (zstdgpu_PerRequestContextImpl *)memoryBlock;
        context->thisMemoryBlock = memoryBlock;

        context->device = persistentContext->device;
        context->device->AddRef();

        context->dispatchCmdSig = persistentContext->dispatchCmdSig;
        context->dispatchCmdSig->AddRef();

        #define ZSTDGPU_DISPATCH32_CMD_SIG(name, rootParamIdx)                      \
            context->name##_CmdSig = persistentContext->name##_CmdSig;    \
            context->name##_CmdSig->AddRef();
            ZSTDGPU_DISPATCH32_CMD_SIG_LIST()
        #undef ZSTDGPU_DISPATCH32_CMD_SIG

        /** NOTE(pamartis): generate PipelineState / RootSignature initialisation through macro list */
        #define ZSTDGPU_KERNEL(name)                \
            context->name = persistentContext->name;\
            context->name.rs->AddRef();             \
            context->name.ps->AddRef();
            ZSTDGPU_RUNTIME_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL
        context->DecompressLiterals_LdsStoreCache_StreamsPerGroup = persistentContext->DecompressLiterals_LdsStoreCache_StreamsPerGroup;
        context->DecompressSequences_StreamsPerGroup = persistentContext->DecompressSequences_StreamsPerGroup;

        context->srts.heap = NULL;
        context->srts.heapOffset = 0;
        zstdgpu_ResourceDataGpu_InitZero(&context->resData);
        zstdgpu_ResourceInfo_InitZero(&context->resInfo);

        context->uploadCallback                     = NULL;
        context->uploadUserdata                     = NULL;
        context->compressedFramesData               = NULL;
        context->compressedFramesRefs               = NULL;
        context->uncompressedFramesData             = NULL;
        context->uncompressedFramesRefs             = NULL;

        d3d12aid_Timestamps_Create(&context->timestamps, context->device, kzstdgpu_KernelScope_Count * 2, 1);

        context->zstdFrameCount                     = 0;
        context->zstdCompressedFramesByteCount      = 0;

        context->zstdUncompressedFrameCount         = 0;
        context->zstdUncompressedFramesByteCount    = 0;

        context->zstdRawBlockCountMax               = 0;
        context->zstdRleBlockCountMax               = 0;
        context->zstdCmpBlockCountMax               = 0;
        context->zstdUncompressedLitByteCountMax    = 0;
        context->zstdUncompressedSeqElemCountMax    = 0;
        context->setupFlags = 0;

        *outPerRequestContext = context;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_DestroyPerRequestContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PerRequestContext inPerRequestContext)
{
    const uint32_t proceed = inPerRequestContext->thisMemoryBlock == (void *)inPerRequestContext;
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        d3d12aid_Timestamps_Release(&inPerRequestContext->timestamps);

        const uint32_t stageCount = ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount);
        for (uint32_t stage = 0; stage < stageCount; ++stage)
        {
            zstdgpu_ResourceDataGpu_Term(&inPerRequestContext->resData, stage);
        }

        D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesData);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesRefs);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->uncompressedFramesData);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->uncompressedFramesRefs);

        D3D12AID_SAFE_RELEASE(inPerRequestContext->srts.heap);

        #define ZSTDGPU_KERNEL(name) d3d12aid_ComputeRsPs_Release(&inPerRequestContext->name);
            ZSTDGPU_RUNTIME_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL

        D3D12AID_SAFE_RELEASE(inPerRequestContext->dispatchCmdSig);

        #define ZSTDGPU_DISPATCH32_CMD_SIG(name, rootParamIdx) D3D12AID_SAFE_RELEASE(inPerRequestContext->name##_CmdSig);
            ZSTDGPU_DISPATCH32_CMD_SIG_LIST()
        #undef ZSTDGPU_DISPATCH32_CMD_SIG

        D3D12AID_SAFE_RELEASE(inPerRequestContext->device);

        if (NULL != outMemoryBlock)
            *outMemoryBlock = inPerRequestContext->thisMemoryBlock;

        if (NULL != outMemoryBlockSizeInBytes)
            *outMemoryBlockSizeInBytes = zstdgpu_GetPersistentContextRequiredMemorySizeInBytes();

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SetupInputsAsFramesInCpuMemory(uint32_t *outStageCount, zstdgpu_PerRequestContext inPerRequestContext, uint32_t frameCount, uint32_t framesMemorySizeInBytes, zstdgpu_UploadFrames *uploadCallback, void *uploadUserdata)
{
    uint32_t proceed = 1;
    proceed = proceed && (inPerRequestContext->thisMemoryBlock == (void *)inPerRequestContext);
    proceed = proceed && (NULL != outStageCount);
    proceed = proceed && (frameCount > 0);
    proceed = proceed && (framesMemorySizeInBytes > 0);
    proceed = proceed && (NULL != uploadCallback);
    proceed = proceed && (NULL != uploadUserdata);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        if (zstdgpu_HasFlag(inPerRequestContext->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory))
        {
            // NOTE(pamartis): we only release ID3D12Resource with the PerRequest context.
            // ID3D12Resource within zstdgpu_ResourceDataGpu will be released with the zstdgpu_ResourceDataGpu_Term call.
            // which is going to be executed either by one of Submit functions or by destroying PerRequest context
            D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesData);
            D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesRefs);
        }

        inPerRequestContext->uploadCallback                 = uploadCallback;
        inPerRequestContext->uploadUserdata                 = uploadUserdata;
        inPerRequestContext->compressedFramesData           = NULL;
        inPerRequestContext->compressedFramesRefs           = NULL;
        inPerRequestContext->zstdFrameCount                 = frameCount;
        inPerRequestContext->zstdCompressedFramesByteCount  = framesMemorySizeInBytes;
        inPerRequestContext->setupFlags = (inPerRequestContext->setupFlags & ~kzstdgpu_SetupFlags_InputsMask) | kzstdgpu_SetupFlags_InputsCpuMemory;

        *outStageCount = 3u;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SetupInputsAsFramesInGpuMemory(uint32_t *outStageCount, zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *framesMemory, uint32_t framesMemorySizeInBytes, struct ID3D12Resource *frames, uint32_t frameCount)
{
    uint32_t proceed = 1;
    proceed = proceed && (inPerRequestContext->thisMemoryBlock == (void *)inPerRequestContext);
    proceed = proceed && (NULL != outStageCount);
    proceed = proceed && (frameCount > 0);
    proceed = proceed && (framesMemorySizeInBytes > 0);
    proceed = proceed && (NULL != framesMemory);
    proceed = proceed && (NULL != frames);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        if (zstdgpu_HasFlag(inPerRequestContext->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory))
        {
            // NOTE(pamartis): we only release ID3D12Resource with the PerRequest context.
            // ID3D12Resource within zstdgpu_ResourceDataGpu will be released with the zstdgpu_ResourceDataGpu_Term call.
            // which is going to be executed either by one of Submit functions or by destroying PerRequest context
            D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesData);
            D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesRefs);
        }

        inPerRequestContext->uploadCallback                 = NULL;
        inPerRequestContext->uploadUserdata                 = NULL;
        inPerRequestContext->compressedFramesData           = framesMemory;
        inPerRequestContext->compressedFramesData->AddRef();
        inPerRequestContext->compressedFramesRefs           = frames;
        inPerRequestContext->compressedFramesRefs->AddRef();
        inPerRequestContext->zstdFrameCount                 = frameCount;
        inPerRequestContext->zstdCompressedFramesByteCount  = framesMemorySizeInBytes;
        inPerRequestContext->setupFlags = (inPerRequestContext->setupFlags & ~kzstdgpu_SetupFlags_InputsMask) | kzstdgpu_SetupFlags_InputsGpuMemory;

        *outStageCount = 3u;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_API ZSTDGPU_ENUM(Status) zstdgpu_SetupOutputs(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *framesMemory, uint32_t framesMemorySizeInBytes, struct ID3D12Resource *frames, uint32_t frameCount)
{
    uint32_t proceed = 1;
    proceed = proceed && (inPerRequestContext->thisMemoryBlock == (void *)inPerRequestContext);
    proceed = proceed && (frameCount > 0);
    proceed = proceed && (framesMemorySizeInBytes > 0);
    proceed = proceed && (NULL != framesMemory);
    proceed = proceed && (NULL != frames);
    ZSTDGPU_ASSERT(proceed > 0);
    if (proceed)
    {
        // NOTE(pamartis): we only release ID3D12Resource with the PerRequest context.
        // ID3D12Resource within zstdgpu_ResourceDataGpu will be released with the zstdgpu_ResourceDataGpu_Term call.
        // which is going to be executed either by one of Submit functions or by destroying PerRequest context
        D3D12AID_SAFE_RELEASE(inPerRequestContext->uncompressedFramesData);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->uncompressedFramesRefs);

        inPerRequestContext->uncompressedFramesData = framesMemory;
        inPerRequestContext->uncompressedFramesData->AddRef();

        inPerRequestContext->uncompressedFramesRefs = frames;
        inPerRequestContext->uncompressedFramesRefs->AddRef();

        inPerRequestContext->zstdUncompressedFrameCount         = frameCount;
        inPerRequestContext->zstdUncompressedFramesByteCount    = framesMemorySizeInBytes;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SetupAllStageSubmission(zstdgpu_PerRequestContext req)
{
    uint32_t proceed = 1;
    proceed = proceed && (NULL != req);
    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        req->setupFlags |= kzstdgpu_SetupFlags_HasSingleSubmission;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SetupFrameInfoConstants(zstdgpu_PerRequestContext inPerRequestContext, uint32_t rawBlockCount, uint32_t rleBlockCount, uint32_t cmpBlockCount)
{
    uint32_t proceed = 1;
    proceed = proceed && (inPerRequestContext->thisMemoryBlock == (void *)inPerRequestContext);
    proceed = proceed && (0 != rawBlockCount + rleBlockCount + cmpBlockCount);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        inPerRequestContext->zstdRawBlockCountMax = zstdgpu_MaxU32(rawBlockCount, kzstdgpu_MinCount_Blocks);
        inPerRequestContext->zstdRleBlockCountMax = zstdgpu_MaxU32(rleBlockCount, kzstdgpu_MinCount_Blocks);
        inPerRequestContext->zstdCmpBlockCountMax = zstdgpu_MaxU32(cmpBlockCount, kzstdgpu_MinCount_Blocks);
        inPerRequestContext->setupFlags |= kzstdgpu_SetupFlags_HasFrameInfoConstants;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SetupBlockInfoConstants(zstdgpu_PerRequestContext inPerRequestContext, uint32_t literalsByteCount, uint32_t sequenceCount)
{
    uint32_t proceed = 1;
    proceed = proceed && (inPerRequestContext->thisMemoryBlock == (void *)inPerRequestContext);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        inPerRequestContext->zstdUncompressedLitByteCountMax = zstdgpu_MaxU32(literalsByteCount, kzstdgpu_MinCount_UncompressedLitBytes);
        inPerRequestContext->zstdUncompressedSeqElemCountMax = zstdgpu_MaxU32(sequenceCount, kzstdgpu_MinCount_UncompressedSeqElems);
        inPerRequestContext->setupFlags |= kzstdgpu_SetupFlags_HasBlockInfoConstants;
        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

uint32_t zstdgpu_IsReadbackRequired(zstdgpu_PerRequestContext req, uint32_t stageIndex)
{
    uint32_t reqFlags = 0;

    if (stageIndex == 0)
    {
        reqFlags = kzstdgpu_SetupFlags_HasFrameInfoConstants | kzstdgpu_SetupFlags_HasSingleSubmission;
    }
    else if (stageIndex == 1)
    {
        reqFlags = kzstdgpu_SetupFlags_HasBlockInfoConstants | kzstdgpu_SetupFlags_HasSingleSubmission;
    }

    if (stageIndex <= 1)
    {
        return 0 != (req->setupFlags & reqFlags) ? 0 : 1;
    }
    return 0;
}

uint32_t zstdgpu_IsAnyStageReadbackRequired(zstdgpu_PerRequestContext inPerRequestContext)
{
    uint32_t reqFlags = kzstdgpu_SetupFlags_HasFrameInfoConstants | kzstdgpu_SetupFlags_HasBlockInfoConstants;
    if (inPerRequestContext->setupFlags & kzstdgpu_SetupFlags_HasSingleSubmission)
    {
        return 0;
    }
    else if (reqFlags == (inPerRequestContext->setupFlags & reqFlags))
    {
        return 0;
    }
    return 1;
}

static uint32_t zstdgpu_OutputSizeToBlockCount(uint32_t size)
{
    // NOTE(pamartis): We compute the number of 4KiB blocks -- which is the minimal size of the block
    // standard ZSTD compressor uses.
    return (size + 4095) >> 12;
}

static uint32_t zstdgpu_OutputSizeToSequenceCount(uint32_t size)
{
    // NOTE(pamartis): 8 bytes per sequence is emperical estimation, not something stipulated
    // by ZSTD standard.
    return size >> 3;
}

static void zstdgpu_RecomputeAndRetrieveFrameInfoConstants(uint32_t *outCntRaw, uint32_t *outCntRle, uint32_t *outCntCmp, zstdgpu_PerRequestContext req)
{
    uint32_t cntRaw, cntRle, cntCmp;
    if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_HasFrameInfoConstants))
    {
        ZSTDGPU_ASSERT(req->zstdRawBlockCountMax >= kzstdgpu_MinCount_Blocks);
        ZSTDGPU_ASSERT(req->zstdRleBlockCountMax >= kzstdgpu_MinCount_Blocks);
        ZSTDGPU_ASSERT(req->zstdCmpBlockCountMax >= kzstdgpu_MinCount_Blocks);

        cntRaw = req->zstdRawBlockCountMax;
        cntRle = req->zstdRleBlockCountMax;
        cntCmp = req->zstdCmpBlockCountMax;
    }
    else
    {
        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_HasSingleSubmission))
        {
            // NOTE(pamartis): The estimation is conservative and therefore can result in insufficient memory
            cntRle = cntRaw = cntCmp = zstdgpu_OutputSizeToBlockCount(req->zstdUncompressedFramesByteCount);
        }
        else
        {
            #define CNTRS(name) req->resData.gpu2Cpu.CountersCpu->name
            cntRaw = CNTRS(Blocks_RAW);
            cntRle = CNTRS(Blocks_RLE);
            cntCmp = CNTRS(Blocks_CMP);
            #undef CNTRS
        }
        // NOTE(pamartis): we clamp constants to `kzstdgpu_MinCount_Blocks` to make sure buffers are always allocated
        // and are never `NULL` so submission code doesn't need to check for NULL.
        // We do "Max" counts adjustment here and not on per-buffer level because doing this
        // per-buffer would be prone to errors when adding new buffers/changing between SoA/AoS / etc.
        cntRaw = zstdgpu_MaxU32(cntRaw, kzstdgpu_MinCount_Blocks);
        cntRle = zstdgpu_MaxU32(cntRle, kzstdgpu_MinCount_Blocks);
        cntCmp = zstdgpu_MaxU32(cntCmp, kzstdgpu_MinCount_Blocks);

        req->zstdRawBlockCountMax = cntRaw;
        req->zstdRleBlockCountMax = cntRle;
        req->zstdCmpBlockCountMax = cntCmp;
    }

    ZSTDGPU_ASSERT(0 != cntRaw + cntRle + cntCmp);

    *outCntRaw = cntRaw;
    *outCntRle = cntRle;
    *outCntCmp = cntCmp;
}

static void zstdgpu_RecomputeAndRetrieveBlockInfoConstants(uint32_t *outCntLit, uint32_t *outCntSeq, zstdgpu_PerRequestContext req)
{
    uint32_t cntLit, cntSeq;
    if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_HasBlockInfoConstants))
    {
        ZSTDGPU_ASSERT(req->zstdUncompressedLitByteCountMax >= kzstdgpu_MinCount_UncompressedLitBytes);
        ZSTDGPU_ASSERT(req->zstdUncompressedSeqElemCountMax >= kzstdgpu_MinCount_UncompressedSeqElems);

        cntLit = req->zstdUncompressedLitByteCountMax;
        cntSeq = req->zstdUncompressedSeqElemCountMax;
    }
    else
    {
        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_HasSingleSubmission))
        {
            // NOTE(pamartis): it's a huge overestimate, but it's best we can do safely,
            // a single output byte requires 1 byte for literal storage
            cntLit = req->zstdUncompressedFramesByteCount;
            cntSeq = zstdgpu_OutputSizeToSequenceCount(req->zstdUncompressedFramesByteCount);
        }
        else
        {
            // NOTE(pamartis): this path is only triggered by multi-stage submission.
            // Block info constants are read back from GPU from previous stage.
            #define CNTRS(name) req->resData.gpu2Cpu.CountersCpu->name
            cntLit = CNTRS(HUF_Streams_DecodedBytes);
            cntSeq = CNTRS(Seq_Streams_DecodedItems);
            #undef CNTRS
        }

        // NOTE(pamartis): we clamp constants to `kzstdgpu_MinCount_Uncompressed{LitBytes,SeqElems}`
        // to make sure buffers are always allocated and are never `NULL` so submission code doesn't need to check for
        // NULL. We do "Max" counts adjustment here and not on per-buffer level because doing so per-buffer would be
        // prone to errors when adding new buffers/changing between SoA/AoS / etc.
        cntLit = zstdgpu_MaxU32(cntLit, kzstdgpu_MinCount_UncompressedLitBytes);
        cntSeq = zstdgpu_MaxU32(cntSeq, kzstdgpu_MinCount_UncompressedSeqElems);

        req->zstdUncompressedLitByteCountMax = cntLit;
        req->zstdUncompressedSeqElemCountMax = cntSeq;
    }
    *outCntSeq = cntSeq;
    *outCntLit = cntLit;
}

ZSTDGPU_ENUM(Status) zstdgpu_GetGpuMemoryRequirement(uint64_t *outDefaultHeapByteCount, uint64_t *outUploadHeapByteCount, uint64_t *outReadbackHeapByteCount, uint32_t *outShaderVisibleDescriptorCount, zstdgpu_PerRequestContext req, uint32_t stageIndex)
{
    uint32_t proceed = 1;
    uint32_t stageCount = ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount);

    proceed = proceed && (NULL != outDefaultHeapByteCount);
    proceed = proceed && (NULL != outUploadHeapByteCount);
    proceed = proceed && (NULL != outReadbackHeapByteCount);
    proceed = proceed && (NULL != outShaderVisibleDescriptorCount);
    proceed = proceed && (stageIndex < stageCount) && stageIndex < ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount);
    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    proceed = proceed && (req->zstdFrameCount > 0);
    proceed = proceed && (req->zstdCompressedFramesByteCount > 0);
    proceed = proceed && (req->zstdUncompressedFrameCount == req->zstdFrameCount);
    proceed = proceed && (req->zstdUncompressedFramesByteCount > 0);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        if (stageIndex == 0)
        {
            zstdgpu_ResourceInfo_Stage_0_Init(&req->resInfo, req->zstdFrameCount, req->zstdCompressedFramesByteCount, zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory) ? 1u : 0u);
        }
        else if (stageIndex == 1)
        {
            uint32_t cntRaw, cntRle, cntCmp;
            zstdgpu_RecomputeAndRetrieveFrameInfoConstants(&cntRaw, &cntRle, &cntCmp, req);
            zstdgpu_ResourceInfo_Stage_1_Init(&req->resInfo, cntRaw, cntRle, cntCmp);

        }
        else if (stageIndex == 2)
        {
            uint32_t cntLit, cntSeq;
            zstdgpu_RecomputeAndRetrieveBlockInfoConstants(&cntLit, &cntSeq, req);
            zstdgpu_ResourceInfo_Stage_2_Init(&req->resInfo, cntLit, cntSeq, req->zstdUncompressedFramesByteCount, req->zstdUncompressedFrameCount);
        }
        *outDefaultHeapByteCount            = req->resInfo.gpuOnly_ByteCount[stageIndex];
        *outUploadHeapByteCount             = req->resInfo.cpu2Gpu_ByteCount[stageIndex];
        *outReadbackHeapByteCount           = req->resInfo.gpu2Cpu_ByteCount[stageIndex];
        *outShaderVisibleDescriptorCount    = zstdgpu_Count_SRTs_Stage(stageIndex);

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

static void zstdgpu_GetAllStageGpuMemoryRequirementInternal(uint64_t *outDefaultHeapByteCount,
                                                            uint64_t *outUploadHeapByteCount,
                                                            uint64_t *outReadbackHeapByteCount,
                                                            zstdgpu_PerRequestContext req)
{
    uint32_t cntRaw, cntRle, cntCmp, cntLit, cntSeq;
    zstdgpu_ResourceInfo_Stage_0_Init(&req->resInfo, req->zstdFrameCount, req->zstdCompressedFramesByteCount, zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory) ? 1u : 0u);

    zstdgpu_RecomputeAndRetrieveFrameInfoConstants(&cntRaw, &cntRle, &cntCmp, req);
    zstdgpu_ResourceInfo_Stage_1_Init(&req->resInfo, cntRaw, cntRle, cntCmp);

    zstdgpu_RecomputeAndRetrieveBlockInfoConstants(&cntLit, &cntSeq, req);
    zstdgpu_ResourceInfo_Stage_2_Init(&req->resInfo, cntLit, cntSeq, req->zstdUncompressedFramesByteCount, req->zstdUncompressedFrameCount);

    *outDefaultHeapByteCount    = req->resInfo.gpuOnly_ByteCount[0]
                                + req->resInfo.gpuOnly_ByteCount[1]
                                + req->resInfo.gpuOnly_ByteCount[2];

    *outUploadHeapByteCount     = req->resInfo.cpu2Gpu_ByteCount[0]
                                + req->resInfo.cpu2Gpu_ByteCount[1]
                                + req->resInfo.cpu2Gpu_ByteCount[2];

    *outReadbackHeapByteCount   = req->resInfo.gpu2Cpu_ByteCount[0]
                                + req->resInfo.gpu2Cpu_ByteCount[1]
                                + req->resInfo.gpu2Cpu_ByteCount[2];
}

ZSTDGPU_ENUM(Status) zstdgpu_GetAllStageGpuMemoryRequirement(uint64_t *outDefaultHeapByteCount,
                                                             uint64_t *outUploadHeapByteCount,
                                                             uint64_t *outReadbackHeapByteCount,
                                                             uint32_t *outShaderVisibleDescriptorCount,
                                                             zstdgpu_PerRequestContext req)
{
    uint32_t proceed = 1;

    proceed = proceed && (NULL != outDefaultHeapByteCount);
    proceed = proceed && (NULL != outUploadHeapByteCount);
    proceed = proceed && (NULL != outReadbackHeapByteCount);
    proceed = proceed && (NULL != outShaderVisibleDescriptorCount);
    proceed = proceed && (NULL != req);
    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    proceed = proceed && (req->zstdFrameCount > 0);
    proceed = proceed && (req->zstdCompressedFramesByteCount > 0);
    proceed = proceed && (req->zstdUncompressedFrameCount == req->zstdFrameCount);
    proceed = proceed && (req->zstdUncompressedFramesByteCount > 0);
    proceed = proceed && (0 == zstdgpu_IsAnyStageReadbackRequired(req));
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        zstdgpu_GetAllStageGpuMemoryRequirementInternal(outDefaultHeapByteCount, outUploadHeapByteCount, outReadbackHeapByteCount, req);

        *outShaderVisibleDescriptorCount    = zstdgpu_Count_SRTs_Stage(0)
                                            + zstdgpu_Count_SRTs_Stage(1)
                                            + zstdgpu_Count_SRTs_Stage(2);

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

static void zstdgpu_SubmitStage0(zstdgpu_PerRequestContext inPerRequestContext, ID3D12GraphicsCommandList *cmdList);
static void zstdgpu_SubmitStage1(zstdgpu_PerRequestContext inPerRequestContext, ID3D12GraphicsCommandList *cmdList);
static void zstdgpu_SubmitStage2(zstdgpu_PerRequestContext inPerRequestContext, ID3D12GraphicsCommandList *cmdList);

ZSTDGPU_ENUM(Status) zstdgpu_SubmitWithExternalMemory(zstdgpu_PerRequestContext req,
                                                uint32_t stageIndex,
                                                struct ID3D12GraphicsCommandList *cmdList,
                                                struct ID3D12Heap *defaultHeap,
                                                uint64_t defaultHeapOffsetInBytes,
                                                struct ID3D12Heap *uploadHeap,
                                                uint64_t uploadHeapOffsetInBytes,
                                                struct ID3D12Heap *readbackHeap,
                                                uint64_t readbackHeap_OffsetInBytes,
                                                struct ID3D12DescriptorHeap *shaderVisibleHeap,
                                                uint32_t shaderVisibileHeapOffsetInDescriptors)
{
    uint32_t proceed = 1;
    uint32_t stageCount = ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount);
    uint64_t defaultHeapMemReq = 0;
    uint64_t uploadHeapMemReq = 0;
    uint64_t readbackHeapMemReq = 0;
    uint32_t shaderVisibleHeapDscCount = 0;
    proceed = ZSTDGPU_ENUM_CONST(StatusSuccess) == zstdgpu_GetGpuMemoryRequirement(&defaultHeapMemReq, &uploadHeapMemReq, &readbackHeapMemReq, &shaderVisibleHeapDscCount, req, stageIndex);
    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    proceed = proceed && (req->zstdFrameCount > 0);
    proceed = proceed && (req->zstdCompressedFramesByteCount > 0);
    proceed = proceed && (req->zstdUncompressedFrameCount == req->zstdFrameCount);
    proceed = proceed && (req->zstdUncompressedFramesByteCount > 0);
    proceed = proceed && (stageIndex < stageCount);
    proceed = proceed && (NULL != cmdList);
    proceed = proceed && (NULL != defaultHeap || 0 == defaultHeapMemReq);
    proceed = proceed && (NULL != uploadHeap || 0 == uploadHeapMemReq);
    proceed = proceed && (NULL != readbackHeap || 0 == readbackHeapMemReq);
    proceed = proceed && (NULL != shaderVisibleHeap || 0 == shaderVisibleHeapDscCount);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        if (stageIndex == 2)
        {
            ZSTDGPU_ASSERT(req->zstdUncompressedLitByteCountMax == 0 || req->zstdCmpBlockCountMax > 0);
        }

        zstdgpu_ResourceDataGpu_Term(&req->resData, stageIndex);

        if (NULL != defaultHeap)
            defaultHeap->AddRef();

        if (NULL != uploadHeap)
            uploadHeap->AddRef();

        if (NULL != readbackHeap)
            readbackHeap->AddRef();

        req->resData.gpuOnly_Heap[stageIndex] = defaultHeap;
        req->resData.gpuOnly_HeapOffset[stageIndex] = defaultHeapOffsetInBytes;

        req->resData.cpu2Gpu_Heap[stageIndex] = uploadHeap;
        req->resData.cpu2Gpu_HeapOffset[stageIndex] = uploadHeapOffsetInBytes;

        req->resData.gpu2Cpu_Heap[stageIndex] = readbackHeap;
        req->resData.gpu2Cpu_HeapOffset[stageIndex] = readbackHeap_OffsetInBytes;

        zstdgpu_ResourceDataGpu_Init(&req->resData, &req->resInfo, req->device, stageIndex);
        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory) && stageIndex == 0u)
        {
            zstdgpu_ResourceDataGpu_ReInitInputExternal(&req->resData, req->compressedFramesData, req->compressedFramesRefs);
        }

        if (stageIndex == 2u)
        {
            zstdgpu_ResourceDataGpu_ReInitOutputsExternal(&req->resData, req->uncompressedFramesData, req->uncompressedFramesRefs);
        }

        // NOTE(pamartis): we need to do call upload callback right after initialising resources of stage == 0
        if (stageIndex == 0)
        {
            if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsCpuMemory))
            {
                req->uploadCallback(req->resData.cpu2Gpu.CompressedDataCpu, req->zstdCompressedFramesByteCount, req->resData.cpu2Gpu.FramesRefsCpu, req->zstdFrameCount, req->uploadUserdata);
            }
            memcpy(req->resData.cpu2Gpu.FseProbsDefaultCpu, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));
        }

        D3D12AID_SAFE_RELEASE(req->srts.heap);
        req->srts.heap = shaderVisibleHeap;
        req->srts.heap->AddRef();
        req->srts.heapOffset = shaderVisibileHeapOffsetInDescriptors;
        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, stageIndex);

        if (stageIndex == 0)
        {
            zstdgpu_SubmitStage0(req, cmdList);
        }
        else if (stageIndex == 1)
        {
            zstdgpu_SubmitStage1(req, cmdList);
        }
        else if (stageIndex == 2)
        {
            zstdgpu_SubmitStage2(req, cmdList);
        }

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SubmitAllStagesWithExternalMemory(zstdgpu_PerRequestContext req,
                                                               struct ID3D12GraphicsCommandList *cmdList,
                                                               struct ID3D12Heap *defaultHeap,
                                                               uint64_t defaultHeap_OffsetInBytes,
                                                               struct ID3D12Heap *uploadHeap,
                                                               uint64_t uploadHeap_OffsetInBytes,
                                                               struct ID3D12Heap *readbackHeap,
                                                               uint64_t readbackHeap_OffsetInBytes,
                                                               struct ID3D12DescriptorHeap *shaderVisibleHeap,
                                                               uint32_t shaderVisibileHeap_OffsetInDescriptors)
{
    uint32_t proceed = 1;
    uint64_t defaultHeapMemReq = 0;
    uint64_t uploadHeapMemReq = 0;
    uint64_t readbackHeapMemReq = 0;
    uint32_t shaderVisibleHeapDscCount = 0;
    proceed = proceed && (0 == zstdgpu_IsAnyStageReadbackRequired(req));
    proceed = proceed && (ZSTDGPU_ENUM_CONST(StatusSuccess) == zstdgpu_GetAllStageGpuMemoryRequirement(&defaultHeapMemReq, &uploadHeapMemReq, &readbackHeapMemReq, &shaderVisibleHeapDscCount, req));
    proceed = proceed && (NULL != cmdList);
    proceed = proceed && (NULL != defaultHeap || 0 == defaultHeapMemReq);
    proceed = proceed && (NULL != uploadHeap || 0 == uploadHeapMemReq);
    proceed = proceed && (NULL != readbackHeap || 0 == readbackHeapMemReq);
    proceed = proceed && (NULL != shaderVisibleHeap || 0 == shaderVisibleHeapDscCount);
    proceed = proceed && (req->zstdUncompressedLitByteCountMax == 0 || req->zstdCmpBlockCountMax > 0);
    proceed = proceed && ZSTDGPU_IS_DEFAULT_ALIGNED(defaultHeap_OffsetInBytes);
    proceed = proceed && ZSTDGPU_IS_DEFAULT_ALIGNED(uploadHeap_OffsetInBytes);
    proceed = proceed && ZSTDGPU_IS_DEFAULT_ALIGNED(readbackHeap_OffsetInBytes);
    ZSTDGPU_ASSERT(proceed > 0);
    if (proceed)
    {
        uint64_t defaultOffs = defaultHeap_OffsetInBytes;
        uint64_t uploadOffs = uploadHeap_OffsetInBytes;
        uint64_t readbackOffs = readbackHeap_OffsetInBytes;
        zstdgpu_ResourceDataGpu_Term(&req->resData, 0);
        zstdgpu_ResourceDataGpu_Term(&req->resData, 1);
        zstdgpu_ResourceDataGpu_Term(&req->resData, 2);

        for (uint32_t stageIndex = 0; stageIndex < ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount); ++stageIndex)
        {
            if (NULL != defaultHeap)
                defaultHeap->AddRef();

            if (NULL != uploadHeap)
                uploadHeap->AddRef();

            if (NULL != readbackHeap)
                readbackHeap->AddRef();

            req->resData.gpuOnly_Heap[stageIndex] = defaultHeap;
            req->resData.gpuOnly_HeapOffset[stageIndex] = defaultOffs;

            req->resData.cpu2Gpu_Heap[stageIndex] = uploadHeap;
            req->resData.cpu2Gpu_HeapOffset[stageIndex] = uploadOffs;

            req->resData.gpu2Cpu_Heap[stageIndex] = readbackHeap;
            req->resData.gpu2Cpu_HeapOffset[stageIndex] = readbackOffs;

            defaultOffs   += req->resInfo.gpuOnly_ByteCount[stageIndex];
            uploadOffs    += req->resInfo.cpu2Gpu_ByteCount[stageIndex];
            readbackOffs  += req->resInfo.gpu2Cpu_ByteCount[stageIndex];

            ZSTDGPU_ASSERT(ZSTDGPU_IS_DEFAULT_ALIGNED(defaultOffs));
            ZSTDGPU_ASSERT(ZSTDGPU_IS_DEFAULT_ALIGNED(uploadOffs));
            ZSTDGPU_ASSERT(ZSTDGPU_IS_DEFAULT_ALIGNED(readbackOffs));
        }
        ZSTDGPU_ASSERT(defaultHeapMemReq == defaultOffs - defaultHeap_OffsetInBytes);
        ZSTDGPU_ASSERT(uploadHeapMemReq == uploadOffs - uploadHeap_OffsetInBytes);
        ZSTDGPU_ASSERT(readbackHeapMemReq == readbackOffs - readbackHeap_OffsetInBytes);

        zstdgpu_ResourceDataGpu_Init(&req->resData, &req->resInfo, req->device, 0);
        zstdgpu_ResourceDataGpu_Init(&req->resData, &req->resInfo, req->device, 1);
        zstdgpu_ResourceDataGpu_Init(&req->resData, &req->resInfo, req->device, 2);

        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory))
        {
            zstdgpu_ResourceDataGpu_ReInitInputExternal(&req->resData, req->compressedFramesData, req->compressedFramesRefs);
        }
        zstdgpu_ResourceDataGpu_ReInitOutputsExternal(&req->resData, req->uncompressedFramesData, req->uncompressedFramesRefs);

        // NOTE(pamartis): we need to do call upload callback right after initialising resources of stage == 0
        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsCpuMemory))
        {
            req->uploadCallback(req->resData.cpu2Gpu.CompressedDataCpu, req->zstdCompressedFramesByteCount, req->resData.cpu2Gpu.FramesRefsCpu, req->zstdFrameCount, req->uploadUserdata);
        }
        memcpy(req->resData.cpu2Gpu.FseProbsDefaultCpu, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));

        D3D12AID_SAFE_RELEASE(req->srts.heap);
        req->srts.heap = shaderVisibleHeap;
        req->srts.heap->AddRef();
        req->srts.heapOffset = shaderVisibileHeap_OffsetInDescriptors;
        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, 0);
        req->srts.heapOffset += zstdgpu_Count_SRTs_Stage(0);

        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, 1);
        req->srts.heapOffset += zstdgpu_Count_SRTs_Stage(1);

        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, 2);
        req->srts.heapOffset += zstdgpu_Count_SRTs_Stage(2);

        ZSTDGPU_ASSERT(shaderVisibleHeapDscCount == req->srts.heapOffset - shaderVisibileHeap_OffsetInDescriptors);
        zstdgpu_SubmitStage0(req, cmdList);
        zstdgpu_SubmitStage1(req, cmdList);
        zstdgpu_SubmitStage2(req, cmdList);

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SubmitWithInteralMemory(zstdgpu_PerRequestContext req, uint32_t stageIndex, struct ID3D12GraphicsCommandList *cmdList)
{
    uint32_t proceed = 1;
    uint32_t stageCount = ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount);

    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    proceed = proceed && (req->zstdFrameCount > 0);
    proceed = proceed && (req->zstdCompressedFramesByteCount > 0);
    proceed = proceed && (req->zstdUncompressedFrameCount == req->zstdFrameCount);
    proceed = proceed && (req->zstdUncompressedFramesByteCount > 0);
    proceed = proceed && (stageIndex < stageCount) && (stageIndex < ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount));
    proceed = proceed && (NULL != cmdList);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        #define CNTRS(name) req->resData.gpu2Cpu.CountersCpu->name

        // NOTE(pamartis): Recompute memory information for a given stage
        if (stageIndex == 0)
        {
            zstdgpu_ResourceInfo_Stage_0_Init(&req->resInfo, req->zstdFrameCount, req->zstdCompressedFramesByteCount, zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory) ? 1u : 0u);
        }
        else if (stageIndex == 1)
        {
            uint32_t cntRaw, cntRle, cntCmp;
            zstdgpu_RecomputeAndRetrieveFrameInfoConstants(&cntRaw, &cntRle, &cntCmp, req);
            zstdgpu_ResourceInfo_Stage_1_Init(&req->resInfo, cntRaw, cntRle, cntCmp);
        }
        else if (stageIndex == 2)
        {
            uint32_t cntLit, cntSeq;
            zstdgpu_RecomputeAndRetrieveBlockInfoConstants(&cntLit, &cntSeq, req);
            zstdgpu_ResourceInfo_Stage_2_Init(&req->resInfo, cntLit, cntSeq, req->zstdUncompressedFramesByteCount, req->zstdUncompressedFrameCount);
        }

        // NOTE(pamartis): if at least one heap from a given stage is too small, release all heaps and recreate
        // TODO(pamartis): consider releasing only single heap
        if (req->resData.gpuOnly_ByteCount[stageIndex] < req->resInfo.gpuOnly_ByteCount[stageIndex] ||
            req->resData.cpu2Gpu_ByteCount[stageIndex] < req->resInfo.cpu2Gpu_ByteCount[stageIndex] ||
            req->resData.gpu2Cpu_ByteCount[stageIndex] < req->resInfo.gpu2Cpu_ByteCount[stageIndex])
        {
            zstdgpu_ResourceDataGpu_Term(&req->resData, stageIndex);
        }

        // NOTE(pamartis): try re-creating heap and then resources (will trigger only if they were released or never created)
        zstdgpu_ResourceDataGpu_InitHeap(&req->resData, &req->resInfo, req->device, stageIndex);
        zstdgpu_ResourceDataGpu_Init(&req->resData, &req->resInfo, req->device, stageIndex);
        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory) && stageIndex == 0u)
        {
            zstdgpu_ResourceDataGpu_ReInitInputExternal(&req->resData, req->compressedFramesData, req->compressedFramesRefs);
        }

        if (stageIndex == 2u)
        {
            zstdgpu_ResourceDataGpu_ReInitOutputsExternal(&req->resData, req->uncompressedFramesData, req->uncompressedFramesRefs);
        }

        // NOTE(pamartis): we need to do call upload callback right after initialising resources of stage == 0
        if (stageIndex == 0)
        {
            if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsCpuMemory))
            {
                req->uploadCallback(req->resData.cpu2Gpu.CompressedDataCpu, req->zstdCompressedFramesByteCount, req->resData.cpu2Gpu.FramesRefsCpu, req->zstdFrameCount, req->uploadUserdata);
            }
            memcpy(req->resData.cpu2Gpu.FseProbsDefaultCpu, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));
        }

        if (NULL == req->srts.heap)
        {
            const uint32_t srtCount = zstdgpu_Count_SRTs_Stage(0) + zstdgpu_Count_SRTs_Stage(1) + zstdgpu_Count_SRTs_Stage(2);
            req->srts.heap = d3d12aid_DescriptorHeap_Create(req->device, srtCount, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
            req->srts.heapOffset = 0;
        }
        {
            uint32_t offset = 0;
            for (uint32_t s = 0; s < stageIndex; ++s)
            {
                offset += zstdgpu_Count_SRTs_Stage(s);
            }
            req->srts.heapOffset = offset;
        }
        #undef CNTRS
        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, stageIndex);

        if (stageIndex == 0)
        {
            zstdgpu_SubmitStage0(req, cmdList);
        }
        else if (stageIndex == 1)
        {
            zstdgpu_SubmitStage1(req, cmdList);
        }
        else if (stageIndex == 2)
        {
            zstdgpu_SubmitStage2(req, cmdList);
        }

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SubmitAllStagesWithInteralMemory(zstdgpu_PerRequestContext req,
                                                              ID3D12GraphicsCommandList *cmdList)
{
    uint32_t proceed = 1;
    proceed = proceed && (0 == zstdgpu_IsAnyStageReadbackRequired(req));
    proceed = proceed && (NULL != req);
    proceed = proceed && (NULL != cmdList);
    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    proceed = proceed && (req->zstdFrameCount > 0);
    proceed = proceed && (req->zstdCompressedFramesByteCount > 0);
    proceed = proceed && (req->zstdUncompressedFrameCount == req->zstdFrameCount);
    proceed = proceed && (req->zstdUncompressedFramesByteCount > 0);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        ID3D12Device *device = req->device;
        uint64_t dflt, upld, rdbk;
        uint64_t dfltP, upldP, rdbkP;
        zstdgpu_GetAllStageGpuMemoryRequirementInternal(&dflt, &upld, &rdbk, req);

        dfltP = req->resData.gpuOnly_ByteCount[0]
              + req->resData.gpuOnly_ByteCount[1]
              + req->resData.gpuOnly_ByteCount[2];

        upldP = req->resData.cpu2Gpu_ByteCount[0]
              + req->resData.cpu2Gpu_ByteCount[1]
              + req->resData.cpu2Gpu_ByteCount[2];

        rdbkP = req->resData.gpu2Cpu_ByteCount[0]
              + req->resData.gpu2Cpu_ByteCount[1]
              + req->resData.gpu2Cpu_ByteCount[2];

        // TODO(pamartis): currently we always re-create buffers because we don't track whether their position changed,
        //                 but this must be done at some point
        #define ZSTDGPU_BUFFER(type, name) D3D12AID_SAFE_RELEASE(req->resData.gpuOnly.name);
            ZSTDGPU_ALL_BUFFERS_LIST_STAGE_0()
            ZSTDGPU_ALL_BUFFERS_LIST_STAGE_1()
            ZSTDGPU_ALL_BUFFERS_LIST_STAGE_2()
        #undef ZSTDGPU_BUFFER

        #define ZSTDGPU_BUFFER(type, name) D3D12AID_SAFE_RELEASE(req->resData.cpu2Gpu.name);
            ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_0()
            ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_1()
            ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_2()
        #undef ZSTDGPU_BUFFER

        #define ZSTDGPU_BUFFER(type, name) D3D12AID_SAFE_RELEASE(req->resData.gpu2Cpu.name);
            ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()
            ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()
            ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()
        #undef ZSTDGPU_BUFFER

        if (dfltP < dflt)
        {
            D3D12AID_SAFE_RELEASE(req->resData.gpuOnly_Heap[0]);
            D3D12AID_SAFE_RELEASE(req->resData.gpuOnly_Heap[1]);
            D3D12AID_SAFE_RELEASE(req->resData.gpuOnly_Heap[2]);
        }

        if (upldP < upld)
        {
            D3D12AID_SAFE_RELEASE(req->resData.cpu2Gpu_Heap[0]);
            D3D12AID_SAFE_RELEASE(req->resData.cpu2Gpu_Heap[1]);
            D3D12AID_SAFE_RELEASE(req->resData.cpu2Gpu_Heap[2]);
        }

        if (rdbkP < rdbk)
        {
            D3D12AID_SAFE_RELEASE(req->resData.gpu2Cpu_Heap[0]);
            D3D12AID_SAFE_RELEASE(req->resData.gpu2Cpu_Heap[1]);
            D3D12AID_SAFE_RELEASE(req->resData.gpu2Cpu_Heap[2]);
        }

        // NOTE(pamartis): when initialising a heap in single submission mode, we set exact memory requirement
        // use shared pointer
        #define INIT_HEAP(name, stage, heap)                                            \
            req->resData.name##_Heap[stage] = heap;                                     \
            req->resData.name##_HeapOffset[stage] = offs;                               \
            req->resData.name##_ByteCount[stage] = req->resInfo.name##_ByteCount[stage];\
            offs += req->resInfo.name##_ByteCount[stage];                               \
            ZSTDGPU_ASSERT(ZSTDGPU_IS_DEFAULT_ALIGNED(offs))

        if (NULL == req->resData.gpuOnly_Heap[0] && 0 != dflt)
        {
            ID3D12Heap *heap = d3d12aid_Heap_Create_WithHeapTypeAndFlags(device, dflt, 0, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
            uint64_t offs = 0;
            INIT_HEAP(gpuOnly, 0, heap);
            INIT_HEAP(gpuOnly, 1, heap);
            INIT_HEAP(gpuOnly, 2, heap);
            ZSTDGPU_ASSERT(offs == dflt);

            heap->AddRef();
            heap->AddRef();
        }
        // TODO(pamartis): currently we always re-create buffers because we don't track whether their position changed,
        //                 but this must be done at some point
        zstdgpu_ResourceDataGpu_Init_GpuOnly(&req->resData, &req->resInfo, device, 0);
        zstdgpu_ResourceDataGpu_Init_GpuOnly(&req->resData, &req->resInfo, device, 1);
        zstdgpu_ResourceDataGpu_Init_GpuOnly(&req->resData, &req->resInfo, device, 2);

        if (NULL == req->resData.cpu2Gpu_Heap[0] && 0 != upld)
        {
            ID3D12Heap *heap = d3d12aid_Heap_Create_WithHeapTypeAndFlags(device, upld, 0, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
            uint64_t offs = 0;
            INIT_HEAP(cpu2Gpu, 0, heap);
            INIT_HEAP(cpu2Gpu, 1, heap);
            INIT_HEAP(cpu2Gpu, 2, heap);
            ZSTDGPU_ASSERT(offs == upld);
            heap->AddRef();
            heap->AddRef();
        }
        // TODO(pamartis): currently we always re-create buffers because we don't track whether their position changed,
        //                 but this must be done at some point
        zstdgpu_ResourceDataGpu_Init_CpuToGpu(&req->resData, &req->resInfo, device, 0);
        zstdgpu_ResourceDataGpu_Init_CpuToGpu(&req->resData, &req->resInfo, device, 1);
        zstdgpu_ResourceDataGpu_Init_CpuToGpu(&req->resData, &req->resInfo, device, 2);

        if (NULL == req->resData.gpu2Cpu_Heap[0] && 0 != rdbk)
        {
            ID3D12Heap *heap = d3d12aid_Heap_Create_WithHeapTypeAndFlags(device, rdbk, 0, D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
            uint64_t offs = 0;
            INIT_HEAP(gpu2Cpu, 0, heap);
            INIT_HEAP(gpu2Cpu, 1, heap);
            INIT_HEAP(gpu2Cpu, 2, heap);
            ZSTDGPU_ASSERT(offs == rdbk);
            heap->AddRef();
            heap->AddRef();
        }
        // TODO(pamartis): currently we always re-create buffers because we don't track whether their position changed,
        //                 but this must be done at some point
        zstdgpu_ResourceDataGpu_Init_GpuToCpu(&req->resData, &req->resInfo, device, 0);
        zstdgpu_ResourceDataGpu_Init_GpuToCpu(&req->resData, &req->resInfo, device, 1);
        zstdgpu_ResourceDataGpu_Init_GpuToCpu(&req->resData, &req->resInfo, device, 2);
        #undef INIT_HEAP

        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsGpuMemory))
        {
            zstdgpu_ResourceDataGpu_ReInitInputExternal(&req->resData, req->compressedFramesData, req->compressedFramesRefs);
        }

        zstdgpu_ResourceDataGpu_ReInitOutputsExternal(&req->resData, req->uncompressedFramesData, req->uncompressedFramesRefs);

        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsCpuMemory))
        {
            req->uploadCallback(req->resData.cpu2Gpu.CompressedDataCpu, req->zstdCompressedFramesByteCount, req->resData.cpu2Gpu.FramesRefsCpu, req->zstdFrameCount, req->uploadUserdata);
        }
        memcpy(req->resData.cpu2Gpu.FseProbsDefaultCpu, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));

        if (NULL == req->srts.heap)
        {
            const uint32_t srtCount = zstdgpu_Count_SRTs_Stage(0)
                                    + zstdgpu_Count_SRTs_Stage(1)
                                    + zstdgpu_Count_SRTs_Stage(2);

            req->srts.heap = d3d12aid_DescriptorHeap_Create(req->device, srtCount, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        }
        req->srts.heapOffset = 0;

        // TODO(pamartis): descriptors should be re-created only when buffers were re-created
        //                 but currently we always re-create buffers
        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, 0);
        req->srts.heapOffset += zstdgpu_Count_SRTs_Stage(0);

        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, 1);
        req->srts.heapOffset += zstdgpu_Count_SRTs_Stage(1);

        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData, 2);
        req->srts.heapOffset += zstdgpu_Count_SRTs_Stage(2);

        zstdgpu_SubmitStage0(req, cmdList);
        zstdgpu_SubmitStage1(req, cmdList);
        zstdgpu_SubmitStage2(req, cmdList);

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

#define setResourceState(barriers, index, resource, stateNameBefore, stateNameAfter)    \
    do                                                                                  \
    {                                                                                   \
        const uint32_t slot = (index);                                                  \
        barriers[slot].Type                    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;\
        barriers[slot].Flags                   = D3D12_RESOURCE_BARRIER_FLAG_NONE;      \
        barriers[slot].Transition.pResource    = resource;                              \
        barriers[slot].Transition.Subresource  = 0;                                     \
        barriers[slot].Transition.StateBefore  = D3D12_RESOURCE_STATE_##stateNameBefore;\
        barriers[slot].Transition.StateAfter   = D3D12_RESOURCE_STATE_##stateNameAfter; \
    }                                                                                   \
    while(0)

#define setResourceUavToSrvSync(barriers, index, resource)                              \
    do                                                                                  \
    {                                                                                   \
        const uint32_t slot = (index);                                                  \
        barriers[slot].Type                    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;\
        barriers[slot].Flags                   = D3D12_RESOURCE_BARRIER_FLAG_NONE;      \
        barriers[slot].Transition.pResource    = resource;                              \
        barriers[slot].Transition.Subresource  = 0;                                     \
        barriers[slot].Transition.StateBefore  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; \
        barriers[slot].Transition.StateAfter   = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE\
                                               | D3D12_RESOURCE_STATE_COPY_SOURCE;      \
    }                                                                                   \
    while(0)

#define setResourceSrvCopyIndirectToUavSync(barriers, index, resource)                  \
    do                                                                                  \
    {                                                                                   \
        const uint32_t slot = (index);                                                  \
        barriers[slot].Type                    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;\
        barriers[slot].Flags                   = D3D12_RESOURCE_BARRIER_FLAG_NONE;      \
        barriers[slot].Transition.pResource    = resource;                              \
        barriers[slot].Transition.Subresource  = 0;                                     \
        barriers[slot].Transition.StateBefore  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE\
                                               | D3D12_RESOURCE_STATE_COPY_SOURCE       \
                                               | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;\
        barriers[slot].Transition.StateAfter   = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; \
    }                                                                                   \
    while(0)

#define setResourceUavToSrvCopyIndirectSync(barriers, index, resource)                  \
    do                                                                                  \
    {                                                                                   \
        const uint32_t slot = (index);                                                  \
        barriers[slot].Type                    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;\
        barriers[slot].Flags                   = D3D12_RESOURCE_BARRIER_FLAG_NONE;      \
        barriers[slot].Transition.pResource    = resource;                              \
        barriers[slot].Transition.Subresource  = 0;                                     \
        barriers[slot].Transition.StateBefore  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; \
        barriers[slot].Transition.StateAfter   = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE\
                                               | D3D12_RESOURCE_STATE_COPY_SOURCE       \
                                               | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;\
    }                                                                                   \
    while(0)

#define setResourceUavSync(barriers, index, resource)                               \
    do                                                                              \
    {                                                                               \
        const uint32_t slot = (index);                                              \
        barriers[slot].Type                    = D3D12_RESOURCE_BARRIER_TYPE_UAV;   \
        barriers[slot].Flags                   = D3D12_RESOURCE_BARRIER_FLAG_NONE;  \
        barriers[slot].UAV.pResource           = resource;                          \
    }                                                                               \
    while(0)

#ifndef zstdgpu_PushReadback
#define zstdgpu_PushReadback(name) if (0 != req->resInfo.name##_ByteSizeInternal) cmdList->CopyResource(req->resData.gpu2Cpu.name, req->resData.gpuOnly.name)
#endif

#if ZSTDGPU_ENABLE_TIMESTAMPS == 0

#define ZSTDGPU_KERNEL_SCOPE(name, cmdList, statement)  \
    do                                                  \
    {                                                   \
        statement                                       \
    }                                                   \
    while (0)

#else

#define ZSTDGPU_KERNEL_SCOPE(name, cmdList, statement)                                  \
    do                                                                                  \
    {                                                                                   \
        req->timestampSlot[kzstdgpu_KernelScope_##name] = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);\
        statement                                                                       \
        d3d12aid_Timestamps_Push(&req->timestamps, cmdList);                            \
    }                                                                                   \
    while (0)

#endif

#define BIND_RS_PS_SRT(name)                                                    \
    do                                                                          \
    {                                                                           \
        d3d12aid_ComputeRsPs_Set(&req->name, cmdList);                          \
        cmdList->SetDescriptorHeaps(1, &req->srts.heap);                        \
        cmdList->SetComputeRootDescriptorTable(0, req->srts.name##GpuHandle);   \
    }                                                                           \
    while (0)

#define BIND_RS_PS_SRT_EX(psoName, srtName)                                     \
    do                                                                          \
    {                                                                           \
        d3d12aid_ComputeRsPs_Set(&req->psoName, cmdList);                       \
        cmdList->SetDescriptorHeaps(1, &req->srts.heap);                        \
        cmdList->SetComputeRootDescriptorTable(0, req->srts.srtName##GpuHandle);\
    }                                                                           \
    while (0)

static void zstdgpu_Dispatch32Bit(ID3D12GraphicsCommandList *cmdList, uint32_t tgCount, uint32_t rootParameterIndex, uint32_t rootParameterOffset)
{
#ifdef _GAMING_XBOX
    cmdList->SetComputeRoot32BitConstant(rootParameterIndex, /* tgOffset*/0, rootParameterOffset);
    cmdList->Dispatch(tgCount, 1, 1);
#else
    // NOTE(pamartis): on PC we should handle awkward D3D12 limitations
    // TODO(pamartis): on PC we should be doing this kind of workaround for all dispatches
    const uint32_t tgCountY = tgCount / D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    const uint32_t tgCountX = tgCount % D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    // Dispatch_0
    if (tgCountY > 0)
    {
        cmdList->SetComputeRoot32BitConstant(rootParameterIndex, /* tgOffset*/0, rootParameterOffset);
        cmdList->Dispatch(D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION, tgCountY, 1);
    }
    // Dispatch_1
    cmdList->SetComputeRoot32BitConstant(rootParameterIndex, /* tgOffset*/D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION * tgCountY, rootParameterOffset);
    cmdList->Dispatch(tgCountX, 1, 1);
#endif
}

#define zstdgpu_DispatchIndirect(cmdList, kernelName, counterName) \
    cmdList->ExecuteIndirect(req->kernelName##_CmdSig, kzstdgpu_DispatchSlot_CmdsPerSlot, req->resData.gpuOnly.DispatchArgs, kzstdgpu_DispatchSlot_##counterName * kzstdgpu_DispatchSlot_StrideInUInt32 * sizeof(uint32_t), req->resData.gpuOnly.DispatchCnts, kzstdgpu_DispatchSlot_##counterName * sizeof(uint32_t));

void zstdgpu_SubmitStage0(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
#if ZSTDGPU_ENABLE_TIMESTAMPS
    // NOTE(pamartis): reset the per-scope timestamp slots to ~0u here.
    // Scopes that are pushed conditionally then remain ~0u and are ignored by zstdgpu_RetrieveTimestamps.
    for (uint32_t i = 0; i < kzstdgpu_KernelScope_Count; ++i)
        req->timestampSlot[i] = ~0u;
#endif
    {
        D3D12_RESOURCE_BARRIER barriers[3];
        uint32_t uploadBarrierCount = 0;

        #define zstdgpu_PushUpload(name) cmdList->CopyResource(req->resData.gpuOnly.name, req->resData.cpu2Gpu.name)

        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsCpuMemory))
        {
            zstdgpu_PushUpload(CompressedData);
            zstdgpu_PushUpload(FramesRefs);
        }
        zstdgpu_PushUpload(FseProbsDefault);
        #undef zstdgpu_PushUpload

        setResourceState(barriers, 0, req->resData.gpuOnly.FseProbsDefault, COPY_DEST, NON_PIXEL_SHADER_RESOURCE);
        uploadBarrierCount += 1;
        if (zstdgpu_HasFlag(req->setupFlags, kzstdgpu_SetupFlags_InputsCpuMemory))
        {
            setResourceState(barriers, 1, req->resData.gpuOnly.CompressedData, COPY_DEST, NON_PIXEL_SHADER_RESOURCE);
            setResourceState(barriers, 2, req->resData.gpuOnly.FramesRefs, COPY_DEST, NON_PIXEL_SHADER_RESOURCE);
            uploadBarrierCount += 2;
        }
        cmdList->ResourceBarrier(uploadBarrierCount, barriers);
    }
#if ZSTDGPU_ENABLE_TIMESTAMPS
    // NOTE(pamartis): So far we don't include upload into measurement intentionally
    req->timestampSlot[kzstdgpu_KernelScope_Stage0_Start] = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);
#endif
    {
        const uint32_t initResourcesStage = 0;

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Init Resources :: Stage 0]");
        BIND_RS_PS_SRT_EX(InitResources, InitResources_S0);
        cmdList->SetComputeRoot32BitConstant(1, initResourcesStage, 0);
        ZSTDGPU_KERNEL_SCOPE(InitResources_CountBlocks, cmdList,
            cmdList->Dispatch(zstdgpu_InitResources_GetDispatchSizeX(initResourcesStage), 1, 1);
        );

        PIXEndEvent(cmdList);
    }
    {
        const uint32_t lookbackCount = zstdgpu_GetLookbackBlockCount(req->zstdFrameCount);
        const uint32_t tgCount = ZSTDGPU_TG_COUNT(lookbackCount, kzstdgpu_TgSizeX_Memset);

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[InitResources :: Memset :: Stage 0]");
        d3d12aid_ComputeRsPs_Set(&req->Memset, cmdList);

        cmdList->SetComputeRoot32BitConstant(1, 0 /* tgOffset */, 0);

        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount /* workItemCount */, 1);
        cmdList->SetComputeRoot32BitConstant(1, ~0u /* value */, 2);

        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameSeqStreamMinIdx->GetGPUVirtualAddress());
        cmdList->Dispatch(ZSTDGPU_TG_COUNT(req->zstdFrameCount, kzstdgpu_TgSizeX_Memset), 1, 1);

        cmdList->SetComputeRoot32BitConstant(1, lookbackCount /* workItemCount */, 1);
        cmdList->SetComputeRoot32BitConstant(1, 0 /* value */, 2);

        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountRAW->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
        cmdList->Dispatch(tgCount, 1, 1);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountRLE->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
        cmdList->Dispatch(tgCount, 1, 1);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountCMP->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
        cmdList->Dispatch(tgCount, 1, 1);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountAll->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
        cmdList->Dispatch(tgCount, 1, 1);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockSizesRAW->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
        cmdList->Dispatch(tgCount, 1, 1);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockSizesRLE->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
        cmdList->Dispatch(tgCount, 1, 1);

        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Parse Frames :: Count Blocks]");

        D3D12_RESOURCE_BARRIER barriers[8];
        uint32_t bc = 0;

        // last written by [Init Resources :: Stage 0]
        // next written/atomically updated by [Parse Frames :: Count Blocks]
        // triggers the barrier by immediate dependency between passes
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.Counters);
        if (0 == zstdgpu_IsReadbackRequired(req, 0))
        {
            // last written by [InitResources :: Memset :: Stage 0]
            // next written/updated by [Parse Compressed Blocks]
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameSeqStreamMinIdx);
        }
        // last written by [InitResources :: Memset :: Stage 0]
        // next written by [Parse Frames :: Block Counts]
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountRAW);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountRLE);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountCMP);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountAll);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockSizesRAW);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockSizesRLE);

        ZSTDGPU_ASSERT(bc <= _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }
    {
        const uint32_t countBlocksOnly = 1;
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Parse Frames :: Count Blocks]");
        BIND_RS_PS_SRT_EX(ParseFrames, ParseFrames_S0);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 0);
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 1);
        cmdList->SetComputeRoot32BitConstant(1, countBlocksOnly, 2);
        ZSTDGPU_KERNEL_SCOPE(ParseFrames_CountBlocks, cmdList,
            cmdList->Dispatch(ZSTDGPU_TG_COUNT(req->zstdFrameCount, kzstdgpu_TgSizeX_ParseCompressedBlocks), 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [PrefixSum :: Block Counts]");

        D3D12_RESOURCE_BARRIER barriers[7];
        // next written/atomically updated by [Parse Frames :: Count Blocks]
        // next written by [PrefixSum :: Block Counts] to store prefix sum instead of counts
        setResourceUavSync(barriers, 0, req->resData.gpuOnly.PerFrameBlockCountRAW);
        setResourceUavSync(barriers, 1, req->resData.gpuOnly.PerFrameBlockCountRLE);
        setResourceUavSync(barriers, 2, req->resData.gpuOnly.PerFrameBlockCountCMP);
        setResourceUavSync(barriers, 3, req->resData.gpuOnly.PerFrameBlockCountAll);
        setResourceUavSync(barriers, 4, req->resData.gpuOnly.PerFrameBlockSizesRAW);
        setResourceUavSync(barriers, 5, req->resData.gpuOnly.PerFrameBlockSizesRLE);
        // last written by [Parse Frames :: Count Blocks]
        // next read by [Update Dispatch Args :: Stage 0] as RWStructuredBuffer (read-only)
        // NOTE: stays in UNORDERED_ACCESS because UpdateDispatchArgs binds Counters as UAV
        setResourceUavSync(barriers, 6, req->resData.gpuOnly.Counters);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    {
        const uint32_t tgCountX = ZSTDGPU_TG_COUNT(req->zstdFrameCount, kzstdgpu_TgSizeX_PrefixSum);

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[PrefixSum :: Block Counts]");
        d3d12aid_ComputeRsPs_Set(&req->PrefixSum, cmdList);
        /**
         *  TODO(pamartis): This is `tgOffset` that should be set up by EmitDispatch on GPU
         *  but because we haven't converted these dispatches to 32-bit indirect dispatches yet,
         *  we setup the constant that shader expects
         */
        cmdList->SetComputeRoot32BitConstant(2, 0 /** tgOffset */, 0);
        cmdList->SetComputeRoot32BitConstant(2, req->zstdFrameCount /** workItemCount */, 1);
        cmdList->SetComputeRoot32BitConstant(2, 0 /** outputInclusive */, 2);

        ZSTDGPU_KERNEL_SCOPE(PrefixSum, cmdList,
        {
            cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountRAW->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerFrameBlockCountRAW->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
            cmdList->Dispatch(tgCountX, 1, 1);

            cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountRLE->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerFrameBlockCountRLE->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
            cmdList->Dispatch(tgCountX, 1, 1);

            cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountCMP->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerFrameBlockCountCMP->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
            cmdList->Dispatch(tgCountX, 1, 1);

            cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockCountAll->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerFrameBlockCountAll->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
            cmdList->Dispatch(tgCountX, 1, 1);

            cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockSizesRAW->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerFrameBlockSizesRAW->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
            cmdList->Dispatch(tgCountX, 1, 1);

            cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerFrameBlockSizesRLE->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerFrameBlockSizesRLE->GetGPUVirtualAddress() + req->zstdFrameCount * sizeof(uint32_t));
            cmdList->Dispatch(tgCountX, 1, 1);
        });

        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Update Dispatch Args :: Stage 0]");
        d3d12aid_ComputeRsPs_Set(&req->UpdateDispatchArgs, cmdList);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.DispatchArgs->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.DispatchCnts->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.Predicate->GetGPUVirtualAddress());
        cmdList->SetComputeRoot32BitConstant(4, req->DecompressSequences_StreamsPerGroup, 0);
        cmdList->SetComputeRoot32BitConstant(4, 0 /* stage */, 1);

        cmdList->SetComputeRoot32BitConstant(4, req->zstdCmpBlockCountMax, 2);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdRawBlockCountMax, 3);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdRleBlockCountMax, 4);
        cmdList->SetComputeRoot32BitConstant(4, 0 /* unused for stage == 0*/, 5);
        cmdList->SetComputeRoot32BitConstant(4, 0 /* unused for stage == 0*/, 6);
        ZSTDGPU_KERNEL_SCOPE(UpdateDispatchArgs_Stage0, cmdList,
            cmdList->Dispatch(1, 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Readback Counters :: After Block Count] and [Parse Compressed Blocks]");
        D3D12_RESOURCE_BARRIER barriers[6];
        uint32_t bc = 0;
        // last read by [Update Dispatch Args :: Stage 0]
        // next read by [Readback Counters :: After Block Count] or [Init Resources :: Stage 1]
        if (zstdgpu_IsReadbackRequired(req, 0))
        {
            setResourceState(barriers, bc ++, req->resData.gpuOnly.Counters, UNORDERED_ACCESS, COPY_SOURCE);
        }
        else
        {
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.Counters);
        }
        // last written by [PrefixSum :: Block Counts]
        // next bound to [Parse Compressed Blocks] as UAV
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountCMP);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountAll);
        // last written by [Update Dispatch Args :: Stage 0]
        // next read by ExecuteIndirect calls as argument/count buffers
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchArgs, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchCnts, UNORDERED_ACCESS, INDIRECT_ARGUMENT);

        if (0 == zstdgpu_IsReadbackRequired(req, 0))
        {
            // last written by [Update Dispatch Args :: Stage 0]
            // next read by Stage 1/2 predication
            setResourceState(barriers, bc ++, req->resData.gpuOnly.Predicate, UNORDERED_ACCESS, PREDICATION);
        }
        ZSTDGPU_ASSERT(bc <= _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }
    if (zstdgpu_IsReadbackRequired(req, 0))
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Readback Counters :: After Block Count]");
        zstdgpu_PushReadback(Counters);
        PIXEndEvent(cmdList);
    }
#if ZSTDGPU_ENABLE_TIMESTAMPS
    req->timestampSlot[kzstdgpu_KernelScope_Stage0_End] = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);
#endif
}
void zstdgpu_SubmitStage1(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
#if ZSTDGPU_ENABLE_TIMESTAMPS
    req->timestampSlot[kzstdgpu_KernelScope_Stage1_Start] = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);
#endif
    /**
     *  NOTE(pamartis): We enable predication only when either Frame Info constants are setup
     *  or single submission mode is requested without providing any up-front information about frame
     *  because in this case the submission happens into a single command list because
     *  `zstdgpu_IsReadbackRequired` returns `0`, so the calling code is free to submit every
     *  stage in a single command list, therefore there's a risk that not sufficiently large
     *  memory was allocated. We use predication on GPU to guard against it.
     *
     *  When Frame Info constants are not set and single submission mode is not requested,
     * `zstdgpu_IsReadbackRequired` returns `1`,
     *  so the calling code must wait for the fence after every stage, so readbacks
     *  get completed, and correct Frame Info constants are read back from GPU to CPU,
     *  so the memory allocation is always correct (unless calling forgets to call `zstdgpu_GetGpuMemoryRequirement`)
     */
    if (0 == zstdgpu_IsReadbackRequired(req, 0))
    {
        cmdList->SetPredication(req->resData.gpuOnly.Predicate, 0 /* Stage 1 predicate */, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    }
    {
        const uint32_t initResourcesStage = 1;

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Init Resources :: Stage 1]");
        BIND_RS_PS_SRT_EX(InitResources, InitResources_S1);
        cmdList->SetComputeRoot32BitConstant(1, initResourcesStage, 0);
        ZSTDGPU_KERNEL_SCOPE(InitResources, cmdList,
            cmdList->Dispatch(zstdgpu_InitResources_GetDispatchSizeX(initResourcesStage), 1, 1);
        );

        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[InitResources :: Memset :: Stage 1]");
        d3d12aid_ComputeRsPs_Set(&req->Memset, cmdList);

        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, 0 /* value */, 2);

        // Group 1: CmpBlockLookback-sized regions (6 UAV rebinds, same dispatch slot)
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.LitGroupEndPerHuffmanTable->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset1->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset2->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset3->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.SeqCountPrefixLookback->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.BlockSeqCountPrefixLookback->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.LitStreamCountPrefixLookback->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.HufLitCompactionLookback->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);

        // Group 2: FseIndexLookback{HufW,LLen,Offs,MLen} -- same shape as CmpBlockLookback (lookbackBlockCount uint32 each)
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.FseIndexLookbackLLen->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.FseIndexLookbackOffs->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.FseIndexLookbackMLen->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);

        // Group 3: HufWIdToHufLitId -- init to ~0 marking all potential Huffman table indices as unused. [Parse Compressed Blocks]
        // would fill in this table with corresponding literal blocks.
        cmdList->SetComputeRoot32BitConstant(1, 0xFFFFFFFFu /* value */, 2);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.HufWIdToHufLitId->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockCount);

        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[InitResources :: Memset :: Stage 1 :: BlockSize Lookback]");
        // Group 4: BlockSizePrefix lookback (allBlockCount-sized)
        const uint32_t allBlockCount = req->zstdRawBlockCountMax
                                     + req->zstdRleBlockCountMax
                                     + req->zstdCmpBlockCountMax;

        d3d12aid_ComputeRsPs_Set(&req->Memset, cmdList);

        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, 0 /* value */, 2);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.BlockSizePrefix->GetGPUVirtualAddress() + allBlockCount * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_AllBlockLookback);

        PIXEndEvent(cmdList);
    }
    {
        // Resources needed by for Parse Frames
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Parse Frames :: Collect Blocks]");
        D3D12_RESOURCE_BARRIER barriers[1];
        // last written by [Init Resources :: Stage 1]
        // next written/updated by [Parse Frames :: Collect Blocks]
        setResourceUavSync(barriers, 0, req->resData.gpuOnly.Counters);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    {
        const uint32_t countBlocksOnly = 0;
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Parse Frames :: Collect Blocks]");
        BIND_RS_PS_SRT_EX(ParseFrames, ParseFrames_S1);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 0);
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 1);
        cmdList->SetComputeRoot32BitConstant(1, countBlocksOnly, 2);
        ZSTDGPU_KERNEL_SCOPE(ParseFrames, cmdList,
            cmdList->Dispatch(ZSTDGPU_TG_COUNT(req->zstdFrameCount, kzstdgpu_TgSizeX_ParseCompressedBlocks), 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Parse Compressed Blocks] and [Memcpy RAW blocks, Memset RLE blocks]");
        D3D12_RESOURCE_BARRIER barriers[19];

        uint32_t bc = 0;
        {
            // last written by [Parse Frames :: Collect Blocks]
            // next written/updated by [Parse Compressed Blocks] with non-intersecting memory ranges with [Parse Frames :: Collect Blocks]
            // so TODO: check if can remove
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.Counters);
            // last written by [Init Resources :: Stage 1]
            // next written/updated by [Parse Compressed Blocks] with non-intersecting memory range with [Init Resources :: Stage 1]
            // so TODO: check if can remove
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.FseInfos);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.FseProbs);
            // last written by [InitResources :: Memset :: Stage 1]
            // next written/updated by [Propagate FSE Index]
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.FseIndexLookbackLLen);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.FseIndexLookbackOffs);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.FseIndexLookbackMLen);
            // last written by [InitResources :: Memset :: Stage 1] to initialise "lookback" region.
            // next written/updated by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.LitGroupEndPerHuffmanTable);
            // last written by [Parse Frames :: Collect Blocks]
            // next read by [Parse Compressed Blocks]
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.BlocksCMPRefs);
            // last bound as UAV to [Parse Frames :: Collect Blocks]
            // next read by [Parse Compressed Blocks]
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountCMP);
            // last bound as UAV to [Parse Frames :: Collect Blocks]
            // next read by [Prefix Sequence Offsets] and [Finalise Sequence Offsets]
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountAll);
            // last written by [InitResources :: Memset :: Stage 1]
            // next written/updated by [Parse Compressed Blocks]
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.SeqCountPrefixLookback);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.BlockSeqCountPrefixLookback);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.LitStreamCountPrefixLookback);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.HufLitCompactionLookback);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.HufWIdToHufLitId);
            // last written by [Parse Frames :: Collect Blocks]
            // next read by [Parse Compressed Blocks] -- to be able to get global index to index into BlockSizePrefix
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.GlobalBlockIndexPerCmpBlock);
            // last written by [Parse Frames :: Collect Blocks]
            // next written/updated by [Parse Compressed Blocks] - sets literal size to be uncompressed block size
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.BlockSizePrefix);
            // last written by [Parse Frames :: Collect Blocks]
            // next read by [Memcpy RAW blocks, Memset RLE blocks]
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.BlocksRAWRefs);
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.BlocksRLERefs);
        }

        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Parse Compressed Blocks]");
        BIND_RS_PS_SRT(ParseCompressedBlocks);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 2);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 3);

        ZSTDGPU_KERNEL_SCOPE(ParseCompressedBlocks, cmdList,
            zstdgpu_DispatchIndirect(cmdList, ParseCompressedBlocks, ParseCompressedBlocks);
        );
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Readback Counters :: After Block Parse] and [Update Dispatch Args] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]");
        D3D12_RESOURCE_BARRIER barriers[15];
        uint32_t bc = 0;
        {
            // last written by [Parse Compressed Blocks]
            // next read by [Readback Counters :: After Block Parse] or [Update Dispatch Args]
            if (zstdgpu_IsReadbackRequired(req, 1))
            {
                setResourceState(barriers, bc ++, req->resData.gpuOnly.Counters, UNORDERED_ACCESS, COPY_SOURCE);
            }
            else
            {
                setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.Counters);
            }
            // last written by [Parse Compressed Blocks]
            // next written/updated by [Decompress Sequences]
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.BlockSizePrefix);
            // last written by [Parse Compressed Blocks]
            // next read by [Prefix Sequence Offsets]
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.PerFrameSeqStreamMinIdx);

            // last written by [Init Resources :: Stage 1] with zero values to lookback data
            // next written by [Decompress Sequences] with encoded "final" offsets per block
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerSeqStreamFinalOffset1);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerSeqStreamFinalOffset2);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerSeqStreamFinalOffset3);
            // last written by [Parse Compressed Blocks]
            // next read by [Finalise Sequence Offsets]
            setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.PerSeqStreamSeqStart);

            // last written by [Parse Compressed Blocks] with un-propagated values
            // next read+written by [Propagate FSE Index] (in-place propagation, indexed by sequence stream)
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToLLenFseId);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToOffsFseId);
            setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToMLenFseId);
            // last written by [Parse Compressed Blocks] with un-propagated values
            // next read by [Propagate FSE Index] HufW dispatch (in-place propagation, indexed by cmp block)
            //setResourceUavToSrvCopyIndirectSync(barriers, bc ++, req->resData.gpuOnly.HufLitIdToHufWId_DBG);
        }

        // last read by ExecuteIndirect as INDIRECT_ARGUMENT
        // next written by [Update Dispatch Args :: Stage 1]
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchArgs, INDIRECT_ARGUMENT, UNORDERED_ACCESS);
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchCnts, INDIRECT_ARGUMENT, UNORDERED_ACCESS);

        if (0 == zstdgpu_IsReadbackRequired(req, 0))
        {
            // last read by SetPredication as PREDICATION
            // next written by [Update Dispatch Args :: Stage 1]
            setResourceState(barriers, bc ++, req->resData.gpuOnly.Predicate, PREDICATION, UNORDERED_ACCESS);
        }

        ZSTDGPU_ASSERT(bc <= _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Update Dispatch Args :: Stage 1]");
        d3d12aid_ComputeRsPs_Set(&req->UpdateDispatchArgs, cmdList);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.DispatchArgs->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.DispatchCnts->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.Predicate->GetGPUVirtualAddress());
        cmdList->SetComputeRoot32BitConstant(4, req->DecompressSequences_StreamsPerGroup, 0);
        cmdList->SetComputeRoot32BitConstant(4, 1 /* stage */, 1);

        cmdList->SetComputeRoot32BitConstant(4, req->zstdCmpBlockCountMax, 2);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdRawBlockCountMax, 3);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdRleBlockCountMax, 4);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdUncompressedLitByteCountMax, 5);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdUncompressedSeqElemCountMax, 6);
        ZSTDGPU_KERNEL_SCOPE(UpdateDispatchArgs_Stage1, cmdList,
            cmdList->Dispatch(1, 1, 1);
        );
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Propagate FSE Index] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]");
        D3D12_RESOURCE_BARRIER barriers[4];
        uint32_t bc = 0;
        // last written by [Update Dispatch Args]
        // next read by [Propagate FSE Index] / [Compute `Per-Huffman Table` Literal Stream Count Prefix] via ExecuteIndirect
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchArgs, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchCnts, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        // last written by [Update Dispatch Args]
        // next read/written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.Counters);

        if (0 == zstdgpu_IsReadbackRequired(req, 1))
        {
            // last written by [Update Dispatch Args :: Stage 1]
            // next read by SetPredication as PREDICATION
            setResourceState(barriers, bc ++, req->resData.gpuOnly.Predicate, UNORDERED_ACCESS, PREDICATION);
        }

        ZSTDGPU_ASSERT(bc <= _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Propagate FSE Index]");
        d3d12aid_ComputeRsPs_Set(&req->PropagateFseIndex, cmdList);

        ZSTDGPU_KERNEL_SCOPE(PropagateFseIndex, cmdList,
        {
            // NOTE: Slot 0 (tgOffset, workItemCount) is set by command signature via indirect dispatch

            // Propagate LLen FSE indices across sequence streams
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.SeqStreamToLLenFseId->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.FseIndexLookbackLLen->GetGPUVirtualAddress());
            zstdgpu_DispatchIndirect(cmdList, PropagateFseIndex, PropagateFseIndex);

            // Propagate Offs FSE indices across sequence streams
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.SeqStreamToOffsFseId->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.FseIndexLookbackOffs->GetGPUVirtualAddress());
            zstdgpu_DispatchIndirect(cmdList, PropagateFseIndex, PropagateFseIndex);

            // Propagate MLen FSE indices across sequence streams
            cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.SeqStreamToMLenFseId->GetGPUVirtualAddress());
            cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.FseIndexLookbackMLen->GetGPUVirtualAddress());
            zstdgpu_DispatchIndirect(cmdList, PropagateFseIndex, PropagateFseIndex);
        });
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Compute `Per-Huffman Table` Literal Stream Group Count Prefix]");
        D3D12_RESOURCE_BARRIER barriers[2];
        uint32_t bc = 0;
        // last written by [Parse Compressed Blocks]
        // next read by [Compute `Per-Huffman Table` Literal Stream Group Count Prefix]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.HufWIdToHufLitId);
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.HufLitIdToLitStreamId);
        ZSTDGPU_ASSERT(bc <= _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    // Unset predication
    if (0 == zstdgpu_IsReadbackRequired(req, 0))
    {
        cmdList->SetPredication(NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }

    if (zstdgpu_IsReadbackRequired(req, 1))
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Readback Counters :: After Block Parse]");
        zstdgpu_PushReadback(Counters);
        PIXEndEvent(cmdList);
    }
#if ZSTDGPU_ENABLE_TIMESTAMPS
    req->timestampSlot[kzstdgpu_KernelScope_Stage1_End] = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);
#endif
}

void zstdgpu_SubmitStage2(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
#if ZSTDGPU_ENABLE_TIMESTAMPS
    req->timestampSlot[kzstdgpu_KernelScope_Stage2_Start] = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);
#endif
    // NOTE(pamartis): enable predication for stage2 in case when either single submission mode is requested or
    // block information is specified
    if (0 == zstdgpu_IsReadbackRequired(req, 1))
    {
        cmdList->SetPredication(req->resData.gpuOnly.Predicate, sizeof(uint64_t) /* Stage 2 predicate */, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Compute `Per-Huffman Table` Literal Stream Count Prefix]");

        d3d12aid_ComputeRsPs_Set(&req->ComputePrefixSum, cmdList);

        cmdList->SetComputeRootShaderResourceView(0, req->resData.gpuOnly.HufWIdToHufLitId->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(1, req->resData.gpuOnly.HufLitIdToLitStreamId->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.LitGroupEndPerHuffmanTable->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.LitGroupEndPerHuffmanTable->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        cmdList->SetComputeRootUnorderedAccessView(4, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
#if 0
        // NOTE(pamartis): Use this pass to with DecompressLiterals kernel
        cmdList->SetComputeRoot32BitConstant(5, kzstdgpu_TgSizeX_DecompressLiterals, 2);
#else
        // NOTE(pamartis): Use this path to with DecompressLiterals_LdsStoreCache* kernels
        cmdList->SetComputeRoot32BitConstant(5, req->DecompressLiterals_LdsStoreCache_StreamsPerGroup, 2);
#endif
        ZSTDGPU_KERNEL_SCOPE(ComputePrefixSum, cmdList,
            zstdgpu_DispatchIndirect(cmdList, ComputePrefixSum, ComputePrefixSum);
        );
        PIXEndEvent(cmdList);
    }

    // NOTE(pamartis): `ComputePrefixSum` writes `DecompressLiteralsGroups` to `Counters`.
    // A separate `UpdateDispatchArgs` pass populates `DecompressLiterals` dispatch slot
    // because `ComputePrefixSum` can't write DispatchArgs (it's in INDIRECT_ARGUMENT state).
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Update Dispatch Args :: DecompressLiterals]");
        D3D12_RESOURCE_BARRIER barriers[3];
        // last written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Update Dispatch Args :: DecompressLiterals]
        setResourceUavSync(barriers, 0, req->resData.gpuOnly.Counters);
        // last read by [Compute `Per-Huffman Table` Literal Stream Count Prefix] as INDIRECT_ARGUMENT
        // next written by [Update Dispatch Args :: DecompressLiterals]
        setResourceState(barriers, 1, req->resData.gpuOnly.DispatchArgs, INDIRECT_ARGUMENT, UNORDERED_ACCESS);
        setResourceState(barriers, 2, req->resData.gpuOnly.DispatchCnts, INDIRECT_ARGUMENT, UNORDERED_ACCESS);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Update Dispatch Args :: DecompressLiterals]");
        d3d12aid_ComputeRsPs_Set(&req->UpdateDispatchArgs, cmdList);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.DispatchArgs->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.DispatchCnts->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.Predicate->GetGPUVirtualAddress() /* unused for stage == 2 */);
        cmdList->SetComputeRoot32BitConstant(4, req->DecompressSequences_StreamsPerGroup, 0);
        cmdList->SetComputeRoot32BitConstant(4, 2 /* stage */, 1);

        cmdList->SetComputeRoot32BitConstant(4, req->zstdCmpBlockCountMax, 2);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdRawBlockCountMax, 3);
        cmdList->SetComputeRoot32BitConstant(4, req->zstdRleBlockCountMax, 4);
        cmdList->SetComputeRoot32BitConstant(4, 0 /* unused for stage == 2 */, 5);
        cmdList->SetComputeRoot32BitConstant(4, 0 /* unused for stage == 2 */, 6);
        ZSTDGPU_KERNEL_SCOPE(UpdateDispatchArgs_DecompressLiterals, cmdList,
            cmdList->Dispatch(1, 1, 1);
        );
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Init FSE Table] and [Group Lilteral Streams]");
        D3D12_RESOURCE_BARRIER barriers[18];
        uint32_t bc = 0;
        // last written by [Update Dispatch Args] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Group Lilteral Streams] and [Init FSE Table] and [Decompress Literals]
        setResourceUavToSrvCopyIndirectSync(barriers, bc ++, req->resData.gpuOnly.Counters);
        // last written by [Update Dispatch Args] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by ExecuteIndirect calls as argument buffer
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchArgs, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        // last written by [Update Dispatch Args] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by ExecuteIndirect calls as count buffer
        setResourceState(barriers, bc ++, req->resData.gpuOnly.DispatchCnts, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        // last written by [Parse Compressed Blocks]
        // next read by [Init FSE Table]
        // CAN MOVE EARLIER
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.FseProbs);
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.FseInfos);
        // last written by [Parse Compressed Blocks]
        // next read by [Init Huffman Table and Decompress Literals] and [DEBUG READBACK]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.CompressedBlocks);
        // last written by [Parse Compressed Blocks]
        // next read by [Decompress Huffman Weights] and [Decode Uncompressed Huffman Weights] and [DEBUG READBACK]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.HufRefs);
        // last written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.LitGroupEndPerHuffmanTable);
        // last written by [Parse Compressed Blocks]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.LitRefs);
        // NOTE: HufWIdToHufLitId + HufLitIdToLitStreamId were already transitioned UAV->SRV before
        // [Compute Prefix Sum] and stay SRV for [Init Huffman Table and Decompress Literals].
        // last written by [Parse Compressed Blocks]
        // next read by [Decompress Sequences]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToRef);
        // last written by [Propagate FSE Index]
        // next read by [Decompress Sequences]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToLLenFseId);
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToOffsFseId);
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToMLenFseId);
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.SeqStreamToBlockId);
        // last written by [Parse Compressed Blocks]
        // next written by [Decompress Huffman Weights] and read as UAV by [Decode Uncompressed Huffman Weights]
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.DecompressedHuffmanWeightCount);

        ZSTDGPU_ASSERT(bc <= _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    {
        // Run FSE Table Initialisation
        ZSTDGPU_KERNEL_SCOPE(InitFseTable, cmdList,
        {
            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Init FSE Table]");
            BIND_RS_PS_SRT(InitFseTable);
            // NOTE: we run 4 ExecuteIndirects (per argument) in order to be able to (but we don't do this for prototype)
            // switch PSO to more optimial (depending on maximal FSE table size) because D3D12 doesn't allow to switch PSOs in ExecuteIndirect.

            // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch.
            // Slot 2 = table type (0=HufW, 1=LLen, 2=Offs, 3=MLen); the shader derives the bases from Counters.

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Huffman Weights");
            cmdList->SetComputeRoot32BitConstant(1, 0u /* HufW */, 2);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseHufW);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Literal Lengths");
            cmdList->SetComputeRoot32BitConstant(1, 1u /* LLen */, 2);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseLLen);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Offsets");
            cmdList->SetComputeRoot32BitConstant(1, 2u /* Offs */, 2);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseOffs);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Match Lengths");
            cmdList->SetComputeRoot32BitConstant(1, 3u /* MLen */, 2);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseMLen);
            PIXEndEvent(cmdList);
            PIXEndEvent(cmdList);
        });
    }

    // Needed by readback
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Huffman Weights Decompression] and [Decompress Literals]");
        D3D12_RESOURCE_BARRIER barriers[1];
        // last written by [Init FSE Table]
        // next read by [Decompress Huffman Weights] and [Decompress Sequences]
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.FseElems);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    // Run Decompression of FSE-compressed Huffman Weights
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decompress Huffman Weights]");
        BIND_RS_PS_SRT(DecompressHuffmanWeights);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(DecompressHuffmanWeights, cmdList,
            zstdgpu_DispatchIndirect(cmdList, DecompressHuffmanWeights, DecompressHuffmanWeights);
        );
        PIXEndEvent(cmdList);
    }
    // Run Decoding of Uncompressed Huffman Weights (can merge with FSE decompression before barrier)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decode Uncompressed Huffman Weights]");
        BIND_RS_PS_SRT(DecodeHuffmanWeights);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 2);

        ZSTDGPU_KERNEL_SCOPE(DecodeHuffmanWeights, cmdList,
            zstdgpu_DispatchIndirect(cmdList, DecodeHuffmanWeights, DecodeHuffmanWeights);
        );
        PIXEndEvent(cmdList);
    }

    // Needed by Initialisation of Huffman Tables
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Init Huffman Table]");
        D3D12_RESOURCE_BARRIER barriers[2];
        // last written by [Decompress Huffman Weights] and [Decode Uncompressed Huffman Weights]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.DecompressedHuffmanWeights);
        // last written by [Decompress Huffman Weights]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.DecompressedHuffmanWeightCount);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Pre-Init Huffman Table]");
        BIND_RS_PS_SRT(InitHuffmanTable);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(InitHuffmanTable, cmdList,
        {
            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Path: FSE-compressed Huffman Weights]");
            {
                cmdList->SetComputeRoot32BitConstant(1, /** fseCompressed = 1 */1u, 2);
                zstdgpu_DispatchIndirect(cmdList, InitHuffmanTable, FseHufW);
            }
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Path: Uncompressed Huffman Weights]");
            {
                cmdList->SetComputeRoot32BitConstant(1, /** fseCompressed = 0 */0u, 2);
                zstdgpu_DispatchIndirect(cmdList, InitHuffmanTable, HUF_WgtStreams);
            }
            PIXEndEvent(cmdList);
        });

        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Decompress Literals]");
        D3D12_RESOURCE_BARRIER barriers[3];
        // last written by [Init Huffman Table]
        // next read by [Decompress Literals]
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.HuffmanTableInfo);
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.HuffmanTableRankIndex);
        setResourceUavToSrvSync(barriers, 2, req->resData.gpuOnly.HuffmanTableCodeAndSymbol);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decompress Literals]");
        BIND_RS_PS_SRT(DecompressLiterals);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        ZSTDGPU_KERNEL_SCOPE(DecompressLiterals, cmdList,
            zstdgpu_DispatchIndirect(cmdList, DecompressLiterals, DecompressLiterals);
        );
        PIXEndEvent(cmdList);
    }

#if ZSTDGPU_ENABLE_TIMESTAMPS
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"DUMMY Barrier for Profiling");
        D3D12_RESOURCE_BARRIER barriers[1];
        setResourceUavSync(barriers, 0, NULL);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
#endif

    // NOTE(pamartis): (can run in parallel with FSE-compressed Huffman Weight Decompression, right after FSE table initialisation)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decompress Sequences]");
        BIND_RS_PS_SRT(DecompressSequences);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(DecompressSequences, cmdList,
            zstdgpu_DispatchIndirect(cmdList, DecompressSequences, DecompressSequences);
        );
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Prefix Block Sizes] and [Prefix Sequence Offsets] and [Execute Sequences]");
        D3D12_RESOURCE_BARRIER barriers[8];
        uint32_t bc = 0;
        // last written/updated by [Decompress Sequences]
        // next written/updated by [Prefix Block Sizes]
        setResourceUavSync(barriers, bc + 0, req->resData.gpuOnly.BlockSizePrefix);
        // last written by [Decompress Sequences]
        // next read/written by [Prefix Sequence Offsets]
        setResourceUavSync(barriers, bc + 1, req->resData.gpuOnly.PerSeqStreamFinalOffset1);
        setResourceUavSync(barriers, bc + 2, req->resData.gpuOnly.PerSeqStreamFinalOffset2);
        setResourceUavSync(barriers, bc + 3, req->resData.gpuOnly.PerSeqStreamFinalOffset3);
        // last written/updated by [Decompress Sequences]
        // next written/updated by [Finalise Sequence Offsets]
        setResourceUavSync(barriers, bc + 4, req->resData.gpuOnly.DecompressedSequenceOffs);
        // last written/updated by [Decompress Sequences]
        // next read by [Execute Sequences]
        setResourceUavToSrvSync(barriers, bc + 5, req->resData.gpuOnly.DecompressedSequenceLLen);
        setResourceUavToSrvSync(barriers, bc + 6, req->resData.gpuOnly.DecompressedSequenceMLen);
        bc += 7;
        // last written/updated by [Init Huffman Table and Decompress Literals]
        // next read by [Execute Sequences]
        {
            setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.DecompressedLiterals);
            bc += 1;
        }
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }
    {
        const uint32_t allBlockCount = req->zstdRawBlockCountMax
                                     + req->zstdRleBlockCountMax
                                     + req->zstdCmpBlockCountMax;

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Prefix Block Sizes]");
        d3d12aid_ComputeRsPs_Set(&req->PrefixSum, cmdList);

        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.BlockSizePrefix->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.BlockSizePrefix->GetGPUVirtualAddress() + allBlockCount * sizeof(uint32_t));
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(2, 1 /** outputInclusive */, 2);

        ZSTDGPU_KERNEL_SCOPE(PrefixBlockSizes, cmdList,
            zstdgpu_DispatchIndirect(cmdList, PrefixSum, PrefixBlockSizes);
        );

        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Prefix Sequence Offsets]");
        d3d12aid_ComputeRsPs_Set(&req->PrefixSequenceOffsets, cmdList);

        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset1->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerSeqStreamFinalOffset2->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.PerSeqStreamFinalOffset3->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.PerSeqStreamFinalOffset1->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        cmdList->SetComputeRootUnorderedAccessView(4, req->resData.gpuOnly.PerSeqStreamFinalOffset2->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        cmdList->SetComputeRootUnorderedAccessView(5, req->resData.gpuOnly.PerSeqStreamFinalOffset3->GetGPUVirtualAddress() + req->zstdCmpBlockCountMax * sizeof(uint32_t));
        cmdList->SetComputeRootShaderResourceView(6, req->resData.gpuOnly.PerFrameSeqStreamMinIdx->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(7, req->resData.gpuOnly.PerFrameBlockCountAll->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(8, req->resData.gpuOnly.SeqStreamToBlockId->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(9, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(10, req->zstdFrameCount, 2);

        ZSTDGPU_KERNEL_SCOPE(PrefixSequenceOffsets, cmdList,
            zstdgpu_DispatchIndirect(cmdList, PrefixSequenceOffsets, PrefixSequenceOffsets);
        );

        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Finalise Sequence Offsets]");
        D3D12_RESOURCE_BARRIER barriers[4];
        uint32_t bc = 0;
        {
            // last written by [Prefix Sequence Offsets]
            // next read by [Finalise Sequence Offsets]
            setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.PerSeqStreamFinalOffset1);
            setResourceUavToSrvSync(barriers, bc + 1, req->resData.gpuOnly.PerSeqStreamFinalOffset2);
            setResourceUavToSrvSync(barriers, bc + 2, req->resData.gpuOnly.PerSeqStreamFinalOffset3);
            bc += 3;
        }

        // last written by [Prefix Block Sizes]
        // next read by [Compute Dest Block Offsets], [Memcpy RAW blocks, Memset RLE blocks], and [Execute Sequences]
        setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.BlockSizePrefix);
        bc += 1;
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Compute Dest Block Offsets]");
        BIND_RS_PS_SRT(ComputeDestBlockOffsets);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 2);

        ZSTDGPU_KERNEL_SCOPE(ComputeDestBlockOffsets, cmdList,
            zstdgpu_DispatchIndirect(cmdList, ComputeDestBlockOffsets, PrefixBlockSizes);
        );

        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Finalise Sequence Offsets]");
        BIND_RS_PS_SRT(FinaliseSequenceOffsets);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(FinaliseSequenceOffsets, cmdList,
            zstdgpu_DispatchIndirect(cmdList, FinaliseSequenceOffsets, FinaliseSequenceOffsets);
        );

        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Memcpy RAW blocks, Memset RLE blocks] and [Execute Sequences]");
        D3D12_RESOURCE_BARRIER barriers[2];
        // last written/updated by [Finalise Sequence Offsets]
        // next read by [Execute Sequences]
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.DecompressedSequenceOffs);
        // last written by [Compute Dest Block Offsets]
        // next read by [Memcpy RAW blocks, Memset RLE blocks], [Execute Sequences], and [Compute Dest Sequence Offsets]
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.BlockDestOffs);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Memcpy RAW blocks, Memset RLE blocks]");
        d3d12aid_ComputeRsPs_Set(&req->MemsetMemcpy, cmdList);
        cmdList->SetDescriptorHeaps(1, &req->srts.heap);
        cmdList->SetComputeRootDescriptorTable(0, req->srts.MemsetMemcpyGpuHandle);
        ZSTDGPU_KERNEL_SCOPE(MemcpyRAW_MemsetRLE, cmdList,
        {
            {
                cmdList->SetComputeRootShaderResourceView(1, req->resData.gpuOnly.RawBlockSizePrefix->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(2, req->resData.gpuOnly.BlocksRAWRefs->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(3, req->resData.gpuOnly.GlobalBlockIndexPerRawBlock->GetGPUVirtualAddress());
                // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
                cmdList->SetComputeRoot32BitConstant(4, 1 /* flags */, 2);
                zstdgpu_DispatchIndirect(cmdList, MemsetMemcpy, MemcpyRAW);
            }

            {
                cmdList->SetComputeRootShaderResourceView(1, req->resData.gpuOnly.RleBlockSizePrefix->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(2, req->resData.gpuOnly.BlocksRLERefs->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(3, req->resData.gpuOnly.GlobalBlockIndexPerRleBlock->GetGPUVirtualAddress());
                // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
                cmdList->SetComputeRoot32BitConstant(4, 0 /* flags */, 2);
                zstdgpu_DispatchIndirect(cmdList, MemsetMemcpy, MemsetRLE);
            }
        });
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Execute Sequences]");
        D3D12_RESOURCE_BARRIER barriers[2];
        uint32_t bc = 0;
        {
            // in case if the number of RAW+RLE blocks > 0, [Memcpy RAW blocks, Memset RLE blocks] has written to 'UnCompressedFramesData'
            // next read by [Execute Sequences]
            setResourceUavSync(barriers, bc + 0, req->resData.gpuOnly.UnCompressedFramesData);
            bc += 1;
        }
        // next written by [Execute Sequences] when allocating
        setResourceSrvCopyIndirectToUavSync(barriers, bc + 0, req->resData.gpuOnly.Counters);
        bc += 1;

        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Execute Sequences]");
        BIND_RS_PS_SRT(ExecuteSequences);

        ZSTDGPU_KERNEL_SCOPE(ExecuteSequences, cmdList,
            cmdList->Dispatch(req->zstdFrameCount, 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Readback Counters :: After Block Decompression]");
        D3D12_RESOURCE_BARRIER barriers[1];
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.Counters);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    if (0) /** IMPORTANT: requires DecompressedSequencesMLen to contain inclusive prefix of total sequence sizes */
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Compute Dest Sequence Offsets]");
        BIND_RS_PS_SRT(ComputeDestSequenceOffsets);

        zstdgpu_Dispatch32Bit(cmdList, ZSTDGPU_TG_COUNT(req->zstdUncompressedSeqElemCountMax, 256), 1, 0);

        PIXEndEvent(cmdList);
    }
    /* It's needed because Counters are updated during Seqeunce Execution */
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Readback Counters :: After Block Decompression]");
        zstdgpu_PushReadback(Counters);
        PIXEndEvent(cmdList);
    }

    // Unset predication
    if (0 == zstdgpu_IsReadbackRequired(req, 1))
    {
        cmdList->SetPredication(NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    }
#if ZSTDGPU_ENABLE_TIMESTAMPS
    req->timestampSlot[kzstdgpu_KernelScope_Stage2_End] = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);
#endif
}

ZSTDGPU_API void zstdgpu_ReadbackGpuResults(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    // NOTE(pamartis): these are required to make sure D3D12 validation doesn't complain about state mismatch when
    // Read-only resource from the last stage (== 2) get a NON_PS_RESOURCE state as a result of promotion from COMMON state
    // (which happens in case if the stage prior to it (==1) is submitted in a separate CommandList/ExecuteCommandList)
    // and then used as COPY_SOURCE for debug readback
    D3D12_RESOURCE_BARRIER barriers[13];
    uint32_t bc = 0;
    if (zstdgpu_IsReadbackRequired(req, 1))
    {
        setResourceState(barriers, 0, req->resData.gpuOnly.PerFrameBlockCountCMP, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, 1, req->resData.gpuOnly.PerFrameBlockCountAll, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, 2, req->resData.gpuOnly.PerFrameBlockSizesRAW, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, 3, req->resData.gpuOnly.PerFrameBlockSizesRLE, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, 4, req->resData.gpuOnly.PerFrameSeqStreamMinIdx, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        bc += 5;
        {
            setResourceState(barriers, bc + 0, req->resData.gpuOnly.GlobalBlockIndexPerCmpBlock, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            setResourceState(barriers, bc + 1, req->resData.gpuOnly.PerSeqStreamSeqStart, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            bc += 2;
        }
        {
            setResourceState(barriers, bc + 0, req->resData.gpuOnly.GlobalBlockIndexPerRawBlock, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            setResourceState(barriers, bc + 1, req->resData.gpuOnly.RawBlockSizePrefix, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            setResourceState(barriers, bc + 2, req->resData.gpuOnly.BlocksRAWRefs, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            bc += 3;
        }
        {
            setResourceState(barriers, bc + 0, req->resData.gpuOnly.GlobalBlockIndexPerRleBlock, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            setResourceState(barriers, bc + 1, req->resData.gpuOnly.RleBlockSizePrefix, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            setResourceState(barriers, bc + 2, req->resData.gpuOnly.BlocksRLERefs, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
            bc += 3;
        }
    }

    if (bc > 0)
    {
        cmdList->ResourceBarrier(bc, barriers);
    }

    #define ZSTDGPU_BUFFER(type, name) zstdgpu_PushReadback(name);
        ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()
        ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()
        ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()
    #undef  ZSTDGPU_BUFFER
}

ZSTDGPU_API void zstdgpu_RetrieveGpuResults(zstdgpu_ResourceDataCpu *outGpuResources, zstdgpu_PerRequestContext req)
{
    zstdgpu_ResourceDataCpu_InitFromResourceDataGpu(outGpuResources, &req->resData);
}

ZSTDGPU_API void zstdgpu_ReadbackTimestamps(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    d3d12aid_Timestamps_AdvanceFrame(&req->timestamps, cmdList);
}

ZSTDGPU_API uint64_t zstdgpu_RetrieveTimestamps(const wchar_t **outTimestampScopeNames, uint64_t *outTimestampScopeClocks, uint32_t *inoutTimestampScopeCnt, zstdgpu_PerRequestContext req, uint32_t stageIndex)
{
#if ZSTDGPU_ENABLE_TIMESTAMPS
    const uint32_t start = 0 == stageIndex ? kzstdgpu_KernelScope_Stage0Start : (1 == stageIndex ? kzstdgpu_KernelScope_Stage1Start : kzstdgpu_KernelScope_Stage2Start);
    const uint32_t count = 0 == stageIndex ? kzstdgpu_KernelScope_Stage0Count : (1 == stageIndex ? kzstdgpu_KernelScope_Stage1Count : kzstdgpu_KernelScope_Stage2Count);

    const uint32_t stageStartSlot = 0 == stageIndex ? kzstdgpu_KernelScope_Stage0_Start : (1 == stageIndex ? kzstdgpu_KernelScope_Stage1_Start : kzstdgpu_KernelScope_Stage2_Start);
    const uint32_t stageEndSlot   = 0 == stageIndex ? kzstdgpu_KernelScope_Stage0_End   : (1 == stageIndex ? kzstdgpu_KernelScope_Stage1_End   : kzstdgpu_KernelScope_Stage2_End);

    uint32_t outSlotCount = 0;
    uint32_t reqSlotCount = 0;

    for (uint32_t i = start; i < start + count; ++i)
    {
        reqSlotCount += ~0u != req->timestampSlot[i] ? 1u : 0u;
    }

    if (reqSlotCount <= *inoutTimestampScopeCnt)
    {
        for (uint32_t i = start; i < start + count; ++i)
        {
            if (~0u != req->timestampSlot[i])
            {
                outTimestampScopeNames[outSlotCount] = kzstdgpu_KernelScopeDesc[i - kzstdgpu_KernelScope_StageSlotCount];
                outTimestampScopeClocks[outSlotCount ++] = d3d12aid_Timestamps_GetScopeDelta(&req->timestamps, 0, req->timestampSlot[i]);
            }
        }
    }

    *inoutTimestampScopeCnt = outSlotCount;
    return d3d12aid_Timestamps_GetDelta(&req->timestamps, 0, req->timestampSlot[stageStartSlot], req->timestampSlot[stageEndSlot]);
#else
    *inoutTimestampScopeCnt = 0;

    ZSTDGPU_UNUSED(outTimestampScopeNames);
    ZSTDGPU_UNUSED(outTimestampScopeClocks);
    ZSTDGPU_UNUSED(req);
    ZSTDGPU_UNUSED(stageIndex);
    return 0;
#endif /* #if ZSTDGPU_ENABLE_TIMESTAMPS */
}
