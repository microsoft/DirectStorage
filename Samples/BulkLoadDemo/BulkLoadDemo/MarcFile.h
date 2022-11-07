//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include "EventWait.h"
#include "MultiHeap.h"
#include "MarcFileFormat.h"
#include "MemoryRegion.h"

#include <dstorage.h>
#include <wrl/client.h>

#include <filesystem>
#include <memory>

using Microsoft::WRL::ComPtr;

class MarcFile
{
    mutable std::mutex m_mutex;

    ComPtr<IDStorageFile> m_file;
    ComPtr<IDStorageStatusArray> m_statusArray;

    // Metadata
    marc::Header m_header{};
    MemoryRegion<marc::CpuMetadataHeader> m_cpuMetadata;
    std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> m_textureAllocationInfos;
    D3D12_RESOURCE_ALLOCATION_INFO m_overallTextureAllocationInfo;
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;

    // Content
    MemoryRegion<marc::CpuDataHeader> m_cpuData;
    std::vector<ComPtr<ID3D12Resource>> m_textures;
    ComPtr<ID3D12Resource> m_gpuBuffer;
    DescriptorHandle m_textureHandles;

    // Model
    std::shared_ptr<Model> m_model;

    enum class StatusArrayEntry : uint32_t
    {
        Metadata,
        CpuData,
        GpuData,
        NumEntries
    };

    enum class InternalState
    {
        FileOpen,
        LoadingHeader,
        LoadingCpuMetadata,
        MetadataReady,
        LoadingContent,
        CpuDataLoaded,
        GpuDataLoaded,
        ContentLoaded,
        Error
    };
    InternalState m_state = InternalState::FileOpen;

    HRESULT m_status = S_OK;

    EventWait m_headerLoaded;
    EventWait m_cpuMetadataLoaded;
    EventWait m_cpuDataLoaded;
    EventWait m_gpuDataLoaded;

public:
    explicit MarcFile(std::filesystem::path const& path);
    ~MarcFile();

    void StartMetadataLoad();
    void StartContentLoad(
        std::vector<MultiHeapAllocation> const& texturesAllocations,
        DescriptorHandle textureHandles,
        MultiHeapAllocation buffersAllocation);

    // immediately destroys all data loaded - it is up to the caller to ensure
    // that the GPU isn't using it
    void UnloadContent();

    enum class State
    {
        Initializing,
        ReadyToLoadContent,
        ContentLoading,
        ContentLoaded,
        Error
    };

    State GetState() const;

    // The DataSize is used to determine how much memory / how many descriptor
    // handles need to be allocated, as well as some interesting statistics for
    // the demo.
    struct DataSize
    {
        size_t CpuByteCount;
        uint64_t TexturesByteCount;
        uint64_t BuffersByteCount;
        uint64_t GpuAlignment;
        uint32_t NumTextureHandles;
        size_t GDeflateByteCount;
        size_t ZLibByteCount;
        size_t UncompressedByteCount;
    };

    DataSize GetRequiredDataSize() const;

    std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> const& GetTextureAllocationInfos() const;

    std::shared_ptr<Model> GetModel();

private:
    bool IsMetadataReady() const;

    void OnHeaderLoaded();
    void OnCpuMetadataLoaded();

    void LoadCpuData();
    void LoadGpuData(
        std::vector<MultiHeapAllocation> const& texturesAllocations,
        MultiHeapAllocation buffersAllocation);

    void OnCpuDataLoaded();
    void OnGpuDataLoaded();

    void OnAllDataLoaded();

    void CreateTextureDescriptors();
    void FixupMaterials();

    void CheckHR(HRESULT hr);

    template<typename... States>
    void ValidateState(States... states) const;

    template<typename... States>
    bool StateIsOneOf(States... states) const;

    template<typename T>
    void EnqueueRead(uint64_t offset, T* dest);

    template<typename T>
    MemoryRegion<T> EnqueueReadMemoryRegion(marc::Region<T> const& region);

    ComPtr<ID3D12Resource> EnqueueReadBufferRegion(ID3D12Heap* heap, uint64_t offset, marc::GpuRegion const& region);

    ComPtr<ID3D12Resource> EnqueueReadTexture(
        ID3D12Heap* heap,
        uint64_t offset,
        D3D12_RESOURCE_DESC const& desc,
        marc::TextureMetadata const& textureMetadata);

    template<typename T>
    DSTORAGE_REQUEST BuildRequestForRegion(marc::Region<T> const& region);

    template<void (MarcFile::*FN)()>
    EventWait CreateEventWait();

    // assumes lock is held
    bool IsOk() const;
};
