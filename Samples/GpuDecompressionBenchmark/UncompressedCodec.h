//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
#pragma once

#include <dstorage.h>
#include <winrt/base.h>

class UncompressedCodec : public winrt::implements<UncompressedCodec, IDStorageCompressionCodec>
{
public:
    HRESULT STDMETHODCALLTYPE CompressBuffer(
        const void* uncompressedData,
        size_t uncompressedDataSize,
        DSTORAGE_COMPRESSION /* compressionSetting */,
        void* compressedBuffer,
        size_t compressedBufferSize,
        size_t* compressedDataSize) override
    {
        if (uncompressedDataSize != compressedBufferSize)
        {
            return E_INVALIDARG; // sizes must match for uncompressed codec
        }

        memcpy(compressedBuffer, uncompressedData, uncompressedDataSize);
        *compressedDataSize = uncompressedDataSize;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DecompressBuffer(
        const void* compressedData,
        size_t compressedDataSize,
        void* uncompressedBuffer,
        size_t uncompressedBufferSize,
        size_t* uncompressedDataSize) override
    {
        if (compressedDataSize != uncompressedBufferSize)
        {
            return E_INVALIDARG; // sizes must match for uncompressed codec
        }

        memcpy(uncompressedBuffer, compressedData, compressedDataSize);
        *uncompressedDataSize = compressedDataSize;
        return S_OK;
    }

    size_t STDMETHODCALLTYPE CompressBufferBound(size_t uncompressedDataSize) override
    {
        return uncompressedDataSize;
    }
};
