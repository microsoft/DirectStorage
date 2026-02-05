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
 */

#include <stdint.h>
#include <assert.h>
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

#define D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT 2
#define D3D12AID_API_STATIC 1
#include "d3d12aid.h"
#include "zstdgpu_resources.h"

#include <pix3.h>

#include "ZstdGpuComputeDestSequenceOffsets.h"
#include "ZstdGpuComputePrefixSum.h"
#include "ZstdGpuDecodeHuffmanWeights.h"
#include "ZstdGpuDecompressHuffmanWeights.h"
#include "ZstdGpuDecompressSequences.h"
#include "ZstdGpuExecuteSequences128.h"
#include "ZstdGpuExecuteSequences64.h"
#include "ZstdGpuExecuteSequences32.h"
#include "ZstdGpuFinaliseSequenceOffsets.h"
#include "ZstdGpuGroupCompressedLiterals.h"
#include "ZstdGpuInitFseTable.h"
#include "ZstdGpuInitHuffmanTableAndDecompressLiterals.h"
#include "ZstdGpuInitResources.h"
#include "ZstdGpuMemsetMemcpy.h"
#include "ZstdGpuParseCompressedBlocks.h"
#include "ZstdGpuParseFrames.h"
#include "ZstdGpuPrefixSequenceOffsets.h"
#include "ZstdGpuPrefixSum.h"
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

