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

#include <cstdint>
#include <string>
#include <string_view>
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
    std::string gpuName;                        // Adapter name of the machine under test (--gpu-name). Used only for the
                                                // manifest's scenario skips.
    uint32_t gpuVendorId = 0;                    // PCI vendor ID forwarded to zstdgpu_demo. 0 = unset.
    int runCount = 40;                          // Number of iterations for performance tests
    int timeoutSeconds = 0;                     // Max seconds before killing a demo process (0 = no timeout)
    int perfMinMB = 4;                          // Min .zst size (MB) required for perf tests. Smaller files skip perf (individually-compressed textures are not representative).
    int gbvSampleCount = 0;                     // Number of files the Gbv/GbvSeq scenarios run on, chosen by an even stride across the sorted corpus. <= 0 = no cap (run GBV on all files within --gbv-max-mb).
    int gbvMaxMB = 4;                           // Max largest-frame decompressed size (MB) for Gbv tests. Files with a bigger single frame are skipped to avoid GBV TDRs (GBV slows the GPU, so hang risk tracks the largest per-frame dispatch's decompressed output, not the whole-file total).
    int maxFrameMB = 0;                          // Skip any file whose largest on-disk zstd frame exceeds this (MB). <= 0 = disabled (run every file).
    int idxMax = -1;                             // Forwarded to the demo as --idx-max (inclusive last frame index). < 0 = unset (demo runs all frames).
    int correctnessBatchMB = 256;                // Max total on-disk (compressed) size (MB) of clean files grouped into one correctness batch. Bounds each concatenated demo run: batching by file count alone lets large-texture batches reach multi-GB decompressed, overflowing the demo's int32 size fields (>2 GB) and exceeding a GPU buffer limit. <= 0 = no byte cap (count-only).
    int correctnessBatchCount = 64;              // Secondary cap: max number of files per correctness batch, regardless of size. <= 0 = no count cap (size-only).

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

// Parses a nonzero PCI vendor ID written in hexadecimal. An optional 0x prefix
// is accepted. On failure, returns false and describes the invalid value.
bool ParseGpuVendorId(std::string_view value, uint32_t& vendorId, std::string& error);

// Appends the zstdgpu_demo adapter selector when an explicit vendor was set.
// A zero ID means "use the demo's normal adapter selection".
void AppendGpuVendorArgs(std::vector<std::string>& args, uint32_t vendorId);

// File discovery — scans a directory for *.zst files. Returns sorted full paths.
// Called by main() during startup.
std::vector<std::string> DiscoverZstFiles(const std::string& contentPath);
