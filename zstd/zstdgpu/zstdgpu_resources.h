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
 * A macro-based declaration of all GPU resources used by the zstdgpu library.
 * This declaration is used to generate shader bindings and create resources on CPU.
 */

#pragma once

#include "zstdgpu_structs.h"

#ifdef _MSC_VER
    // NOTE(pamartis): Make sure 4505 is not leaked into a file including this header. 4505 is fine because some non-inlined function may be unused
    __pragma(warning(push))
    __pragma(warning(disable : 4505))   /**< warning C4505: 'function name': unreferenced function with internal linkage has been removed */
#endif

#define ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_0()                                                   \
    ZSTDGPU_BUFFER(int16_t                                  , FseProbsDefault               )   \
    ZSTDGPU_BUFFER(uint32_t                                 , CompressedData                )   \
    ZSTDGPU_BUFFER(zstdgpu_OffsetAndSize                    , FramesRefs                    )

#define ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()                                                 \
    ZSTDGPU_BUFFER(zstdgpu_Counters                         , Counters                      )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerFrameBlockCountRAW         )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerFrameBlockCountRLE         )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerFrameBlockCountCMP         )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerFrameBlockCountAll         )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerFrameBlockSizesRAW         )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerFrameBlockSizesRLE         )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerFrameSeqStreamMinIdx       )   \
    ZSTDGPU_BUFFER(zstdgpu_FrameInfo                        , Frames                        )

#define ZSTDGPU_BUFFERS_LIST_STAGE_0() \
    ZSTDGPU_BUFFER(uint32_t                                 , DispatchArgs                  )   \
    ZSTDGPU_BUFFER(uint32_t                                 , DispatchCnts                  )

#define ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_2() /* empty so far*/

#define ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()                                                 \
    ZSTDGPU_BUFFER(uint8_t                                  , DecompressedLiterals          )   \
    \
    ZSTDGPU_BUFFER(uint32_t                                 , DecompressedSequenceLLen      )   \
    ZSTDGPU_BUFFER(uint32_t                                 , DecompressedSequenceMLen      )   \
    ZSTDGPU_BUFFER(uint32_t                                 , DecompressedSequenceOffs      )   \
    \
    ZSTDGPU_BUFFER(uint8_t                                  , UnCompressedFramesData        )   \
    ZSTDGPU_BUFFER(zstdgpu_OffsetAndSize                    , UnCompressedFramesRefs        )

#define ZSTDGPU_BUFFERS_LIST_STAGE_2() /* empty so far*/\
    ZSTDGPU_BUFFER(uint32_t                                 , DestSequenceOffsets           )

#define ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_1() /* empty so far*/

#define ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()                                                 \
    ZSTDGPU_BUFFER(uint32_t                                 , BlockSizePrefix               )   \
    ZSTDGPU_BUFFER(uint32_t                                 , GlobalBlockIndexPerRawBlock   )   \
    ZSTDGPU_BUFFER(uint32_t                                 , GlobalBlockIndexPerRleBlock   )   \
    ZSTDGPU_BUFFER(uint32_t                                 , GlobalBlockIndexPerCmpBlock   )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerSeqStreamFinalOffset1      )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerSeqStreamFinalOffset2      )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerSeqStreamFinalOffset3      )   \
    ZSTDGPU_BUFFER(uint32_t                                 , PerSeqStreamSeqStart          )   \
    \
    ZSTDGPU_BUFFER(uint32_t                                 , RawBlockSizePrefix            )   \
    ZSTDGPU_BUFFER(uint32_t                                 , RleBlockSizePrefix            )   \
    \
    ZSTDGPU_BUFFER(int16_t                                  , FseProbs                      )   \
    ZSTDGPU_BUFFER(zstdgpu_FseInfo                          , FseInfos                      )   \
    \
    ZSTDGPU_BUFFER(uint32_t                                 , FseElems                      )   \
    \
    ZSTDGPU_BUFFER(uint8_t                                  , DecompressedHuffmanWeights    )   \
    ZSTDGPU_BUFFER(uint8_t                                  , DecompressedHuffmanWeightCount)   \
    \
    ZSTDGPU_BUFFER(zstdgpu_LitStreamInfo                    , LitRefs                       )   \
    ZSTDGPU_BUFFER(zstdgpu_CompressedLiteralHuffmanBucket   , LitStreamBuckets              )   \
    ZSTDGPU_BUFFER(uint32_t                                 , LitStreamRemap                )   \
    ZSTDGPU_BUFFER(zstdgpu_SeqStreamInfo                    , SeqRefs                       )   \
    ZSTDGPU_BUFFER(uint32_t                                 , BlockSeqCountPrefixLookback   )   \
    ZSTDGPU_BUFFER(uint32_t                                 , SeqCountPrefixLookback        )   \
    \
    ZSTDGPU_BUFFER(zstdgpu_OffsetAndSize                    , HufRefs                       )   \
    ZSTDGPU_BUFFER(zstdgpu_CompressedBlockData              , CompressedBlocks              )   \
    ZSTDGPU_BUFFER(zstdgpu_TableIndexLookback               , TableIndexLookback            )   \
    ZSTDGPU_BUFFER(uint32_t                                 , LitStreamEndPerHuffmanTable   )   \
    ZSTDGPU_BUFFER(uint32_t                                 , LitGroupEndPerHuffmanTable    )   \
    \
    ZSTDGPU_BUFFER(zstdgpu_OffsetAndSize                    , BlocksRLERefs                 )   \
    ZSTDGPU_BUFFER(zstdgpu_OffsetAndSize                    , BlocksRAWRefs                 )   \
    ZSTDGPU_BUFFER(zstdgpu_OffsetAndSize                    , BlocksCMPRefs                 )

