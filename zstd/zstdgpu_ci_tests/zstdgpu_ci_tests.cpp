/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Test definitions and demo runner for the Zstd GPU CI tests.
//
// Parameterized test suite (ZstdGpuDemoTests) instantiated once per .zst file
// under the content path. Each file produces the following scenarios:
//
//   Correctness — "for all data" (ASSERT — hard fail):
//     - GpuCheck           : --chk-gpu
//     - GpuCheckSeq        : --chk-gpu --seq-cnt
//     - ExternalMemory     : --chk-gpu --ext-mem
//     - ExternalMemorySeq  : --chk-gpu --ext-mem --seq-cnt
//     - D3D12DebugLayer    : --chk-gpu --d3d-dbg            (skipped on ARM)
//     - D3D12DebugLayerSeq : --chk-gpu --d3d-dbg --seq-cnt  (skipped on ARM)
//     - SimulationCheck    : --chk-gpu --chk-cpu --sim-gpu
//     - SimulationCheckSeq : --chk-gpu --chk-cpu --sim-gpu --seq-cnt
//
//   Correctness — GBV (ASSERT; skipped on ARM; run only on the stride-selected
//   subset of files chosen by --gbv-sample-count):
//     - Gbv                : --chk-gpu --d3d-dbg --d3d-gbv
//     - GbvSeq             : --chk-gpu --d3d-dbg --d3d-gbv --seq-cnt
//
//   Performance (EXPECT — soft fail, also verify CSV output was written):
//     - PerStageTiming     : --prf-lvl 2 --d3d-gfx --seq-cnt → results/stages_<stem>.csv

#include "zstdgpu_ci_tests.h"
#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <Windows.h>

// Internal types + forward declarations
//
// These are used only inside this translation unit; keeping them out of the
// header limits the wrapper's public surface to the two things main.cpp
// actually needs (TestConfig, DiscoverZstFiles).

namespace
{
    // Captures the outcome of a single demo process invocation.
    struct DemoResult
    {
        int exitCode = -1;        // Process exit code (0 = success)
        std::string stdOut;       // Captured stdout + stderr
        std::string launchError;  // Error message if the process failed to launch
        std::string commandLine;  // The exact command line that was executed
        bool timedOut = false;    // True if the process was killed due to timeout
    };

    DemoResult RunDemo(
        const std::string& demoPath,
        const std::vector<std::string>& args,
        int timeoutSeconds);

    std::vector<std::string> BuildCorrectnessArgs(
        const std::string& zstFile,
        const std::vector<std::string>& scenarioFlags);

    std::vector<std::string> BuildPerformanceArgs(
        const std::string& zstFile,
        int profilingLevel,
        int runCount,
        const std::string& csvOutputPath,
        const std::vector<std::string>& extraFlags);
}

// Helpers

// Returns the list of .zst files to parameterize over.
// GTest evaluates this lazily when the test suite is instantiated (after main
// has parsed CLI args, validated inputs, and cached the discovered file list
// in TestConfig). We just return the cached list here — the actual filesystem
// walk happened once, up front, in main().
static std::vector<std::string> GetTestFiles()
{
    return g_testConfig.discoveredFiles;
}

