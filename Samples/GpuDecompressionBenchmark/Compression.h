//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
#pragma once

#include "Metadata.h"

#include <dstorage.h>
#include <winrt/base.h>

#include <filesystem>
#include <vector>

winrt::com_ptr<IDStorageCompressionCodec> GetCodec(DSTORAGE_COMPRESSION_FORMAT format);

Metadata Compress(
    DSTORAGE_COMPRESSION_FORMAT format,
    const wchar_t* originalFilename,
    std::vector<uint8_t>& compressedData,
    uint32_t chunkSizeBytes);

Metadata CompressToArchive(
    DSTORAGE_COMPRESSION_FORMAT format,
    std::vector<std::filesystem::path>& files,
    const wchar_t* archiveFilename,
    uint32_t chunkSizeBytes,
    bool bValidate);