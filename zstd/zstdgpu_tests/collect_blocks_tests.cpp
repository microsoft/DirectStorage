/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

/**
 *  Coverage for the host-side frame/block enumeration API:
 *  `zstdgpu_CountFramesAndBlocks` -> `zstdgpu_CollectFrames` -> `zstdgpu_CollectBlocks`.
 *
 *  `zstdgpu_CollectBlocks` had NO callers anywhere -- not in the library, the demo or the tests --
 *  so nothing exercised it. It is a documented public entry point, so it is pinned here rather
 *  than left to be discovered wrong by whoever first calls it.
 *
 *  The frames are built by hand, byte by byte, so the expected offsets are known independently of
 *  the parser. That is what makes this a test rather than a tautology: a corpus file would only
 *  tell us the parser agrees with itself.
 *
 *  Two format details are pinned deliberately, because both have caused real defects in this code
 *  base before:
 *
 *   - An `RLE_Block`'s `Block_Size` is its REGENERATED size, but its payload on disk is a SINGLE
 *     byte. Advancing the parse by `Block_Size` desynchronises the block walk and invents blocks
 *     that are not there.
 *   - For an RLE block `zstdgpu_OffsetAndSize::offs` carries the repeated SYMBOL, not an offset.
 *     Reading it as an offset is a silent out-of-bounds.
 */

#include <gtest/gtest.h>

#include <vector>

#include "zstdgpu_structs.h"
#include "zstdgpu.h"

namespace
{

/**
 *  A byte-level zstd frame builder. Everything is emitted literally so a test can state the
 *  offsets it expects without asking the parser where anything is.
 */
struct FrameBuilder
{
    std::vector<uint8_t> bytes;

    void u8(uint32_t v) { bytes.push_back((uint8_t)(v & 0xffu)); }

    /** Magic + a single-segment frame header carrying a 1-byte Frame_Content_Size. */
    uint32_t BeginFrame(uint32_t contentSize)
    {
        const uint32_t frameOffs = (uint32_t)bytes.size();

        // Magic 0xFD2FB528, little-endian on disk.
        u8(0x28); u8(0xB5); u8(0x2F); u8(0xFD);

        // Frame_Header_Descriptor: Frame_Content_Size_flag = 0, Single_Segment_flag = 1,
        // Content_Checksum_flag = 0, Dictionary_ID_flag = 0. With Single_Segment set, a
        // Frame_Content_Size_flag of 0 still means a 1-byte size field, and the Window_Descriptor
        // byte is absent.
        u8(0x20);

        ZSTDGPU_ASSERT(contentSize <= 255u);
        u8(contentSize);

        return frameOffs;
    }

    /** 3-byte Block_Header: bit 0 Last_Block, bits 2..1 Block_Type, bits 23..3 Block_Size. */
    void BlockHeader(uint32_t blockType, uint32_t blockSize, bool lastBlock)
    {
        const uint32_t hdr = (blockSize << 3u) | (blockType << 1u) | (lastBlock ? 1u : 0u);
        u8(hdr); u8(hdr >> 8u); u8(hdr >> 16u);
    }

    /** Returns the offset of the block's payload, which is what `CollectBlocks` reports. */
    uint32_t RawBlock(const char *payload, uint32_t size, bool lastBlock)
    {
        BlockHeader(0u, size, lastBlock);
        const uint32_t payloadOffs = (uint32_t)bytes.size();
        for (uint32_t i = 0; i < size; ++i)
        {
            u8((uint8_t)payload[i]);
        }
        return payloadOffs;
    }

    /** `repeatCount` is the regenerated size; exactly ONE byte reaches the stream. */
    void RleBlock(uint8_t symbol, uint32_t repeatCount, bool lastBlock)
    {
        BlockHeader(1u, repeatCount, lastBlock);
        u8(symbol);
    }

    /** The API requires the backing allocation to be a multiple of 4 bytes. */
    uint32_t PaddedSize() const { return ((uint32_t)bytes.size() + 3u) & ~3u; }

    std::vector<uint8_t> PaddedCopy() const
    {
        std::vector<uint8_t> out = bytes;
        out.resize(PaddedSize(), 0u);
        return out;
    }
};

} // namespace

/**
 *  One frame of RAW, RLE, RAW. Pins every reported offset and size against hand-computed values,
 *  and pins that the RLE payload advances the walk by one byte rather than by `Block_Size` --
 *  if it did not, the trailing RAW block would be missed or invented somewhere else entirely.
 */
