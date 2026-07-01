/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Shared header for the Zstd GPU CI tests. Defines the runtime configuration,
// demo process result type, and declarations for the demo runner and file
// discovery helpers. Both main.cpp and zstdgpu_ci_tests.cpp include this.

#pragma once

#include <string>
#include <vector>

// Test configuration — parsed from CLI in main(), read by tests.

struct TestConfig
{
    std::string contentPath;                    // Directory containing .zst test files
    std::string demoPath;                       // Full path to zstdgpu_demo.exe
    std::string logDir;                         // Directory for logs, CSVs, and GTest XML output
    std::string logFile;                        // Consolidated text log file path (--log-file)
    int runCount = 40;                          // Number of iterations for performance tests
    int timeoutSeconds = 0;                     // Max seconds before killing a demo process (0 = no timeout)

    // Cached list of .zst files discovered under contentPath. Populated once
    // in main() after validation; consumed by GetTestFiles() at fixture
    // instantiation. Avoids walking the tree twice.
    std::vector<std::string> discoveredFiles;
};

// Singleton access. SetTestConfig called once from main(), GetTestConfig
// called from test helpers.
// Needed for GTest's TEST_P bodies to access config without passing it through
// parameters.
const TestConfig& GetTestConfig();
void SetTestConfig(TestConfig config);

// Demo runner — spawns zstdgpu_demo.exe and captures output.

// Captures the outcome of a single demo process invocation.
struct DemoResult
{
    int exitCode = -1;        // Process exit code (0 = success)
    std::string stdOut;       // Captured stdout + stderr
    std::string launchError;  // Error message if the process failed to launch
    std::string commandLine;  // The exact command line that was executed
    bool timedOut = false;    // True if the process was killed due to timeout
};

// Spawns zstdgpu_demo.exe with the given arguments, captures output, and
// returns the result. timeoutSeconds=0 means no timeout.
DemoResult RunDemo(
    const std::string& demoPath,
    const std::vector<std::string>& args,
    int timeoutSeconds = 0);

// Convenience: builds the full argument list for a correctness scenario.
std::vector<std::string> BuildCorrectnessArgs(
    const std::string& zstFile,
    const std::vector<std::string>& scenarioFlags);

// Convenience: builds the full argument list for a performance scenario.
std::vector<std::string> BuildPerformanceArgs(
    const std::string& zstFile,
    int profilingLevel,
    int runCount,
    const std::string& csvOutputPath);

// File discovery — scans directories for .zst test content.
// Recursively scans a directory for *.zst files. Returns sorted full paths.
std::vector<std::string> DiscoverZstFiles(const std::string& contentPath);
