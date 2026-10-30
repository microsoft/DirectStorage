//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#define NOMINMAX

#include "Compression.h"
#include "UncompressedCodec.h"
#include "ZlibCodec.h"

#include <dstorage.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>

using winrt::check_hresult;
using winrt::com_ptr;

com_ptr<IDStorageCompressionCodec> GetCodec(DSTORAGE_COMPRESSION_FORMAT format)
{
    com_ptr<IDStorageCompressionCodec> codec;
    switch (format)
    {
    case DSTORAGE_COMPRESSION_FORMAT_NONE:
        codec = winrt::make<UncompressedCodec>();
        break;
    case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE:
        check_hresult(DStorageCreateCompressionCodec(format, 0, IID_PPV_ARGS(codec.put())));
        break;

    case DSTORAGE_COMPRESSION_FORMAT_ZSTD:
        check_hresult(DStorageCreateCompressionCodec(format, 0, IID_PPV_ARGS(codec.put())));
        break;

#if USE_ZLIB
    case DSTORAGE_CUSTOM_COMPRESSION_0:
        codec = winrt::make<ZLibCodec>();
        break;
#endif

    default:
        std::terminate();
    }

    return codec;
}

Metadata Compress(
    DSTORAGE_COMPRESSION_FORMAT format,
    const wchar_t* originalFilename,
    std::vector<uint8_t>& compressedData,
    uint32_t chunkSizeBytes)
{
    ScopedHandle inHandle(CreateFile(
        originalFilename,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    winrt::check_bool(inHandle.get());

    DWORD size = GetFileSize(inHandle.get(), nullptr);

    ScopedHandle inMapping(CreateFileMapping(inHandle.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    winrt::check_bool(inMapping.get());

    uint8_t* srcData = reinterpret_cast<uint8_t*>(MapViewOfFile(inMapping.get(), FILE_MAP_READ, 0, 0, size));
    winrt::check_bool(srcData);

    struct Chunk
    {
        Chunk() {};
        Chunk(size_t size)
        {
            Data.resize(size);
        }
        std::vector<uint8_t> Data;
        size_t UncompressedSize = 0;
        uint32_t TransformType = DSTORAGE_GACL_SHUFFLE_TRANSFORM_NONE;
    };

    uint32_t numChunks = (size + chunkSizeBytes - 1) / chunkSizeBytes;

    std::wcout << "Compressing " << originalFilename << " to data buffers in " << numChunks << "x"
               << chunkSizeBytes / 1024 << " KiB chunks" << std::endl;

    std::vector<Chunk> chunks;
    chunks.resize(numChunks);

    std::atomic<size_t> nextChunk = 0;

    std::vector<std::thread> threads;
    threads.reserve(std::thread::hardware_concurrency());

    for (unsigned int i = 0; i < std::thread::hardware_concurrency(); ++i)
    {
        threads.emplace_back(
            [&]()
            {
                // Each thread needs its own instance of the codec
                com_ptr<IDStorageCompressionCodec> codec = GetCodec(format);

                while (true)
                {
                    size_t chunkIndex = nextChunk.fetch_add(1);
                    if (chunkIndex >= numChunks)
                        return;

                    size_t thisChunkOffset = chunkIndex * chunkSizeBytes;
                    size_t thisChunkSize = std::min<size_t>(size - thisChunkOffset, chunkSizeBytes);

                    Chunk chunk(codec->CompressBufferBound(thisChunkSize));

                    uint8_t* uncompressedStart = srcData + thisChunkOffset;

                    size_t compressedSize = 0;
                    check_hresult(codec->CompressBuffer(
                        uncompressedStart,
                        thisChunkSize,
                        DSTORAGE_COMPRESSION_BEST_RATIO,
                        chunk.Data.data(),
                        chunk.Data.size(),
                        &compressedSize));
                    chunk.Data.resize(compressedSize);
                    chunk.UncompressedSize = thisChunkSize;
                    chunk.TransformType = DSTORAGE_GACL_SHUFFLE_TRANSFORM_NONE;

                    chunks[chunkIndex] = std::move(chunk);
                }
            });
    }

    size_t lastNextChunk = std::numeric_limits<size_t>::max();

    do
    {
        Sleep(250);
        if (nextChunk != lastNextChunk)
        {
            lastNextChunk = nextChunk;
            std::cout << "   " << std::min<size_t>(numChunks, lastNextChunk + 1) << " / " << numChunks << "   \r";
            std::cout.flush();
        }
    } while (lastNextChunk < numChunks);

    for (auto& thread : threads)
    {
        thread.join();
    }

    uint32_t totalCompressedSize = 0;
    uint32_t offset = 0;

    Metadata metadata;
    metadata.CompressedSize = 0;
    metadata.UncompressedSize = 0;
    metadata.LargestCompressedChunkSize = 0;

    for (uint32_t i = 0; i < numChunks; ++i)
    {
        metadata.CompressedSize += static_cast<uint32_t>(chunks[i].Data.size());
        metadata.UncompressedSize += static_cast<uint32_t>(chunks[i].UncompressedSize);

        // Copy data to buffer in memory
        compressedData.reserve(compressedData.size() + chunks[i].Data.size());
        compressedData.insert(compressedData.end(), chunks[i].Data.begin(), chunks[i].Data.end());

        uint32_t thisChunkSize = static_cast<uint32_t>(chunks[i].UncompressedSize);

        ChunkMetadata chunkMetadata{};
        chunkMetadata.Offset = offset;
        chunkMetadata.CompressedSize = static_cast<uint32_t>(chunks[i].Data.size());
        chunkMetadata.UncompressedSize = thisChunkSize;
        chunkMetadata.TransformType = static_cast<uint32_t>(chunks[i].TransformType);
        metadata.Chunks.push_back(chunkMetadata);

        totalCompressedSize += chunkMetadata.CompressedSize;
        offset += chunkMetadata.CompressedSize;

        metadata.LargestCompressedChunkSize =
            std::max(metadata.LargestCompressedChunkSize, chunkMetadata.CompressedSize);

        // Free already copied chunk memory
        chunks[i].Data.clear();
    }

    std::cout << "Total: " << size << " --> " << totalCompressedSize << " bytes (" << totalCompressedSize * 100.0 / size
              << "%)     " << std::endl;

    return metadata;
}

// Archive Format
// ==============
// Archive file (.gdeflate / .zstd / .uncompressed / .zlib / etc.):
//
//   +------------------------+-----------+-------+-------------+
//   |  Chunk 0 (compressed)  |  Chunk 1  |  ...  |  Chunk N-1  |
//   +------------------------+-----------+-------+-------------+
//
//   All input files are concatenated and split into fixed-size chunks
//   (default 256 KiB uncompressed). Each chunk is independently
//   compressed, so DirectStorage can decompress them in parallel.
//   Chunks are tightly packed with no padding or headers.
//
// Caching: If a valid metadata file already exists on disk for the
// requested chunk size, the archive is reused without recompressing.
//
Metadata CompressToArchive(
    DSTORAGE_COMPRESSION_FORMAT format,
    std::vector<std::filesystem::path>& files,
    const wchar_t* archiveFilename,
    uint32_t chunkSizeBytes,
    bool bValidate)
{
    Metadata archiveMetadata = {};

    if (TryLoadMetadata(archiveFilename, chunkSizeBytes, archiveMetadata))
    {
        std::wcout << L"Using cached archive: " << archiveFilename << std::endl;
        return archiveMetadata;
    }
    else
    {
        std::wcout << L"No valid cached metadata found for " << archiveFilename << L", generating archive..." << std::endl;
    }

    ScopedHandle outHandle(CreateFile(
        archiveFilename,
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    winrt::check_bool(outHandle.get());

    std::vector<Metadata> archiveFileMetadata;
    std::vector<uint8_t> archiveData;
    for (auto& file : files)
    {
        archiveFileMetadata.push_back(Compress(format, file.c_str(), archiveData, chunkSizeBytes));
    }

    // Construct archive metadata and adjust chunk offsets
    uint32_t chunkOffset = 0;
    bool adjustOffsets = false;
    for (auto& fileMetadata : archiveFileMetadata)
    {
        int offset = 0;
        if (adjustOffsets)
        {
            // Grab the last chunk offset in the archive
            // and use it as the base offset for the next file's chunks.
            chunkOffset = archiveMetadata.Chunks.back().Offset + archiveMetadata.Chunks.back().CompressedSize;
        }

        for (auto& chunk : fileMetadata.Chunks)
        {
            ChunkMetadata chunkInArchive = chunk;
            chunkInArchive.Offset += chunkOffset;
            archiveMetadata.Chunks.push_back(chunkInArchive);
        }
        archiveMetadata.CompressedSize += fileMetadata.CompressedSize;
        archiveMetadata.UncompressedSize += fileMetadata.UncompressedSize;
        archiveMetadata.LargestCompressedChunkSize =
            std::max(archiveMetadata.LargestCompressedChunkSize, fileMetadata.LargestCompressedChunkSize);

        adjustOffsets = true;
    }

    winrt::check_bool(
        WriteFile(outHandle.get(), archiveData.data(), static_cast<DWORD>(archiveData.size()), nullptr, nullptr));

    outHandle.reset();

    // Capture the archive file's last-write time before saving the metadata
    WIN32_FILE_ATTRIBUTE_DATA archiveAttribs = {};
    winrt::check_bool(GetFileAttributesEx(archiveFilename, GetFileExInfoStandard, &archiveAttribs));
    archiveMetadata.ArchiveLastWriteTime = archiveAttribs.ftLastWriteTime;

    SaveMetadata(archiveFilename, chunkSizeBytes, archiveMetadata);

    if (bValidate)
    {
        Metadata loadedMetadata = {};
        if (!TryLoadMetadata(archiveFilename, chunkSizeBytes, loadedMetadata))
        {
            std::wcerr << L"Validation failed: could not reload saved metadata from disk." << std::endl;
            std::terminate();
        }

        if (archiveMetadata != loadedMetadata)
        {
            std::wcerr << L"Validation failed: saved metadata does not match the reloaded metadata." << std::endl;
            std::terminate();
        }

        std::wcout << L"Validation passed: saved metadata matches the reloaded metadata." << std::endl;
    }

    return archiveMetadata;
}
