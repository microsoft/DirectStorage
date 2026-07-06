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
// Contains a single parameterized test suite (ZstdGpuDemoTests) instantiated
// once per .zst file found in the content directory. Each file gets 6 test
// scenarios:
//
// Correctness tests (4 scenarios per file):
//   - SimulationCheck:  Software GPU simulation (--sim-gpu) with CPU+GPU validation
//   - D3D12DebugLayer:  Hardware GPU with D3D12 debug layer (--d3d-dbg)
//   - ExternalMemory:   External memory mode (--ext-mem)
//   - GraphicsQueue:    Graphics queue instead of compute (--d3d-gfx)
//
// Performance tests (2 scenarios per file):
//   - OverallThroughput: Profiling level 0 — CSV: results/throughput_<stem>.csv
//   - PerStageTiming:    Profiling level 2 — CSV: results/stages_<stem>.csv
//   Performance tests use EXPECT (not ASSERT) — they fail on infrastructure
//   errors but do not check performance values against thresholds.
//
// If no .zst files are found, GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST
// prevents GTest from reporting an error — zero tests run, exit code 0.
//
// The demo runner at the bottom of this file spawns zstdgpu_demo.exe as a child
// process using Win32 CreateProcess with anonymous pipes. A background thread
// drains stdout to avoid pipe-buffer deadlocks. If the process exceeds the
// configured timeout, it is terminated. This avoids any D3D12/GPU dependency
// in the test binary itself — all GPU work happens inside the demo process.

// NOTES: IF THE PATH IS EMPTY, FAIL THE TEST 

#include "zstdgpu_ci_tests.h"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <Windows.h>

// Helpers

// Returns the list of .zst files to parameterize over.
// GTest evaluates this lazily when the test suite is instantiated (after main has parsed CLI args and set TestConfig), so contentPath is available here.
static std::vector<std::string> GetTestFiles()
{
    const auto& config = GetTestConfig();
    return DiscoverZstFiles(config.contentPath);
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
    const auto& config = GetTestConfig();
    std::filesystem::path full(info.param);
    std::filesystem::path rel;
    if (!config.contentPath.empty())
    {
        std::error_code ec;
        rel = std::filesystem::relative(full, config.contentPath, ec);
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
    const auto& config = GetTestConfig();
    if (config.logFile.empty())
        return;

    std::ofstream log(config.logFile, std::ios::app);
    auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
    log << "=== " << testInfo->test_suite_name() << "." << testInfo->name() << " ===\n";
    log << "File: " << zstFile << "\n";
    log << "Exit code: " << result.exitCode << "\n";
    log << result.stdOut << "\n";
}

// Test runners

// Run a correctness scenario. Spawns zstdgpu_demo.exe with the given .zst file and scenario flags, then asserts exit code == 0.
// Failures include the full command line and demo stdout for diagnostic output.
static void RunCorrectnessTest(const std::string& zstFile, const std::vector<std::string>& scenarioFlags)
{
    const auto& config = GetTestConfig();

    if (config.demoPath.empty())
    {
        GTEST_SKIP() << "zstdgpu_demo.exe not found. Set --demo-path.";
    }

    auto args = BuildCorrectnessArgs(zstFile, scenarioFlags);
    auto result = RunDemo(config.demoPath, args, config.timeoutSeconds);

    // Write to log file before assertions so logs are captured even if an ASSERT aborts early.
    WriteToLogFile(zstFile, result);

    // Log the output regardless of pass/fail.
    std::cout << "[DEMO CMD] " << result.commandLine << "\n";
    if (!result.stdOut.empty())
    {
        std::cout << "[DEMO OUT] " << result.stdOut << "\n";
    }

    ASSERT_FALSE(result.timedOut)
        << "Demo process timed out after " << config.timeoutSeconds << " seconds.\n"
        << "Command: " << result.commandLine;

    ASSERT_TRUE(result.launchError.empty())
        << "Failed to launch demo: " << result.launchError << "\n"
        << "Command: " << result.commandLine;

    ASSERT_EQ(result.exitCode, 0)
        << "Demo process returned non-zero exit code: " << result.exitCode << "\n"
        << "Command: " << result.commandLine << "\n"
        << "Output:\n"
        << result.stdOut;
}

// Run a performance scenario. Spawns zstdgpu_demo.exe with profiling flags and requests CSV output. Uses EXPECT (not ASSERT) to verify the demo executed successfully and produced CSV output.
static void RunPerformanceTest(const std::string& zstFile, int profilingLevel)
{
    const auto& config = GetTestConfig();

    if (config.demoPath.empty())
    {
        GTEST_SKIP() << "zstdgpu_demo.exe not found. Set --demo-path.";
    }

    // Build CSV output path matching spec convention:
    //   prf-lvl 0 → results/throughput_<stem>.csv
    //   prf-lvl 2 → results/stages_<stem>.csv
    std::string stem = std::filesystem::path(zstFile).stem().string();
    std::string prefix = (profilingLevel == 0) ? "throughput" : "stages";
    std::filesystem::path resultsDir = std::filesystem::path(config.logDir) / "results";
    if (!std::filesystem::exists(resultsDir))
    {
        std::filesystem::create_directories(resultsDir);
    }
    std::string csvPath = (resultsDir / (prefix + "_" + stem + ".csv")).string();

    auto args = BuildPerformanceArgs(zstFile, profilingLevel, config.runCount, csvPath);
    auto result = RunDemo(config.demoPath, args, config.timeoutSeconds);

    // Write to log file before assertions so logs are captured even if a check fails.
    WriteToLogFile(zstFile, result);

    std::cout << "[DEMO CMD] " << result.commandLine << "\n";
    if (!result.stdOut.empty())
    {
        std::cout << "[DEMO OUT] " << result.stdOut << "\n";
    }

    EXPECT_FALSE(result.timedOut)
        << "Demo process timed out after " << config.timeoutSeconds << " seconds.\n"
        << "Command: " << result.commandLine;

    EXPECT_TRUE(result.launchError.empty())
        << "Failed to launch demo: " << result.launchError << "\n"
        << "Command: " << result.commandLine;

    EXPECT_EQ(result.exitCode, 0)
        << "Demo process returned non-zero exit code: " << result.exitCode << "\n"
        << "Command: " << result.commandLine << "\n"
        << "Output:\n"
        << result.stdOut;

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

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(ZstdGpuDemoTests);

// --- Correctness tests ---

TEST_P(ZstdGpuDemoTests, SimulationCheck)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--chk-cpu", "--sim-gpu"});
}

TEST_P(ZstdGpuDemoTests, D3D12DebugLayer)
{
    RunCorrectnessTest(GetParam(), {"--chk-gpu", "--d3d-dbg"});
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
