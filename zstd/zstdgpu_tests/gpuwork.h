//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include <d3d12.h>
#include <d3dx12.h>
#include <wil/cppwinrt.h>
#include <wil/resource.h>
#include <wil/result.h>
#include <windows.h>
#include <winrt/base.h>

#include <vector>

#include "libzstdgpu.h"

using winrt::check_hresult;
using winrt::com_ptr;
using namespace Decompression;

class GpuWork
{
public:
    GpuWork(ID3D12Device* device, const wchar_t* name);

    HRESULT GetDeviceRemovedReason() const
    {
        return m_device->GetDeviceRemovedReason();
    }

protected:
    com_ptr<ID3D12Device5> m_device;
    com_ptr<ID3D12CommandQueue> m_commandQueue;
    com_ptr<ID3D12CommandAllocator> m_commandAllocator;
    com_ptr<ID3D12GraphicsCommandList4> m_commandList;
    com_ptr<ID3D12Fence> m_fence;
    wil::unique_event m_event;
    uint64_t m_nextFenceValue = 1;

    com_ptr<ID3D12QueryHeap> m_timestampQueryHeap;

    template<typename... ARGS>
    void ExecuteCommandListsSynchronously(ARGS... args)
    {
        ID3D12CommandList* commandLists[] = {args...};

        for (auto* cl : commandLists)
        {
            m_commandQueue->ExecuteCommandLists(1, &cl);
        }

        m_commandQueue->Signal(m_fence.get(), m_nextFenceValue);
        m_fence->SetEventOnCompletion(m_nextFenceValue, m_event.get());

        ++m_nextFenceValue;

        m_event.wait(INFINITE);

        m_commandAllocator->Reset();
    }

    void CopyFromResource(std::vector<uint8_t>& dst, ID3D12Resource* src)
    {
        auto srcDesc = src->GetDesc();
        size_t srcSize = static_cast<size_t>(srcDesc.Width);
        assert(srcDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

        // Allocate a readback heap large enough for the source data being copied
        com_ptr<ID3D12Resource> readbackBuffer;
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(srcSize);
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        check_hresult(m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readbackBuffer.put())));

        D3D12_RESOURCE_BARRIER barrier{CD3DX12_RESOURCE_BARRIER::Transition(
            src,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE)};

        m_commandList->Reset(m_commandAllocator.get(), nullptr);
        m_commandList->ResourceBarrier(1, &barrier);
        m_commandList->CopyResource(readbackBuffer.get(), src);
        m_commandList->Close();
        ExecuteCommandListsSynchronously(m_commandList.get());
        uint8_t* readbackPtr = nullptr;
        check_hresult(readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&readbackPtr)));
        dst.resize(srcSize);
        memcpy(dst.data(), readbackPtr, srcSize);
        readbackBuffer->Unmap(0, nullptr);
    }

    void CopyToResource(ID3D12Resource* dst, uint8_t* src, size_t srcSize)
    {
        auto dstDesc = dst->GetDesc();
        assert(dstDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        assert(static_cast<size_t>(dstDesc.Width) >= srcSize); // destination large enough for copy size?

        // Allocate an upload heap large enough for the source data being copied
        com_ptr<ID3D12Resource> uploadBuffer;
        auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(srcSize);
        auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        check_hresult(m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(uploadBuffer.put())));

        uint8_t* uploadPtr = nullptr;
        check_hresult(uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&uploadPtr)));
        memcpy(uploadPtr, src, srcSize);

        uploadBuffer->Unmap(0, nullptr);

        m_commandList->Reset(m_commandAllocator.get(), nullptr);
        m_commandList->CopyBufferRegion(dst, 0, uploadBuffer.get(), 0, srcSize);
        m_commandList->Close();

        ExecuteCommandListsSynchronously(m_commandList.get());
    }
};

using Frames = std::vector<std::vector<uint8_t>>;

struct OffsetsAndSizes
{
    uint32_t UnCompressedFramesMemorySizeInBytes;
    std::vector<zstdgpu_OffsetAndSize> InputOffsets;
    std::vector<zstdgpu_OffsetAndSize> OutputOffsets;
};

class ZstdDecompressionWork : public GpuWork
{
    std::unique_ptr<ZstdFallbackShaderDecompressionLibrary> m_zstdgu;

public:
    ZstdDecompressionWork(ID3D12Device* device, const wchar_t* name);

    Frames Decompress(
        std::vector<uint8_t>& compressedData,
        OffsetsAndSizes& offsetsAndSizes,
        std::vector<uint32_t>* outFrameStatus = nullptr);

protected:
    winrt::com_ptr<ID3D12Resource> CreateBuffer(size_t size, const wchar_t* name);
    winrt::com_ptr<ID3D12Heap> CreateHeap(size_t size, D3D12_HEAP_TYPE type, const wchar_t* name);
    winrt::com_ptr<ID3D12DescriptorHeap> CreateDescriptorHeap(
        size_t numDescriptors,
        const wchar_t* name);
};