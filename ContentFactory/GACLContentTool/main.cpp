// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

// Condition texture data, compressing it with Zstd, and verifying that the original bytes can be reconstructed. 

/*
Notes:
- GACL supplies the forward shuffle; this file implements the reverse reconstruction used for checking.
- BC7 currently bypasses conditioning.
- Output contains compressed payload bytes only; interpreting them requires the accompanying format/transform information.

Steps:

Original texture-block bytes
    ↓
Shuffle related fields together
    ↓
Compress with Zstd
    ↓
Decompress with Zstd
    ↓
Reverse the shuffle
    ↓
Compare with the original bytes
*/

#include "gacl.h"
#include "shuffle.h"
#include "zstd.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Forward shuffle functions provided by the linked GACL shuffle implementation.
// Implemented in GACL's bc1.cpp, bc3,cpp, bc4,cpp, bc5.cpp
HRESULT Shuffle_BC1(uint8_t*, const uint8_t*, size_t, size_t);
HRESULT Shuffle_BC3(uint8_t*, const uint8_t*, size_t, size_t);
HRESULT Shuffle_BC4(uint8_t*, const uint8_t*, size_t, size_t);
HRESULT Shuffle_BC5(uint8_t*, const uint8_t*, size_t, size_t);

namespace
{
    constexpr std::string_view kVersion = "1.1.0";
    constexpr size_t kShuffleVersion = 1;

    // Each supported format selects its block size, shuffle grouping, and transform ID.
    struct TextureFormatInfo
    {
        const wchar_t* name;
        size_t bytesPerBlock;
        size_t groupSize;
        HRESULT (*shuffle)(uint8_t*, const uint8_t*, size_t, size_t);
        uint32_t transformId;
    };

    // BC1/3/4/5 use their stable shuffle; BC7 currently uses Zstd without a shuffle.
    const std::array<TextureFormatInfo, 5> kCases{{
        {L"BC1", 8, 2, Shuffle_BC1, GACL_SHUFFLE_TRANSFORM_ZSTD_BC1_224},
        {L"BC3", 16, 4, Shuffle_BC3, GACL_SHUFFLE_TRANSFORM_ZSTD_BC3_116224},
        {L"BC4", 8, 4, Shuffle_BC4, GACL_SHUFFLE_TRANSFORM_ZSTD_BC4_116},
        {L"BC5", 16, 4, Shuffle_BC5, GACL_SHUFFLE_TRANSFORM_ZSTD_BC5_116116},
        {L"BC7", 16, 1, nullptr, GACL_SHUFFLE_TRANSFORM_ZSTD_ONLY},
    }};

    // Original block layouts used to rebuild the payload during CPU verification.
    // Size checks guard against padding changing these expected byte layouts.
    struct LocalBC1Block            // 8 bytes
    {
        uint16_t color0;
        uint16_t color1;
        uint32_t indices;
    };

    struct LocalBC3Block            // 16 bytes
    {
        uint8_t alpha0;
        uint8_t alpha1;
        uint8_t alphaIndices[6];
        LocalBC1Block color;
    };

    struct LocalBC4Block            // 8 bytes
    {
        uint8_t endpoint0;
        uint8_t endpoint1;
        uint8_t indices[6];
    };

    struct LocalBC5Block            // 16 bytes
    {
        LocalBC4Block red;
        LocalBC4Block green;
    };
    static_assert(sizeof(LocalBC1Block) == 8 && sizeof(LocalBC3Block) == 16);
    static_assert(sizeof(LocalBC4Block) == 8 && sizeof(LocalBC5Block) == 16);

    // A small helper for reading a field from a shuffled byte stream.
    // Example: Read<uint16_t>(color0)
    //    Reads 2 bytes and advances color0 by 2
    template <typename T>
    T Read(const uint8_t*& source)
    {
        T value{};
        std::memcpy(&value, source, sizeof(value));
        source += sizeof(value);
        return value;
    }

