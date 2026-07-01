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
//   - parses custom CLI flags (--content-path, --demo-path, etc.), validates
// them (hard failure with a non-zero exit on any misconfiguration — never
// silently skip), discovers .zst files under the content path exactly once,
// then hands off to GTest which runs parameterized tests defined in
// zstdgpu_ci_tests.cpp. Each test spawns the demo as a child process.
//
// If the content path is missing, unreadable, or contains no .zst files, or if
// the demo executable is missing, the process exits non-zero before any test
// runs. This intentionally prevents a "green" run with zero coverage.
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
#include <system_error>

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
//
// Uses the non-throwing overloads of recursive_directory_iterator so a single
// unreadable subdirectory anywhere in the tree does not abort the entire walk
// (which would take down the process before any test runs — see review
// feedback on the throwing overload). Per-entry errors are logged as warnings
// and the walk continues.

std::vector<std::string> DiscoverZstFiles(const std::string& contentPath)
{
    std::vector<std::string> files;

    if (contentPath.empty() || !std::filesystem::exists(contentPath) || !std::filesystem::is_directory(contentPath))
    {
        return files;
    }

    std::error_code ec;
    auto iter = std::filesystem::recursive_directory_iterator(
        contentPath,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    if (ec)
    {
        std::cerr << "Warning: could not open '" << contentPath << "' for scanning: "
                  << ec.message() << "\n";
        return files;
    }

    auto endIter = std::filesystem::recursive_directory_iterator();
    while (iter != endIter)
    {
        std::error_code entryEc;
        if (iter->is_regular_file(entryEc) && !entryEc)
        {
            if (iter->path().extension() == ".zst")
            {
                files.push_back(iter->path().string());
            }
        }
        else if (entryEc)
        {
            std::cerr << "Warning: skipping '" << iter->path().string() << "': "
                      << entryEc.message() << "\n";
        }
        iter.increment(ec);
        if (ec)
        {
            std::cerr << "Warning: recursive walk aborted early after '"
                      << iter->path().string() << "': " << ec.message() << "\n";
            break;
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
              << "  --content-path <dir>    Directory containing .zst test files (required)\n"
              << "  --demo-path <path>      Path to zstdgpu_demo.exe (required)\n"
              << "  --log-dir <dir>         Directory for logs and CSV output\n"
              << "  --log-file <path>       Consolidated text log file\n"
              << "  --run-count <N>         Perf test iteration count (default: 40)\n"
              << "  --timeout <seconds>     Per-test process timeout (default: no timeout)\n"
              << std::endl;
}

// Prints an error prefixed with "Error:" to stderr and returns the exit code so
// main() can 'return Fail(...)' in a single expression. Keeps validation blocks
// short and consistent.
static int Fail(const std::string& msg)
{
    std::cerr << "Error: " << msg << std::endl;
    return 1;
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

    // Fail-loud validation. Any of these misconfigurations means the run cannot
    // produce meaningful test coverage, so we exit non-zero before any test
    // instantiates. Silent skipping (previously done via warnings + GTEST_SKIP)
    // would let broken pipelines pass green with zero coverage.
    if (config.demoPath.empty())
        return Fail("--demo-path is required.");
    if (!std::filesystem::exists(config.demoPath))
        return Fail("--demo-path '" + config.demoPath + "' does not exist.");
    if (!std::filesystem::is_regular_file(config.demoPath))
        return Fail("--demo-path '" + config.demoPath + "' is not a regular file.");

    if (config.contentPath.empty())
        return Fail("--content-path is required.");
    if (!std::filesystem::exists(config.contentPath))
        return Fail("--content-path '" + config.contentPath + "' does not exist.");
    if (!std::filesystem::is_directory(config.contentPath))
        return Fail("--content-path '" + config.contentPath + "' is not a directory.");

    // Discover exactly once. Cached on TestConfig for GetTestFiles() to reuse
    // when instantiating the parameterized fixture — avoids walking the tree
    // twice.
    config.discoveredFiles = DiscoverZstFiles(config.contentPath);
    if (config.discoveredFiles.empty())
        return Fail("--content-path '" + config.contentPath + "' contains no .zst files.");

    std::cout << "Discovered " << config.discoveredFiles.size() << " .zst file(s) at '"
              << config.contentPath << "'.\n";

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
