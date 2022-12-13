/*
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: MIT
 */
#define NOMINMAX

#include <dstorage.h>
#include <GDeflate.h>
#include <winrt/base.h>

#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

using Buffer = std::vector<uint8_t>;

static Buffer GenerateBuffer(std::default_random_engine& r, size_t size)
{
    std::cout << "Generating data (" << size << " bytes)" << std::flush;

    // Use random doubles because these will compress better than random bytes
    std::uniform_real_distribution<double> randomDouble(0, 100.0);

    Buffer b;
    b.reserve(size);

    while (b.size() < size)
    {
        double value = randomDouble(r);

        size_t toAdd = std::min(size - b.size(), sizeof(value));
        for (size_t i = 0; i < toAdd; ++i)
        {
            uint8_t* v = reinterpret_cast<uint8_t*>(&value);
            b.push_back(v[i]);
        }
    }

    std::cout << std::endl;

    return b;
}

class GDeflateCodec : public winrt::implements<GDeflateCodec, IDStorageCompressionCodec>
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
        int compressionLevel;
        switch (compressionSetting)
        {
        case DSTORAGE_COMPRESSION_FASTEST:
            compressionLevel = 1;
            break;
        case DSTORAGE_COMPRESSION_BEST_RATIO:
            compressionLevel = 12;
            break;

        case DSTORAGE_COMPRESSION_DEFAULT:
        default:
            compressionLevel = 9;
            break;
        }

        *compressedDataSize = compressedBufferSize;

        if (!GDeflate::Compress(
                reinterpret_cast<uint8_t*>(compressedBuffer),
                compressedDataSize,
                reinterpret_cast<uint8_t const*>(uncompressedData),
                uncompressedDataSize,
                compressionLevel,
                0))
        {
            return E_FAIL;
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DecompressBuffer(
        const void* compressedData,
        size_t compressedDataSize,
        void* uncompressedBuffer,
        size_t uncompressedBufferSize,
        size_t* uncompressedDataSize) override
    {
        *uncompressedDataSize = 0;

        if (!GDeflate::Decompress(
                reinterpret_cast<uint8_t*>(uncompressedBuffer),
                uncompressedBufferSize,
                reinterpret_cast<uint8_t const*>(compressedData),
                compressedDataSize,
                std::thread::hardware_concurrency()))
        {
            return E_FAIL;
        }

        *uncompressedDataSize = uncompressedBufferSize;

        return S_OK;
    }

    size_t STDMETHODCALLTYPE CompressBufferBound(size_t uncompressedDataSize) override
    {
        return GDeflate::CompressBound(uncompressedDataSize);
    }
};

static Buffer Compress(IDStorageCompressionCodec* codec, Buffer const& buffer)
{
    Buffer compressedBuffer(codec->CompressBufferBound(buffer.size()));

    size_t bufferSize;
    winrt::check_hresult(codec->CompressBuffer(
        buffer.data(),
        buffer.size(),
        DSTORAGE_COMPRESSION_DEFAULT,
        compressedBuffer.data(),
        compressedBuffer.size(),
        &bufferSize));

    compressedBuffer.resize(bufferSize);

    return compressedBuffer;
}

static Buffer Decompress(IDStorageCompressionCodec* codec, Buffer const& compressedBuffer, size_t expectedSize)
{
    Buffer uncompressedBuffer(expectedSize);

    size_t uncompressedSize;
    winrt::check_hresult(codec->DecompressBuffer(
        compressedBuffer.data(),
        compressedBuffer.size(),
        uncompressedBuffer.data(),
        uncompressedBuffer.size(),
        &uncompressedSize));

    uncompressedBuffer.resize(uncompressedSize);

    return uncompressedBuffer;
}

static bool Validate(IDStorageCompressionCodec* codec, Buffer const& compressedBuffer, Buffer const& expectedBuffer)
{
    try
    {
        Buffer uncompressedBuffer = Decompress(codec, compressedBuffer, expectedBuffer.size());

        if (uncompressedBuffer.size() != expectedBuffer.size())
            return false;

        if (memcmp(expectedBuffer.data(), uncompressedBuffer.data(), expectedBuffer.size()) != 0)
            return false;

        return true;
    }
    catch (...)
    {
        std::cout << "Expection thrown during validation" << std::endl;
        return false;
    }
}

int main()
{
    std::default_random_engine r;

    std::vector<Buffer> sourceBuffers;

    size_t s64k = 64 * 1024;

    // Some fixed test case sizes
    for (size_t size : {1, 2, 123, 64 * 1024, 64 * 1024 - 1, 64 * 1024 + 1, 64 * 1024 * 64})
    {
        sourceBuffers.push_back(GenerateBuffer(r, size));
    }

    // Some random test case sizes
    for (int i = 0; i < 5; ++i)
    {
        std::uniform_int_distribution<size_t> randomSize(1, 32 * 1024 * 1024);
        sourceBuffers.push_back(GenerateBuffer(r, randomSize(r)));
    }

    winrt::com_ptr<IDStorageCompressionCodec> dstorageCodec;
    winrt::check_hresult(
        DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, 0, IID_PPV_ARGS(dstorageCodec.put())));

    winrt::com_ptr<IDStorageCompressionCodec> referenceCodec = winrt::make<GDeflateCodec>();

    auto row = [](auto&& source, auto&& dstorage, auto&& reference, auto&& result)
    {
        std::cout << std::setw(15) << source            //
                  << " |" << std::setw(26) << dstorage  //
                  << " |" << std::setw(28) << reference //
                  << " | " << result << std::endl;
    };

    row("Source bytes", "DStorage compressed bytes", "Reference compressed bytes", "Result");

    for (auto& sourceBuffer : sourceBuffers)
    {
        std::cout << "." << std::flush;

        Buffer dstorageBuffer = Compress(dstorageCodec.get(), sourceBuffer);

        std::cout << "." << std::flush;

        Buffer referenceBuffer = Compress(referenceCodec.get(), sourceBuffer);

        std::string result;

        if (dstorageBuffer.size() != referenceBuffer.size())
            result += "Compressed buffer size mismatch ";
        else if (memcmp(dstorageBuffer.data(), referenceBuffer.data(), dstorageBuffer.size()) != 0)
            result += "Compressed buffer contents mismatch ";

        if (!Validate(dstorageCodec.get(), dstorageBuffer, sourceBuffer))
            result += "DS->DS failed ";

        std::cout << "." << std::flush;

        if (!Validate(dstorageCodec.get(), referenceBuffer, sourceBuffer))
            result += "Ref->DS failed ";

        std::cout << "." << std::flush;

        if (!Validate(referenceCodec.get(), dstorageBuffer, sourceBuffer))
            result += "DS->Ref failed ";

        std::cout << "." << std::flush;

        if (!Validate(referenceCodec.get(), referenceBuffer, sourceBuffer))
            result += "Ref->Ref failed ";

        if (result.empty())
            result = "Ok";

        std::cout << "\r";
        row(sourceBuffer.size(), dstorageBuffer.size(), referenceBuffer.size(), result);
    }

    return 0;
}