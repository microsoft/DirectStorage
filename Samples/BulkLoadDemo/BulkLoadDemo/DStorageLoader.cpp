//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"

#define USE_PIX

#include "DStorageLoader.h"

#include <GraphicsCore.h>
#include <pix3.h>
#include <zlib.h>

#include <algorithm>
#include <deque>

using Microsoft::WRL::ComPtr;

ComPtr<IDStorageFactory> g_dsFactory;
ComPtr<IDStorageQueue1> g_dsSystemMemoryQueue;
ComPtr<IDStorageQueue1> g_dsGpuQueue;

//
// Custom decompression implementation.
//
// ZLib is integrated with DirectStorage using the
// IDStorageCustomDecompressionQueue1 interface.  This means that no
// compression, or Zlib or GDeflate decompression can be selected just by
// changing the value of the CompressionForm field in DSTORAGE_REQUEST_OPTIONS,
// and the runtime takes care of figuring out the right pipeline for completing
// the request.
//
// The DirectStorage's custom decompression has been designed to be easy to
// integrate with existing job systems.  In this example, the Windows Threadpool
// APIs are used.  The basic approach is:
//
// * g_customDecompressionRequestsAvailableWait is configured to run a callback,
//   OnCustomDecompressionRequestsAvailable, when the event returned by
//   IDStorageCustomDecompressionQueue1::GetEvent() is set.
//
// * g_decompressionWork is configured to be able to execute units of work on
//   the threadpool by calling DecompressionWork.
//
// * OnCustomDecompressionRequestsAvailable pulls batches of custom
//   decompression requests from the custom decompression queue.  These are then
//   added to g_decompressionRequests, and one g_decompressionWork is submitted
//   for each request.
//
// * DecompressionWork pulls one request from g_decompressionRequests and
//   decompresses it, calling IDStorageCustomDecompressionQueue1::SetResult when
//   complete.
//

static ComPtr<IDStorageCustomDecompressionQueue1> g_customDecompressionQueue;
static HANDLE g_customDecompressionQueueEvent;
static TP_WAIT* g_customDecompressionRequestsAvailableWait;
static TP_WORK* g_decompressionWork;

static std::mutex g_decompressionRequestsMutex;
static std::deque<DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST> g_decompressionRequests;

//
// This is called on the threadpool, once per custom decompression request.
//
static void CALLBACK DecompressionWork(TP_CALLBACK_INSTANCE*, void*, TP_WORK*)
{
    PIXScopedEvent(0, "OnDecompress");

    // Give this thread a high priority to ensure we perform decompression without
    // excessive context switching between available cores.
    auto oldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Get the next request.
    DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST request;
    {
        std::unique_lock lock(g_decompressionRequestsMutex);

        // DecompressionWork is submitted once per request added to the deque
        ASSERT(!g_decompressionRequests.empty());

        request = g_decompressionRequests.front();
        g_decompressionRequests.pop_front();
    }
    
    // We only expect ZLib requests
    ASSERT(request.CompressionFormat == CUSTOM_COMPRESSION_FORMAT_ZLIB);

    // If the destination is in an upload heap (write-combined memory) then it's
    // better for us to first decompress to a scratch buffer and then copy that
    // to the final destination.  This is because ZLib decompression tends to
    // read from the destination buffer, which is extremely slow from
    // write-combined memory.
    std::unique_ptr<uint8_t[]> scratchBuffer;
    void* decompressionDestination;

    if (request.Flags & DSTORAGE_CUSTOM_DECOMPRESSION_FLAG_DEST_IN_UPLOAD_HEAP)
    {
        scratchBuffer.reset(new uint8_t[request.DstSize]);
        decompressionDestination = scratchBuffer.get();
    }
    else
    {
        decompressionDestination = request.DstBuffer;
    }

    // Perform the actual decompression
    uLong decompressedSize = static_cast<uLong>(request.DstSize);
    int uncompressResult = uncompress(
        reinterpret_cast<Bytef*>(decompressionDestination),
        &decompressedSize,
        static_cast<Bytef const*>(request.SrcBuffer),
        static_cast<uLong>(request.SrcSize));

    if (uncompressResult == Z_OK && decompressionDestination != request.DstBuffer)
    {
        memcpy(request.DstBuffer, decompressionDestination, request.DstSize);
    }

    // Tell DirectStorage that this request has been completed.
    DSTORAGE_CUSTOM_DECOMPRESSION_RESULT result{};
    result.Id = request.Id;
    result.Result = uncompressResult == Z_OK ? S_OK : E_FAIL;

    g_customDecompressionQueue->SetRequestResults(1, &result);

    // Restore the original thread's priority back to its original setting.
    SetThreadPriority(GetCurrentThread(), oldPriority);
}