TEST(CollectBlocks, ReportsEveryBlockOfASingleFrame)
{
    FrameBuilder fb;

    const uint32_t frameOffs = fb.BeginFrame(4u + 10u + 2u);
    const uint32_t raw0Offs  = fb.RawBlock("ABCD", 4u, false);
    fb.RleBlock((uint8_t)'Z', 10u, false);
    const uint32_t raw1Offs  = fb.RawBlock("EF", 2u, true);

    const uint32_t contentSize = (uint32_t)fb.bytes.size();
    const std::vector<uint8_t> blob = fb.PaddedCopy();

    EXPECT_EQ(0u, frameOffs);

    zstdgpu_CountFramesAndBlocksInfo counts = {};
    zstdgpu_CountFramesAndBlocks(&counts, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(1u, counts.frameCount);
    EXPECT_EQ(2u, counts.rawBlockCount);
    EXPECT_EQ(1u, counts.rleBlockCount);
    EXPECT_EQ(0u, counts.cmpBlockCount);
    EXPECT_EQ(16u, counts.frameByteCount);

    std::vector<zstdgpu_OffsetAndSize> frames(counts.frameCount);
    std::vector<zstdgpu_FrameInfo>     frameInfos(counts.frameCount);
    zstdgpu_CollectFrames(frames.data(), frameInfos.data(), counts.frameCount, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(frameOffs, frames[0].offs);
    EXPECT_EQ(contentSize, frames[0].size);
    EXPECT_EQ(16u, frameInfos[0].uncompSize);

    std::vector<zstdgpu_OffsetAndSize> raw(counts.rawBlockCount);
    std::vector<zstdgpu_OffsetAndSize> rle(counts.rleBlockCount);
    std::vector<zstdgpu_OffsetAndSize> cmp(1u); // never written; sized so the pointer is valid

    zstdgpu_CollectBlocks(raw.data(), rle.data(), cmp.data(), frames.data(), frameInfos.data(),
                          0u, counts.frameCount, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(raw0Offs, raw[0].offs);
    EXPECT_EQ(4u, raw[0].size);
    EXPECT_EQ(raw1Offs, raw[1].offs);
    EXPECT_EQ(2u, raw[1].size);

    // NB2 on the public API: for an RLE block `offs` is the repeated symbol, `size` the count.
    EXPECT_EQ((uint32_t)'Z', rle[0].offs);
    EXPECT_EQ(10u, rle[0].size);

    // With no compressed blocks the frame's regenerated size is fully accounted for by the
    // RAW and RLE blocks, so this is an exact identity rather than a bound.
    EXPECT_EQ(frameInfos[0].uncompSize, raw[0].size + raw[1].size + rle[0].size);

    // The reported RAW payloads must actually be the bytes we wrote.
    EXPECT_EQ('A', (char)blob[raw[0].offs]);
    EXPECT_EQ('D', (char)blob[raw[0].offs + 3u]);
    EXPECT_EQ('E', (char)blob[raw[1].offs]);
    EXPECT_EQ('F', (char)blob[raw[1].offs + 1u]);
}

/**
 *  Two frames back to back. `zstdgpu_CollectBlocks` is documented as safe to call per frame and
 *  writes through the per-type prefix sums in `zstdgpu_FrameInfo`, so this pins that a later
 *  frame lands after the earlier frame's blocks instead of overwriting them -- the part of an
 *  uncalled API most likely to be wrong.
 */
TEST(CollectBlocks, SecondFrameWritesAfterTheFirstFramesBlocks)
{
    FrameBuilder fb;

    fb.BeginFrame(4u + 10u + 2u);
    const uint32_t f0Raw0 = fb.RawBlock("ABCD", 4u, false);
    fb.RleBlock((uint8_t)'Z', 10u, false);
    const uint32_t f0Raw1 = fb.RawBlock("EF", 2u, true);

    const uint32_t frame1Offs = fb.BeginFrame(3u);
    const uint32_t f1Raw0     = fb.RawBlock("GHI", 3u, true);

    const uint32_t contentSize = (uint32_t)fb.bytes.size();
    const std::vector<uint8_t> blob = fb.PaddedCopy();

    zstdgpu_CountFramesAndBlocksInfo counts = {};
    zstdgpu_CountFramesAndBlocks(&counts, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(2u, counts.frameCount);
    EXPECT_EQ(3u, counts.rawBlockCount);
    EXPECT_EQ(1u, counts.rleBlockCount);
    EXPECT_EQ(0u, counts.cmpBlockCount);
    EXPECT_EQ(16u + 3u, counts.frameByteCount);

    std::vector<zstdgpu_OffsetAndSize> frames(counts.frameCount);
    std::vector<zstdgpu_FrameInfo>     frameInfos(counts.frameCount);
    zstdgpu_CollectFrames(frames.data(), frameInfos.data(), counts.frameCount, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(0u, frames[0].offs);
    EXPECT_EQ(frame1Offs, frames[1].offs);
    EXPECT_EQ(frame1Offs, frames[0].size);
    EXPECT_EQ(contentSize - frame1Offs, frames[1].size);

    // The per-type starts are running prefix sums over the preceding frames.
    EXPECT_EQ(0u, frameInfos[0].rawBlockStart);
    EXPECT_EQ(0u, frameInfos[0].rleBlockStart);
    EXPECT_EQ(2u, frameInfos[1].rawBlockStart);
    EXPECT_EQ(1u, frameInfos[1].rleBlockStart);

    std::vector<zstdgpu_OffsetAndSize> raw(counts.rawBlockCount);
    std::vector<zstdgpu_OffsetAndSize> rle(counts.rleBlockCount);
    std::vector<zstdgpu_OffsetAndSize> cmp(1u);

    // Deliberately collected out of order: the API states each frame is independent.
    zstdgpu_CollectBlocks(raw.data(), rle.data(), cmp.data(), frames.data(), frameInfos.data(),
                          1u, counts.frameCount, blob.data(), (uint32_t)blob.size(), contentSize);
    zstdgpu_CollectBlocks(raw.data(), rle.data(), cmp.data(), frames.data(), frameInfos.data(),
                          0u, counts.frameCount, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(f0Raw0, raw[0].offs);
    EXPECT_EQ(4u, raw[0].size);
    EXPECT_EQ(f0Raw1, raw[1].offs);
    EXPECT_EQ(2u, raw[1].size);
    EXPECT_EQ(f1Raw0, raw[2].offs);
    EXPECT_EQ(3u, raw[2].size);

    EXPECT_EQ((uint32_t)'Z', rle[0].offs);
    EXPECT_EQ(10u, rle[0].size);

    // Offsets are absolute within the memory block, not relative to the owning frame.
    EXPECT_GT(raw[2].offs, frames[1].offs);
    EXPECT_EQ('G', (char)blob[raw[2].offs]);
}

/**
 *  A frame whose every block is RLE. `Block_Size` is the regenerated size, so a walk that advanced
 *  by it instead of by the single payload byte would run far past the frame; here it would run
 *  past the end of the blob. Pinned separately because this exact confusion has bitten twice.
 */
TEST(CollectBlocks, RleBlockPayloadIsOneByteNotBlockSize)
{
    FrameBuilder fb;

    fb.BeginFrame(200u + 55u);
    fb.RleBlock((uint8_t)'a', 200u, false);
    fb.RleBlock((uint8_t)'b', 55u, true);

    const uint32_t contentSize = (uint32_t)fb.bytes.size();

    // Header (6) + two blocks of (3 header + 1 payload). If the payload were `Block_Size` bytes
    // this frame would occupy 261 bytes instead of 14.
    EXPECT_EQ(6u + 4u + 4u, contentSize);

    const std::vector<uint8_t> blob = fb.PaddedCopy();

    zstdgpu_CountFramesAndBlocksInfo counts = {};
    zstdgpu_CountFramesAndBlocks(&counts, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(1u, counts.frameCount);
    EXPECT_EQ(0u, counts.rawBlockCount);
    EXPECT_EQ(2u, counts.rleBlockCount);
    EXPECT_EQ(255u, counts.frameByteCount);

    std::vector<zstdgpu_OffsetAndSize> frames(counts.frameCount);
    std::vector<zstdgpu_FrameInfo>     frameInfos(counts.frameCount);
    zstdgpu_CollectFrames(frames.data(), frameInfos.data(), counts.frameCount, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ(contentSize, frames[0].size);

    std::vector<zstdgpu_OffsetAndSize> raw(1u);
    std::vector<zstdgpu_OffsetAndSize> rle(counts.rleBlockCount);
    std::vector<zstdgpu_OffsetAndSize> cmp(1u);

    zstdgpu_CollectBlocks(raw.data(), rle.data(), cmp.data(), frames.data(), frameInfos.data(),
                          0u, counts.frameCount, blob.data(), (uint32_t)blob.size(), contentSize);

    EXPECT_EQ((uint32_t)'a', rle[0].offs);
    EXPECT_EQ(200u, rle[0].size);
    EXPECT_EQ((uint32_t)'b', rle[1].offs);
    EXPECT_EQ(55u, rle[1].size);

    EXPECT_EQ(frameInfos[0].uncompSize, rle[0].size + rle[1].size);
}
