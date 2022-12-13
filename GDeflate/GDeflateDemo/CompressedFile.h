/*
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstring>

struct CompressedFileHeader
{
    char Id[8]; // "GDEFLATE"
    size_t UncompressedSize = 0;
};

static void InitializeHeader(CompressedFileHeader* header, size_t uncompressedSize)
{
    header->Id[0] = 'G';
    header->Id[1] = 'D';
    header->Id[2] = 'E';
    header->Id[3] = 'F';
    header->Id[4] = 'L';
    header->Id[5] = 'A';
    header->Id[6] = 'T';
    header->Id[7] = 'E';
    header->UncompressedSize = uncompressedSize;
}

static bool IsValidHeader(CompressedFileHeader* header)
{
    CompressedFileHeader expected{};
    InitializeHeader(&expected, header->UncompressedSize);
    return (memcmp(&expected, header, sizeof(expected)) == 0);
}

#ifdef WIN32

static std::filesystem::path GetModulePath()
{
    int32_t cchPath = MAX_PATH;
    std::wstring path;
    while (true)
    {
        path.resize(cchPath);
        ::SetLastError(0);
        cchPath = GetModuleFileNameW(nullptr, path.data(), cchPath);
        if (cchPath == 0)
        {
            winrt::check_win32(::GetLastError());
            break;
        }
        else if (::GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            cchPath *= 2;
        }
        else
        {
            break;
        }
    }

    return std::filesystem::path(path.data()).parent_path();
}

#endif