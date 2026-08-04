/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Measures the compressed size of zstd frames by parsing frame and block
// headers only, without decompressing.
//
// Derived from the reference zstd implementation (github.com/facebook/zstd,
// lib/decompress/zstd_decompress.c and zstd_decompress_block.c).

#include "zstd_frame_size.h"

#include <cstdint>
#include <fstream>
#include <vector>

namespace zstdframe
{
    namespace
    {
        // ---- format constants -------------------------------------------------

        constexpr uint32_t kMagicNumber = 0xFD2FB528U;
        constexpr uint32_t kMagicSkippableStart = 0x184D2A50U;
        constexpr uint32_t kMagicSkippableMask = 0xFFFFFFF0U;

        constexpr size_t kFrameIdSize = 4;          // magic number size
        constexpr size_t kSkippableHeaderSize = 8;  // magic + 4-byte length
        constexpr size_t kBlockHeaderSize = 3;
        constexpr size_t kStartingInputLength = 5;  // magic + first header byte

        constexpr uint32_t kBlockSizeLogMax = 17;
        constexpr uint32_t kBlockSizeMax = 1U << kBlockSizeLogMax;   // 128 KB
        constexpr uint32_t kWindowLogAbsoluteMin = 10;
        constexpr uint32_t kWindowLogMax = 31;                      // 64-bit build

        constexpr uint64_t kContentSizeUnknown = 0ULL - 1;

        // Dictionary-ID and frame-content-size field-size lookup tables.
        constexpr size_t kDidFieldSize[4] = { 0, 1, 2, 4 };
        constexpr size_t kFcsFieldSize[4] = { 0, 2, 4, 8 };

        // ---- error signalling -------------------------------------------------
        // Errors are encoded as size_t values in the top 128 of the range.

        constexpr size_t kError = static_cast<size_t>(-1);
        inline bool IsError(size_t code) { return code > static_cast<size_t>(-128); }

        // ---- little-endian readers --------------------------------------------

        inline uint32_t ReadLE16(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
        }
        inline uint32_t ReadLE24(const uint8_t* p)
        {
            return ReadLE16(p) | (static_cast<uint32_t>(p[2]) << 16);
        }
        inline uint32_t ReadLE32(const uint8_t* p)
        {
            return ReadLE16(p) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }
        inline uint64_t ReadLE64(const uint8_t* p)
        {
            return static_cast<uint64_t>(ReadLE32(p)) | (static_cast<uint64_t>(ReadLE32(p + 4)) << 32);
        }

        // ---- frame header ------------------------------------------------------

        enum class FrameType { Zstd, Skippable };

        struct FrameHeader
        {
            uint64_t frameContentSize = kContentSizeUnknown;
            uint64_t windowSize = 0;
            uint32_t blockSizeMax = 0;
            uint32_t headerSize = 0;
            uint32_t dictID = 0;
            uint32_t checksumFlag = 0;
            FrameType frameType = FrameType::Zstd;
        };

        // Returns the frame header length in bytes, or an error code (IsError).
        size_t FrameHeaderSize(const uint8_t* src, size_t srcSize)
        {
            constexpr size_t minInputSize = kStartingInputLength;
            if (srcSize < minInputSize) return kError;

            const uint8_t fhd = src[minInputSize - 1];
            const uint32_t dictID = fhd & 3;
            const uint32_t singleSegment = (fhd >> 5) & 1;
            const uint32_t fcsId = fhd >> 6;
            return minInputSize + (singleSegment ? 0 : 1)
                + kDidFieldSize[dictID] + kFcsFieldSize[fcsId]
                + ((singleSegment && !fcsId) ? 1 : 0);
        }