    // Reads an entire binary file into a byte vector
    std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
    {
        // Open at the end
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            throw std::runtime_error("could not open input: " + path.u8string());
        // Length
        const auto end = stream.tellg();
        if (end < 0)
            throw std::runtime_error("could not size input: " + path.u8string());
        // Allocate the vector
        std::vector<uint8_t> data(static_cast<size_t>(end));
        // Seek to beginning 
        stream.seekg(0);
        // Read and check success
        if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), end))
            throw std::runtime_error("could not read input: " + path.u8string());
        return data;
    }

    // Write only the compressed bytes; format and original size are supplied separately.
    void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data)
    {
        // Open destination in binary/truncate mode
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::runtime_error("could not open output: " + path.u8string());
        if (!data.empty())
            // Write the bytes
            stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        stream.close();
        if (!stream)
            throw std::runtime_error("could not write or close output: " + path.u8string());
    }

    // Look up the requested BC format and reject unsupported names.
    const TextureFormatInfo& FindCase(std::wstring_view name)
    {
        for (const auto& value : kCases)
            if (name == value.name)
                return value;
        throw std::runtime_error("format must be BC1, BC3, BC4, BC5, or BC7");
    }

    // Converts an option value into an integer and checks if the string was consumed, and the result range
    int ParseInteger(std::wstring_view text, const char* label, int minimum, int maximum)
    {
        size_t used = 0;
        const long parsed = std::stol(std::wstring(text), &used, 10);
        if (used != text.size() || parsed < minimum || parsed > maximum)
            throw std::runtime_error(std::string(label) + " is out of range");
        return static_cast<int>(parsed);
    }

    // =============================================================================
    // Format-specific reverse shuffles
    // These four methods rebuild the original block structures from separate field streams
    
    // BC1 stores two color endpoints and indices in separate streams (2/2/4 bytes).
    void UnshuffleBC1(uint8_t* destination, const uint8_t* source, size_t count)
    {
        const uint8_t* color0 = source;
        const uint8_t* color1 = color0 + count * 2;
        const uint8_t* indices = color1 + count * 2;

        auto* blocks = reinterpret_cast<LocalBC1Block*>(destination);
        for (size_t index = 0; index < count; ++index)
        {
            blocks[index].color0 = Read<uint16_t>(color0);
            blocks[index].color1 = Read<uint16_t>(color1);
            blocks[index].indices = Read<uint32_t>(indices);
        }
    }

    // BC3 restores alpha fields first, then the embedded BC1 color fields.
    void UnshuffleBC3(uint8_t* destination, const uint8_t* source, size_t count)
    {
        const uint8_t* alpha0 = source;
        const uint8_t* alpha1 = alpha0 + count;
        const uint8_t* alphaIndices = alpha1 + count;
        const uint8_t* color0 = alphaIndices + count * 6;
        const uint8_t* color1 = color0 + count * 2;
        const uint8_t* colorIndices = color1 + count * 2;

        auto* blocks = reinterpret_cast<LocalBC3Block*>(destination);
        for (size_t index = 0; index < count; ++index)
        {
            blocks[index].alpha0 = Read<uint8_t>(alpha0);
            blocks[index].alpha1 = Read<uint8_t>(alpha1);
            std::memcpy(blocks[index].alphaIndices, alphaIndices, 6);
            alphaIndices += 6;

            blocks[index].color.color0 = Read<uint16_t>(color0);
            blocks[index].color.color1 = Read<uint16_t>(color1);
            blocks[index].color.indices = Read<uint32_t>(colorIndices);
        }
    }

    // BC4 reconstructs each block from two endpoints and six bytes of indices.
    void UnshuffleBC4(uint8_t* destination, const uint8_t* source, size_t count)
    {
        const uint8_t* endpoint0 = source;
        const uint8_t* endpoint1 = endpoint0 + count;
        const uint8_t* indices = endpoint1 + count;

        auto* blocks = reinterpret_cast<LocalBC4Block*>(destination);
        for (size_t index = 0; index < count; ++index)
        {
            blocks[index].endpoint0 = Read<uint8_t>(endpoint0);
            blocks[index].endpoint1 = Read<uint8_t>(endpoint1);
            std::memcpy(blocks[index].indices, indices, 6);
            indices += 6;
        }
    }

    // BC5 reconstructs both channel blocks from their separate field streams.
    void UnshuffleBC5(uint8_t* destination, const uint8_t* source, size_t count)
    {
        const uint8_t* redEndpoint0 = source;
        const uint8_t* redEndpoint1 = redEndpoint0 + count;
        const uint8_t* redIndices = redEndpoint1 + count;
        const uint8_t* greenEndpoint0 = redIndices + count * 6;
        const uint8_t* greenEndpoint1 = greenEndpoint0 + count;
        const uint8_t* greenIndices = greenEndpoint1 + count;

        auto* blocks = reinterpret_cast<LocalBC5Block*>(destination);
        for (size_t index = 0; index < count; ++index)
        {
            blocks[index].red.endpoint0 = Read<uint8_t>(redEndpoint0);
            blocks[index].red.endpoint1 = Read<uint8_t>(redEndpoint1);
            std::memcpy(blocks[index].red.indices, redIndices, 6);
            redIndices += 6;

            blocks[index].green.endpoint0 = Read<uint8_t>(greenEndpoint0);
            blocks[index].green.endpoint1 = Read<uint8_t>(greenEndpoint1);
            std::memcpy(blocks[index].green.indices, greenIndices, 6);
            greenIndices += 6;
        }
    }

    // This is the dispatcher and leftover-block handler.
    void Unshuffle(
        const TextureFormatInfo& value,
        uint8_t* destination,
        const uint8_t* source,
        size_t blockCount)
    {
        // For level 7, no reverse transformation is needed
        if (value.transformId == 7)
        {
            std::memcpy(destination, source, blockCount * value.bytesPerBlock);
            return;
        }
        // Reverse only complete shuffle groups; leftover blocks remain in original order.
        const size_t shuffledCount = blockCount - blockCount % value.groupSize;
        if (value.transformId == 1)
            UnshuffleBC1(destination, source, shuffledCount);
        else if (value.transformId == 2)
            UnshuffleBC3(destination, source, shuffledCount);
        else if (value.transformId == 3)
            UnshuffleBC4(destination, source, shuffledCount);
        else
            UnshuffleBC5(destination, source, shuffledCount);

        const size_t shuffledBytes = shuffledCount * value.bytesPerBlock;
        const size_t tailBytes = (blockCount - shuffledCount) * value.bytesPerBlock;
        if (tailBytes != 0)
            std::memcpy(destination + shuffledBytes, source + shuffledBytes, tailBytes);
    }

    // Complete reconstruction check
    void Verify(
        const TextureFormatInfo& value,
        const std::vector<uint8_t>& original,
        const std::vector<uint8_t>& compressed)
    {
        // Original payload shape
        if (original.empty() || original.size() % value.bytesPerBlock != 0)
            throw std::runtime_error("original payload must be non-empty and block-aligned");
        std::vector<uint8_t> shuffled(original.size());

        // Sstd decompress
        const size_t size = ZSTD_decompress(
            shuffled.data(),
            shuffled.size(),
            compressed.data(),
            compressed.size());
        if (ZSTD_isError(size) || size != original.size())
            throw std::runtime_error("Zstd verification failed");
        std::vector<uint8_t> restored(original.size());

        // Reverse conditioning
        Unshuffle(value, restored.data(), shuffled.data(), original.size() / value.bytesPerBlock);

        // Compare bytes
        if (restored != original)
            throw std::runtime_error("GACL reverse verification failed");
    }

    // Shuffle and compress a BC payload, then verify that it restores the input. The main encoding operation.
    std::vector<uint8_t> Compress(
        const TextureFormatInfo& value,
        const std::vector<uint8_t>& input,
        int level,
        int targetBlockSize)
    {
        // The input is raw block data. Partial blocks and empty payloads are invalid.
        if (input.empty() || input.size() % value.bytesPerBlock != 0)
            throw std::runtime_error("input must contain a non-empty, block-aligned BC payload");
        // Rearrange supported formats before compression; keep BC7 bytes unchanged.
        std::vector<uint8_t> shuffled(input.size());
        if (value.shuffle != nullptr)
        {
            if (FAILED(value.shuffle(shuffled.data(), input.data(), input.size(), kShuffleVersion)))
                throw std::runtime_error("GACL stable shuffle failed");
        }
        else
        {
            shuffled = input;
        }

        // Create the zstd compression context
        ZSTD_CCtx* context = ZSTD_createCCtx();
        if (!context)
            throw std::runtime_error("could not create Zstd context");
        // Apply zstd settings
        const ZSTD_compressionParameters params = ZSTD_getCParams(level, input.size(), 0);
        size_t status = ZSTD_CCtx_setCParams(context, params);
        if (!ZSTD_isError(status))
            status = ZSTD_CCtx_setParameter(context, ZSTD_c_targetCBlockSize, targetBlockSize);
        std::vector<uint8_t> compressed(ZSTD_compressBound(shuffled.size()));

        // Compress
        if (!ZSTD_isError(status))
            status = ZSTD_compress2(
                context,
                compressed.data(),
                compressed.size(),
                shuffled.data(),
                shuffled.size());
        ZSTD_freeCCtx(context);
        if (ZSTD_isError(status))
            throw std::runtime_error(ZSTD_getErrorName(status));
        compressed.resize(status);

        // Check decoding and reverse shuffling before publishing the compressed bytes.
        Verify(value, input, compressed);
        return compressed;
    }

    // Searches the arguments for a named option and returns the following value.
    std::wstring RequireOptionValue(int argc, wchar_t** argv, std::wstring_view name)
    {
        for (int index = 1; index + 1 < argc; ++index)
            if (std::wstring_view(argv[index]) == name)
                return argv[index + 1];
        throw std::runtime_error("missing required command-line option");
    }
}

