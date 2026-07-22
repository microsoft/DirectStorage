/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Shared header for the Zstd GPU CI tests.
// Exposes only what genuinely crosses translation-unit boundaries:
// the TestConfig struct, a global instance, and file discovery.
// The demo runner, DemoResult, and argument builders are internal
// to zstdgpu_ci_tests.cpp and stay hidden as static functions there.

#pragma once

#include <string>
#include <vector>

#include "adversarial_manifest.h"

// Test configuration — parsed from CLI in main(), read by tests.

struct TestConfig
{
    std::string contentPath;                    // Directory containing .zst test files
    std::string demoPath;                       // Full path to zstdgpu_demo.exe
    std::string logDir;                         // Directory for logs, CSVs, and GTest XML output
    std::string logFile;                        // Consolidated text log file path (--log-file)
    std::string adversarialManifestPath;        // Optional path to adversarial_manifest.json (--adversarial-manifest)
    int runCount = 40;                          // Number of iterations for performance tests
    int timeoutSeconds = 0;                     // Max seconds before killing a demo process (0 = no timeout)
    int perfMinMB = 4;                          // Min .zst size (MB) required for perf tests. Smaller files skip perf (individually-compressed textures are not representative).
    int gbvSampleCount = 10;                    // Number of files the Gbv/GbvSeq scenarios run on, chosen by an even stride across the sorted corpus. <= 0 = no cap (run GBV on all files).

    // Cached list of .zst files discovered under contentPath. Populated once
    // in main() after validation; consumed by GetTestFiles() at fixture
    // instantiation. Avoids walking the tree twice.
    std::vector<std::string> discoveredFiles;

    // Loaded once in main() from --adversarial-manifest, then read-only. When
    // not loaded (flag absent OR file missing), the wrapper falls back to the
    // legacy behavior of expecting every file to succeed — additive, no test
    // starts failing just because the manifest isn't wired up.
    AdversarialManifest adversarialManifest;
};

// Global config, set once in main() before RUN_ALL_TESTS(), then read-only
// from test bodies.
extern TestConfig g_testConfig;

// File discovery — scans a directory for *.zst files. Returns sorted full paths.
// Called by main() during startup.
std::vector<std::string> DiscoverZstFiles(const std::string& contentPath);
