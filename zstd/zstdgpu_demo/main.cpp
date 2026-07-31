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
#include <time.h>

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

#include "zstdgpu_assert.h"

#define D3D12AID_CHECK(call)                            \
    do                                                  \
    {                                                   \
        HRESULT hr = call;                              \
        ZSTDGPU_ASSERT_MSG(S_OK == hr, "S_OK != 0x%08lx " #call "\n", hr); \
    }                                                   \
    while(0)

#define D3D12AID_ASSERT(cond) ZSTDGPU_ASSERT(cond)

#define D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT 3
#define D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT 1

#include <d3d12aid.h>
#include <pix3.h>

extern "C"
{
    #include "zstd_decompress.h"
}

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
    fflush(stdout);
    OutputDebugStringW(buffer);
    va_end(args);
}

static void loadFileAligned(void **outData, uint32_t *outDataSize, uint32_t *outBufferSize, uint32_t alignmentLog2, const wchar_t* fileName)
{
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
        if (-1 != dataSize)
        {
            bufferSize = zstdgpu_AlignUp((uint32_t)dataSize, 1u << alignmentLog2);
            data = malloc(bufferSize);
            ZSTDGPU_ASSERT(NULL != data);
            if (NULL != data)
            {
                size_t readSize = fread(data, 1, dataSize, file);
                ZSTDGPU_ASSERT(readSize == dataSize);
                if(bufferSize > dataSize)
                {
                    memset((char*)data + dataSize, 0, bufferSize - dataSize);
                }
            }
        }
        fclose(file);
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

#define ZSTDGPU_RO_RAW_BUFFER_DECL(type, name, index)                               srt.in##name                = resources.name;

#define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                                   srt.in##name                = resources.name;
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                                   srt.inout##name             = resources.name;
#define ZSTDGPU_RW_BUFFER_DECL_GLC(type, name, index)                               srt.inout##name             = resources.name;

#define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  srt.in##name                = resources.name;
#define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  srt.inout##name             = resources.name;
#define ZSTDGPU_RW_TYPED_BUFFER_DECL_GLC(hlsl_type, type, name, index)              srt.inout##name             = resources.name;

#define ZSTDGPU_RO_BUFFER_ALIAS_DECL(type, name, alias, index)                      srt.in##name##_##alias      = resources.name;
#define ZSTDGPU_RW_BUFFER_ALIAS_DECL(type, name, alias, index)                      srt.inout##name##_##alias   = resources.name;
#define ZSTDGPU_RW_BUFFER_ALIAS_DECL_GLC(type, name, alias, index)                  srt.inout##name##_##alias   = resources.name;

#define ZSTDGPU_RO_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     srt.in##name##_##alias      = resources.name;
#define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     srt.inout##name##_##alias   = resources.name;
#define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL_GLC(hlsl_type, type, name, alias, index) srt.inout##name##_##alias   = resources.name;

static void zstdgpu_Init_InitResources_SRT(zstdgpu_InitResources_SRT & srt, zstdgpu_ResourceDataCpu & resources)
{
    ZSTDGPU_INIT_RESOURCES_SRT();
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

#include "zstdgpu_srt_decl_undef.h"

#define VALIDATE(name, data) ZSTDGPU_ASSERT(ZSTDGPU_ENUM_CONST(Validate_Success) == zstdgpu_ReferenceStore_Validate_##name(data))


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
        VALIDATE(DecompressedHuffmanWeights, &gpuReadbackRes);
        VALIDATE(DecodedHuffmanWeights, &gpuReadbackRes);
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
        for (uint32_t i = 0; i < gpuReadbackRes.Counters->FseHufW; ++i)
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
            VALIDATE(DecompressedHuffmanWeights, &gpuReadbackRes);
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
        srt.compressedBufferSizeInBytes         = zstdDataBufferSize;
        for (uint32_t i = 0; i < gpuReadbackRes.Counters->HUF_WgtStreams; ++i)
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
            VALIDATE(DecodedHuffmanWeights, &gpuReadbackRes);
            gpuReadbackRes.CompressedData                   = CompressedData;
            gpuReadbackRes.DecompressedHuffmanWeights       = DecompressedHuffmanWeights;
        }
    }
}

static uint32_t zstdgpu_HufLitStreamCountToGroupCount(zstdgpu_ResourceDataCpu & zstdCpu, uint32_t hufLitCount, uint32_t hufLitStreamCountTotal, uint32_t zstdCmpBlockCount)
{
    uint32_t groupPrefix = 0;
    for (uint32_t i = 0; i < zstdCmpBlockCount; ++i)
    {
        const uint32_t hufLitId = zstdCpu.HufWIdToHufLitId[i];
        uint32_t hufLitStreamCount = 0;
        if (~0u != hufLitId)
        {
            const uint32_t hufLitStreamStart = zstdCpu.HufLitIdToLitStreamId[hufLitId];

            const uint32_t hufLitStreamEnd   = (hufLitId + 1u < hufLitCount)
                                             ? zstdCpu.HufLitIdToLitStreamId[hufLitId + 1u]
                                             : hufLitStreamCountTotal;
            hufLitStreamCount = hufLitStreamEnd - hufLitStreamStart;
        }
        const uint32_t groupCount = hufLitStreamCount; // streamsPerGroup == 1 on CPU
        groupPrefix += groupCount;
        zstdCpu.LitGroupEndPerHuffmanTable[i] = groupPrefix;
    }
    return groupPrefix;
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
        VALIDATE(DecompressedLiterals, &gpuReadbackRes);
        gpuReadbackRes.CompressedData = tmp;
    }

    if (simGpu)
    {
        // NOTE(pamartis): When GPU output data is potentially broken, compute it on CPU (to debug) using same inputs as on GPU
        zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT srt;
        zstdgpu_Init_InitHuffmanTable_And_DecompressLiterals_SRT(srt, gpuReadbackRes);
        srt.inCompressedData            = cpuRes.CompressedData;
        srt.inoutDecompressedLiterals   = cpuRes.DecompressedLiterals;
        const uint32_t htSlotCount = gpuReadbackRes.Counters->Blocks_CMP;
        const uint32_t hufLitCount = gpuReadbackRes.Counters->HufLit;
        const uint32_t hufLitStreamCountTotal = gpuReadbackRes.Counters->HUF_Streams;

        uint32_t *tmpLitGroupEndPerHuffmanTable = gpuReadbackRes.LitGroupEndPerHuffmanTable;

        // NOTE(pamartis): Because we need to re-run [Decompress Literals] on CPU with tgSize == 1,
        // we need to recompute `srt.inLitGroupEndPerHuffmanTable`, so we re-use cpuRes buffer to  replace GPU buffer
        // temporally
        gpuReadbackRes.LitGroupEndPerHuffmanTable = cpuRes.LitGroupEndPerHuffmanTable;
        const uint32_t groupCount = zstdgpu_HufLitStreamCountToGroupCount(gpuReadbackRes, hufLitCount, hufLitStreamCountTotal, htSlotCount);

        // NOTE(pamartis): also replace `srt.inLitGroupEndPerHuffmanTable` in SRT
        srt.inLitGroupEndPerHuffmanTable = cpuRes.LitGroupEndPerHuffmanTable;

        for (uint32_t groupId = 0; groupId < groupCount; ++groupId)
        {
            zstdgpu_ShaderEntry_InitHuffmanTable_And_DecompressLiterals(srt, groupId, 0, 1);
        }
        gpuReadbackRes.LitGroupEndPerHuffmanTable = tmpLitGroupEndPerHuffmanTable;

        if (chkGpu)
        {
            // NOTE(pamartis): After CPU data is computed, validate it against reference, and if it's broken, likely the inputs are wrong
            // NOTE(pamartis): Make sure that validation against reference see the same blocks as when validating GPU data
            uint32_t *CompressedData            = gpuReadbackRes.CompressedData;
            uint8_t *DecompressedLiterals       = gpuReadbackRes.DecompressedLiterals;

            gpuReadbackRes.CompressedData       = cpuRes.CompressedData;
            gpuReadbackRes.DecompressedLiterals = cpuRes.DecompressedLiterals;
            VALIDATE(DecompressedLiterals, &gpuReadbackRes);
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
        VALIDATE(DecompressedSequences, &gpuReadbackRes);
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

            // FIXUP(pamartis): because DecompreSequences requres 'BlockSizePrefix' to contain literal size (on GPU actual prefix is computed later)
            // we need to setup it properly
            for (uint32_t i = 0; i < cpuRes.Counters->Blocks_CMP; ++i)
            {
                const uint32_t literalSize = zstdgpu_DecodeLitSize(cpuRes.CompressedBlocks[i].literal.size);
                const uint32_t dstBlockIndex = cpuRes.GlobalBlockIndexPerCmpBlock[i];
                cpuRes.BlockSizePrefix[dstBlockIndex] = literalSize;
            }
            for (uint32_t i = 0; i < cpuRes.Counters->Blocks_RAW; ++i)
            {
                const uint32_t dstBlockIndex = cpuRes.GlobalBlockIndexPerRawBlock[i];
                cpuRes.BlockSizePrefix[dstBlockIndex] = cpuRes.BlocksRAWRefs[i].size;;
            }
            for (uint32_t i = 0; i < cpuRes.Counters->Blocks_RLE; ++i)
            {
                const uint32_t dstBlockIndex = cpuRes.GlobalBlockIndexPerRleBlock[i];
                cpuRes.BlockSizePrefix[dstBlockIndex] = cpuRes.BlocksRLERefs[i].size;;
            }

            for (uint32_t i = 0; i < gpuReadbackRes.Counters->Seq_Streams; ++i)
            {
                zstdgpu_ShaderEntry_DecompressSequences_MultiStream(srt, /* groupId */ i, /* threadId */ 0, /* streamsPerGroup */ 1);
            }
            // Compute prefix sum of block sizes
            const uint32_t allBlockCount = cpuRes.Counters->Blocks_CMP
                                         + cpuRes.Counters->Blocks_RAW
                                         + cpuRes.Counters->Blocks_RLE;

            // FIXUP(pamartis): because after `DecompreSequences` execution 'BlockSizePrefix' contain actual size of the block,
            // not the prefix (it's computed after `DecompreSequences`  on GPU) we update the prefix manually
            uint32_t blockSizePrefix = 0;
            {
                for (uint32_t i = 0; i < allBlockCount; ++i)
                {
                    const uint32_t blockSize = cpuRes.BlockSizePrefix[i];
                    blockSizePrefix += blockSize;
                    cpuRes.BlockSizePrefix[i] = blockSizePrefix;
                }
            }
        }

        {
            zstdgpu_FinaliseSequenceOffsets_SRT srt;
            zstdgpu_Init_FinaliseSequenceOffsets_SRT(srt, gpuReadbackRes);
            srt.inoutDecompressedSequenceOffs = cpuRes.DecompressedSequenceOffs;
            for (uint32_t i = 0; i < gpuReadbackRes.Counters->Seq_Streams_DecodedItems; ++i)
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

            VALIDATE(DecompressedSequences, &gpuReadbackRes);

            gpuReadbackRes.CompressedData           = CompressedData;
            gpuReadbackRes.DecompressedSequenceLLen = DecompressedSequenceLLen;
            gpuReadbackRes.DecompressedSequenceMLen = DecompressedSequenceMLen;
            gpuReadbackRes.DecompressedSequenceOffs = DecompressedSequenceOffs;
        }

    }
}

