/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// Adversarial manifest: maps known-adversarial fuzz files to their expected
// demo failure signatures. When a matching file is exercised by the wrapper,
// the assertion criteria change:
//   - Perf tests (OverallThroughput / PerStageTiming) are SKIPPED entirely
//     (perf can't produce timing on files that won't decode).
//   - Correctness tests still run, but expect exit_code == manifest value AND
//     at least one of the expected_stderr_any_of substrings appears in
//     captured demo output.
//
// If the manifest is not loaded (--adversarial-manifest not passed OR file
// missing), the wrapper falls back to legacy behavior: every file expected to
// succeed. This is intentionally additive — no test starts failing just
// because the manifest isn't wired up yet.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

// One entry describes an expected outcome for a set of files matched by
// path_glob (relative to --content-path). See adversarial_manifest.json for
// the shipping schema and field semantics.
struct AdversarialEntry
{
    std::string pathGlob;                          // e.g. "public/fuzz/generated/gen_bitflip_off0[0-3]_*.zst"
    std::string corruption;                        // Human-readable description of what's wrong with the file(s)
    int expectedExitCode = 1;                      // Expected demo process exit code (always 1 today; field exists for future flexibility)
    std::vector<std::string> expectedStderrAnyOf;  // Any ONE of these substrings appearing in stdout+stderr = correct rejection
    bool skipPerf = true;                          // If true, perf tests skip this file entirely
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

    size_t Size() const { return m_entries.size(); }
    bool Loaded() const { return m_loaded; }
    const std::filesystem::path& SourcePath() const { return m_sourcePath; }

private:
    std::vector<AdversarialEntry> m_entries;
    std::filesystem::path m_sourcePath;
    bool m_loaded = false;
};