void zstdgpu_CountFramesAndBlocks(zstdgpu_CountFramesAndBlocksInfo *outInfo, const void *memoryBlock, uint32_t memoryBlockSizeInBytes, uint32_t contentSizeInBytes)
{
    uint32_t byteOfs = 0;

    zstdgpu_Forward_BitBuffer bits;
    zstdgpu_FrameInfo frameInfo;
    zstdgpu_OffsetAndSize unusedRawBlockSize;
    zstdgpu_OffsetAndSize unusedRLEBlockSize;
    zstdgpu_OffsetAndSize unusedCmpBlockSize;

    outInfo->rawBlockCount  = 0;
    outInfo->rleBlockCount  = 0;
    outInfo->cmpBlockCount  = 0;
    outInfo->frameCount     = 0;
    outInfo->frameByteCount = 0;

    zstdgpu_Forward_BitBuffer_Init(bits, (uint32_t *)memoryBlock, contentSizeInBytes, memoryBlockSizeInBytes);

    while (byteOfs < bits.datasz)
    {
        const uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
        if (magic == 0xFD2FB528U)
        {
            frameInfo.rawBlockStart = 0;
            frameInfo.rleBlockStart = 0;
            frameInfo.cmpBlockStart = 0;
            zstdgpu_ShaderEntry_ParseFrame(frameInfo, &unusedRawBlockSize, &unusedRLEBlockSize, &unusedCmpBlockSize, NULL, NULL, NULL, NULL, NULL, NULL, bits, 0);

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
    zstdgpu_FrameInfo frameInfo;
    zstdgpu_OffsetAndSize unusedRawBlockSize;
    zstdgpu_OffsetAndSize unusedRLEBlockSize;
    zstdgpu_OffsetAndSize unusedCmpBlockSize;

    zstdgpu_Forward_BitBuffer_Init(bits, (uint32_t*)memoryBlock, contentSizeInBytes, memoryBlockSizeInBytes);

    frameInfo.rawBlockStart = 0;
    frameInfo.rleBlockStart = 0;
    frameInfo.cmpBlockStart = 0;

    for (uint32_t frameId = 0; frameId < frameCount; ++frameId)
    {
        const uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
        if (magic == 0xFD2FB528U)
        {
            outFrames[frameId].offs = byteOfs;

            // store prefix
            outFrameInfos[frameId].rawBlockStart = frameInfo.rawBlockStart;
            outFrameInfos[frameId].rleBlockStart = frameInfo.rleBlockStart;
            outFrameInfos[frameId].cmpBlockStart = frameInfo.cmpBlockStart;

            frameInfo.rawBlockStart = 0;
            frameInfo.rleBlockStart = 0;
            frameInfo.cmpBlockStart = 0;
            zstdgpu_ShaderEntry_ParseFrame(frameInfo, &unusedRawBlockSize, &unusedRLEBlockSize, &unusedCmpBlockSize, NULL, NULL, NULL, NULL, NULL, NULL, bits, 0);

            // store just retrieved data
            outFrameInfos[frameId].windowSize = frameInfo.windowSize;
            outFrameInfos[frameId].uncompSize = frameInfo.uncompSize;
            outFrameInfos[frameId].dictionary = frameInfo.dictionary;

            byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);
            outFrames[frameId].size = byteOfs - outFrames[frameId].offs;

            // accumulate previous prefix onto current frame's block counts
            frameInfo.rawBlockStart += outFrameInfos[frameId].rawBlockStart;
            frameInfo.rleBlockStart += outFrameInfos[frameId].rleBlockStart;
            frameInfo.cmpBlockStart += outFrameInfos[frameId].cmpBlockStart;
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
    zstdgpu_FrameInfo frameInfo;

    zstdgpu_Forward_BitBuffer_InitWithSegment(bits, (uint32_t *)memoryBlock, frames[frameIndex], memoryBlockSizeInBytes);

    frameInfo.rawBlockStart = frameInfos[frameIndex].rawBlockStart;
    frameInfo.rleBlockStart = frameInfos[frameIndex].rleBlockStart;
    frameInfo.cmpBlockStart = frameInfos[frameIndex].cmpBlockStart;

    const uint32_t magic = zstdgpu_Forward_BitBuffer_Get(bits, 32);
    if (magic == 0xFD2FB528U)
    {
        const uint32_t byteEnd = frameIndex < frameCount - 1u ? frames[frameIndex + 1u].offs : contentSizeInBytes;

        zstdgpu_ShaderEntry_ParseFrame(frameInfo, outBlocksRaw, outBlocksRLE, outBlocksCmp, NULL, NULL, NULL, NULL, NULL, NULL, bits, 1u);
        byteOfs = zstdgpu_Forward_BitBuffer_GetByteOffset(bits);

        ZSTDGPU_ASSERT(byteOfs == byteEnd);
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

static uint32_t zstdgpu_Count_SRTs(void)
{
    uint32_t descCount = 0;

    #define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                      descCount += 1;
    #define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                      descCount += 1;
    #define ZSTDGPU_RW_BUFFER_DECL_GLC(type, name, index)                  descCount += 1;
    #define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     descCount += 1;
    #define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     descCount += 1;

    #define ZSTDGPU_SRT(name, SRT) SRT
        ZSTDGPU_SRT_LIST()
    #undef  ZSTDGPU_SRT

    #undef ZSTDGPU_RW_TYPED_BUFFER_DECL
    #undef ZSTDGPU_RO_TYPED_BUFFER_DECL
    #undef ZSTDGPU_RW_BUFFER_DECL_GLC
    #undef ZSTDGPU_RW_BUFFER_DECL
    #undef ZSTDGPU_RO_BUFFER_DECL

    return descCount;
}

static void zstdgpu_ReCreate_SRTs(zstdgpu_SRTs & srts, ID3D12Device *device, const zstdgpu_ResourceInfo & resInfo, const zstdgpu_ResourceDataGpu & gpuResData)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = d3d12aid_DescriptorHeap_GetGpuStart(srts.heap);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = d3d12aid_DescriptorHeap_GetCpuStart(srts.heap);
    const uint32_t descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE cpuDest = { cpuStart.ptr + srts.heapOffset * descSize };
    D3D12_GPU_DESCRIPTOR_HANDLE gpuDest = { gpuStart.ptr + srts.heapOffset * descSize };

    D3D12_SHADER_RESOURCE_VIEW_DESC SRV;
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAV;
    const DXGI_FORMAT DXGI_FORMAT_uint8_t = DXGI_FORMAT_R8_UINT;
    const DXGI_FORMAT DXGI_FORMAT_uint16_t = DXGI_FORMAT_R16_UINT;
    const DXGI_FORMAT DXGI_FORMAT_int16_t = DXGI_FORMAT_R16_SINT;

    #define ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, viewType) \
        d3d12aid_##viewType##_Create(cpuDest, device,                                                       \
            gpuResData.gpuOnly.name,                                                                        \
            d3d12aid_##viewType##_InitAsStructBuffer(&viewType, resInfo.name##_ByteSize, sizeof(type))      \
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

    #define ZSTDGPU_RO_BUFFER_DECL(type, name, index) ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, SRV)
    #define ZSTDGPU_RW_BUFFER_DECL(type, name, index) ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, UAV)
    #define ZSTDGPU_RW_BUFFER_DECL_GLC(type, name, index) ZSTDGPU_PUSH_STRUCT_BUFFER(type, name, UAV)
    #define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index) ZSTDGPU_PUSH_TYPED_BUFFER(type, name, SRV)
    #define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index) ZSTDGPU_PUSH_TYPED_BUFFER(type, name, UAV)

    #define ZSTDGPU_SRT(name, SRT) srts.name##GpuHandle = gpuDest; SRT
        ZSTDGPU_SRT_LIST()
    #undef  ZSTDGPU_SRT

    #undef ZSTDGPU_RW_TYPED_BUFFER_DECL
    #undef ZSTDGPU_RO_TYPED_BUFFER_DECL
    #undef ZSTDGPU_RW_BUFFER_DECL_GLC
    #undef ZSTDGPU_RW_BUFFER_DECL
    #undef ZSTDGPU_RO_BUFFER_DECL
}

#define ZSTDGPU_KERNEL_LIST()                                                                                                       \
    ZSTDGPU_KERNEL(ComputeDestSequenceOffsets               , L"Compute Destination Sequence Offsets")                              \
    ZSTDGPU_KERNEL(ComputePrefixSum                         , L"Compute Prefix of Literal and TG Count for Literal Decompression")  \
    ZSTDGPU_KERNEL(DecodeHuffmanWeights                     , L"Decode (from nibbles) Uncompressed Huffman Weights")                \
    ZSTDGPU_KERNEL(DecompressHuffmanWeights                 , L"Decompress FSE-compressed Huffman Weights")                         \
    ZSTDGPU_KERNEL(DecompressSequences                      , L"Decompress Sequences")                                              \
    ZSTDGPU_KERNEL(ExecuteSequences128                      , L"Execute Sequences 128")                                             \
    ZSTDGPU_KERNEL(ExecuteSequences64                       , L"Execute Sequences 64")                                              \
    ZSTDGPU_KERNEL(ExecuteSequences32                       , L"Execute Sequences 32")                                              \
    ZSTDGPU_KERNEL(FinaliseSequenceOffsets                  , L"Finalise Sequence Offsets")                                         \
    ZSTDGPU_KERNEL(GroupCompressedLiterals                  , L"Group Huffman-compressed Literals")                                 \
    ZSTDGPU_KERNEL(InitFseTable                             , L"Init Fse Table")                                                    \
    ZSTDGPU_KERNEL(InitHuffmanTableAndDecompressLiterals    , L"Init Huffman Table and Decompress Literals")                        \
    ZSTDGPU_KERNEL(InitResources                            , L"Init Resources")                                                    \
    ZSTDGPU_KERNEL(MemsetMemcpy                             , L"Memset-Memcpy")                                                     \
    ZSTDGPU_KERNEL(ParseCompressedBlocks                    , L"Parse Compressed Blocks")                                           \
    ZSTDGPU_KERNEL(ParseFrames                              , L"Parse Frames")                                                      \
    ZSTDGPU_KERNEL(PrefixSequenceOffsets                    , L"Prefix Sequence Offsets")                                           \
    ZSTDGPU_KERNEL(PrefixSum                                , L"Prefix Sum")                                                        \
    ZSTDGPU_KERNEL(UpdateDispatchArgs                       , L"Update Dispatch Args")

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0() \
    ZSTDGPU_KERNEL_SCOPE_X(InitResources_CountBlocks            , L"Init Resources"             )   \
    ZSTDGPU_KERNEL_SCOPE_X(ParseFrames_CountBlocks              , L"Parse Frames"               )   \
    ZSTDGPU_KERNEL_SCOPE_X(PrefixSum                            , L"Prefix Sums"                )

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1() \
    ZSTDGPU_KERNEL_SCOPE_X(InitResources                        , L"Init Resources"             )   \
    ZSTDGPU_KERNEL_SCOPE_X(ParseFrames                          , L"Parse Frames"               )   \
    ZSTDGPU_KERNEL_SCOPE_X(ParseCompressedBlocks                , L"Parse Compressed Blocks"    )

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_CMP_BLOCKS() \
    ZSTDGPU_KERNEL_SCOPE_X(UpdateDispatchArgs                   , L"Update Dispatch Arguments"  )   \
    ZSTDGPU_KERNEL_SCOPE_X(ComputePrefixSum                     , L"Compute Prefix Sums"        )   \
    ZSTDGPU_KERNEL_SCOPE_X(GroupCompressedLiterals              , L"Group Compressed Literals"  )   \
    ZSTDGPU_KERNEL_SCOPE_X(InitFseTable                         , L"Init FSE Tables"            )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecompressHuffmanWeights             , L"Decompress Huffman Weights" )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecodeHuffmanWeights                 , L"Decode Huffman Weights"     )   \
    ZSTDGPU_KERNEL_SCOPE_X(InitHuffmanTableAndDecompressLiterals, L"Init Huffman Table and Decompress Literals Huffman Weights" )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecompressSequences                  , L"Decompress Sequences"       )   \
    ZSTDGPU_KERNEL_SCOPE_X(PrefixSequenceOffsets                , L"Propagate Sequence Offsets" )   \
    ZSTDGPU_KERNEL_SCOPE_X(FinaliseSequenceOffsets              , L"Finalise Sequence Offsets"  )   \
    ZSTDGPU_KERNEL_SCOPE_X(ExecuteSequences                     , L"ExecuteSequences"           )

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_RAW_RLE_BLOCKS() \
    ZSTDGPU_KERNEL_SCOPE_X(MemcpyRAW_MemsetRLE                  , L"Memcpy Raw/Memset RLE Blocks")

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_ALL_BLOCKS() \
    ZSTDGPU_KERNEL_SCOPE_X(PrefixBlockSizes                     , L"Prefix Block Sizes"         )

#define ZSTDGPU_KERNEL_SCOPE_LIST()                     \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0()                 \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1()                 \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_CMP_BLOCKS()      \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_RAW_RLE_BLOCKS()  \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_ALL_BLOCKS()

struct zstdgpu_PersistentContextImpl
{
    void                    *thisMemoryBlock;
    ID3D12Device            *device;
    ID3D12CommandSignature  *dispatchCmdSig;
    uint32_t                 minLaneCount;
    uint32_t                 maxLaneCount;

    #define ZSTDGPU_KERNEL(name, desc) d3d12aid_ComputeRsPs name;
        ZSTDGPU_KERNEL_LIST()
    #undef ZSTDGPU_KERNEL
};

enum zstdgpu_SetupInputsType
{
    kzstdgpu_SetupInputsType_Frames_CpuMemory   = 0,
    kzstdgpu_SetupInputsType_Frames_GpuMemory   = 1,

    kzstdgpu_SetupInputsType_Frames_Unknown     = 0x7fffffff
};

struct zstdgpu_PerRequestContextImpl
{
    void                    *thisMemoryBlock;
    ID3D12Device            *device;
    ID3D12CommandSignature  *dispatchCmdSig;

    #define ZSTDGPU_KERNEL(name, desc) d3d12aid_ComputeRsPs name;
        ZSTDGPU_KERNEL_LIST()
    #undef ZSTDGPU_KERNEL
    d3d12aid_ComputeRsPs    ExecuteSequences;

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

    #define ZSTDGPU_KERNEL_SCOPE_X(name, desc) uint32_t name##_TimestampSlot;
        ZSTDGPU_KERNEL_SCOPE_LIST()
    #undef  ZSTDGPU_KERNEL_SCOPE_X

    uint32_t                zstdFrameCount;
    uint32_t                zstdCompressedFramesByteCount;
    uint32_t                zstdUncompressedFrameCount;
    uint32_t                zstdUncompressedFramesByteCount;

    uint32_t                zstdRawBlockCount;
    uint32_t                zstdRleBlockCount;
    uint32_t                zstdCmpBlockCount;

    uint32_t                zstdRawByteCount;
    uint32_t                zstdRleByteCount;

    uint32_t                zstdUncompressedLiteralsByteCount;
    uint32_t                zstdUncompressedSequenceCount;
    uint32_t                zstdSeqStreamCount;

    zstdgpu_SetupInputsType setupInputsType;
};

static uint32_t zstdgpu_GetStageCountFromSetupInputsType(zstdgpu_SetupInputsType type)
{
    ZSTDGPU_UNUSED(type);
    return 3u;
}

uint32_t zstdgpu_GetPersistentContextRequiredMemorySizeInBytes(void)
{
    return sizeof(zstdgpu_PersistentContextImpl);
}

uint32_t zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes(void)
{
    return sizeof(zstdgpu_PerRequestContextImpl);
}

zstdgpu_Status zstdgpu_CreatePersistentContext(zstdgpu_PersistentContext *outPersistentContext, ID3D12Device *device, void *memoryBlock, uint32_t memoryBlockSizeInBytes)
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

        D3D12_INDIRECT_ARGUMENT_DESC dispatchArgDesc;
        dispatchArgDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

        D3D12_COMMAND_SIGNATURE_DESC dispatchCmdSigDesc;
        dispatchCmdSigDesc.ByteStride           = sizeof(uint32_t) * 3;
        dispatchCmdSigDesc.NumArgumentDescs     = 1;
        dispatchCmdSigDesc.pArgumentDescs       = &dispatchArgDesc;
        dispatchCmdSigDesc.NodeMask             = 0x1;
        D3D12AID_CHECK(device->CreateCommandSignature(&dispatchCmdSigDesc, NULL, D3D12AID_IID_PPV_ARGS(&context->dispatchCmdSig)));

        D3D12_FEATURE_DATA_D3D12_OPTIONS1 featureOptions1;
        D3D12AID_CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &featureOptions1, sizeof(featureOptions1)));
        context->minLaneCount = featureOptions1.WaveLaneCountMin;
        context->maxLaneCount = featureOptions1.WaveLaneCountMax;

        /** NOTE(pamartis): generate PipelineState / RootSignature initialisation through macro list */
        #define ZSTDGPU_KERNEL(name, desc) \
            d3d12aid_ComputeRsPs_Create(&context->name, device, g_ZstdGpu##name, sizeof(g_ZstdGpu##name));\
            context->name.rs->SetName(desc);\
            context->name.ps->SetName(desc);

            ZSTDGPU_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL

        *outPersistentContext = context;
        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}