// Converts a full file path to a valid GTest parameter name.
// GTest names must be alphanumeric + underscore, no leading digits, AND UNIQUE
// across the full INSTANTIATE_TEST_SUITE_P set. Using just the filename stem
// collides when the same leaf name appears across different subdirectories
// (e.g. firefly_albedo.DDS.zst exists under BC1/, BC1mip0/, block4K_*, etc.),
// causing a fatal gtest assertion at startup. Use the path relative to
// --content-path so different folders produce different names.
static std::string SanitizeTestName(const testing::TestParamInfo<std::string>& info)
{
    std::filesystem::path full(info.param);
    std::filesystem::path rel;
    if (!g_testConfig.contentPath.empty())
    {
        std::error_code ec;
        rel = std::filesystem::relative(full, g_testConfig.contentPath, ec);
        if (ec || rel.empty() || rel.string().rfind("..", 0) == 0)
        {
            rel = full.filename();  // fallback: out-of-tree, just use leaf
        }
    }
    else
    {
        rel = full.filename();
    }

    // Drop the trailing .zst extension for readability; everything else stays.
    std::string name = rel.string();
    const std::string ext = ".zst";
    if (name.size() >= ext.size() &&
        name.compare(name.size() - ext.size(), ext.size(), ext) == 0)
    {
        name.resize(name.size() - ext.size());
    }

    std::string result;
    result.reserve(name.size());
    for (char c : name)
    {
        result += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0])))
    {
        result = "_" + result;
    }
    return result.empty() ? "Unknown" : result;
}

// Appends per-test output to the consolidated log file (--log-file).
static void WriteToLogFile(const std::string& zstFile, const DemoResult& result)
{
    if (g_testConfig.logFile.empty())
        return;

    std::ofstream log(g_testConfig.logFile, std::ios::app);
    if (!log)
    {
        std::cerr << "Warning: could not open --log-file '"
                  << g_testConfig.logFile << "' for append.\n";
        return;
    }
    auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
    log << "=== " << testInfo->test_suite_name() << "." << testInfo->name() << " ===\n";
    log << "File: " << zstFile << "\n";
    log << "Exit code: " << result.exitCode << "\n";
    log << result.stdOut << "\n";
}

// Test runners

// Returns true if the .zst file lives under a directory named "fuzz". Fuzzing
// content is a mix of clean and corrupt inputs that exercise varying code
// paths, so its timing isn't meaningful performance data — perf tests skip it.
// This is a pure path check, independent of the manifest.
static bool IsFuzzContent(const std::string& zstFile)
{
    for (const auto& part : std::filesystem::path(zstFile))
    {
        if (part == "fuzz")
            return true;
    }
    return false;
}

// Returns the set of files the Gbv/GbvSeq scenarios run on: an even, endpoint-
// inclusive stride of g_testConfig.gbvSampleCount files across the sorted
// discovered-file list. A count <= 0, or a corpus no larger than the count,
// selects every file; a count of 1 selects the first file. Computed once.
static const std::unordered_set<std::string>& GbvSampledFiles()
{
    static const std::unordered_set<std::string> selected = []
    {
        std::unordered_set<std::string> s;
        std::vector<std::string> smallFiles;
        {
            const auto& files = g_testConfig.discoveredFiles; // sorted full paths
            std::copy_if(
                files.begin(),
                files.end(),
                std::back_inserter(smallFiles),
                [](const std::string& filename)
                {
                    try
                    {
                        return std::filesystem::file_size(filename) < g_testConfig.gbvMaxMB * (1024 * 1024);
                    }
                    catch (const std::filesystem::filesystem_error&)
                    {
                        return false; // skip missing/inaccessible files
                    }
                });
        }
        const size_t n = smallFiles.size();
        if (n == 0)
            return s;
        const int target = g_testConfig.gbvSampleCount;
        if (target <= 0 || n <= static_cast<size_t>(target))
        {
            // count <= 0, or corpus no larger than the count: select every file.
            s.insert(smallFiles.begin(), smallFiles.end());
            return s;
        }
        if (target == 1)
        {
            s.insert(smallFiles.front());
            return s;
        }
        const size_t t = static_cast<size_t>(target);
        // Endpoint-inclusive stride: indices 0 .. n-1 taken in t steps so the
        // sample spans the whole sorted list (first through last).
        for (size_t i = 0; i < t; ++i)
        {
            const size_t idx = (i * (n - 1)) / (t - 1);
            s.insert(smallFiles[idx]);
        }
        return s;
    }();
    return selected;
}

