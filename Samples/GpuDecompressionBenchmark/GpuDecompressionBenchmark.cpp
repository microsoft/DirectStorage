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
#include "CustomDecompression.h"
#include "Metadata.h"
#include "Validation.h"

#include "DStorageTestContext.h"

#include <winrt/windows.applicationmodel.datatransfer.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>

using winrt::check_hresult;
using winrt::com_ptr;

void SetClipboardText(std::wstring const& str);

void ShowHelpText()
{
    std::cout << "Compresses a file or set of files, saves it to disk, and then loads & decompresses using DirectStorage." << std::endl
              << std::endl;
    std::cout << "USAGE: GpuDecompressionBenchmark <path> [-chunksize:chunk size in KiB] [-sustained:seconds] [-validate]" << std::endl << std::endl;
    std::cout << "       <path>: If a directory is specifed then the contents of that directory will be enumerated and collected into a" << std::endl;
    std::cout << "               single chunked archive for benchmarking. " << std::endl;
    std::cout << "       [-chunksize:chunk size in KiB]: Default chunk size is 256KiB." << std::endl;
    std::cout << "       [-sustained:seconds]: Measure sustained throughput for N seconds with the DirectStorage queue kept continuously fed" << std::endl;
    std::cout << "       [-validate]: Compare decompressed output against the uncompressed input on the first run" << std::endl;
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

    DStorageTestContext ctx(factory, stagingSizeMiB, metadata.UncompressedSize);

    std::cout << "  " << stagingSizeMiB << " MiB staging buffer: ";
    if (metadata.LargestCompressedChunkSize > ctx.StagingBufferSizeBytes)
    {
        std::cout << " SKIPPED! " << std::endl;
        return {0, 0};
    }

    uint64_t fenceValue = 1;
    double meanBandwidth = 0;
    uint64_t meanCycleTime = 0;

    for (int i = 0; i < numRuns; ++i)
    {
        check_hresult(ctx.Fence->SetEventOnCompletion(fenceValue, ctx.FenceEvent.get()));

        if (metadata.Chunks.size() >= (DSTORAGE_MAX_QUEUE_CAPACITY / 2))
        {
            std::cout << "The number of requests exceeds half the queue capacity. This would result in auto-submit. ("
                      << DSTORAGE_MAX_QUEUE_CAPACITY / 2 << ")." << std::endl;
        }

        // Enqueue requests to load each compressed chunk.
        EnqueueChunks(ctx.Queue.get(), file.get(), compressionFormat, ctx.BufferResource.get(), metadata);

        // Signal the fence when done
        ctx.Queue->EnqueueSignal(ctx.Fence.get(), fenceValue);

        auto startTime = std::chrono::high_resolution_clock::now();
        auto startCycleTime = GetProcessCycleTime();

        // Tell DirectStorage to start executing all queued items.
        ctx.Queue->Submit();

        // Wait for the submitted work to complete
        WaitForSingleObject(ctx.FenceEvent.get(), INFINITE);

        auto endCycleTime = GetProcessCycleTime();
        auto endTime = std::chrono::high_resolution_clock::now();

        ctx.TerminateIfError();

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

TestResult RunSustainedThroughputTest(
    IDStorageFactory* factory,
    uint32_t stagingSizeMiB,
    wchar_t const* sourceFilename,
    DSTORAGE_COMPRESSION_FORMAT compressionFormat,
    Metadata const& metadata,
    uint32_t durationSeconds)
{
    com_ptr<IDStorageFile> file;

    HRESULT hr = factory->OpenFile(sourceFilename, IID_PPV_ARGS(file.put()));
    if (FAILED(hr))
    {
        std::wcout << L"The file '" << sourceFilename << L"' could not be opened. HRESULT=0x" << std::hex << hr
                   << std::endl;
        std::abort();
    }

    std::cout << "  " << stagingSizeMiB << " MiB staging buffer: ";
    DStorageTestContext ctx(factory, stagingSizeMiB, metadata.UncompressedSize);
    if (metadata.LargestCompressedChunkSize > ctx.StagingBufferSizeBytes)
    {
        std::cout << " SKIPPED! " << std::endl;
        return {0, 0};
    }

    // Running total of requests currently enqueued across all in-flight passes.
    // Incremented in enqueuePass, decremented after each fence signals to
    // reflect only the requests still live in the queue.
    uint32_t totalEnqueuedRequests = 0;

    // Helper: enqueue all chunks for one pass and signal the given fence value.
    // Does NOT call Submit().
    auto enqueuePass = [&](uint64_t signalValue)
    {
        totalEnqueuedRequests += static_cast<uint32_t>(metadata.Chunks.size());
        if (totalEnqueuedRequests >= (DSTORAGE_MAX_QUEUE_CAPACITY / 2))
        {
            std::cout << "The number of requests exceeds half the queue capacity. This would result in auto-submit. (" << DSTORAGE_MAX_QUEUE_CAPACITY / 2 << ")." << std::endl;
        }

        EnqueueChunks(ctx.Queue.get(), file.get(), compressionFormat, ctx.BufferResource.get(), metadata);
        ctx.Queue->EnqueueSignal(ctx.Fence.get(), signalValue);
    };

    using dseconds = std::chrono::duration<double>;
    const dseconds targetDuration(durationSeconds);

    int numBatches = 1;
    uint64_t fenceValue = 1;

    // Enqueue the first batch, start the clock, then submit. The loop below
    // will immediately pre-submit batch 2 before waiting on batch 1, keeping
    // the pipeline continuously fed from the very first submit onwards.
    enqueuePass(fenceValue);
    auto measureStart = std::chrono::high_resolution_clock::now();
    auto measureStartCycles = GetProcessCycleTime();
    ctx.Queue->Submit();

    while (true)
    {
        // Pre-submit the next batch before waiting on the current one,
        // keeping the decompressor continuously fed.
        uint64_t nextFenceValue = fenceValue + 1;
        enqueuePass(nextFenceValue);
        ctx.Queue->Submit();

        check_hresult(ctx.Fence->SetEventOnCompletion(fenceValue, ctx.FenceEvent.get()));
        WaitForSingleObject(ctx.FenceEvent.get(), INFINITE);

        ctx.TerminateIfError();

        // The completed pass's requests have been retired from the queue.
        totalEnqueuedRequests -= static_cast<uint32_t>(metadata.Chunks.size());

        ++numBatches;
        fenceValue = nextFenceValue;

        double elapsedSeconds = std::chrono::duration_cast<dseconds>(std::chrono::high_resolution_clock::now() - measureStart).count();

        if (elapsedSeconds >= static_cast<double>(durationSeconds))
            break;
    }

    // Wait for the final pre-submitted batch to complete
    check_hresult(ctx.Fence->SetEventOnCompletion(fenceValue, ctx.FenceEvent.get()));
    WaitForSingleObject(ctx.FenceEvent.get(), INFINITE);

    ctx.TerminateIfError();

    auto measureEnd = std::chrono::high_resolution_clock::now();
    uint64_t measureEndCycles = GetProcessCycleTime();

    double totalElapsedSeconds = std::chrono::duration_cast<dseconds>(measureEnd - measureStart).count();
    double totalUncompressedBytes = static_cast<double>(metadata.UncompressedSize) * numBatches;
    double bandwidth = (totalUncompressedBytes / totalElapsedSeconds) / 1e9;
    uint64_t meanCycles = (measureEndCycles - measureStartCycles) / numBatches;

    std::cout << "  " << bandwidth << " GB/s"
              << " mean cycle time: " << std::dec << meanCycles << " (" << numBatches << " batches)" << std::endl;

    return {bandwidth, meanCycles};
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
        GpuGDeflate,
        CpuZstd,
        GpuZstd
    };

    TestCase testCases[] = {
        TestCase::Uncompressed,
#if USE_ZLIB
        TestCase::CpuZLib,
#endif
        TestCase::CpuGDeflate,
        TestCase::GpuGDeflate,
        TestCase::CpuZstd,
        TestCase::GpuZstd
        };

    if (argc < 2)
    {
        ShowHelpText();
        return -1;
    }

    const wchar_t* originalFilename = argv[1];

    std::wstring uncompressedFilename = std::wstring(originalFilename) + L".uncompressed";
    std::wstring gdeflateFilename = std::wstring(originalFilename) + L".gdeflate";
    std::wstring zstdFilename = std::wstring(originalFilename) + L".zstd";
#if USE_ZLIB
    std::wstring zlibFilename = std::wstring(originalFilename) + L".zlib";
#endif

    std::filesystem::path inputPath = argv[1];
    std::vector<std::filesystem::path> inputFiles;
    if (std::filesystem::is_directory(inputPath))
    {
        std::wcout << "Directory of files has been specified, archives will be built using all the files found in this directory." << std::endl;
        std::error_code ec;
        std::filesystem::directory_iterator dirIter(inputPath, ec);
        if (ec)
        {
            std::cerr << "Error reading directory: " << ec.message() << std::endl;
            return -1;
        }

        for (const auto& entry : dirIter)
        {
            if (std::filesystem::is_regular_file(entry.status()))
            {
                inputFiles.push_back(entry.path());
            }
        }
    }
    else
    {
        std::wcout << "Single file has been specified, archive will be built using this file." << std::endl;
        inputFiles.push_back(inputPath);
    }

    uint32_t chunkSizeKiB = 256; // 256 KiB
    bool bValidate = false;
    uint32_t sustainedDurationSeconds = 0;

    for (int i = 2; i < argc; ++i)
    {
        constexpr wchar_t kChunkSizePrefix[] = L"-chunksize:";
        constexpr size_t kChunkSizePrefixLen = std::size(kChunkSizePrefix) - 1;

        constexpr wchar_t kSustainedPrefix[] = L"-sustained:";
        constexpr size_t kSustainedPrefixLen = std::size(kSustainedPrefix) - 1;

        if (_wcsnicmp(argv[i], kChunkSizePrefix, kChunkSizePrefixLen) == 0)
        {
            chunkSizeKiB = _wtoi(argv[i] + kChunkSizePrefixLen);
            if (chunkSizeKiB == 0)
            {
                ShowHelpText();
                std::wcout << L"Invalid chunk size: " << (argv[i] + kChunkSizePrefixLen) << std::endl;
                return -1;
            }
        }
        else if (_wcsicmp(argv[i], L"-validate") == 0)
        {
            bValidate = true;
        }
        else if (_wcsnicmp(argv[i], kSustainedPrefix, kSustainedPrefixLen) == 0)
        {
            sustainedDurationSeconds = _wtoi(argv[i] + kSustainedPrefixLen);
            if (sustainedDurationSeconds == 0)
            {
                ShowHelpText();
                std::wcout << L"Invalid sustained duration: " << (argv[i] + kSustainedPrefixLen) << std::endl;
                return -1;
            }
        }
        else
        {
            ShowHelpText();
            std::wcout << L"Unknown argument: " << argv[i] << std::endl;
            return -1;
        }
    }

    if (bValidate)
    {
        std::wcout << "Validation is enabled. The decompressed output will be compared against the original uncompressed file." << std::endl;
    }

    if (sustainedDurationSeconds > 0)
    {
        std::wcout << "Sustained mode enabled. Throughput will be measured for " << sustainedDurationSeconds << " second(s) with the queue kept continuously fed." << std::endl;
    }

    uint32_t chunkSizeBytes = chunkSizeKiB * 1024;

    Metadata uncompressedMetadata = CompressToArchive(DSTORAGE_COMPRESSION_FORMAT_NONE, inputFiles, uncompressedFilename.c_str(), chunkSizeBytes, bValidate);
    Metadata gdeflateMetadata = CompressToArchive(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, inputFiles, gdeflateFilename.c_str(), chunkSizeBytes, bValidate);
    Metadata zstdMetadata = CompressToArchive(DSTORAGE_COMPRESSION_FORMAT_ZSTD, inputFiles, zstdFilename.c_str(), chunkSizeBytes, bValidate);
#if USE_ZLIB
    Metadata zlibMetadata = CompressToArchive(DSTORAGE_CUSTOM_COMPRESSION_0, inputFiles, zlibFilename.c_str(), chunkSizeBytes, bValidate);
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
            filename = uncompressedFilename.c_str();
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

        case TestCase::CpuZstd:
            compressionFormat = DSTORAGE_COMPRESSION_FORMAT_ZSTD;
            numRuns = 5;

            config.DisableGpuDecompression = true;

            metadata = &zstdMetadata;
            filename = zstdFilename.c_str();
            std::cout << "CPU ZSTD:" << std::endl;
            break;

        case TestCase::GpuZstd:
            compressionFormat = DSTORAGE_COMPRESSION_FORMAT_ZSTD;
            numRuns = 10;
            metadata = &zstdMetadata;
            filename = zstdFilename.c_str();
            std::cout << "GPU ZSTD:" << std::endl;
            break;

        default:
            std::terminate();
        }

        check_hresult(DStorageSetConfiguration(&config));

        com_ptr<IDStorageFactory> factory;
        check_hresult(DStorageGetFactory(IID_PPV_ARGS(factory.put())));

        factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_BREAK_ON_ERROR);

        CustomDecompression customDecompression(factory.get(), std::thread::hardware_concurrency());

        // Validation pass - runs once before benchmarking
        if (bValidate)
        {
            uint32_t validationStagingMiB = 1;
            while ((validationStagingMiB * 1024) < chunkSizeKiB)
                validationStagingMiB *= 2;

            std::cout << "  Validating decompressed output ... ";
            bool passed = ValidateDecompression(
                factory.get(),
                validationStagingMiB,
                filename,
                uncompressedFilename.c_str(),
                compressionFormat,
                *metadata);

            if (passed)
            {
                std::cout << " PASSED." << std::endl;
            }
            else
            {
                std::cerr << " FAILED! Decompressed output does not match original." << std::endl;
                std::terminate();
            }
        }

        for (uint32_t stagingSizeMiB = 1; stagingSizeMiB <= MAX_STAGING_BUFFER_SIZE; stagingSizeMiB *= 2)
        {
            if ((stagingSizeMiB *1024) < chunkSizeKiB)
                continue;

            TestResult data = {};

            if (sustainedDurationSeconds > 0)
            {
                data = RunSustainedThroughputTest(factory.get(), stagingSizeMiB, filename, compressionFormat, *metadata, sustainedDurationSeconds);
            }
            else
            {
                data = RunTest(factory.get(), stagingSizeMiB, filename, compressionFormat, *metadata, numRuns);
            }

            results.push_back({testCase, stagingSizeMiB, data});
        }
    }

    std::cout << "\n\n";

    std::wstringstream bandwidth;
    std::wstringstream cycles;

    std::wstring header =
        L"\"Staging Buffer Size MiB\"\t\"Uncompressed\"\t\"ZLib\"\t\"CPU GDEFLATE\"\t\"GPU GDEFLATE\"\t\"CPU ZSTD\"\t\"GPU ZSTD\"";
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
    ratioLine("ZSTD", zstdMetadata);

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
