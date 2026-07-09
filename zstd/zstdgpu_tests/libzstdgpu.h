// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <stdint.h>
#include <winrt/base.h>
#include <d3d12.h>

#include "zstdgpu.h"
extern "C"
{
    #include "zstd_decompress.h"
}

// Input buffer data must be 4-byte aligned before sending to
// be decompressed. This is a shader limitation.
#define DWORD_ALIGN(count) ((count + 3) & ~3)
#define ZSTDGPU_ALIGN(offset, aligment) ((offset) + (aligment) - 1) & ~((aligment) - 1)
#define ZSTDGPU_ALIGN_DEFAULT(offset) ZSTDGPU_ALIGN(offset, 0x10000)

// Description:
// The decompression stage is not required for this session.
// This can happen if the compressed data does not need specific
// processing so the library is able to skip stages.
//
#define E_DECOMPRESSION_STAGE_NOT_REQUIRED ((HRESULT)0x89240900L) // starting custom errors at 900 to avoid collisions with DirectStorage defined errors

namespace Decompression
{
    struct OffsetAndSize
    {
        uint32_t Offset;
        uint32_t Size;
    };

    HRESULT _cdecl AllocatePersistentContext(ID3D12Device* device, void** persistantContext);
    void _cdecl FreePersistentContext(void* persistantContext);

    HRESULT _cdecl AllocatePerRequestContext(void* persistantContext, void** perRequestContext);
    void _cdecl FreePerRequestContext(void* perRequestContext);

    HRESULT _cdecl SetupDecompressionStages(
        void* perRequestContext,
        ID3D12Resource* inputBuffer,
        uint32_t inputBufferSizeBytes,
        ID3D12Resource* inputOffsetsAndSizes,  // buffer of <OffsetAndSize> entries
        ID3D12Resource* outputBuffer,
        uint32_t outputBufferSizeBytes,
        ID3D12Resource* outputOffsetsAndSizes, // buffer of <OffsetAndSize> entries
        uint32_t totalOffsetsAndSizes,
        uint32_t* totalStageCount);

    HRESULT _cdecl GetHeapMemorySizesForStage(
        void* perRequestContext,
        uint32_t stage, // 0-based stage index ( bounded by the totalStageCount returned by SetupDecompressionStages )
        uint64_t* defaultHeapSizeBytes,
        uint64_t* uploadHeapSizeBytes,
        uint64_t* readbackHeapSizeBytes,
        uint32_t* shaderVisibleDescriptorCount);

    HRESULT _cdecl AddDecompressionStageToCommandlist(
        void* perRequestContext,
        uint32_t stage, // 0-based stage index ( bounded by the totalStageCount returned by SetupDecompressionStages )
        ID3D12Heap* defaultHeap,
        uint32_t defaultHeapOffsetInBytes,
        ID3D12Heap* uploadHeap,
        uint32_t uploadHeapOffsetInBytes,
        ID3D12Heap* readbackHeap,
        uint32_t readbackHeapOffsetInBytes,
        ID3D12DescriptorHeap* shaderVisibleHeap,
        uint32_t shaderVisibileHeapOffsetInDescriptors,
        ID3D12GraphicsCommandList4* commandList);

    // C++ wrapper around the C-style libzstdgpu decompression API.
    // This wrapper throws exceptions on errors and should be used
    // in exception capable code only.
    class PersistentContext
    {
        void* m_context = nullptr;

    public:
        PersistentContext(ID3D12Device* device)
        {
            winrt::check_hresult(AllocatePersistentContext(device, &m_context));
        }
        ~PersistentContext()
        {
            FreePersistentContext(m_context);
        }
        void* Get() const
        {
            return m_context;
        }
    };

    class ZstdFallbackShaderDecompressionSession
    {
        class PerRequestContext
        {
            void* m_context = nullptr;

        public:
            PerRequestContext(void* persistantContext)
            {
                winrt::check_hresult(AllocatePerRequestContext(persistantContext, &m_context));
            }
            ~PerRequestContext()
            {
                FreePerRequestContext(m_context);
            }
            void* Get() const
            {
                return m_context;
            }
        };

