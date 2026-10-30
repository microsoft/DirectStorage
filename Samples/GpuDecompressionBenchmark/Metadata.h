//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include <windows.h>

struct handle_closer
{
    void operator()(HANDLE h) noexcept
    {
        assert(h != INVALID_HANDLE_VALUE);
        if (h)
        {
            CloseHandle(h);
        }
    }
};

using ScopedHandle = std::unique_ptr<void, handle_closer>;

struct ChunkMetadata
{
    uint32_t Offset = 0;
    uint32_t CompressedSize = 0;
    uint32_t UncompressedSize = 0;
    uint32_t TransformType = 0;

    bool operator==(const ChunkMetadata& rhs) const noexcept
    {
        return Offset == rhs.Offset && CompressedSize == rhs.CompressedSize &&
               UncompressedSize == rhs.UncompressedSize && TransformType == rhs.TransformType;
    }

    bool operator!=(const ChunkMetadata& rhs) const noexcept
    {
        return !(*this == rhs);
    }
};

struct Metadata
{
    uint32_t UncompressedSize = 0;
    uint32_t CompressedSize = 0;
    uint32_t LargestCompressedChunkSize = 0;
    FILETIME ArchiveLastWriteTime = {};
    std::vector<ChunkMetadata> Chunks;

    bool operator==(const Metadata& rhs) const noexcept
    {
        return UncompressedSize == rhs.UncompressedSize && CompressedSize == rhs.CompressedSize &&
               LargestCompressedChunkSize == rhs.LargestCompressedChunkSize &&
               ArchiveLastWriteTime.dwLowDateTime == rhs.ArchiveLastWriteTime.dwLowDateTime &&
               ArchiveLastWriteTime.dwHighDateTime == rhs.ArchiveLastWriteTime.dwHighDateTime &&
               Chunks == rhs.Chunks;
    }

    bool operator!=(const Metadata& rhs) const noexcept
    {
        return !(*this == rhs);
    }
};

bool TryLoadMetadata(const wchar_t* filename, uint32_t expectedChunkSize, Metadata& metadata);
void SaveMetadata(const wchar_t* filename, uint32_t chunkSizeBytes, const Metadata& metadata);
