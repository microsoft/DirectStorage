//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#define NOMINMAX

#include "DStorageTestContext.h"
#include "Validation.h"

#include <iostream>

using winrt::check_hresult;
using winrt::com_ptr;

void SaveValidationFailureDump(
    const wchar_t* compressedFilename,
    const uint8_t* decompressedData,
    const uint8_t* expectedData,
    uint32_t dataSize)
{
    std::wstring decompressedPath = std::wstring(compressedFilename) + L".validation_decompressed";
    ScopedHandle decompFile(
        CreateFile(decompressedPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (decompFile.get())
    {
        DWORD written = 0;
        WriteFile(decompFile.get(), decompressedData, dataSize, &written, nullptr);
        std::wcout << L"  Decompressed output saved to: " << decompressedPath << std::endl;
    }

    std::wstring expectedPath = std::wstring(compressedFilename) + L".validation_expected";
    ScopedHandle expectedFile(
        CreateFile(expectedPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (expectedFile.get())
    {
        DWORD written = 0;
        WriteFile(expectedFile.get(), expectedData, dataSize, &written, nullptr);
        std::wcout << L"  Expected output saved to: " << expectedPath << std::endl;
    }
}

bool ValidateDecompression(
    IDStorageFactory* factory,
    uint32_t stagingSizeMiB,
    const wchar_t* compressedFilename,
    const wchar_t* uncompressedFilename,
    DSTORAGE_COMPRESSION_FORMAT compressionFormat,
    Metadata const& metadata)
{
    com_ptr<IDStorageFile> file;
    HRESULT hr = factory->OpenFile(compressedFilename, IID_PPV_ARGS(file.put()));
    if (FAILED(hr))
    {
        std::wcout << L"  Validation: could not open '" << compressedFilename << L"'. HRESULT=0x" << std::hex << hr
                   << std::dec << std::endl;
        return false;
    }

    DStorageTestContext ctx(factory, stagingSizeMiB, metadata.UncompressedSize);
    if (metadata.LargestCompressedChunkSize > ctx.StagingBufferSizeBytes)
    {
        std::cout << "  Validation: staging buffer too small, skipping." << std::endl;
        return false;
    }

    // Decompress via DirectStorage
    EnqueueChunks(ctx.Queue.get(), file.get(), compressionFormat, ctx.BufferResource.get(), metadata);
    ctx.Queue->EnqueueSignal(ctx.Fence.get(), 1);
    ctx.Queue->Submit();

    check_hresult(ctx.Fence->SetEventOnCompletion(1, ctx.FenceEvent.get()));
    WaitForSingleObject(ctx.FenceEvent.get(), INFINITE);

    DSTORAGE_ERROR_RECORD errorRecord{};
    ctx.Queue->RetrieveErrorRecord(&errorRecord);
    if (FAILED(errorRecord.FirstFailure.HResult))
    {
        std::cout << "  Validation: DirectStorage request failed! HRESULT=0x" << std::hex
                  << errorRecord.FirstFailure.HResult << std::dec << std::endl;
        return false;
    }

    // Create readback buffer and copy GPU result into it
    D3D12_HEAP_PROPERTIES readbackHeapProps = {};
    readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc = ctx.BufferResource->GetDesc();
    com_ptr<ID3D12Resource> readbackResource;
    check_hresult(ctx.Device->CreateCommittedResource(
        &readbackHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(readbackResource.put())));

    com_ptr<ID3D12CommandQueue> copyQueue;
    D3D12_COMMAND_QUEUE_DESC copyQueueDesc = {};
    copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check_hresult(ctx.Device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(copyQueue.put())));

    com_ptr<ID3D12CommandAllocator> cmdAllocator;
    check_hresult(ctx.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmdAllocator.put())));

    com_ptr<ID3D12GraphicsCommandList> cmdList;
    check_hresult(ctx.Device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAllocator.get(),
        nullptr,
        IID_PPV_ARGS(cmdList.put())));

    cmdList->CopyBufferRegion(readbackResource.get(), 0, ctx.BufferResource.get(), 0, metadata.UncompressedSize);
    check_hresult(cmdList->Close());

    ID3D12CommandList* ppCmdLists[] = {cmdList.get()};
    copyQueue->ExecuteCommandLists(1, ppCmdLists);

    com_ptr<ID3D12Fence> copyFence;
    check_hresult(ctx.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(copyFence.put())));
    ScopedHandle copyFenceEvent(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    check_hresult(copyQueue->Signal(copyFence.get(), 1));
    check_hresult(copyFence->SetEventOnCompletion(1, copyFenceEvent.get()));
    WaitForSingleObject(copyFenceEvent.get(), INFINITE);

    // Map the readback buffer
    void* readbackData = nullptr;
    D3D12_RANGE readRange = {0, metadata.UncompressedSize};
    check_hresult(readbackResource->Map(0, &readRange, &readbackData));

    // Memory-map the uncompressed archive as reference
    ScopedHandle refHandle(CreateFile(
        uncompressedFilename,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!refHandle.get())
    {
        std::wcout << L"  Validation: could not open reference file '" << uncompressedFilename << L"'." << std::endl;
        readbackResource->Unmap(0, nullptr);
        return false;
    }

    ScopedHandle refMapping(CreateFileMapping(refHandle.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!refMapping.get())
    {
        std::wcout << L"  Validation: could not map reference file." << std::endl;
        readbackResource->Unmap(0, nullptr);
        return false;
    }

    uint8_t* refData =
        reinterpret_cast<uint8_t*>(MapViewOfFile(refMapping.get(), FILE_MAP_READ, 0, 0, metadata.UncompressedSize));
    if (!refData)
    {
        std::wcout << L"  Validation: MapViewOfFile failed." << std::endl;
        readbackResource->Unmap(0, nullptr);
        return false;
    }

    // Compare
    const uint8_t* actual = reinterpret_cast<const uint8_t*>(readbackData);
    bool valid = true;
    uint32_t mismatchCount = 0;
    constexpr uint32_t maxMismatchesToReport = 10;

    for (uint32_t i = 0; i < metadata.UncompressedSize; ++i)
    {
        if (actual[i] != refData[i])
        {
            if (mismatchCount < maxMismatchesToReport)
            {
                std::cout << "  Mismatch at byte " << i << ": expected 0x" << std::hex << static_cast<int>(refData[i])
                          << " got 0x" << static_cast<int>(actual[i]) << std::dec << std::endl;
            }
            ++mismatchCount;
            valid = false;
        }
    }

    if (!valid)
    {
        std::cout << "  Total mismatches: " << mismatchCount << " out of " << metadata.UncompressedSize << " bytes."
                  << std::endl;
        SaveValidationFailureDump(compressedFilename, actual, refData, metadata.UncompressedSize);
    }

    UnmapViewOfFile(refData);
    D3D12_RANGE emptyRange = {0, 0};
    readbackResource->Unmap(0, &emptyRange);

    return valid;
}