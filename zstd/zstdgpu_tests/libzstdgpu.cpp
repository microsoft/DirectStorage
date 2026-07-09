// Copyright (C) Microsoft Corporation. All rights reserved.

#include "libzstdgpu.h"

#include <stdint.h>
#include <windows.h>
#include <winrt/base.h>

namespace Decompression
{
    HRESULT hresult_from_status(zstdgpu_Status status)
    {
        switch (status)
        {
        case zstdgpu_Status::kzstdgpu_StatusSuccess:
            return S_OK;
        case zstdgpu_Status::kzstdgpu_StatusInvalidArgument:
            return E_INVALIDARG;
        // case zstdgpu_Status::kzstdgpu_StageNotRequired:
        //    return E_DECOMPRESSION_STAGE_NOT_REQUIRED;
        }
        return E_FAIL;
    }

    HRESULT _cdecl AllocatePersistentContext(ID3D12Device* device, void** persistantContext)
    {
        if (!device || !persistantContext)
            return E_INVALIDARG;

        uint32_t contextSizeBytes = zstdgpu_GetPersistentContextRequiredMemorySizeInBytes();

        void* memoryBlock = malloc(contextSizeBytes);
        if (!memoryBlock)
            return E_OUTOFMEMORY;

        zstdgpu_PersistentContext context = nullptr;
        HRESULT hr =
            hresult_from_status(zstdgpu_CreatePersistentContext(&context, device, memoryBlock, contextSizeBytes));
        if (SUCCEEDED(hr))
        {
            *persistantContext = context;
        }
        else
        {
            *persistantContext = nullptr;
        }

        return hr;
    }

    void _cdecl FreePersistentContext(void* persistantContext)
    {
        if (!persistantContext)
            return;

        zstdgpu_PersistentContext context = reinterpret_cast<zstdgpu_PersistentContext>(persistantContext);
        void* memoryBlock = nullptr;
        zstdgpu_Status status = zstdgpu_DestroyPersistentContext(&memoryBlock, nullptr, context);
        if (memoryBlock && status == zstdgpu_Status::kzstdgpu_StatusSuccess)
        {
            free(memoryBlock);
        }
    }

    HRESULT _cdecl AllocatePerRequestContext(void* persistantContext, void** perRequestContext)
    {
        if (!persistantContext || !perRequestContext)
            return E_INVALIDARG;

        *perRequestContext = nullptr;
        uint32_t contextSizeBytes = zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes();

        void* memoryBlock = malloc(contextSizeBytes);
        if (!memoryBlock)
            return E_OUTOFMEMORY;

        zstdgpu_PerRequestContext context = nullptr;
        HRESULT hr = hresult_from_status(zstdgpu_CreatePerRequestContext(
            &context,
            reinterpret_cast<zstdgpu_PersistentContext>(persistantContext),
            memoryBlock,
            contextSizeBytes));
        if (SUCCEEDED(hr))
        {
            *perRequestContext = context;
        }
        else
        {
            *perRequestContext = nullptr;
        }

        return hr;
    }

    void _cdecl FreePerRequestContext(void* perRequestContext)
    {
        if (!perRequestContext)
            return;

        zstdgpu_PerRequestContext context = reinterpret_cast<zstdgpu_PerRequestContext>(perRequestContext);
        void* memoryBlock = nullptr;
        zstdgpu_Status status = zstdgpu_DestroyPerRequestContext(&memoryBlock, nullptr, context);
        if (memoryBlock && status == zstdgpu_Status::kzstdgpu_StatusSuccess)
        {
            free(memoryBlock);
        }
    }

    HRESULT _cdecl SetupDecompressionStages(
        void* perRequestContext,
        ID3D12Resource* inputBuffer,
        uint32_t inputBufferSizeBytes,
        ID3D12Resource* inputOffsetsAndSizes,
        ID3D12Resource* outputBuffer,
        uint32_t outputBufferSizeBytes,
        ID3D12Resource* outputOffsetsAndSizes,
        uint32_t totalOffsetsAndSizes,
        uint32_t* totalStageCount)
    {
        HRESULT hr = hresult_from_status(zstdgpu_SetupInputsAsFramesInGpuMemory(
            totalStageCount,
            reinterpret_cast<zstdgpu_PerRequestContext>(perRequestContext),
            inputBuffer,
            inputBufferSizeBytes,
            inputOffsetsAndSizes,
            totalOffsetsAndSizes));
        if (FAILED(hr))
            return hr;

        return hresult_from_status(zstdgpu_SetupOutputs(
            reinterpret_cast<zstdgpu_PerRequestContext>(perRequestContext),
            outputBuffer,
            outputBufferSizeBytes,
            outputOffsetsAndSizes,
            totalOffsetsAndSizes));
    }

    HRESULT _cdecl GetHeapMemorySizesForStage(
        void* perRequestContext,
        uint32_t stage,
        uint64_t* defaultHeapSizeBytes,
        uint64_t* uploadHeapSizeBytes,
        uint64_t* readbackHeapSizeBytes,
        uint32_t* shaderVisibleDescriptorCount)
    {
        return hresult_from_status(zstdgpu_GetGpuMemoryRequirement(
            defaultHeapSizeBytes,
            uploadHeapSizeBytes,
            readbackHeapSizeBytes,
            shaderVisibleDescriptorCount,
            reinterpret_cast<zstdgpu_PerRequestContext>(perRequestContext),
            stage));
    }

    HRESULT _cdecl AddDecompressionStageToCommandlist(
        void* perRequestContext,
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
        return hresult_from_status(zstdgpu_SubmitWithExternalMemory(
            reinterpret_cast<zstdgpu_PerRequestContext>(perRequestContext),
            stage,
            commandList,
            defaultHeap,
            defaultHeapOffsetInBytes,
            uploadHeap,
            uploadHeapOffsetInBytes,
            readbackHeap,
            readbackHeapOffsetInBytes,
            shaderVisibleHeap,
            shaderVisibileHeapOffsetInDescriptors));
    }
} // namespace Decompression
