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
// under the content path. Each file produces 6 scenarios:
//
//   Correctness (ASSERT — hard fail):
//     - SimulationCheck  : --sim-gpu with CPU+GPU validation
//     - D3D12DebugLayer  : --d3d-dbg
//     - ExternalMemory   : --ext-mem
//     - GraphicsQueue    : --d3d-gfx
//
//   Performance (EXPECT — soft fail, also verify CSV output was written):
//     - OverallThroughput: --prf-lvl 0 → results/throughput_<stem>.csv
//     - PerStageTiming   : --prf-lvl 2 → results/stages_<stem>.csv

#include "zstdgpu_ci_tests.h"
#include <gtest/gtest.h>
#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
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
        const std::string& csvOutputPath);
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

// Appends the phase-2 (--d3d-dbg diagnostic) demo output to the same
// consolidated log file with a marker separator. Used only by the
// D3D12DebugLayer gated helper below when phase 1 fails and we re-run
// with --d3d-dbg for GBV diagnostic output. Keeping both phases in a
// single log entry preserves the "one log block per gtest case" contract
// so downstream log consumers don't need to know about the two-phase flow.
static void AppendPhaseTwoToLogFile(const std::string& zstFile, const DemoResult& result)
{
    if (g_testConfig.logFile.empty())
        return;

    std::ofstream log(g_testConfig.logFile, std::ios::app);
    if (!log)
    {
        std::cerr << "Warning: could not open --log-file '"
                  << g_testConfig.logFile << "' for phase-2 append.\n";
        return;
    }
    log << "--- PHASE 2 (--d3d-dbg diagnostic re-run) ---\n";
    log << "File: " << zstFile << "\n";
    log << "Exit code: " << result.exitCode << "\n";
    log << result.stdOut << "\n";
}

// D3D12DebugLayer gated-fallback metrics. Incremented from
// RunCorrectnessTestWithGbvFallback below. Destructor prints a one-line
// summary at program exit so operators can see at a glance how often the
// GBV re-run fired. File-static + destructor-print keeps the reporting
// self-contained here without touching main.cpp or the header.
namespace {
struct D3D12DebugLayerFallbackCounters
{
    std::atomic<int> totalRuns{0};
    std::atomic<int> fallbackFires{0};

    ~D3D12DebugLayerFallbackCounters()
    {
        const int runs = totalRuns.load();
        if (runs > 0)
        {
            // std::endl to force flush during static destruction — some
            // stdout paths swallow buffered output when the runtime is
            // tearing down.
            std::cout << "\n[D3D12DebugLayer] " << fallbackFires.load()
                      << " of " << runs
                      << " test(s) re-ran with --d3d-dbg for GBV diagnostics."
                      << std::endl;
        }
    }
};
D3D12DebugLayerFallbackCounters g_d3d12Metrics;
} // anonymous namespace

// Test runners

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

    ASSERT_EQ(result.exitCode, 0)
        << "Demo process returned non-zero exit code: " << result.exitCode << "\n"
        << "Command: " << result.commandLine
        << "  (stdout already printed above as [DEMO OUT])";
}

