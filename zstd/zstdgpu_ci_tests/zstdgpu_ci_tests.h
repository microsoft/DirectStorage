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
    std::string perfManifestPath;               // Optional path to perf_manifest.json (--perf-manifest); selects the latency/throughput perf corpus
    std::string gpuName;                        // Adapter name of the machine under test (--gpu-name). Used only for the
                                                // manifest's scenario skips.
    int timeoutSeconds = 0;                     // Max seconds before killing a demo process (0 = no timeout)
    int gbvSampleCount = 0;                     // Number of files the Gbv/GbvSeq scenarios run on, chosen by an even stride across the sorted corpus. <= 0 = no cap (run GBV on all files within --gbv-max-mb).
    int gbvMaxMB = 4;                           // Max largest-frame decompressed size (MB) for Gbv tests. Files with a bigger single frame are skipped to avoid GBV TDRs (GBV slows the GPU, so hang risk tracks the largest per-frame dispatch's decompressed output, not the whole-file total).
    int maxFrameMB = 0;                          // Skip any file whose largest on-disk zstd frame exceeds this (MB). <= 0 = disabled (run every file).
    int idxMax = -1;                             // Forwarded to the demo as --idx-max (inclusive last frame index). < 0 = unset (demo runs all frames).
    int correctnessBatchMB = 256;                // Max total on-disk (compressed) size (MB) of clean files grouped into one correctness batch. Bounds each concatenated demo run: batching by file count alone lets large-texture batches reach multi-GB decompressed, overflowing the demo's int32 size fields (>2 GB) and exceeding a GPU buffer limit. <= 0 = no byte cap (count-only).
    int correctnessBatchCount = 64;              // Secondary cap: max number of files per correctness batch, regardless of size. <= 0 = no count cap (size-only).

    // --- Batched perf knobs ---
    // The perf tests tile a fixed-size frame batch across the perf-manifest's
    // selected corpus (one batch per GPU dispatch) over a number of measured
    // sweeps. The demo reports each batch's latency/bandwidth on [PERF] lines plus
    // a pooled total: the P50 of the per-batch latencies and the median of the
    // per-batch bandwidths. Latency runs a single small batch size. Throughput
    // sweeps a ladder of batch sizes (the harness invokes the demo once per rung),
    // so per-dispatch overhead is amortized differently at each window size.
    int perfLatencyFrameCount = 12;              // Frames per batch for the Latency scenario (fixed CI constant).
    // Throughput ladder: the harness invokes the demo once per rung (batch size),
    // each tiling the whole corpus, so every rung reports its own median bandwidth
    // -- a bandwidth-vs-window curve. Smallest 64, top 1024, with x1.5 midpoints
    // (192/384/768) as extra rungs beyond straight doubling. A fixed CI constant.
    std::vector<int> perfThroughputFrameCounts = { 64, 128, 192, 256, 384, 512, 768, 1024 };
    int perfRunCount = 5;                        // Measured sweeps over the whole set (a fixed CI constant), forwarded to the demo as --run-cnt.  A compromise value that keeps the pooled P50/median stable while bounding CI execution time.

    // Cached list of .zst files discovered under contentPath. Populated once
    // in main() after validation; consumed by GetTestFiles() at fixture
    // instantiation. Avoids walking the tree twice.
    std::vector<std::string> discoveredFiles;

    // Loaded once in main() from --adversarial-manifest, then read-only. When
    // not loaded (flag absent OR file missing), the wrapper falls back to the
    // legacy behavior of expecting every file to succeed — additive, no test
    // starts failing just because the manifest isn't wired up.
    AdversarialManifest adversarialManifest;

    // Loaded once in main() from --perf-manifest (or auto-discovered at
    // <content-path>/perf_manifest.json), then read-only. Selects the files each
    // perf scenario (latency/throughput) runs over, replacing the old hardcoded
    // "path contains 64KB" rule. When not loaded, the perf tests fail loud on an
    // empty selection; correctness tests are unaffected.
    PerfManifest perfManifest;
};

// Global config, set once in main() before RUN_ALL_TESTS(), then read-only
// from test bodies.
extern TestConfig g_testConfig;

// File discovery — scans a directory for *.zst files. Returns sorted full paths.
// Called by main() during startup.
std::vector<std::string> DiscoverZstFiles(const std::string& contentPath);
