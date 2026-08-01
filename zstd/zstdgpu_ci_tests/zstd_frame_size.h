/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Reports zstd frame sizes by walking frame and block headers, without
// decompressing: the compressed (on-disk) size of the largest frame, and the
// total decompressed (content) size of the whole stream.
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

    // Returns the total decompressed (content) size of every zstd frame in
    // [src, src+srcSize), summed. Skippable frames contribute nothing. Returns 0
    // and sets *error (if non-null) on malformed/truncated input, or if any
    // frame omits its content size (so the decompressed size is unknowable
    // without decompressing).
    uint64_t GetTotalDecompressedSize(const uint8_t* src, size_t srcSize, std::string* error);

    // Reads the file at `path` and returns its total decompressed size.
    // Returns 0 and sets *error (if non-null) if the file cannot be read, is not
    // a valid zstd stream, or omits a frame content size.
    uint64_t GetTotalDecompressedSizeFromFile(const std::string& path, std::string* error);
}
