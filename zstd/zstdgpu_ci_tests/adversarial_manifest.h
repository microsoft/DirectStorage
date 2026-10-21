/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Adversarial manifest: maps known-adversarial fuzz files to their expected
// demo exit code. When a matching file is exercised by the wrapper, the
// correctness test expects exit_code == manifest value rather than success.
//
// The manifest also carries an optional "scenario_skips" list. Each entry
// matches a scenario by name and the machine by --gpu-name, causing matching
// scenarios to GTEST_SKIP before the demo runs.
//
// Perf tests do NOT consult the per-file entries; they skip fuzz content by
// path (see IsFuzzContent in zstdgpu_ci_tests.cpp). Scenario skips apply to any
// matching scenario, correctness or performance.
//
// If the manifest is not loaded (--adversarial-manifest not passed OR file
// missing), the wrapper falls back to legacy behavior: every file expected to
// succeed and no scenario skips. This is intentionally additive — no test
// starts failing just because the manifest isn't wired up yet.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

// One entry describes an expected outcome for a set of files matched by
// path_glob (relative to --content-path).
struct AdversarialEntry
{
    std::string pathGlob;                          // e.g. "public/fuzz/generated/gen_bitflip_off0[0-3]_*.zst"
    std::string reason;                            // Human-readable description of what's wrong with the file(s) (JSON key "reason")
    int expectedExitCode = 1;                      // Expected demo process exit code (always 1 today; field exists for future flexibility)
};

// One scenario skip. Matches a GTest scenario by name and the machine by
// --gpu-name; matching scenarios GTEST_SKIP before the demo is spawned.
struct ScenarioSkip
{
    std::string scenarioGlob;                      // Glob over the GTest scenario name (JSON key "scenario_glob")
    std::string gpuNameGlob;                       // Glob over the --gpu-name value (JSON key "gpu_name_glob")
    std::string pathGlob;                          // Optional glob over the file path relative to --content-path (JSON key "path_glob"); empty matches every file in the scenario
    std::string reason;                            // Explanation shown in the skip message (JSON key "reason")
    std::string trackingBug;                       // Reference for the skip, e.g. a bug ID (JSON key "tracking_bug")
};

// Loads and matches the manifest. Loaded once at startup, then read-only from
// test threads. Thread-safe for concurrent Match() calls after LoadFromFile()
// returns.
class AdversarialManifest
{
public:
    // Loads entries from a JSON file on disk. Returns false if the file is
    // missing, malformed, or has an unsupported schema_version. On failure,
    // writes a diagnostic message to errorOut and leaves the manifest empty
    // (Loaded() returns false; Match() always returns nullptr).
    bool LoadFromFile(const std::filesystem::path& jsonPath, std::string& errorOut);

    // Returns the matching entry for the given file, or nullptr if no match.
    // Matching is on the path relative to contentPath, normalized to forward
    // slashes. If contentPath is empty, matches on the leaf filename only.
    // O(N) scan of entries; N is expected to be small (~12 entries today).
    const AdversarialEntry* Match(const std::string& zstFullPath,
                                  const std::filesystem::path& contentPath) const;

    // Returns the first scenario_skip whose scenario_glob matches scenarioName,
    // gpu_name_glob matches gpuName, and (when set) path_glob matches the file
    // path relative to contentPath. Returns nullptr on no match, when the
    // manifest isn't loaded, or when scenarioName/gpuName is empty. A skip with
    // an empty path_glob matches every file in the scenario.
    const ScenarioSkip* MatchScenarioSkip(const std::string& scenarioName,
                                          const std::string& gpuName,
                                          const std::string& zstFullPath,
                                          const std::filesystem::path& contentPath) const;

    size_t Size() const { return m_entries.size(); }
    size_t ScenarioSkipCount() const { return m_scenarioSkips.size(); }
    bool Loaded() const { return m_loaded; }
    const std::filesystem::path& SourcePath() const { return m_sourcePath; }

    // Startup coverage self-check. Returns how many of `files` match at least
    // one manifest entry. The wrapper uses this to fail loud when a loaded
    // manifest matches nothing (an inert manifest is otherwise indistinguishable
    // from a working one and silently disables the adversarial-rejection checks).
    size_t CountCoverage(const std::vector<std::string>& files,
                         const std::filesystem::path& contentPath) const;

private:
    std::vector<AdversarialEntry> m_entries;
    std::vector<ScenarioSkip> m_scenarioSkips;
    std::filesystem::path m_sourcePath;
    bool m_loaded = false;
};

// One perf-manifest entry: a glob (relative to --content-path) that selects
// files for a perf scenario, plus a human-readable reason the files qualify.
struct PerfEntry
{
    std::string pathGlob;                          // Glob over the file path relative to --content-path (JSON key "path_glob")
    std::string reason;                            // Human-readable note on why these files qualify (JSON key "reason")
};

// Perf manifest: selects the content each perf scenario runs over. Carries two
// independent glob sets, "latency" and "throughput"; they may (and eventually
// will) select different files even though today they glob to the same tiles.
// This replaces the old hardcoded "path contains 64KB" selection so the perf
// corpus is data-driven, authored beside the content rather than in code.
//
// Loaded once at startup, then read-only from test threads. Uses the same
// anchored glob syntax ('*', '[a-b]') as the adversarial manifest, matched
// against each discovered file's path relative to --content-path.
class PerfManifest
{
public:
    // Loads the "latency" and "throughput" glob arrays from a JSON file. Returns
    // false (with a diagnostic in errorOut) if the file is missing, malformed, or
    // has an unsupported schema_version, leaving the manifest empty/unloaded.
    bool LoadFromFile(const std::filesystem::path& jsonPath, std::string& errorOut);

    // Returns the discovered files matching at least one glob for the given
    // scenario ("latency" or "throughput"). Files are returned in discovery
    // order and deduplicated (a file selected by several globs appears once);
    // an unknown scenario name selects nothing. Order/dedupe matter: the demo
    // concatenates the result into one stream, so duplicates or reordering would
    // double-count frames and destabilize the pooled P50/median.
    std::vector<std::string> SelectFiles(const std::string& scenario,
                                         const std::vector<std::string>& discoveredFiles,
                                         const std::filesystem::path& contentPath) const;

    // Startup coverage self-check: how many of `files` match at least one glob in
    // either scenario. The wrapper fails loud when a loaded manifest matches
    // nothing (almost always a content-path / glob-prefix mismatch).
    size_t CountCoverage(const std::vector<std::string>& files,
                         const std::filesystem::path& contentPath) const;

    size_t LatencyCount() const { return m_latency.size(); }
    size_t ThroughputCount() const { return m_throughput.size(); }
    size_t Size() const { return m_latency.size() + m_throughput.size(); }
    bool Loaded() const { return m_loaded; }
    const std::filesystem::path& SourcePath() const { return m_sourcePath; }

private:
    std::vector<PerfEntry> m_latency;
    std::vector<PerfEntry> m_throughput;
    std::filesystem::path m_sourcePath;
    bool m_loaded = false;
};
