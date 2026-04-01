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

#define ZSTDGPU_ENABLE_TIMESTAMPS 0

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
#include "ZstdGpuDecompressLiterals.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache128_8.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache64_16.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache64_8.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache32_32.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache32_16.h"
#include "ZstdGpuDecompressLiterals_LdsStoreCache32_8.h"
#include "ZstdGpuDecompressSequences_MultiStream_4.h"
#include "ZstdGpuDecompressSequences_MultiStream_8.h"
#include "ZstdGpuDecompressSequences_MultiStream_4_LdsOutCache_128.h"
#include "ZstdGpuDecompressSequences_MultiStream_4_LdsOutCache_64.h"
#include "ZstdGpuDecompressSequences_MultiStream_8_LdsOutCache_64.h"
#include "ZstdGpuDecompressSequences_MultiStream_8_LdsOutCache_32.h"
#include "ZstdGpuDecompressSequences_MultiStream_16_LdsOutCache_32.h"
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
#include "ZstdGpuGroupCompressedLiterals.h"
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
        ZSTDGPU_SRT_LIST()
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

static void zstdgpu_ReCreate_SRTs(zstdgpu_SRTs & srts, ID3D12Device *device, const zstdgpu_ResourceInfo & resInfo, const zstdgpu_ResourceDataGpu & gpuResData)
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
        ZSTDGPU_SRT_LIST()
    #undef  ZSTDGPU_SRT

    #include "zstdgpu_srt_decl_undef.h"
}

