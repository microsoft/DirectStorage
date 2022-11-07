//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
#pragma once

#ifdef _M_ARM64
# define USE_ZLIB 0
#else
# define USE_ZLIB 1

#include <dstorage.h>
#include <winrt/base.h>
#include <zlib.h>

class ZLibCodec : public winrt::implements<ZLibCodec, IDStorageCompressionCodec>
{
public:
    HRESULT STDMETHODCALLTYPE CompressBuffer(
        const void* uncompressedData,
        size_t uncompressedDataSize,
        DSTORAGE_COMPRESSION compressionSetting,
        void* compressedBuffer,
        size_t compressedBufferSize,
        size_t* compressedDataSize) override
    {
        uLong destLen = static_cast<uLong>(compressedBufferSize);

        int level;
        switch (compressionSetting)
        {
        case DSTORAGE_COMPRESSION_DEFAULT:
            level = Z_DEFAULT_COMPRESSION;
            break;
        case DSTORAGE_COMPRESSION_BEST_RATIO:
            level = Z_BEST_COMPRESSION;
            break;
        case DSTORAGE_COMPRESSION_FASTEST:
            level = Z_BEST_SPEED;
            break;
        default:
            std::terminate();
        }

        if (compress2(
                static_cast<Bytef*>(compressedBuffer),
                &destLen,
                static_cast<Bytef const*>(uncompressedData),
                static_cast<uLong>(uncompressedDataSize),
                level) == Z_OK)
        {
            *compressedDataSize = destLen;
            return S_OK;
        }
        else
        {
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE DecompressBuffer(
        const void* compressedData,
        size_t compressedDataSize,
        void* uncompressedBuffer,
        size_t uncompressedBufferSize,
        size_t* uncompressedDataSize) override
    {
        uLong destLen = static_cast<uLong>(uncompressedBufferSize);

        if (uncompress(
                reinterpret_cast<Bytef*>(uncompressedBuffer),
                &destLen,
                static_cast<Bytef const*>(compressedData),
                static_cast<uLong>(compressedDataSize)) == Z_OK)
        {
            *uncompressedDataSize = destLen;
            return S_OK;
        }
        else
        {
            return E_FAIL;
        }
    }

    size_t STDMETHODCALLTYPE CompressBufferBound(size_t uncompressedDataSize) override
    {
        return compressBound(static_cast<uLong>(uncompressedDataSize));
    }
};

#endif
