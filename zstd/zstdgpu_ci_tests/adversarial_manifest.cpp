/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

// AdversarialManifest implementation.
//
// This file contains a minimal, purpose-built JSON parser scoped to the schema
// documented in adversarial_manifest.json — NOT a general-purpose JSON parser.
// It handles the subset our manifest uses: top-level object with a "notes"
// string field and an "entries" array of objects. Each entry has string /
// integer / boolean / string-array fields. Basic backslash escapes in strings
// (\", \\, \/, \n, \r, \t) are supported. Unicode escapes and nested objects
// are NOT supported (the manifest schema doesn't use them). Any deviation from
// the expected shape produces a clear error via the errorOut parameter and
// leaves the manifest empty rather than throwing.
//
// A hand-rolled parser avoids adding a third-party JSON dependency for what is
// a small, controlled data file. If the schema grows to need general JSON
// features, switch to a real library (nlohmann/json or similar).

#include "adversarial_manifest.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>

namespace
{

// Minimal JSON parser scoped to the manifest schema.

struct Parser
{
    std::string_view text;
    size_t pos = 0;
    std::string error;

    // Advances past whitespace (and JSON does not allow comments, so nothing
    // else to skip). Returns true if there's more input to consume.
    bool SkipWs()
    {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                     text[pos] == '\n' || text[pos] == '\r'))
        {
            ++pos;
        }
        return pos < text.size();
    }

    // Reports a parse error at the current position with line/column context.
    // Only the first error is retained; subsequent calls are no-ops. Callers
    // check Failed() after any consume operation.
    void Fail(const std::string& msg)
    {
        if (!error.empty())
            return;
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos && i < text.size(); ++i)
        {
            if (text[i] == '\n') { ++line; col = 1; } else { ++col; }
        }
        std::ostringstream oss;
        oss << "JSON parse error at line " << line << " col " << col << ": " << msg;
        error = oss.str();
    }

    bool Failed() const { return !error.empty(); }

    // Consumes an exact character; fails if not present. Callers must SkipWs()
    // first when whitespace is allowed.
    bool Expect(char c)
    {
        if (pos >= text.size() || text[pos] != c)
        {
            std::string got = (pos < text.size()) ? std::string(1, text[pos]) : "<eof>";
            Fail(std::string("expected '") + c + "' got '" + got + "'");
            return false;
        }
        ++pos;
        return true;
    }

    // Reads a JSON string (must start at the current position after SkipWs).
    // Handles the basic backslash escapes; unicode escapes are rejected.
    std::string ReadString()
    {
        std::string out;
        if (!SkipWs()) { Fail("expected string, got eof"); return out; }
        if (!Expect('"')) return out;
        while (pos < text.size() && text[pos] != '"')
        {
            char c = text[pos++];
            if (c == '\\')
            {
                if (pos >= text.size()) { Fail("unterminated escape"); return out; }
                char e = text[pos++];
                switch (e)
                {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                // Unicode escapes not supported — manifest is ASCII-only.
                default:
                    Fail(std::string("unsupported escape '\\") + e + "'");
                    return out;
                }
            }
            else
            {
                out += c;
            }
        }
        if (!Expect('"')) return out;
        return out;
    }

    // Reads a JSON integer. Rejects floats — the manifest only uses integer
    // fields (schema_version, expected_exit_code).
    int ReadInt()
    {
        if (!SkipWs()) { Fail("expected int, got eof"); return 0; }
        size_t start = pos;
        if (pos < text.size() && text[pos] == '-') ++pos;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
        if (start == pos || (start + 1 == pos && text[start] == '-'))
        {
            Fail("expected integer");
            return 0;
        }
        if (pos < text.size() && (text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E'))
        {
            Fail("floating-point numbers not supported in manifest");
            return 0;
        }
        std::string num(text.substr(start, pos - start));
        try { return std::stoi(num); }
        catch (...) { Fail("integer out of range"); return 0; }
    }

    // Reads a JSON boolean.
    bool ReadBool()
    {
        if (!SkipWs()) { Fail("expected bool, got eof"); return false; }
        if (text.compare(pos, 4, "true") == 0) { pos += 4; return true; }
        if (text.compare(pos, 5, "false") == 0) { pos += 5; return false; }
        Fail("expected 'true' or 'false'");
        return false;
    }

    // Reads a JSON array of strings (used for expected_stderr_any_of).
    std::vector<std::string> ReadStringArray()
    {
        std::vector<std::string> out;
        if (!SkipWs()) { Fail("expected '[', got eof"); return out; }
        if (!Expect('[')) return out;
        if (!SkipWs()) { Fail("unexpected eof in array"); return out; }
        if (pos < text.size() && text[pos] == ']') { ++pos; return out; }
        while (true)
        {
            std::string s = ReadString();
            if (Failed()) return out;
            out.push_back(std::move(s));
            if (!SkipWs()) { Fail("unexpected eof in array"); return out; }
            if (text[pos] == ',') { ++pos; continue; }
            if (text[pos] == ']') { ++pos; return out; }
            Fail(std::string("expected ',' or ']' got '") + text[pos] + "'");
            return out;
        }
    }

    // Consumes a JSON value of unknown type and discards it. Used to
    // gracefully ignore unknown fields (forward compatibility — a manifest
    // written by a newer schema version that adds fields should still parse
    // in older wrappers as long as the required fields are still present).
    void SkipValue()
    {
        if (!SkipWs()) { Fail("expected value, got eof"); return; }
        char c = text[pos];
        if (c == '"') { ReadString(); return; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) { ReadInt(); return; }
        if (c == 't' || c == 'f') { ReadBool(); return; }
        if (c == 'n')
        {
            if (text.compare(pos, 4, "null") == 0) { pos += 4; return; }
        }
        if (c == '[')
        {
            ++pos;
            if (!SkipWs()) { Fail("unexpected eof in array"); return; }
            if (text[pos] == ']') { ++pos; return; }
            while (true)
            {
                SkipValue();
                if (Failed()) return;
                if (!SkipWs()) { Fail("unexpected eof in array"); return; }
                if (text[pos] == ',') { ++pos; continue; }
                if (text[pos] == ']') { ++pos; return; }
                Fail("expected ',' or ']'");
                return;
            }
        }
        if (c == '{')
        {
            ++pos;
            if (!SkipWs()) { Fail("unexpected eof in object"); return; }
            if (text[pos] == '}') { ++pos; return; }
            while (true)
            {
                ReadString();       // key
                if (Failed()) return;
                if (!SkipWs() || !Expect(':')) return;
                SkipValue();        // value
                if (Failed()) return;
                if (!SkipWs()) { Fail("unexpected eof in object"); return; }
                if (text[pos] == ',') { ++pos; continue; }
                if (text[pos] == '}') { ++pos; return; }
                Fail("expected ',' or '}'");
                return;
            }
        }
        Fail(std::string("unexpected character '") + c + "' when reading value");
    }
};

// Parses one manifest entry (a JSON object). Consumes fields we recognize and
// silently skips unknown ones for forward compatibility.
static bool ParseEntry(Parser& p, AdversarialEntry& entry, std::string& errorOut)
{
    if (!p.SkipWs() || !p.Expect('{')) { errorOut = p.error; return false; }

    bool haveGlob = false;
    while (true)
    {
        if (!p.SkipWs()) { p.Fail("unexpected eof in entry"); errorOut = p.error; return false; }
        if (p.text[p.pos] == '}') { ++p.pos; break; }

        std::string key = p.ReadString();
        if (p.Failed()) { errorOut = p.error; return false; }
        if (!p.SkipWs() || !p.Expect(':')) { errorOut = p.error; return false; }

        if (key == "path_glob")
        {
            entry.pathGlob = p.ReadString();
            haveGlob = true;
        }
        else if (key == "corruption")
        {
            entry.corruption = p.ReadString();
        }
        else if (key == "expected_exit_code")
        {
            entry.expectedExitCode = p.ReadInt();
        }
        else if (key == "expected_stderr_any_of")
        {
            entry.expectedStderrAnyOf = p.ReadStringArray();
        }
        else if (key == "skip_perf")
        {
            entry.skipPerf = p.ReadBool();
        }
        else
        {
            // Unknown field — skip it so newer manifest schemas can add fields
            // without breaking older wrappers.
            p.SkipValue();
        }
        if (p.Failed()) { errorOut = p.error; return false; }

        if (!p.SkipWs()) { p.Fail("unexpected eof in entry"); errorOut = p.error; return false; }
        if (p.text[p.pos] == ',') { ++p.pos; continue; }
        if (p.text[p.pos] == '}') { ++p.pos; break; }
        p.Fail("expected ',' or '}' in entry"); errorOut = p.error; return false;
    }
    if (!haveGlob) { errorOut = "entry missing required 'path_glob' field"; return false; }
    return true;
}

// Glob matcher supporting '*' wildcards and '[a-b]' character ranges.
// Case-sensitive. Not general-purpose — no '?', no '**', no negation.
// O(N*M) worst case but N and M are small (paths are typically <256 chars).
static bool GlobMatch(std::string_view pattern, std::string_view path)
{
    size_t pi = 0, si = 0;
    size_t starPi = std::string_view::npos, starSi = 0;

    while (si < path.size())
    {
        if (pi < pattern.size() && pattern[pi] == '*')
        {
            starPi = pi++;
            starSi = si;
        }
        else if (pi < pattern.size() && pattern[pi] == '[')
        {
            // Bracket expression: [abc], [a-z], [0-9] etc. No negation.
            size_t bracketEnd = pattern.find(']', pi + 1);
            if (bracketEnd == std::string_view::npos)
                return false;  // malformed pattern — treat as no match
            bool matched = false;
            for (size_t k = pi + 1; k < bracketEnd; ++k)
            {
                if (k + 2 < bracketEnd && pattern[k + 1] == '-')
                {
                    char lo = pattern[k], hi = pattern[k + 2];
                    if (path[si] >= lo && path[si] <= hi) matched = true;
                    k += 2;
                }
                else if (pattern[k] == path[si])
                {
                    matched = true;
                }
            }
            if (matched)
            {
                pi = bracketEnd + 1;
                ++si;
            }
            else if (starPi != std::string_view::npos)
            {
                pi = starPi + 1;
                si = ++starSi;
            }
            else
            {
                return false;
            }
        }
        else if (pi < pattern.size() && pattern[pi] == path[si])
        {
            ++pi;
            ++si;
        }
        else if (starPi != std::string_view::npos)
        {
            pi = starPi + 1;
            si = ++starSi;
        }
        else
        {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

// Normalizes a path for glob matching: relative to contentPath (if provided)
// AND with all backslashes converted to forward slashes. Used because Windows
// paths in the test log mix separators freely
// (e.g. C:/agent/data/zstd_content\public\fuzz\...).
static std::string RelativeAndNormalize(const std::string& fullPath,
                                        const std::filesystem::path& contentPath)
{
    std::filesystem::path rel;
    if (!contentPath.empty())
    {
        std::error_code ec;
        rel = std::filesystem::relative(std::filesystem::path(fullPath), contentPath, ec);
        if (ec || rel.empty() || rel.string().rfind("..", 0) == 0)
        {
            rel = std::filesystem::path(fullPath).filename();
        }
    }
    else
    {
        rel = std::filesystem::path(fullPath).filename();
    }
    std::string s = rel.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

} // namespace

bool AdversarialManifest::LoadFromFile(const std::filesystem::path& jsonPath, std::string& errorOut)
{
    m_entries.clear();
    m_loaded = false;
    m_sourcePath = jsonPath;

    std::ifstream file(jsonPath, std::ios::binary);
    if (!file)
    {
        errorOut = "could not open manifest file: " + jsonPath.string();
        return false;
    }
    std::ostringstream buf;
    buf << file.rdbuf();
    std::string text = buf.str();

    Parser p{ std::string_view(text), 0, {} };

    if (!p.SkipWs() || !p.Expect('{')) { errorOut = p.error; return false; }

    while (true)
    {
        if (!p.SkipWs()) { p.Fail("unexpected eof at top level"); errorOut = p.error; return false; }
        if (p.text[p.pos] == '}') { ++p.pos; break; }

        std::string key = p.ReadString();
        if (p.Failed()) { errorOut = p.error; return false; }
        if (!p.SkipWs() || !p.Expect(':')) { errorOut = p.error; return false; }

        if (key == "$schema_version")
        {
            int v = p.ReadInt();
            if (p.Failed()) { errorOut = p.error; return false; }
            if (v != 1)
            {
                errorOut = "unsupported $schema_version " + std::to_string(v) + " (this wrapper supports version 1)";
                return false;
            }
        }
        else if (key == "entries")
        {
            if (!p.SkipWs() || !p.Expect('[')) { errorOut = p.error; return false; }
            if (!p.SkipWs()) { p.Fail("unexpected eof in entries"); errorOut = p.error; return false; }
            if (p.text[p.pos] == ']') { ++p.pos; }
            else
            {
                while (true)
                {
                    AdversarialEntry entry;
                    if (!ParseEntry(p, entry, errorOut)) return false;
                    m_entries.push_back(std::move(entry));
                    if (!p.SkipWs()) { p.Fail("unexpected eof in entries"); errorOut = p.error; return false; }
                    if (p.text[p.pos] == ',') { ++p.pos; continue; }
                    if (p.text[p.pos] == ']') { ++p.pos; break; }
                    p.Fail("expected ',' or ']' in entries"); errorOut = p.error; return false;
                }
            }
        }
        else
        {
            // Unknown top-level field — skip for forward compat.
            p.SkipValue();
            if (p.Failed()) { errorOut = p.error; return false; }
        }

        if (!p.SkipWs()) { p.Fail("unexpected eof at top level"); errorOut = p.error; return false; }
        if (p.text[p.pos] == ',') { ++p.pos; continue; }
        if (p.text[p.pos] == '}') { ++p.pos; break; }
        p.Fail("expected ',' or '}' at top level"); errorOut = p.error; return false;
    }

    m_loaded = true;
    return true;
}

const AdversarialEntry* AdversarialManifest::Match(const std::string& zstFullPath,
                                                   const std::filesystem::path& contentPath) const
{
    if (!m_loaded) return nullptr;
    const std::string rel = RelativeAndNormalize(zstFullPath, contentPath);
    for (const auto& e : m_entries)
    {
        if (GlobMatch(e.pathGlob, rel))
            return &e;
    }
    return nullptr;
}
