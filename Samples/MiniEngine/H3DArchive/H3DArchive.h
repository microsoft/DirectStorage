//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include <stdint.h>

#include "../Model/ModelH3D.h"

enum H3DCompression
{
    None,
    Zlib
};

// Zlib compressed assets are split into blocks of this size.  Each block is
// compressed separately. The compressed data starts with an array of uint32_ts
// describing the offset to each block.
constexpr size_t ZLIB_BLOCK_SIZE = 256 * 1024;

constexpr inline uint64_t GetH3DZlibBlockCount(uint64_t uncompressedSize)
{
    return (uncompressedSize + ZLIB_BLOCK_SIZE - 1) / ZLIB_BLOCK_SIZE;
}

constexpr inline uint32_t GetH3DMagicNumber()
{
    return 'H' << 000 | '3' << 010 | 'D' << 020 | 'A' << 030;
}

struct H3DArchiveHeader
{
    uint32_t magic;
    H3DCompression compression;

    // These offsets are relative to BOF
    uint64_t cpuDataOffset;
    uint64_t uncompressedCpuDataSize;
    uint64_t compressedCpuDataSize;
    uint64_t geometryDataOffset;
    uint64_t uncompressedGeometryDataSize;
    uint64_t compressedGeometryDataSize;
    uint64_t texturesOffset;

    // These offsets are relative to cpuDataOffset
    // ModelH3D::Header is at offset 0
    uint64_t meshesOffset;
    uint64_t materialsOffset;
    uint64_t archivedTexturesOffset;
    uint64_t archivedTexturesCount;
};

struct H3DArchivedTexture
{
	char path[ModelH3D::Material::maxTexPath];
	uint64_t offset; // relative to header's texturesOffset
    uint64_t uncompressedSize;
    uint64_t compressedSize;
	D3D12_RESOURCE_DESC desc;
};

