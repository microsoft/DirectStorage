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

bool ValidateDecompression(
    IDStorageFactory* factory,
    uint32_t stagingSizeMiB,
    const wchar_t* compressedFilename,
    const wchar_t* uncompressedFilename,
    DSTORAGE_COMPRESSION_FORMAT compressionFormat,
    Metadata const& metadata);