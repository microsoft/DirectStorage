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
