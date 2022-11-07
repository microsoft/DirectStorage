//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include <dstorage.h>
#include <wrl/client.h>

void InitializeDStorage(bool disableGpuDecompression);
void ShutdownDStorage();

extern Microsoft::WRL::ComPtr<IDStorageFactory> g_dsFactory;
extern Microsoft::WRL::ComPtr<IDStorageQueue1> g_dsSystemMemoryQueue;
extern Microsoft::WRL::ComPtr<IDStorageQueue1> g_dsGpuQueue;

//
// ZLib is supported via custom compression.  The CUSTOM_COMPRESSION_FORMAT_ZLIB
// constant provides a more meaningful name that DSTORAGE_CUSTOM_COMPRESSION_0.
//

constexpr DSTORAGE_COMPRESSION_FORMAT CUSTOM_COMPRESSION_FORMAT_ZLIB = DSTORAGE_CUSTOM_COMPRESSION_0;