zstdgpu_Status zstdgpu_DestroyPersistentContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PersistentContext inPersistentContext)
{
    const uint32_t proceed = inPersistentContext->thisMemoryBlock == (void *)inPersistentContext;
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed > 0)
    {
        #define ZSTDGPU_KERNEL(name, desc) d3d12aid_ComputeRsPs_Release(&inPersistentContext->name);
            ZSTDGPU_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL

        D3D12AID_SAFE_RELEASE(inPersistentContext->dispatchCmdSig);
        D3D12AID_SAFE_RELEASE(inPersistentContext->device);

        if (NULL != outMemoryBlock)
            *outMemoryBlock = inPersistentContext->thisMemoryBlock;

        if (NULL != outMemoryBlockSizeInBytes)
            *outMemoryBlockSizeInBytes = zstdgpu_GetPersistentContextRequiredMemorySizeInBytes();

        inPersistentContext->thisMemoryBlock = NULL;

        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

zstdgpu_Status zstdgpu_CreatePerRequestContext(zstdgpu_PerRequestContext *outPerRequestContext, zstdgpu_PersistentContext persistentContext, void *memoryBlock, uint32_t memoryBlockSizeInBytes)
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

        /** NOTE(pamartis): generate PipelineState / RootSignature initialisation through macro list */
        #define ZSTDGPU_KERNEL(name, desc)          \
            context->name = persistentContext->name;\
            context->name.rs->AddRef();             \
            context->name.ps->AddRef();
            ZSTDGPU_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL
#ifdef _GAMING_XBOX_SCARLETT
        context->ExecuteSequences = context->ExecuteSequences64;
#else
        if (persistentContext->maxLaneCount == 128)
        {
            context->ExecuteSequences = context->ExecuteSequences128;
        }
        else if (persistentContext->maxLaneCount == 64)
        {
            context->ExecuteSequences = context->ExecuteSequences64;
        }
        else
        {
            context->ExecuteSequences = context->ExecuteSequences32;
        }
#endif
        context->ExecuteSequences.rs->AddRef();
        context->ExecuteSequences.ps->AddRef();

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

        const uint32_t timestampCount = 0
        #define ZSTDGPU_KERNEL_SCOPE_X(name, desc) +2
            ZSTDGPU_KERNEL_SCOPE_LIST();
        #undef  ZSTDGPU_KERNEL_SCOPE_X
        d3d12aid_Timestamps_Create(&context->timestamps, context->device, timestampCount, 1);

        context->zstdFrameCount                     = 0;
        context->zstdCompressedFramesByteCount      = 0;

        context->zstdUncompressedFrameCount         = 0;
        context->zstdUncompressedFramesByteCount    = 0;

        context->zstdRawBlockCount                  = 0;
        context->zstdRleBlockCount                  = 0;
        context->zstdCmpBlockCount                  = 0;
        context->zstdRawByteCount                   = 0;
        context->zstdRleByteCount                   = 0;
        context->zstdUncompressedLiteralsByteCount  = 0;
        context->zstdUncompressedSequenceCount      = 0;
        context->zstdSeqStreamCount                 = 0;
        context->setupInputsType                    = kzstdgpu_SetupInputsType_Frames_Unknown;

        *outPerRequestContext = context;
        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

zstdgpu_Status zstdgpu_DestroyPerRequestContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PerRequestContext inPerRequestContext)
{
    const uint32_t proceed = inPerRequestContext->thisMemoryBlock == (void *)inPerRequestContext;
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        d3d12aid_Timestamps_Release(&inPerRequestContext->timestamps);

        const uint32_t stageCount = zstdgpu_GetStageCountFromSetupInputsType(inPerRequestContext->setupInputsType);
        for (uint32_t stage = 0; stage < stageCount; ++stage)
        {
            zstdgpu_ResourceDataGpu_Term(&inPerRequestContext->resData, stage);
        }

        D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesData);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->compressedFramesRefs);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->uncompressedFramesData);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->uncompressedFramesRefs);

        D3D12AID_SAFE_RELEASE(inPerRequestContext->srts.heap);

        d3d12aid_ComputeRsPs_Release(&inPerRequestContext->ExecuteSequences);
        #define ZSTDGPU_KERNEL(name, desc) d3d12aid_ComputeRsPs_Release(&inPerRequestContext->name);
            ZSTDGPU_KERNEL_LIST()
        #undef ZSTDGPU_KERNEL

        D3D12AID_SAFE_RELEASE(inPerRequestContext->dispatchCmdSig);
        D3D12AID_SAFE_RELEASE(inPerRequestContext->device);

        if (NULL != outMemoryBlock)
            *outMemoryBlock = inPerRequestContext->thisMemoryBlock;

        if (NULL != outMemoryBlockSizeInBytes)
            *outMemoryBlockSizeInBytes = zstdgpu_GetPersistentContextRequiredMemorySizeInBytes();

        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

zstdgpu_Status zstdgpu_SetupInputsAsFramesInCpuMemory(uint32_t *outStageCount, zstdgpu_PerRequestContext inPerRequestContext, uint32_t frameCount, uint32_t framesMemorySizeInBytes, zstdgpu_UploadFrames *uploadCallback, void *uploadUserdata)
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
        if (kzstdgpu_SetupInputsType_Frames_GpuMemory == inPerRequestContext->setupInputsType)
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
        inPerRequestContext->setupInputsType                = kzstdgpu_SetupInputsType_Frames_CpuMemory;

        *outStageCount = 3u;
        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

zstdgpu_Status zstdgpu_SetupInputsAsFramesInGpuMemory(uint32_t *outStageCount, zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *framesMemory, uint32_t framesMemorySizeInBytes, struct ID3D12Resource *frames, uint32_t frameCount)
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
        if (kzstdgpu_SetupInputsType_Frames_GpuMemory == inPerRequestContext->setupInputsType)
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
        inPerRequestContext->setupInputsType                = kzstdgpu_SetupInputsType_Frames_GpuMemory;

        *outStageCount = 3u;
        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

ZSTDGPU_API zstdgpu_Status zstdgpu_SetupOutputs(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *framesMemory, uint32_t framesMemorySizeInBytes, struct ID3D12Resource *frames, uint32_t frameCount)
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
        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

