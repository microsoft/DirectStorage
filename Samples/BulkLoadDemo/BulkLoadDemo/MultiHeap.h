//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include "GraphicsCore.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <optional>
#include <vector>

using Graphics::g_Device;

//
// This manages allocations across multiple heaps.
//
struct MultiHeapAllocation
{
    Microsoft::WRL::ComPtr<ID3D12Heap> Heap;
    uint64_t Offset = 0;
};

class MultiHeap
{
    const uint64_t PerHeapAllocationSize = (4u * 1024u * 1024u * 1024u) - (1024u * 1024u); // 4GB - 1MB allocation

    uint64_t m_totalSize;
    struct HeapEntry
    {
        Microsoft::WRL::ComPtr<ID3D12Heap> Heap;
        uint64_t HeapSizeInBytes = 0;
        uint64_t NextLocalOffset = 0;
    };

    std::vector<HeapEntry> m_heaps;

public:
    MultiHeap() = default;

    MultiHeap(D3D12_HEAP_FLAGS flags, uint64_t totalSize)
        : m_totalSize(totalSize)
    {
        // Create multiple heaps each <= PerHeapAllocationSize up to
        // the configured total size specfied
        D3D12_HEAP_DESC heapDesc{};
        heapDesc.Alignment = 64 * 1024;
        heapDesc.Flags = flags;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;

        size_t totalHeaps = (totalSize / PerHeapAllocationSize) + 1;
        m_heaps.reserve(totalHeaps);

        uint64_t bytesAllocated = 0;
        for (size_t heapIndex = 0; heapIndex < totalHeaps; ++heapIndex)
        {
            heapDesc.SizeInBytes = std::min<uint64_t>(PerHeapAllocationSize, (m_totalSize - bytesAllocated));
            Microsoft::WRL::ComPtr<ID3D12Heap> heap;
            if (FAILED(g_Device->CreateHeap(&heapDesc, IID_PPV_ARGS(&heap))))
            {
                std::abort();
            }

            m_heaps.push_back({std::move(heap), heapDesc.SizeInBytes, 0});
            bytesAllocated += heapDesc.SizeInBytes;
        }
    }

    void Clear()
    {
        for (auto& heapEntry : m_heaps)
        {
            heapEntry.NextLocalOffset = 0;
        }
    }

    bool CanAllocate(std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> allocations)
    {
        auto heaps = m_heaps;
        for (auto& allocation : allocations)
        {
            if (!TryAllocate(heaps, allocation.SizeInBytes))
                return false;
        }
        return true;
    }

    bool CanAllocate(uint64_t sizeInBytes)
    {
        auto heaps = m_heaps;
        auto allocation = TryAllocate(heaps, sizeInBytes);
        return (allocation != std::nullopt);
    }

    std::vector<MultiHeapAllocation> Allocate(std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> const& allocations)
    {
        std::vector<MultiHeapAllocation> heapAllocations;
        heapAllocations.reserve(allocations.size());
        for (auto& allocation : allocations)
        {
            auto alloc = TryAllocate(m_heaps, allocation.SizeInBytes);
            assert(alloc); // It is assumed that CanAllocate( ) was called so allocations must succeed.
            heapAllocations.push_back(std::move(*alloc));
        }

        return heapAllocations;
    }

    MultiHeapAllocation Allocate(uint64_t sizeInBytes)
    {
        auto allocation = TryAllocate(m_heaps, sizeInBytes);
        assert(allocation); // It is assumed that CanAllocate( ) was called so allocations must succeed.
        return *allocation;
    }

private:
    static HeapEntry* TryGetHeapEntryForAllocation(std::vector<HeapEntry>& heaps, uint64_t sizeInBytes)
    {
        for (auto& heapEntry : heaps)
        {
            if (Math::AlignUp(heapEntry.NextLocalOffset + sizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) <
                heapEntry.HeapSizeInBytes)
                return &heapEntry;
        }
        return nullptr;
    }

    static std::optional<MultiHeapAllocation> TryAllocate(std::vector<HeapEntry>& heaps, uint64_t sizeInBytes)
    {
        MultiHeapAllocation allocation{};
        auto* heapEntry = TryGetHeapEntryForAllocation(heaps, sizeInBytes);
        if (heapEntry)
        {
            allocation.Heap = heapEntry->Heap;
            allocation.Offset = heapEntry->NextLocalOffset;
            heapEntry->NextLocalOffset =
                Math::AlignUp(heapEntry->NextLocalOffset + sizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
            return allocation;
        }

        return std::nullopt;
    }
};
