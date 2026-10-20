// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

/* Steps:
Read one original file
    ↓
Compress its bytes using the existing GDeflate library
    ↓
Decode and compare the result in memory
    ↓
Write our 32-byte header followed by the compressed bytes
    ↓
Read the generated file back, decode it, and compare again
*/

// This file does not implement the compression algorithm. It calls the existing GDeflate library


#define NOMINMAX
#include <Windows.h>
#include "GDeflate.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kFileMagic = 0x31464447; // "GDF1" in little-endian byte order.
    constexpr uint16_t kFileVersion = 1;
    constexpr const char* kToolVersion = "1.0.0";

    // GDeflate content header. Also defined in factory_contracts.py
#pragma pack(push, 1)
    struct ContentHeader
    {
        uint32_t magic;
        uint16_t version;
        uint16_t headerSize;
        uint32_t compressionLevel;
        uint32_t reserved;
        uint64_t uncompressedSize;
        uint64_t compressedSize;
    };
#pragma pack(pop)

    static_assert(sizeof(ContentHeader) == 32, "Unexpected GDeflate content header size");

    struct Options
    {
        fs::path input;
        fs::path output;
        uint32_t level = 9;     // Defult GDeflate level
        bool overwrite = false; // Replacement permission
    };

    // Print command-line forms and exit with a usage error.
    [[noreturn]] void Usage(const wchar_t* program)
    {
        std::wcerr
            << L"Usage:\n"
            << L"  " << program << L" --input <file> --output <file> [--level 1-12] [--overwrite]\n"
            << L"  " << program << L" --version\n";
        std::exit(2);
    }

    // Check a givewn level from command line
    uint32_t ParseLevel(const std::wstring& value)
    {
        // Require digits
        if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
        {
            throw std::runtime_error("Compression level must be a decimal integer between 1 and 12");
        }
        const auto level = std::stoul(value);

        // Check range
        if (level < GDeflate::MinimumCompressionLevel || level > GDeflate::MaximumCompressionLevel)
        {
            throw std::runtime_error("Compression level must be between 1 and 12");
        }
        return static_cast<uint32_t>(level);
    }

    // Parse single-file compression options and require both paths.
    Options ParseOptions(int argc, wchar_t** argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring argument = argv[i];
            // Consume the next argument or report a missing option value.
            auto requireValue = [&]() -> std::wstring
            {
                if (++i >= argc)
                {
                    throw std::runtime_error("Missing value for " + fs::path(argument).u8string());
                }
                return argv[i];
            };

            if (argument == L"--input")
            {
                options.input = requireValue();
            }
            else if (argument == L"--output")
            {
                options.output = requireValue();
            }
            else if (argument == L"--level")
            {
                options.level = ParseLevel(requireValue());
            }
            else if (argument == L"--overwrite")
            {
                options.overwrite = true;
            }
            else if (argument == L"--help" || argument == L"-h")
            {
                Usage(argv[0]);
            }
            else
            {
                throw std::runtime_error("Unknown argument: " + fs::path(argument).u8string());
            }
        }

        if (options.input.empty() || options.output.empty())
        {
            Usage(argv[0]);
        }
        return options;
    }

    // Read an entire binary file into mdmory.
    // First checks size against process and I/O limits.
    std::vector<uint8_t> ReadFile(const fs::path& path)
    {
        // Open in binary mode, positioned at the end:
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error("Failed to open input file: " + path.u8string());
        }
        const auto end = stream.tellg();    // Byte count
        if (end < 0)
        {
            throw std::runtime_error("Failed to query input size: " + path.u8string());
        }

        const auto fileSize = static_cast<uint64_t>(end);

        // Check size fits 1) addressable size and 2) stream API's size argument
        if (fileSize > static_cast<uint64_t>(SIZE_MAX) ||
            fileSize > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        {
            throw std::runtime_error("Input file exceeds this process address space: " + path.u8string());
        }

        // Allocate byte vector, go to beginning
        std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
        stream.seekg(0, std::ios::beg);

        // Read bytes and check success
        if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            throw std::runtime_error("Failed to read input file: " + path.u8string());
        }
        return bytes;
    }

    // Write the header and payload to a temporary file, then replace the destination.
    void WriteFileAtomically(
        const fs::path& path,
        const ContentHeader& header,
        const std::vector<uint8_t>& payload,
        bool overwrite)
    {
        // Destination exists and overwrite option is false -> fail
        if (fs::exists(path) && !overwrite)
        {
            throw std::runtime_error("Output already exists (use --overwrite): " + path.u8string());
        }
        // Create parent path if necessary
        if (!path.parent_path().empty())
        {
            fs::create_directories(path.parent_path());
        }

        // Temp file has ".syncing" suffix
        auto temporary = path;
        temporary += L".syncing";
        if (fs::exists(temporary) || fs::is_symlink(temporary))
        {
            throw std::runtime_error("Temporary output already exists: " + temporary.u8string());
        }
        try
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                throw std::runtime_error("Failed to create output file: " + temporary.u8string());
            }
            // Write header
            stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (!payload.empty())
            {
                // Write payload
                stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            }
            if (!stream)
            {
                throw std::runtime_error("Failed to write output file: " + temporary.u8string());
            }
            stream.close();
            if (!stream)
            {
                throw std::runtime_error("Failed to close output file: " + temporary.u8string());
            }
            // Move from temp to permanent file
            if (!MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    (overwrite ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH))
            {
                throw std::runtime_error("Failed to atomically replace output file: " + path.u8string());
            }
        }
        catch (...)
        {
            std::error_code ignored;
            fs::remove(temporary, ignored); // Delete temp file
            throw;
        }
    }

    // Check the stored sizes and header, then decompress the content payload.
    // This is our read-back verification path
    std::vector<uint8_t> DecompressContentFile(const fs::path& path)
    {
        // Open and measure file size
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error("Failed to open GDeflate content: " + path.u8string());
        }
        const auto end = stream.tellg();
        if (end < 0)
        {
            throw std::runtime_error("Failed to query GDeflate content size: " + path.u8string());
        }
        const auto fileSize = static_cast<uint64_t>(end);
        stream.seekg(0, std::ios::beg);

        // Read and check ContentHeader
        ContentHeader header{};
        if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)))
        {
            throw std::runtime_error("GDeflate content header is truncated: " + path.u8string());
        }
        if (header.magic != kFileMagic ||
            header.version != kFileVersion ||
            header.headerSize != sizeof(ContentHeader) ||
            header.reserved != 0)
        {
            throw std::runtime_error("Unsupported or invalid GDeflate content header: " + path.u8string());
        }
        if (header.compressionLevel < GDeflate::MinimumCompressionLevel ||
            header.compressionLevel > GDeflate::MaximumCompressionLevel)
        {
            throw std::runtime_error("Invalid compression level in header: " + path.u8string());
        }

        // Check recorded sizes
        if (fileSize != header.headerSize + header.compressedSize)
        {
            throw std::runtime_error("GDeflate content size does not match the header: " + path.u8string());
        }
        if (header.uncompressedSize > static_cast<uint64_t>(SIZE_MAX) ||
            header.compressedSize > static_cast<uint64_t>(SIZE_MAX))
        {
            throw std::runtime_error("GDeflate content exceeds this process address space: " + path.u8string());
        }
        if (header.compressedSize > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        {
            throw std::runtime_error("GDeflate payload is too large for stream I/O: " + path.u8string());
        }

        // Read the compressed payload
        std::vector<uint8_t> payload(static_cast<size_t>(header.compressedSize));
        if (!payload.empty() && !stream.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size())))
        {
            throw std::runtime_error("Failed to read GDeflate payload: " + path.u8string());
        }
        std::vector<uint8_t> output(static_cast<size_t>(header.uncompressedSize));

        // Finally, call GDeflate::Decompress
        // It returns the decoded bytes. The comparison with the original happens in its caller.
        if (!GDeflate::Decompress(output.data(), output.size(), payload.data(), payload.size(), 1))
        {
            throw std::runtime_error("GDeflate decompression failed: " + path.u8string());
        }
        return output;
    }

    // Compress one regular file and validate both the in-memory and written round trips.
    void CompressFile(const fs::path& source, const fs::path& output, uint32_t level, bool overwrite)
    {
        // Source/output file checks
        if (!fs::is_regular_file(source))
        {
            throw std::runtime_error("Input must be a regular file: " + source.u8string());
        }
        if (fs::is_directory(output))
        {
            throw std::runtime_error("Output must be a file, not a directory: " + output.u8string());
        }
        if (fs::exists(output) && fs::equivalent(source, output))
        {
            throw std::runtime_error("Output must not refer to the input file: " + output.u8string());
        }

        // Read the original file
        const auto input = ReadFile(source);
        if (input.empty())
        {
            throw std::runtime_error("GDeflate does not accept empty input: " + source.u8string());
        }

        // Allocate the compressed buffer based on the estimate
        const auto bound = GDeflate::CompressBound(input.size());
        std::vector<uint8_t> compressed(bound);
        size_t compressedSize = compressed.size();

        // Compress the file
        const auto compressionSucceeded = GDeflate::Compress(
            compressed.data(),
            &compressedSize,
            input.data(),
            input.size(),
            level,
            GDeflate::COMPRESS_SINGLE_THREAD);
        if (!compressionSucceeded)
        {
            throw std::runtime_error("GDeflate compression failed: " + source.u8string());
        }
        compressed.resize(compressedSize);

        // Verify the in-memory result by decompressing and comparing to original
        std::vector<uint8_t> roundTrip(input.size());
        if (!GDeflate::Decompress(roundTrip.data(), roundTrip.size(), compressed.data(), compressed.size(), 1))
        {
            throw std::runtime_error("GDeflate CPU validation failed: " + source.u8string());
        }
        if (roundTrip != input)
        {
            throw std::runtime_error("CPU round-trip validation failed: " + source.u8string());
        }

        // Populate the header and write the file
        const ContentHeader header{
            kFileMagic,
            kFileVersion,
            static_cast<uint16_t>(sizeof(ContentHeader)),
            level,
            0,
            static_cast<uint64_t>(input.size()),
            static_cast<uint64_t>(compressed.size())};
        WriteFileAtomically(output, header, compressed, overwrite);

        // Second round trip ti verify the actual stored file
        if (DecompressContentFile(output) != input)
        {
            throw std::runtime_error("Written GDeflate file does not round-trip: " + output.u8string());
        }
    }
}

// Handle version or single-file compression requests and report failures.
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring(argv[1]) == L"--version")
        {
            std::cout << "GDeflateContentTool " << kToolVersion << '\n';
            return 0;
        }
        const auto options = ParseOptions(argc, argv);
        CompressFile(options.input, options.output, options.level, options.overwrite);
        std::wcout << L"created " << options.output.native() << L'\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
