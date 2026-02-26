/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

#include "adapters.h"
#include "buffers.h"
#include "gpuwork.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace Decompression;

static std::vector<uint8_t> LoadFile(std::filesystem::path path)
{
    std::vector<uint8_t> data;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (file)
    {
        auto size = file.tellg();
        data.resize(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(data.data()), size);
    }
    return data;
}

static void SaveFile(std::filesystem::path path, const std::vector<uint8_t>& data)
{
    std::ofstream file(path, std::ios::binary);
    if (file)
    {
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
    }
}

// Decompresses a single frame using the zstd educational decoder.
// The educational decoder assumes single frame decompression.
static std::vector<uint8_t> DecompressFrame(uint8_t* frameData, size_t frameDataSize)
{
    auto uncompressedSize = ZSTD_get_decompressed_size(frameData, frameDataSize);
    std::vector<uint8_t> decompressedData(uncompressedSize);
    ZSTD_decompress(decompressedData.data(), uncompressedSize, frameData, frameDataSize);
    return decompressedData;
}

// Decompresses all frames in the compressed data using the zstd educational decoder to produce reference decompressed
// frames for validating GPU decompression results.
static Frames CpuDecompressFrames(
    std::vector<uint8_t>& compressedData,
    const OffsetsAndSizes& offsetsAndSizes)
{
    Frames decompressedFrames;
    for (size_t i = 0; i < offsetsAndSizes.InputOffsets.size(); ++i)
    {
        auto& inputOffsetAndSize = offsetsAndSizes.InputOffsets[i];
        auto& outputOffsetAndSize = offsetsAndSizes.OutputOffsets[i];
        auto decompressedFrame =
            DecompressFrame(compressedData.data() + inputOffsetAndSize.offs, inputOffsetAndSize.size);
        EXPECT_EQ(decompressedFrame.size(), outputOffsetAndSize.size);
        decompressedFrames.push_back(std::move(decompressedFrame));
    }
    return decompressedFrames;
}

// Computes the offsets and sizes of each frame in the compressed data.
static OffsetsAndSizes ComputeOffsetsAndSizes(const std::vector<uint8_t>& alignedFrameData, size_t rawFrameDataSize)
{
    zstdgpu_CountFramesAndBlocksInfo fbInfo{};
    uint32_t zstdCompressedFramesMemorySizeInBytes = static_cast<uint32_t>(alignedFrameData.size());
    zstdgpu_CountFramesAndBlocks(
        &fbInfo,
        alignedFrameData.data(),
        zstdCompressedFramesMemorySizeInBytes,
        static_cast<uint32_t>(rawFrameDataSize));

    OffsetsAndSizes offsetsAndSizes;
    offsetsAndSizes.InputOffsets.resize(fbInfo.frameCount);
    offsetsAndSizes.OutputOffsets.resize(fbInfo.frameCount);
    std::vector<zstdgpu_FrameInfo> zstdFrameInfos(fbInfo.frameCount);
    zstdgpu_CollectFrames(
        offsetsAndSizes.InputOffsets.data(),
        zstdFrameInfos.data(),
        fbInfo.frameCount,
        alignedFrameData.data(),
        zstdCompressedFramesMemorySizeInBytes,
        static_cast<uint32_t>(rawFrameDataSize));

    // Compute offsets of frame in the output data using the decompressed frame sizes.
    uint32_t offs = 0;
    uint32_t vcnt = 0;
    for (uint32_t i = 0; i < fbInfo.frameCount; ++i)
    {
        offsetsAndSizes.OutputOffsets[i].offs = offs;
        offsetsAndSizes.OutputOffsets[i].size = (uint32_t)zstdFrameInfos[i].uncompSize;

        offs += offsetsAndSizes.OutputOffsets[i].size;
        offs = ZSTDGPU_ALIGN(offs, 256);

        vcnt += zstdFrameInfos[i].uncompSize != 0 ? 1 : 0;
    }
    offsetsAndSizes.UnCompressedFramesMemorySizeInBytes = offs;
    EXPECT_EQ(fbInfo.frameCount, vcnt); // All frames should have valid uncompressed size.

    return offsetsAndSizes;
}