zstdgpu_Status zstdgpu_GetGpuMemoryRequirement(uint32_t *outDefaultHeapByteCount, uint32_t *outUploadHeapByteCount, uint32_t *outReadbackHeapByteCount, uint32_t *outShaderVisibleDescriptorCount, zstdgpu_PerRequestContext req, uint32_t stageIndex)
{
    uint32_t proceed = 1;
    uint32_t stageCount = zstdgpu_GetStageCountFromSetupInputsType(req->setupInputsType);

    proceed = proceed && (NULL != outDefaultHeapByteCount);
    proceed = proceed && (NULL != outUploadHeapByteCount);
    proceed = proceed && (NULL != outReadbackHeapByteCount);
    proceed = proceed && (NULL != outShaderVisibleDescriptorCount);
    proceed = proceed && (stageIndex < stageCount);
    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    proceed = proceed && (req->zstdFrameCount > 0);
    proceed = proceed && (req->zstdCompressedFramesByteCount > 0);
    proceed = proceed && (req->zstdUncompressedFrameCount == req->zstdFrameCount);
    proceed = proceed && (req->zstdUncompressedFramesByteCount > 0);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        #define CNTRS(name) req->resData.gpu2Cpu.CountersCpu[kzstdgpu_CounterIndex_##name]
        if (stageIndex == 0)
        {
            zstdgpu_ResourceInfo_Stage_0_Init(&req->resInfo, req->zstdFrameCount, req->zstdCompressedFramesByteCount, kzstdgpu_SetupInputsType_Frames_GpuMemory == req->setupInputsType ? 1u : 0u);
        }
        else if (stageIndex == 1)
        {
            const uint32_t cntRaw = CNTRS(Blocks_RAW);
            const uint32_t cntRle = CNTRS(Blocks_RLE);
            const uint32_t cntCmp = CNTRS(Blocks_CMP);

            req->zstdRawBlockCount = cntRaw;
            req->zstdRleBlockCount = cntRle;
            req->zstdCmpBlockCount = cntCmp;
            req->zstdRawByteCount  = CNTRS(BlocksBytes_RAW);
            req->zstdRleByteCount  = CNTRS(BlocksBytes_RLE);

            zstdgpu_ResourceInfo_Stage_1_Init(&req->resInfo, cntRaw, cntRle, cntCmp);

        }
        else if (stageIndex == 2)
        {
            const uint32_t cntLit = CNTRS(HUF_Streams_DecodedBytes);
            const uint32_t cntSeq = CNTRS(Seq_Streams_DecodedItems);

            req->zstdUncompressedLiteralsByteCount = cntLit;
            req->zstdUncompressedSequenceCount     = cntSeq;
            req->zstdSeqStreamCount                = CNTRS(Seq_Streams);

            zstdgpu_ResourceInfo_Stage_2_Init(&req->resInfo, cntLit, cntSeq, req->zstdUncompressedFramesByteCount, req->zstdUncompressedFrameCount);
        }
        *outDefaultHeapByteCount            = req->resInfo.gpuOnly_ByteCount[stageIndex];
        *outUploadHeapByteCount             = req->resInfo.cpu2Gpu_ByteCount[stageIndex];
        *outReadbackHeapByteCount           = req->resInfo.gpu2Cpu_ByteCount[stageIndex];
        *outShaderVisibleDescriptorCount    = zstdgpu_Count_SRTs();

        #undef CNTRS
        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

static void zstdgpu_SubmitStage0(zstdgpu_PerRequestContext inPerRequestContext, ID3D12GraphicsCommandList *cmdList);
static void zstdgpu_SubmitStage1(zstdgpu_PerRequestContext inPerRequestContext, ID3D12GraphicsCommandList *cmdList);
static void zstdgpu_SubmitStage2(zstdgpu_PerRequestContext inPerRequestContext, ID3D12GraphicsCommandList *cmdList);

zstdgpu_Status zstdgpu_SubmitWithExternalMemory(zstdgpu_PerRequestContext req,
                                                uint32_t stageIndex,
                                                struct ID3D12GraphicsCommandList *cmdList,
                                                struct ID3D12Heap *defaultHeap,
                                                uint32_t defaultHeapOffsetInBytes,
                                                struct ID3D12Heap *uploadHeap,
                                                uint32_t uploadHeapOffsetInBytes,
                                                struct ID3D12Heap *readbackHeap,
                                                uint32_t readbackHeap_OffsetInBytes,
                                                struct ID3D12DescriptorHeap *shaderVisibleHeap,
                                                uint32_t shaderVisibileHeapOffsetInDescriptors)
{
    uint32_t proceed = 1;
    uint32_t stageCount = zstdgpu_GetStageCountFromSetupInputsType(req->setupInputsType);
    uint32_t defaultHeapMemReq = 0;
    uint32_t uploadHeapMemReq = 0;
    uint32_t readbackHeapMemReq = 0;
    uint32_t shaderVisibleHeapDscCount = 0;
    proceed = kzstdgpu_StatusSuccess == zstdgpu_GetGpuMemoryRequirement(&defaultHeapMemReq, &uploadHeapMemReq, &readbackHeapMemReq, &shaderVisibleHeapDscCount, req, stageIndex);
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
            ZSTDGPU_ASSERT(req->zstdUncompressedLiteralsByteCount == 0 || req->zstdCmpBlockCount > 0);
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
        if (kzstdgpu_SetupInputsType_Frames_GpuMemory == req->setupInputsType && stageIndex == 0u)
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
            if (kzstdgpu_SetupInputsType_Frames_CpuMemory == req->setupInputsType)
            {
                req->uploadCallback(req->resData.cpu2Gpu.CompressedDataCpu, req->zstdCompressedFramesByteCount, req->resData.cpu2Gpu.FramesRefsCpu, req->zstdFrameCount, req->uploadUserdata);
            }
            memcpy(req->resData.cpu2Gpu.FseProbsDefaultCpu, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));
        }

        D3D12AID_SAFE_RELEASE(req->srts.heap);
        req->srts.heap = shaderVisibleHeap;
        req->srts.heap->AddRef();
        req->srts.heapOffset = shaderVisibileHeapOffsetInDescriptors;
        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData);

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

        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
}