#define ZSTDGPU_BUFFERS_LIST_STAGE_1() \
    ZSTDGPU_BUFFER(uint32_t                                 , HuffmanTableCodeAndSymbol     )   \
    ZSTDGPU_BUFFER(uint32_t                                 , HuffmanTableRankIndex         )   \
    ZSTDGPU_BUFFER(uint32_t                                 , HuffmanTableInfo              )

#define ZSTDGPU_ALL_BUFFERS_LIST_STAGE_0()     \
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_0()      \
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()    \
    ZSTDGPU_BUFFERS_LIST_STAGE_0()

#define ZSTDGPU_ALL_BUFFERS_LIST_STAGE_1()     \
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_1()      \
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()    \
    ZSTDGPU_BUFFERS_LIST_STAGE_1()

#define ZSTDGPU_ALL_BUFFERS_LIST_STAGE_2()     \
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_2()      \
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()    \
    ZSTDGPU_BUFFERS_LIST_STAGE_2()

#define ZSTDGPU_ALL_BUFFERS_LIST()          \
    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_0()      \
    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_1()      \
    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_2()

#define ZSTDGPU_ALL_BUFFERS_LIST_UPLOAD()   \
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_0()   \
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_1()   \
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_2()

#define ZSTDGPU_ALL_BUFFERS_LIST_READBACK()   \
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()   \
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()   \
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()

static const uint32_t kzstdgpu_ResourceAllocation_StageCount = 3;

typedef struct zstdgpu_ResourceInfo
{
    // declare the number of elements in each buffer
    #define ZSTDGPU_BUFFER(type, name) uint32_t name##_Count;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER

    // declare the number of byte size of each buffer
    #define ZSTDGPU_BUFFER(type, name) uint32_t name##_ByteSizeInternal;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) uint32_t name##_ByteSize;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER

    uint32_t gpuOnly_ByteCount[kzstdgpu_ResourceAllocation_StageCount];
    uint32_t cpu2Gpu_ByteCount[kzstdgpu_ResourceAllocation_StageCount];
    uint32_t gpu2Cpu_ByteCount[kzstdgpu_ResourceAllocation_StageCount];

    // declare byte offsets
    #define ZSTDGPU_BUFFER(type, name) uint32_t name##_gpuOnlyByteOffset;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) uint32_t name##_cpu2GpuByteOffset;
        ZSTDGPU_ALL_BUFFERS_LIST_UPLOAD()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) uint32_t name##_gpu2CpuByteOffset;
        ZSTDGPU_ALL_BUFFERS_LIST_READBACK()
    #undef  ZSTDGPU_BUFFER
} zstdgpu_ResourceInfo;

#ifndef ZSTDGPU_DISABLE_RESOURCE_DATA_GPU

typedef struct zstdgpu_GpuOnlyBuffers
{
    #define ZSTDGPU_BUFFER(type, name) ID3D12Resource *name;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER
} zstdgpu_GpuOnlyBuffers;

typedef struct zstdgpu_Cpu2GpuBuffers
{
    #define ZSTDGPU_BUFFER(type, name) ID3D12Resource *name;
        ZSTDGPU_ALL_BUFFERS_LIST_UPLOAD()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) type *name##Cpu;
        ZSTDGPU_ALL_BUFFERS_LIST_UPLOAD()
    #undef  ZSTDGPU_BUFFER
} zstdgpu_Cpu2GpuBuffers;

typedef struct zstdgpu_Gpu2CpuBuffers
{
    #define ZSTDGPU_BUFFER(type, name) ID3D12Resource *name;
        ZSTDGPU_ALL_BUFFERS_LIST_READBACK()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) const type *name##Cpu;
        ZSTDGPU_ALL_BUFFERS_LIST_READBACK()
    #undef  ZSTDGPU_BUFFER
} zstdgpu_Gpu2CpuBuffers;

typedef struct zstdgpu_ResourceDataGpu
{
    zstdgpu_GpuOnlyBuffers  gpuOnly;
    zstdgpu_Cpu2GpuBuffers  cpu2Gpu;
    zstdgpu_Gpu2CpuBuffers  gpu2Cpu;

    ID3D12Heap *gpuOnly_Heap[kzstdgpu_ResourceAllocation_StageCount];
    ID3D12Heap *cpu2Gpu_Heap[kzstdgpu_ResourceAllocation_StageCount];
    ID3D12Heap *gpu2Cpu_Heap[kzstdgpu_ResourceAllocation_StageCount];

    uint64_t gpuOnly_HeapOffset[kzstdgpu_ResourceAllocation_StageCount];
    uint64_t cpu2Gpu_HeapOffset[kzstdgpu_ResourceAllocation_StageCount];
    uint64_t gpu2Cpu_HeapOffset[kzstdgpu_ResourceAllocation_StageCount];

    uint32_t gpuOnly_ByteCount[kzstdgpu_ResourceAllocation_StageCount];
    uint32_t cpu2Gpu_ByteCount[kzstdgpu_ResourceAllocation_StageCount];
    uint32_t gpu2Cpu_ByteCount[kzstdgpu_ResourceAllocation_StageCount];
} zstdgpu_ResourceDataGpu;