struct ZstFileInfo
{
    size_t TotalFramesSizeBytes;
    OffsetsAndSizes FrameOffsetsAndSizes;
    std::vector<uint8_t> FrameDataAligned; // DWORD aligned buffer
    Frames ReferenceDecompressedFrames;
};

// Loads a .zst file and computes necessary information for testing, including frame offsets/sizes and reference
// decompressed frames.
ZstFileInfo LoadZstFile(std::filesystem::path zstFilePath)
{
    ZstFileInfo info{};
    info.FrameDataAligned = LoadFile(zstFilePath);
    info.TotalFramesSizeBytes = info.FrameDataAligned.size();
    info.FrameDataAligned.resize(DWORD_ALIGN(info.TotalFramesSizeBytes));
    info.FrameOffsetsAndSizes = ComputeOffsetsAndSizes(info.FrameDataAligned, info.TotalFramesSizeBytes);
    info.ReferenceDecompressedFrames = CpuDecompressFrames(info.FrameDataAligned, info.FrameOffsetsAndSizes);
    return info;
}

// Debug function to save decompressed frames to disk for manual inspection. This can be used when a test failure occurs
// to help diagnose issues.
static void SaveDecompressedFrames(
    std::filesystem::path outputDirectory,
    const Frames& uncompressedFrames,
    const OffsetsAndSizes& offsetsAndSizes)
{
    for (size_t frameIndex = 0; frameIndex < uncompressedFrames.size(); ++frameIndex)
    {
        auto& uncompressedFrame = uncompressedFrames[frameIndex];
        EXPECT_FALSE(uncompressedFrame.empty());
        std::stringstream ss;
        ss << outputDirectory.string() << "\\zstdgpu_test.frame_" << frameIndex;
        SaveFile(ss.str(), uncompressedFrame);
    }
}

// Compares the decompressed frames with reference decompressed frames to validate correctness of GPU decompression
// results.
void ValidateUncompressedFrames(ZstFileInfo& fileInfo, Frames& decompressedFrames)
{
    EXPECT_EQ(decompressedFrames.size(), fileInfo.ReferenceDecompressedFrames.size());
    for (size_t frameIndex = 0; frameIndex < decompressedFrames.size(); ++frameIndex)
    {
        auto& actual = decompressedFrames[frameIndex];
        auto& expected = fileInfo.ReferenceDecompressedFrames[frameIndex];
        EXPECT_TRUE(memcmp(actual.data(), expected.data(), expected.size()) == 0);
    }
}

// #define ENABLE_GPU_VALIDATION
struct ZstdDecompressionTests : public ::testing::TestWithParam<std::string>
{
    std::unique_ptr<ZstdDecompressionWork> m_gpuWork;

    ZstdDecompressionTests()
    {
#ifdef ENABLE_GPU_VALIDATION
        com_ptr<ID3D12Debug1> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.put()))))
        {
            debugController->EnableDebugLayer();
            debugController->SetEnableGPUBasedValidation(TRUE);
        }
        else
        {
            OutputDebugStringW(L"WARNING: D3D12 debug interface not available\n");
        }
#endif // ENABLE_GPU_VALIDATION
        auto adapter = FindDefaultAdapter();
        m_gpuWork = std::make_unique<ZstdDecompressionWork>(adapter.Device.get(), L"ZstdDecompressionCorrectnessTests");
    }

    Frames GpuDecompressFrames(std::vector<uint8_t>& compressedData, OffsetsAndSizes& offsetsAndSizes)
    {
        return m_gpuWork->Decompress(compressedData, offsetsAndSizes);
    }
};

