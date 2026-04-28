/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 *
 * Advanced Technology Group (ATG)
 * Author(s):   Pavel Martishevsky (pamartis@microsoft.com)
 */

#pragma once

struct zstdgpu_OffsetAndSize
{
    uint32_t offs;
    uint32_t size;
};

struct zstdgpu_FrameInfo
{
    uint64_t windowSize;
    uint64_t uncompSize;
    uint32_t dictionary;

    uint32_t rrBlockStart;
    uint32_t cmpBlockStart;

    uint32_t rrBlockBytesStart;
};