        // Parses a frame header (regular or skippable). Returns 0 on success and
        // fills *zfh, a positive value if more input is needed, or an error code
        // (IsError).
        size_t GetFrameHeader(FrameHeader* zfh, const uint8_t* src, size_t srcSize)
        {
            const uint8_t* ip = src;
            constexpr size_t minInputSize = kStartingInputLength;

            if (srcSize < minInputSize) return minInputSize;

            *zfh = FrameHeader{};

            if (ReadLE32(src) != kMagicNumber)
            {
                if ((ReadLE32(src) & kMagicSkippableMask) == kMagicSkippableStart)
                {
                    if (srcSize < kSkippableHeaderSize) return kSkippableHeaderSize;
                    zfh->frameType = FrameType::Skippable;
                    zfh->dictID = ReadLE32(src) - kMagicSkippableStart;
                    zfh->headerSize = static_cast<uint32_t>(kSkippableHeaderSize);
                    zfh->frameContentSize = ReadLE32(src + kFrameIdSize);
                    return 0;
                }
                return kError;  // unknown prefix
            }

            {
                const size_t fhsize = FrameHeaderSize(src, srcSize);
                if (IsError(fhsize)) return fhsize;
                if (srcSize < fhsize) return fhsize;
                zfh->headerSize = static_cast<uint32_t>(fhsize);
            }

            {
                const uint8_t fhdByte = ip[minInputSize - 1];
                size_t pos = minInputSize;
                const uint32_t dictIDSizeCode = fhdByte & 3;
                const uint32_t checksumFlag = (fhdByte >> 2) & 1;
                const uint32_t singleSegment = (fhdByte >> 5) & 1;
                const uint32_t fcsID = fhdByte >> 6;
                uint64_t windowSize = 0;
                uint32_t dictID = 0;
                uint64_t frameContentSize = kContentSizeUnknown;

                if ((fhdByte & 0x08) != 0) return kError;  // reserved bits must be zero

                if (!singleSegment)
                {
                    const uint8_t wlByte = ip[pos++];
                    const uint32_t windowLog = (wlByte >> 3) + kWindowLogAbsoluteMin;
                    if (windowLog > kWindowLogMax) return kError;
                    windowSize = (1ULL << windowLog);
                    windowSize += (windowSize >> 3) * (wlByte & 7);
                }
                switch (dictIDSizeCode)
                {
                    case 0: break;
                    case 1: dictID = ip[pos]; pos += 1; break;
                    case 2: dictID = ReadLE16(ip + pos); pos += 2; break;
                    case 3: dictID = ReadLE32(ip + pos); pos += 4; break;
                }
                switch (fcsID)
                {
                    case 0: if (singleSegment) frameContentSize = ip[pos]; break;
                    case 1: frameContentSize = ReadLE16(ip + pos) + 256; break;
                    case 2: frameContentSize = ReadLE32(ip + pos); break;
                    case 3: frameContentSize = ReadLE64(ip + pos); break;
                }
                if (singleSegment) windowSize = frameContentSize;

                zfh->frameType = FrameType::Zstd;
                zfh->frameContentSize = frameContentSize;
                zfh->windowSize = windowSize;
                zfh->blockSizeMax = static_cast<uint32_t>(windowSize < kBlockSizeMax ? windowSize : kBlockSizeMax);
                zfh->dictID = dictID;
                zfh->checksumFlag = checksumFlag;
            }
            return 0;
        }

        // ---- block header ------------------------------------------------------

        enum BlockType { bt_raw = 0, bt_rle = 1, bt_compressed = 2, bt_reserved = 3 };

        struct BlockProperties
        {
            BlockType blockType = bt_raw;
            uint32_t lastBlock = 0;
            uint32_t origSize = 0;
        };

        // Returns the on-disk size of one block from its 3-byte header. RLE
        // blocks occupy a single byte regardless of origSize.
        size_t GetCBlockSize(const uint8_t* src, size_t srcSize, BlockProperties* bp)
        {
            if (srcSize < kBlockHeaderSize) return kError;

            const uint32_t cBlockHeader = ReadLE24(src);
            const uint32_t cSize = cBlockHeader >> 3;
            bp->lastBlock = cBlockHeader & 1;
            bp->blockType = static_cast<BlockType>((cBlockHeader >> 1) & 3);
            bp->origSize = cSize;
            if (bp->blockType == bt_rle) return 1;
            if (bp->blockType == bt_reserved) return kError;
            return cSize;
        }

        // ---- skippable frame ---------------------------------------------------

        // Returns the total on-disk size of a skippable frame.
        size_t ReadSkippableFrameSize(const uint8_t* src, size_t srcSize)
        {
            if (srcSize < kSkippableHeaderSize) return kError;

            const uint32_t sizeU32 = ReadLE32(src + kFrameIdSize);
            if (static_cast<uint32_t>(sizeU32 + kSkippableHeaderSize) < sizeU32) return kError;  // overflow
            const size_t skippableSize = kSkippableHeaderSize + sizeU32;
            if (skippableSize > srcSize) return kError;
            return skippableSize;
        }

        // ---- single-frame compressed size -------------------------------------

        // Returns the on-disk size of one frame (regular or skippable) at src, or
        // an error code (IsError).
        size_t FindFrameCompressedSize(const uint8_t* src, size_t srcSize)
        {
            if (srcSize >= kSkippableHeaderSize
                && (ReadLE32(src) & kMagicSkippableMask) == kMagicSkippableStart)
            {
                return ReadSkippableFrameSize(src, srcSize);
            }

            const uint8_t* ip = src;
            const uint8_t* const ipstart = ip;
            size_t remainingSize = srcSize;
            FrameHeader zfh;

            {
                const size_t ret = GetFrameHeader(&zfh, src, srcSize);
                if (IsError(ret)) return ret;
                if (ret > 0) return kError;  // header incomplete
            }

            ip += zfh.headerSize;
            remainingSize -= zfh.headerSize;

            while (true)
            {
                BlockProperties bp;
                const size_t cBlockSize = GetCBlockSize(ip, remainingSize, &bp);
                if (IsError(cBlockSize)) return cBlockSize;

                if (kBlockHeaderSize + cBlockSize > remainingSize) return kError;

                ip += kBlockHeaderSize + cBlockSize;
                remainingSize -= kBlockHeaderSize + cBlockSize;

                if (bp.lastBlock) break;
            }

            if (zfh.checksumFlag)
            {
                if (remainingSize < 4) return kError;
                ip += 4;  // content checksum
            }

            return static_cast<size_t>(ip - ipstart);
        }
    }  // namespace