// Two-phase D3D12DebugLayer runner (per DirectStorage team spec, 2026-07-07):
// GBV validation is expensive, and most tests pass reference comparison
// without needing any GBV diagnostic output. This helper skips GBV for the
// passing majority and only re-runs with --d3d-dbg when the reference
// comparison actually fails, keeping GBV output focused on genuinely
// failing tests.
//
//   Phase 1: spawn demo with just [--chk-gpu] (reference comparison only).
//   If phase 1 exits 0 -> test passes, we're done. GBV cost skipped.
//   If phase 1 fails  -> spawn demo again with [--chk-gpu, --d3d-dbg] and
//                        append its output to the same log block with a
//                        marker separator. Test verdict stays based on
//                        phase 1's exit code; phase 2 is diagnostic only.
//
// The --force-gbv CLI flag bypasses the gate entirely and always runs
// with --d3d-dbg (matches pre-gating behavior). Useful when someone wants
// unconditional GBV output on all tests for a stress run.
//
// A file-static counter (g_d3d12Metrics) tracks how often phase 2 fires
// and prints a one-line summary at program exit — makes it easy to
// eyeball whether the gate is doing meaningful work.
static void RunCorrectnessTestWithGbvFallback(const std::string& zstFile)
{
    // --force-gbv: skip the gate entirely; behave like the original
    // RunCorrectnessTest with the full flag set. We don't touch the
    // g_d3d12Metrics counters in this path so the summary line at exit
    // only prints when the gate was actually active.
    if (g_testConfig.forceGbv)
    {
        RunCorrectnessTest(zstFile, {"--chk-gpu", "--d3d-dbg"});
        return;
    }

    ++g_d3d12Metrics.totalRuns;

    // Phase 1: reference comparison only (no --d3d-dbg).
    auto phase1Args = BuildCorrectnessArgs(zstFile, {"--chk-gpu"});
    auto phase1 = RunDemo(g_testConfig.demoPath, phase1Args, g_testConfig.timeoutSeconds);
    WriteToLogFile(zstFile, phase1);

    std::cout << "[DEMO CMD] " << phase1.commandLine << "\n";
    if (!phase1.stdOut.empty())
    {
        std::cout << "[DEMO OUT] " << phase1.stdOut << "\n";
    }

    // If phase 1 passed cleanly, we're done — skip GBV, test passes.
    const bool phase1Ok = !phase1.timedOut
                          && phase1.launchError.empty()
                          && phase1.exitCode == 0;
    if (phase1Ok)
    {
        return;
    }

    // Phase 2: re-run with --d3d-dbg for GBV diagnostic. Log gets the
    // marker separator + phase-2 output appended to the same test's block.
    ++g_d3d12Metrics.fallbackFires;
    std::cout << "[D3D12DebugLayer PHASE 2] Phase 1 (reference comparison) failed. "
              << "Re-running with --d3d-dbg for GBV diagnostic output.\n";

    auto phase2Args = BuildCorrectnessArgs(zstFile, {"--chk-gpu", "--d3d-dbg"});
    auto phase2 = RunDemo(g_testConfig.demoPath, phase2Args, g_testConfig.timeoutSeconds);
    AppendPhaseTwoToLogFile(zstFile, phase2);

    std::cout << "[DEMO CMD] " << phase2.commandLine << "\n";
    if (!phase2.stdOut.empty())
    {
        std::cout << "[DEMO OUT] " << phase2.stdOut << "\n";
    }

    // Assert on PHASE 1 exit code — phase 2 is diagnostic only. If phase 2
    // also failed (which it usually will since it's the same input), the
    // GBV output is already captured in the log for post-mortem analysis.
    ASSERT_FALSE(phase1.timedOut)
        << "Phase 1 (reference comparison) timed out after "
        << g_testConfig.timeoutSeconds << " seconds.\n"
        << "Command: " << phase1.commandLine;

    ASSERT_TRUE(phase1.launchError.empty())
        << "Failed to launch demo (phase 1): " << phase1.launchError << "\n"
        << "Command: " << phase1.commandLine;

    ASSERT_EQ(phase1.exitCode, 0)
        << "Phase 1 (reference comparison) returned non-zero exit code: "
        << phase1.exitCode << "\n"
        << "Command: " << phase1.commandLine
        << "\n  (phase 2 with --d3d-dbg was re-run — its GBV output is "
        << "in the log above under the PHASE 2 marker)";
}

// Run a performance scenario. Spawns zstdgpu_demo.exe with profiling flags and requests CSV output. Uses EXPECT (not ASSERT) to verify the demo executed successfully and produced CSV output.
// main() has already validated the demo path exists.
static void RunPerformanceTest(const std::string& zstFile, int profilingLevel)
{
    // Build CSV output path matching spec convention:
    //   prf-lvl 0 → results/throughput_<stem>.csv
    //   prf-lvl 2 → results/stages_<stem>.csv
    std::string stem = std::filesystem::path(zstFile).stem().string();
    std::string prefix = (profilingLevel == 0) ? "throughput" : "stages";
    std::filesystem::path resultsDir = std::filesystem::path(g_testConfig.logDir) / "results";
    if (!std::filesystem::exists(resultsDir))
    {
        std::filesystem::create_directories(resultsDir);
    }
    std::string csvPath = (resultsDir / (prefix + "_" + stem + ".csv")).string();

    auto args = BuildPerformanceArgs(zstFile, profilingLevel, g_testConfig.runCount, csvPath);
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

// --- Correctness tests ---

TEST_P(ZstdGpuDemoTests, SimulationCheck)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--chk-cpu", "--sim-gpu"});
}

TEST_P(ZstdGpuDemoTests, D3D12DebugLayer)
{
    RunCorrectnessTestWithGbvFallback(GetParam());
}

TEST_P(ZstdGpuDemoTests, ExternalMemory)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--ext-mem"});
}

TEST_P(ZstdGpuDemoTests, GraphicsQueue)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--d3d-gfx"});
}

// --- Performance tests ---

TEST_P(ZstdGpuDemoTests, OverallThroughput)
{
    RunPerformanceTest(GetParam(), 0);
}

TEST_P(ZstdGpuDemoTests, PerStageTiming)
{
    RunPerformanceTest(GetParam(), 2);
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
// profiling level, optionally writing per-run timing data to a CSV file.
std::vector<std::string> BuildPerformanceArgs(
    const std::string& zstFile,
    int profilingLevel,
    int runCount,
    const std::string& csvOutputPath)
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
    return args;
}

} // namespace
