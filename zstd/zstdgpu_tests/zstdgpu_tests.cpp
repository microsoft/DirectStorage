/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

#include "adapters.h"
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
const size_t MAX_FRAME_SIZE = 1024 * 1024 * 100; // 100MB max frame size for sanity check to avoid OOM
static std::vector<uint8_t> DecompressFrame(uint8_t* frameData, size_t frameDataSize)
{
    auto uncompressedSize = ZSTD_get_decompressed_size(frameData, frameDataSize);
    if (uncompressedSize == (size_t)-1)
    {
        // Uncompressed size is not available in the frame header, which may be the case for some frames. In this case,
        // we use a default size to pre-allocate a buffer for decompressed data.
        uncompressedSize = MAX_FRAME_SIZE;
    }
    std::vector<uint8_t> decompressedData(uncompressedSize);
    auto actualUncompressedSize = ZSTD_decompress(decompressedData.data(), uncompressedSize, frameData, frameDataSize);
    decompressedData.resize(actualUncompressedSize);

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
    offsetsAndSizes.UnCompressedFramesMemorySizeInBytes = 0;
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
        // Handle cases where the uncompressed size is not available from the frame header. In this case, we need to
        // perform a CPU decompression of the frame. This is pretty expensive and currently the
        // uncompressed data is thrown away after we get the size. In the future, we may want to optimize this by
        // caching the decompressed data for frames without uncompressed size.
        if (zstdFrameInfos[i].uncompSize == 0)
        {
            auto decompressedFrame = DecompressFrame(
                const_cast<uint8_t*>(alignedFrameData.data()) + offsetsAndSizes.InputOffsets[i].offs,
                offsetsAndSizes.InputOffsets[i].size);
            zstdFrameInfos[i].uncompSize = decompressedFrame.size();
        }

        offsetsAndSizes.OutputOffsets[i].offs = offs;
        offsetsAndSizes.OutputOffsets[i].size = (uint32_t)zstdFrameInfos[i].uncompSize;

        offs += offsetsAndSizes.OutputOffsets[i].size;
        offs = ZSTDGPU_ALIGN(offs, 256);

        vcnt += zstdFrameInfos[i].uncompSize != 0 ? 1 : 0;
    }
    offsetsAndSizes.UnCompressedFramesMemorySizeInBytes = offs;

    return offsetsAndSizes;
}

struct ZstFileInfo
{
    std::filesystem::path ZSTFilePath;
    size_t TotalFramesSizeBytes;
    OffsetsAndSizes FrameOffsetsAndSizes;
    std::vector<uint8_t> FrameDataAligned; // DWORD aligned buffer
    Frames ReferenceDecompressedFrames;
};