// True if this file is one of the stride-selected files the GBV scenarios run on.
static bool IsSelectedForGbv(const std::string& zstFile)
{
    const auto& sel = GbvSampledFiles();
    return sel.find(zstFile) != sel.end();
}

// True if the file is smaller than --perf-min-mb. Returns false when
// --perf-min-mb <= 0 (disabled) or the file size cannot be read.
static bool IsSmallForPerf(const std::string& zstFile)
{
    if (g_testConfig.perfMinMB <= 0) return false;
    std::error_code ec;
    auto size = std::filesystem::file_size(zstFile, ec);
    if (ec) return false;
    return size < static_cast<uintmax_t>(g_testConfig.perfMinMB) * 1024ULL * 1024ULL;
}

// If the adversarial manifest lists this file, verify that the demo rejected it
// with the expected exit code. Returns true iff the outcome is a correct
// rejection, or false if the file isn't in the manifest (in which case the
// caller applies the legacy "expect 0" check). Sets handledByManifest when a
// manifest entry matched, and adds a gtest failure on mismatch.
static bool CheckAdversarialOrLegacy(const std::string& zstFile, const DemoResult& result, bool& handledByManifest)
{
    handledByManifest = false;
    const AdversarialEntry* entry = g_testConfig.adversarialManifest.Match(zstFile, g_testConfig.contentPath);
    if (!entry)
        return false;

    handledByManifest = true;

    if (result.exitCode != entry->expectedExitCode)
    {
        ADD_FAILURE()
            << "Adversarial file expected to be rejected with exit code "
            << entry->expectedExitCode << " but demo returned " << result.exitCode << ".\n"
            << "File: " << zstFile << "\n"
            << "Reason: " << entry->reason << "\n"
            << "Command: " << result.commandLine
            << "  (stdout already printed above as [DEMO OUT])";
        return false;
    }

    return true;
}

// Run a correctness scenario. Spawns zstdgpu_demo.exe with the given .zst file and scenario flags, then asserts exit code == 0.
// main() has already validated the demo path exists, so we don't re-check here.
// stdout is printed via the unconditional [DEMO OUT] block below and does NOT
// appear a second time in the ASSERT_EQ failure message — that would duplicate
// the same text in the log.
static void RunCorrectnessTest(const std::string& zstFile, const std::vector<std::string>& scenarioFlags)
{
    auto args = BuildCorrectnessArgs(zstFile, scenarioFlags);
    auto result = RunDemo(g_testConfig.demoPath, args, g_testConfig.timeoutSeconds);

    // Write to log file before assertions so logs are captured even if an ASSERT aborts early.
    WriteToLogFile(zstFile, result);

    // Log the output regardless of pass/fail. This IS the demo stdout capture —
    // the assertion messages below intentionally do NOT reprint result.stdOut.
    std::cout << "[DEMO CMD] " << result.commandLine << "\n";
    if (!result.stdOut.empty())
    {
        std::cout << "[DEMO OUT] " << result.stdOut << "\n";
    }

    ASSERT_FALSE(result.timedOut)
        << "Demo process timed out after " << g_testConfig.timeoutSeconds << " seconds.\n"
        << "Command: " << result.commandLine;

    ASSERT_TRUE(result.launchError.empty())
        << "Failed to launch demo: " << result.launchError << "\n"
        << "Command: " << result.commandLine;

    bool handledByManifest = false;
    if (CheckAdversarialOrLegacy(zstFile, result, handledByManifest))
    {
        // Adversarial file rejected as expected — success. Nothing more to check.
        return;
    }
    if (handledByManifest)
    {
        // Manifest entry matched but the outcome didn't match. CheckAdversarialOrLegacy
        // already added the failure diagnostic. Stop here rather than fall through
        // to the legacy check (which would produce a redundant "exit != 0" failure).
        return;
    }

    // Legacy path: file not in manifest, expect success.
    ASSERT_EQ(result.exitCode, 0)
        << "Demo process returned non-zero exit code: " << result.exitCode << "\n"
        << "Command: " << result.commandLine
        << "  (stdout already printed above as [DEMO OUT])";
}

