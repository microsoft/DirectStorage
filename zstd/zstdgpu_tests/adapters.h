//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include <d3d12.h>
#include <windows.h>
#include <winrt/base.h>

#include <optional>
#include <string>
#include <vector>

struct AdapterDesc
{
    std::string Name = "<unknown>";
    LUID InstanceLuid = {};
    uint64_t DedicatedAdapterMemory = 0;
    uint64_t DedicatedSystemMemory = 0;
    uint64_t SharedSystemMemory = 0;
    bool IsHardware = false;
    bool IsIntegrated = false;
    bool IsDetachable = false;
    winrt::com_ptr<ID3D12Device> Device;
};

std::vector<AdapterDesc> EnumerateAdapters();
std::optional<AdapterDesc> FindAdapterByName(std::string const& name);
std::optional<AdapterDesc> FindAdapterByLUID(LUID luid);
AdapterDesc FindDefaultAdapter();

inline uint64_t LUIDToUint64(const LUID& luid)
{
    return (static_cast<uint64_t>(luid.HighPart) << 32) | luid.LowPart;
}