// Loads a .zst file and computes necessary information for testing.
ZstFileInfo LoadZstFile(std::filesystem::path zstFilePath)
{
    ZstFileInfo info{};
    info.ZSTFilePath = zstFilePath;
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

// Logs formatted failure messages for a test.  Calling this method will cause the current
// test to be reported as failed.
void GTEST_LOG_FAILURE_MESSAGE(_Printf_format_string_ const char* const fmt, ...)
{
    va_list vaArgs;
    va_start(vaArgs, fmt);
    va_list vaArgsCopy;
    va_copy(vaArgsCopy, vaArgs);
    int iLen = std::vsnprintf(NULL, 0, fmt, vaArgsCopy);
    if (iLen < 0)
    {
        va_end(vaArgsCopy);
        va_end(vaArgs);
        GTEST_NONFATAL_FAILURE_("Log message formatting error.");
        return;
    }
    va_end(vaArgsCopy);

    std::vector<char> zc(iLen + 1);
    std::vsnprintf(zc.data(), zc.size(), fmt, vaArgs);
    va_end(vaArgs);
    std::string item = std::string(zc.data(), iLen);

    GTEST_NONFATAL_FAILURE_(item.c_str());
}

bool ValidateUncompressedFrames(ZstFileInfo& fileInfo, Frames& decompressedFrames)
{
    bool validationPassed = true;
    // The total number of frames decompressed must match the expected number
    // of frames in the reference data.
    if (decompressedFrames.size() != fileInfo.ReferenceDecompressedFrames.size())
    {
        GTEST_LOG_FAILURE_MESSAGE(
            "Decompressed frame count mismatch found in '%s'. Expected: %zu, Actual: %zu",
            fileInfo.ZSTFilePath.string().c_str(),
            fileInfo.ReferenceDecompressedFrames.size(),
            decompressedFrames.size());
        return false; // avoid further processing which may cause out of bounds access
    }

    // The decompressed content must match the reference data.
    for (size_t frameIndex = 0; frameIndex < decompressedFrames.size(); ++frameIndex)
    {
        auto& actual = decompressedFrames[frameIndex];
        auto& expected = fileInfo.ReferenceDecompressedFrames[frameIndex];
        bool contentMatches = (memcmp(actual.data(), expected.data(), expected.size()) == 0);
        if (!contentMatches)
        {
            GTEST_LOG_FAILURE_MESSAGE(
                "Decompressed frame content mismatch found in '%s' at frame index %zu",
                fileInfo.ZSTFilePath.string().c_str(),
                frameIndex);
            validationPassed = false;
        }
    }

    return validationPassed;
}

// Every well-formed frame in the good corpus must report kzstdgpu_FrameStatus_Success (S_OK)
// in the per-frame status buffer written by the parse shader.
bool ValidateFrameStatusAllSuccess(ZstFileInfo& fileInfo, const std::vector<uint32_t>& frameStatus)
{
    bool validationPassed = true;
    if (frameStatus.size() != fileInfo.ReferenceDecompressedFrames.size())
    {
        GTEST_LOG_FAILURE_MESSAGE(
            "Frame status count mismatch found in '%s'. Expected: %zu, Actual: %zu",
            fileInfo.ZSTFilePath.string().c_str(),
            fileInfo.ReferenceDecompressedFrames.size(),
            frameStatus.size());
        return false;
    }

    for (size_t frameIndex = 0; frameIndex < frameStatus.size(); ++frameIndex)
    {
        if (frameStatus[frameIndex] != kzstdgpu_FrameStatus_Success)
        {
            GTEST_LOG_FAILURE_MESSAGE(
                "Frame status failure in '%s' at frame index %zu: 0x%08X (expected S_OK).",
                fileInfo.ZSTFilePath.string().c_str(),
                frameIndex,
                frameStatus[frameIndex]);
            validationPassed = false;
        }
    }

    return validationPassed;
}

static std::filesystem::path FindFirstContentPath(bool isInternal)
{
    const char* drives[3] = {"C:\\", "D:\\", "E:\\"};
    for (auto& drive : drives)
    {
        std::filesystem::path contentPath = std::filesystem::path(drive);
        contentPath /= "DSTESTPC_CONTENT";
        contentPath /= isInternal ? "internal" : "public";
        if (std::filesystem::exists(contentPath))
        {
            return contentPath;
        }
    }
    return {};
}

// File paths containing "skip" will be skipped in testing.
// This allows test iterations to be easily disabled by renaming
// the files or directories.
static bool SkipFile(const std::filesystem::path& filePath)
{
    std::string pathStr = filePath.string();
    std::transform(
        pathStr.begin(),
        pathStr.end(),
        pathStr.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return pathStr.find("skip") != std::string::npos;
}

// #define ENABLE_GPU_VALIDATION
struct ZstdDecompressionTests : public ::testing::Test
{
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
    }

    void ContentCorrectnessTest(bool isInternal)
    {
        auto contentPath = FindFirstContentPath(isInternal);
        if (contentPath.empty())
        {
            GTEST_LOG_FAILURE_MESSAGE(
                "Content folder not found. Searched drives C, D, and E for path 'DSTESTPC_CONTENT\\%s'.",
                isInternal ? "internal" : "public");
            return;
        }

        std::vector<std::filesystem::path> zstFiles;
        for (auto& entry : std::filesystem::recursive_directory_iterator(contentPath))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".zst" && !SkipFile(entry.path()))
            {
                zstFiles.push_back(entry.path());
            }
        }

        if (zstFiles.empty())
        {
            GTEST_LOG_FAILURE_MESSAGE(
                "No .zst files found in content folder '%s'.", contentPath.string().c_str());
            return;
        }

        size_t currentFileIndex = 0;
        size_t totalFiles = zstFiles.size();
        size_t filesFailed = 0;
        std::vector<std::filesystem::path> filePathsWithNoFrameData;

        for (auto& zstfile : zstFiles)
        {
            std::unique_ptr<ZstdDecompressionWork> gpuWork = std::make_unique<ZstdDecompressionWork>(
                FindDefaultAdapter().Device.get(),
                L"ZstdDecompressionCorrectnessTests");

            try
            {
                auto zstFileData = LoadZstFile(zstfile);
                GTEST_LOG_(INFO) << "Testing file " << (currentFileIndex + 1) << " of " << totalFiles << ": ("
                                 << zstFileData.ReferenceDecompressedFrames.size() << " frame(s)) '" << zstfile.string() << "', "
                                 << zstFileData.TotalFramesSizeBytes << " bytes(compressed), "
                                 << zstFileData.FrameOffsetsAndSizes.UnCompressedFramesMemorySizeInBytes << " bytes(uncompressed).";

                currentFileIndex++;
                if (!zstFileData.FrameOffsetsAndSizes.UnCompressedFramesMemorySizeInBytes)
                {
                    filePathsWithNoFrameData.push_back(zstfile);
                    continue;
                }

                std::vector<uint32_t> frameStatus;
                auto decompressedFrames =
                    gpuWork->Decompress(zstFileData.FrameDataAligned, zstFileData.FrameOffsetsAndSizes, &frameStatus);
                if (!ValidateUncompressedFrames(zstFileData, decompressedFrames))
                {
                    filesFailed++;
                }
                if (!ValidateFrameStatusAllSuccess(zstFileData, frameStatus))
                {
                    filesFailed++;
                }
            }
            catch (...)
            {
                HRESULT hr = gpuWork->GetDeviceRemovedReason();
                if (FAILED(hr))
                {
                    GTEST_LOG_FAILURE_MESSAGE(
                        "Device Removed detected while processing file '%s'",
                        zstfile.string().c_str());
                }
                else
                {
                    GTEST_LOG_FAILURE_MESSAGE(
                        "Exception occurred while processing file '%s'",
                        zstfile.string().c_str());
                }
                filesFailed++;

                GTEST_LOG_FAILURE_MESSAGE(
                    "SUMMARY: %zu files failed so far with TDR or exceptions, and %zu files detected with no frame data out of a total of %zu files.",
                    filesFailed,
                    filePathsWithNoFrameData.size(),
                    totalFiles);
            }
        }

        if (filesFailed > 0)
        {
            GTEST_LOG_FAILURE_MESSAGE(
                "Test completed with %zu files passed, %zu files failed with TDR or exceptions, and %zu files with no frame data out of a total of %zu files.",
                (totalFiles - (filesFailed + filePathsWithNoFrameData.size())),
                filesFailed,
                filePathsWithNoFrameData.size(),
                totalFiles);
        }
    }
};

