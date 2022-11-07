//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
#pragma once

#include "ZlibCodec.h"

#include <dstorage.h>
#include <winrt/base.h>

#include <deque>
#include <mutex>

using winrt::check_hresult;
using winrt::com_ptr;

class Codec
{
    com_ptr<IDStorageCompressionCodec> m_gdeflateCodec;

#if USE_ZLIB
    com_ptr<IDStorageCompressionCodec> m_zlibCodec;
#endif

    std::vector<uint8_t> m_stagingBuffer;

public:
    Codec()
    {
        // GDEFLATE's decompressor can go "wide" and use multiple threads to
        // decompress a single request. However, as we want to compare this to
        // ZLIB we only allow the codec to use a single thread.
        constexpr uint32_t CODEC_THREADS = 1; // 0 means "use default"
        check_hresult(DStorageCreateCompressionCodec(
            DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
            CODEC_THREADS,
            IID_PPV_ARGS(m_gdeflateCodec.put())));

#if USE_ZLIB
        m_zlibCodec = winrt::make<ZLibCodec>();
#endif
    }

    size_t Decompress(
        IDStorageCompressionCodec* codec,
        void const* src,
        uint64_t srcSize,
        void* dest,
        uint64_t dstSize)
    {
        size_t uncompressedDataSize;
        check_hresult(codec->DecompressBuffer(src, srcSize, dest, dstSize, &uncompressedDataSize));

        return uncompressedDataSize;
    }

    DSTORAGE_CUSTOM_DECOMPRESSION_RESULT Decompress(DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST const& request)
    {
        DSTORAGE_CUSTOM_DECOMPRESSION_RESULT result{};

        result.Id = request.Id;

        try
        {
            void* dest = GetDestination(request);

            IDStorageCompressionCodec* codec;

            switch (request.CompressionFormat)
            {
            case DSTORAGE_COMPRESSION_FORMAT_GDEFLATE:
                codec = m_gdeflateCodec.get();
                break;
#if USE_ZLIB
            case DSTORAGE_CUSTOM_COMPRESSION_0:
                codec = m_zlibCodec.get();
                break;
#endif
            default:
                std::terminate();
            }
            size_t actualDecompressedSize = Decompress(codec, request.SrcBuffer, request.SrcSize, dest, request.DstSize);

            if (dest != request.DstBuffer)
            {
                memcpy(request.DstBuffer, dest, actualDecompressedSize);
            }

            result.Result = S_OK;
        }
        catch (...)
        {
            result.Result = E_FAIL;
        }

        return result;
    }

    void* GetDestination(DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST const& request)
    {
        if (request.Flags & DSTORAGE_CUSTOM_DECOMPRESSION_FLAG_DEST_IN_UPLOAD_HEAP)
        {
            // CPU decompressors tend to read from the destination as they're
            // decompressing it.  This is a bad thing if the destination is in
            // write-combined memory, like an upload heap.  So instead the
            // decompression will target a staging buffer that we can then
            // memcpy into the upload heap.
            m_stagingBuffer.resize(request.DstSize);
            return m_stagingBuffer.data();
        }
        else
        {
            return request.DstBuffer;
        }
    }
};

class CustomDecompression
{
    com_ptr<IDStorageCustomDecompressionQueue1> m_queue;
    TP_WAIT* m_tpWait;

    std::vector<std::thread> m_threads;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_quit = false;
    std::deque<DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST> m_requests;

public:
    CustomDecompression(IDStorageFactory* factory, int numThreads)
    {
        check_hresult(factory->QueryInterface(IID_PPV_ARGS(m_queue.put())));

        m_tpWait = CreateThreadpoolWait(OnDecompressionRequestsReady, this, nullptr);
        SetWaitForDecompressionRequest();

        if (numThreads > 0)
        {
            for (int i = 0; i < numThreads; ++i)
            {
                m_threads.emplace_back([this] { Thread(); });
            }
        }
    }

    ~CustomDecompression()
    {
        WaitForThreadpoolWaitCallbacks(m_tpWait, TRUE);
        CloseThreadpoolWait(m_tpWait);

        std::unique_lock lock(m_mutex);
        m_quit = true;
        lock.unlock();

        m_cv.notify_all();

        for (auto& thread : m_threads)
        {
            thread.join();
        }
    }

private:
    void SetWaitForDecompressionRequest()
    {
        SetThreadpoolWait(m_tpWait, m_queue->GetEvent(), nullptr);
    }

    static void CALLBACK OnDecompressionRequestsReady(TP_CALLBACK_INSTANCE*, void* context, TP_WAIT*, TP_WAIT_RESULT)
    {
        CustomDecompression* self = reinterpret_cast<CustomDecompression*>(context);

        Codec codec;

        while (true)
        {
            DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST requests[10];
            uint32_t numRequests;
            check_hresult(
                self->m_queue
                    ->GetRequests1(DSTORAGE_GET_REQUEST_FLAG_SELECT_ALL, _countof(requests), requests, &numRequests));

            if (numRequests == 0)
                break;

            if (self->m_threads.empty())
            {

                for (uint32_t i = 0; i < numRequests; ++i)
                {
                    auto& request = requests[i];
                    auto result = codec.Decompress(request);
                    self->m_queue->SetRequestResults(1, &result);
                }
            }
            else
            {
                std::unique_lock lock(self->m_mutex);
                for (uint32_t i = 0; i < numRequests; ++i)
                {
                    self->m_requests.push_back(requests[i]);
                }
                lock.unlock();
                self->m_cv.notify_all();
            }
        }

        self->SetWaitForDecompressionRequest();
    }

    void Thread()
    {
        Codec codec;

        while (true)
        {
            DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST request;

            std::unique_lock lock(m_mutex);
            m_cv.wait(lock);

            if (m_quit)
                return;

            while (!m_requests.empty())
            {
                request = m_requests.front();
                m_requests.pop_front();
                lock.unlock();

                auto result = codec.Decompress(request);
                m_queue->SetRequestResults(1, &result);

                lock.lock();
            }
        }
    }
};