/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 *
 * Advanced Technology Group (ATG)
 * Author(s):   Pavel Martishevsky (pamartis@microsoft.com)
 */

#pragma once

struct zstdgpu_OffsetAndSize
{
    uint32_t offs;
    uint32_t size;
};

struct zstdgpu_FrameInfo
{
    uint64_t windowSize;
    uint64_t uncompSize;
    uint32_t dictionary;

    uint32_t rawBlockStart;
    uint32_t rleBlockStart;
    uint32_t cmpBlockStart;

    uint32_t rawBlockBytesStart;
    uint32_t rleBlockBytesStart;
};

// -----------------------------------------------------------------------------
// Per-frame decompression status.
//
// The GPU frame parser writes one entry per input frame into the caller-supplied
// status buffer. Each entry is formatted as an HRESULT:
//   * kzstdgpu_FrameStatus_Success (S_OK, 0) when the frame header is well-formed
//     and the frame was accepted for decompression.
//   * a failure HRESULT otherwise. Failure codes set the Severity (0x8...) and
//     Customer (0x2...) bits and use a zstdgpu-specific facility (0x7A) so they
//     never collide with system-defined HRESULTs. Test them with the usual
//     FAILED()/SUCCEEDED() semantics; the low 16 bits carry the reason code.
//
// NB: these constants are shared verbatim between the C++ library and the HLSL
//     shaders, so the values written on the GPU match what the caller reads back.
// -----------------------------------------------------------------------------
static const uint32_t kzstdgpu_FrameStatus_Success               = 0x00000000u; // S_OK
static const uint32_t kzstdgpu_FrameStatus_NotZstdFrame          = 0xA07A0001u; // input is not a zstd frame (bad / absent magic)
static const uint32_t kzstdgpu_FrameStatus_ReservedBitSet        = 0xA07A0002u; // frame header reserved bit set (spec violation)
static const uint32_t kzstdgpu_FrameStatus_DictionaryUnsupported = 0xA07A0003u; // frame requires a dictionary (unsupported by the GPU decoder)
static const uint32_t kzstdgpu_FrameStatus_WindowTooLarge        = 0xA07A0004u; // window size exceeds the decoder maximum
static const uint32_t kzstdgpu_FrameStatus_MissingContentSize    = 0xA07A0005u; // frame declares no content size (the GPU decoder needs it to size output)
static const uint32_t kzstdgpu_FrameStatus_RequestTooLarge       = 0xA07A0006u; // request output exceeds kzstdgpu_MaxRequestUncompressedByteSize (see below)

// Largest total decompressed size of one request: the sum of Frame_Content_Size over
// all accepted frames. Block output offsets are prefix-summed with 30-bit lookback
// values, so larger requests cannot be decoded correctly. When a request exceeds this
// limit, every frame that would otherwise be accepted reports
// kzstdgpu_FrameStatus_RequestTooLarge and its output is undefined; split such
// requests into smaller batches.
static const uint32_t kzstdgpu_MaxRequestUncompressedByteSize    = 0x3FFFFFFFu;

// -----------------------------------------------------------------------------
// Scratch-exhaustion status.
//
// Unlike the codes above, these do NOT describe anything wrong with the frame.
// They report that the scratch memory sized for the batch was too small, so the
// decode was abandoned. The condition is detected per DISPATCH, from batch-global
// counters, and cannot be attributed to an individual frame -- so every frame in
// the batch that had not already failed for its own reason is marked, because no
// frame's output can be trusted once predicated work has been skipped.
//
// Without these, exceeding the scratch bound skips the predicated work SILENTLY
// and the decode returns wrong output with a success status. That is the exact
// failure mode the per-frame status buffer exists to make impossible.
// -----------------------------------------------------------------------------
static const uint32_t kzstdgpu_FrameStatus_BlockCountExceeded    = 0xA07A0007u; // more blocks than the scratch was sized for (host pre-scan disagreed with the GPU parse, or block counts were estimated)
static const uint32_t kzstdgpu_FrameStatus_ScratchExceeded       = 0xA07A0008u; // literal/sequence arena too small for the decoded content