TEST_F(ZstdDecompressionTests, InternalContentCorrectnessTest)
{
    ContentCorrectnessTest(true /* Use internal content folder location */);
}

TEST_F(ZstdDecompressionTests, PublicContentCorrectnessTest)
{
    ContentCorrectnessTest(false /* Use public content folder location */);
}

// Verifies the per-frame status buffer reports a rejection HRESULT for a frame whose header
// sets the reserved bit (a spec violation).  A real corpus frame is used with the reserved bit
// injected: this leaves the frame structurally valid so the decode pipeline still runs, while
// the header parse must flag it as rejected.  Only the status is asserted (not the output).
TEST_F(ZstdDecompressionTests, MalformedFrameReservedBitStatusTest)
{
    std::filesystem::path contentPath = FindFirstContentPath(true /* internal */);
    if (contentPath.empty())
    {
        contentPath = FindFirstContentPath(false /* public */);
    }
    if (contentPath.empty())
    {
        GTEST_SKIP() << "No content folder found for malformed-frame status test.";
    }

    // Find the first usable .zst file (one that actually contains frame data).
    ZstFileInfo zstFileData{};
    bool found = false;
    for (auto& entry : std::filesystem::recursive_directory_iterator(contentPath))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".zst" || SkipFile(entry.path()))
        {
            continue;
        }

        auto candidate = LoadZstFile(entry.path());
        if (candidate.FrameOffsetsAndSizes.UnCompressedFramesMemorySizeInBytes != 0 &&
            !candidate.FrameOffsetsAndSizes.InputOffsets.empty())
        {
            zstFileData = std::move(candidate);
            found = true;
            break;
        }
    }

    if (!found)
    {
        GTEST_SKIP() << "No .zst file with frame data found for malformed-frame status test.";
    }

    // Inject the reserved bit (bit 3 of the frame-header descriptor byte, which immediately
    // follows the 4-byte frame magic) into the first frame.  This does not change the frame's
    // block geometry, so the pipeline still decodes it, but the header parse must now reject it.
    const uint32_t frame0Offset = zstFileData.FrameOffsetsAndSizes.InputOffsets[0].offs;
    zstFileData.FrameDataAligned[frame0Offset + 4] |= 0x08;

    std::unique_ptr<ZstdDecompressionWork> gpuWork = std::make_unique<ZstdDecompressionWork>(
        FindDefaultAdapter().Device.get(),
        L"ZstdDecompressionMalformedFrameStatusTest");

    std::vector<uint32_t> frameStatus;
    gpuWork->Decompress(zstFileData.FrameDataAligned, zstFileData.FrameOffsetsAndSizes, &frameStatus);

    ASSERT_EQ(frameStatus.size(), zstFileData.FrameOffsetsAndSizes.InputOffsets.size());
    EXPECT_EQ(frameStatus[0], kzstdgpu_FrameStatus_ReservedBitSet)
        << "Frame with the reserved bit set should report kzstdgpu_FrameStatus_ReservedBitSet.";
}