        std::unique_ptr<PerRequestContext> m_perRequestContext;

    public:
        explicit ZstdFallbackShaderDecompressionSession(
            PersistentContext* persistantContext,
            ID3D12Resource* inputBuffer,
            uint32_t inputBufferSizeBytes,
            ID3D12Resource* inputOffsetsAndSizes,
            ID3D12Resource* outputBuffer,
            uint32_t outputBufferSizeBytes,
            ID3D12Resource* outputOffsetsAndSizes,
            uint32_t totalOffsetsAndSizes,
            uint32_t* totalStageCount)
        {
            m_perRequestContext = std::make_unique<PerRequestContext>(persistantContext->Get());
            winrt::check_hresult(SetupDecompressionStages(
                m_perRequestContext->Get(),
                inputBuffer,
                inputBufferSizeBytes,
                inputOffsetsAndSizes,
                outputBuffer,
                outputBufferSizeBytes,
                outputOffsetsAndSizes,
                totalOffsetsAndSizes,
                totalStageCount));
        }

        // Returns true if the the specified stage requires allocations, false if the stage was not required.
        bool TryGetHeapMemorySizes(
            uint32_t stage,
            uint64_t* defaultHeapSizeBytes,
            uint64_t* uploadHeapSizeBytes,
            uint64_t* readbackHeapSizeBytes,
            uint32_t* shaderVisibleDescriptorCount)
        {
            HRESULT hr = GetHeapMemorySizesForStage(
                m_perRequestContext->Get(),
                stage,
                defaultHeapSizeBytes,
                uploadHeapSizeBytes,
                readbackHeapSizeBytes,
                shaderVisibleDescriptorCount);

            if (hr == E_DECOMPRESSION_STAGE_NOT_REQUIRED)
            {
                return false;
            }
            winrt::check_hresult(hr);
            return true;
        }

        void AddToCommandlist(
            uint32_t stage,
            ID3D12Heap* defaultHeap,
            uint32_t defaultHeapOffsetInBytes,
            ID3D12Heap* uploadHeap,
            uint32_t uploadHeapOffsetInBytes,
            ID3D12Heap* readbackHeap,
            uint32_t readbackHeapOffsetInBytes,
            ID3D12DescriptorHeap* shaderVisibleHeap,
            uint32_t shaderVisibileHeapOffsetInDescriptors,
            ID3D12GraphicsCommandList4* commandList)
        {
            winrt::check_hresult(AddDecompressionStageToCommandlist(
                m_perRequestContext->Get(),
                stage,
                defaultHeap,
                defaultHeapOffsetInBytes,
                uploadHeap,
                uploadHeapOffsetInBytes,
                readbackHeap,
                readbackHeapOffsetInBytes,
                shaderVisibleHeap,
                shaderVisibileHeapOffsetInDescriptors,
                commandList));
        }
    };

    class ZstdFallbackShaderDecompressionLibrary
    {
        std::unique_ptr<PersistentContext> m_persistantContext;

    public:
        explicit ZstdFallbackShaderDecompressionLibrary(ID3D12Device* device)
        {
            m_persistantContext = std::make_unique<PersistentContext>(device);
        }

        std::unique_ptr<ZstdFallbackShaderDecompressionSession> CreateDecompressionSession(
            ID3D12Resource* inputBuffer,
            uint32_t inputBufferSizeBytes,
            ID3D12Resource* inputOffsetsAndSizes,
            ID3D12Resource* outputBuffer,
            uint32_t outputBufferSizeBytes,
            ID3D12Resource* outputOffsetsAndSizes,
            uint32_t totalOffsetsAndSizes,
            uint32_t* totalStageCount)
        {
            return std::make_unique<ZstdFallbackShaderDecompressionSession>(
                m_persistantContext.get(),
                inputBuffer,
                inputBufferSizeBytes,
                inputOffsetsAndSizes,
                outputBuffer,
                outputBufferSizeBytes,
                outputOffsetsAndSizes,
                totalOffsetsAndSizes,
                totalStageCount);
        }
    };
} // namespace Decompression