// Loads a list of .zst content paths to use for generating unique tests
// that target a specific set of content. Each line in the file should be
// a path to a .zst file to test with.
static std::vector<std::string> LoadTestContentFileList(const std::string& path)
{
    std::ifstream in(path);
    std::vector<std::string> files;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty())
            files.push_back(line);
    }
    return files;
}

// Sanitizer: converts arbitrary strings into valid GoogleTest identifiers
inline std::string SanitizeTestName(const std::string& name)
{
    std::string result = name;

    // Replace invalid characters with '_'
    std::replace_if(
        result.begin(),
        result.end(),
        [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); },
        '_');

    // Ensure it doesn't start with a digit
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0])))
    {
        result = "_" + result;
    }

    // Avoid empty names
    if (result.empty())
    {
        result = "Empty";
    }

    return result;
}

std::string ParamNameGenerator(
    const testing::TestParamInfo<std::string>& info)
{
    return SanitizeTestName(info.param);
}

static bool file_exists_strict(const std::filesystem::path& p)
{
    return std::filesystem::exists(std::filesystem::absolute(p)) &&
           std::filesystem::is_regular_file(std::filesystem::absolute(p));
}

// Constructs a valid file path for content specifed in the test content list files.
// This assumes the content for internal and public tests are stored in known
// root folder locations:
// Example <root_drive_letter>:\DSTESTPC_CONTENT\internal
// Example <root_drive_letter>:\DSTESTPC_CONTENT\public
static std::filesystem::path MakeContentPath(const std::string& filename, bool isInternal)
{
    const char* drives[3] = {"C:\\", "D:\\", "E:\\"};
    for (auto& drive : drives)
    {
        std::filesystem::path contentPath = std::filesystem::path(drive);
        contentPath /= "DSTESTPC_CONTENT";
        contentPath /= isInternal ? "internal" : "public";
        contentPath /= filename;
        if (file_exists_strict(contentPath))
        {
            return contentPath;
        }
    }
    return filename; // Content was not found in one of the generated paths, so return just the filename and let the
                     // test handle the missing content case.
}

// Test suite for validating zstdgpu decompression using internal test content.
struct ZstdDecompressionInternalContentTests : public ZstdDecompressionTests
{
};

TEST_P(ZstdDecompressionInternalContentTests, CorrectnessTest)
{
    std::string filename = GetParam();
    std::filesystem::path contentPath = MakeContentPath(filename, true);
    if (!file_exists_strict(contentPath))
    {
        GTEST_SKIP() << "Test content `" << contentPath.string() << "` not found, skipping test.";
    }

    auto zstFileData = LoadZstFile(contentPath);
    auto decompressedFrames = GpuDecompressFrames(zstFileData.FrameDataAligned, zstFileData.FrameOffsetsAndSizes);
    ValidateUncompressedFrames(zstFileData, decompressedFrames);
}

INSTANTIATE_TEST_SUITE_P(
    ,
    ZstdDecompressionInternalContentTests,
    ::testing::ValuesIn(LoadTestContentFileList("internal_test_content.txt")),
    ParamNameGenerator);

// Test suite for validating zstdgpu decompression using public/shared test content.
struct ZstdDecompressionPublicContentTests : public ZstdDecompressionTests
{
};

TEST_P(ZstdDecompressionPublicContentTests, CorrectnessTest)
{
    std::string filename = GetParam();
    std::filesystem::path contentPath = MakeContentPath(filename, false);
    if (!file_exists_strict(contentPath))
    {
         GTEST_SKIP() << "Test content `" << contentPath.string() << "` not found, skipping test.";
    }

    auto zstFileData = LoadZstFile(contentPath);
    auto decompressedFrames = GpuDecompressFrames(zstFileData.FrameDataAligned, zstFileData.FrameOffsetsAndSizes);
    ValidateUncompressedFrames(zstFileData, decompressedFrames);
}

INSTANTIATE_TEST_SUITE_P(
    ,
    ZstdDecompressionPublicContentTests,
    ::testing::ValuesIn(LoadTestContentFileList("public_test_content.txt")),
    ParamNameGenerator);
