//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "MarcFileManager.h"

#include "DStorageLoader.h"

#include <Renderer.h>

#include <algorithm>

MarcFileManager::MarcFileManager()
    : m_loadComplete(EventWait::Create<MarcFileManager, &MarcFileManager::OnLoadComplete>(this))
{
    ComPtr<IDXGIFactory4> dxgiFactory;
    if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&dxgiFactory))))
    {
        std::abort();
    }

    ComPtr<IDXGIAdapter3> dxgiAdapter;
    if (FAILED(dxgiFactory->EnumAdapterByLuid(Graphics::g_Device->GetAdapterLuid(), IID_PPV_ARGS(&dxgiAdapter))))
    {
        std::abort();
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
    if (FAILED(dxgiAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo)))
    {
        dxgiAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &videoMemoryInfo);
    }

    UINT64 maxAllocationSize = ((videoMemoryInfo.Budget * 3) / 4); // 3/4 total gpu memory budget

    UINT64 totalTexturesMemorySize = ((maxAllocationSize * 3) / 4); // 3/4 of gpu budget for textures
    Utility::Printf("Using %f GiB of heap(s) for textures\n", totalTexturesMemorySize / 1024.0 / 1024.0 / 1024.0);

    m_texturesHeap =
        std::make_unique<MultiHeap>(D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES, totalTexturesMemorySize);

    UINT64 totalBuffersMemorySize = (maxAllocationSize / 4); // 1/4 of gpu budget for buffers
    Utility::Printf("Using %f GiB of heap(s) for buffers\n", totalBuffersMemorySize / 1024.0 / 1024.0 / 1024.0);

    m_buffersHeap = std::make_unique<MultiHeap>(D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS, totalBuffersMemorySize);

    m_state = State::LoadingMetadata;
}

MarcFileManager::~MarcFileManager()
{
}

MarcFileManager::FileId MarcFileManager::Add(std::wstring const& filename)
{
    File f;

    f.Filename = filename;
    f.MarcFile = std::make_unique<MarcFile>(filename);
    f.MarcFile->StartMetadataLoad();

    auto id = m_files.size();
    m_files.push_back(std::move(f));

    return id;
}

//
// SetNextSet attempts to load the content for all of the passed in files, in
// order.  If there's not enough space in the heap then the file is silently
// skipped.
//
void MarcFileManager::SetNextSet(std::vector<FileId> const& ids)
{
    assert(m_state == State::ReadyToLoad);

    m_nextDescriptorHandleIndex = 0;

    m_buffersHeap->Clear();
    m_texturesHeap->Clear();

    m_currentSetSize = MarcFile::DataSize{};
    m_numLoadedModels = 0;
    m_startLoadTime = std::chrono::high_resolution_clock::now();

    for (auto id : ids)
    {
        auto size = TryStartLoad(m_files[id]);

        if ((size.TexturesByteCount + m_currentSetSize.BuffersByteCount) > 0)
            m_numLoadedModels++;

        m_currentSetSize.CpuByteCount += size.CpuByteCount;
        m_currentSetSize.TexturesByteCount += size.TexturesByteCount;
        m_currentSetSize.BuffersByteCount += size.BuffersByteCount;
        m_currentSetSize.NumTextureHandles += size.NumTextureHandles;
        m_currentSetSize.GDeflateByteCount += size.GDeflateByteCount;
        m_currentSetSize.ZLibByteCount += size.ZLibByteCount;
        m_currentSetSize.UncompressedByteCount += size.UncompressedByteCount;
    }

    m_loadComplete.SetThreadpoolWait();
    g_dsGpuQueue->EnqueueSetEvent(m_loadComplete);
    g_dsGpuQueue->Submit();

    m_state = State::Loading;
}

void MarcFileManager::OnLoadComplete()
{
    using namespace std::chrono;

    auto loadTime = high_resolution_clock::now() - m_startLoadTime;
    m_loadTime = duration_cast<milliseconds>(loadTime);
}