static void zstdgpu_Test_BlockPrefix(zstdgpu_ResourceDataCpu & cpuRes, zstdgpu_ResourceDataCpu & gpuReadbackRes)
{
    const uint32_t refRleBlockCount = cpuRes.Counters->Blocks_RLE;
    const uint32_t refRawBlockCount = cpuRes.Counters->Blocks_RAW;
    const uint32_t refCmpBlockCount = cpuRes.Counters->Blocks_CMP;
    const uint32_t refAllBlockCount = refRleBlockCount
                                    + refRawBlockCount
                                    + refCmpBlockCount;

    ZSTDGPU_ASSERT_MSG(refRleBlockCount == gpuReadbackRes.Counters->Blocks_RLE, "%u != %u", refRleBlockCount, gpuReadbackRes.Counters->Blocks_RLE);
    ZSTDGPU_ASSERT_MSG(refRawBlockCount == gpuReadbackRes.Counters->Blocks_RAW, "%u != %u", refRawBlockCount, gpuReadbackRes.Counters->Blocks_RAW);
    ZSTDGPU_ASSERT_MSG(refCmpBlockCount == gpuReadbackRes.Counters->Blocks_CMP, "%u != %u", refCmpBlockCount, gpuReadbackRes.Counters->Blocks_CMP);

    #define CHK(name) 0 == memcmp(cpuRes.GlobalBlockIndexPer##name##Block, gpuReadbackRes.GlobalBlockIndexPer##name##Block, sizeof(cpuRes.GlobalBlockIndexPer##name##Block[0]) * ref##name##BlockCount)

    if (refRleBlockCount == gpuReadbackRes.Counters->Blocks_RLE)
        ZSTDGPU_ASSERT_MSG(CHK(Rle), "Global block indices for Rle blocks don't match between CPU and GPU");

    if (refRawBlockCount == gpuReadbackRes.Counters->Blocks_RAW)
        ZSTDGPU_ASSERT_MSG(CHK(Raw), "Global block indices for Raw blocks don't match between CPU and GPU");

    if (refCmpBlockCount == gpuReadbackRes.Counters->Blocks_CMP)
        ZSTDGPU_ASSERT_MSG(CHK(Cmp), "Global block indices for Cmp blocks don't match between CPU and GPU");

    #undef CHK

    ZSTDGPU_ASSERT(0 == memcmp(cpuRes.BlockSizePrefix, gpuReadbackRes.BlockSizePrefix, sizeof(cpuRes.BlockSizePrefix[0]) * refAllBlockCount));
}