// Run a performance scenario. Spawns zstdgpu_demo.exe with profiling flags and requests CSV output. Uses EXPECT (not ASSERT) to verify the demo executed successfully and produced CSV output.
// main() has already validated the demo path exists.
static void RunPerformanceTest(const std::string& zstFile, int profilingLevel,
                                const std::vector<std::string>& extraFlags)
{
    // Fuzzing content mixes clean and corrupt inputs with varying code paths,
    // so its timing isn't meaningful perf data — skip it before running the demo.
    if (IsFuzzContent(zstFile))
    {
        GTEST_SKIP()
            << "Perf test skipped: file is fuzzing content (under a 'fuzz' directory).\n"
            << "File: " << zstFile;
        return;
    }

    // Skip files smaller than --perf-min-mb.
    if (IsSmallForPerf(zstFile))
    {
        GTEST_SKIP()
            << "Perf test skipped: file is under --perf-min-mb ("
            << g_testConfig.perfMinMB << " MB).\n"
            << "File: " << zstFile;
        return;
    }

    // Perf CSVs are written as stages_<stem>.csv under the results directory.
    std::string stem = std::filesystem::path(zstFile).stem().string();
    std::filesystem::path resultsDir = std::filesystem::path(g_testConfig.logDir) / "results";
    if (!std::filesystem::exists(resultsDir))
    {
        std::filesystem::create_directories(resultsDir);
    }
    std::string csvPath = (resultsDir / ("stages_" + stem + ".csv")).string();

    auto args = BuildPerformanceArgs(zstFile, profilingLevel, g_testConfig.runCount, csvPath, extraFlags);
    auto result = RunDemo(g_testConfig.demoPath, args, g_testConfig.timeoutSeconds);

    // Write to log file before assertions so logs are captured even if a check fails.
    WriteToLogFile(zstFile, result);

    // Log the output regardless of pass/fail. This IS the demo stdout capture —
    // the assertion messages below intentionally do NOT reprint result.stdOut.
    std::cout << "[DEMO CMD] " << result.commandLine << "\n";
    if (!result.stdOut.empty())
    {
        std::cout << "[DEMO OUT] " << result.stdOut << "\n";
    }

    EXPECT_FALSE(result.timedOut)
        << "Demo process timed out after " << g_testConfig.timeoutSeconds << " seconds.\n"
        << "Command: " << result.commandLine;

    EXPECT_TRUE(result.launchError.empty())
        << "Failed to launch demo: " << result.launchError << "\n"
        << "Command: " << result.commandLine;

    // Fuzz content was already skipped above, so any file reaching here is
    // expected to decode successfully AND produce a CSV.
    EXPECT_EQ(result.exitCode, 0)
        << "Demo process returned non-zero exit code: " << result.exitCode << "\n"
        << "Command: " << result.commandLine
        << "  (stdout already printed above as [DEMO OUT])";

    EXPECT_TRUE(std::filesystem::exists(csvPath)) << "CSV not created: " << csvPath;

    if (std::filesystem::exists(csvPath))
    {
        std::cout << "[PERF CSV] Written to: " << csvPath << "\n";
    }
}

// Test fixture and test cases

// Test fixture parameterized over .zst file paths (spec: ZstdGpuDemoTests).
// Both correctness and performance tests share this fixture — correctness tests
// use ASSERT (hard fail), performance tests use EXPECT (soft fail).
class ZstdGpuDemoTests : public ::testing::TestWithParam<std::string>
{
};

// --- Correctness tests — "for all data" matrix ---

// GPU correctness check (--chk-gpu only).
TEST_P(ZstdGpuDemoTests, GpuCheck)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu"});
}

// GpuCheck in single-submission mode (--seq-cnt).
TEST_P(ZstdGpuDemoTests, GpuCheckSeq)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--seq-cnt"});
}