void MarcFileManager::Update()
{
    bool allMetadataReady = true;
    bool allLoaded = true;

    for (auto& file : m_files)
    {
        switch (file.MarcFile->GetState())
        {
        case MarcFile::State::Initializing:
            allMetadataReady = allLoaded = false;
            break;

        case MarcFile::State::ContentLoading:
            allLoaded = false;
            break;
        }
    }

    using namespace std::chrono_literals;

    switch (m_state)
    {
    case State::LoadingMetadata:
        if (allMetadataReady)
        {
            AllocateDescriptors();
            m_state = State::ReadyToLoad;
        }
        break;

    case State::ReadyToLoad:
        // nothing
        break;

    case State::Loading:
        if (allLoaded)
        {
            m_state = State::Loaded;
        }
        break;

    case State::Loaded:
        break;
    }
}

void MarcFileManager::AllocateDescriptors()
{
    uint32_t descriptorCount = 0;
    for (auto& file : m_files)
    {
        if (file.MarcFile)
            descriptorCount += file.MarcFile->GetRequiredDataSize().NumTextureHandles;
    }

    m_baseTextureHandle = Renderer::s_TextureHeap.Alloc(descriptorCount);
    m_nextDescriptorHandleIndex = 0;
}

std::vector<ModelInstance> MarcFileManager::CreateInstancesForSet()
{
    std::vector<ModelInstance> instances;
    instances.reserve(m_files.size());

    for (auto& file : m_files)
    {
        if (file.MarcFile->GetState() != MarcFile::State::ContentLoaded)
            continue;

        instances.emplace_back(file.MarcFile->GetModel());
    }

    return instances;
}

void MarcFileManager::UnloadSet()
{
    // Unload anything already loaded
    for (auto& file : m_files)
    {
        file.MarcFile->UnloadContent();
    }

    m_state = State::ReadyToLoad;
}

MarcFile::DataSize MarcFileManager::TryStartLoad(File& file)
{
    if (file.MarcFile->GetState() != MarcFile::State::ReadyToLoadContent)
    {
        // Something's wrong with this file so we can't load it.
        return {};
    }

    // Is there enough space to store the contents of this file?
    auto allocationInfos = file.MarcFile->GetTextureAllocationInfos();
    auto requiredDataSize = file.MarcFile->GetRequiredDataSize();
    if (!m_texturesHeap->CanAllocate(allocationInfos) ||
        !m_buffersHeap->CanAllocate(requiredDataSize.BuffersByteCount))
    {
        // out of space
        return {};
    }

    auto increment = Graphics::g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    DescriptorHandle textureHandles = m_baseTextureHandle + m_nextDescriptorHandleIndex * increment;
    m_nextDescriptorHandleIndex += requiredDataSize.NumTextureHandles;

    auto textureAllocations = m_texturesHeap->Allocate(allocationInfos);
    auto buffersAllocation = m_buffersHeap->Allocate(requiredDataSize.BuffersByteCount);

    file.MarcFile->StartContentLoad(
        textureAllocations,
        textureHandles,
        buffersAllocation);

    return requiredDataSize;
}

bool MarcFileManager::IsReadyToLoad() const
{
    return m_state == State::ReadyToLoad;
}

bool MarcFileManager::IsLoading() const
{
    return m_state == State::Loading;
}

bool MarcFileManager::SetIsLoaded() const
{
    return m_state == State::Loaded;
}

MarcFileManager::LoadedDataSize MarcFileManager::GetCurrentSetSize() const
{
    LoadedDataSize s = {m_currentSetSize, m_numLoadedModels};
    return s;
}

MarcFileManager::float_seconds MarcFileManager::GetLoadTime() const
{
    return m_loadTime;
}

MarcFileManager::float_seconds MarcFileManager::GetTimeSinceLoad() const
{
    return std::chrono::high_resolution_clock::now() - m_startLoadTime;
}