#define ZSTDGPU_KERNEL_LIST()                                                                                                           \
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
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_4_LdsOutCache_128,   L"Decompress Sequences (Multi-Stream, Streams= 4, LDS Out Cache=128 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_4_LdsOutCache_64 ,   L"Decompress Sequences (Multi-Stream, Streams= 4, LDS Out Cache= 64 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_8_LdsOutCache_64 ,   L"Decompress Sequences (Multi-Stream, Streams= 8, LDS Out Cache= 64 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_8_LdsOutCache_32 ,   L"Decompress Sequences (Multi-Stream, Streams= 8, LDS Out Cache= 32 Sequences)")    \
    ZSTDGPU_KERNEL(DecompressSequences_MultiStream_16_LdsOutCache_32,   L"Decompress Sequences (Multi-Stream, Streams=16, LDS Out Cache= 32 Sequences)")    \
    ZSTDGPU_KERNEL(ExecuteSequences128                              ,   L"Execute Sequences 128")                                               \
    ZSTDGPU_KERNEL(ExecuteSequences64                               ,   L"Execute Sequences 64")                                                \
    ZSTDGPU_KERNEL(ExecuteSequences32                               ,   L"Execute Sequences 32")                                                \
    ZSTDGPU_KERNEL(FinaliseSequenceOffsets                          ,   L"Finalise Sequence Offsets")                                           \
    ZSTDGPU_KERNEL(GroupCompressedLiterals                          ,   L"Group Huffman-compressed Literals")                                   \
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
    ZSTDGPU_DISPATCH32_CMD_SIG(DecodeHuffmanWeights     , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(DecompressHuffmanWeights , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(DecompressLiterals       , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(DecompressSequences      , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(FinaliseSequenceOffsets  , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(GroupCompressedLiterals  , 4)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(InitFseTable             , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(InitHuffmanTable         , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(Memset                   , 1)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(MemsetMemcpy             , 5)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(PrefixSequenceOffsets    , 10)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(PrefixSum                , 2)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(ComputePrefixSum         , 5)    \
    ZSTDGPU_DISPATCH32_CMD_SIG(ParseCompressedBlocks    , 1)

#define ZSTDGPU_RUNTIME_KERNEL_LIST_SHARED()        \
    ZSTDGPU_KERNEL(ComputeDestSequenceOffsets)      \
    ZSTDGPU_KERNEL(ComputePrefixSum)                \
    ZSTDGPU_KERNEL(DecodeHuffmanWeights)            \
    ZSTDGPU_KERNEL(DecompressHuffmanWeights)        \
    ZSTDGPU_KERNEL(FinaliseSequenceOffsets)         \
    ZSTDGPU_KERNEL(GroupCompressedLiterals)         \
    ZSTDGPU_KERNEL(InitFseTable)                    \
    ZSTDGPU_KERNEL(InitHuffmanTable)                \
    ZSTDGPU_KERNEL(InitResources)                   \
    ZSTDGPU_KERNEL(Memset)                          \
    ZSTDGPU_KERNEL(MemsetMemcpy)                    \
    ZSTDGPU_KERNEL(ParseCompressedBlocks)           \
    ZSTDGPU_KERNEL(ParseFrames)                     \
    ZSTDGPU_KERNEL(PrefixSequenceOffsets)           \
    ZSTDGPU_KERNEL(PrefixSum)                       \
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
    ZSTDGPU_KERNEL_SCOPE_X(PrefixSum                            , L"Prefix Sums"                )

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_ALL_BLOCKS() \
    ZSTDGPU_KERNEL_SCOPE_X(InitResources                        , L"Init Resources"             )   \
    ZSTDGPU_KERNEL_SCOPE_X(ParseFrames                          , L"Parse Frames"               )

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_CMP_BLOCKS() \
    ZSTDGPU_KERNEL_SCOPE_X(ParseCompressedBlocks                , L"Parse Compressed Blocks"    )

#define ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_CMP_BLOCKS() \
    ZSTDGPU_KERNEL_SCOPE_X(UpdateDispatchArgs                   , L"Update Dispatch Arguments"  )   \
    ZSTDGPU_KERNEL_SCOPE_X(ComputePrefixSum                     , L"Compute Prefix Sums"        )   \
    ZSTDGPU_KERNEL_SCOPE_X(GroupCompressedLiterals              , L"Group Compressed Literals"  )   \
    ZSTDGPU_KERNEL_SCOPE_X(InitFseTable                         , L"Init FSE Tables"            )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecompressHuffmanWeights             , L"Decompress Huffman Weights" )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecodeHuffmanWeights                 , L"Decode Huffman Weights"     )   \
    ZSTDGPU_KERNEL_SCOPE_X(InitHuffmanTable                     , L"Init Huffman Table"         )   \
    ZSTDGPU_KERNEL_SCOPE_X(DecompressLiterals                   , L"Decompress Literals"        )   \
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
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_ALL_BLOCKS()      \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_CMP_BLOCKS()      \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_CMP_BLOCKS()      \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_RAW_RLE_BLOCKS()  \
    ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_2_ALL_BLOCKS()

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

    ZSTDGPU_ENUM(SetupInputsType) setupInputsType;
};

static uint32_t zstdgpu_GetStageCountFromSetupInputsType(ZSTDGPU_ENUM(SetupInputsType) type)
{
    ZSTDGPU_UNUSED(type);
    return ZSTDGPU_ENUM_CONST(ResourceAllocation_StageCount);
}

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
            ZSTDGPU_KERNEL_MAP(DecompressSequences, DecompressSequences_SingleStream_LdsFseCache32);
            context->DecompressSequences_StreamsPerGroup = 1;
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
        context->setupInputsType                    = ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_Unknown);

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
        if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_GpuMemory) == inPerRequestContext->setupInputsType)
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
        inPerRequestContext->setupInputsType                = ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_CpuMemory);

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
        if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_GpuMemory) == inPerRequestContext->setupInputsType)
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
        inPerRequestContext->setupInputsType                = ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_GpuMemory);

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