// Handle version, encoding, or verification requests and report failures.
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--version")
        {
            std::cout << "GACLContentTool " << kVersion << '\n';
            return 0;
        }
        // Verification reads existing files; encoding writes only after round-trip checks.
        const bool verify = argc > 1 && std::wstring_view(argv[1]) == L"--verify";
        const auto& format = FindCase(RequireOptionValue(argc, argv, L"--format"));
        if (verify)
        {
            const auto original = ReadFile(RequireOptionValue(argc, argv, L"--original"));
            const auto compressed = ReadFile(RequireOptionValue(argc, argv, L"--input"));
            Verify(format, original, compressed);
            std::cout << "GACL verification passed; transform_id=" << format.transformId << '\n';
            return 0;
        }
        // The caller supplies codec settings explicitly; this native CLI has no level default.
        const int level = ParseInteger(RequireOptionValue(argc, argv, L"--zstd-level"), "zstd level", 1, 22);
        const int targetBlockSize = ParseInteger(
            RequireOptionValue(argc, argv, L"--target-block-size"), "target block size", 1, 1 << 20);
        const auto input = ReadFile(RequireOptionValue(argc, argv, L"--input"));
        const auto output = std::filesystem::path(RequireOptionValue(argc, argv, L"--output"));
        const auto compressed = Compress(format, input, level, targetBlockSize);
        WriteFile(output, compressed);
        std::cout << "transform_id=" << format.transformId << " uncompressed_size=" << input.size()
                  << " compressed_size=" << compressed.size() << '\n';
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "error: " << exception.what() << '\n';
        return 1;
    }
}
