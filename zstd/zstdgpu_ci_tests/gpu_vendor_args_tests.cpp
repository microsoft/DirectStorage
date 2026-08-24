// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).

#include "gpu_vendor_args.h"

#include <gtest/gtest.h>

TEST(GpuVendorArgsTests, ParsesHexadecimalVendorId)
{
    uint32_t vendorId = 0;
    std::string error;
    EXPECT_TRUE(ParseGpuVendorId("10de", vendorId, error));
    EXPECT_EQ(vendorId, 0x10deu);
    EXPECT_TRUE(error.empty());

    EXPECT_TRUE(ParseGpuVendorId("0X10DE", vendorId, error));
    EXPECT_EQ(vendorId, 0x10deu);
}

TEST(GpuVendorArgsTests, RejectsMalformedZeroAndOverflowValues)
{
    for (const std::string value : {"", "0", "10de-tail", "100000000"})
    {
        uint32_t vendorId = 123;
        std::string error;
        EXPECT_FALSE(ParseGpuVendorId(value, vendorId, error)) << value;
        EXPECT_EQ(vendorId, 0u) << value;
        EXPECT_FALSE(error.empty()) << value;
    }
}

TEST(GpuVendorArgsTests, OmitsUnsetVendorId)
{
    std::vector<std::string> args{"--chk-gpu"};
    AppendGpuVendorArgs(args, 0);
    EXPECT_EQ(args, std::vector<std::string>({"--chk-gpu"}));
}

TEST(GpuVendorArgsTests, AppendsNormalizedVendorId)
{
    std::vector<std::string> args{"--chk-gpu"};
    AppendGpuVendorArgs(args, 0x10de);
    EXPECT_EQ(args, std::vector<std::string>({"--chk-gpu", "--gpu-ven-id", "10de"}));
}