//
// The is called on the threadpool when the custom decompression queue's event
// is set.
//
static void CALLBACK OnCustomDecompressionRequestsAvailable(TP_CALLBACK_INSTANCE*, void*, TP_WAIT* wait, TP_WAIT_RESULT)
{
    PIXScopedEvent(0, "OnCustomDecompressionRequestsReady");

    // Loop through all requests pending requests until no more remain.
    while (true)
    {
        DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST requests[64];
        uint32_t numRequests = 0;

        DSTORAGE_GET_REQUEST_FLAGS flags = DSTORAGE_GET_REQUEST_FLAG_SELECT_CUSTOM;

        // Pull off a batch of requests to process in this loop.
        ASSERT_SUCCEEDED(g_customDecompressionQueue->GetRequests1(flags, _countof(requests), requests, &numRequests));

        if (numRequests == 0)
            break;

        std::unique_lock lock(g_decompressionRequestsMutex);

        for (uint32_t i = 0; i < numRequests; ++i)
        {
            g_decompressionRequests.push_back(requests[i]);
        }

        lock.unlock();

        // Submit one piece of work for each request.
        for (uint32_t i = 0; i < numRequests; ++i)
        {
            SubmitThreadpoolWork(g_decompressionWork);
        }
    }

    // Re-register the custom decompression queue event with this callback to be
    // called when the next decompression requests become available for
    // processing.
    SetThreadpoolWait(wait, g_customDecompressionQueueEvent, nullptr);
}

//
// Public entry points
//

void InitializeDStorage(bool disableGpuDecompression)
{
    DSTORAGE_CONFIGURATION config{};
    config.DisableGpuDecompression = disableGpuDecompression;
    ASSERT_SUCCEEDED(DStorageSetConfiguration(&config));

    ASSERT_SUCCEEDED(DStorageGetFactory(IID_PPV_ARGS(&g_dsFactory)));
    g_dsFactory->SetDebugFlags(DSTORAGE_DEBUG_BREAK_ON_ERROR | DSTORAGE_DEBUG_SHOW_ERRORS);
    g_dsFactory->SetStagingBufferSize(256 * 1024 * 1024);

    // Create the system memory queue, used for reading data into system memory.
    {
        DSTORAGE_QUEUE_DESC queueDesc{};
        queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
        queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        queueDesc.Name = "g_dsSystemMemoryQueue";

        ASSERT_SUCCEEDED(g_dsFactory->CreateQueue(&queueDesc, IID_PPV_ARGS(&g_dsSystemMemoryQueue)));
    }

    // Create the GPU queue, used for reading GPU resources.
    {
        DSTORAGE_QUEUE_DESC queueDesc{};
        queueDesc.Device = Graphics::g_Device;
        queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
        queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        queueDesc.Name = "g_dsGpuQueue";

        ASSERT_SUCCEEDED(g_dsFactory->CreateQueue(&queueDesc, IID_PPV_ARGS(&g_dsGpuQueue)));
    }

    // Configure custom decompression queue
    ASSERT_SUCCEEDED(g_dsFactory.As(&g_customDecompressionQueue));
    g_customDecompressionQueueEvent = g_customDecompressionQueue->GetEvent();
    g_customDecompressionRequestsAvailableWait =
        CreateThreadpoolWait(OnCustomDecompressionRequestsAvailable, nullptr, nullptr);
    SetThreadpoolWait(g_customDecompressionRequestsAvailableWait, g_customDecompressionQueueEvent, nullptr);

    g_decompressionWork = CreateThreadpoolWork(DecompressionWork, nullptr, nullptr);
}

void ShutdownDStorage()
{
    if (!g_dsFactory)
        return;

    CloseThreadpoolWait(g_customDecompressionRequestsAvailableWait);
    CloseThreadpoolWork(g_decompressionWork);
    g_customDecompressionQueue.Reset();
    CloseHandle(g_customDecompressionQueueEvent);

    g_dsGpuQueue.Reset();
    g_dsSystemMemoryQueue.Reset();
    g_dsFactory.Reset();
}
