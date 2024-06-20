//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#define NOMINMAX

#include "CustomDecompression.h"

#include <dstorage.h>
#include <dxgi1_4.h>
#include <winrt/base.h>
#include <winrt/windows.applicationmodel.datatransfer.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using winrt::check_hresult;
using winrt::com_ptr;

void SetClipboardText(std::wstring const& str);

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

void ShowHelpText()
{
    std::cout << "Compresses a file, saves it to disk, and then loads & decompresses using DirectStorage." << std::endl
              << std::endl;
    std::cout << "USAGE: GpuDecompressionBenchmark <path> [chunk size in MiB]" << std::endl << std::endl;
    std::cout << "       Default chunk size is 16." << std::endl;
}

struct ChunkMetadata
{
    uint32_t Offset;
    uint32_t CompressedSize;
    uint32_t UncompressedSize;
};

struct Metadata
{
    uint32_t UncompressedSize;
    uint32_t CompressedSize;
    uint32_t LargestCompressedChunkSize;
    std::vector<ChunkMetadata> Chunks;
};

Metadata GenerateUncompressedMetadata(wchar_t const* filename, uint32_t chunkSizeBytes)
{
    ScopedHandle inHandle(
        CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    winrt::check_bool(inHandle.get());

    DWORD size = GetFileSize(inHandle.get(), nullptr);

    Metadata metadata;
    metadata.UncompressedSize = size;
    metadata.CompressedSize = size;
    metadata.LargestCompressedChunkSize = chunkSizeBytes;

    uint32_t offset = 0;

    while (offset < size)
    {
        uint32_t chunkSize = std::min<uint32_t>(size - offset, chunkSizeBytes);

        metadata.Chunks.push_back({offset, chunkSize, chunkSize});
        offset += chunkSize;
    }

    return metadata;
}

com_ptr<IDStorageCompressionCodec> GetCodec(DSTORAGE_COMPRESSION_FORMAT format)
{
    com_ptr<IDStorageCompressionCodec> codec;
    switch (format)
    {
    case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE:
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
    const wchar_t* compressedFilename,
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

    ScopedHandle outHandle(CreateFile(
        compressedFilename,
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    winrt::check_bool(outHandle.get());

    uint32_t numChunks = (size + chunkSizeBytes - 1) / chunkSizeBytes;

    std::wcout << "Compressing " << originalFilename << " to " << compressedFilename << " in " << numChunks << "x"
               << chunkSizeBytes / 1024 / 1024 << " MiB chunks" << std::endl;

    using Chunk = std::vector<uint8_t>;

    std::vector<Chunk> chunks;
    std::vector<uint32_t> chunkOffsets;

    chunks.resize(numChunks);
    for (uint32_t i = 0; i < numChunks; ++i)
    {
        uint32_t thisChunkOffset = i * chunkSizeBytes;
        chunkOffsets.push_back(thisChunkOffset);
    }

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
                        chunk.data(),
                        chunk.size(),
                        &compressedSize));
                    chunk.resize(compressedSize);

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
    metadata.UncompressedSize = size;
    metadata.LargestCompressedChunkSize = 0;

    for (uint32_t i = 0; i < numChunks; ++i)
    {
        winrt::check_bool(
            WriteFile(outHandle.get(), chunks[i].data(), static_cast<DWORD>(chunks[i].size()), nullptr, nullptr));

        uint32_t thisChunkOffset = i * chunkSizeBytes;
        uint32_t thisChunkSize = std::min<uint32_t>(size - thisChunkOffset, chunkSizeBytes);

        ChunkMetadata chunkMetadata{};
        chunkMetadata.Offset = offset;
        chunkMetadata.CompressedSize = static_cast<uint32_t>(chunks[i].size());
        chunkMetadata.UncompressedSize = thisChunkSize;
        metadata.Chunks.push_back(chunkMetadata);

        totalCompressedSize += chunkMetadata.CompressedSize;
        offset += chunkMetadata.CompressedSize;

        metadata.LargestCompressedChunkSize =
            std::max(metadata.LargestCompressedChunkSize, chunkMetadata.CompressedSize);
    }

    outHandle.reset();

    metadata.CompressedSize = totalCompressedSize;

    std::cout << "Total: " << size << " --> " << totalCompressedSize << " bytes (" << totalCompressedSize * 100.0 / size
              << "%)     " << std::endl;

    return metadata;
}

static uint64_t GetProcessCycleTime()
{
    ULONG64 cycleTime;

    winrt::check_bool(QueryProcessCycleTime(GetCurrentProcess(), &cycleTime));

    return cycleTime;
}

struct TestResult
{
    double Bandwidth;
    uint64_t ProcessCycles;
};

TestResult RunTest(
    IDStorageFactory* factory,
    uint32_t stagingSizeMiB,
    wchar_t const* sourceFilename,
    DSTORAGE_COMPRESSION_FORMAT compressionFormat,
    Metadata const& metadata,
    int numRuns)
{
    com_ptr<IDStorageFile> file;

    HRESULT hr = factory->OpenFile(sourceFilename, IID_PPV_ARGS(file.put()));
    if (FAILED(hr))
    {
        std::wcout << L"The file '" << sourceFilename << L"' could not be opened. HRESULT=0x" << std::hex << hr
                   << std::endl;
        std::abort();
    }

    // The staging buffer size must be set before any queues are created.
    std::cout << "  " << stagingSizeMiB << " MiB staging buffer: ";
    uint32_t stagingBufferSizeBytes = stagingSizeMiB * 1024 * 1024;
    check_hresult(factory->SetStagingBufferSize(stagingBufferSizeBytes));

    if (metadata.LargestCompressedChunkSize > stagingBufferSizeBytes)
    {
        std::cout << " SKIPPED! " << std::endl;
        return {0, 0};
    }

    com_ptr<ID3D12Device> device;
    check_hresult(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));

    // Create a DirectStorage queue which will be used to load data into a
    // buffer on the GPU.
    DSTORAGE_QUEUE_DESC queueDesc{};
    queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
    queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
    queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    queueDesc.Device = device.get();

    com_ptr<IDStorageQueue> queue;
    check_hresult(factory->CreateQueue(&queueDesc, IID_PPV_ARGS(queue.put())));

    // Create the ID3D12Resource buffer which will be populated with the file's contents
    D3D12_HEAP_PROPERTIES bufferHeapProps = {};
    bufferHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = metadata.UncompressedSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.SampleDesc.Count = 1;

    com_ptr<ID3D12Resource> bufferResource;
    check_hresult(device->CreateCommittedResource(
        &bufferHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(bufferResource.put())));

    // Configure a fence to be signaled when the request is completed
    com_ptr<ID3D12Fence> fence;
    check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));

    ScopedHandle fenceEvent(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    uint64_t fenceValue = 1;

    double meanBandwidth = 0;
    uint64_t meanCycleTime = 0;

    for (int i = 0; i < numRuns; ++i)
    {
        check_hresult(fence->SetEventOnCompletion(fenceValue, fenceEvent.get()));

        // Enqueue requests to load each compressed chunk.
        uint32_t destOffset = 0;
        for (auto const& chunk : metadata.Chunks)
        {
            DSTORAGE_REQUEST request = {};
            request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
            request.Options.CompressionFormat = compressionFormat;
            request.Source.File.Source = file.get();
            request.Source.File.Offset = chunk.Offset;
            request.Source.File.Size = chunk.CompressedSize;
            request.UncompressedSize = chunk.UncompressedSize;
            request.Destination.Buffer.Resource = bufferResource.get();
            request.Destination.Buffer.Offset = destOffset;
            request.Destination.Buffer.Size = chunk.UncompressedSize;
            queue->EnqueueRequest(&request);
            destOffset += request.UncompressedSize;
        }

        // Signal the fence when done
        queue->EnqueueSignal(fence.get(), fenceValue);

        auto startTime = std::chrono::high_resolution_clock::now();
        auto startCycleTime = GetProcessCycleTime();

        // Tell DirectStorage to start executing all queued items.
        queue->Submit();

        // Wait for the submitted work to complete
        WaitForSingleObject(fenceEvent.get(), INFINITE);

        auto endCycleTime = GetProcessCycleTime();
        auto endTime = std::chrono::high_resolution_clock::now();

        if (fence->GetCompletedValue() == (uint64_t)-1)
        {
            // Device removed!  Give DirectStorage a chance to detect the error.
            Sleep(5);
        }

        // If an error was detected the first failure record
        // can be retrieved to get more details.
        DSTORAGE_ERROR_RECORD errorRecord{};
        queue->RetrieveErrorRecord(&errorRecord);
        if (FAILED(errorRecord.FirstFailure.HResult))
        {
            //
            // errorRecord.FailureCount - The number of failed requests in the queue since the last
            //                            RetrieveErrorRecord call.
            // errorRecord.FirstFailure - Detailed record about the first failed command in the enqueue order.
            //
            std::cout << "The DirectStorage request failed! HRESULT=0x" << std::hex << errorRecord.FirstFailure.HResult
                      << std::endl;

            if (errorRecord.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST)
            {
                auto& r = errorRecord.FirstFailure.Request.Request;

                std::cout << std::dec << "   " << r.Source.File.Offset << "   " << r.Source.File.Size << std::endl;
            }
            std::terminate();
        }
        else
        {
            auto duration = endTime - startTime;

            using dseconds = std::chrono::duration<double>;

            double durationInSeconds = std::chrono::duration_cast<dseconds>(duration).count();
            double bandwidth = (metadata.UncompressedSize / durationInSeconds) / 1000.0 / 1000.0 / 1000.0;
            meanBandwidth += bandwidth;

            meanCycleTime += (endCycleTime - startCycleTime);

            std::cout << ".";
        }
        ++fenceValue;
    }

    meanBandwidth /= numRuns;
    meanCycleTime /= numRuns;

    std::cout << "  " << meanBandwidth << " GB/s"
              << " mean cycle time: " << std::dec << meanCycleTime << std::endl;

    return {meanBandwidth, meanCycleTime};
}

