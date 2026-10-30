//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#define NOMINMAX

#include "Metadata.h"

#include <winrt/base.h>

#include <algorithm>
#include <filesystem>

// Metadata Format (.metadata)
// ====================================
// A small binary file saved alongside each archive that stores everything
// needed to issue DirectStorage read requests at load time. The file is
// a flat sequence of fields with no alignment padding:
//
//   Offset  Size      Field
//   ------  --------  ----------------------------------------
//   0       4 bytes   Magic number (0xDEADBEEF)
//   4       4 bytes   Chunk size in bytes
//   8       4 bytes   Total uncompressed size of all chunks
//   12      4 bytes   Total compressed size of all chunks
//   16      4 bytes   Largest single compressed chunk size
//   20      8 bytes   Number of chunks (N)
//   28      N * 16    Chunk table (array of ChunkMetadata)
//
// Each ChunkMetadata entry (16 bytes) contains:
//   - Offset           (4 bytes) Byte offset of the chunk within the archive file
//   - CompressedSize   (4 bytes) Size of the compressed chunk data
//   - UncompressedSize (4 bytes) Original size before compression
//   - TransformType    (4 bytes) GACL Shuffle transform applied
//
// At load time, TryLoadMetadata reads this file back and validates the
// magic number and chunk size before populating the Metadata struct.
//
const uint32_t METADATA_MAGIC = 0xDEADBEEF;

void SaveMetadata(const wchar_t* filename, uint32_t chunkSizeBytes, const Metadata& metadata)
{
    std::wstring metaPath = std::wstring(filename) + L".metadata";
    ScopedHandle file(
        CreateFile(metaPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    winrt::check_bool(file.get());

    DWORD written = 0;
    winrt::check_bool(WriteFile(file.get(), &METADATA_MAGIC, sizeof(METADATA_MAGIC), &written, nullptr));
    winrt::check_bool(WriteFile(file.get(), &chunkSizeBytes, sizeof(chunkSizeBytes), &written, nullptr));

    winrt::check_bool(
        WriteFile(file.get(), &metadata.UncompressedSize, sizeof(metadata.UncompressedSize), &written, nullptr));
    winrt::check_bool(
        WriteFile(file.get(), &metadata.CompressedSize, sizeof(metadata.CompressedSize), &written, nullptr));
    winrt::check_bool(
        WriteFile(file.get(), &metadata.LargestCompressedChunkSize, sizeof(metadata.LargestCompressedChunkSize), &written, nullptr));
    winrt::check_bool(
        WriteFile(file.get(), &metadata.ArchiveLastWriteTime, sizeof(metadata.ArchiveLastWriteTime), &written, nullptr));

    uint64_t numChunks = static_cast<uint64_t>(metadata.Chunks.size());
    winrt::check_bool(WriteFile(file.get(), &numChunks, sizeof(numChunks), &written, nullptr));

    if (numChunks > 0)
    {
        winrt::check_bool(WriteFile(
            file.get(),
            metadata.Chunks.data(),
            static_cast<DWORD>(numChunks * sizeof(ChunkMetadata)),
            &written,
            nullptr));
    }
}

bool TryLoadMetadata(const wchar_t* filename, uint32_t expectedChunkSize, Metadata& metadata)
{
    std::wstring metadataPath = std::wstring(filename) + L".metadata";

    if (!std::filesystem::is_regular_file(metadataPath))
    {
        return false;
    }

    ScopedHandle file(CreateFile(
        metadataPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    winrt::check_bool(file.get());

    DWORD read;
    uint32_t magic = 0, version = 0, chunkSize = 0;
    if (!ReadFile(file.get(), &magic, sizeof(magic), &read, nullptr) || magic != METADATA_MAGIC)
        return false;
    if (!ReadFile(file.get(), &chunkSize, sizeof(chunkSize), &read, nullptr) || chunkSize != expectedChunkSize)
        return false;

    if (!ReadFile(file.get(), &metadata.UncompressedSize, sizeof(metadata.UncompressedSize), &read, nullptr))
        return false;
    if (!ReadFile(file.get(), &metadata.CompressedSize, sizeof(metadata.CompressedSize), &read, nullptr))
        return false;
    if (!ReadFile(file.get(), &metadata.LargestCompressedChunkSize, sizeof(metadata.LargestCompressedChunkSize), &read, nullptr))
        return false;

    // Validate the archive file's timestamp still matches what was recorded.
    if (!ReadFile(file.get(), &metadata.ArchiveLastWriteTime, sizeof(metadata.ArchiveLastWriteTime), &read, nullptr))
        return false;
    WIN32_FILE_ATTRIBUTE_DATA archiveAttribs = {};
    if (!GetFileAttributesEx(filename, GetFileExInfoStandard, &archiveAttribs))
        return false;
    if (CompareFileTime(&metadata.ArchiveLastWriteTime, &archiveAttribs.ftLastWriteTime) != 0)
        return false;

    uint64_t numChunks = 0;
    if (!ReadFile(file.get(), &numChunks, sizeof(numChunks), &read, nullptr))
        return false;

    if (numChunks > 0)
    {
        metadata.Chunks.resize(static_cast<size_t>(numChunks));
        if (!ReadFile(
                file.get(),
                metadata.Chunks.data(),
                static_cast<DWORD>(numChunks * sizeof(ChunkMetadata)),
                &read,
                nullptr))
            return false;
    }

    return true;
}