zstdgpu_Status zstdgpu_SubmitWithInteralMemory(zstdgpu_PerRequestContext req, uint32_t stageIndex, struct ID3D12GraphicsCommandList *cmdList)
{
    uint32_t proceed = 1;
    uint32_t stageCount = zstdgpu_GetStageCountFromSetupInputsType(req->setupInputsType);

    proceed = proceed && (req->thisMemoryBlock == (void *)req);
    proceed = proceed && (req->zstdFrameCount > 0);
    proceed = proceed && (req->zstdCompressedFramesByteCount > 0);
    proceed = proceed && (req->zstdUncompressedFrameCount == req->zstdFrameCount);
    proceed = proceed && (req->zstdUncompressedFramesByteCount > 0);
    proceed = proceed && (stageIndex < stageCount);
    proceed = proceed && (NULL != cmdList);
    ZSTDGPU_ASSERT(proceed > 0);

    if (proceed)
    {
        #define CNTRS(name) req->resData.gpu2Cpu.CountersCpu[kzstdgpu_CounterIndex_##name]

        // NOTE(pamartis): Recompute memory information for a given stage
        if (stageIndex == 0)
        {
            zstdgpu_ResourceInfo_Stage_0_Init(&req->resInfo, req->zstdFrameCount, req->zstdCompressedFramesByteCount, kzstdgpu_SetupInputsType_Frames_GpuMemory == req->setupInputsType ? 1u : 0u);
        }
        else if (stageIndex == 1)
        {
            const uint32_t cntRaw = CNTRS(Blocks_RAW);
            const uint32_t cntRle = CNTRS(Blocks_RLE);
            const uint32_t cntCmp = CNTRS(Blocks_CMP);

            req->zstdRawBlockCount = cntRaw;
            req->zstdRleBlockCount = cntRle;
            req->zstdCmpBlockCount = cntCmp;
            req->zstdRawByteCount = CNTRS(BlocksBytes_RAW);
            req->zstdRleByteCount = CNTRS(BlocksBytes_RLE);

            zstdgpu_ResourceInfo_Stage_1_Init(&req->resInfo, cntRaw, cntRle, cntCmp);
        }
        else if (stageIndex == 2)
        {
            const uint32_t cntLit = CNTRS(HUF_Streams_DecodedBytes);
            const uint32_t cntSeq = CNTRS(Seq_Streams_DecodedItems);

            req->zstdUncompressedLiteralsByteCount = cntLit;
            req->zstdUncompressedSequenceCount     = cntSeq;
            req->zstdSeqStreamCount                = CNTRS(Seq_Streams);

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
        if (kzstdgpu_SetupInputsType_Frames_GpuMemory == req->setupInputsType && stageIndex == 0u)
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
            if (kzstdgpu_SetupInputsType_Frames_CpuMemory == req->setupInputsType)
            {
                req->uploadCallback(req->resData.cpu2Gpu.CompressedDataCpu, req->zstdCompressedFramesByteCount, req->resData.cpu2Gpu.FramesRefsCpu, req->zstdFrameCount, req->uploadUserdata);
            }
            memcpy(req->resData.cpu2Gpu.FseProbsDefaultCpu, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));
        }

        if (NULL == req->srts.heap)
        {
            req->srts.heap = d3d12aid_DescriptorHeap_Create(req->device, zstdgpu_Count_SRTs(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
            req->srts.heapOffset = 0;
        }
        #undef CNTRS
        zstdgpu_ReCreate_SRTs(req->srts, req->device, req->resInfo, req->resData);

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

        return kzstdgpu_StatusSuccess;
    }
    return kzstdgpu_StatusInvalidArgument;
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

#if 0
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
        req->name##_TimestampSlot = d3d12aid_Timestamps_Push(&req->timestamps, cmdList);\
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

void zstdgpu_SubmitStage0(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    {
        D3D12_RESOURCE_BARRIER barriers[3];
        uint32_t uploadBarrierCount = 0;

        #define zstdgpu_PushUpload(name) cmdList->CopyResource(req->resData.gpuOnly.name, req->resData.cpu2Gpu.name)

        if (kzstdgpu_SetupInputsType_Frames_CpuMemory == req->setupInputsType)
        {
            zstdgpu_PushUpload(CompressedData);
            zstdgpu_PushUpload(FramesRefs);
        }
        zstdgpu_PushUpload(FseProbsDefault);
        #undef zstdgpu_PushUpload

        setResourceState(barriers, 0, req->resData.gpuOnly.FseProbsDefault, COPY_DEST, NON_PIXEL_SHADER_RESOURCE);
        uploadBarrierCount += 1;
        if (kzstdgpu_SetupInputsType_Frames_CpuMemory == req->setupInputsType)
        {
            setResourceState(barriers, 1, req->resData.gpuOnly.CompressedData, COPY_DEST, NON_PIXEL_SHADER_RESOURCE);
            setResourceState(barriers, 2, req->resData.gpuOnly.FramesRefs, COPY_DEST, NON_PIXEL_SHADER_RESOURCE);
            uploadBarrierCount += 2;
        }
        cmdList->ResourceBarrier(uploadBarrierCount, barriers);
    }
    {
        const uint32_t initResourcesStage = 0;
        const uint32_t allBlockCount = 0;
        const uint32_t cmpBlockCount = 0;

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Init Resources :: Stage 0]");
        BIND_RS_PS_SRT(InitResources);
        cmdList->SetComputeRoot32BitConstant(1, allBlockCount, 0);
        cmdList->SetComputeRoot32BitConstant(1, cmpBlockCount, 1);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 2);
        cmdList->SetComputeRoot32BitConstant(1, initResourcesStage, 3);
        ZSTDGPU_KERNEL_SCOPE(InitResources_CountBlocks, cmdList,
            cmdList->Dispatch(zstdgpu_InitResources_GetDispatchSizeX(allBlockCount, cmpBlockCount, req->zstdFrameCount, initResourcesStage), 1, 1);
        );

        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Parse Frames :: Count Blocks]");
        D3D12_RESOURCE_BARRIER barriers[7];
        // last written by [Init Resources :: Stage 0]
        // next written/atomically updated by [Parse Frames :: Count Blocks]
        setResourceUavSync(barriers, 0, req->resData.gpuOnly.Counters);
        setResourceUavSync(barriers, 1, req->resData.gpuOnly.PerFrameBlockCountRAW);
        setResourceUavSync(barriers, 2, req->resData.gpuOnly.PerFrameBlockCountRLE);
        setResourceUavSync(barriers, 3, req->resData.gpuOnly.PerFrameBlockCountCMP);
        setResourceUavSync(barriers, 4, req->resData.gpuOnly.PerFrameBlockCountAll);
        setResourceUavSync(barriers, 5, req->resData.gpuOnly.PerFrameBlockSizesRAW);
        setResourceUavSync(barriers, 6, req->resData.gpuOnly.PerFrameBlockSizesRLE);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    {
        const uint32_t countBlocksOnly = 1;
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Parse Frames :: Count Blocks]");
        BIND_RS_PS_SRT(ParseFrames);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 0);
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 1);
        cmdList->SetComputeRoot32BitConstant(1, countBlocksOnly, 2);
        ZSTDGPU_KERNEL_SCOPE(ParseFrames_CountBlocks, cmdList,
            cmdList->Dispatch(ZSTDGPU_TG_COUNT(req->zstdFrameCount, kzstdgpu_TgSizeX_ParseCompressedBlocks), 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    {
        // Resources needed by for Parse Frames
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [PrefixSum :: Block Counts]");
        D3D12_RESOURCE_BARRIER barriers[6];
        // next written/atomically updated by [Parse Frames :: Count Blocks]
        // next written by [PrefixSum :: Block Counts] to store prefix sum instead of counts
        setResourceUavSync(barriers, 0, req->resData.gpuOnly.PerFrameBlockCountRAW);
        setResourceUavSync(barriers, 1, req->resData.gpuOnly.PerFrameBlockCountRLE);
        setResourceUavSync(barriers, 2, req->resData.gpuOnly.PerFrameBlockCountCMP);
        setResourceUavSync(barriers, 3, req->resData.gpuOnly.PerFrameBlockCountAll);
        setResourceUavSync(barriers, 4, req->resData.gpuOnly.PerFrameBlockSizesRAW);
        setResourceUavSync(barriers, 5, req->resData.gpuOnly.PerFrameBlockSizesRLE);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    {
        const uint32_t tgCountX = ZSTDGPU_TG_COUNT(req->zstdFrameCount, kzstdgpu_TgSizeX_PrefixSum);

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[PrefixSum :: Block Counts]");
        d3d12aid_ComputeRsPs_Set(&req->PrefixSum, cmdList);
        cmdList->SetComputeRoot32BitConstant(2, req->zstdFrameCount, 0);
        cmdList->SetComputeRoot32BitConstant(2, 0 /** outputInclusive */, 1);

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
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Readback Counters :: After Block Count] and [Parse Compressed Blocks] and [Prefix Sequence Offsets] and [Finalise Sequence Offsets]");
        D3D12_RESOURCE_BARRIER barriers[3];
        // last written by [Parse Frames :: Count Blocks]
        // next read by [Readback Counters :: After Block Count]
        setResourceState(barriers, 0, req->resData.gpuOnly.Counters, UNORDERED_ACCESS, COPY_SOURCE);
        // last written by [PrefixSum :: Block Counts]
        // next read by [Parse Compressed Blocks]
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.PerFrameBlockCountCMP);
        // last written by [PrefixSum :: Block Counts]
        // next read by [Prefix Sequence Offsets] and [Finalise Sequence Offsets]
        setResourceUavToSrvSync(barriers, 2, req->resData.gpuOnly.PerFrameBlockCountAll);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Readback Counters :: After Block Count]");
        zstdgpu_PushReadback(Counters);
        PIXEndEvent(cmdList);
    }
}
void zstdgpu_SubmitStage1(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    {
        const uint32_t initResourcesStage = 1;
        const uint32_t allBlockCount = req->zstdRawBlockCount
                                     + req->zstdRleBlockCount
                                     + req->zstdCmpBlockCount;

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Init Resources :: Stage 1]");
        BIND_RS_PS_SRT(InitResources);
        cmdList->SetComputeRoot32BitConstant(1, allBlockCount, 0);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount, 1);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 2);
        cmdList->SetComputeRoot32BitConstant(1, initResourcesStage, 3);
        ZSTDGPU_KERNEL_SCOPE(InitResources, cmdList,
            cmdList->Dispatch(zstdgpu_InitResources_GetDispatchSizeX(allBlockCount, req->zstdCmpBlockCount, req->zstdFrameCount, initResourcesStage), 1, 1);
        );

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
        BIND_RS_PS_SRT(ParseFrames);
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
        D3D12_RESOURCE_BARRIER barriers[14];

        uint32_t bc = 0;
        if (req->zstdCmpBlockCount > 0)
        {
            // last written by [Parse Frames :: Collect Blocks]
            // next written/updated by [Parse Compressed Blocks] with non-intersecting memory ranges with [Parse Frames :: Collect Blocks]
            // so TODO: check if can remove
            setResourceUavSync(barriers, bc + 0, req->resData.gpuOnly.Counters);
            // last written by [Init Resources :: Stage 1]
            // next written/updated by [Parse Compressed Blocks] with non-intersecting memory range with [Init Resources :: Stage 1]
            // so TODO: check if can remove
            setResourceUavSync(barriers, bc + 1, req->resData.gpuOnly.FseInfos);
            setResourceUavSync(barriers, bc + 2, req->resData.gpuOnly.FseProbs);
            // last written by [Init Resources :: Stage 1] -- only "lookback" sub-buffer
            // next written/updated by [Parse Compressed Blocks] -- both "lookback" and "prefix" sub-buffers
            setResourceUavSync(barriers, bc + 3, req->resData.gpuOnly.TableIndexLookback);
            // last written by [Init Resources :: Stage 1]
            // next written/updated by [Parse Compressed Blocks]
            setResourceUavSync(barriers, bc + 4, req->resData.gpuOnly.LitStreamEndPerHuffmanTable);
            setResourceUavSync(barriers, bc + 5, req->resData.gpuOnly.PerFrameSeqStreamMinIdx);
            // last written by [Parse Frames :: Collect Blocks]
            // next read by [Parse Compressed Blocks]
            setResourceUavToSrvSync(barriers, bc + 6, req->resData.gpuOnly.BlocksCMPRefs);
            // last written by [Init Resources :: Stage 1]
            // next written/updated by [Parse Compressed Blocks]
            setResourceUavSync(barriers, bc + 7, req->resData.gpuOnly.SeqCountPrefixLookback);
            setResourceUavSync(barriers, bc + 8, req->resData.gpuOnly.BlockSeqCountPrefixLookback);
            // last written by [Parse Frames :: Collect Blocks]
            // next read by [Parse Compressed Blocks] -- to be able to get global index to index into BlockSizePrefix
            setResourceUavToSrvSync(barriers, bc + 9, req->resData.gpuOnly.GlobalBlockIndexPerCmpBlock);
            // last written by [Parse Frames :: Collect Blocks]
            // next written/updated by [Parse Compressed Blocks] - sets literal size to be uncompressed block size
            setResourceUavSync(barriers, bc + 10, req->resData.gpuOnly.BlockSizePrefix);
            //
            // [DUMMY] Because PerFrameBlockCountCMP was bound as UAV to InitResources (but wasn't actually written), COMMON state promotion sets its state
            // to UAV, and later read in ParseCompressedBlocks, so we set Read state to make sure VALIDATION LAYER doesn't complain
            setResourceUavToSrvSync(barriers, bc + 11, req->resData.gpuOnly.PerFrameBlockCountCMP);
            bc += 12;
        }
        else
        {
            // last written by [Parse Frames :: Collect Blocks]
            // next updated by [Prefix Block Sizes]
            setResourceUavSync(barriers, bc + 0, req->resData.gpuOnly.BlockSizePrefix);
            bc += 1;
        }

        // last written by [Parse Frames :: Collect Blocks]
        // next read by [Memcpy RAW blocks, Memset RLE blocks]
        if (req->zstdRawBlockCount > 0)
        {
            setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.BlocksRAWRefs);
            bc += 1;
        }

        if (req->zstdRleByteCount > 0)
        {
            setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.BlocksRLERefs);
            bc += 1;
        }

        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Parse Compressed Blocks]");
        BIND_RS_PS_SRT(ParseCompressedBlocks);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount, 0);
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 1);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 2);

        ZSTDGPU_KERNEL_SCOPE(ParseCompressedBlocks, cmdList,
            cmdList->Dispatch(ZSTDGPU_TG_COUNT(req->zstdCmpBlockCount, kzstdgpu_TgSizeX_ParseCompressedBlocks), 1, 1);
        );
        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Readback Counters :: After Block Parse] and [Update Dispatch Args] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]");
        D3D12_RESOURCE_BARRIER barriers[9];
        // last written by [Parse Compressed Blocks]
        // next read by [Readback Counters :: After Block Parse] and updated by [Update Dispatch Args]
        setResourceState(barriers, 0, req->resData.gpuOnly.Counters, UNORDERED_ACCESS, COPY_SOURCE);
        // last written by [Parse Compressed Blocks]
        // next written/updated by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        setResourceUavSync(barriers, 1, req->resData.gpuOnly.LitStreamEndPerHuffmanTable);
        setResourceUavSync(barriers, 2, req->resData.gpuOnly.LitGroupEndPerHuffmanTable);
        // last written by [Parse Compressed Blocks]
        // next written/updated by [Decompress Sequences]
        setResourceUavSync(barriers, 3, req->resData.gpuOnly.BlockSizePrefix);
        // last written by [Parse Compressed Blocks]
        // next read by [Prefix Sequence Offsets]
        setResourceUavToSrvSync(barriers, 4, req->resData.gpuOnly.PerFrameSeqStreamMinIdx);

        // last written by [Init Resources :: Stage 1] with zero values to lookback data
        // next written by [Decompress Sequences] with encoded "final" offsets per block
        setResourceUavSync(barriers, 5, req->resData.gpuOnly.PerSeqStreamFinalOffset1);
        setResourceUavSync(barriers, 6, req->resData.gpuOnly.PerSeqStreamFinalOffset2);
        setResourceUavSync(barriers, 7, req->resData.gpuOnly.PerSeqStreamFinalOffset3);
        // last written by [Parse Compressed Blocks]
        // next read by [Finalise Sequence Offsets]
        setResourceUavToSrvSync(barriers, 8, req->resData.gpuOnly.PerSeqStreamSeqStart);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Readback Counters :: After Block Parse");
        zstdgpu_PushReadback(Counters);
        PIXEndEvent(cmdList);
    }
}
void zstdgpu_SubmitStage2(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Update Dispatch Args]");
        d3d12aid_ComputeRsPs_Set(&req->UpdateDispatchArgs, cmdList);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        ZSTDGPU_KERNEL_SCOPE(UpdateDispatchArgs, cmdList,
            cmdList->Dispatch(1, 1, 1);
        );
        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Compute `Per-Huffman Table` Literal Stream Count Prefix]");

        d3d12aid_ComputeRsPs_Set(&req->ComputePrefixSum, cmdList);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.LitStreamEndPerHuffmanTable->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.LitGroupEndPerHuffmanTable->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.LitStreamEndPerHuffmanTable->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.LitGroupEndPerHuffmanTable->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        cmdList->SetComputeRootUnorderedAccessView(4, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        cmdList->SetComputeRoot32BitConstant(5, req->zstdCmpBlockCount, 0);
        ZSTDGPU_KERNEL_SCOPE(ComputePrefixSum, cmdList,
            cmdList->Dispatch(ZSTDGPU_TG_COUNT(req->zstdCmpBlockCount, kzstdgpu_TgSizeX_PrefixSum_LiteralCount), 1, 1);
        );
        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Init FSE Table] and [Group Lilteral Streams]");
        D3D12_RESOURCE_BARRIER barriers[12];
        // last written by [Update Dispatch Args] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Group Lilteral Streams] and [Init FSE Table]
        setResourceUavToSrvCopyIndirectSync(barriers, 0, req->resData.gpuOnly.Counters);
        // last written by [Parse Compressed Blocks]
        // next read by [Init FSE Table]
        // CAN MOVE EARLIER
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.FseProbs);
        setResourceUavToSrvSync(barriers, 2, req->resData.gpuOnly.FseInfos);
        // last written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Group Lilteral Streams]
        setResourceUavToSrvSync(barriers, 3, req->resData.gpuOnly.LitStreamEndPerHuffmanTable);
        // last written by [Parse Compressed Blocks]
        // next read by [Group Lilteral Streams]
        setResourceUavToSrvSync(barriers, 4, req->resData.gpuOnly.LitStreamBuckets);
        // last written by [Parse Compressed Blocks]
        // last read by [DEBUG READBACK]
        setResourceUavToSrvSync(barriers, 5, req->resData.gpuOnly.CompressedBlocks);
        // last written by [Parse Compressed Blocks]
        // next read by [Decompress Huffman Weights] and [Decode Uncompressed Huffman Weights] and [DEBUG READBACK]
        setResourceUavToSrvSync(barriers, 6, req->resData.gpuOnly.HufRefs);
        // last written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, 7, req->resData.gpuOnly.LitGroupEndPerHuffmanTable);
        //
        setResourceUavToSrvSync(barriers, 8, req->resData.gpuOnly.TableIndexLookback);
        // last written by [Parse Compressed Blocks]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, 9, req->resData.gpuOnly.LitRefs);
        // last written by [Parse Compressed Blocks]
        // next read by [Decompress Sequences]
        setResourceUavToSrvSync(barriers, 10, req->resData.gpuOnly.SeqRefs);
        // last written by [Parse Compressed Blocks]
        // next written by [Decompress Huffman Weights] and read as UAV by [Decode Uncompressed Huffman Weights]
        setResourceUavSync(barriers, 11, req->resData.gpuOnly.DecompressedHuffmanWeightCount);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    // Groups Huffman-compressed literal streams by Huffman table index
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Group Lilteral Streams]");
        d3d12aid_ComputeRsPs_Set(&req->GroupCompressedLiterals, cmdList);
        cmdList->SetComputeRootShaderResourceView(0, req->resData.gpuOnly.LitStreamEndPerHuffmanTable->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(1, req->resData.gpuOnly.LitStreamBuckets->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(2, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.LitStreamRemap->GetGPUVirtualAddress());
        ID3D12Resource* argBuf = req->resData.gpuOnly.Counters;

        ZSTDGPU_KERNEL_SCOPE(GroupCompressedLiterals, cmdList,
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_GroupCompressedLiteralsGroups * sizeof(uint32_t), NULL, 0);
        );

        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        // Run FSE Table Initialisation
        ZSTDGPU_KERNEL_SCOPE(InitFseTable, cmdList,
        {
            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Init FSE Table]");
            BIND_RS_PS_SRT(InitFseTable);

            // NOTE: we run 4 ExecuteIndirects (per argument) in order to be able to (but we don't do this for prototype)
            // switch PSO to more optimial (depending on maximal FSE table size) because D3D12 doesn't allow to switch PSOs in ExecuteIndirect.
            ID3D12Resource* argBuf = req->resData.gpuOnly.Counters;

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Huffman Weights");
            cmdList->SetComputeRoot32BitConstant(1, 0, 0);
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_FseHufW * sizeof(uint32_t), NULL, 0);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Literal Lengths");
            cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount, 0);
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_FseLLen * sizeof(uint32_t), NULL, 0);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Offsets");
            cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount * 2 + 1, 0);
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_FseOffs * sizeof(uint32_t), NULL, 0);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Match Lengths");
            cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount * 3 + 2, 0);
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_FseMLen * sizeof(uint32_t), NULL, 0);
            PIXEndEvent(cmdList);
            PIXEndEvent(cmdList);
        });
    }

    // Needed by readback
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Huffman Weights Decompression] and [Decompress Literals]");
        D3D12_RESOURCE_BARRIER barriers[4];
        // last written by [Init FSE Table]
        // next read by [Decompress Huffman Weights] and [Decompress Sequences]
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.FseSymbols);
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.FseNStates);
        setResourceUavToSrvSync(barriers, 2, req->resData.gpuOnly.FseBitcnts);
        // last written by [Group Lilteral Streams]
        // next read [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, 3, req->resData.gpuOnly.LitStreamRemap);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    // Run Decompression of FSE-compressed Huffman Weights
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decompress Huffman Weights]");
        BIND_RS_PS_SRT(DecompressHuffmanWeights);

        ID3D12Resource* argBuf = req->resData.gpuOnly.Counters;
        ZSTDGPU_KERNEL_SCOPE(DecompressHuffmanWeights, cmdList,
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_DecompressHuffmanWeightsGroups * sizeof(uint32_t), NULL, 0);
        );
        PIXEndEvent(cmdList);
    }
    // Run Decoding of Uncompressed Huffman Weights (can merge with FSE decompression before barrier)
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decode Uncompressed Huffman Weights]");
        BIND_RS_PS_SRT(DecodeHuffmanWeights);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount, 0);
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 1);

        ID3D12Resource* argBuf = req->resData.gpuOnly.Counters;
        ZSTDGPU_KERNEL_SCOPE(DecodeHuffmanWeights, cmdList,
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_DecodeHuffmanWeightsGroups * sizeof(uint32_t), NULL, 0);
        );
        PIXEndEvent(cmdList);
    }

    // Needed by Initialisation of Huffman Tables
    if (req->zstdCmpBlockCount > 0)
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

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Init Huffman Table and Decompress Literals]");
        BIND_RS_PS_SRT(InitHuffmanTableAndDecompressLiterals);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount, 0);

        ID3D12Resource* argBuf = req->resData.gpuOnly.Counters;
        ZSTDGPU_KERNEL_SCOPE(InitHuffmanTableAndDecompressLiterals, cmdList,
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_DecompressLiteralsGroups * sizeof(uint32_t), NULL, 0);
        );
        PIXEndEvent(cmdList);
    }

    // NOTE(pamartis): (can run in parallel with FSE-compressed Huffman Weight Decompression, right after FSE table initialisation)
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decompress Sequences]");
        BIND_RS_PS_SRT(DecompressSequences);

        ID3D12Resource* argBuf = req->resData.gpuOnly.Counters;
        ZSTDGPU_KERNEL_SCOPE(DecompressSequences, cmdList,
            cmdList->ExecuteIndirect(req->dispatchCmdSig, 1, argBuf, kzstdgpu_CounterIndex_DecompressSequencesGroups * sizeof(uint32_t), NULL, 0);
        );
        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
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
        if (req->zstdUncompressedLiteralsByteCount > 0)
        {
            setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.DecompressedLiterals);
            bc += 1;
        }
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }
    {
        const uint32_t allBlockCount = req->zstdRawBlockCount
                                     + req->zstdRleBlockCount
                                     + req->zstdCmpBlockCount;

        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Prefix Block Sizes]");
        d3d12aid_ComputeRsPs_Set(&req->PrefixSum, cmdList);

        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.BlockSizePrefix->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.BlockSizePrefix->GetGPUVirtualAddress() + allBlockCount * sizeof(uint32_t));
        cmdList->SetComputeRoot32BitConstant(2, allBlockCount, 0);
        cmdList->SetComputeRoot32BitConstant(2, 1 /** outputInclusive */, 1);

        ZSTDGPU_KERNEL_SCOPE(PrefixBlockSizes, cmdList,
            cmdList->Dispatch(ZSTDGPU_TG_COUNT(allBlockCount, kzstdgpu_TgSizeX_PrefixSum), 1, 1);
        );

        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Prefix Sequence Offsets]");
        d3d12aid_ComputeRsPs_Set(&req->PrefixSequenceOffsets, cmdList);

        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset1->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.PerSeqStreamFinalOffset2->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.PerSeqStreamFinalOffset3->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(3, req->resData.gpuOnly.PerSeqStreamFinalOffset1->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        cmdList->SetComputeRootUnorderedAccessView(4, req->resData.gpuOnly.PerSeqStreamFinalOffset2->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        cmdList->SetComputeRootUnorderedAccessView(5, req->resData.gpuOnly.PerSeqStreamFinalOffset3->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        cmdList->SetComputeRootShaderResourceView(6, req->resData.gpuOnly.PerFrameSeqStreamMinIdx->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(7, req->resData.gpuOnly.PerFrameBlockCountAll->GetGPUVirtualAddress());
        cmdList->SetComputeRootShaderResourceView(8, req->resData.gpuOnly.SeqRefs->GetGPUVirtualAddress());
        cmdList->SetComputeRoot32BitConstant(9, req->zstdSeqStreamCount, 0);
        cmdList->SetComputeRoot32BitConstant(9, req->zstdFrameCount, 1);

        ZSTDGPU_KERNEL_SCOPE(PrefixSequenceOffsets, cmdList,
            cmdList->Dispatch(ZSTDGPU_TG_COUNT(req->zstdSeqStreamCount, kzstdgpu_TgSizeX_PrefixSequenceOffsets), 1, 1);
        );

        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Finalise Sequence Offsets]");
        D3D12_RESOURCE_BARRIER barriers[4];
        uint32_t bc = 0;
        if (req->zstdCmpBlockCount > 0)
        {
            // last written by [Prefix Sequence Offsets]
            // next read by [Finalise Sequence Offsets]
            setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.PerSeqStreamFinalOffset1);
            setResourceUavToSrvSync(barriers, bc + 1, req->resData.gpuOnly.PerSeqStreamFinalOffset2);
            setResourceUavToSrvSync(barriers, bc + 2, req->resData.gpuOnly.PerSeqStreamFinalOffset3);
            bc += 3;
        }

        // last written by [Prefix Block Sizes]
        // next read by [Memcpy RAW blocks, Memset RLE blocks] and [Execute Sequences]
        setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.BlockSizePrefix);
        bc += 1;
        cmdList->ResourceBarrier(bc, barriers);
        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Finalise Sequence Offsets]");
        BIND_RS_PS_SRT(FinaliseSequenceOffsets);

        const uint32_t tgCount = ZSTDGPU_TG_COUNT(req->zstdUncompressedSequenceCount, kzstdgpu_TgSizeX_FinaliseSequenceOffsets);
        ZSTDGPU_KERNEL_SCOPE(FinaliseSequenceOffsets, cmdList,
            zstdgpu_Dispatch32Bit(cmdList, tgCount, 1, 0);
        );

        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Memcpy RAW blocks, Memset RLE blocks] and [Execute Sequences]");
        D3D12_RESOURCE_BARRIER barriers[1];
        // last written/updated by [Finalise Sequence Offsets]
        // next read by [Execute Sequences]
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.DecompressedSequenceOffs);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    if (req->zstdRawByteCount > 0 || req->zstdRleByteCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Memcpy RAW blocks, Memset RLE blocks]");
        d3d12aid_ComputeRsPs_Set(&req->MemsetMemcpy, cmdList);
        cmdList->SetDescriptorHeaps(1, &req->srts.heap);
        cmdList->SetComputeRootDescriptorTable(0, req->srts.MemsetMemcpyGpuHandle);
        ZSTDGPU_KERNEL_SCOPE(MemcpyRAW_MemsetRLE, cmdList,
        {
            if (req->zstdRawByteCount > 0)
            {
                cmdList->SetComputeRootShaderResourceView(1, req->resData.gpuOnly.RawBlockSizePrefix->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(2, req->resData.gpuOnly.PerFrameBlockSizesRAW->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(3, req->resData.gpuOnly.BlocksRAWRefs->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(4, req->resData.gpuOnly.GlobalBlockIndexPerRawBlock->GetGPUVirtualAddress());
                cmdList->SetComputeRoot32BitConstant(5, req->zstdRawByteCount, 0);
                cmdList->SetComputeRoot32BitConstant(5, req->zstdRawBlockCount, 1);
                cmdList->SetComputeRoot32BitConstant(5, req->zstdFrameCount, 2);
                cmdList->SetComputeRoot32BitConstant(5, 1 /* flags */, 3);
                zstdgpu_Dispatch32Bit(cmdList, ZSTDGPU_TG_COUNT(req->zstdRawByteCount, kzstdgpu_TgSizeX_MemsetMemcpy), 5, 4);
            }
            if (req->zstdRleByteCount > 0)
            {
                cmdList->SetComputeRootShaderResourceView(1, req->resData.gpuOnly.RleBlockSizePrefix->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(2, req->resData.gpuOnly.PerFrameBlockSizesRLE->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(3, req->resData.gpuOnly.BlocksRLERefs->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(4, req->resData.gpuOnly.GlobalBlockIndexPerRleBlock->GetGPUVirtualAddress());
                cmdList->SetComputeRoot32BitConstant(5, req->zstdRleByteCount, 0);
                cmdList->SetComputeRoot32BitConstant(5, req->zstdRleBlockCount, 1);
                cmdList->SetComputeRoot32BitConstant(5, req->zstdFrameCount, 2);
                cmdList->SetComputeRoot32BitConstant(5, 0 /* flags */, 3);
                zstdgpu_Dispatch32Bit(cmdList, ZSTDGPU_TG_COUNT(req->zstdRleByteCount, kzstdgpu_TgSizeX_MemsetMemcpy), 5, 4);
            }
        });
        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Execute Sequences]");
        BIND_RS_PS_SRT(ExecuteSequences);

        ZSTDGPU_KERNEL_SCOPE(ExecuteSequences, cmdList,
            cmdList->Dispatch(req->zstdFrameCount, 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    if (0) /** IMPORTANT: requires DecompressedSequencesMLen to contain inclusive prefix of total sequence sizes */
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Compute Dest Sequence Offsets]");
        BIND_RS_PS_SRT(ComputeDestSequenceOffsets);

        zstdgpu_Dispatch32Bit(cmdList, ZSTDGPU_TG_COUNT(req->zstdUncompressedSequenceCount, 256), 1, 0);

        PIXEndEvent(cmdList);
    }
}

ZSTDGPU_API void zstdgpu_ReadbackGpuResults(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    // NOTE(pamartis): these are required to make sure D3D12 validation doesn't complain about state mismatch when
    // Read-only resource from the last stage get a NON_PS_RESOURCE state as a result of promotion from COMMON state
    // and then used as COPY_SOURCE for debug readback
    D3D12_RESOURCE_BARRIER barriers[13];
    uint32_t bc = 0;
    setResourceState(barriers, 0, req->resData.gpuOnly.PerFrameBlockCountCMP, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
    setResourceState(barriers, 1, req->resData.gpuOnly.PerFrameBlockCountAll, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
    setResourceState(barriers, 2, req->resData.gpuOnly.PerFrameBlockSizesRAW, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
    setResourceState(barriers, 3, req->resData.gpuOnly.PerFrameBlockSizesRLE, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
    setResourceState(barriers, 4, req->resData.gpuOnly.PerFrameSeqStreamMinIdx, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
    bc += 5;
    if (req->zstdCmpBlockCount > 0)
    {
        setResourceState(barriers, bc + 0, req->resData.gpuOnly.GlobalBlockIndexPerCmpBlock, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, bc + 1, req->resData.gpuOnly.PerSeqStreamSeqStart, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        bc += 2;
    }
    if (req->zstdRawBlockCount > 0)
    {
        setResourceState(barriers, bc + 0, req->resData.gpuOnly.GlobalBlockIndexPerRawBlock, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, bc + 1, req->resData.gpuOnly.RawBlockSizePrefix, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, bc + 2, req->resData.gpuOnly.BlocksRAWRefs, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        bc += 3;
    }
    if (req->zstdRleBlockCount > 0)
    {
        setResourceState(barriers, bc + 0, req->resData.gpuOnly.GlobalBlockIndexPerRleBlock, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, bc + 1, req->resData.gpuOnly.RleBlockSizePrefix, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        setResourceState(barriers, bc + 2, req->resData.gpuOnly.BlocksRLERefs, NON_PIXEL_SHADER_RESOURCE, COPY_SOURCE);
        bc += 3;
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

    // HACK (pamartis): we copy block counts for now, because validation needs it, but we never compute block count on GPU
    outGpuResources->Counters[kzstdgpu_CounterIndex_Blocks_RAW] = req->zstdRawBlockCount;
    outGpuResources->Counters[kzstdgpu_CounterIndex_Blocks_RLE] = req->zstdRleBlockCount;
    outGpuResources->Counters[kzstdgpu_CounterIndex_Blocks_CMP] = req->zstdCmpBlockCount;
}

ZSTDGPU_API void zstdgpu_ReadbackTimestamps(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    d3d12aid_Timestamps_AdvanceFrame(&req->timestamps, cmdList);
}

ZSTDGPU_API void zstdgpu_RetrieveTimestamps(const wchar_t **outTimestampScopeNames, uint64_t *outTimestampScopeClocks, uint32_t *inoutTimestampScopeCnt, zstdgpu_PerRequestContext req, uint32_t stageIndex)
{
    uint32_t timestampScopeCountPerStage = 0;
    #define ZSTDGPU_KERNEL_SCOPE_X(name, desc) +1
    if (stageIndex == 0)
    {
        timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0();
    }
    else if (stageIndex == 1)
    {
        timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1();
    }
    else if (stageIndex == 2)
    {
        timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_ALL_BLOCKS();
        if (req->zstdCmpBlockCount > 0)
        {
            timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_CMP_BLOCKS();
        }
        if (req->zstdRawBlockCount > 0 || req->zstdRleBlockCount > 0)
        {
            timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_RAW_RLE_BLOCKS();
        }
    }
    #undef  ZSTDGPU_KERNEL_SCOPE_X

    if (timestampScopeCountPerStage > 0 && *inoutTimestampScopeCnt >= timestampScopeCountPerStage)
    {
        *inoutTimestampScopeCnt = timestampScopeCountPerStage;
        uint32_t i = 0;
        #define ZSTDGPU_KERNEL_SCOPE_X(name, desc)  \
            outTimestampScopeNames[i] = desc;       \
            outTimestampScopeClocks[i++] = d3d12aid_Timestamps_GetScopeDelta(&req->timestamps, 0, req->name##_TimestampSlot);
        if (stageIndex == 0)
        {
            ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0()
        }
        else if (stageIndex == 1)
        {
            ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1()
        }
        else if (stageIndex == 2)
        {
            ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_ALL_BLOCKS()
            if (req->zstdCmpBlockCount > 0)
            {
                ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_CMP_BLOCKS();
            }
            if (req->zstdRawBlockCount > 0 || req->zstdRleBlockCount > 0)
            {
                ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_RAW_RLE_BLOCKS();
            }
        }
        #undef  ZSTDGPU_KERNEL_SCOPE_X
    }
    else
    {
        *inoutTimestampScopeCnt = 0;
    }
}
