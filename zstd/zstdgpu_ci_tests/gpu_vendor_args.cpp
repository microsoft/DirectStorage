// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).

#include "gpu_vendor_args.h"

#include <charconv>
#include <system_error>

bool ParseGpuVendorId(std::string_view value, uint32_t& vendorId, std::string& error)
{
    vendorId = 0;
    error.clear();

    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    {
        value.remove_prefix(2);
    }
    if (value.empty())
    {
        error = "GPU vendor ID is empty";
        return false;
    }

    uint32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
    if (result.ec == std::errc::result_out_of_range)
    {
        error = "GPU vendor ID is outside the 32-bit range";
        return false;
    }
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    {
        error = "GPU vendor ID must contain only hexadecimal digits";
        return false;
    }
    if (parsed == 0)
    {
        error = "GPU vendor ID must be nonzero";
        return false;
    }

    vendorId = parsed;
    return true;
}

void AppendGpuVendorArgs(std::vector<std::string>& args, uint32_t vendorId)
{
    if (vendorId == 0)
    {
        return;
    }

    static constexpr char digits[] = "0123456789abcdef";
    char buffer[8]{};
    size_t index = sizeof(buffer);
    for (uint32_t remaining = vendorId; remaining != 0; remaining >>= 4)
    {
        buffer[--index] = digits[remaining & 0xF];
    }

    args.push_back("--gpu-ven-id");
    args.emplace_back(buffer + index, sizeof(buffer) - index);
}
