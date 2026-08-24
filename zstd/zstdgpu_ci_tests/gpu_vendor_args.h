// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Parses a nonzero PCI vendor ID written in hexadecimal. An optional 0x prefix
// is accepted. On failure, returns false and describes the invalid value.
bool ParseGpuVendorId(std::string_view value, uint32_t& vendorId, std::string& error);

// Appends the zstdgpu_demo adapter selector when an explicit vendor was set.
// A zero ID means "use the demo's normal adapter selection".
void AppendGpuVendorArgs(std::vector<std::string>& args, uint32_t vendorId);