#endif /** ZSTDGPU_DISABLE_RESOURCE_DATA_GPU */

typedef struct zstdgpu_ResourceDataCpu
{
    // declare resources
    #define ZSTDGPU_BUFFER(type, name) type *name;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER

    uint32_t initialisedFromHeap;
    uint32_t padding;
} zstdgpu_ResourceDataCpu;

static void zstdgpu_ResourceInfo_InitZero(zstdgpu_ResourceInfo *outInfo)
{
    #define ZSTDGPU_BUFFER(type, name)  \
        outInfo->name##_Count = 0;      \
        outInfo->name##_ByteSize = 0;   \
        outInfo->name##_ByteSizeInternal = 0;

        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER
}

#define ZSTDGPU_ALIGN_DEFAULT(offset) zstdgpu_AlignUp(offset, 0x10000)

// initialize the counts, sizes and offsets
#define ZSTDGPU_BUFFER(type, name)                                      \
    outInfo->name##_Count = name##_Count;                               \
    outInfo->name##_ByteSize = (outInfo->name##_Count) * sizeof(type);  \
    outInfo->name##_ByteSizeInternal = outInfo->name##_ByteSize;

static void zstdgpu_ResourceInfo_Stage_0_InitSize(zstdgpu_ResourceInfo *outInfo, uint32_t frameCount, uint32_t dataCount)
{
    const uint32_t FseProbsDefault_Count = kzstdgpu_FseDefaultProbCount_LLen
                                         + kzstdgpu_FseDefaultProbCount_Offs
                                         + kzstdgpu_FseDefaultProbCount_MLen;

    const uint32_t Frames_Count = frameCount;
    const uint32_t FramesRefs_Count = frameCount;
    const uint32_t CompressedData_Count = (dataCount + 3) / 4; // because CompressedData is in uint32_t
    const uint32_t Counters_Count = 1;
    const uint32_t PerFrameBlockCountRAW_Count = frameCount + zstdgpu_GetLookbackBlockCount(frameCount);
    const uint32_t PerFrameBlockCountRLE_Count = PerFrameBlockCountRAW_Count;
    const uint32_t PerFrameBlockCountCMP_Count = PerFrameBlockCountRAW_Count;
    const uint32_t PerFrameBlockCountAll_Count = PerFrameBlockCountRAW_Count;
    const uint32_t PerFrameBlockSizesRAW_Count = PerFrameBlockCountRAW_Count;
    const uint32_t PerFrameBlockSizesRLE_Count = PerFrameBlockCountRLE_Count;
    const uint32_t PerFrameSeqStreamMinIdx_Count = frameCount;
    const uint32_t DispatchArgs_Count = kzstdgpu_DispatchSlot_Count * kzstdgpu_DispatchSlot_StrideInUInt32;
    const uint32_t DispatchCnts_Count = kzstdgpu_DispatchSlot_Count;

    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_0()
}

static void zstdgpu_ResourceInfo_Stage_1_InitSize(zstdgpu_ResourceInfo *outInfo, uint32_t rawBlockCount, uint32_t rleBlockCount, uint32_t cmpBlockCount)
{
    const uint32_t BlocksRAWRefs_Count = rawBlockCount;
    const uint32_t BlocksRLERefs_Count = rleBlockCount;
    const uint32_t BlocksCMPRefs_Count = cmpBlockCount;
    const uint32_t allBlockCount = rawBlockCount + rleBlockCount + cmpBlockCount;

    const uint32_t RawBlockSizePrefix_Count = rawBlockCount;
    const uint32_t RleBlockSizePrefix_Count = rleBlockCount;

    // TODO: this must a total of all blocks (including RLE and RAW)
    const uint32_t BlockSizePrefix_Count = allBlockCount + zstdgpu_GetLookbackBlockCount(allBlockCount);
    const uint32_t GlobalBlockIndexPerRawBlock_Count = rawBlockCount;
    const uint32_t GlobalBlockIndexPerRleBlock_Count = rleBlockCount;
    const uint32_t GlobalBlockIndexPerCmpBlock_Count = cmpBlockCount;

    const uint32_t PerSeqStreamFinalOffset1_Count = cmpBlockCount + zstdgpu_GetLookbackBlockCount(cmpBlockCount);
    const uint32_t PerSeqStreamFinalOffset2_Count = PerSeqStreamFinalOffset1_Count;
    const uint32_t PerSeqStreamFinalOffset3_Count = PerSeqStreamFinalOffset1_Count;
    const uint32_t PerSeqStreamSeqStart_Count = cmpBlockCount;

    const uint32_t SeqFseElemMaxCount = kzstdgpu_FseElemMaxCount_LLen + kzstdgpu_FseElemMaxCount_Offs + kzstdgpu_FseElemMaxCount_MLen;
    const uint32_t NonRLE_FseTableCount = cmpBlockCount * 4 + 3; // 4 FSE tables per compressed block (Huff, LLen, Offs, MLen) + 3 default tables for (LLen, Offs, MLen);
    const uint32_t FseTable_Count = kzstdgpu_FseRleTableCount + NonRLE_FseTableCount;
    const uint32_t FseProbs_Count = NonRLE_FseTableCount * kzstdgpu_MaxCount_FseProbs;
    const uint32_t FseTableElem_Count = kzstdgpu_FseRleTableCount + cmpBlockCount * (kzstdgpu_FseElemMaxCount_HufW + SeqFseElemMaxCount) + SeqFseElemMaxCount;

    const uint32_t FseElems_Count = FseTableElem_Count;

    const uint32_t DecompressedHuffmanWeights_Count = cmpBlockCount * kzstdgpu_MaxCount_HuffmanWeights;
    const uint32_t DecompressedHuffmanWeightCount_Count = cmpBlockCount;

    const uint32_t FseInfos_Count = FseTable_Count;

    const uint32_t HufRefs_Count = cmpBlockCount;
    const uint32_t LitRefs_Count = cmpBlockCount * 4;
    const uint32_t LitStreamBuckets_Count = cmpBlockCount * 4;
    const uint32_t LitStreamRemap_Count = cmpBlockCount * 4;
    const uint32_t SeqRefs_Count = cmpBlockCount;
    const uint32_t CompressedBlocks_Count = cmpBlockCount;
    const uint32_t TableIndexLookback_Count = zstdgpu_GetLookbackBlockCount(cmpBlockCount);
    const uint32_t LitStreamEndPerHuffmanTable_Count = cmpBlockCount + zstdgpu_GetLookbackBlockCount(cmpBlockCount);
    const uint32_t LitGroupEndPerHuffmanTable_Count = LitStreamEndPerHuffmanTable_Count;
    const uint32_t BlockSeqCountPrefixLookback_Count = TableIndexLookback_Count;
    const uint32_t SeqCountPrefixLookback_Count = TableIndexLookback_Count;

    const uint32_t HuffmanTableCodeAndSymbol_Count = kzstdgpu_MaxCount_HuffmanWeights * cmpBlockCount;
    const uint32_t HuffmanTableRankIndex_Count = kzstdgpu_MaxCount_HuffmanWeightRanks * cmpBlockCount;
    const uint32_t HuffmanTableInfo_Count = cmpBlockCount;

    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_1()
}

