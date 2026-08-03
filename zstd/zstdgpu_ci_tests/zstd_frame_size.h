/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Reports frame sizes of a zstd stream -- both the largest frame's on-disk
// (compressed) size and the largest frame's decompressed (content) size -- by
// walking frame and block headers, without decompressing.
//
// Frame-walking logic derived from the reference zstd implementation
// (github.com/facebook/zstd, lib/decompress).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace zstdframe
{
    // Returns the on-disk byte size of the largest frame in [src, src+srcSize).
    // Returns 0 and sets *error (if non-null) on malformed or truncated input.
    uint64_t GetLargestFrameCompressedSize(const uint8_t* src, size_t srcSize, std::string* error);

    // Reads the file at `path` and returns the size of its largest frame.
    // Returns 0 and sets *error (if non-null) if the file cannot be read or is
    // not a valid zstd stream.
    uint64_t GetLargestFrameCompressedSizeFromFile(const std::string& path, std::string* error);

    // Returns the decompressed (content) size of the single largest zstd frame
    // in [src, src+srcSize). Returns 0 and sets *error (if non-null) on
    // malformed/truncated input, or when a frame omits its content size.
    uint64_t GetLargestFrameDecompressedSize(const uint8_t* src, size_t srcSize, std::string* error);

    // Reads the file at `path` and returns its largest single frame's
    // decompressed size. Returns 0 and sets *error (if non-null) on failure.
    uint64_t GetLargestFrameDecompressedSizeFromFile(const std::string& path, std::string* error);
}