    uint64_t GetLargestFrameCompressedSize(const uint8_t* src, size_t srcSize, std::string* error)
    {
        auto fail = [&](const char* msg) -> uint64_t
        {
            if (error) *error = msg;
            return 0;
        };

        if (src == nullptr || srcSize == 0)
            return fail("empty input");

        uint64_t largest = 0;
        size_t offset = 0;

        // Walk each frame, tracking the largest on-disk frame size.
        while (offset < srcSize)
        {
            const size_t remaining = srcSize - offset;
            if (remaining < kStartingInputLength)
                return fail("trailing bytes are too short to form a valid zstd frame");

            const size_t frameSize = FindFrameCompressedSize(src + offset, remaining);
            if (IsError(frameSize))
                return fail("invalid or truncated zstd frame while scanning the stream");
            if (frameSize == 0 || frameSize > remaining)
                return fail("frame size out of range while scanning the stream");

            if (frameSize > largest)
                largest = frameSize;

            offset += frameSize;
        }

        if (largest == 0)
            return fail("no zstd frames found");

        if (error) error->clear();
        return largest;
    }

    uint64_t GetLargestFrameCompressedSizeFromFile(const std::string& path, std::string* error)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            if (error) *error = "could not open file '" + path + "'";
            return 0;
        }

        const std::streamoff size = file.tellg();
        if (size <= 0)
        {
            if (error) *error = "file '" + path + "' is empty or unreadable";
            return 0;
        }

        // Reject sizes that don't fit in size_t.
        if (static_cast<uint64_t>(size) > static_cast<uint64_t>(SIZE_MAX))
        {
            if (error) *error = "file '" + path + "' is too large to read";
            return 0;
        }

        const size_t byteCount = static_cast<size_t>(size);
        std::vector<uint8_t> buffer(byteCount);
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(byteCount)))
        {
            if (error) *error = "failed to read file '" + path + "'";
            return 0;
        }

        return GetLargestFrameCompressedSize(buffer.data(), buffer.size(), error);
    }

    uint64_t GetLargestFrameDecompressedSize(const uint8_t* src, size_t srcSize, std::string* error)
    {
        auto fail = [&](const char* msg) -> uint64_t
        {
            if (error) *error = msg;
            return 0;
        };

        if (src == nullptr || srcSize == 0)
            return fail("empty input");

        uint64_t largest = 0;
        size_t offset = 0;

        // Walk each frame, tracking the single largest frame's decompressed
        // (content) size. The size comes from frame headers.
        while (offset < srcSize)
        {
            const size_t remaining = srcSize - offset;
            if (remaining < kStartingInputLength)
                return fail("trailing bytes are too short to form a valid zstd frame");

            FrameHeader zfh;
            {
                const size_t ret = GetFrameHeader(&zfh, src + offset, remaining);
                if (IsError(ret))
                    return fail("invalid or truncated zstd frame header while scanning the stream");
                if (ret > 0)
                    return fail("incomplete zstd frame header while scanning the stream");
            }

            const size_t frameSize = FindFrameCompressedSize(src + offset, remaining);
            if (IsError(frameSize))
                return fail("invalid or truncated zstd frame while scanning the stream");
            if (frameSize == 0 || frameSize > remaining)
                return fail("frame size out of range while scanning the stream");

            if (zfh.frameType == FrameType::Zstd)
            {
                if (zfh.frameContentSize == kContentSizeUnknown)
                    return fail("zstd frame omits its content size; decompressed size is unknowable");
                if (zfh.frameContentSize > largest)
                    largest = zfh.frameContentSize;
            }

            offset += frameSize;
        }

        if (largest == 0)
            return fail("no zstd frames with a known content size found");

        if (error) error->clear();
        return largest;
    }

    uint64_t GetLargestFrameDecompressedSizeFromFile(const std::string& path, std::string* error)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            if (error) *error = "could not open file '" + path + "'";
            return 0;
        }

        const std::streamoff size = file.tellg();
        if (size <= 0)
        {
            if (error) *error = "file '" + path + "' is empty or unreadable";
            return 0;
        }

        // Reject sizes that don't fit in size_t.
        if (static_cast<uint64_t>(size) > static_cast<uint64_t>(SIZE_MAX))
        {
            if (error) *error = "file '" + path + "' is too large to read";
            return 0;
        }

        const size_t byteCount = static_cast<size_t>(size);
        std::vector<uint8_t> buffer(byteCount);
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(byteCount)))
        {
            if (error) *error = "failed to read file '" + path + "'";
            return 0;
        }

        return GetLargestFrameDecompressedSize(buffer.data(), buffer.size(), error);
    }
}  // namespace zstdframe
