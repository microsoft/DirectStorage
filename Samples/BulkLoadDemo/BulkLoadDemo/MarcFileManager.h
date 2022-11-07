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
#include "MarcFile.h"

#include <Model.h>

#include <chrono>

//
// MarcFileManager keeps track of MarcFiles.
//
// It manages a single D3D12 heap that the MarcFiles use for storing their GPU
// data as well as a range of GPU descriptors.
//
// Sets of MarcFiles can be loaded or unloaded.
//

class MarcFileManager
{
    struct File
    {
        std::wstring Filename;
        std::unique_ptr<MarcFile> MarcFile;
    };

    std::vector<File> m_files;

    std::unique_ptr<MultiHeap> m_texturesHeap;
    std::unique_ptr<MultiHeap> m_buffersHeap;

    DescriptorHandle m_baseTextureHandle;
    uint32_t m_nextDescriptorHandleIndex = 0;

    enum class State
    {
        LoadingMetadata,
        ReadyToLoad,
        Loading,
        Loaded,
    };

    State m_state = State::LoadingMetadata;

    MarcFile::DataSize m_currentSetSize{};
    size_t m_numLoadedModels;

    EventWait m_loadComplete;

    std::chrono::time_point<std::chrono::high_resolution_clock> m_startLoadTime;
    std::chrono::microseconds m_loadTime;

public:
    MarcFileManager();
    ~MarcFileManager();

    using FileId = size_t;

    FileId Add(std::wstring const& filename);
    void SetNextSet(std::vector<FileId> const& ids);
    std::vector<ModelInstance> CreateInstancesForSet();
    void UnloadSet();

    void Update();

    bool IsReadyToLoad() const;
    bool SetIsLoaded() const;
    bool IsLoading() const;

    struct LoadedDataSize : MarcFile::DataSize
    {
        size_t NumLoadedModels;

    };
    LoadedDataSize GetCurrentSetSize() const;

    using float_seconds = std::chrono::duration<float, std::chrono::seconds::period>;

    float_seconds GetLoadTime() const;
    float_seconds GetTimeSinceLoad() const;

private:
    MarcFile::DataSize TryStartLoad(File& file);

    void OnLoadComplete();

    void AllocateDescriptors();
};