ZSTDGPU_ENUM(Status) zstdgpu_GetGpuMemoryRequirement(uint32_t *outDefaultHeapByteCount, uint32_t *outUploadHeapByteCount, uint32_t *outReadbackHeapByteCount, uint32_t *outShaderVisibleDescriptorCount, zstdgpu_PerRequestContext req, uint32_t stageIndex)
{
    uint32_t proceed = 1;
    uint32_t stageCount = zstdgpu_GetStageCountFromSetupInputsType(req->setupInputsType);

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
        #define CNTRS(name) req->resData.gpu2Cpu.CountersCpu->name
        if (stageIndex == 0)
        {
            zstdgpu_ResourceInfo_Stage_0_Init(&req->resInfo, req->zstdFrameCount, req->zstdCompressedFramesByteCount, ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_GpuMemory) == req->setupInputsType ? 1u : 0u);
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

            ZSTDGPU_ASSERT(0 != cntRaw + cntRle + cntCmp);

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
        if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_GpuMemory) == req->setupInputsType && stageIndex == 0u)
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
            if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_CpuMemory) == req->setupInputsType)
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

        return ZSTDGPU_ENUM_CONST(StatusSuccess);
    }
    return ZSTDGPU_ENUM_CONST(StatusInvalidArgument);
}

ZSTDGPU_ENUM(Status) zstdgpu_SubmitWithInteralMemory(zstdgpu_PerRequestContext req, uint32_t stageIndex, struct ID3D12GraphicsCommandList *cmdList)
{
    uint32_t proceed = 1;
    uint32_t stageCount = zstdgpu_GetStageCountFromSetupInputsType(req->setupInputsType);

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
            zstdgpu_ResourceInfo_Stage_0_Init(&req->resInfo, req->zstdFrameCount, req->zstdCompressedFramesByteCount, ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_GpuMemory) == req->setupInputsType ? 1u : 0u);
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

            ZSTDGPU_ASSERT(0 != cntRaw + cntRle + cntCmp);

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
        if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_GpuMemory) == req->setupInputsType && stageIndex == 0u)
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
            if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_CpuMemory) == req->setupInputsType)
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

