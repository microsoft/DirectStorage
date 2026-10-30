//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
#pragma once

#include "Metadata.h"

#include <d3d12.h>
#include <dstorage.h>
#include <dxgi1_4.h>
#include <winrt/base.h>

#include <iostream>

using winrt::check_hresult;
using winrt::com_ptr;

struct DStorageTestContext
{
    com_ptr<ID3D12Device> Device;
    com_ptr<IDStorageQueue> Queue;
    uint32_t StagingBufferSizeBytes;
    com_ptr<ID3D12Resource> BufferResource;
    com_ptr<ID3D12Fence> Fence;
    ScopedHandle FenceEvent;

    DStorageTestContext(IDStorageFactory* factory, uint32_t stagingSizeMiB, uint32_t bufferSizeBytes)
        : FenceEvent(CreateEvent(nullptr, FALSE, FALSE, nullptr))
    {
        // The staging buffer size must be set before any queues are created.
        StagingBufferSizeBytes = stagingSizeMiB * 1024 * 1024;
        check_hresult(factory->SetStagingBufferSize(StagingBufferSizeBytes));

        check_hresult(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&Device)));

        DSTORAGE_QUEUE_DESC queueDesc{};
        queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
        queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        queueDesc.Device = Device.get();

        check_hresult(factory->CreateQueue(&queueDesc, IID_PPV_ARGS(Queue.put())));

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = bufferSizeBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.SampleDesc.Count = 1;

        check_hresult(Device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(BufferResource.put())));

        check_hresult(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(Fence.put())));
    }

    // Checks for device removal and DirectStorage errors after a fence wait
    // completes. Terminates the process if any error is detected.
    void TerminateIfError() const
    {
        if (Fence->GetCompletedValue() == static_cast<uint64_t>(-1))
        {
            // Device removed!  Give DirectStorage a chance to detect the error.
            Sleep(5);
            std::cout << "Device removed!" << std::endl;
        }

        // If an error was detected the first failure record
        // can be retrieved to get more details.
        DSTORAGE_ERROR_RECORD errorRecord{};
        Queue->RetrieveErrorRecord(&errorRecord);
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
    }
};

inline uint32_t EnqueueChunks(
    IDStorageQueue* queue,
    IDStorageFile* file,
    DSTORAGE_COMPRESSION_FORMAT compressionFormat,
    ID3D12Resource* destResource,
    Metadata const& metadata)
{
    uint32_t destOffset = 0;
    for (auto const& chunk : metadata.Chunks)
    {
        DSTORAGE_REQUEST request = {};
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
        request.Options.CompressionFormat = compressionFormat;
        request.Options.GaclTransformType = static_cast<DSTORAGE_GACL_SHUFFLE_TRANSFORM_TYPE>(chunk.TransformType);
        request.Source.File.Source = file;
        request.Source.File.Offset = chunk.Offset;
        request.Source.File.Size = chunk.CompressedSize;
        request.UncompressedSize = chunk.UncompressedSize;
        request.Destination.Buffer.Resource = destResource;
        request.Destination.Buffer.Offset = destOffset;
        request.Destination.Buffer.Size = chunk.UncompressedSize;
        queue->EnqueueRequest(&request);
        destOffset += request.UncompressedSize;
    }
    return destOffset;
}