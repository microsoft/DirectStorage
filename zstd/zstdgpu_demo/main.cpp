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
#include <stdio.h>

#include <winsdkver.h>
#define _WIN32_WINNT 0x0A00
#include <sdkddkver.h>

#define NOMINMAX
#define NODRAWTEXT
#define NOGDI
#define NOBITMAP
#define NOMCX
#define NOSERVICE
#define NOHELP
#include <Windows.h>

#if defined(_GAMING_XBOX_SCARLETT)
#   include <d3d12_xs.h>
#elif defined(_GAMING_XBOX_XBOXONE)
#   include <d3d12_x.h>
#else
#   include <d3d12.h>
#   include <dxgi1_6.h>
#   include <dxgidebug.h>
#endif

#if defined(_GAMING_XBOX)
#include <xmem.h>
#endif

#define D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT 2
#include <d3d12aid.h>

#include <pix3.h>

extern "C" {
#include "zstd_decompress.h"
}

#include <assert.h>

#include "zstdgpu_reference_store.h"
#include "zstdgpu_shaders.h"
#include "zstdgpu.h"

#include "platform/zstdgpu_demo_platform.h"

#ifdef __clang__
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

#pragma warning(disable : 4505) // warning C4505: 'name': unreferenced function with internal linkage has been removed

static void debugPrint(const wchar_t *format, ...)
{
    const size_t bufferSize = 1024;
    wchar_t buffer[bufferSize + 1];

    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, bufferSize + 1, bufferSize, format, args);
    wprintf(buffer);
    OutputDebugStringW(buffer);
    va_end(args);
}

static void loadFileAligned(void **outData, uint32_t *outDataSize, uint32_t *outBufferSize, uint32_t alignmentLog2, const wchar_t* fileName)
{
    const size_t alignmentMask = (1ull << alignmentLog2) - 1ull;
    size_t dataSize = 0;
    size_t bufferSize = 0;

    void* data = NULL;
    FILE* file = NULL;
    _wfopen_s(&file, fileName, L"rb");
    if (NULL != file)
    {
        fseek(file, 0, SEEK_END);
        dataSize = ftell(file);
        fseek(file, 0, SEEK_SET);
        bufferSize = (dataSize + alignmentMask) & ~alignmentMask;
        data = malloc(bufferSize);
        size_t readSize = fread(data, 1, dataSize, file);
        ZSTDGPU_ASSERT(readSize == dataSize);
        fclose(file);
        if (bufferSize > dataSize)
        {
            memset((char *)data + dataSize, 0, bufferSize - dataSize);
        }
    }
    *outData = data;
    *outDataSize = (uint32_t)dataSize;
    *outBufferSize = (uint32_t)bufferSize;
}

static void saveFile(const wchar_t *fileName, const void *data, uint32_t dataSize)
{
    FILE *file = NULL;
    _wfopen_s(&file, fileName, L"wb");
    if (NULL != file)
    {
        uint32_t writtenByteCount = (uint32_t)fwrite(data, 1, dataSize, file);
        if (writtenByteCount != dataSize)
        {
            debugPrint(L"[IO] Saving '%s' failed, written %u bytes instead of %u bytes.\n", fileName, writtenByteCount, dataSize);
        }
        fflush(file);
        fclose(file);
    }
    else
    {
        debugPrint(L"[IO] Saving '%s' failed. Couldn't open file.\n", fileName);
    }
}

/***********************************************************************************************************************
 *
 *
 * Start of the actual ZSTDGPU stuff. TODO: Move to zstdgpu.h/c
 *
 *
 **********************************************************************************************************************/

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

#include "zstdgpu_resources.h"

#define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                      srt.in##name    = resources.name;
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                      srt.inout##name = resources.name;
#define ZSTDGPU_RW_BUFFER_DECL_GLC(type, name, index)                  srt.inout##name = resources.name;
#define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     srt.in##name    = resources.name;
#define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)     srt.inout##name = resources.name;

static void zstdgpu_Init_InitResources_SRT(zstdgpu_InitResources_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    // HACK(pamartis): redefine one macro to allow casts (needed by TableIndexLookback)
#undef ZSTDGPU_RW_BUFFER_DECL
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                      srt.inout##name = (type *)resources.name;
    ZSTDGPU_INIT_RESOURCES_SRT();
#undef ZSTDGPU_RW_BUFFER_DECL
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                      srt.inout##name = resources.name;
}

static void zstdgpu_Init_ParseFrames_SRT(zstdgpu_ParseFrames_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_PARSE_FRAMES_SRT()
}

static void zstdgpu_Init_ParseCompressedBlocks_SRT(zstdgpu_ParseCompressedBlocks_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_PARSE_COMPRESSED_BLOCKS_SRT()
}

static void zstdgpu_Init_InitFseTable_SRT(zstdgpu_InitFseTable_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_INIT_FSE_TABLE_SRT()
}

static void zstdgpu_Init_DecompressHuffmanWeights_SRT(zstdgpu_DecompressHuffmanWeights_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_DECOMPRESS_HUFFMAN_WEIGHTS_SRT()
}

static void zstdgpu_Init_DecodeHuffmanWeights_SRT(zstdgpu_DecodeHuffmanWeights_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_DECODE_HUFFMAN_WEIGHTS_SRT()
}

static void zstdgpu_Init_InitHuffmanTable_And_DecompressLiterals_SRT(zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_SRT()
}

static void zstdgpu_Init_DecompressSequences_SRT(zstdgpu_DecompressSequences_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()
}

static void zstdgpu_Init_FinaliseSequenceOffsets_SRT(zstdgpu_FinaliseSequenceOffsets_SRT & srt, zstdgpu_ResourceDataCpu& resources)
{
    ZSTDGPU_FINALISE_SEQUENCE_OFFSETS_SRT()
}

#undef ZSTDGPU_RW_TYPED_BUFFER_DECL
#undef ZSTDGPU_RO_TYPED_BUFFER_DECL
#undef ZSTDGPU_RW_BUFFER_DECL_GLC
#undef ZSTDGPU_RW_BUFFER_DECL
#undef ZSTDGPU_RO_BUFFER_DECL