#define zstdgpu_DispatchIndirect(cmdList, kernelName, counterName) \
    cmdList->ExecuteIndirect(req->kernelName##_CmdSig, kzstdgpu_DispatchSlot_CmdsPerSlot, req->resData.gpuOnly.DispatchArgs, kzstdgpu_DispatchSlot_##counterName * kzstdgpu_DispatchSlot_StrideInUInt32 * sizeof(uint32_t), req->resData.gpuOnly.DispatchCnts, kzstdgpu_DispatchSlot_##counterName * sizeof(uint32_t));

void zstdgpu_SubmitStage0(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    {
        D3D12_RESOURCE_BARRIER barriers[3];
        uint32_t uploadBarrierCount = 0;

        #define zstdgpu_PushUpload(name) cmdList->CopyResource(req->resData.gpuOnly.name, req->resData.cpu2Gpu.name)

        if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_CpuMemory) == req->setupInputsType)
        {
            zstdgpu_PushUpload(CompressedData);
            zstdgpu_PushUpload(FramesRefs);
        }
        zstdgpu_PushUpload(FseProbsDefault);
        #undef zstdgpu_PushUpload

        setResourceState(barriers, 0, req->resData.gpuOnly.FseProbsDefault, COPY_DEST, NON_PIXEL_SHADER_RESOURCE);
        uploadBarrierCount += 1;
        if (ZSTDGPU_ENUM_CONST(SetupInputsType_Frames_CpuMemory) == req->setupInputsType)
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

        // last written by [Init Resources :: Stage 0] and [InitResources :: Memset :: Stage 0]
        // next written/atomically updated by [Parse Frames :: Count Blocks]
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.Counters);
        // last written by [InitResources :: Memset :: Stage 0]
        // next written/updated by [Parse Compressed Blocks]
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameSeqStreamMinIdx);
        // last written by [InitResources :: Memset :: Stage 0]
        // next written by [Parse Frames :: Block Counts]
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountRAW);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountRLE);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountCMP);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockCountAll);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockSizesRAW);
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.PerFrameBlockSizesRLE);

        ZSTDGPU_ASSERT(bc == _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
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
        cmdList->SetComputeRoot32BitConstant(3, req->DecompressSequences_StreamsPerGroup, 0);
        cmdList->SetComputeRoot32BitConstant(3, 0 /* stage */, 1);
        ZSTDGPU_KERNEL_SCOPE(UpdateDispatchArgs_Stage0, cmdList,
            cmdList->Dispatch(1, 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Readback Counters :: After Block Count] and [Parse Compressed Blocks]");
        D3D12_RESOURCE_BARRIER barriers[5];
        // last read by [Update Dispatch Args :: Stage 0]
        // next read by [Readback Counters :: After Block Count]
        setResourceState(barriers, 0, req->resData.gpuOnly.Counters, UNORDERED_ACCESS, COPY_SOURCE);
        // last written by [PrefixSum :: Block Counts]
        // next read by [Parse Compressed Blocks]
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.PerFrameBlockCountCMP);
        // last written by [PrefixSum :: Block Counts]
        // next read by [Prefix Sequence Offsets] and [Finalise Sequence Offsets]
        setResourceUavToSrvSync(barriers, 2, req->resData.gpuOnly.PerFrameBlockCountAll);
        // last written by [Update Dispatch Args :: Stage 0]
        // next read by ExecuteIndirect calls as argument/count buffers
        setResourceState(barriers, 3, req->resData.gpuOnly.DispatchArgs, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        setResourceState(barriers, 4, req->resData.gpuOnly.DispatchCnts, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
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
            cmdList->Dispatch(zstdgpu_InitResources_GetDispatchSizeX(initResourcesStage), 1, 1);
        );

        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[InitResources :: Memset :: Stage 1]");
        d3d12aid_ComputeRsPs_Set(&req->Memset, cmdList);

        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, 0 /* value */, 2);

        // Group 1: CmpBlockLookback-sized regions (6 UAV rebinds, same dispatch slot)
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.LitGroupEndPerHuffmanTable->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset1->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset2->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.PerSeqStreamFinalOffset3->GetGPUVirtualAddress() + req->zstdCmpBlockCount * sizeof(uint32_t));
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.SeqCountPrefixLookback->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.BlockSeqCountPrefixLookback->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_CmpBlockLookback);

        // Group 2: TableIndexLookback
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.TableIndexLookback->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_TableIndexLookback);

        // Group 3: LitStreamEndPerHuffmanTable (prefix + lookback region)
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.LitStreamEndPerHuffmanTable->GetGPUVirtualAddress());
        zstdgpu_DispatchIndirect(cmdList, Memset, Memset_LitStreamEnd);

        // Group 4: BlockSizePrefix lookback (allBlockCount-sized)
        {
            const uint32_t allBlockCount = req->zstdRawBlockCount + req->zstdRleBlockCount + req->zstdCmpBlockCount;
            cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.BlockSizePrefix->GetGPUVirtualAddress() + allBlockCount * sizeof(uint32_t));
        }
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
            // last written by [InitResources :: Memset :: Stage 1]
            // next written/updated by [Parse Compressed Blocks] -- both "lookback" and "prefix" sub-buffers
            setResourceUavSync(barriers, bc + 3, req->resData.gpuOnly.TableIndexLookback);
            // last written by [InitResources :: Memset :: Stage 1]
            // next written/updated by [Parse Compressed Blocks]
            setResourceUavSync(barriers, bc + 4, req->resData.gpuOnly.LitStreamEndPerHuffmanTable);
            setResourceUavSync(barriers, bc + 5, req->resData.gpuOnly.PerFrameSeqStreamMinIdx);
            // last written by [Parse Frames :: Collect Blocks]
            // next read by [Parse Compressed Blocks]
            setResourceUavToSrvSync(barriers, bc + 6, req->resData.gpuOnly.BlocksCMPRefs);
            // last written by [InitResources :: Memset :: Stage 1]
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
            // [NOTE] PerFrameBlockCountCMP is bound as UAV by [InitResources :: Memset :: Stage 1] and [Parse Frames],
            // and later read by ParseCompressedBlocks as SRV, so we transition to read state
            setResourceUavToSrvSync(barriers, bc + 11, req->resData.gpuOnly.PerFrameBlockCountCMP);
            bc += 12;
        }
        else
        {
            // last written by [Parse Frames :: Collect Blocks]
            // next read by [Readback Counters :: After Block Parse] and updated by [Update Dispatch Args]
            setResourceUavToSrvSync(barriers, bc + 0, req->resData.gpuOnly.Counters);
            bc += 1;

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
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 2);
        cmdList->SetComputeRoot32BitConstant(1, req->zstdFrameCount, 3);

        ZSTDGPU_KERNEL_SCOPE(ParseCompressedBlocks, cmdList,
            zstdgpu_DispatchIndirect(cmdList, ParseCompressedBlocks, ParseCompressedBlocks);
        );
        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Readback Counters :: After Block Parse] and [Update Dispatch Args] and [Compute `Per-Huffman Table` Literal Stream Count Prefix]");
        D3D12_RESOURCE_BARRIER barriers[11];
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
        // last read by ExecuteIndirect as INDIRECT_ARGUMENT
        // next written by [Update Dispatch Args :: Stage 2]
        setResourceState(barriers, 9, req->resData.gpuOnly.DispatchArgs, INDIRECT_ARGUMENT, UNORDERED_ACCESS);
        setResourceState(barriers, 10, req->resData.gpuOnly.DispatchCnts, INDIRECT_ARGUMENT, UNORDERED_ACCESS);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Readback Counters :: After Block Parse]");
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
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.DispatchArgs->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.DispatchCnts->GetGPUVirtualAddress());
        cmdList->SetComputeRoot32BitConstant(3, req->DecompressSequences_StreamsPerGroup, 0);
        cmdList->SetComputeRoot32BitConstant(3, 1 /* stage */, 1);
        ZSTDGPU_KERNEL_SCOPE(UpdateDispatchArgs, cmdList,
            cmdList->Dispatch(1, 1, 1);
        );
        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier for [Compute `Per-Huffman Table` Literal Stream Count Prefix]");
        D3D12_RESOURCE_BARRIER barriers[3];
        // last written by [Update Dispatch Args]
        // next read by [Compute `Per-Huffman Table` Literal Stream Count Prefix] via ExecuteIndirect
        setResourceState(barriers, 0, req->resData.gpuOnly.DispatchArgs, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        setResourceState(barriers, 1, req->resData.gpuOnly.DispatchCnts, UNORDERED_ACCESS, INDIRECT_ARGUMENT);
        // last written by [Update Dispatch Args]
        // next read/written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        setResourceUavSync(barriers, 2, req->resData.gpuOnly.Counters);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
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
    if (req->zstdCmpBlockCount > 0)
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
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Update Dispatch Args :: DecompressLiterals]");
        d3d12aid_ComputeRsPs_Set(&req->UpdateDispatchArgs, cmdList);
        cmdList->SetComputeRootUnorderedAccessView(0, req->resData.gpuOnly.Counters->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(1, req->resData.gpuOnly.DispatchArgs->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(2, req->resData.gpuOnly.DispatchCnts->GetGPUVirtualAddress());
        cmdList->SetComputeRoot32BitConstant(3, req->DecompressSequences_StreamsPerGroup, 0);
        cmdList->SetComputeRoot32BitConstant(3, 2 /* stage */, 1);
        ZSTDGPU_KERNEL_SCOPE(UpdateDispatchArgs_DecompressLiterals, cmdList,
            cmdList->Dispatch(1, 1, 1);
        );
        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Init FSE Table] and [Group Lilteral Streams]");
        D3D12_RESOURCE_BARRIER barriers[14];
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
        // last written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Group Lilteral Streams]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.LitStreamEndPerHuffmanTable);
        // last written by [Parse Compressed Blocks]
        // next read by [Group Lilteral Streams]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.LitStreamBuckets);
        // last written by [Parse Compressed Blocks]
        // last read by [DEBUG READBACK]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.CompressedBlocks);
        // last written by [Parse Compressed Blocks]
        // next read by [Decompress Huffman Weights] and [Decode Uncompressed Huffman Weights] and [DEBUG READBACK]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.HufRefs);
        // last written by [Compute `Per-Huffman Table` Literal Stream Count Prefix]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.LitGroupEndPerHuffmanTable);
        //
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.TableIndexLookback);
        // last written by [Parse Compressed Blocks]
        // next read by [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.LitRefs);
        // last written by [Parse Compressed Blocks]
        // next read by [Decompress Sequences]
        setResourceUavToSrvSync(barriers, bc ++, req->resData.gpuOnly.SeqRefs);
        // last written by [Parse Compressed Blocks]
        // next written by [Decompress Huffman Weights] and read as UAV by [Decode Uncompressed Huffman Weights]
        setResourceUavSync(barriers, bc ++, req->resData.gpuOnly.DecompressedHuffmanWeightCount);

        ZSTDGPU_ASSERT(bc <= _countof(barriers));
        cmdList->ResourceBarrier(bc, barriers);
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
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(GroupCompressedLiterals, cmdList,
            zstdgpu_DispatchIndirect(cmdList, GroupCompressedLiterals, GroupCompressedLiterals);
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

            // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Huffman Weights");
            uint32_t tableStartIndex = 0;
            cmdList->SetComputeRoot32BitConstant(1, tableStartIndex, 2);
            cmdList->SetComputeRoot32BitConstant(1, zstdgpu_ComputeFseDataStartHufW(0, req->zstdCmpBlockCount), 3);
            cmdList->SetComputeRoot32BitConstant(1, kzstdgpu_FseElemMaxCount_HufW, 4);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseHufW);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Literal Lengths");
            tableStartIndex += req->zstdCmpBlockCount;
            cmdList->SetComputeRoot32BitConstant(1, tableStartIndex, 2);
            cmdList->SetComputeRoot32BitConstant(1, zstdgpu_ComputeFseDataStartLLen(0, req->zstdCmpBlockCount), 3);
            cmdList->SetComputeRoot32BitConstant(1, kzstdgpu_FseElemMaxCount_LLen, 4);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseLLen);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Offsets");
            tableStartIndex += req->zstdCmpBlockCount + 1;
            cmdList->SetComputeRoot32BitConstant(1, tableStartIndex, 2);
            cmdList->SetComputeRoot32BitConstant(1, zstdgpu_ComputeFseDataStartOffs(0, req->zstdCmpBlockCount), 3);
            cmdList->SetComputeRoot32BitConstant(1, kzstdgpu_FseElemMaxCount_Offs, 4);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseOffs);
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"FSEs for Match Lengths");
            tableStartIndex += req->zstdCmpBlockCount + 1;
            cmdList->SetComputeRoot32BitConstant(1, tableStartIndex, 2);
            cmdList->SetComputeRoot32BitConstant(1, zstdgpu_ComputeFseDataStartMLen(0, req->zstdCmpBlockCount), 3);
            cmdList->SetComputeRoot32BitConstant(1, kzstdgpu_FseElemMaxCount_MLen, 4);
            zstdgpu_DispatchIndirect(cmdList, InitFseTable, FseMLen);
            PIXEndEvent(cmdList);
            PIXEndEvent(cmdList);
        });
    }

    // Needed by readback
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Huffman Weights Decompression] and [Decompress Literals]");
        D3D12_RESOURCE_BARRIER barriers[2];
        // last written by [Init FSE Table]
        // next read by [Decompress Huffman Weights] and [Decompress Sequences]
        setResourceUavToSrvSync(barriers, 0, req->resData.gpuOnly.FseElems);
        // last written by [Group Lilteral Streams]
        // next read [Init Huffman Table and Decompress Literals]
        setResourceUavToSrvSync(barriers, 1, req->resData.gpuOnly.LitStreamRemap);
        cmdList->ResourceBarrier(_countof(barriers), barriers);
        PIXEndEvent(cmdList);
    }

    // Run Decompression of FSE-compressed Huffman Weights
    if (req->zstdCmpBlockCount > 0)
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
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decode Uncompressed Huffman Weights]");
        BIND_RS_PS_SRT(DecodeHuffmanWeights);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount, 2);
        cmdList->SetComputeRoot32BitConstant(1, req->resInfo.CompressedData_ByteSize, 3);

        ZSTDGPU_KERNEL_SCOPE(DecodeHuffmanWeights, cmdList,
            zstdgpu_DispatchIndirect(cmdList, DecodeHuffmanWeights, DecodeHuffmanWeights);
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
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Pre-Init Huffman Table]");
        BIND_RS_PS_SRT(InitHuffmanTable);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(InitHuffmanTable, cmdList,
        {
            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Path: FSE-compressed Huffman Weights]");
            {
                cmdList->SetComputeRoot32BitConstant(1, /** HuffmaTableIndexBase*/0, 2);
                zstdgpu_DispatchIndirect(cmdList, InitHuffmanTable, FseHufW);
            }
            PIXEndEvent(cmdList);

            PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Path: Uncompressed Huffman Weights]");
            {
                cmdList->SetComputeRoot32BitConstant(1, /** HuffmaTableIndexBase = zstdCmpBlockCount meaning indices are reversed */req->zstdCmpBlockCount, 2);
                zstdgpu_DispatchIndirect(cmdList, InitHuffmanTable, HUF_WgtStreams);
            }
            PIXEndEvent(cmdList);
        });

        PIXEndEvent(cmdList);
    }

    if (req->zstdCmpBlockCount > 0)
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

    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decompress Literals]");
        BIND_RS_PS_SRT(DecompressLiterals);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(1, req->zstdCmpBlockCount, 2);

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
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Decompress Sequences]");
        BIND_RS_PS_SRT(DecompressSequences);
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(DecompressSequences, cmdList,
            zstdgpu_DispatchIndirect(cmdList, DecompressSequences, DecompressSequences);
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
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
        cmdList->SetComputeRoot32BitConstant(2, 1 /** outputInclusive */, 2);

        ZSTDGPU_KERNEL_SCOPE(PrefixBlockSizes, cmdList,
            zstdgpu_DispatchIndirect(cmdList, PrefixSum, PrefixBlockSizes);
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
        // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch

        ZSTDGPU_KERNEL_SCOPE(FinaliseSequenceOffsets, cmdList,
            zstdgpu_DispatchIndirect(cmdList, FinaliseSequenceOffsets, FinaliseSequenceOffsets);
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
                // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
                cmdList->SetComputeRoot32BitConstant(5, req->zstdRawBlockCount, 2);
                cmdList->SetComputeRoot32BitConstant(5, req->zstdFrameCount, 3);
                cmdList->SetComputeRoot32BitConstant(5, 1 /* flags */, 4);
                zstdgpu_DispatchIndirect(cmdList, MemsetMemcpy, MemcpyRAW);
            }
            if (req->zstdRleByteCount > 0)
            {
                cmdList->SetComputeRootShaderResourceView(1, req->resData.gpuOnly.RleBlockSizePrefix->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(2, req->resData.gpuOnly.PerFrameBlockSizesRLE->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(3, req->resData.gpuOnly.BlocksRLERefs->GetGPUVirtualAddress());
                cmdList->SetComputeRootShaderResourceView(4, req->resData.gpuOnly.GlobalBlockIndexPerRleBlock->GetGPUVirtualAddress());
                // NOTE: Slots 0 (tgOffset) and 1 (workItemCount) are set by command signature via indirect dispatch
                cmdList->SetComputeRoot32BitConstant(5, req->zstdRleBlockCount, 2);
                cmdList->SetComputeRoot32BitConstant(5, req->zstdFrameCount, 3);
                cmdList->SetComputeRoot32BitConstant(5, 0 /* flags */, 4);
                zstdgpu_DispatchIndirect(cmdList, MemsetMemcpy, MemsetRLE);
            }
        });
        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"Barrier with Resources for [Execute Sequences]");
        D3D12_RESOURCE_BARRIER barriers[2];
        uint32_t bc = 0;
        if (req->zstdRawByteCount > 0 || req->zstdRleByteCount > 0)
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
    if (req->zstdCmpBlockCount > 0)
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Execute Sequences]");
        BIND_RS_PS_SRT(ExecuteSequences);

        ZSTDGPU_KERNEL_SCOPE(ExecuteSequences, cmdList,
            cmdList->Dispatch(req->zstdFrameCount, 1, 1);
        );
        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0)
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

        zstdgpu_Dispatch32Bit(cmdList, ZSTDGPU_TG_COUNT(req->zstdUncompressedSequenceCount, 256), 1, 0);

        PIXEndEvent(cmdList);
    }
    if (req->zstdCmpBlockCount > 0) /* Readback for Counters is only needed if the number of compressed blocks > 0 because Counters are updated during Seqeunce Execution */
    {
        PIXBeginEvent(cmdList, PIX_COLOR_DEFAULT, L"[Readback Counters :: After Block Decompression]");
        zstdgpu_PushReadback(Counters);
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
    outGpuResources->Counters->Blocks_RAW = req->zstdRawBlockCount;
    outGpuResources->Counters->Blocks_RLE = req->zstdRleBlockCount;
    outGpuResources->Counters->Blocks_CMP = req->zstdCmpBlockCount;
}