TEST_P(ZstdGpuDemoTests, ExternalMemory)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--ext-mem"});
}

TEST_P(ZstdGpuDemoTests, ExternalMemorySeq)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--ext-mem", "--seq-cnt"});
}

TEST_P(ZstdGpuDemoTests, D3D12DebugLayer)
{
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_ARM64EC)
    GTEST_SKIP() << "D3D12 debug layer tests are skipped on ARM platforms.";
#else
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--d3d-dbg"});
#endif
}

TEST_P(ZstdGpuDemoTests, D3D12DebugLayerSeq)
{
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_ARM64EC)
    GTEST_SKIP() << "D3D12 debug layer tests are skipped on ARM platforms.";
#else
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--d3d-dbg", "--seq-cnt"});
#endif
}

TEST_P(ZstdGpuDemoTests, SimulationCheck)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--chk-cpu", "--sim-gpu"});
}

TEST_P(ZstdGpuDemoTests, SimulationCheckSeq)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--chk-cpu", "--sim-gpu", "--seq-cnt"});
}

// --- Correctness tests — GBV ---

TEST_P(ZstdGpuDemoTests, Gbv)
{
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_ARM64EC)
    GTEST_SKIP() << "GBV tests are skipped on ARM platforms.";
#else
    if (!IsSelectedForGbv(GetParam()))
    {
        GTEST_SKIP() << "GBV skipped: file is not in the stride-selected GBV sample ("
                     << g_testConfig.gbvSampleCount << " files).";
        return;
    }
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--d3d-dbg", "--d3d-gbv"});
#endif
}

TEST_P(ZstdGpuDemoTests, GbvSeq)
{
#if defined(_M_ARM) || defined(_M_ARM64) || defined(_M_ARM64EC)
    GTEST_SKIP() << "GBV tests are skipped on ARM platforms.";
#else
    if (!IsSelectedForGbv(GetParam()))
    {
        GTEST_SKIP() << "GBV skipped: file is not in the stride-selected GBV sample ("
                     << g_testConfig.gbvSampleCount << " files).";
        return;
    }
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--d3d-dbg", "--d3d-gbv", "--seq-cnt"});
#endif
}

// --- Performance tests ---

// Per-stage timings (--prf-lvl 2) on the DIRECT queue (--d3d-gfx) in single-
// submission mode (--seq-cnt). Skips fuzz content and files under --perf-min-mb.
TEST_P(ZstdGpuDemoTests, PerStageTiming)
{
    RunPerformanceTest(GetParam(), 2, {"--d3d-gfx", "--seq-cnt"});
}

INSTANTIATE_TEST_SUITE_P(
    ContentTests,
    ZstdGpuDemoTests,
    ::testing::ValuesIn(GetTestFiles()),
    SanitizeTestName);

// Demo runner implementation
//
// Spawns zstdgpu_demo.exe as a child process via CreateProcess with anonymous
// pipes. A background thread drains stdout to avoid pipe-buffer deadlocks. If
// the child exceeds the configured timeout, it is terminated and CancelIoEx
// releases the reader thread so the wrapper does not itself hang.
//
// All GPU / D3D12 dependencies live in the demo process; the test binary
// itself has no GPU dependency.