static void zstdgpu_ResourceInfo_Stage_2_InitSize(zstdgpu_ResourceInfo *outInfo, uint32_t literalCount, uint32_t sequencesCount, uint32_t uncompressedFramesByteCount, uint32_t uncompressedFrameCount)
{
    const uint32_t DecompressedLiterals_Count       = literalCount;

    // NOTE(pamartis): we never allocate memory for these because they are always external resources
    const uint32_t UnCompressedFramesData_Count     = uncompressedFramesByteCount;
    const uint32_t UnCompressedFramesRefs_Count     = uncompressedFrameCount;

    const uint32_t DecompressedSequenceLLen_Count   = sequencesCount;
    const uint32_t DecompressedSequenceMLen_Count   = DecompressedSequenceLLen_Count;
    const uint32_t DecompressedSequenceOffs_Count   = DecompressedSequenceLLen_Count;
    const uint32_t DestSequenceOffsets_Count = DecompressedSequenceLLen_Count;

    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_2()
}

#undef  ZSTDGPU_BUFFER

#define ZSTDGPU_BUFFER(type, name)                                                 \
        outInfo->name##_gpuOnlyByteOffset = byteOffset;                             \
        byteOffset = ZSTDGPU_ALIGN_DEFAULT(byteOffset + outInfo->name##_ByteSizeInternal);

static void zstdgpu_ResourceInfo_Stage_0_InitOffsetGpuOnly(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_0()
    outInfo->gpuOnly_ByteCount[0] = byteOffset;
}

static void zstdgpu_ResourceInfo_Stage_1_InitOffsetGpuOnly(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_1()
    outInfo->gpuOnly_ByteCount[1] = byteOffset;
}

static void zstdgpu_ResourceInfo_Stage_2_InitOffsetGpuOnly(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_ALL_BUFFERS_LIST_STAGE_2()
    outInfo->gpuOnly_ByteCount[2] = byteOffset;
}

#undef  ZSTDGPU_BUFFER

#define ZSTDGPU_BUFFER(type, name)                                             \
    outInfo->name##_cpu2GpuByteOffset = byteOffset;                             \
    byteOffset = ZSTDGPU_ALIGN_DEFAULT(byteOffset + outInfo->name##_ByteSizeInternal);

static void zstdgpu_ResourceInfo_Stage_0_InitOffsetCpu2Gpu(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_0()
    outInfo->cpu2Gpu_ByteCount[0] = byteOffset;
}

static void zstdgpu_ResourceInfo_Stage_1_InitOffsetCpu2Gpu(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_1()
    outInfo->cpu2Gpu_ByteCount[1] = byteOffset;
}

static void zstdgpu_ResourceInfo_Stage_2_InitOffsetCpu2Gpu(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_2()
    outInfo->cpu2Gpu_ByteCount[2] = byteOffset;
}

#undef  ZSTDGPU_BUFFER

#define ZSTDGPU_BUFFER(type, name)                                             \
    outInfo->name##_gpu2CpuByteOffset = byteOffset;                            \
    byteOffset = ZSTDGPU_ALIGN_DEFAULT(byteOffset + outInfo->name##_ByteSizeInternal);

static void zstdgpu_ResourceInfo_Stage_0_InitOffsetGpu2Cpu(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()
    outInfo->gpu2Cpu_ByteCount[0] = byteOffset;
}

static void zstdgpu_ResourceInfo_Stage_1_InitOffsetGpu2Cpu(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()
    outInfo->gpu2Cpu_ByteCount[1] = byteOffset;
}

static void zstdgpu_ResourceInfo_Stage_2_InitOffsetGpu2Cpu(zstdgpu_ResourceInfo *outInfo)
{
    uint32_t byteOffset = 0;
    ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()
    outInfo->gpu2Cpu_ByteCount[2] = byteOffset;
}

#undef  ZSTDGPU_BUFFER

static void zstdgpu_ResourceInfo_Stage_0_Init(zstdgpu_ResourceInfo *outInfo, uint32_t frameCount, uint32_t dataCount, uint32_t externalData)
{
    zstdgpu_ResourceInfo_Stage_0_InitSize(outInfo, frameCount, dataCount);

    if (externalData)
    {
        outInfo->CompressedData_ByteSizeInternal = 0;
        outInfo->FramesRefs_ByteSizeInternal     = 0;
    }

    zstdgpu_ResourceInfo_Stage_0_InitOffsetGpuOnly(outInfo);
    zstdgpu_ResourceInfo_Stage_0_InitOffsetCpu2Gpu(outInfo);
    zstdgpu_ResourceInfo_Stage_0_InitOffsetGpu2Cpu(outInfo);
}

static void zstdgpu_ResourceInfo_Stage_1_Init(zstdgpu_ResourceInfo *outInfo, uint32_t rawBlockCount, uint32_t rleBlockCount, uint32_t cmpBlockCount)
{
    zstdgpu_ResourceInfo_Stage_1_InitSize(outInfo, rawBlockCount, rleBlockCount, cmpBlockCount);
    zstdgpu_ResourceInfo_Stage_1_InitOffsetGpuOnly(outInfo);
    zstdgpu_ResourceInfo_Stage_1_InitOffsetCpu2Gpu(outInfo);
    zstdgpu_ResourceInfo_Stage_1_InitOffsetGpu2Cpu(outInfo);
}


static void zstdgpu_ResourceInfo_Stage_2_Init(zstdgpu_ResourceInfo *outInfo, uint32_t literalCount, uint32_t sequencesCount, uint32_t uncompressedFramesByteCount, uint32_t uncompressedFrameCount)
{
    zstdgpu_ResourceInfo_Stage_2_InitSize(outInfo, literalCount, sequencesCount, uncompressedFramesByteCount, uncompressedFrameCount);
    outInfo->UnCompressedFramesData_ByteSizeInternal = 0;
    outInfo->UnCompressedFramesRefs_ByteSizeInternal = 0;
    zstdgpu_ResourceInfo_Stage_2_InitOffsetGpuOnly(outInfo);
    zstdgpu_ResourceInfo_Stage_2_InitOffsetCpu2Gpu(outInfo);
    zstdgpu_ResourceInfo_Stage_2_InitOffsetGpu2Cpu(outInfo);
}

#ifndef ZSTDGPU_DISABLE_RESOURCE_DATA_GPU

static void zstdgpu_ResourceDataGpu_InitZero(zstdgpu_ResourceDataGpu *outResData)
{
    for (uint32_t stage = 0; stage < kzstdgpu_ResourceAllocation_StageCount; ++stage)
    {
        outResData->gpuOnly_Heap[stage] = NULL;
        outResData->cpu2Gpu_Heap[stage] = NULL;
        outResData->gpu2Cpu_Heap[stage] = NULL;

        outResData->gpuOnly_HeapOffset[stage] = 0;
        outResData->cpu2Gpu_HeapOffset[stage] = 0;
        outResData->gpu2Cpu_HeapOffset[stage] = 0;

        outResData->gpuOnly_ByteCount[stage] = 0;
        outResData->cpu2Gpu_ByteCount[stage] = 0;
        outResData->gpu2Cpu_ByteCount[stage] = 0;
    }

    #define ZSTDGPU_BUFFER(type, name) outResData->gpuOnly.name = NULL;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) outResData->cpu2Gpu.name = NULL;
            ZSTDGPU_ALL_BUFFERS_LIST_UPLOAD()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) outResData->gpu2Cpu.name = NULL;
        ZSTDGPU_ALL_BUFFERS_LIST_READBACK()
    #undef  ZSTDGPU_BUFFER
}

static void zstdgpu_ResourceDataGpu_InitHeap(zstdgpu_ResourceDataGpu *outResData, const zstdgpu_ResourceInfo *info, ID3D12Device *device, uint32_t stageIndex)
{
    #define INIT_HEAP(name, stage, type)                                                        \
        if (NULL == outResData->name##_Heap[stage] && 0 != info->name##_ByteCount[stage])       \
        {                                                                                       \
            outResData->name##_Heap[stage] = d3d12aid_Heap_Create_WithHeapTypeAndFlags(device, info->name##_ByteCount[stage], 0, D3D12_HEAP_TYPE_##type, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);\
            outResData->name##_ByteCount[stage] = info->name##_ByteCount[stage];                \
        }

        INIT_HEAP(gpuOnly, stageIndex, DEFAULT)
        INIT_HEAP(cpu2Gpu, stageIndex, UPLOAD)
        INIT_HEAP(gpu2Cpu, stageIndex, READBACK)

    #undef INIT_HEAP
}

static void zstdgpu_ResourceDataGpu_ReInitInputExternal(zstdgpu_ResourceDataGpu *outResData, ID3D12Resource *compressedFrameData, ID3D12Resource *compressedFrameRefs)
{
    ZSTDGPU_ASSERT(NULL != compressedFrameData);
    if (NULL != compressedFrameData && compressedFrameData != outResData->gpuOnly.CompressedData)
    {
        D3D12AID_SAFE_RELEASE(outResData->gpuOnly.CompressedData);

        outResData->gpuOnly.CompressedData = compressedFrameData;
        outResData->gpuOnly.CompressedData->AddRef();
    }

    ZSTDGPU_ASSERT(NULL != compressedFrameRefs);
    if (NULL != compressedFrameRefs && compressedFrameRefs != outResData->gpuOnly.FramesRefs)
    {
        D3D12AID_SAFE_RELEASE(outResData->gpuOnly.FramesRefs);

        outResData->gpuOnly.FramesRefs = compressedFrameRefs;
        outResData->gpuOnly.FramesRefs->AddRef();
    }
}

static void zstdgpu_ResourceDataGpu_ReInitOutputsExternal(zstdgpu_ResourceDataGpu *outResData, ID3D12Resource *uncompressedFramesData, ID3D12Resource *uncompressedFramesRefs)
{
    ZSTDGPU_ASSERT(NULL != uncompressedFramesData);
    if (NULL != uncompressedFramesData && uncompressedFramesData != outResData->gpuOnly.UnCompressedFramesData)
    {
        D3D12AID_SAFE_RELEASE(outResData->gpuOnly.UnCompressedFramesData);

        outResData->gpuOnly.UnCompressedFramesData = uncompressedFramesData;
        outResData->gpuOnly.UnCompressedFramesData->AddRef();
    }

    ZSTDGPU_ASSERT(NULL != uncompressedFramesRefs);
    if (NULL != uncompressedFramesRefs && uncompressedFramesRefs != outResData->gpuOnly.UnCompressedFramesRefs)
    {
        D3D12AID_SAFE_RELEASE(outResData->gpuOnly.UnCompressedFramesRefs);

        outResData->gpuOnly.UnCompressedFramesRefs = uncompressedFramesRefs;
        outResData->gpuOnly.UnCompressedFramesRefs->AddRef();
    }
}

#define USE_PLACED_BUFFERS 1

#if USE_PLACED_BUFFERS
    #define ZSTDGPU_CREATE_BUFFER(name, dest, state, type)              \
        bufDesc.Width = info->name##_ByteSizeInternal;                  \
        outResData->dest.name = d3d12aid_Resource_CreatePlaced(         \
            device,                                                     \
            heap,                                                       \
            baseOffset + info->name##_##dest##ByteOffset,               \
            &bufDesc,                                                   \
            D3D12_RESOURCE_STATE_##state                                \
        );                                                              \
        outResData->dest.name->SetName(L""#name);

#else
    #define ZSTDGPU_CREATE_BUFFER(name, dest, state, type)                      \
        bufDesc.Width = info->name##_ByteSizeInternal;                          \
        outResData->dest.name = d3d12aid_Resource_CreateCommitted(              \
            device,                                                             \
            &bufDesc,                                                           \
            D3D12_HEAP_TYPE_##type                                              \
        );
#endif

#define ZSTDGPU_BUFFER(type, name)                                              \
    if (NULL == outResData->gpuOnly.name && 0 != info->name##_ByteSizeInternal) \
    {                                                                           \
        ZSTDGPU_CREATE_BUFFER(name, gpuOnly, COMMON, DEFAULT);                  \
    }

static void zstdgpu_ResourceDataGpu_Init_GpuOnly(zstdgpu_ResourceDataGpu *outResData, const zstdgpu_ResourceInfo *info, ID3D12Device *device, uint32_t stageIndex)
{
    uint64_t baseOffset = outResData->gpuOnly_HeapOffset[stageIndex];
    D3D12_RESOURCE_DESC bufDesc;
    d3d12aid_Resource_InitAsBuffer(&bufDesc, 0);
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Heap *heap = outResData->gpuOnly_Heap[stageIndex];
    if (stageIndex == 0)
    {
#if defined(_GAMING_XBOX)
        bufDesc.Width = info->DispatchArgs_ByteSizeInternal;
        bufDesc.Flags |= D3D12XBOX_RESOURCE_FLAG_ALLOW_INDIRECT_BUFFER;
        outResData->gpuOnly.DispatchArgs = d3d12aid_Resource_CreateCommitted_WithHeapTypeAndFlags(device, &bufDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12XBOX_HEAP_FLAG_ALLOW_INDIRECT_BUFFERS);
        bufDesc.Flags &= ~D3D12XBOX_RESOURCE_FLAG_ALLOW_INDIRECT_BUFFER;
#endif

        ZSTDGPU_ALL_BUFFERS_LIST_STAGE_0()
    }
    else if (stageIndex == 1)
    {
        ZSTDGPU_ALL_BUFFERS_LIST_STAGE_1()
    }
    else if (stageIndex == 2)
    {
        ZSTDGPU_ALL_BUFFERS_LIST_STAGE_2()
    }

}

#undef ZSTDGPU_BUFFER

#define ZSTDGPU_BUFFER(type, name)                                              \
    if (NULL == outResData->cpu2Gpu.name && 0 != info->name##_ByteSizeInternal) \
    {                                                                           \
        ZSTDGPU_CREATE_BUFFER(name, cpu2Gpu, GENERIC_READ, UPLOAD);             \
        outResData->cpu2Gpu.name##Cpu = (type *)d3d12aid_Resource_MapWrite(outResData->cpu2Gpu.name);\
    }                                                                           \
    else if (NULL == outResData->cpu2Gpu.name##Cpu)                             \
    {                                                                           \
        outResData->cpu2Gpu.name##Cpu = NULL;                                   \
    }

static void zstdgpu_ResourceDataGpu_Init_CpuToGpu(zstdgpu_ResourceDataGpu *outResData, const zstdgpu_ResourceInfo *info, ID3D12Device *device, uint32_t stageIndex)
{
    uint64_t baseOffset = outResData->cpu2Gpu_HeapOffset[stageIndex];
    D3D12_RESOURCE_DESC bufDesc;
    d3d12aid_Resource_InitAsBuffer(&bufDesc, 0);

    ID3D12Heap *heap = outResData->cpu2Gpu_Heap[stageIndex];
    if (stageIndex == 0)
    {
        ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_0()
    }
    else if (stageIndex == 1)
    {
        ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_1()
    }
    else if (stageIndex == 2)
    {
        ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_2()
    }
}

#undef  ZSTDGPU_BUFFER

#undef  ZSTDGPU_CREATE_BUFFER

#if USE_PLACED_BUFFERS
    #define ZSTDGPU_CREATE_BUFFER(name, dest, state, type)              \
        bufDesc.Width = info->name##_ByteSizeInternal;                  \
        outResData->dest.name = d3d12aid_Resource_CreatePlaced(         \
            device,                                                     \
            heap,                                                       \
            baseOffset + info->name##_##dest##ByteOffset,               \
            &bufDesc,                                                   \
            D3D12_RESOURCE_STATE_##state                                \
        );

#else
    #define ZSTDGPU_CREATE_BUFFER(name, dest, state, type)              \
        bufDesc.Width = info->name##_ByteSizeInternal;                  \
        outResData->dest.name = d3d12aid_Resource_CreateCommitted(      \
            device,                                                     \
            &bufDesc,                                                   \
            D3D12_HEAP_TYPE_##type                                      \
        );
#endif

#define ZSTDGPU_BUFFER(type, name)                                                          \
    if (NULL == outResData->gpu2Cpu.name && 0 != info->name##_ByteSizeInternal)             \
    {                                                                                       \
        ZSTDGPU_CREATE_BUFFER(name, gpu2Cpu, COPY_DEST, READBACK);                          \
        outResData->gpu2Cpu.name##Cpu = (const type *)d3d12aid_Resource_MapRead(outResData->gpu2Cpu.name);  \
    }                                                                                       \
    else if (NULL == outResData->gpu2Cpu.name)                                              \
    {                                                                                       \
        outResData->gpu2Cpu.name##Cpu = NULL;                                               \
    }

static void zstdgpu_ResourceDataGpu_Init_GpuToCpu(zstdgpu_ResourceDataGpu *outResData, const zstdgpu_ResourceInfo *info, ID3D12Device *device, uint32_t stageIndex)
{
    uint64_t baseOffset = outResData->gpu2Cpu_HeapOffset[stageIndex];

    D3D12_RESOURCE_DESC bufDesc;
    d3d12aid_Resource_InitAsBuffer(&bufDesc, 0);

    ID3D12Heap *heap = outResData->gpu2Cpu_Heap[stageIndex];
    if (stageIndex == 0)
    {
        ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()
    }
    else if (stageIndex == 1)
    {
        ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()
    }
    else if (stageIndex == 2)
    {
        ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()
    }
}

#undef  ZSTDGPU_BUFFER

#undef  ZSTDGPU_CREATE_BUFFER

static void zstdgpu_ResourceDataGpu_Init(zstdgpu_ResourceDataGpu *outResData, const zstdgpu_ResourceInfo *info, ID3D12Device *device, uint32_t stageIndex)
{
    zstdgpu_ResourceDataGpu_Init_GpuOnly(outResData, info, device, stageIndex);
    zstdgpu_ResourceDataGpu_Init_CpuToGpu(outResData, info, device, stageIndex);
    zstdgpu_ResourceDataGpu_Init_GpuToCpu(outResData, info, device, stageIndex);
}

static void zstdgpu_ResourceDataGpu_Term(zstdgpu_ResourceDataGpu *outResData, uint32_t stageIndex)
{
    ZSTDGPU_ASSERT(stageIndex < kzstdgpu_ResourceAllocation_StageCount);
    if (stageIndex >= kzstdgpu_ResourceAllocation_StageCount)
        return;

    // release the resources
    #define ZSTDGPU_BUFFER(type, name) D3D12AID_SAFE_RELEASE(outResData->gpuOnly.name);
        if (stageIndex == 0)
        {
            ZSTDGPU_ALL_BUFFERS_LIST_STAGE_0()
        }
        else if (stageIndex == 1)
        {
            ZSTDGPU_ALL_BUFFERS_LIST_STAGE_1()
        }
        else if (stageIndex == 2)
        {
            ZSTDGPU_ALL_BUFFERS_LIST_STAGE_2()
        }
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) D3D12AID_SAFE_RELEASE(outResData->cpu2Gpu.name);
        if (stageIndex == 0)
        {
            ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_0()
        }
        else if (stageIndex == 1)
        {
            ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_1()
        }
        else if (stageIndex == 2)
        {
            ZSTDGPU_BUFFERS_LIST_UPLOAD_STAGE_2()
        }
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) D3D12AID_SAFE_RELEASE(outResData->gpu2Cpu.name);
        if (stageIndex == 0)
        {
            ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_0()
        }
        else if (stageIndex == 1)
        {
            ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_1()
        }
        else if (stageIndex == 2)
        {
            ZSTDGPU_BUFFERS_LIST_READBACK_STAGE_2()
        }

    #undef  ZSTDGPU_BUFFER

    // release the heaps
    D3D12AID_SAFE_RELEASE(outResData->gpuOnly_Heap[stageIndex]);
    D3D12AID_SAFE_RELEASE(outResData->cpu2Gpu_Heap[stageIndex]);
    D3D12AID_SAFE_RELEASE(outResData->gpu2Cpu_Heap[stageIndex]);
}

#endif /** ZSTDGPU_DISABLE_RESOURCE_DATA_GPU */

static void zstdgpu_ResourceDataCpu_InitZero(zstdgpu_ResourceDataCpu* outResData)
{
    outResData->initialisedFromHeap = 0;

    #define ZSTDGPU_BUFFER(type, name) outResData->name = NULL;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER
}

#define DEBUG_CPU_MEMORY 1

#if DEBUG_CPU_MEMORY
#include <windows.h>
#endif

static void *alloc(uint32_t size)
{
#if DEBUG_CPU_MEMORY
    MEMORY_BASIC_INFORMATION info;
    SYSTEM_INFO systemInfo;
    GetNativeSystemInfo(&systemInfo);
    const DWORD pageSize = systemInfo.dwPageSize;

    const uint32_t allocatedSize = zstdgpu_AlignUp(size, pageSize) + pageSize;
    void *memory = VirtualAlloc(NULL, allocatedSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    SIZE_T rsize = VirtualQuery(memory, &info, sizeof(info));
    ZSTDGPU_ASSERT(0 != rsize);
    ZSTDGPU_ASSERT(info.AllocationBase == info.BaseAddress);
    ZSTDGPU_ASSERT(info.AllocationBase == memory);

    void *protect = (char *)memory + info.RegionSize - pageSize;
    DWORD oldProtect = 0;
    if (FALSE == VirtualProtect(protect, pageSize, PAGE_READONLY, &oldProtect))
    {
        DWORD result = GetLastError();
        (void)result;
        ZSTDGPU_BREAK();
    }
    return (char *)protect - size;
#else
    return malloc(size);
#endif
}

static void dealloc(void *ptr)
{
#if DEBUG_CPU_MEMORY
    MEMORY_BASIC_INFORMATION info;
    SIZE_T size = VirtualQuery(ptr, &info, sizeof(info));
    ZSTDGPU_ASSERT(0 != size);
    VirtualFree(info.BaseAddress, 0, MEM_RELEASE);
#else
    return free(ptr);
#endif
}

static void markreadonly(void *ptr)
{
#if DEBUG_CPU_MEMORY
    MEMORY_BASIC_INFORMATION info;
    SIZE_T size = VirtualQuery(ptr, &info, sizeof(info));
    DWORD oldProtect = 0;
    ZSTDGPU_ASSERT(0 != size);
    if (FALSE == VirtualProtect(info.BaseAddress, info.RegionSize, PAGE_READONLY, &oldProtect))
    {
        DWORD result = GetLastError();
        (void)result;
        ZSTDGPU_BREAK();
    }

#else
    ZSTDGPU_UNUSED(ptr);
#endif
}


static void zstdgpu_ResourceDataCpu_InitFromHeap(zstdgpu_ResourceDataCpu *outResData, const zstdgpu_ResourceInfo *info)
{
    outResData->initialisedFromHeap = 1;
    #define ZSTDGPU_BUFFER(type, name) if (outResData->name == NULL && info->name##_ByteSizeInternal != 0) outResData->name = (type *)alloc(info->name##_ByteSizeInternal);
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER
}

#ifndef ZSTDGPU_DISABLE_RESOURCE_DATA_GPU

static void zstdgpu_ResourceDataCpu_InitFromResourceDataGpu(zstdgpu_ResourceDataCpu *outResData, zstdgpu_ResourceDataGpu *resourceDataGpu)
{
    outResData->initialisedFromHeap = 0;

    #define ZSTDGPU_BUFFER(type, name) outResData->name = NULL;
        ZSTDGPU_ALL_BUFFERS_LIST()
    #undef  ZSTDGPU_BUFFER

    #define ZSTDGPU_BUFFER(type, name) outResData->name = (type *)resourceDataGpu->gpu2Cpu.name##Cpu;
        ZSTDGPU_ALL_BUFFERS_LIST_READBACK()
    #undef  ZSTDGPU_BUFFER
}

#endif /** ZSTDGPU_DISABLE_RESOURCE_DATA_GPU */

static void zstdgpu_ResourceDataCpu_Term(zstdgpu_ResourceDataCpu *inoutResData)
{
    // release the resources
    if (inoutResData->initialisedFromHeap)
    {
        #define ZSTDGPU_BUFFER(type, name) dealloc(inoutResData->name);
            ZSTDGPU_ALL_BUFFERS_LIST()
        #undef  ZSTDGPU_BUFFER
    }
}

static void zstdgpu_ResourceDataCpu_MarkReadOnly(zstdgpu_ResourceDataCpu *inoutResData)
{
    // release the resources
    if (inoutResData->initialisedFromHeap)
    {
        #define ZSTDGPU_BUFFER(type, name) if (NULL != inoutResData->name) markreadonly(inoutResData->name);
            ZSTDGPU_ALL_BUFFERS_LIST()
        #undef  ZSTDGPU_BUFFER
    }
}

#ifdef _MSC_VER
    __pragma(warning(pop))
#endif
