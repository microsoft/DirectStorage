//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include <dstorage.h>
#include <dxgi1_4.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <winrt/base.h>

using winrt::com_ptr;
using winrt::check_hresult;

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
    std::cout << "Reads the contents of a file and writes them out to a buffer on the GPU using DirectStorage." << std::endl << std::endl;
    std::cout << "USAGE: HelloDirectStorage [path]" << std::endl << std::endl;
}

// The following example reads from a specified data file and writes the contents
// to a D3D12 buffer resource.
int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        ShowHelpText();
        return -1;
    }

    com_ptr<ID3D12Device> device;
    check_hresult(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)));

    com_ptr<IDStorageFactory> factory;
    check_hresult(DStorageGetFactory(IID_PPV_ARGS(factory.put())));

    com_ptr<IDStorageFile> file;
    const wchar_t* fileToLoad = argv[1];
    HRESULT hr = factory->OpenFile(fileToLoad, IID_PPV_ARGS(file.put()));
    if (FAILED(hr))
    {
        std::wcout << L"The file '" << fileToLoad << L"' could not be opened. HRESULT=0x" << std::hex << hr << std::endl;
        ShowHelpText();
        return -1;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    check_hresult(file->GetFileInformation(&info));
    uint32_t fileSize = info.nFileSizeLow;

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
    bufferDesc.Width = fileSize;
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

    // Enqueue a request to read the file contents into a destination D3D12 buffer resource.
    // Note: The example request below is performing a single read of the entire file contents.
    DSTORAGE_REQUEST request = {};
    request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
    request.Source.File.Source = file.get();
    request.Source.File.Offset = 0;
    request.Source.File.Size = fileSize;
    request.UncompressedSize = fileSize;
    request.Destination.Buffer.Resource = bufferResource.get();
    request.Destination.Buffer.Offset = 0;
    request.Destination.Buffer.Size = request.Source.File.Size;

    queue->EnqueueRequest(&request);

    // Configure a fence to be signaled when the request is completed
    com_ptr<ID3D12Fence> fence;
    check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())));

    ScopedHandle fenceEvent(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    constexpr uint64_t fenceValue = 1;
    check_hresult(fence->SetEventOnCompletion(fenceValue, fenceEvent.get()));
    queue->EnqueueSignal(fence.get(), fenceValue);

    // Tell DirectStorage to start executing all queued items.
    queue->Submit();

    // Wait for the submitted work to complete
    std::cout << "Waiting for the DirectStorage request to complete..." << std::endl;
    WaitForSingleObject(fenceEvent.get(), INFINITE);

    // Check the status array for errors.
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
        std::cout << "The DirectStorage request failed! HRESULT=0x" << std::hex << errorRecord.FirstFailure.HResult << std::endl;
    }
    else
    {
        std::cout << "The DirectStorage request completed successfully!" << std::endl;
    }

    return 0;
}