#define STRINGIZE(x) STRINGIZE2(x)
#define STRINGIZE2(x) #x
#define VALIDATE(name) \
    if (kzstdgpu_Validate_Success != zstdgpu_ReferenceStore_Validate_##name) \
        debugPrint(L"Validation of "#name" failed in function: " __FUNCTION__ ", file: " __FILE__ ", line: " STRINGIZE(__LINE__) "\n")

static void zstdgpu_Test_DecompressHuffmanWeights(zstdgpu_ResourceDataCpu & cpuRes, zstdgpu_ResourceDataCpu & gpuReadbackRes, uint32_t zstdDataBufferSize, bool chkGpu, bool simGpu)
{
    ZSTDGPU_UNUSED(cpuRes);
    ZSTDGPU_UNUSED(gpuReadbackRes);
    ZSTDGPU_UNUSED(zstdDataBufferSize);

    // NOTE(pamartis): Validate GPU data first against reference
    if (chkGpu)
    {
        uint32_t* tmp = gpuReadbackRes.CompressedData;
        gpuReadbackRes.CompressedData = cpuRes.CompressedData;
        VALIDATE(DecompressedHuffmanWeights(&gpuReadbackRes));
        VALIDATE(DecodedHuffmanWeights(&gpuReadbackRes));
        gpuReadbackRes.CompressedData = tmp;
    }

    // NOTE(pamartis): When GPU output data is potentially broken, compute it on CPU (to debug) using same inputs as on GPU
    if (simGpu)
    {
        zstdgpu_DecompressHuffmanWeights_SRT srt = {};
        zstdgpu_Init_DecompressHuffmanWeights_SRT(srt, gpuReadbackRes);
        srt.inCompressedData                    = cpuRes.CompressedData;
        srt.inoutDecompressedHuffmanWeights     = cpuRes.DecompressedHuffmanWeights;
        srt.inoutDecompressedHuffmanWeightCount = cpuRes.DecompressedHuffmanWeightCount;
        for (uint32_t i = 0; i < gpuReadbackRes.Counters[kzstdgpu_CounterIndex_FseHufW]; ++i)
        {
            zstdgpu_ShaderEntry_DecompressHuffmanWeights(srt, i);
        }

        if (chkGpu)
        {
            // NOTE(pamartis): After CPU data is computed, validate it against reference, and if it's broken, likely the inputs are wrong
            // NOTE(pamartis): Make sure that validation against reference see the same blocks as when validating GPU data
            uint32_t *CompressedData                = gpuReadbackRes.CompressedData;
            uint8_t *DecompressedHuffmanWeights     = gpuReadbackRes.DecompressedHuffmanWeights;
            uint8_t *DecompressedHuffmanWeightCount = gpuReadbackRes.DecompressedHuffmanWeightCount;

            gpuReadbackRes.CompressedData                   = cpuRes.CompressedData;
            gpuReadbackRes.DecompressedHuffmanWeights       = cpuRes.DecompressedHuffmanWeights;
            gpuReadbackRes.DecompressedHuffmanWeightCount   = cpuRes.DecompressedHuffmanWeightCount;
            VALIDATE(DecompressedHuffmanWeights(&gpuReadbackRes));
            gpuReadbackRes.CompressedData                   = CompressedData;
            gpuReadbackRes.DecompressedHuffmanWeights       = DecompressedHuffmanWeights;
            gpuReadbackRes.DecompressedHuffmanWeightCount   = DecompressedHuffmanWeightCount;
        }
    }

    if (simGpu)
    {
        zstdgpu_DecodeHuffmanWeights_SRT srt = {};
        zstdgpu_Init_DecodeHuffmanWeights_SRT(srt, gpuReadbackRes);
        srt.inCompressedData                    = cpuRes.CompressedData;
        srt.inoutDecompressedHuffmanWeights     = cpuRes.DecompressedHuffmanWeights;
        // NOTE(pamartis): We don't need to set CPU-side Huffman Weight Counts because they're not computed within kernel
        // like in the "Decompress" case, and only read instead. And therefore we want to use GPU data
        //srt.inoutDecompressedHuffmanWeightCount = cpuRes.DecompressedHuffmanWeightCount;
        srt.compressedBlockCount                = gpuReadbackRes.Counters[kzstdgpu_CounterIndex_Blocks_CMP];
        srt.compressedBufferSizeInBytes         = zstdDataBufferSize;
        for (uint32_t i = 0; i < gpuReadbackRes.Counters[kzstdgpu_CounterIndex_HUF_WgtStreams]; ++i)
        {
            zstdgpu_ShaderEntry_DecodeHuffmanWeights(srt, i);
        }

        if (chkGpu)
        {
            // NOTE(pamartis): After CPU data is computed, validate it against reference, and if it's broken, likely the inputs are wrong
            // NOTE(pamartis): Make sure that validation against reference see the same blocks as when validating GPU data
            uint32_t *CompressedData                = gpuReadbackRes.CompressedData;
            uint8_t *DecompressedHuffmanWeights     = gpuReadbackRes.DecompressedHuffmanWeights;

            gpuReadbackRes.CompressedData                   = cpuRes.CompressedData;
            gpuReadbackRes.DecompressedHuffmanWeights       = cpuRes.DecompressedHuffmanWeights;
            VALIDATE(DecodedHuffmanWeights(&gpuReadbackRes));
            gpuReadbackRes.CompressedData                   = CompressedData;
            gpuReadbackRes.DecompressedHuffmanWeights       = DecompressedHuffmanWeights;
        }
    }
}

static void zstdgpu_Test_DecompressLiterals(zstdgpu_ResourceDataCpu & cpuRes, zstdgpu_ResourceDataCpu & gpuReadbackRes, uint32_t zstdDataBufferSize, bool chkGpu, bool simGpu)
{
    ZSTDGPU_UNUSED(cpuRes);
    ZSTDGPU_UNUSED(gpuReadbackRes);
    ZSTDGPU_UNUSED(zstdDataBufferSize);

    // NOTE(pamartis): Validate GPU data first against reference
    if (chkGpu)
    {
        uint32_t* tmp = gpuReadbackRes.CompressedData;
        gpuReadbackRes.CompressedData = cpuRes.CompressedData;
        VALIDATE(DecompressedLiterals(&gpuReadbackRes));
        gpuReadbackRes.CompressedData = tmp;
    }

    if (simGpu)
    {
        // NOTE(pamartis): When GPU output data is potentially broken, compute it on CPU (to debug) using same inputs as on GPU
        zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT srt;
        zstdgpu_Init_InitHuffmanTable_And_DecompressLiterals_SRT(srt, gpuReadbackRes);
        srt.inCompressedData            = cpuRes.CompressedData;
        srt.inoutDecompressedLiterals   = cpuRes.DecompressedLiterals;
        srt.huffmanTableSlotCount       = gpuReadbackRes.Counters[kzstdgpu_CounterIndex_Blocks_CMP];

        for (uint32_t groupId = 0; groupId < gpuReadbackRes.Counters[kzstdgpu_CounterIndex_DecompressLiteralsGroups]; ++groupId)
        {
            zstdgpu_ShaderEntry_InitHuffmanTable_And_DecompressLiterals(srt, groupId, 0);
        }

        if (chkGpu)
        {
            // NOTE(pamartis): After CPU data is computed, validate it against reference, and if it's broken, likely the inputs are wrong
            // NOTE(pamartis): Make sure that validation against reference see the same blocks as when validating GPU data
            uint32_t *CompressedData            = gpuReadbackRes.CompressedData;
            uint8_t *DecompressedLiterals       = gpuReadbackRes.DecompressedLiterals;

            gpuReadbackRes.CompressedData       = cpuRes.CompressedData;
            gpuReadbackRes.DecompressedLiterals = cpuRes.DecompressedLiterals;
            VALIDATE(DecompressedLiterals(&gpuReadbackRes));
            gpuReadbackRes.CompressedData       = CompressedData;
            gpuReadbackRes.DecompressedLiterals = DecompressedLiterals;
        }

    }
}

static void zstdgpu_Test_DecompressSequences(zstdgpu_ResourceDataCpu & cpuRes, zstdgpu_ResourceDataCpu & gpuReadbackRes, uint32_t zstdDataBufferSize, bool chkGpu, bool simGpu)
{
    ZSTDGPU_UNUSED(cpuRes);
    ZSTDGPU_UNUSED(gpuReadbackRes);
    ZSTDGPU_UNUSED(zstdDataBufferSize);

    // NOTE(pamartis): Validate GPU data first against reference
    if (chkGpu)
    {
        uint32_t* tmp = gpuReadbackRes.CompressedData;
        gpuReadbackRes.CompressedData = cpuRes.CompressedData;
        VALIDATE(DecompressedSequences(&gpuReadbackRes));
        gpuReadbackRes.CompressedData = tmp;
    }

    if (simGpu)
    {
        // NOTE(pamartis): When GPU output data is potentially broken, compute it on CPU (to debug) using same inputs as on GPU
        {
            zstdgpu_DecompressSequences_SRT srt;
            zstdgpu_Init_DecompressSequences_SRT(srt, gpuReadbackRes);
            srt.inCompressedData                = cpuRes.CompressedData;
            srt.inoutDecompressedSequenceLLen   = cpuRes.DecompressedSequenceLLen;
            srt.inoutDecompressedSequenceMLen   = cpuRes.DecompressedSequenceMLen;
            srt.inoutDecompressedSequenceOffs   = cpuRes.DecompressedSequenceOffs;
            srt.inoutPerSeqStreamFinalOffset1   = cpuRes.PerSeqStreamFinalOffset1;
            srt.inoutPerSeqStreamFinalOffset2   = cpuRes.PerSeqStreamFinalOffset2;
            srt.inoutPerSeqStreamFinalOffset3   = cpuRes.PerSeqStreamFinalOffset3;
            srt.inoutBlockSizePrefix            = cpuRes.BlockSizePrefix;


            for (uint32_t i = 0; i < gpuReadbackRes.Counters[kzstdgpu_CounterIndex_Seq_Streams]; ++i)
            {
                zstdgpu_ShaderEntry_DecompressSequences(srt, i);
            }
        }

        {
            zstdgpu_FinaliseSequenceOffsets_SRT srt;
            zstdgpu_Init_FinaliseSequenceOffsets_SRT(srt, gpuReadbackRes);
            srt.inoutDecompressedSequenceOffs = cpuRes.DecompressedSequenceOffs;
            for (uint32_t i = 0; i < gpuReadbackRes.Counters[kzstdgpu_CounterIndex_Seq_Streams_DecodedItems]; ++i)
            {
                zstdgpu_ShaderEntry_FinaliseSequenceOffsets(srt, i);
            }
        }

        if (chkGpu)
        {
            // NOTE(pamartis): After CPU data is computed, validate it against reference, and if it's broken, likely the inputs are wrong
            // NOTE(pamartis): Make sure that validation against reference see the same blocks as when validating GPU data
            uint32_t *CompressedData                = gpuReadbackRes.CompressedData;
            uint32_t *DecompressedSequenceLLen      = gpuReadbackRes.DecompressedSequenceLLen;
            uint32_t *DecompressedSequenceMLen      = gpuReadbackRes.DecompressedSequenceMLen;
            uint32_t *DecompressedSequenceOffs      = gpuReadbackRes.DecompressedSequenceOffs;

            gpuReadbackRes.CompressedData           = cpuRes.CompressedData;
            gpuReadbackRes.DecompressedSequenceLLen = cpuRes.DecompressedSequenceLLen;
            gpuReadbackRes.DecompressedSequenceMLen = cpuRes.DecompressedSequenceMLen;
            gpuReadbackRes.DecompressedSequenceOffs = cpuRes.DecompressedSequenceOffs;

            VALIDATE(DecompressedSequences(&gpuReadbackRes));

            gpuReadbackRes.CompressedData           = CompressedData;
            gpuReadbackRes.DecompressedSequenceLLen = DecompressedSequenceLLen;
            gpuReadbackRes.DecompressedSequenceMLen = DecompressedSequenceMLen;
            gpuReadbackRes.DecompressedSequenceOffs = DecompressedSequenceOffs;
        }

    }
}

/**
 *  @brief  This function executes GPU Decompression pipeline on CPU (by calling shader function on CPU)
 *          to give opportunity to catch errors early
 */
static void zstdgpu_Validate_GpuDecompressOnCpu(zstdgpu_ResourceDataCpu & zstdCpu, const void *zstdGpuCompressedData, const zstdgpu_OffsetAndSize *zstdFrameRefs, uint32_t zstdFrameCount, uint32_t zstdCompressedFramesByteCount, uint64_t zstdUncompressedFramesByteCount)
{
    zstdgpu_ResourceInfo zstdInfo;
    zstdgpu_ResourceInfo_InitZero(&zstdInfo);
    zstdgpu_ResourceInfo_Stage_0_Init(&zstdInfo, zstdFrameCount, zstdCompressedFramesByteCount, 0);

    zstdgpu_ResourceDataCpu_InitZero(&zstdCpu);
    zstdgpu_ResourceDataCpu_InitFromHeap(&zstdCpu, &zstdInfo);

    // TODO: consider if we can avoid this temporal copies (compressed data and frame references)
    memcpy(zstdCpu.CompressedData, zstdGpuCompressedData, zstdCompressedFramesByteCount);
    memcpy(zstdCpu.FramesRefs, zstdFrameRefs, sizeof(zstdgpu_OffsetAndSize) * zstdFrameCount);
    memcpy(zstdCpu.FseProbsDefault, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));

    #define CNTRS(name) zstdCpu.Counters[kzstdgpu_CounterIndex_##name]
    {
        zstdgpu_InitResources_SRT srt = {};
        zstdgpu_Init_InitResources_SRT(srt, zstdCpu);
        srt.allBlockCount       = 0;    // 0 means "unknown" when "initResourcesStage < 1"
        srt.cmpBlockCount       = 0;    // 0 means "unknown" when "initResourcesStage < 1"
        srt.frameCount          = zstdFrameCount;
        srt.initResourcesStage  = 0;    // 0 means -- right before 1st "parse frames" (for counting)
        zstdgpu_ShaderEntry_InitResources(srt, 0);
    }

    {
        zstdgpu_ParseFrames_SRT srt = {};
        zstdgpu_Init_ParseFrames_SRT(srt, zstdCpu);
        srt.frameCount                  = zstdFrameCount;
        srt.compressedBufferSizeInBytes = zstdInfo.CompressedData_ByteSize;
        srt.countBlocksOnly             = 1; // 1 - means we are going to count blocks only

        for (uint32_t i = 0; i < zstdFrameCount; ++i)
        {
            zstdgpu_ShaderEntry_ParseFrames(srt, i);
        }
    }
    ZSTDGPU_ASSERT(zstdFrameCount == CNTRS(Frames));
    ZSTDGPU_ASSERT(zstdUncompressedFramesByteCount == CNTRS(Frames_UncompressedByteSize));

    const uint32_t zstdRawBlockCount = CNTRS(Blocks_RAW);
    const uint32_t zstdRleBlockCount = CNTRS(Blocks_RLE);
    const uint32_t zstdCmpBlockCount = CNTRS(Blocks_CMP);

    const uint32_t zstdAllBlockCount = zstdRawBlockCount
                                     + zstdRleBlockCount
                                     + zstdCmpBlockCount;

    zstdgpu_ResourceInfo_Stage_1_Init(&zstdInfo, zstdRawBlockCount, zstdRleBlockCount, zstdCmpBlockCount);
    zstdgpu_ResourceDataCpu_InitFromHeap(&zstdCpu, &zstdInfo);

    // Compute prefixes
    {
        uint32_t prefix = 0;
        for (uint32_t i = 0; i < zstdFrameCount; ++i)
        {
            uint32_t count = zstdCpu.PerFrameBlockCountRAW[i];
            zstdCpu.PerFrameBlockCountRAW[i] = prefix;
            prefix += count;
        }

        prefix = 0;
        for (uint32_t i = 0; i < zstdFrameCount; ++i)
        {
            uint32_t count = zstdCpu.PerFrameBlockCountRLE[i];
            zstdCpu.PerFrameBlockCountRLE[i] = prefix;
            prefix += count;
        }

        prefix = 0;
        for (uint32_t i = 0; i < zstdFrameCount; ++i)
        {
            uint32_t count = zstdCpu.PerFrameBlockCountCMP[i];
            zstdCpu.PerFrameBlockCountCMP[i] = prefix;
            prefix += count;
        }

        prefix = 0;
        for (uint32_t i = 0; i < zstdFrameCount; ++i)
        {
            uint32_t count = zstdCpu.PerFrameBlockCountAll[i];
            zstdCpu.PerFrameBlockCountAll[i] = prefix;
            prefix += count;
        }
    }
    {
        zstdgpu_ParseFrames_SRT srt = {};
        zstdgpu_Init_ParseFrames_SRT(srt, zstdCpu);
        srt.frameCount                  = zstdFrameCount;
        srt.compressedBufferSizeInBytes = zstdInfo.CompressedData_ByteSize;
        srt.countBlocksOnly             = 0; // 0 - means we are going to output per-block information

        for (uint32_t i = 0; i < zstdFrameCount; ++i)
        {
            zstdgpu_ShaderEntry_ParseFrames(srt, i);
        }
    }
    VALIDATE(Blocks(&zstdCpu));

    {
        zstdgpu_InitResources_SRT srt = {};
        zstdgpu_Init_InitResources_SRT(srt, zstdCpu);
        srt.allBlockCount       = zstdRawBlockCount + zstdRleBlockCount + zstdCmpBlockCount;
        srt.cmpBlockCount       = zstdCmpBlockCount;
        srt.frameCount          = zstdFrameCount;
        srt.initResourcesStage  = 1; // 1 means -- right before "parse compressed blocks"
        zstdgpu_ShaderEntry_InitResources(srt, 0);

    }
    // Parse Compressed Blocks on CPU with the same code we use on GPU
    {
        zstdgpu_ParseCompressedBlocks_SRT srt;
        zstdgpu_Init_ParseCompressedBlocks_SRT(srt, zstdCpu);

        srt.compressedBufferSizeInBytes = zstdInfo.CompressedData_ByteSize;
        srt.compressedBlockCount        = zstdCmpBlockCount;
        srt.frameCount                  = zstdFrameCount;

        for (uint32_t i = 0; i < zstdCmpBlockCount; ++i)
        {
            zstdgpu_ShaderEntry_ParseCompressedBlocks(srt, i);
        }
        VALIDATE(CompressedBlocksData(&zstdCpu));
    }
    const uint32_t literalCount = CNTRS(HUF_Streams_DecodedBytes);
    const uint32_t sequenceCount = CNTRS(Seq_Streams_DecodedItems);
    zstdgpu_ResourceInfo_Stage_2_Init(&zstdInfo, literalCount, sequenceCount, 0, 0);
    zstdgpu_ResourceDataCpu_InitFromHeap(&zstdCpu, &zstdInfo);

    {
        zstdgpu_InitFseTable_SRT srt;
        zstdgpu_Init_InitFseTable_SRT(srt, zstdCpu);

        srt.tableStartIndex = 0;
        for (uint32_t i = 0; i < CNTRS(FseHufW); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }

        srt.tableStartIndex += zstdCmpBlockCount;
        for (uint32_t i = 0; i < CNTRS(FseLLen); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }

        srt.tableStartIndex += zstdCmpBlockCount + 1;
        for (uint32_t i = 0; i < CNTRS(FseOffs); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }
        srt.tableStartIndex += zstdCmpBlockCount + 1;
        for (uint32_t i = 0; i < CNTRS(FseMLen); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }
        VALIDATE(FseTables(&zstdCpu));
    }

    {
        zstdgpu_DecompressHuffmanWeights_SRT srt;
        zstdgpu_Init_DecompressHuffmanWeights_SRT(srt, zstdCpu);
        for (uint32_t i = 0; i < CNTRS(FseHufW); ++i)
        {
            zstdgpu_ShaderEntry_DecompressHuffmanWeights(srt, i);
        }

        VALIDATE(DecompressedHuffmanWeights(&zstdCpu));
    }

    {
        zstdgpu_DecodeHuffmanWeights_SRT srt;
        zstdgpu_Init_DecodeHuffmanWeights_SRT(srt, zstdCpu);
        srt.compressedBlockCount        = zstdCmpBlockCount;
        srt.compressedBufferSizeInBytes = zstdInfo.CompressedData_ByteSize;
        for (uint32_t i = 0; i < CNTRS(HUF_WgtStreams); ++i)
        {
            zstdgpu_ShaderEntry_DecodeHuffmanWeights(srt, i);
        }
        VALIDATE(DecodedHuffmanWeights(&zstdCpu));
    }

    {
        // NOTE(pamartis): some helper passes that don't have CPU/GPU portability
        uint32_t streamPrefix = 0;
        uint32_t groupPrefix = 0;
        {
            // Compute prefix sum of literal stream count and literal decompression group count per Huffman Table
            for (uint32_t i = 0; i < zstdCmpBlockCount; ++i)
            {
                const uint32_t streamCount = zstdCpu.LitStreamEndPerHuffmanTable[i];
                const uint32_t groupCount = ZSTDGPU_TG_COUNT(streamCount, kzstdgpu_TgSizeX_DecompressLiterals);
                streamPrefix += streamCount;
                groupPrefix += groupCount;
                zstdCpu.LitStreamEndPerHuffmanTable[i] = streamPrefix;
                zstdCpu.LitGroupEndPerHuffmanTable[i] = groupPrefix;
            }

            // Group literals per Huffman Table
            for (uint32_t i = 0; i < CNTRS(HUF_Streams); ++i)
            {
                const zstdgpu_CompressedLiteralHuffmanBucket& bucket = zstdCpu.LitStreamBuckets[i];

                uint32_t groupStart = 0;
                if (bucket.huffmanBucketIndex != 0)
                {
                    groupStart = zstdCpu.LitStreamEndPerHuffmanTable[bucket.huffmanBucketIndex - 1];
                }
                const uint32_t groupOffset = bucket.huffmanBucketOffset;
                zstdCpu.LitStreamRemap[groupStart + groupOffset] = i;
            }
        }

        // Run Literal Decompression
        zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT srt;
        zstdgpu_Init_InitHuffmanTable_And_DecompressLiterals_SRT(srt, zstdCpu);
        srt.huffmanTableSlotCount = zstdCmpBlockCount;
        for (uint32_t groupId = 0; groupId < groupPrefix; ++groupId)
        {
            zstdgpu_ShaderEntry_InitHuffmanTable_And_DecompressLiterals(srt, groupId, 0);
        }
        VALIDATE(DecompressedLiterals(&zstdCpu));
    }

    {
        zstdgpu_DecompressSequences_SRT srt;
        zstdgpu_Init_DecompressSequences_SRT(srt, zstdCpu);
        for (uint32_t i = 0; i < CNTRS(Seq_Streams); ++i)
        {
            zstdgpu_ShaderEntry_DecompressSequences_LdsOutCache(srt, i / kzstdgpu_TgSizeX_DecompressSequences, i % kzstdgpu_TgSizeX_DecompressSequences);
        }

        // NOTE(pamartis): some helper passes that don't have CPU/GPU portability

        // Compute prefix sum of block sizes
        uint32_t blockSizePrefix = 0;
        {
            for (uint32_t i = 0; i < zstdAllBlockCount; ++i)
            {
                const uint32_t blockSize = zstdCpu.BlockSizePrefix[i];
                blockSizePrefix += blockSize;
                zstdCpu.BlockSizePrefix[i] = blockSizePrefix;
            }
        }
    }
    // CPU-side replacement for cross-block "final" offset propagation
    {
        uint32_t offset1 = 0;
        uint32_t offset2 = 0;
        uint32_t offset3 = 0;
        for (uint32_t i = 0; i < zstdFrameCount; )
        {
            const uint32_t seqStreamMinIdx = zstdCpu.PerFrameSeqStreamMinIdx[i];
            uint32_t seqStreamEndIdx = CNTRS(Seq_Streams);

            // 1. skip frames with no sequence streams, until a frame with sequence streams are encountered.
            // 2. remember the minimal index of the sequence stream in that found
            for (++i; i < zstdFrameCount; ++i)
            {
                if (~0u != zstdCpu.PerFrameSeqStreamMinIdx[i])
                {
                    seqStreamEndIdx = zstdCpu.PerFrameSeqStreamMinIdx[i];
                    break;
                }
            }

            for (uint32_t j = seqStreamMinIdx; j < seqStreamEndIdx; ++j)
            {
                if (j == seqStreamMinIdx)
                {
                    offset1 = 1 + 3;
                    offset2 = 4 + 3;
                    offset3 = 8 + 3;
                }

                uint32_t offs1 = zstdCpu.PerSeqStreamFinalOffset1[j];
                uint32_t offs2 = zstdCpu.PerSeqStreamFinalOffset2[j];
                uint32_t offs3 = zstdCpu.PerSeqStreamFinalOffset3[j];

                if (offset1 == 0 || offset2 == 0 || offset3 == 0)
                {
                    ZSTDGPU_ASSERT((offset1 | offset2 | offset3) == 0);
                }
                else
                {
                    zstdgpu_DecodeSeqRepeatOffsetsAndApplyPreviousOffsets(offs1, offs2, offs3, offset1, offset2, offset3);
                    offset1 = offs1;
                    offset2 = offs2;
                    offset3 = offs3;
                }
                zstdCpu.PerSeqStreamFinalOffset1[j] = offs1;
                zstdCpu.PerSeqStreamFinalOffset2[j] = offs2;
                zstdCpu.PerSeqStreamFinalOffset3[j] = offs3;
            }
        }
    }
    // CPU-side "finalisation" pass for offsets. Encoded offsets within the block that are either a) refer to "final" offset of the previous block b) absolute with "+3 bytes" offsets,
    // so we "decode" them into "absolute"
    {
        zstdgpu_FinaliseSequenceOffsets_SRT srt;
        zstdgpu_Init_FinaliseSequenceOffsets_SRT(srt, zstdCpu);
        for (uint32_t i = 0; i < CNTRS(Seq_Streams_DecodedItems); ++i)
        {
            zstdgpu_ShaderEntry_FinaliseSequenceOffsets(srt, i);
        }
    }
    VALIDATE(DecompressedSequences(&zstdCpu));
    #undef CNTRS
}

static void zstdgpu_DefaultUploadCallback(void *zstdCompressedFramesBytes, uint32_t zstdCompressedFramesByteCount, zstdgpu_OffsetAndSize *zstdCompressedFrames, uint32_t zstdCompressedFrameCount, void *uploadUserdata)
{
    void **uploadUserDataExpanded = (void **)uploadUserdata;
    memcpy(zstdCompressedFramesBytes, uploadUserDataExpanded[0], zstdCompressedFramesByteCount);
    memcpy(zstdCompressedFrames, uploadUserDataExpanded[1], zstdCompressedFrameCount * sizeof(zstdCompressedFrames[0]));
}

ZSTDGPU_API void zstdgpu_ReadbackGpuResults(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList* cmdList);
ZSTDGPU_API void zstdgpu_RetrieveGpuResults(zstdgpu_ResourceDataCpu *outGpuResources, zstdgpu_PerRequestContext req);

ZSTDGPU_API void zstdgpu_ReadbackTimestamps(zstdgpu_PerRequestContext req, ID3D12GraphicsCommandList *cmdList);
ZSTDGPU_API void zstdgpu_RetrieveTimestamps(const wchar_t **outTimestampScopeNames, uint64_t *outTimestampScopeClocks, uint32_t *inoutTimestampScopeCnt, zstdgpu_PerRequestContext req, uint32_t stageIndex);

// Entry point
#ifndef _GAMING_XBOX
int wmain(int argc, wchar_t **argv)
#else
int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
#endif
{
#ifdef _GAMING_XBOX
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);
#endif

    bool extMem = false;
    bool chkGpu = false;
    bool chkCpu = false;
    bool simGpu = false;
    bool d3dDbg = false;
    bool d3dGfx = false;

    const wchar_t *zstFilePath = L"data\\group_0_cmp17_block8192.zst";
    wchar_t *zstFilePathStorage = NULL;
    uint32_t gpuVenId = 0x1414; // means -- find any vendor id, but not 0x1414
    uint32_t gpuDevId = ~0u;    // means -- find any device id
    uint32_t repCount = 10;
    uint32_t prfLevel = 0;

#ifndef _GAMING_XBOX
    {
        int argi = 0;
        {
            bool nextZst = false;
            bool nextGpuVenId = false;
            bool nextGpuDevId = false;
            bool nextRepCount = false;
            bool nextPrfLevel = false;
            for (argi = 1; argi < argc; ++argi)
            {
                if (nextZst)
                {
                    nextZst = false;
                    zstFilePathStorage = _wcsdup(argv[argi]);
                    zstFilePath = zstFilePathStorage;
                }
                else if (nextGpuVenId || nextGpuDevId)
                {
                    errno = 0;
                    wchar_t *end = NULL;
                    uint32_t value = (uint32_t)wcstol(argv[argi], &end, 16);
                    if (*end == L'\0' && ERANGE != errno)
                    {
                        if (nextGpuVenId)
                            gpuVenId = value;
                        else if (nextGpuDevId)
                            gpuDevId = value;
                    }
                    nextGpuVenId = false;
                    nextGpuDevId = false;
                }
                else if (nextRepCount || nextPrfLevel)
                {
                    errno = 0;
                    wchar_t *end = NULL;
                    uint32_t value = (uint32_t)wcstol(argv[argi], &end, 10);
                    if (*end == L'\0' && ERANGE != errno)
                    {
                        if (nextRepCount)
                            repCount = value;
                        else if (nextPrfLevel)
                            prfLevel = value;
                    }

                    nextRepCount = false;
                    nextPrfLevel = false;
                }
                else if (0 == wcscmp(argv[argi], L"--chk-gpu"))
                {
                    chkGpu = true;
                }
                else if (0 == wcscmp(argv[argi], L"--chk-cpu"))
                {
                    chkCpu = true;
                }
                else if (0 == wcscmp(argv[argi], L"--sim-gpu"))
                {
                    simGpu = true;
                }
                else if (0 == wcscmp(argv[argi], L"--zst"))
                {
                    nextZst = true;
                }
                else if (0 == wcscmp(argv[argi], L"--gpu-ven-id"))
                {
                    nextGpuVenId = true;
                }
                else if (0 == wcscmp(argv[argi], L"--gpu-dev-id"))
                {
                    nextGpuDevId = true;
                }
                else if (0 == wcscmp(argv[argi], L"--d3d-dbg"))
                {
                    d3dDbg = true;
                }
                else if (0 == wcscmp(argv[argi], L"--d3d-gfx"))
                {
                    d3dGfx = true;
                }
                else if (0 == wcscmp(argv[argi], L"--run-cnt"))
                {
                    nextRepCount = true;
                }
                else if (0 == wcscmp(argv[argi], L"--ext-mem"))
                {
                    extMem = true;
                }
                else if (0 == wcscmp(argv[argi], L"--prf-lvl"))
                {
                    nextPrfLevel = true;
                }
            }
            if (1 == argc)
            {
                debugPrint(L"USAGE:\n");
                debugPrint(L"\t--zst <path to .zst file> [Required] Specifies a file path to .zst file to decompress. Could be absolute or relative path.\n");
                debugPrint(L"\t--chk-gpu                 [Optional] After running decompresssion on GPU, validates its outputs against the outputs from reference decompressor.\n");
                debugPrint(L"\t--chk-cpu                 [Optional] Before running decompression on GPU, runs GPU decompressor code on CPU and validates its outputs against the outputs from reference decompressor.\n");
                debugPrint(L"\t--sim-gpu                 [Optional] After running decompression on GPU, runs key GPU decompressor stages on CPU using intermediate inputs from GPU decompression and validates its outputs against the outputs from reference decompressor.\n");
                debugPrint(L"\t--gpu-ven-id <id (hex)>   [Optional] VendorId (base16) to use when choosing GPU to run on.\n");
                debugPrint(L"\t--gpu-dev-id <id (hex)>   [Optional] DeviceId (base16) to use when choosing GPU to run on.\n");
                debugPrint(L"\t--d3d-dbg                 [Optional] Enables D3D12 debug layer.\n");
                debugPrint(L"\t--d3d-gfx                 [Optional] Enables D3D12 Graphics queue (DIRECT), otherwise COMPUTE (by default).\n");
                debugPrint(L"\t--run-cnt <count>         [Optional] The number of times to repeat the experiment.\n");
                debugPrint(L"\t--ext-mem                 [Optional] Enables external heaps so the library doesn't create them.\n");
                debugPrint(L"\t--prf-lvl <0, 1, 2>       [Optional] Chooses the level of profiling: 0 - overall bandwidth in GB/s, 1 - stage cost, 2 - internal pass cost.\n");
            }
            if (NULL == zstFilePathStorage)
            {
                debugPrint(L"WARN: No '--zst <path to .zst file>' option was specified, running with pre-defined file path: '%s'.\n", zstFilePath);
            }
        }
    }
#endif
    void *zstdData = NULL;
    uint32_t zstdDataSize = 0;
    uint32_t zstdCompressedFramesMemorySizeInBytes = 0;
    uint32_t zstdUnCompressedFramesMemorySizeInBytes = 0;

    loadFileAligned(&zstdData, &zstdDataSize, &zstdCompressedFramesMemorySizeInBytes, 2u, zstFilePath);
    if (NULL == zstdData)
    {
        debugPrint(L"FAIL: Couldn't load '%s'. Early Out.\n", zstFilePath);
        return ERROR_FILE_NOT_FOUND;
    }
    else
    {
        debugPrint(L"Loaded '%s' -- %u bytes.\n", zstFilePath, zstdDataSize);
    }

    zstdgpu_CountFramesAndBlocksInfo fbInfo;
    zstdgpu_CountFramesAndBlocks(&fbInfo, zstdData, zstdCompressedFramesMemorySizeInBytes, zstdDataSize);

    zstdgpu_FrameInfo *zstdFrameInfo = (zstdgpu_FrameInfo *)malloc(sizeof(zstdgpu_FrameInfo) * fbInfo.frameCount);
    zstdgpu_OffsetAndSize *zstdInFrameRefs = (zstdgpu_OffsetAndSize *)malloc(sizeof(zstdgpu_OffsetAndSize) * fbInfo.frameCount);
    zstdgpu_OffsetAndSize *zstdOutFrameRefs = (zstdgpu_OffsetAndSize *)malloc(sizeof(zstdgpu_OffsetAndSize) * fbInfo.frameCount);
    zstdgpu_CollectFrames(zstdInFrameRefs, zstdFrameInfo, fbInfo.frameCount, zstdData, zstdCompressedFramesMemorySizeInBytes, zstdDataSize);

    // NOTE(pamartis): compute offsets of frame in the output data using the decompressed frame sizes.
    // We also align offset to make sure we support writing several non-consecutive frames from zstdgpu
    // Beceause zstd format doesn't guarantee the presense of uncompressed frame size, we check if any
    // all frames have valid size (!= 0) and only then proceed
    {
        uint32_t offs = 0;
        uint32_t vcnt = 0;
        for (uint32_t i = 0; i < fbInfo.frameCount; ++i)
        {
            zstdOutFrameRefs[i].offs = offs;
            zstdOutFrameRefs[i].size = (uint32_t)zstdFrameInfo[i].uncompSize;

            offs += zstdOutFrameRefs[i].size;
            offs =  ZSTDGPU_ALIGN(offs, 256);

            vcnt += zstdFrameInfo[i].uncompSize != 0 ? 1 : 0;
        }
        zstdUnCompressedFramesMemorySizeInBytes = offs;

        if (fbInfo.frameCount != vcnt)
        {
            debugPrint(L"FAIL: Some frames don't carry uncompressed size. Early Out.\n");

            free(zstdOutFrameRefs);
            free(zstdInFrameRefs);
            free(zstdFrameInfo);
            free(zstdData);
            if (NULL != zstFilePathStorage)
                free(zstFilePathStorage);

            return 1;
        }
    }

    static const uint32_t kBackBufferCount = 2;
#ifdef _GAMING_XBOX
    static const uint32_t kFrameInterval = D3D12XBOX_FRAME_INTERVAL_60_HZ;
#else
    static const uint32_t kFrameInterval = 1;
#endif

    ID3D12Device *device = zstdgpu_Demo_PlatformInit(gpuVenId, gpuDevId, d3dDbg);

    d3d12aid_CmdQueue cmdQueue;
#ifdef _GAMING_XBOX
    d3d12aid_CmdQueue_Create(&cmdQueue, device, kBackBufferCount, 1u, D3D12_COMMAND_LIST_TYPE_DIRECT);
#else
    d3d12aid_CmdQueue_Create(&cmdQueue, device, kBackBufferCount, 1u, d3dGfx ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE);
#endif

    #define ZSTDGPU_TS_LIST()   \
        ZSTDGPU_TS(Stage0)      \
        ZSTDGPU_TS(Stage1)      \
        ZSTDGPU_TS(Stage2)      \
        ZSTDGPU_TS(Readback0)   \
        ZSTDGPU_TS(Readback1)

    d3d12aid_Timestamps timestamps;
    d3d12aid_Timestamps_Create(&timestamps, device, 0
        /** NOTE(pamartis): generate timestamp counting using macro list */
        #define ZSTDGPU_TS(name) + 2
            ZSTDGPU_TS_LIST()
        #undef  ZSTDGPU_TS
        , kBackBufferCount
    );

    uint32_t zstdReferenceUncompressedDataSize = 0;
    void *zstdReferenceUncompressedData = NULL;

    if (simGpu)
    {
        chkCpu = true;
        debugPrint(L"[VALIDATION] Option '--sim-gpu' was set. Enabling '--chk-cpu' automatically (required by '--sim-gpu').\n");
    }

    if (chkCpu || chkGpu)
    {
        debugPrint(L"[VALIDATION] Running Reference Decompression and building Reference Uncompressed data ('--chk-cpu' or '--chk-gpu' was set).\n");

        zstdReferenceUncompressedDataSize = (uint32_t)ZSTD_get_decompressed_size(zstdData, zstdDataSize);
        zstdReferenceUncompressedData = malloc(zstdReferenceUncompressedDataSize);

        // TODO: consider "per-file" or "per-frame" state (not just state internal) for validation layer
        zstdgpu_ReferenceStore_Report_ChunkBase(zstdData);
        zstdgpu_ReferenceStore_AllocateMemory();

        // NOTE(pamartis): this call to reference ZSTD decompressor populates zstdgpu_ReferenceStore with ground-truth  data
        ZSTD_decompress(zstdReferenceUncompressedData, zstdReferenceUncompressedDataSize, zstdData, zstdDataSize);
    }

#if 0
    {
        for (uint32_t i = 0; i < fbInfo.frameCount; ++i)
        {
            zstdgpu_CollectBlocks(
                zstdCpu.BlocksRAWRefs,
                zstdCpu.BlocksRLERefs,
                zstdCpu.BlocksCMPRefs,
                zstdCpu.FramesRefs,
                zstdCpu.Frames,
                i,
                fbInfo.frameCount,
                zstdData,
                zstdBufferSize,
                zstdDataSize
            );
        }
    }
#endif

    zstdgpu_ResourceDataCpu zstdCpu;
    if (chkCpu)
    {
        debugPrint(L"[VALIDATION] Running GPU Decompression code on CPU ('--chk-cpu' option was set).\n");
        // NOTE(pamartis): We run GPU Decompression pipeline on CPU to catch possible errors/assert early
        // TODO(pamartis): Because currently we don't know "frame count" and "uncompressed size" we pass that information from fbInfo.
        zstdgpu_Validate_GpuDecompressOnCpu(zstdCpu, zstdData, zstdInFrameRefs, fbInfo.frameCount, zstdDataSize, fbInfo.frameByteCount);
        if (!simGpu)
        {
            zstdgpu_ResourceDataCpu_MarkReadOnly(&zstdCpu);
        }
    }

    debugPrint(L"Initializing 'zstdgpu' Persistent Context.\n");
    zstdgpu_PersistentContext persistentContext = NULL;
    {
        const uint32_t persistentMemorySize = zstdgpu_GetPersistentContextRequiredMemorySizeInBytes();
        zstdgpu_Status status = zstdgpu_CreatePersistentContext(&persistentContext, device, malloc(persistentMemorySize), persistentMemorySize);
        ZSTDGPU_ASSERT(kzstdgpu_StatusSuccess == status);
    }

    debugPrint(L"Initializing 'zstdgpu' PerRequest Context.\n");
    zstdgpu_PerRequestContext perRequestContext = NULL;
    {
        const uint32_t perRequestMemorySize = zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes();
        zstdgpu_Status status = zstdgpu_CreatePerRequestContext(&perRequestContext, persistentContext, malloc(perRequestMemorySize), perRequestMemorySize);
        ZSTDGPU_ASSERT(kzstdgpu_StatusSuccess == status);
    }

    uint32_t stageCount = 0;
    uint32_t testSourceInGpuMemory = 0u;

    d3d12aid_MappedBuffer zstdCompressedFramesMemory;
    d3d12aid_MappedBuffer zstdCompressedFramesRefs;

    d3d12aid_MappedBuffer zstdUnCompressedFramesMemory;
    d3d12aid_MappedBuffer zstdUnCompressedFramesRefs;

    const uint32_t zstdFramesRefsSizeInBytes = sizeof(zstdgpu_OffsetAndSize) * fbInfo.frameCount;

    void* defaultUploadCallbackUserData[2];
    if (testSourceInGpuMemory > 0)
    {
        zstdCompressedFramesMemorySizeInBytes = ZSTDGPU_TG_MULTIPLE(zstdDataSize, 4u);

        d3d12aid_MappedBuffer_Create(&zstdCompressedFramesMemory, device, 1u, zstdCompressedFramesMemorySizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdCompressedFramesRefs, device, 1u, zstdFramesRefsSizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesRefs, device, 1u, zstdFramesRefsSizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesMemory, device, kBackBufferCount, zstdUnCompressedFramesMemorySizeInBytes, D3D12_HEAP_TYPE_READBACK);

        d3d12aid_MappedBuffer_Append(&zstdCompressedFramesMemory, 0, (void *)zstdData, zstdDataSize);
        d3d12aid_MappedBuffer_Append(&zstdCompressedFramesRefs, 0, (void *)zstdInFrameRefs, zstdFramesRefsSizeInBytes);
        d3d12aid_MappedBuffer_Append(&zstdUnCompressedFramesRefs, 0, (void *)zstdOutFrameRefs, zstdFramesRefsSizeInBytes);

        zstdgpu_SetupInputsAsFramesInGpuMemory(&stageCount, perRequestContext, zstdCompressedFramesMemory.bufGpu, zstdCompressedFramesMemorySizeInBytes, zstdCompressedFramesRefs.bufGpu, fbInfo.frameCount);
    }
    else
    {
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesRefs, device, 1u, zstdFramesRefsSizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesMemory, device, kBackBufferCount, zstdUnCompressedFramesMemorySizeInBytes, D3D12_HEAP_TYPE_READBACK);

        d3d12aid_MappedBuffer_Append(&zstdUnCompressedFramesRefs, 0, (void *)zstdOutFrameRefs, zstdFramesRefsSizeInBytes);

        defaultUploadCallbackUserData[0] = (void *)zstdData;
        defaultUploadCallbackUserData[1] = (void *)zstdInFrameRefs;
        zstdgpu_SetupInputsAsFramesInCpuMemory(&stageCount, perRequestContext, fbInfo.frameCount, zstdDataSize, zstdgpu_DefaultUploadCallback, defaultUploadCallbackUserData);
    }
    zstdgpu_SetupOutputs(perRequestContext, zstdUnCompressedFramesMemory.bufGpu, zstdUnCompressedFramesMemorySizeInBytes, zstdUnCompressedFramesRefs.bufGpu, fbInfo.frameCount);

    ID3D12Heap *readbackHeap[3] = { NULL, NULL, NULL };
    ID3D12Heap *uploadHeap[3] = { NULL, NULL, NULL };
    ID3D12Heap *defaultHeap[3] = { NULL, NULL, NULL };
    ID3D12DescriptorHeap *descriptorHeap[3] = { NULL, NULL, NULL };

    uint32_t readbackHeapSize[3] = { 0, 0, 0 };
    uint32_t uploadHeapSize[3] = {0, 0, 0 };
    uint32_t defaultHeapSize[3] = {0, 0, 0};
    uint32_t descriptorCount[3] = { 0, 0, 0 };

    for (uint32_t frameIndex = 0; frameIndex < repCount; ++frameIndex)
    {
        if (zstdgpu_Demo_PlatformTick())
        {
            // Main sample loop
            PIXBeginEvent(PIX_COLOR_DEFAULT, L"Frame %llu", frameIndex);

#ifdef _GAMING_XBOX
            D3D12XBOX_FRAME_PIPELINE_TOKEN frameOriginToken = D3D12XBOX_FRAME_PIPELINE_TOKEN_NULL;
            D3D12AID_CHECK(device->WaitFrameEventX(D3D12XBOX_FRAME_EVENT_ORIGIN, INFINITE, NULL, D3D12XBOX_WAIT_FRAME_EVENT_FLAG_NONE, &frameOriginToken));
#endif
            // Prepare the command list to render a new frame.
            const uint32_t kBackBufferIndex = frameIndex % kBackBufferCount;

            #define ZSTDGPU_TS(name) uint32_t name##_Stamp = ~0u;
                ZSTDGPU_TS_LIST()
            #undef ZSTDGPU_TS


            ID3D12GraphicsCommandList *cmdList = d3d12aid_CmdQueue_StartCmdList(&cmdQueue, 0 /** cmdListId */);

            if (frameIndex == 0)
            {
                if (testSourceInGpuMemory > 0)
                {
                    d3d12aid_MappedBuffer_Transfer(cmdList, &zstdCompressedFramesMemory, 0);
                    d3d12aid_MappedBuffer_Transfer(cmdList, &zstdCompressedFramesRefs, 0);
                }
                d3d12aid_MappedBuffer_Transfer(cmdList, &zstdUnCompressedFramesRefs, 0);

                D3D12_RESOURCE_BARRIER barriers[3];
                uint32_t bufferCount = 0;
                if (testSourceInGpuMemory > 0)
                {
                    d3d12aid_MappedBuffer_EndTransfer(&barriers[bufferCount ++], &zstdCompressedFramesMemory, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    d3d12aid_MappedBuffer_EndTransfer(&barriers[bufferCount ++], &zstdCompressedFramesRefs, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                d3d12aid_MappedBuffer_EndTransfer(&barriers[bufferCount ++], &zstdUnCompressedFramesRefs, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                cmdList->ResourceBarrier(bufferCount, barriers);
            }

            // This example shows simplified case:
            //      1. There's only one `zstdgpu_PerRequestContext`
            //      2. Because of 1. -- there's no other contexts to overlap with, so we wait for GPU Idle after every submission
            //
            // What also possible to do  -- is to create several `zstdgpu_PerRequestContext`, for example, when requests to decompress
            // some zstd frames arrive, the user of the library can accumulate them and then use single `zstdgpu_PerRequestContext` to decompress
            // when sufficient number of zstd frames are accumulated. In such scenario when multiple instances `zstdgpu_PerRequestContext`
            // exist -- it's possible to overlap stages any number of `zstdgpu_PerRequestContext`, for example:
            //      [Submission   0] [Wait for Idle]  [Submission     1] [Wait for Idle]  [Submission     2] [Wait for Idle]  [Submission     3]
            //      [Stage2 - Req T]                  [Stage0 - Req T+1]                  [Stage1 - Req T+1]                  [Stage2 - Req T+1]
            //      [Stage0 - Req K]                  [Stage1 - Req K  ]                  [Stage2 - Req K  ]                  [Stage0 - Req K+1]
            //      [Stage1 - Req J]                  [Stage2 - Req J  ]                  [Stage0 - Req J+1]                  [Stage1 - Req J+1]
            //
            {
                if (extMem)
                {
                    uint32_t *const StageTimestamp [3] = { &Stage0_Stamp, &Stage1_Stamp, &Stage2_Stamp };
                    uint32_t *const ReadbackTimestamp[2] = { &Readback0_Stamp, &Readback1_Stamp };
                    for (uint32_t stageIndex = 0; stageIndex < 3; ++stageIndex)
                    {
                        uint32_t defaultHeapSizeReq = 0;
                        uint32_t uploadHeapSizeReq = 0;
                        uint32_t readbackHeapSizeReq = 0;
                        uint32_t descriptorCountReq = 0;

                        zstdgpu_GetGpuMemoryRequirement(&defaultHeapSizeReq, &uploadHeapSizeReq, &readbackHeapSizeReq, &descriptorCountReq, perRequestContext, stageIndex);

                        if (defaultHeapSizeReq > defaultHeapSize[stageIndex])
                        {
                            D3D12AID_SAFE_RELEASE(defaultHeap[stageIndex]);

                            defaultHeap[stageIndex] = d3d12aid_Heap_Create(device, defaultHeapSizeReq, 0, D3D12_HEAP_TYPE_DEFAULT);
                            defaultHeapSize[stageIndex] = defaultHeapSizeReq;
                        }

                        if (uploadHeapSizeReq > uploadHeapSize[stageIndex])
                        {
                            D3D12AID_SAFE_RELEASE(uploadHeap[stageIndex]);

                            uploadHeap[stageIndex] = d3d12aid_Heap_Create(device, uploadHeapSizeReq, 0, D3D12_HEAP_TYPE_UPLOAD);
                            uploadHeapSize[stageIndex] = uploadHeapSizeReq;
                        }

                        if (readbackHeapSizeReq > readbackHeapSize[stageIndex])
                        {
                            D3D12AID_SAFE_RELEASE(readbackHeap[stageIndex]);

                            readbackHeap[stageIndex] = d3d12aid_Heap_Create(device, readbackHeapSizeReq, 0, D3D12_HEAP_TYPE_READBACK);
                            readbackHeapSize[stageIndex] = readbackHeapSizeReq;
                        }

                        if (descriptorCountReq > descriptorCount[stageIndex])
                        {
                            D3D12AID_SAFE_RELEASE(descriptorHeap[stageIndex]);
                            descriptorHeap[stageIndex] =  d3d12aid_DescriptorHeap_Create(device, descriptorCountReq, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
                            descriptorCount[stageIndex] = descriptorCountReq;
                        }

                        d3d12aid_Timestamp_PushScope(*StageTimestamp[stageIndex], timestamps, cmdList,
                            zstdgpu_SubmitWithExternalMemory(perRequestContext, stageIndex, cmdList, defaultHeap[stageIndex], 0, uploadHeap[stageIndex], 0, readbackHeap[stageIndex], 0, descriptorHeap[stageIndex], 0);
                        );

                        if (stageIndex < 2)
                        {
                            d3d12aid_Timestamp_PushScope(*ReadbackTimestamp[stageIndex], timestamps, cmdList,
                                d3d12aid_CmdQueue_SubmitCmdList(&cmdQueue, 0);
                                d3d12aid_CmdQueue_CpuWaitForGpuIdle(&cmdQueue);
                                cmdList = d3d12aid_CmdQueue_StartCmdList(&cmdQueue, 0/** cmdListId */);
                            );
                        }
                    }
                }
                else
                {
                    d3d12aid_Timestamp_PushScope(Stage0_Stamp, timestamps, cmdList,
                        zstdgpu_SubmitWithInteralMemory(perRequestContext, 0, cmdList);
                    );

                    d3d12aid_Timestamp_PushScope(Readback0_Stamp, timestamps, cmdList,
                        d3d12aid_CmdQueue_SubmitCmdList(&cmdQueue, 0);
                        d3d12aid_CmdQueue_CpuWaitForGpuIdle(&cmdQueue);
                        cmdList = d3d12aid_CmdQueue_StartCmdList(&cmdQueue, 0/** cmdListId */);
                    );

                    d3d12aid_Timestamp_PushScope(Stage1_Stamp, timestamps, cmdList,
                        zstdgpu_SubmitWithInteralMemory(perRequestContext, 1, cmdList);
                    );

                    d3d12aid_Timestamp_PushScope(Readback1_Stamp, timestamps, cmdList,
                        d3d12aid_CmdQueue_SubmitCmdList(&cmdQueue, 0);
                        d3d12aid_CmdQueue_CpuWaitForGpuIdle(&cmdQueue);
                        cmdList = d3d12aid_CmdQueue_StartCmdList(&cmdQueue, 0/** cmdListId */);
                    );

                    d3d12aid_Timestamp_PushScope(Stage2_Stamp, timestamps, cmdList,
                        zstdgpu_SubmitWithInteralMemory(perRequestContext, 2, cmdList);
                    );
                }
                {
                    D3D12_RESOURCE_BARRIER barrier;
                    d3d12aid_MappedBuffer_BeginTransfer(&barrier, &zstdUnCompressedFramesMemory, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    cmdList->ResourceBarrier(1u, &barrier);

                    d3d12aid_MappedBuffer_Transfer(cmdList, &zstdUnCompressedFramesMemory, 0 /** works only when submissions aren't overlapped*/);
                }
                zstdgpu_ReadbackTimestamps(perRequestContext, cmdList);
                if (simGpu || chkGpu)
                {
                    zstdgpu_ReadbackGpuResults(perRequestContext, cmdList);
                }
                d3d12aid_Timestamps_AdvanceFrame(&timestamps, cmdList);
                d3d12aid_CmdQueue_SubmitCmdList(&cmdQueue, 0);
                d3d12aid_CmdQueue_CpuWaitForGpuIdle(&cmdQueue);

                if (simGpu || chkGpu)
                {
                    zstdgpu_ResourceDataCpu gpuData;
                    zstdgpu_RetrieveGpuResults(&gpuData, perRequestContext);

                    if (chkGpu)
                    {
                        zstdgpu_ReferenceStore_Validate_CompressedBlocksData(&gpuData);
                    }

                    zstdgpu_Test_DecompressHuffmanWeights(zstdCpu, gpuData, zstdCompressedFramesMemorySizeInBytes, chkGpu, simGpu);
                    zstdgpu_Test_DecompressLiterals(zstdCpu, gpuData, zstdCompressedFramesMemorySizeInBytes, chkGpu, simGpu);
                    zstdgpu_Test_DecompressSequences(zstdCpu, gpuData, zstdCompressedFramesMemorySizeInBytes, chkGpu, simGpu);
                }
                if (chkGpu)
                {
                    uint32_t refOffs = 0;
                    for (uint32_t i = 0; i < fbInfo.frameCount; ++i)
                    {
                        const char *ref = (char*)zstdReferenceUncompressedData + refOffs;
                        const char *tst = (char*)zstdUnCompressedFramesMemory.bufMem[0] + zstdOutFrameRefs[i].offs;
                        int result = memcmp(tst, ref, zstdOutFrameRefs[i].size);
                        refOffs += zstdOutFrameRefs[i].size;

                        if (0 != result)
                        {
                            debugPrint(L"[VALIDATION] Failed to verify decompressed 'frame %u' against decompressed frame from reference decompressor\n", i);
                            for (uint32_t j = 0; j < zstdOutFrameRefs[i].size; ++j)
                            {
                                if (tst[j] != ref[j])
                                {
                                    debugPrint(L"[VALIDATION] The first byte failing validation is '%u'\n", j);
                                    break;
                                }
                            }
                        }
                    }
                }

                if (frameIndex == 0)
                {
#ifdef _MSC_VER
                    __pragma(warning(push))
                    __pragma(warning(disable : 4996))
#endif
                    const int bufferSize = _snwprintf(NULL, 0, L"%s.frame_%u", zstFilePath, fbInfo.frameCount) + 1;
                    wchar_t *buffer = (wchar_t *)malloc(bufferSize * sizeof(wchar_t));
                    for (uint32_t i = 0; i < fbInfo.frameCount; ++i)
                    {
                        _snwprintf(buffer, bufferSize, L"%s.frame_%u", zstFilePath, i);
                        saveFile(buffer, (char*)zstdUnCompressedFramesMemory.bufMem[0] + zstdOutFrameRefs[i].offs, zstdOutFrameRefs[i].size);
                    }
#ifdef _MSC_VER
                    __pragma(warning(pop))
#endif
                    free(buffer);
                }

                static uint64_t freqGpuClocks = 0;
                if (freqGpuClocks == 0)
                {
                    cmdQueue.queue->GetTimestampFrequency(&freqGpuClocks);
                }
                const wchar_t *timestampScopeNames[16];
                uint64_t timestampScopeClocks[16];
                uint64_t clks = 0;
                #define ZSTDGPU_TS(name)                                                                        \
                    if (name##_Stamp != ~0u && prfLevel > 0)                                                    \
                    {                                                                                           \
                        clks = d3d12aid_Timestamps_GetScopeDelta(&timestamps, kBackBufferIndex, name##_Stamp);  \
                        const uint64_t usec = (clks * 1000000) / freqGpuClocks;                                 \
                        debugPrint(L"%u frame: %7llu us - '" #name "' \n", frameIndex, usec);                  \
                    }

                #define ZSTDGPU_DETAIL_TS(stage) \
                    if (prfLevel > 1)            \
                    {                            \
                        uint32_t timestampScopeCount = _countof(timestampScopeClocks);                                                                              \
                        zstdgpu_RetrieveTimestamps(timestampScopeNames, timestampScopeClocks, &timestampScopeCount, perRequestContext, stage);                      \
                        for (uint32_t i = 0; i < timestampScopeCount; ++i)                                                                                          \
                        {                                                                                                                                           \
                            debugPrint(L"%u frame: \t%7llu us - 'Stage"#stage" :: %s'\n", frameIndex,  (timestampScopeClocks[i] * 1000000) / freqGpuClocks, timestampScopeNames[i]);  \
                        }                                                                                                                                           \
                    }
                    ZSTDGPU_TS(Stage0)
                    ZSTDGPU_DETAIL_TS(0)
                    ZSTDGPU_TS(Readback0)
                    ZSTDGPU_TS(Stage1)
                    ZSTDGPU_DETAIL_TS(1)
                    ZSTDGPU_TS(Readback1)
                    ZSTDGPU_TS(Stage2)
                    ZSTDGPU_DETAIL_TS(2)
                    if (prfLevel == 0)
                    {
                        clks = d3d12aid_Timestamps_GetDelta(&timestamps, kBackBufferIndex, Stage0_Stamp, Stage2_Stamp + 1);
                        const uint64_t ns = (clks * 1000000000) / freqGpuClocks;
                        const double decompressionThroughput = (double)zstdUnCompressedFramesMemorySizeInBytes / ns;
                        debugPrint(L"%u frame: Decompression throughput %lf (GB/s)\n", frameIndex, decompressionThroughput);
                    }
                #undef ZSTDGPU_DETAIL_TS
                #undef ZSTDGPU_TS
            }

            // Show the new frame.
            PIXBeginEvent(PIX_COLOR_DEFAULT, L"PresentX");
#ifdef _GAMING_XBOX
            /** Present the backbuffer using the PresentX API. */
            D3D12XBOX_PRESENT_PLANE_PARAMETERS planeParameters = {};
            planeParameters.Token           = frameOriginToken;
            planeParameters.ResourceCount   = 0;
            planeParameters.ppResources     = NULL;

            D3D12AID_CHECK(cmdQueue.queue->PresentX(1, &planeParameters, NULL));

            /** uint64_t presentFence = */d3d12aid_CmdQueue_GpuSignal(&cmdQueue);
#endif
            PIXEndEvent();

            PIXEndEvent();
        }
    }

    d3d12aid_CmdQueue_CpuWaitForGpuIdle(&cmdQueue);
    {
        void *memory = NULL;
        zstdgpu_Status status = zstdgpu_DestroyPerRequestContext(&memory, NULL, perRequestContext);
        ZSTDGPU_ASSERT(kzstdgpu_StatusSuccess == status);
        free(memory);
    }

    {
        void *memory = NULL;
        zstdgpu_Status status = zstdgpu_DestroyPersistentContext(&memory, NULL, persistentContext);
        ZSTDGPU_ASSERT(kzstdgpu_StatusSuccess == status);
        free(memory);
    }

    if (extMem)
    {
        for (uint32_t i = 0; i < 3; ++i)
        {
            D3D12AID_SAFE_RELEASE(descriptorHeap[i]);
            D3D12AID_SAFE_RELEASE(readbackHeap[i]);
            D3D12AID_SAFE_RELEASE(uploadHeap[i]);
            D3D12AID_SAFE_RELEASE(defaultHeap[i]);
        }
    }

    if (testSourceInGpuMemory > 0)
    {
        d3d12aid_MappedBuffer_Release(&zstdCompressedFramesRefs);
        d3d12aid_MappedBuffer_Release(&zstdCompressedFramesMemory);
    }
    d3d12aid_MappedBuffer_Release(&zstdUnCompressedFramesMemory);
    d3d12aid_MappedBuffer_Release(&zstdUnCompressedFramesRefs);

    d3d12aid_Timestamps_Release(&timestamps);
    d3d12aid_CmdQueue_Release(&cmdQueue);

    if (chkCpu)
    {
        zstdgpu_ResourceDataCpu_Term(&zstdCpu);
    }
    if (NULL != zstdReferenceUncompressedData)
        free(zstdReferenceUncompressedData);
    free(zstdOutFrameRefs);
    free(zstdInFrameRefs);
    free(zstdFrameInfo);
    free(zstdData);
    if (NULL != zstFilePathStorage)
        free(zstFilePathStorage);

    zstdgpu_Demo_PlatformTerm(device);
    debugPrint(L"Finished.\n");
    return 0;
}