int wmain(int argc, wchar_t* argv[])
{
    enum class TestCase
    {
        Uncompressed,
#if USE_ZLIB
        CpuZLib,
#endif
        CpuGDeflate,
        GpuGDeflate
    };

    TestCase testCases[] =
    { TestCase::Uncompressed,
#if USE_ZLIB
      TestCase::CpuZLib,
#endif
      TestCase::CpuGDeflate,
      TestCase::GpuGDeflate };

    if (argc < 2)
    {
        ShowHelpText();
        return -1;
    }

    const wchar_t* originalFilename = argv[1];
    std::wstring gdeflateFilename = std::wstring(originalFilename) + L".gdeflate";

#if USE_ZLIB
    std::wstring zlibFilename = std::wstring(originalFilename) + L".zlib";
#endif

    uint32_t chunkSizeMiB = 16;
    if (argc > 2)
    {
        chunkSizeMiB = _wtoi(argv[2]);
        if (chunkSizeMiB == 0)
        {
            ShowHelpText();
            std::wcout << std::endl << L"Invalid chunk size: " << argv[2] << std::endl;
            return -1;
        }
    }
    uint32_t chunkSizeBytes = chunkSizeMiB * 1024 * 1024;

    Metadata uncompressedMetadata = GenerateUncompressedMetadata(originalFilename, chunkSizeBytes);
    Metadata gdeflateMetadata =
        Compress(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, originalFilename, gdeflateFilename.c_str(), chunkSizeBytes);

#if USE_ZLIB
    Metadata zlibMetadata =
        Compress(DSTORAGE_CUSTOM_COMPRESSION_0, originalFilename, zlibFilename.c_str(), chunkSizeBytes);
#endif

    constexpr uint32_t MAX_STAGING_BUFFER_SIZE = 1024;

    struct Result
    {
        TestCase TestCase;
        uint32_t StagingBufferSizeMiB;
        TestResult Data;
    };

    std::vector<Result> results;

    for (TestCase testCase : testCases)
    {
        DSTORAGE_COMPRESSION_FORMAT compressionFormat;
        DSTORAGE_CONFIGURATION config{};
        int numRuns = 0;
        Metadata* metadata = nullptr;
        wchar_t const* filename = nullptr;

        switch (testCase)
        {
        case TestCase::Uncompressed:
            compressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
            numRuns = 10;
            metadata = &uncompressedMetadata;
            filename = originalFilename;
            std::cout << "Uncompressed:" << std::endl;
            break;

#if USE_ZLIB
        case TestCase::CpuZLib:
            compressionFormat = DSTORAGE_CUSTOM_COMPRESSION_0;
            numRuns = 2;
            metadata = &zlibMetadata;
            filename = zlibFilename.c_str();
            std::cout << "ZLib:" << std::endl;
            break;
#endif

        case TestCase::CpuGDeflate:
            compressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
            numRuns = 2;

            // When forcing the CPU implementation of GDEFLATE we need to go
            // through the custom decompression path so we can ensure that
            // GDEFLATE doesn't try and decompress directly to an upload heap.
            config.NumBuiltInCpuDecompressionThreads = DSTORAGE_DISABLE_BUILTIN_CPU_DECOMPRESSION;
            config.DisableGpuDecompression = true;

            metadata = &gdeflateMetadata;
            filename = gdeflateFilename.c_str();
            std::cout << "CPU GDEFLATE:" << std::endl;
            break;

        case TestCase::GpuGDeflate:
            compressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
            numRuns = 10;
            metadata = &gdeflateMetadata;
            filename = gdeflateFilename.c_str();
            std::cout << "GPU GDEFLATE:" << std::endl;
            break;

        default:
            std::terminate();
        }

        check_hresult(DStorageSetConfiguration(&config));

        com_ptr<IDStorageFactory> factory;
        check_hresult(DStorageGetFactory(IID_PPV_ARGS(factory.put())));

        factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_BREAK_ON_ERROR);

        CustomDecompression customDecompression(factory.get(), std::thread::hardware_concurrency());

        for (uint32_t stagingSizeMiB = 1; stagingSizeMiB <= MAX_STAGING_BUFFER_SIZE; stagingSizeMiB *= 2)
        {
            if (stagingSizeMiB < chunkSizeMiB)
                continue;

            TestResult data = RunTest(factory.get(), stagingSizeMiB, filename, compressionFormat, *metadata, numRuns);

            results.push_back({testCase, stagingSizeMiB, data});
        }
    }

    std::cout << "\n\n";

    std::wstringstream bandwidth;
    std::wstringstream cycles;

    std::wstring header =
        L"\"Staging Buffer Size MiB\"\t\"Uncompressed\"\t\"ZLib\"\t\"CPU GDEFLATE\"\t\"GPU GDEFLATE\"";
    bandwidth << header << std::endl;
    cycles << header << std::endl;

    for (uint32_t stagingBufferSize = 1; stagingBufferSize <= MAX_STAGING_BUFFER_SIZE; stagingBufferSize *= 2)
    {
        std::wstringstream bandwidthRow;
        std::wstringstream cyclesRow;

        bandwidthRow << stagingBufferSize << "\t";
        cyclesRow << stagingBufferSize << "\t";

        constexpr bool showEmptyRows = true;

        bool foundOne = false;

        for (auto& testCase : testCases)
        {
            auto it = std::find_if(
                results.begin(),
                results.end(),
                [&](Result const& r) { return r.TestCase == testCase && r.StagingBufferSizeMiB == stagingBufferSize; });

            if (it == results.end())
            {
                bandwidthRow << L"\t";
                cyclesRow << L"\t";
            }
            else
            {
                bandwidthRow << it->Data.Bandwidth << L"\t";
                cyclesRow << it->Data.ProcessCycles << L"\t";
                foundOne = true;
            }
        }

        if (showEmptyRows || foundOne)
        {
            bandwidth << bandwidthRow.str() << std::endl;
            cycles << cyclesRow.str() << std::endl;
        }
    }

    std::wstringstream combined;
    combined << "Bandwidth" << std::endl
             << bandwidth.str() << std::endl
             << std::endl
             << "Cycles" << std::endl
             << cycles.str() << std::endl;

    combined << std::endl << "Compression" << std::endl;
    combined << "Case\tSize\tRatio" << std::endl;

    auto ratioLine = [&](char const* name, Metadata const& metadata)
    {
        combined << name << "\t" << metadata.CompressedSize << "\t"
                 << static_cast<double>(metadata.CompressedSize) / static_cast<double>(metadata.UncompressedSize)
                 << std::endl;
    };

    ratioLine("Uncompressed", uncompressedMetadata);
#if USE_ZLIB
    ratioLine("ZLib", zlibMetadata);
#else
    combined << "ZLib" << "\tn/a\tn/a" << std::endl;
#endif
    ratioLine("GDEFLATE", gdeflateMetadata);

    combined << std::endl;

    std::wcout << combined.str();

    try
    {
        SetClipboardText(combined.str());
        std::wcout << "\nThese results have been copied to the clipboard, ready to paste into Excel." << std::endl;
        return 0;
    }
    catch (...)
    {
        std::wcout << "\nFailed to copy results to clipboard. Sorry." << std::endl;
    }

    return 0;
}

void SetClipboardText(std::wstring const& str)
{
    using namespace winrt::Windows::ApplicationModel::DataTransfer;

    DataPackage dataPackage;
    dataPackage.SetText(str);

    Clipboard::SetContent(dataPackage);
    Clipboard::Flush();
}
