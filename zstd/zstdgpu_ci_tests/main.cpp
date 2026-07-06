/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Entry point for the Zstd GPU CI tests. This is a thin GTest wrapper
// that shells out to zstdgpu_demo.exe to validate Zstd GPU decompression shaders.
//
//   - parses custom CLI flags (--content-path, --demo-path, etc.), resolves the
// demo executable, then hands off to GTest which runs parameterized tests defined
// in zstdgpu_ci_tests.cpp. Each test spawns the demo as a child process.
//
// If no .zst content files are found, zero tests are instantiated and the test
// binary exits 0 (success). If the demo exe is missing, tests are skipped (not failed).
//
// This file also implements the TestConfig singleton and file discovery helpers
// declared in zstdgpu_ci_tests.h.

#include "zstdgpu_ci_tests.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

// TestConfig singleton
// Implementation of the singleton declared in zstdgpu_ci_tests.h.

static TestConfig g_testConfig;

const TestConfig& GetTestConfig()
{
    return g_testConfig;
}

void SetTestConfig(TestConfig config)
{
    g_testConfig = std::move(config);
}

// File discovery

std::vector<std::string> DiscoverZstFiles(const std::string& contentPath)
{
    std::vector<std::string> files;

    if (contentPath.empty() || !std::filesystem::exists(contentPath) || !std::filesystem::is_directory(contentPath))
    {
        return files;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(contentPath))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".zst")
        {
            files.push_back(entry.path().string());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

// CLI and entry point

// QOL for diagnostics. For running manually
// Activate with --help-ci to avoid conflicting with GTest's own --help output.
static void PrintUsage(const char* exe)
{
    std::cout << "Usage: " << exe << " [gtest_options] [options]\n"
              << "\n"
              << "Options:\n"
              << "  --content-path <dir>    Directory containing .zst test files\n"
              << "  --demo-path <path>      Path to zstdgpu_demo.exe\n"
              << "  --log-dir <dir>         Directory for logs and CSV output\n"
              << "  --log-file <path>       Consolidated text log file\n"
              << "  --run-count <N>         Perf test iteration count (default: 40)\n"
              << "  --timeout <seconds>     Per-test process timeout (default: no timeout)\n"
              << std::endl;
}

int main(int argc, char** argv)
{
    // Parse custom flags before handing off to GTest. GTest's InitGoogleTest()
    // is called later and will consume its own flags (e.g. --gtest_filter).
    TestConfig config;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--content-path") == 0 && i + 1 < argc)
        {
            config.contentPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--demo-path") == 0 && i + 1 < argc)
        {
            config.demoPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc)
        {
            config.logDir = argv[++i];
        }
        else if (std::strcmp(argv[i], "--log-file") == 0 && i + 1 < argc)
        {
            config.logFile = argv[++i];
        }
        else if (std::strcmp(argv[i], "--run-count") == 0 && i + 1 < argc)
        {
            config.runCount = std::atoi(argv[++i]);
            if (config.runCount <= 0)
                config.runCount = 40;
        }
        else if (std::strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
        {
            config.timeoutSeconds = std::atoi(argv[++i]);
            if (config.timeoutSeconds < 0)
                config.timeoutSeconds = 0;
        }
        else if (std::strcmp(argv[i], "--help-ci") == 0)
        {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    if (config.demoPath.empty())
    {
        std::cerr << "Warning: --demo-path not set. Tests will skip.\n";
    }

    if (config.contentPath.empty())
    {
        std::cerr << "Warning: --content-path not set. Zero tests will be discovered "
                     "(gtest will print 'This test program does NOT link in any test case').\n";
    }
    else if (!std::filesystem::exists(config.contentPath))
    {
        std::cerr << "Warning: --content-path '" << config.contentPath
                  << "' does not exist. Zero tests will be discovered.\n";
    }
    else if (!std::filesystem::is_directory(config.contentPath))
    {
        std::cerr << "Warning: --content-path '" << config.contentPath
                  << "' is not a directory. Zero tests will be discovered.\n";
    }
    else
    {
        const size_t fileCount = DiscoverZstFiles(config.contentPath).size();
        if (fileCount == 0)
        {
            std::cerr << "Warning: --content-path '" << config.contentPath
                      << "' contains no .zst files. Zero tests will be discovered.\n";
        }
        else
        {
            std::cout << "Discovered " << fileCount << " .zst file(s) at '"
                      << config.contentPath << "'.\n";
        }
    }

    // Default log dir to current directory.
    if (config.logDir.empty())
    {
        config.logDir = std::filesystem::current_path().string();
    }

    // Ensure log directory exists.
    if (!std::filesystem::exists(config.logDir))
    {
        std::filesystem::create_directories(config.logDir);
    }

    SetTestConfig(std::move(config));

    testing::InitGoogleTest(&argc, argv);
    testing::GTEST_FLAG(catch_exceptions) = false;
    return RUN_ALL_TESTS();
}