namespace
{

// Builds a command line string with proper quoting for arguments containing spaces.
static std::string BuildCommandLine(const std::string& exe, const std::vector<std::string>& args)
{
    std::ostringstream cmd;
    cmd << "\"" << exe << "\"";
    for (const auto& arg : args)
    {
        cmd << " ";
        if (arg.find(' ') != std::string::npos)
            cmd << "\"" << arg << "\"";
        else
            cmd << arg;
    }
    return cmd.str();
}

DemoResult RunDemo(
    const std::string& demoPath,
    const std::vector<std::string>& args,
    int timeoutSeconds)
{
    DemoResult result;
    result.commandLine = BuildCommandLine(demoPath, args);

    // Create an anonymous pipe for capturing the child process's stdout/stderr.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr;
    HANDLE hWritePipe = nullptr;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
    {
        result.launchError = "Failed to create pipe for demo process.";
        return result;
    }

    // Prevent the read end from being inherited by the child process.
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // Redirect child's stdout and stderr to the write end of the pipe.
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi{};

    std::vector<char> cmdBuf(result.commandLine.begin(), result.commandLine.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(
            nullptr,
            cmdBuf.data(),
            nullptr,
            nullptr,
            TRUE, // inherit handles
            0,
            nullptr,
            nullptr,
            &si,
            &pi))
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        result.launchError = "Failed to launch demo process. Error: " + std::to_string(GetLastError());
        return result;
    }

    // Close the write end in the parent so ReadFile on the read end returns
    // EOF when the child exits.
    CloseHandle(hWritePipe);

    // Read the child's output on a background thread to prevent pipe buffer
    // deadlocks (the pipe has a finite buffer; if it fills, the child blocks).
    std::string capturedOutput;
    std::thread readerThread([&capturedOutput, hReadPipe]() {
        std::array<char, 4096> buf;
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr) && bytesRead > 0)
        {
            capturedOutput.append(buf.data(), bytesRead);
        }
    });

    // Wait for the child process, enforcing the timeout.
    DWORD waitMs = (timeoutSeconds > 0) ? static_cast<DWORD>(timeoutSeconds) * 1000 : INFINITE;
    DWORD waitResult = WaitForSingleObject(pi.hProcess, waitMs);

    if (waitResult == WAIT_TIMEOUT)
    {
        result.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);

        // Cancel any pending ReadFile on hReadPipe so the reader thread exits
        // and readerThread.join() below returns. Without this, TerminateProcess
        // on a wedged child can leave the child's inherited write-end of the
        // pipe orphaned in kernel state briefly, and the reader thread's
        // blocking ReadFile never returns — hanging the entire test runner
        // indefinitely. CancelIoEx targets outstanding I/O on the handle from
        // any thread. Safe to call even if no I/O is pending (returns FALSE
        // with ERROR_NOT_FOUND, harmless).
        CancelIoEx(hReadPipe, nullptr);
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Wait for the reader thread to finish draining the pipe, then clean up.
    readerThread.join();
    CloseHandle(hReadPipe);

    result.stdOut = std::move(capturedOutput);
    return result;
}

// Builds argument list for correctness tests: decompress once (--run-cnt 1)
// with GPU and CPU validation enabled, plus scenario-specific flags.
std::vector<std::string> BuildCorrectnessArgs(
    const std::string& zstFile,
    const std::vector<std::string>& scenarioFlags)
{
    std::vector<std::string> args;
    args.push_back("--zst");
    args.push_back(zstFile);
    args.push_back("--run-cnt");
    args.push_back("1");
    for (const auto& flag : scenarioFlags)
    {
        args.push_back(flag);
    }
    return args;
}

// Builds argument list for performance tests: run N iterations at the specified
// profiling level, optionally writing per-run timing data to a CSV file. Any
// scenario-specific demo flags are appended from `extraFlags`.
std::vector<std::string> BuildPerformanceArgs(
    const std::string& zstFile,
    int profilingLevel,
    int runCount,
    const std::string& csvOutputPath,
    const std::vector<std::string>& extraFlags)
{
    std::vector<std::string> args;
    args.push_back("--zst");
    args.push_back(zstFile);
    args.push_back("--prf-lvl");
    args.push_back(std::to_string(profilingLevel));
    args.push_back("--run-cnt");
    args.push_back(std::to_string(runCount));
    if (!csvOutputPath.empty())
    {
        args.push_back("--out-csv");
        args.push_back(csvOutputPath);
    }
    for (const auto& flag : extraFlags)
    {
        args.push_back(flag);
    }
    return args;
}

} // namespace