static uint32_t zstdgpu_Test_DecompressedDataPerBlockType(const uint32_t *gpuGlobalBlockIndex,
                                                          const uint32_t blockCount,
                                                          const uint32_t *gpuPerFrameBlockCountPrefix,
                                                          const zstdgpu_OffsetAndSize *cpuPerFrameOffsAndSize,
                                                          const uint32_t frameCount,
                                                          const uint32_t *gpuBlockSizePrefix,
                                                          const void *refData,
                                                          const void *tstData)
{
    uint32_t failedBlockCount = 0;
    for (uint32_t i = 0; i < blockCount; ++i)
    {
        const uint32_t globalBlockIdx = gpuGlobalBlockIndex[i];

        // We start by finding the right frame index for currently processed block
        const uint32_t frameIndex = zstdgpu_BinarySearch(gpuPerFrameBlockCountPrefix, 0, frameCount, globalBlockIdx);

        // and get the index of the first block in this frame
        const uint32_t firstInFrameGlobalBlockIndex = gpuPerFrameBlockCountPrefix[frameIndex];

        // knowing the index of the first block in the frame, we read its offset (tight one, non-aligned or anything)
        uint32_t firstInFrameBlockDataBeg = 0;
        if (firstInFrameGlobalBlockIndex > 0)
            firstInFrameBlockDataBeg = gpuBlockSizePrefix[firstInFrameGlobalBlockIndex - 1];

        // we read the data start and end for a given block
        const uint32_t refDataBlockDataEnd = gpuBlockSizePrefix[globalBlockIdx];
        const uint32_t refDataBlockDataBeg = globalBlockIdx == 0 ? 0 : gpuBlockSizePrefix[globalBlockIdx - 1];

        // rebase those offset into new offset (provided by CPU-side meta buffer), so data offset become relative not relative to start of the the first block)
        const uint32_t tstDataBlockDataBeg = cpuPerFrameOffsAndSize[frameIndex].offs + (refDataBlockDataBeg - firstInFrameBlockDataBeg);
        const uint32_t tstDataBlockDataEnd = cpuPerFrameOffsAndSize[frameIndex].offs + (refDataBlockDataEnd - firstInFrameBlockDataBeg);

        const uint32_t size = refDataBlockDataEnd - refDataBlockDataBeg;
        ZSTDGPU_ASSERT(tstDataBlockDataEnd <= cpuPerFrameOffsAndSize[frameIndex].offs + cpuPerFrameOffsAndSize[frameIndex].size);

        failedBlockCount += (0 != memcmp((char*)refData + refDataBlockDataBeg, (char*)tstData + tstDataBlockDataBeg, size));
    }
    return failedBlockCount;
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
    for (uint32_t i = 0; i < zstdFrameCount; ++i)
    {
        memcpy((char *)zstdCpu.CompressedData + zstdFrameRefs[i].offs, (char *)zstdGpuCompressedData + zstdFrameRefs[i].offs, zstdFrameRefs[i].size);
    }
    memcpy(zstdCpu.FramesRefs, zstdFrameRefs, sizeof(zstdgpu_OffsetAndSize) * zstdFrameCount);
    memcpy(zstdCpu.FseProbsDefault, kzstdgpuFseProbsDefault, sizeof(kzstdgpuFseProbsDefault));

    #define CNTRS(name) zstdCpu.Counters->name
    {
        zstdgpu_InitResources_SRT srt = {};
        zstdgpu_Init_InitResources_SRT(srt, zstdCpu);
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

    // NOTE(pamartis):On CPU, lookback regions for PerFrameBlockCount{RAW,RLE,CMP,All} and
    // PerFrameBlockSizes{RAW,RLE} do NOT need zeroing because prefix sums are computed sequentially
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
    VALIDATE(Blocks, &zstdCpu);

    {
        zstdgpu_InitResources_SRT srt = {};
        zstdgpu_Init_InitResources_SRT(srt, zstdCpu);
        srt.initResourcesStage  = 1; // 1 means -- right before "parse compressed blocks"
        zstdgpu_ShaderEntry_InitResources(srt, 0);

    }
    // NOTE(pamartis): On GPU, HufWIdToHufLitId is initialized to ~0u by a direct Memset dispatch.
    // The meaning of ~0 is -- no HufLitStreams were assigned to a Huffman table with a given index.
    // [Compute Prefix Sum] skips such slots.
    {
        memset(zstdCpu.HufWIdToHufLitId, 0xFF, zstdCmpBlockCount * sizeof(uint32_t));
    }
    // NOTE(pamartis): On GPU, PerFrameSeqStreamMinIdx is initialized to ~0u by a direct Memset dispatch.
    // On CPU, we fill explicitly. Each entry stores the minimum compressed block index with non-zero
    // sequence count per frame
    {
        memset(zstdCpu.PerFrameSeqStreamMinIdx, 0xFF, zstdFrameCount * sizeof(uint32_t));
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
    }
    {
        // CPU equivalent of [Propagate FSE Index] dispatches: propagate LLen / Offs / MLen
        // FSE table indices across all sequence streams. The GPU lookback buffers are zeroed
        // by Memset at the start of every request, so the CPU side mirrors that by using
        // plain (non-static) locals initialised to `Unused` here -- using `static` would leak
        // state across multiple decompressions in the same process.
        {
            uint32_t cpuLastLLenIndex = kzstdgpu_FseProbTableIndex_Unused;
            uint32_t cpuLastOffsIndex = kzstdgpu_FseProbTableIndex_Unused;
            uint32_t cpuLastMLenIndex = kzstdgpu_FseProbTableIndex_Unused;

            const uint32_t seqStreamCount = CNTRS(Seq_Streams);
            for (uint32_t i = 0; i < seqStreamCount; ++i)
            {
                zstdgpu_PropagateFseIndexCpu(zstdCpu.SeqStreamToLLenFseId[i], cpuLastLLenIndex);
                zstdgpu_PropagateFseIndexCpu(zstdCpu.SeqStreamToOffsFseId[i], cpuLastOffsIndex);
                zstdgpu_PropagateFseIndexCpu(zstdCpu.SeqStreamToMLenFseId[i], cpuLastMLenIndex);
            }
        }

        VALIDATE(CompressedBlocksData, &zstdCpu);
    }
    const uint32_t literalCount = CNTRS(HUF_Streams_DecodedBytes);
    const uint32_t sequenceCount = CNTRS(Seq_Streams_DecodedItems);
    zstdgpu_ResourceInfo_Stage_2_Init(&zstdInfo, literalCount, sequenceCount, 0, 0);
    zstdgpu_ResourceDataCpu_InitFromHeap(&zstdCpu, &zstdInfo);

    {
        zstdgpu_InitFseTable_SRT srt;
        zstdgpu_Init_InitFseTable_SRT(srt, zstdCpu);

        srt.tableStartIndex = 0;
        srt.tableDataStart  = zstdgpu_ComputeFseDataStartHufW(0, zstdCmpBlockCount);
        srt.tableDataCount  = kzstdgpu_FseElemMaxCount_HufW;
        for (uint32_t i = 0; i < CNTRS(FseHufW); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }

        srt.tableStartIndex += zstdCmpBlockCount;
        srt.tableDataStart  = zstdgpu_ComputeFseDataStartLLen(0, zstdCmpBlockCount);
        srt.tableDataCount  = kzstdgpu_FseElemMaxCount_LLen;
        for (uint32_t i = 0; i < CNTRS(FseLLen); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }

        srt.tableStartIndex += zstdCmpBlockCount + 1;
        srt.tableDataStart  = zstdgpu_ComputeFseDataStartOffs(0, zstdCmpBlockCount);
        srt.tableDataCount  = kzstdgpu_FseElemMaxCount_Offs;
        for (uint32_t i = 0; i < CNTRS(FseOffs); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }
        srt.tableStartIndex += zstdCmpBlockCount + 1;
        srt.tableDataStart  = zstdgpu_ComputeFseDataStartMLen(0, zstdCmpBlockCount);
        srt.tableDataCount  = kzstdgpu_FseElemMaxCount_MLen;
        for (uint32_t i = 0; i < CNTRS(FseMLen); ++i)
        {
            zstdgpu_ShaderEntry_InitFseTable(srt, i, 0);
        }
        VALIDATE(FseTables, &zstdCpu);
    }

    {
        zstdgpu_DecompressHuffmanWeights_SRT srt;
        zstdgpu_Init_DecompressHuffmanWeights_SRT(srt, zstdCpu);
        for (uint32_t i = 0; i < CNTRS(FseHufW); ++i)
        {
            zstdgpu_ShaderEntry_DecompressHuffmanWeights(srt, i);
        }

        VALIDATE(DecompressedHuffmanWeights, &zstdCpu);
    }

    {
        zstdgpu_DecodeHuffmanWeights_SRT srt;
        zstdgpu_Init_DecodeHuffmanWeights_SRT(srt, zstdCpu);
        srt.compressedBufferSizeInBytes = zstdInfo.CompressedData_ByteSize;
        for (uint32_t i = 0; i < CNTRS(HUF_WgtStreams); ++i)
        {
            zstdgpu_ShaderEntry_DecodeHuffmanWeights(srt, i);
        }
        VALIDATE(DecodedHuffmanWeights, &zstdCpu);
    }

    {
        // NOTE(pamartis): some helper passes that don't have CPU/GPU portability
        uint32_t groupPrefix = 0;
        {
            // CPU equivalent of [Compute Prefix Sum]
            const uint32_t hufLitCount = CNTRS(HufLit);
            const uint32_t hufLitStreamCountTotal = CNTRS(HUF_Streams);
            groupPrefix = zstdgpu_HufLitStreamCountToGroupCount(zstdCpu, hufLitCount, hufLitStreamCountTotal, zstdCmpBlockCount);

            CNTRS(DecompressLiteralsGroups) = groupPrefix;
        }

        // Run Literal Decompression
        zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT srt;
        zstdgpu_Init_InitHuffmanTable_And_DecompressLiterals_SRT(srt, zstdCpu);
        for (uint32_t groupId = 0; groupId < groupPrefix; ++groupId)
        {
            zstdgpu_ShaderEntry_InitHuffmanTable_And_DecompressLiterals(srt, groupId, 0, 1);
        }
        VALIDATE(DecompressedLiterals, &zstdCpu);
    }

    {
        zstdgpu_DecompressSequences_SRT srt;
        zstdgpu_Init_DecompressSequences_SRT(srt, zstdCpu);
        for (uint32_t i = 0; i < CNTRS(Seq_Streams); ++i)
        {
            zstdgpu_ShaderEntry_DecompressSequences_MultiStream_LdsOutCache(srt, /* groupId */ i, /* threadId */ 0, /* tgSize */ 1, /* streamsPerGroup */ 1, /* cacheDwordsPerStream */ 64);
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
    VALIDATE(DecompressedSequences, &zstdCpu);
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
ZSTDGPU_API uint64_t zstdgpu_RetrieveTimestamps(const wchar_t **outTimestampScopeNames, uint64_t *outTimestampScopeClocks, uint32_t *inoutTimestampScopeCnt, zstdgpu_PerRequestContext req, uint32_t stageIndex);

extern "C" void zstdgpu_AssertReportCback(const char *expr, const char *file, int line, const char *func, const char *msg, void *args)
{
    if (NULL != msg)
    {
        const size_t bufferSize = 1024;
        char buffer[bufferSize + 1];
        _vsnprintf_s(buffer, bufferSize + 1, bufferSize, msg, *(va_list*)args);
        debugPrint(L"[FAIL] (%hs) : %hs at %hs:%d in %hs\n", expr, buffer, file, line, func);
    }
    else
    {
        debugPrint(L"[FAIL] (%hs) at %hs:%d in %hs\n", expr, file, line, func);
    }
}

/**
 *  NOTE(pamartis): this structure holds all resources that could be created by `demoRun`.
 *  This helps to return early from `demoRun` (or longjmp/throw from anywhere on the stack above it)
 *  and release/free resources within `demoCleanup`
 */
struct DemoCtx
{
#ifndef _GAMING_XBOX
    int                         argc;
    wchar_t                   **argv;
#endif
    int                         retv;

    void                       *zstdData;
    zstdgpu_FrameInfo          *zstdFrameInfo;
    zstdgpu_OffsetAndSize      *zstdInFrameRefs;
    zstdgpu_OffsetAndSize      *zstdOutFrameRefs;
    void                       *zstdReferenceUncompressedData;
    wchar_t                    *zstFilePathStorage;
    wchar_t                    *csvFilePathStorage;

    ID3D12Device               *device;
    d3d12aid_CmdQueue           cmdQueue;
    d3d12aid_Timestamps         timestamps;
    zstdgpu_PersistentContext   persistentContext;
    zstdgpu_PerRequestContext   perRequestContext;
    d3d12aid_MappedBuffer       zstdCompressedFramesMemory;
    d3d12aid_MappedBuffer       zstdCompressedFramesRefs;
    d3d12aid_MappedBuffer       zstdUnCompressedFramesMemory;
    d3d12aid_MappedBuffer       zstdUnCompressedFramesRefs;
    ID3D12Heap                 *readbackHeap[3];
    ID3D12Heap                 *uploadHeap[3];
    ID3D12Heap                 *defaultHeap[3];
    ID3D12DescriptorHeap       *descriptorHeap[3];
    zstdgpu_ResourceDataCpu     zstdCpu;
    bool                        zstdCpuInit;

    uint64_t                    freqGpuClocks;
    FILE                       *csvFile;
};

/** Releases whatever `demoRun` managed to initialise (zero fields are skipped). */
static void demoCleanup(DemoCtx *ctx)
{
    if (NULL != ctx->cmdQueue.queue)
        d3d12aid_CmdQueue_CpuWaitForGpuIdle(&ctx->cmdQueue);

    if (NULL != ctx->perRequestContext)
    {
        void *memory = NULL;
        zstdgpu_DestroyPerRequestContext(&memory, NULL, ctx->perRequestContext);
        free(memory);
    }
    if (NULL != ctx->persistentContext)
    {
        void *memory = NULL;
        zstdgpu_DestroyPersistentContext(&memory, NULL, ctx->persistentContext);
        free(memory);
    }

    for (uint32_t i = 0; i < 3; ++i)
    {
        D3D12AID_SAFE_RELEASE(ctx->descriptorHeap[i]);
        D3D12AID_SAFE_RELEASE(ctx->readbackHeap[i]);
        D3D12AID_SAFE_RELEASE(ctx->uploadHeap[i]);
        D3D12AID_SAFE_RELEASE(ctx->defaultHeap[i]);
    }

    if (NULL != ctx->zstdCompressedFramesRefs.bufGpu)
        d3d12aid_MappedBuffer_Release(&ctx->zstdCompressedFramesRefs);
    if (NULL != ctx->zstdCompressedFramesMemory.bufGpu)
        d3d12aid_MappedBuffer_Release(&ctx->zstdCompressedFramesMemory);
    if (NULL != ctx->zstdUnCompressedFramesMemory.bufGpu)
        d3d12aid_MappedBuffer_Release(&ctx->zstdUnCompressedFramesMemory);
    if (NULL != ctx->zstdUnCompressedFramesRefs.bufGpu)
        d3d12aid_MappedBuffer_Release(&ctx->zstdUnCompressedFramesRefs);

    if (NULL != ctx->timestamps.heap)
        d3d12aid_Timestamps_Release(&ctx->timestamps);
    if (NULL != ctx->cmdQueue.queue)
        d3d12aid_CmdQueue_Release(&ctx->cmdQueue);

    if (ctx->zstdCpuInit)
        zstdgpu_ResourceDataCpu_Term(&ctx->zstdCpu);

    #define SAFE_FREE(ptr) do { if (NULL != ptr) { free(ptr); ptr = NULL; } } while (0)
    SAFE_FREE(ctx->zstdReferenceUncompressedData);
    SAFE_FREE(ctx->zstdOutFrameRefs);
    SAFE_FREE(ctx->zstdInFrameRefs);
    SAFE_FREE(ctx->zstdFrameInfo);
    SAFE_FREE(ctx->zstdData);
    SAFE_FREE(ctx->zstFilePathStorage);
    SAFE_FREE(ctx->csvFilePathStorage);
    #undef SAFE_FREE

    if (NULL != ctx->csvFile)
    {
        fflush(ctx->csvFile);
        fclose(ctx->csvFile);
    }

    if (NULL != ctx->device)
    {
        ctx->device->SetStablePowerState(FALSE);
        zstdgpu_Demo_PlatformTerm(ctx->device);
    }
}

static int demoRun(void *demoCtx);

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

    tta_AssertSetReportCback(&zstdgpu_AssertReportCback);

    DemoCtx ctx = {};
#ifndef _GAMING_XBOX
    ctx.argc = argc;
    ctx.argv = argv;
#endif

    int asserted = 0;
    if (IsDebuggerPresent())
    {
        tta_AssertSetReportLevel(ktta_AssertReportLevel_PrintAndBreak);
        demoRun(&ctx);
    }
    else
    {
        asserted = tta_AssertCallAndCatch(&demoRun, &ctx);
    }

    demoCleanup(&ctx);

    return asserted ? 1 : ctx.retv;
}

static int demoRun(void *demoCtx)
{
    DemoCtx *ctx = (DemoCtx *)demoCtx;

#ifndef _GAMING_XBOX
    int        argc = ctx->argc;
    wchar_t  **argv = ctx->argv;
#endif

    void                  *&zstdData                       = ctx->zstdData;
    zstdgpu_FrameInfo     *&zstdFrameInfo                  = ctx->zstdFrameInfo;
    zstdgpu_OffsetAndSize *&zstdInFrameRefs                = ctx->zstdInFrameRefs;
    zstdgpu_OffsetAndSize *&zstdOutFrameRefs               = ctx->zstdOutFrameRefs;
    void                  *&zstdReferenceUncompressedData  = ctx->zstdReferenceUncompressedData;
    wchar_t               *&zstFilePathStorage             = ctx->zstFilePathStorage;
    wchar_t               *&csvFilePathStorage             = ctx->csvFilePathStorage;

    ID3D12Device              *&device                       = ctx->device;
    d3d12aid_CmdQueue          &cmdQueue                     = ctx->cmdQueue;
    d3d12aid_Timestamps        &timestamps                   = ctx->timestamps;
    zstdgpu_PersistentContext  &persistentContext            = ctx->persistentContext;
    zstdgpu_PerRequestContext  &perRequestContext            = ctx->perRequestContext;
    d3d12aid_MappedBuffer      &zstdCompressedFramesMemory   = ctx->zstdCompressedFramesMemory;
    d3d12aid_MappedBuffer      &zstdCompressedFramesRefs     = ctx->zstdCompressedFramesRefs;
    d3d12aid_MappedBuffer      &zstdUnCompressedFramesMemory = ctx->zstdUnCompressedFramesMemory;
    d3d12aid_MappedBuffer      &zstdUnCompressedFramesRefs   = ctx->zstdUnCompressedFramesRefs;
    ID3D12Heap                *(&readbackHeap)[3]            = ctx->readbackHeap;
    ID3D12Heap                *(&uploadHeap)[3]              = ctx->uploadHeap;
    ID3D12Heap                *(&defaultHeap)[3]             = ctx->defaultHeap;
    ID3D12DescriptorHeap      *(&descriptorHeap)[3]          = ctx->descriptorHeap;
    zstdgpu_ResourceDataCpu    &zstdCpu                      = ctx->zstdCpu;
    uint64_t                   &freqGpuClocks                = ctx->freqGpuClocks;
    FILE                      *&csvFile                      = ctx->csvFile;

    bool extMem = false;
    bool blkCnt = false;
    bool seqCnt = false;
    bool chkGpu = false;
    bool chkCpu = false;
    bool simGpu = false;
    bool d3dDbg = false;
    bool d3dGbv = false;
    bool d3dGfx = false;
    bool outFrm = false;
    bool ssm = false;

    const wchar_t *zstFilePath = L"data\\group_0_cmp17_block8192.zst";
    const wchar_t *csvFilePath = L"perf.csv";
    uint32_t gpuVenId = 0x1414; // means -- find any vendor id, but not 0x1414
    uint32_t gpuDevId = ~0u;    // means -- find any device id
    uint32_t repCount = 10;
    uint32_t prfLevel = 0;
    uint32_t minFrame = 0;
    uint32_t maxFrame = ~0u;

#ifndef _GAMING_XBOX
    {
        int argi = 0;
        {
            bool nextZst = false;
            bool nextCsv = false;
            bool nextGpuVenId = false;
            bool nextGpuDevId = false;
            bool nextRepCount = false;
            bool nextPrfLevel = false;
            bool nextMinFrame = false;
            bool nextMaxFrame = false;
            bool badArg = false;
            for (argi = 1; argi < argc; ++argi)
            {
                if (nextZst)
                {
                    nextZst = false;
                    zstFilePathStorage = _wcsdup(argv[argi]);
                    zstFilePath = zstFilePathStorage;
                }
                else if (nextCsv)
                {
                    nextCsv = false;
                    csvFilePathStorage = _wcsdup(argv[argi]);
                    csvFilePath = csvFilePathStorage;
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
                else if (nextRepCount || nextPrfLevel || nextMinFrame || nextMaxFrame)
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
                        else if (nextMinFrame)
                            minFrame = value;
                        else if (nextMaxFrame)
                            maxFrame = value;
                    }

                    nextRepCount = false;
                    nextPrfLevel = false;
                    nextMinFrame = false;
                    nextMaxFrame = false;
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
                else if (0 == wcscmp(argv[argi], L"--out-csv"))
                {
                    nextCsv = true;
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
                else if (0 == wcscmp(argv[argi], L"--d3d-gbv"))
                {
                    d3dGbv = true;
                    d3dDbg = true; // --d3d-gbv implies --d3d-dbg (GBV requires the debug layer)
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
                else if (0 == wcscmp(argv[argi], L"--blk-cnt"))
                {
                    blkCnt = true;
                }
                else if (0 == wcscmp(argv[argi], L"--seq-cnt"))
                {
                    seqCnt = true;
                    blkCnt = true; // --seq-cnt implies --blk-cnt
                }
                else if (0 == wcscmp(argv[argi], L"--prf-lvl"))
                {
                    nextPrfLevel = true;
                }
                else if (0 == wcscmp(argv[argi], L"--idx-min"))
                {
                    nextMinFrame = true;
                }
                else if (0 == wcscmp(argv[argi], L"--idx-max"))
                {
                    nextMaxFrame = true;
                }
                else if (0 == wcscmp(argv[argi], L"--out-frm"))
                {
                    outFrm = true;
                }
                else if (0 == wcscmp(argv[argi], L"--ssm"))
                {
                    ssm = true;
                }
                else
                {
                    debugPrint(L"Unknown argv[%d] %s\n", argi, argv[argi]);
                    badArg = true;
                }
            }
            if (1 == argc || badArg)
            {
                debugPrint(L"USAGE:\n");
                debugPrint(L"\t--zst <path to .zst file> [Required] Specifies a file path to .zst file to decompress. Could be absolute or relative path.\n");
                debugPrint(L"\t--chk-gpu                 [Optional] After running decompresssion on GPU, validates its outputs against the outputs from reference decompressor.\n");
                debugPrint(L"\t--chk-cpu                 [Optional] Before running decompression on GPU, runs GPU decompressor code on CPU and validates its outputs against the outputs from reference decompressor.\n");
                debugPrint(L"\t--sim-gpu                 [Optional] After running decompression on GPU, runs key GPU decompressor stages on CPU using intermediate inputs from GPU decompression and validates its outputs against the outputs from reference decompressor.\n");
                debugPrint(L"\t--gpu-ven-id <id (hex)>   [Optional] VendorId (base16) to use when choosing GPU to run on.\n");
                debugPrint(L"\t--gpu-dev-id <id (hex)>   [Optional] DeviceId (base16) to use when choosing GPU to run on.\n");
                debugPrint(L"\t--d3d-dbg                 [Optional] Enables D3D12 debug layer (without GPU-Based Validation).\n");
                debugPrint(L"\t--d3d-gbv                 [Optional] Enables D3D12 GPU-Based Validation (implies --d3d-dbg).\n");
                debugPrint(L"\t--d3d-gfx                 [Optional] Enables D3D12 Graphics queue (DIRECT), otherwise COMPUTE (by default).\n");
                debugPrint(L"\t--run-cnt <count>         [Optional] The number of times to repeat the experiment.\n");
                debugPrint(L"\t--ext-mem                 [Optional] Enables external heaps so the library doesn't create them.\n");
                debugPrint(L"\t--blk-cnt                 [Optional] Uses SetupFrameInfoConstants path (user-specified block counts from CPU pre-scan).\n");
                debugPrint(L"\t--seq-cnt                 [Optional] Also uses SetupBlockInfoConstants (implies --blk-cnt). Merges all stages into single submission.\n");
                debugPrint(L"\t--prf-lvl <0, 1, 2>       [Optional] Chooses the level of profiling: 0 - overall bandwidth in GB/s, 1 - stage cost, 2 - internal pass cost.\n");
                debugPrint(L"\t--idx-{min,max} <number>  [Optional] Chooses the {minimal, maximal} index of the frame to decompress in multi-frame .zst file. Both values are clamped to the number of available frames.\n");
                debugPrint(L"\t--out-frm                 [Optional] Outputs decompressed frames to files with <source_name.frame_N> name.\n");
                debugPrint(L"\t--out-csv <path to .csv>  [Optional] Outputs performance information into CSV file.\n");
                debugPrint(L"\t--ssm                     [Optional] Forces single-submission mode with automatic scratch estimation.\n");
                if (badArg)
                {
                    ctx->retv = 1;
                    return 0;
                }
            }
            if (NULL == zstFilePathStorage)
            {
                debugPrint(L"[WARN] No '--zst <path to .zst file>' option was specified, running with pre-defined file path: '%s'.\n", zstFilePath);
            }
        }
    }
#endif
    uint32_t zstdDataSize = 0;
    uint32_t zstdCompressedFramesMemorySizeInBytes = 0;
    uint32_t zstdUnCompressedFramesMemorySizeInBytes = 0;

    loadFileAligned(&zstdData, &zstdDataSize, &zstdCompressedFramesMemorySizeInBytes, 2u, zstFilePath);
    if (NULL == zstdData)
    {
        debugPrint(L"[FAIL] Couldn't load '%s'. Early Out.\n", zstFilePath);
        ctx->retv = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    else
    {
        debugPrint(L"[INFO] Loaded '%s' -- %u bytes.\n", zstFilePath, zstdDataSize);
    }

    zstdgpu_CountFramesAndBlocksInfo fbInfo;
    zstdgpu_CountFramesAndBlocks(&fbInfo, zstdData, zstdCompressedFramesMemorySizeInBytes, zstdDataSize);

    zstdFrameInfo = (zstdgpu_FrameInfo *)malloc(sizeof(zstdgpu_FrameInfo) * fbInfo.frameCount);
    zstdInFrameRefs = (zstdgpu_OffsetAndSize *)malloc(sizeof(zstdgpu_OffsetAndSize) * fbInfo.frameCount);
    zstdOutFrameRefs = (zstdgpu_OffsetAndSize *)malloc(sizeof(zstdgpu_OffsetAndSize) * fbInfo.frameCount);
    zstdgpu_CollectFrames(zstdInFrameRefs, zstdFrameInfo, fbInfo.frameCount, zstdData, zstdCompressedFramesMemorySizeInBytes, zstdDataSize);

    // An invalid file can parse to 0 frames; clamp to 0 to avoid unsigned underflow of endFrame.
    const uint32_t endFrame = fbInfo.frameCount == 0 ? 0 : fbInfo.frameCount - 1;

    // NOTE(pamartis): Support the option to choose a range of frame in the input package/data
    if (minFrame > 0 || maxFrame < endFrame)
    {
        maxFrame = maxFrame < endFrame
                 ? maxFrame : endFrame;
        minFrame = minFrame < maxFrame
                 ? minFrame : maxFrame;

        zstdData = (char*)zstdData + zstdInFrameRefs[minFrame].offs;
        zstdDataSize = zstdInFrameRefs[maxFrame].offs - zstdInFrameRefs[minFrame].offs + zstdInFrameRefs[maxFrame].size;
        zstdCompressedFramesMemorySizeInBytes = (zstdDataSize + 3) & ~3u;

        // NOTE(pamartis): update all structures because 'zstdData' and 'zstdDataSize' has changed
        zstdgpu_CountFramesAndBlocks(&fbInfo, zstdData, zstdCompressedFramesMemorySizeInBytes, zstdDataSize);
        zstdgpu_CollectFrames(zstdInFrameRefs, zstdFrameInfo, fbInfo.frameCount, zstdData, zstdCompressedFramesMemorySizeInBytes, zstdDataSize);
    }

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
            offs =  zstdgpu_AlignUp(offs, 256);

            vcnt += zstdFrameInfo[i].uncompSize != 0 ? 1 : 0;
        }
        zstdUnCompressedFramesMemorySizeInBytes = offs;

        if (fbInfo.frameCount != vcnt)
        {
            debugPrint(L"[FAIL] Some frames don't carry uncompressed size. Early Out.\n");
            ctx->retv = 1;
            return 0;
        }
    }

    static const uint32_t kBackBufferCount = 2;
#ifdef _GAMING_XBOX
    static const uint32_t kFrameInterval = D3D12XBOX_FRAME_INTERVAL_60_HZ;
#else
    static const uint32_t kFrameInterval = 1;
#endif

    device = zstdgpu_Demo_PlatformInit(gpuVenId, gpuDevId, d3dDbg, d3dGbv);
    if (NULL == device)
    {
        debugPrint(L"[FAIL] Couldn't load create D3D12 device with venId=%u, devId=%u. Early Out.\n", gpuVenId, gpuDevId);
        ctx->retv = ERROR_SYSTEM_DEVICE_NOT_FOUND;
        return 0;
    }
    device->SetStablePowerState(TRUE);

#ifdef _GAMING_XBOX
    d3d12aid_CmdQueue_Create(&cmdQueue, device, kBackBufferCount, 1u, D3D12_COMMAND_LIST_TYPE_DIRECT);
#else
    d3d12aid_CmdQueue_Create(&cmdQueue, device, kBackBufferCount, 1u, d3dGfx ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE);
#endif

    #define ZSTDGPU_TS_LIST()   \
        ZSTDGPU_TS(Readback0)   \
        ZSTDGPU_TS(Readback1)

    d3d12aid_Timestamps_Create(&timestamps, device, 0
        /** NOTE(pamartis): generate timestamp counting using macro list */
        #define ZSTDGPU_TS(name) + 2
            ZSTDGPU_TS_LIST()
        #undef  ZSTDGPU_TS
        , kBackBufferCount
    );

    uint32_t zstdReferenceUncompressedDataSize = 0;

    if (simGpu)
    {
        chkCpu = true;
        debugPrint(L"[INFO] Option '--sim-gpu' was set. Enabling '--chk-cpu' automatically (required by '--sim-gpu').\n");
    }

    if (chkCpu || chkGpu)
    {
        debugPrint(L"[INFO] Running Reference Decompression and building Reference Uncompressed data ('--chk-cpu' or '--chk-gpu' was set).\n");

        zstdReferenceUncompressedDataSize = (uint32_t)ZSTD_get_decompressed_size(zstdData, zstdDataSize);
        zstdReferenceUncompressedData = malloc(zstdReferenceUncompressedDataSize);

        // Clear the buffer to a known zero state in case ZSTD_decompress writes less data than the frame header claims (this matches the GPU)
        memset(zstdReferenceUncompressedData, 0x00, zstdReferenceUncompressedDataSize);

        // TODO: consider "per-file" or "per-frame" state (not just state internal) for validation layer
        zstdgpu_ReferenceStore_Report_ChunkBase(zstdData);
        zstdgpu_ReferenceStore_AllocateMemory();

        // NOTE(pamartis): this call to reference ZSTD decompressor populates zstdgpu_ReferenceStore with ground-truth data.
        int r = ZSTD_decompress(zstdReferenceUncompressedData, zstdReferenceUncompressedDataSize, zstdData, zstdDataSize);
        debugPrint(L"[INFO] ZSTD_decompress  input size: %d  output size: %d   result: %d\n", zstdDataSize, zstdReferenceUncompressedDataSize, r); 
    }

    if (chkCpu)
    {
        debugPrint(L"[INFO] Running GPU Decompression code on CPU ('--chk-cpu' option was set).\n");

        // NOTE(pamartis): We run GPU Decompression pipeline on CPU to catch possible errors/assert early
        zstdgpu_Validate_GpuDecompressOnCpu(zstdCpu, zstdData, zstdInFrameRefs, fbInfo.frameCount, zstdDataSize, fbInfo.frameByteCount);
        ctx->zstdCpuInit = true;

        if (!simGpu)
        {
            zstdgpu_ResourceDataCpu_MarkReadOnly(&zstdCpu);
        }
    }

    debugPrint(L"[INFO] Initializing 'zstdgpu' Persistent Context.\n");
    {
        const uint32_t persistentMemorySize = zstdgpu_GetPersistentContextRequiredMemorySizeInBytes();
        ZSTDGPU_ENUM(Status) status = zstdgpu_CreatePersistentContext(&persistentContext, device, malloc(persistentMemorySize), persistentMemorySize);
        ZSTDGPU_ASSERT(ZSTDGPU_ENUM_CONST(StatusSuccess) == status);
    }

    debugPrint(L"[INFO] Initializing 'zstdgpu' PerRequest Context.\n");
    {
        const uint32_t perRequestMemorySize = zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes();
        ZSTDGPU_ENUM(Status) status = zstdgpu_CreatePerRequestContext(&perRequestContext, persistentContext, malloc(perRequestMemorySize), perRequestMemorySize);
        ZSTDGPU_ASSERT(ZSTDGPU_ENUM_CONST(StatusSuccess) == status);
    }

    uint32_t stageCount = 0;

    // NOTE(pamartis): This variable is needed to support '--ext-mem' demo mode supplying into zstdgpu library
    // 'compressed' data and 'meta' (references) to zstd frames -- as pre-loaded into VMEM buffers
    // TODO(pamartis): Expose this option as command line option
    const volatile uint32_t testSourceInGpuMemory = 0u;

    const uint32_t zstdFramesRefsSizeInBytes = sizeof(zstdgpu_OffsetAndSize) * fbInfo.frameCount;

    void* defaultUploadCallbackUserData[2];
    if (testSourceInGpuMemory > 0)
    {
        zstdCompressedFramesMemorySizeInBytes = zstdgpu_AlignUp(zstdDataSize, 4u);

        d3d12aid_MappedBuffer_Create(&zstdCompressedFramesMemory, device, 1u, zstdCompressedFramesMemorySizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdCompressedFramesRefs, device, 1u, zstdFramesRefsSizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesRefs, device, 1u, zstdFramesRefsSizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesMemory, device, 1u, zstdUnCompressedFramesMemorySizeInBytes, D3D12_HEAP_TYPE_READBACK);

        d3d12aid_MappedBuffer_Append(&zstdCompressedFramesMemory, 0, (void *)zstdData, zstdDataSize);
        d3d12aid_MappedBuffer_Append(&zstdCompressedFramesRefs, 0, (void *)zstdInFrameRefs, zstdFramesRefsSizeInBytes);
        d3d12aid_MappedBuffer_Append(&zstdUnCompressedFramesRefs, 0, (void *)zstdOutFrameRefs, zstdFramesRefsSizeInBytes);

        zstdgpu_SetupInputsAsFramesInGpuMemory(&stageCount, perRequestContext, zstdCompressedFramesMemory.bufGpu, zstdCompressedFramesMemorySizeInBytes, zstdCompressedFramesRefs.bufGpu, fbInfo.frameCount);
    }
    else
    {
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesRefs, device, 1u, zstdFramesRefsSizeInBytes, D3D12_HEAP_TYPE_UPLOAD);
        d3d12aid_MappedBuffer_Create(&zstdUnCompressedFramesMemory, device, 1, zstdUnCompressedFramesMemorySizeInBytes, D3D12_HEAP_TYPE_READBACK);

        d3d12aid_MappedBuffer_Append(&zstdUnCompressedFramesRefs, 0, (void *)zstdOutFrameRefs, zstdFramesRefsSizeInBytes);

        defaultUploadCallbackUserData[0] = (void *)zstdData;
        defaultUploadCallbackUserData[1] = (void *)zstdInFrameRefs;
        zstdgpu_SetupInputsAsFramesInCpuMemory(&stageCount, perRequestContext, fbInfo.frameCount, zstdDataSize, zstdgpu_DefaultUploadCallback, defaultUploadCallbackUserData);
    }
    if (ssm)
    {
        zstdgpu_SetupAllStageSubmission(perRequestContext);
    }
    if (blkCnt)
    {
        /**
         *  NOTE(pamartis):
         *  Depending on whether `zstdgpu_SetupAllStageSubmission` was called or not,
         *  calling `zstdgpu_SetupFrameInfoConstants` / `zstdgpu_SetupBlockInfoConstants`
         *  either:
         *
         *  - improves scratch memory estimation (`zstdgpu_SetupAllStageSubmission` was called)
         *
         *  - enables elimination of mandatory wait for completion on GPU of the command list
         *    populated with commands for the previous stage (if `zstdgpu_SetupAllStageSubmission` was NOT called)
         */
        zstdgpu_SetupFrameInfoConstants(perRequestContext, fbInfo.rawBlockCount, fbInfo.rleBlockCount, fbInfo.cmpBlockCount);
        if (seqCnt)
        {
            zstdgpu_CountLiteralAndSequenceInfo blkInfo;
            zstdgpu_CountCompressedLiteralsAndSequences(&blkInfo, zstdInFrameRefs, fbInfo.frameCount, zstdData, zstdCompressedFramesMemorySizeInBytes);
            zstdgpu_SetupBlockInfoConstants(perRequestContext, blkInfo.decodedLiteralsByteCount, blkInfo.sequenceCount);
        }
    }
    zstdgpu_SetupOutputs(perRequestContext, zstdUnCompressedFramesMemory.bufGpu, zstdUnCompressedFramesMemorySizeInBytes, zstdUnCompressedFramesRefs.bufGpu, fbInfo.frameCount);

    uint64_t readbackHeapSize[3] = { 0, 0, 0 };
    uint64_t uploadHeapSize[3] = {0, 0, 0 };
    uint64_t defaultHeapSize[3] = {0, 0, 0};
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
                #define RECREATE_HEAP(name, i)                      \
                    if (name##HeapSizeReq > name##HeapSize[i])      \
                    {                                               \
                        D3D12AID_SAFE_RELEASE(name##Heap[i]);       \
                        name##Heap[i] = d3d12aid_Heap_Create_WithHeapTypeAndFlags(device, name##HeapSizeReq, 0, name##Type, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);\
                        name##HeapSize[i] = name##HeapSizeReq;      \
                    }
                D3D12_HEAP_TYPE defaultType = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_HEAP_TYPE uploadType = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_HEAP_TYPE readbackType = D3D12_HEAP_TYPE_READBACK;
                if (0 == zstdgpu_IsAnyStageReadbackRequired(perRequestContext))
                {
                    if (extMem /** a scenario with supplying external memory */)
                    {
                        uint64_t defaultHeapSizeReq = 0;
                        uint64_t uploadHeapSizeReq = 0;
                        uint64_t readbackHeapSizeReq = 0;
                        uint32_t descriptorCountReq = 0;

                        zstdgpu_GetAllStageGpuMemoryRequirement(&defaultHeapSizeReq, &uploadHeapSizeReq, &readbackHeapSizeReq, &descriptorCountReq, perRequestContext);

                        RECREATE_HEAP(default, 0)
                        RECREATE_HEAP(upload, 0)
                        RECREATE_HEAP(readback, 0)

                        if (descriptorCountReq > descriptorCount[0])
                        {
                            D3D12AID_SAFE_RELEASE(descriptorHeap[0]);
                            descriptorHeap[0] =  d3d12aid_DescriptorHeap_Create(device, descriptorCountReq, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
                            descriptorCount[0] = descriptorCountReq;
                        }

                        zstdgpu_SubmitAllStagesWithExternalMemory(perRequestContext, cmdList, defaultHeap[0], 0, uploadHeap[0], 0, readbackHeap[0], 0, descriptorHeap[0], 0);
                    }
                    else
                    {
                        zstdgpu_SubmitAllStagesWithInteralMemory(perRequestContext, cmdList);
                    }
                }
                else
                {
                    uint32_t *const ReadbackTimestamp[2] = { &Readback0_Stamp, &Readback1_Stamp };
                    for (uint32_t stageIndex = 0; stageIndex < 3; ++stageIndex)
                    {
                        if (extMem /** a scenario with supplying external memory */)
                        {
                            uint64_t defaultHeapSizeReq = 0;
                            uint64_t uploadHeapSizeReq = 0;
                            uint64_t readbackHeapSizeReq = 0;
                            uint32_t descriptorCountReq = 0;

                            zstdgpu_GetGpuMemoryRequirement(&defaultHeapSizeReq, &uploadHeapSizeReq, &readbackHeapSizeReq, &descriptorCountReq, perRequestContext, stageIndex);

                            RECREATE_HEAP(default, stageIndex)
                            RECREATE_HEAP(upload, stageIndex)
                            RECREATE_HEAP(readback, stageIndex)

                            if (descriptorCountReq > descriptorCount[stageIndex])
                            {
                                D3D12AID_SAFE_RELEASE(descriptorHeap[stageIndex]);
                                descriptorHeap[stageIndex] =  d3d12aid_DescriptorHeap_Create(device, descriptorCountReq, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
                                descriptorCount[stageIndex] = descriptorCountReq;
                            }

                            zstdgpu_SubmitWithExternalMemory(perRequestContext, stageIndex, cmdList, defaultHeap[stageIndex], 0, uploadHeap[stageIndex], 0, readbackHeap[stageIndex], 0, descriptorHeap[stageIndex], 0);
                        }
                        else
                        {
                            zstdgpu_SubmitWithInteralMemory(perRequestContext, stageIndex, cmdList);
                        }
                        if (stageIndex < 2 && zstdgpu_IsReadbackRequired(perRequestContext, stageIndex))
                        {
                            d3d12aid_Timestamp_PushScope(*ReadbackTimestamp[stageIndex], timestamps, cmdList,
                                d3d12aid_CmdQueue_SubmitCmdList(&cmdQueue, 0);
                                d3d12aid_CmdQueue_CpuWaitForGpuIdle(&cmdQueue);
                                cmdList = d3d12aid_CmdQueue_StartCmdList(&cmdQueue, 0/** cmdListId */);
                            );
                        }
                    }
                }
                #undef RECREATE_HEAP

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

                    if (chkCpu && chkGpu)
                        zstdgpu_Test_BlockPrefix(zstdCpu, gpuData);

                    if (chkGpu)
                    {
                        uint32_t failedFrameCount = 0;

                        {
                            const char *ref = (char*)zstdReferenceUncompressedData;
                            const char *tst = (char*)zstdUnCompressedFramesMemory.bufMem[0];
                            for (uint32_t i = 0; i < fbInfo.frameCount; ++i)
                            {
                                failedFrameCount += (0 != memcmp(ref, tst + zstdOutFrameRefs[i].offs, zstdOutFrameRefs[i].size));

                                ref += zstdOutFrameRefs[i].size;
                            }
                        }

                        if (failedFrameCount > 0)
                        {
                            const char *ref = (char*)zstdReferenceUncompressedData;
                            const char *tst = (char*)zstdUnCompressedFramesMemory.bufMem[0];

                            debugPrint(L"[FAIL] %u/%u frames failed validation.\n", failedFrameCount, fbInfo.frameCount);

                            const uint32_t failedRawBlockCount = zstdgpu_Test_DecompressedDataPerBlockType(
                                gpuData.GlobalBlockIndexPerRawBlock,
                                fbInfo.rawBlockCount,
                                gpuData.PerFrameBlockCountAll,
                                zstdOutFrameRefs,
                                fbInfo.frameCount,
                                gpuData.BlockSizePrefix,
                                ref,
                                tst
                            );

                            const uint32_t failedRleBlockCount = zstdgpu_Test_DecompressedDataPerBlockType(
                                gpuData.GlobalBlockIndexPerRleBlock,
                                fbInfo.rleBlockCount,
                                gpuData.PerFrameBlockCountAll,
                                zstdOutFrameRefs,
                                fbInfo.frameCount,
                                gpuData.BlockSizePrefix,
                                ref,
                                tst
                            );

                            const uint32_t failedCmpBlockCount = zstdgpu_Test_DecompressedDataPerBlockType(
                                gpuData.GlobalBlockIndexPerCmpBlock,
                                fbInfo.cmpBlockCount,
                                gpuData.PerFrameBlockCountAll,
                                zstdOutFrameRefs,
                                fbInfo.frameCount,
                                gpuData.BlockSizePrefix,
                                ref,
                                tst
                            );

                            if (failedRawBlockCount > 0 || failedRleBlockCount > 0)
                                debugPrint(L"[FAIL] %u/%u RAW blocks and %u/%u RLE blocks failed validation. Likely MemCpy/MemSet pass is broken, unless ExecuteSequence stomps the memory written by MemCpu/MemSet.\n", failedRawBlockCount, fbInfo.rawBlockCount, failedRleBlockCount, fbInfo.rleBlockCount);

                            if (failedCmpBlockCount > 0)
                                debugPrint(L"[FAIL] %u/%u CMP blocks failed validation. ExecuteSequences is likely broken unless an issue happens earlier in the pipeline or unless TDR is hit.\n", failedCmpBlockCount, fbInfo.cmpBlockCount);

                            ctx->retv = 1;
                            return 0;
                        }
                    }
                }

                if (frameIndex == 0 && outFrm /** output decompressed frame data to files if requested via command line */)
                {
                    const int bufferSize = ZSTDGPU_WARN_DISABLE_MSVC(4996, _snwprintf(NULL, 0, L"%s.frame_%u", zstFilePath, fbInfo.frameCount) + 1);
                    wchar_t *buffer = (wchar_t *)malloc(bufferSize * sizeof(wchar_t));
                    for (uint32_t i = 0; i < fbInfo.frameCount; ++i)
                    {
                        ZSTDGPU_WARN_DISABLE_MSVC(4996, _snwprintf(buffer, bufferSize, L"%s.frame_%u", zstFilePath, i));
                        saveFile(buffer, (char*)zstdUnCompressedFramesMemory.bufMem[0] + zstdOutFrameRefs[i].offs, zstdOutFrameRefs[i].size);
                    }
                    free(buffer);
                }

                if (freqGpuClocks == 0)
                {
                    cmdQueue.queue->GetTimestampFrequency(&freqGpuClocks);
                }
                const wchar_t *timestampScopeNames[22];
                uint64_t timestampScopeClocks[22];
                uint32_t timestampScopeCount = 0;
                uint64_t clks = 0;
                uint64_t clksAll = 0;

                if (NULL == csvFile)
                {
                    if (NULL != csvFilePathStorage)
                    {
                        // --out-csv <path> was specified by the caller.
                        // Honor the exact path so automated callers can locate the CSV
                        // at the location they requested.
                        _wfopen_s(&csvFile, csvFilePath, L"w");
                    }
                    else
                    {
                        // --out-csv was not specified; default csvFilePath is "perf.csv".
                        // Append the .zst stem and a timestamp so repeated interactive runs
                        // don't clobber each other.

                        // find the start of .zst file name (to further add into .csv name)
                        const wchar_t *zstNameStart = wcsrchr(zstFilePath, L'\\');
                        if (NULL == zstNameStart)
                        {
                            zstNameStart = zstFilePath;
                        }
                        else
                        {
                            zstNameStart += 1;
                        }

                        // find the start of .zst extension (to remove it from the name when adding into .csv name)
                        const wchar_t *zstExtStart = wcsrchr(zstNameStart, L'.');

                        // calculate the length of .zst name without extension
                        size_t zstNameLen = NULL != zstExtStart ? (zstExtStart - zstNameStart) : wcslen(zstNameStart);

                        const wchar_t *csvExtStart = wcsrchr(csvFilePath, L'.');
                        size_t csvFilePathLenNoExt = NULL != csvExtStart ? (csvExtStart - csvFilePath) : wcslen(csvFilePath);
                        csvExtStart = L".csv";

                        time_t timeData = time(NULL);
                        tm timeDataLocal;
                        localtime_s(&timeDataLocal, &timeData);

                        const int printBufSize = ZSTDGPU_WARN_DISABLE_MSVC(4996, _snwprintf(NULL, 0, L"%.*s_%.*s_%4d-%02d-%02d_%02d-%02d-%02d%s", (int)csvFilePathLenNoExt, csvFilePath, (int)zstNameLen, zstNameStart, 1900 + timeDataLocal.tm_year, 1 + timeDataLocal.tm_mon, timeDataLocal.tm_mday, timeDataLocal.tm_hour, timeDataLocal.tm_min, timeDataLocal.tm_sec, csvExtStart) + 1);
                        wchar_t *printBuf = (wchar_t *)malloc(printBufSize * sizeof(wchar_t));

                        if (NULL != printBuf)
                        {
                            ZSTDGPU_WARN_DISABLE_MSVC(4996, _snwprintf(printBuf, printBufSize, L"%.*s_%.*s_%4d-%02d-%02d_%02d-%02d-%02d%s", (int)csvFilePathLenNoExt, csvFilePath, (int)zstNameLen, zstNameStart, 1900 + timeDataLocal.tm_year, 1 + timeDataLocal.tm_mon, timeDataLocal.tm_mday, timeDataLocal.tm_hour, timeDataLocal.tm_min, timeDataLocal.tm_sec, csvExtStart));
                            _wfopen_s(&csvFile, printBuf, L"w");
                        }

                        free(printBuf);
                    }
                }

                if (NULL != csvFile)
                {
                    if (0 == frameIndex)
                    {
                        #define WRITE_CSV_HEADER(file, scopeName, subScopeCount, subScopeNames) \
                            if (prfLevel > 0)                                                   \
                            {                                                                   \
                                fwprintf_s(file, L"%s (us),", scopeName);                       \
                                if (prfLevel > 1)                                               \
                                {                                                               \
                                    for (uint32_t i = 0; i < subScopeCount; ++i)                \
                                    {                                                           \
                                        fwprintf_s(file, L"%s :: %s (us),", scopeName, subScopeNames[i]);\
                                    }                                                           \
                                }                                                               \
                            }
                        // write the header together with the first frame
                        fwprintf_s(csvFile, L"RunIdx,");

                        timestampScopeCount = _countof(timestampScopeClocks);
                        zstdgpu_RetrieveTimestamps(timestampScopeNames, timestampScopeClocks, &timestampScopeCount, perRequestContext, 0);
                        WRITE_CSV_HEADER(csvFile, L"Stage 0", timestampScopeCount, timestampScopeNames);
                        if (Readback0_Stamp != ~0u)
                        {
                            WRITE_CSV_HEADER(csvFile, L"Readback 0", 0, timestampScopeNames);
                        }

                        timestampScopeCount = _countof(timestampScopeClocks);
                        zstdgpu_RetrieveTimestamps(timestampScopeNames, timestampScopeClocks, &timestampScopeCount, perRequestContext, 1);
                        WRITE_CSV_HEADER(csvFile, L"Stage 1", timestampScopeCount, timestampScopeNames);
                        if (Readback1_Stamp != ~0u)
                        {
                            WRITE_CSV_HEADER(csvFile, L"Readback 1", 0, timestampScopeNames);
                        }

                        timestampScopeCount = _countof(timestampScopeClocks);
                        zstdgpu_RetrieveTimestamps(timestampScopeNames, timestampScopeClocks, &timestampScopeCount, perRequestContext, 2);
                        WRITE_CSV_HEADER(csvFile, L"Stage 2", timestampScopeCount, timestampScopeNames);
                        fwprintf_s(csvFile, L"Bandwidth (GB/s)\n");
                        #undef WRITE_CSV_HEADER

                        #define WRITE_CSV_DATA(file, scopeClks, subScopeCount, subScopeClks)    \
                            if (prfLevel > 0)                                                   \
                            {                                                                   \
                                uint64_t usec = (scopeClks * 1000000) / freqGpuClocks;          \
                                fwprintf_s(file, L"%llu,", usec);                               \
                                if (prfLevel > 1)                                               \
                                {                                                               \
                                    for (uint32_t i = 0; i < subScopeCount; ++i)                \
                                    {                                                           \
                                        usec = (subScopeClks[i] * 1000000) / freqGpuClocks;     \
                                        fwprintf_s(file, L"%llu,", usec);                       \
                                    }                                                           \
                                }                                                               \
                            }
                    }

                    fwprintf_s(csvFile, L"%u,", frameIndex);

                    timestampScopeCount = _countof(timestampScopeClocks);
                    clks = zstdgpu_RetrieveTimestamps(timestampScopeNames, timestampScopeClocks, &timestampScopeCount, perRequestContext, 0);
                    clksAll = clks;
                    WRITE_CSV_DATA(csvFile, clks, timestampScopeCount, timestampScopeClocks);
                    if (Readback0_Stamp != ~0u)
                    {
                        clks = d3d12aid_Timestamps_GetScopeDelta(&timestamps, kBackBufferIndex, Readback0_Stamp);
                        clksAll += clks;
                        WRITE_CSV_DATA(csvFile, clks, 0, timestampScopeClocks);
                    }

                    timestampScopeCount = _countof(timestampScopeClocks);
                    clks = zstdgpu_RetrieveTimestamps(timestampScopeNames, timestampScopeClocks, &timestampScopeCount, perRequestContext, 1);
                    clksAll += clks;
                    WRITE_CSV_DATA(csvFile, clks, timestampScopeCount, timestampScopeClocks);
                    if (Readback1_Stamp != ~0u)
                    {
                        clks = d3d12aid_Timestamps_GetScopeDelta(&timestamps, kBackBufferIndex, Readback1_Stamp);
                        clksAll += clks;
                        WRITE_CSV_DATA(csvFile, clks, 0, timestampScopeClocks);
                    }

                    timestampScopeCount = _countof(timestampScopeClocks);
                    clks = zstdgpu_RetrieveTimestamps(timestampScopeNames, timestampScopeClocks, &timestampScopeCount, perRequestContext, 2);
                    clksAll += clks;
                    WRITE_CSV_DATA(csvFile, clks, timestampScopeCount, timestampScopeClocks);
                    #undef WRITE_CSV_DATA

                    const uint64_t ns = (clksAll * 1000000000) / freqGpuClocks;
                    const double decompressionThroughput = (double)zstdUnCompressedFramesMemorySizeInBytes / ns;
                    fwprintf_s(csvFile, L"%lf\n", decompressionThroughput);
                }
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
    debugPrint(L"Finished.\n");
    return 0;
}

#define TTA_ASSERT_IMPL

/**
 *  NOTE(pamartis): tta_assert.h leaves usage `TTA_ASSERT_NOEXCEPT` to the user,
 *  so we choose to enable `TTA_ASSERT_NOEXCEPT` (which effectively switching to `longjmp` insteead of `throw`)
 *  when the compiler doesn't support exceptions
 */
#if defined(__clang__)
#   ifndef __EXCEPTIONS
#       define TTA_ASSERT_NOEXCEPT 1
#   endif
#elif defined(_MSC_VER)
#   ifndef _CPPUNWIND
#       define TTA_ASSERT_NOEXCEPT 1
#   endif
#else
#   error Unknown compiler
#endif

ZSTDGPU_WARN_PUSH_MSVC()
ZSTDGPU_WARN_STOP_MSVC(4611) /* warning C4611: interaction between 'function' and C++ object destruction is non-portable */
#include <tta_assert.h>
ZSTDGPU_WARN_POP_MSVC()
