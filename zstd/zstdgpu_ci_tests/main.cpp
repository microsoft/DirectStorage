/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Entry point for the Zstd GPU CI tests. Thin GTest wrapper that shells out
// to zstdgpu_demo.exe to validate Zstd GPU decompression shaders.
//
// main() parses custom CLI flags (--content-path, --demo-path, etc.),
// validates them (hard failure with non-zero exit on any misconfiguration —
// never silently skip), discovers .zst files under the content path exactly
// once, then hands off to GTest which runs the parameterized suite defined
// in zstdgpu_ci_tests.cpp. Each test spawns the demo as a child process.
//
// If the content path is missing, unreadable, or contains no .zst files, or
// if the demo executable is missing, the process exits non-zero before any
// test runs. This intentionally prevents a "green" run with zero coverage
//
// This file also owns the g_testConfig storage and the file discovery
// implementation declared in zstdgpu_ci_tests.h.

#include "zstdgpu_ci_tests.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

// Global config, declared extern in zstdgpu_ci_tests.h. Set once in main(),
// read from test bodies.
TestConfig g_testConfig;

// File discovery
//
// Uses the non-throwing overloads of recursive_directory_iterator so a single
// unreadable subdirectory anywhere in the tree does not abort the entire walk
// (the throwing overloads would terminate the process before any test runs).
// Per-entry errors are logged as warnings and the walk continues.

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

// Prints CLI usage for manual and diagnostic runs. Activated with --help-ci to
// avoid conflicting with GTest's own --help output.
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
              << "  --perf-min-mb <N>       Minimum .zst file size (MB) required for perf tests (default: 4).\n"
              << "                          Smaller files skip perf; individually-compressed textures are not representative of throughput.\n"
              << "  --gbv-sample-count <N>  Number of files the GBV scenarios run on, sampled by an even stride\n"
              << "                          across the sorted corpus (default: 10). A value <= 0 runs GBV on all files.\n"
              << "  --gpu-name <name>       Adapter name of this machine. Consumed only by the manifest's\n"
              << "                          scenario_skips; if omitted, no scenario is skipped by GPU name.\n"
              << "  --gbv-max-mb <N>        Max size of file to use for GBV tests in MB (default: 1)\n"
              << "  --adversarial-manifest <path>   Optional JSON manifest of known-adversarial fuzz files.\n"
              << "                                  If NOT specified, the wrapper auto-discovers the manifest at\n"
              << "                                  <content-path>/adversarial_manifest.json. If found (either way),\n"
              << "                                  matching files change the assertion criteria:\n"
              << "                                    - perf tests skip these files entirely\n"
              << "                                    - correctness tests expect a specific exit code + stderr signature\n"
              << "                                  If no manifest is found (no flag AND no file in content-path), the\n"
              << "                                  wrapper falls back to legacy behavior: every file expected to succeed.\n"
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

// Parse custom flags out of argv before we hand argv to GTest. GTest's own
// InitGoogleTest() runs later and will consume its own flags (e.g.
// --gtest_filter). Returns true on success; on --help-ci, prints usage and
// sets shouldExit=true so main() can return 0 cleanly.
static bool ParseArgs(int argc, char** argv, TestConfig& config, bool& shouldExit)
{
    shouldExit = false;
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
        else if (std::strcmp(argv[i], "--adversarial-manifest") == 0 && i + 1 < argc)
        {
            config.adversarialManifestPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--gpu-name") == 0 && i + 1 < argc)
        {
            config.gpuName = argv[++i];
        }
        else if (std::strcmp(argv[i], "--perf-min-mb") == 0 && i + 1 < argc)
        {
            config.perfMinMB = std::atoi(argv[++i]);
            if (config.perfMinMB < 0)
                config.perfMinMB = 0;   // <= 0 disables the perf-size skip (all files eligible for perf)
        }
        else if (std::strcmp(argv[i], "--gbv-sample-count") == 0 && i + 1 < argc)
        {
            config.gbvSampleCount = std::atoi(argv[++i]);
            if (config.gbvSampleCount < 0)
                config.gbvSampleCount = 0;   // <= 0 = no cap; GBV runs on every file
        }
        else if (std::strcmp(argv[i], "--gbv-max-mb") == 0 && i + 1 < argc)
        {
            config.gbvMaxMB = std::atoi(argv[++i]);
            if (config.gbvMaxMB< 0)
                config.gbvMaxMB = INT_MAX / (1024*1024); // <= 0 = no cap; GBV runs on any file size
        }
        else if (std::strcmp(argv[i], "--help-ci") == 0)
        {
            PrintUsage(argv[0]);
            shouldExit = true;
            return true;
        }
    }
    return true;
}