ZSTDGPU_API void zstdgpu_ReadbackTimestamps(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList)
{
    d3d12aid_Timestamps_AdvanceFrame(&req->timestamps, cmdList);
}

ZSTDGPU_API void zstdgpu_RetrieveTimestamps(const wchar_t **outTimestampScopeNames, uint64_t *outTimestampScopeClocks, uint32_t *inoutTimestampScopeCnt, zstdgpu_PerRequestContext req, uint32_t stageIndex)
{
#if ZSTDGPU_ENABLE_TIMESTAMPS
    uint32_t timestampScopeCountPerStage = 0;
    #define ZSTDGPU_KERNEL_SCOPE_X(name, desc) +1
    if (stageIndex == 0)
    {
        timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_0();
    }
    else if (stageIndex == 1)
    {
        timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_ALL_BLOCKS();
        if (req->zstdCmpBlockCount > 0)
        {
            timestampScopeCountPerStage += 0 ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_CMP_BLOCKS();
        }
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
            ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_ALL_BLOCKS()
            if (req->zstdCmpBlockCount > 0)
            {
                ZSTDGPU_KERNEL_SCOPE_LIST_STAGE_1_CMP_BLOCKS();
            }
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
#else
    *inoutTimestampScopeCnt = 0;

    ZSTDGPU_UNUSED(outTimestampScopeNames);
    ZSTDGPU_UNUSED(outTimestampScopeClocks);
    ZSTDGPU_UNUSED(req);
    ZSTDGPU_UNUSED(stageIndex);
#endif /* #if ZSTDGPU_ENABLE_TIMESTAMPS */
}
