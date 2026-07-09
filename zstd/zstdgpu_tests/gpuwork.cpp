//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "gpuwork.h"

#include <d3d12.h>
#include <d3dx12.h>
#include <windows.h>
#include <winrt/base.h>

#include <iostream>
#include <limits>

using winrt::check_hresult;
using namespace Decompression;

GpuWork::GpuWork(ID3D12Device* device, const wchar_t* name)
{
    check_hresult(device->QueryInterface(IID_PPV_ARGS(&m_device)));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    check_hresult(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    std::wstring queueName = name ? name : L" Compute Command Queue";
    m_commandQueue->SetName(queueName.c_str());

    check_hresult(m_device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&m_commandAllocator)));
    std::wstring allocatorName = name ? name : L" Command Allocator";
    m_commandAllocator->SetName(queueName.c_str());

    check_hresult(
        m_device
            ->CreateCommandList(0, queueDesc.Type, m_commandAllocator.get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    std::wstring commandListName = name ? name : L" Command List";
    m_commandList->SetName(commandListName.c_str());

    m_commandList->Close();

    m_commandAllocator->Reset();

    check_hresult(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    std::wstring fenceName = name ? name : L" Fence";
    m_fence->SetName(fenceName.c_str());
    m_event.reset(CreateEvent(nullptr, false, false, nullptr));

    D3D12_QUERY_HEAP_DESC queryHeapDesc{};
    queryHeapDesc.Count = 2; // start and end timestamps
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    check_hresult(m_device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(m_timestampQueryHeap.put())));
    std::wstring heapName = name ? name : L" Timestamp Query Heap";
    m_timestampQueryHeap->SetName(heapName.c_str());
}

ZstdDecompressionWork::ZstdDecompressionWork(ID3D12Device* device, const wchar_t* name)
    : GpuWork(device, name)
{
    m_zstdgu = std::make_unique<Decompression::ZstdFallbackShaderDecompressionLibrary>(device);
}

Frames ZstdDecompressionWork::Decompress(std::vector<uint8_t>& compressedData, OffsetsAndSizes& offsetsAndSizes)
{
    uint32_t totalOffsetsAndSizes = static_cast<uint32_t>(
        offsetsAndSizes.InputOffsets.size()); // assume input and output offsets and sizes count are the same
    uint32_t offsetsAndSizesBufferSize = totalOffsetsAndSizes * sizeof(zstdgpu_OffsetAndSize);

    uint32_t inputBufferSize = static_cast<uint32_t>(compressedData.size());
    auto inputBuffer = CreateBuffer(compressedData.size(), L"Input Buffer");
    CopyToResource(inputBuffer.get(), compressedData.data(), compressedData.size());

    auto inputOffsetsAndSizesBuffer = CreateBuffer(offsetsAndSizesBufferSize, L"Input Offsets and Sizes Buffer");
    CopyToResource(
        inputOffsetsAndSizesBuffer.get(),
        reinterpret_cast<uint8_t*>(offsetsAndSizes.InputOffsets.data()),
        offsetsAndSizesBufferSize);

    uint32_t outputBufferSize = offsetsAndSizes.UnCompressedFramesMemorySizeInBytes;
    auto outputBuffer = CreateBuffer(static_cast<size_t>(outputBufferSize), L"Output Buffer");

    auto outputOffsetsAndSizesBuffer = CreateBuffer(offsetsAndSizesBufferSize, L"Output Offsets and Sizes Buffer");
    CopyToResource(
        outputOffsetsAndSizesBuffer.get(),
        reinterpret_cast<uint8_t*>(offsetsAndSizes.OutputOffsets.data()),
        offsetsAndSizesBufferSize);

    uint32_t totalStageCount = 0;
    auto session = m_zstdgu->CreateDecompressionSession(
        inputBuffer.get(),
        inputBufferSize,
        inputOffsetsAndSizesBuffer.get(),
        outputBuffer.get(),
        outputBufferSize,
        outputOffsetsAndSizesBuffer.get(),
        totalOffsetsAndSizes,
        &totalStageCount);

    struct DecompressionStageResources
    {
        com_ptr<ID3D12Heap> defaultHeap;
        uint64_t defaultHeapSizeBytes = 0;
        com_ptr<ID3D12Heap> uploadHeap;
        uint64_t uploadHeapSizeBytes = 0;
        com_ptr<ID3D12Heap> readbackHeap;
        uint64_t readbackHeapSizeBytes = 0;
        com_ptr<ID3D12DescriptorHeap> shaderVisibleDescriptorHeap;
        uint32_t shaderVisibleDescriptorCount = 0;
    };

    std::vector<DecompressionStageResources> stageResources(totalStageCount);
    for (uint32_t stage = 0; stage < totalStageCount; ++stage)
    {
        auto& resources = stageResources[stage];
        if (session->TryGetHeapMemorySizes(
                stage,
                &resources.defaultHeapSizeBytes,
                &resources.uploadHeapSizeBytes,
                &resources.readbackHeapSizeBytes,
                &resources.shaderVisibleDescriptorCount))
        {
            if (resources.defaultHeapSizeBytes)
                resources.defaultHeap =
                    CreateHeap(resources.defaultHeapSizeBytes, D3D12_HEAP_TYPE_DEFAULT, L"Default Heap");

            if (resources.uploadHeapSizeBytes)
                resources.uploadHeap =
                    CreateHeap(resources.uploadHeapSizeBytes, D3D12_HEAP_TYPE_UPLOAD, L"Upload Heap");

            if (resources.readbackHeapSizeBytes)
                resources.readbackHeap =
                    CreateHeap(resources.readbackHeapSizeBytes, D3D12_HEAP_TYPE_READBACK, L"Readback Heap");

            if (resources.shaderVisibleDescriptorCount)
                resources.shaderVisibleDescriptorHeap =
                    CreateDescriptorHeap(resources.shaderVisibleDescriptorCount, L"Shader Visible Descriptor Heap");

            m_commandList->Reset(m_commandAllocator.get(), nullptr);
            session->AddToCommandlist(
                stage,
                resources.defaultHeap.get(),
                0,
                resources.uploadHeap.get(),
                0,
                resources.readbackHeap.get(),
                0,
                resources.shaderVisibleDescriptorHeap.get(),
                0,
                m_commandList.get());
            m_commandList->Close();

            ExecuteCommandListsSynchronously(m_commandList.get());
        }
    }

    std::vector<uint8_t> decompressedData;
    CopyFromResource(decompressedData, outputBuffer.get());

    Frames decompressedFrames;
    for (size_t i = 0; i < offsetsAndSizes.OutputOffsets.size(); ++i)
    {
        uint32_t offset = offsetsAndSizes.OutputOffsets[i].offs;
        uint32_t size = offsetsAndSizes.OutputOffsets[i].size;
        decompressedFrames.emplace_back(decompressedData.begin() + offset, decompressedData.begin() + offset + size);
    }

    return decompressedFrames;
}

winrt::com_ptr<ID3D12Resource> ZstdDecompressionWork::CreateBuffer(size_t size, const wchar_t* name)
{
    com_ptr<ID3D12Resource> resource;
    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);

    check_hresult(m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(resource.put())));

    resource->SetName(name);
    return resource;
}

winrt::com_ptr<ID3D12Heap> ZstdDecompressionWork::CreateHeap(size_t size, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_DESC desc = CD3DX12_HEAP_DESC(size, type, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    com_ptr<ID3D12Heap> heap;
    check_hresult(m_device->CreateHeap(&desc, IID_PPV_ARGS(&heap)));
    check_hresult(heap->SetName(name));
    return heap;
}

winrt::com_ptr<ID3D12DescriptorHeap> ZstdDecompressionWork::CreateDescriptorHeap(
    size_t numDescriptors,
    const wchar_t* name)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = static_cast<UINT>(numDescriptors);
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    com_ptr<ID3D12DescriptorHeap> descriptorHeap;
    check_hresult(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap)));
    check_hresult(descriptorHeap->SetName(name));
    return descriptorHeap;
}
