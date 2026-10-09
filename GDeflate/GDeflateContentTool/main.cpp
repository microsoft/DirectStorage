// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#define NOMINMAX
#include <Windows.h>
#include "GDeflate.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t kFileMagic = 0x31464447; // "GDF1" in little-endian byte order.
    constexpr uint16_t kFileVersion = 1;
    constexpr const char* kToolVersion = "1.0.0";

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
        uint32_t level = 9;
        bool recursive = false;
        bool overwrite = false;
        bool verifyOnly = false;
    };

    struct Result
    {
        fs::path source;
        fs::path output;
        uint64_t sourceSize;
        uint64_t outputSize;
        uint32_t level;
        double milliseconds;
        std::string sourceSha256;
        std::string payloadSha256;
    };

    [[noreturn]] void Usage(const char* program)
    {
        std::cerr
            << "Usage:\n"
            << "  " << program << " --input <file-or-directory> --output <file-or-directory>\n"
            << "      [--level 1-12] [--recursive] [--overwrite]\n"
            << "  " << program << " --verify <file-or-directory> [--recursive]\n";
        std::exit(2);
    }

    uint32_t ParseLevel(const std::string& value)
    {
        const auto level = static_cast<uint32_t>(std::stoul(value));
        if (level < GDeflate::MinimumCompressionLevel || level > GDeflate::MaximumCompressionLevel)
        {
            throw std::runtime_error("Compression level must be between 1 and 12");
        }
        return level;
    }

    Options ParseOptions(int argc, char** argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            auto requireValue = [&]() -> std::string
            {
                if (++i >= argc)
                {
                    throw std::runtime_error("Missing value for " + argument);
                }
                return argv[i];
            };

            if (argument == "--input")
            {
                options.input = requireValue();
            }
            else if (argument == "--output")
            {
                options.output = requireValue();
            }
            else if (argument == "--level")
            {
                options.level = ParseLevel(requireValue());
            }
            else if (argument == "--recursive")
            {
                options.recursive = true;
            }
            else if (argument == "--overwrite")
            {
                options.overwrite = true;
            }
            else if (argument == "--verify")
            {
                options.input = requireValue();
                options.verifyOnly = true;
            }
            else if (argument == "--help" || argument == "-h")
            {
                Usage(argv[0]);
            }
            else
            {
                throw std::runtime_error("Unknown argument: " + argument);
            }
        }

        if (options.input.empty() || (!options.verifyOnly && options.output.empty()))
        {
            Usage(argv[0]);
        }
        return options;
    }

    constexpr uint32_t RotateRight(uint32_t value, uint32_t bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }

    std::string Sha256(const uint8_t* data, size_t size)
    {
        static constexpr uint32_t constants[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::vector<uint8_t> padded(data, data + size);
        padded.push_back(0x80);
        while ((padded.size() % 64) != 56) padded.push_back(0);
        const uint64_t bitLength = static_cast<uint64_t>(size) * 8;
        for (int shift = 56; shift >= 0; shift -= 8) padded.push_back(static_cast<uint8_t>(bitLength >> shift));
        for (size_t offset = 0; offset < padded.size(); offset += 64)
        {
            uint32_t words[64]{};
            for (size_t i = 0; i < 16; ++i)
                words[i] = (static_cast<uint32_t>(padded[offset + i * 4]) << 24) | (static_cast<uint32_t>(padded[offset + i * 4 + 1]) << 16) | (static_cast<uint32_t>(padded[offset + i * 4 + 2]) << 8) | padded[offset + i * 4 + 3];
            for (size_t i = 16; i < 64; ++i)
            {
                const auto s0 = RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
                const auto s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
                words[i] = words[i - 16] + s0 + words[i - 7] + s1;
            }
            uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4], f = hash[5], g = hash[6], h = hash[7];
            for (size_t i = 0; i < 64; ++i)
            {
                const auto s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                const auto choice = (e & f) ^ (~e & g);
                const auto temp1 = h + s1 + choice + constants[i] + words[i];
                const auto s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
                const auto majority = (a & b) ^ (a & c) ^ (b & c);
                const auto temp2 = s0 + majority;
                h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
            }
            hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d; hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
        }
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (const auto value : hash) stream << std::setw(8) << value;
        return stream.str();
    }

    std::vector<uint8_t> ReadFile(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error("Failed to open input file: " + path.string());
        }
        const auto end = stream.tellg();
        if (end < 0)
        {
            throw std::runtime_error("Failed to query input size: " + path.string());
        }
        const auto fileSize = static_cast<uint64_t>(end);
        if (fileSize > static_cast<uint64_t>(SIZE_MAX) || fileSize > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        {
            throw std::runtime_error("Input file exceeds this process address space: " + path.string());
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
        stream.seekg(0, std::ios::beg);
        if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            throw std::runtime_error("Failed to read input file: " + path.string());
        }
        return bytes;
    }

    void WriteFileAtomically(const fs::path& path, const ContentHeader& header, const std::vector<uint8_t>& payload, bool overwrite)
    {
        if (fs::exists(path) && !overwrite)
        {
            throw std::runtime_error("Output already exists (use --overwrite): " + path.string());
        }
        if (!path.parent_path().empty())
        {
            fs::create_directories(path.parent_path());
        }
        const auto temporary = path.string() + ".syncing";
        try
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                throw std::runtime_error("Failed to create output file: " + temporary);
            }
            stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
            if (!payload.empty())
            {
                stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            }
            if (!stream)
            {
                throw std::runtime_error("Failed to write output file: " + temporary);
            }
            stream.close();
            if (!MoveFileExW(
                    fs::path(temporary).c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::runtime_error("Failed to atomically replace output file: " + path.string());
            }
        }
        catch (...)
        {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            throw;
        }
    }

    std::string EscapeJson(const std::string& value)
    {
        std::ostringstream stream;
        for (const unsigned char character : value)
        {
            switch (character)
            {
            case '\\': stream << "\\\\"; break;
            case '"': stream << "\\\""; break;
            case '\b': stream << "\\b"; break;
            case '\f': stream << "\\f"; break;
            case '\n': stream << "\\n"; break;
            case '\r': stream << "\\r"; break;
            case '\t': stream << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(character) << std::dec;
                }
                else
                {
                    stream << static_cast<char>(character);
                }
            }
        }
        return stream.str();
    }

    ContentHeader ReadHeader(std::ifstream& stream, const fs::path& path)
    {
        ContentHeader header{};
        if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)))
        {
            throw std::runtime_error("GDeflate content header is truncated: " + path.string());
        }
        if (header.magic != kFileMagic || header.version != kFileVersion || header.headerSize != sizeof(ContentHeader) || header.reserved != 0)
        {
            throw std::runtime_error("Unsupported or invalid GDeflate content header: " + path.string());
        }
        if (header.compressionLevel < GDeflate::MinimumCompressionLevel ||
            header.compressionLevel > GDeflate::MaximumCompressionLevel)
        {
            throw std::runtime_error("Invalid compression level in header: " + path.string());
        }
        return header;
    }

    std::vector<uint8_t> DecompressContentFile(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            throw std::runtime_error("Failed to open GDeflate content: " + path.string());
        }
        const auto end = stream.tellg();
        if (end < 0)
        {
            throw std::runtime_error("Failed to query GDeflate content size: " + path.string());
        }
        const auto fileSize = static_cast<uint64_t>(end);
        stream.seekg(0, std::ios::beg);
        const auto header = ReadHeader(stream, path);
        if (fileSize != header.headerSize + header.compressedSize)
        {
            throw std::runtime_error("GDeflate content size does not match the header: " + path.string());
        }
        if (header.uncompressedSize > static_cast<uint64_t>(SIZE_MAX) || header.compressedSize > static_cast<uint64_t>(SIZE_MAX))
        {
            throw std::runtime_error("GDeflate content exceeds this process address space: " + path.string());
        }
        if (header.compressedSize > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        {
            throw std::runtime_error("GDeflate payload is too large for stream I/O: " + path.string());
        }

        std::vector<uint8_t> payload(static_cast<size_t>(header.compressedSize));
        if (!payload.empty() && !stream.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size())))
        {
            throw std::runtime_error("Failed to read GDeflate payload: " + path.string());
        }
        std::vector<uint8_t> output(static_cast<size_t>(header.uncompressedSize));
        if (!GDeflate::Decompress(output.data(), output.size(), payload.data(), payload.size(), 1))
        {
            throw std::runtime_error("GDeflate decompression failed: " + path.string());
        }
        return output;
    }

    Result CompressFile(const fs::path& source, const fs::path& output, uint32_t level, bool overwrite)
    {
        const auto input = ReadFile(source);
        if (input.empty())
        {
            throw std::runtime_error("GDeflate does not accept empty input: " + source.string());
        }
        const auto bound = GDeflate::CompressBound(input.size());
        std::vector<uint8_t> compressed(bound);
        size_t compressedSize = compressed.size();

        const auto start = std::chrono::steady_clock::now();
        const auto compressionSucceeded = GDeflate::Compress(
            compressed.data(),
            &compressedSize,
            input.data(),
            input.size(),
            level,
            GDeflate::COMPRESS_SINGLE_THREAD);
        const auto stop = std::chrono::steady_clock::now();
        if (!compressionSucceeded)
        {
            throw std::runtime_error("GDeflate compression failed: " + source.string());
        }
        compressed.resize(compressedSize);

        std::vector<uint8_t> roundTrip(input.size());
        if (!GDeflate::Decompress(roundTrip.data(), roundTrip.size(), compressed.data(), compressed.size(), 1))
        {
            throw std::runtime_error("GDeflate CPU validation failed: " + source.string());
        }
        if (roundTrip != input)
        {
            throw std::runtime_error("CPU round-trip validation failed: " + source.string());
        }

        const ContentHeader header{
            kFileMagic,
            kFileVersion,
            static_cast<uint16_t>(sizeof(ContentHeader)),
            level,
            0,
            static_cast<uint64_t>(input.size()),
            static_cast<uint64_t>(compressed.size())};
        WriteFileAtomically(output, header, compressed, overwrite);
        if (DecompressContentFile(output) != input)
        {
            throw std::runtime_error("Written GDeflate file does not round-trip: " + output.string());
        }

        return {
            source,
            output,
            static_cast<uint64_t>(input.size()),
            static_cast<uint64_t>(sizeof(ContentHeader) + compressed.size()),
            level,
            std::chrono::duration<double, std::milli>(stop - start).count(),
            Sha256(input.data(), input.size()),
            Sha256(compressed.data(), compressed.size())};
    }

    std::vector<fs::path> EnumerateFiles(const fs::path& root, bool recursive, bool contentFilesOnly)
    {
        std::vector<fs::path> files;
        const auto include = [contentFilesOnly](const fs::path& path)
        {
            return !contentFilesOnly || path.extension() == ".gdeflate";
        };

        if (fs::is_regular_file(root))
        {
            if (include(root))
            {
                files.push_back(root);
            }
        }
        else if (fs::is_directory(root))
        {
            if (recursive)
            {
                for (const auto& entry : fs::recursive_directory_iterator(root))
                {
                    if (entry.is_regular_file() && include(entry.path()))
                    {
                        files.push_back(entry.path());
                    }
                }
            }
            else
            {
                for (const auto& entry : fs::directory_iterator(root))
                {
                    if (entry.is_regular_file() && include(entry.path()))
                    {
                        files.push_back(entry.path());
                    }
                }
            }
        }
        else
        {
            throw std::runtime_error("Input path does not exist: " + root.string());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    void WriteManifest(const fs::path& outputRoot, const std::vector<Result>& results)
    {
        fs::create_directories(outputRoot);
        const auto manifestPath = outputRoot / "variants.json";
        std::ofstream stream(manifestPath, std::ios::trunc);
        if (!stream)
        {
            throw std::runtime_error("Failed to create manifest: " + manifestPath.string());
        }
        stream << "{\n  \"schema_version\": 1,\n  \"generator\": \"GDeflateContentTool\",\n  \"variants\": [\n";
        for (size_t i = 0; i < results.size(); ++i)
        {
            const auto& result = results[i];
            const auto relativeOutput = fs::relative(result.output, outputRoot).generic_string();
            stream << "    {\n"
                   << "      \"source\": \"" << EscapeJson(result.source.generic_string()) << "\",\n"
                   << "      \"output\": \"" << EscapeJson(relativeOutput) << "\",\n"
                   << "      \"source_size\": " << result.sourceSize << ",\n"
                   << "      \"output_size\": " << result.outputSize << ",\n"
                   << "      \"source_sha256\": \"" << result.sourceSha256 << "\",\n"
                   << "      \"payload_sha256\": \"" << result.payloadSha256 << "\",\n"
                   << "      \"compression_level\": " << result.level << ",\n"
                   << "      \"validation\": \"pass\"\n"
                   << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
        }
        stream << "  ]\n}\n";
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--version")
        {
            std::cout << "GDeflateContentTool " << kToolVersion << '\n';
            return 0;
        }
        const auto options = ParseOptions(argc, argv);
        const auto files = EnumerateFiles(options.input, options.recursive, options.verifyOnly);
        if (files.empty())
        {
            throw std::runtime_error("No input files were found");
        }

        if (options.verifyOnly)
        {
            for (const auto& file : files)
            {
                DecompressContentFile(file);
                std::cout << "verified " << file << '\n';
            }
            return 0;
        }

        const bool inputIsDirectory = fs::is_directory(options.input);
        std::vector<Result> results;
        for (const auto& source : files)
        {
            fs::path output;
            if (inputIsDirectory)
            {
                output = options.output / fs::relative(source, options.input);
                output += ".gdeflate";
            }
            else
            {
                output = options.output;
            }
            auto result = CompressFile(source, output, options.level, options.overwrite);
            result.source = inputIsDirectory ? fs::relative(source, options.input).generic_string() : source.filename().generic_string();
            results.push_back(std::move(result));
            std::cout << "created " << output << '\n';
        }

        const auto manifestRoot = inputIsDirectory ? options.output : options.output.parent_path();
        WriteManifest(manifestRoot.empty() ? fs::current_path() : manifestRoot, results);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