// Fail-loud validation + one-shot filesystem discovery. Any misconfiguration
// means the run cannot produce meaningful test coverage, so we return a
// non-zero exit code before any test instantiates. Silent skipping would let
// broken pipelines pass green with zero coverage.
//
// On success also creates the log directory (defaulting to cwd) so downstream
// writes don't have to check.
static int ValidateAndDiscover(TestConfig& config)
{
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

    if (config.logDir.empty())
    {
        config.logDir = std::filesystem::current_path().string();
    }
    if (!std::filesystem::exists(config.logDir))
    {
        std::filesystem::create_directories(config.logDir);
    }

    // Load the adversarial manifest. Resolution order:
    //   1. If --adversarial-manifest was passed, use that path (explicit override).
    //      A missing file at an explicit path is a hard error — the user asked
    //      for a specific manifest and we can't find it (probably pipeline
    //      misconfiguration worth failing loud).
    //   2. Else look for <content-path>/adversarial_manifest.json. If it
    //      exists, load it. If it doesn't, run in legacy mode (every file
    //      expected to succeed) — no message, no error.
    //   3. In BOTH loaded cases, if the file exists but fails to parse, that
    //      is a hard error — a broken manifest sitting in the content tree
    //      is a corruption bug worth flagging, not silently ignoring.
    bool manifestFromExplicitFlag = !config.adversarialManifestPath.empty();
    if (!manifestFromExplicitFlag)
    {
        std::filesystem::path autoPath =
            std::filesystem::path(config.contentPath) / "adversarial_manifest.json";
        if (std::filesystem::exists(autoPath))
        {
            config.adversarialManifestPath = autoPath.string();
        }
    }

    if (!config.adversarialManifestPath.empty())
    {
        std::string loadError;
        if (!config.adversarialManifest.LoadFromFile(config.adversarialManifestPath, loadError))
        {
            return Fail("adversarial manifest failed to load from '" +
                        config.adversarialManifestPath + "': " + loadError);
        }
        std::cout << "Loaded adversarial manifest with "
                  << config.adversarialManifest.Size()
                  << " entries from '" << config.adversarialManifestPath << "'"
                  << (manifestFromExplicitFlag ? " (via --adversarial-manifest)." : " (auto-discovered in content-path).")
                  << "\n";

        // Coverage self-check. A manifest that loads but matches NOTHING is
        // almost always a content-path / glob-prefix misconfiguration (globs are
        // authored relative to a sub-tree while --content-path points elsewhere).
        // Left undetected this silently turns every correctly-rejected file into
        // a spurious failure AND disables the rejection safety net, so fail loud
        // rather than run with an inert manifest.
        const size_t filesMatched = config.adversarialManifest.CountCoverage(
            config.discoveredFiles, config.contentPath);
        if (config.adversarialManifest.Size() > 0 && filesMatched == 0)
        {
            return Fail("adversarial manifest loaded " +
                        std::to_string(config.adversarialManifest.Size()) +
                        " entries but matched 0 of " +
                        std::to_string(config.discoveredFiles.size()) +
                        " discovered files. This is almost certainly a content-path/glob "
                        "prefix mismatch: manifest globs are authored relative to a sub-tree. "
                        "Verify --content-path '" +
                        config.contentPath + "' points at or above that tree.");
        }
    }
    else
    {
        std::cout << "No adversarial manifest found (neither --adversarial-manifest passed "
                  << "nor <content-path>/adversarial_manifest.json exists). "
                  << "Running in legacy mode: every file expected to succeed.\n";
    }

    return 0;
}

int main(int argc, char** argv)
{
    TestConfig config;

    bool shouldExit = false;
    if (!ParseArgs(argc, argv, config, shouldExit))
        return 1;
    if (shouldExit)
        return 0;   // --help-ci printed usage and asked to exit cleanly

    if (int rc = ValidateAndDiscover(config); rc != 0)
        return rc;

    g_testConfig = std::move(config);

    testing::InitGoogleTest(&argc, argv);
    testing::GTEST_FLAG(catch_exceptions) = false;
    return RUN_ALL_TESTS();
}
