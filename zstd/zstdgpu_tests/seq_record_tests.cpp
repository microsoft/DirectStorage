/**
 * Copyright (c) Microsoft. All rights reserved.
 * This code is licensed under the MIT License (MIT).
 * THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
 * ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
 * IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
 * PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
 */

/**
 *  Round-trip tests for the packed sequence record.
 *
 *  A decoded sequence is stored in the shared literal/sequence arena as two dwords, with the match
 *  length split across both and biased by the minimum match length. Every field is at its exact
 *  width -- 17 + 17 + 30 = 64 bits, with nothing spare -- so an off-by-one mask or a missed bias
 *  silently corrupts decoded output rather than failing loudly. These tests pin the layout.
 */

#include <gtest/gtest.h>

#include "zstdgpu_structs.h"

namespace
{

constexpr uint32_t kMaxLLen = kzstdgpu_SeqMaskLLen;                      // 131071
constexpr uint32_t kMaxMLen = kzstdgpu_SeqMaskLLen + kzstdgpu_MinMatchLength; // 131074
constexpr uint32_t kMaxOffs = kzstdgpu_SeqMaskOffs;                      // 0x3fffffff

// Literal lengths, match lengths and offsets spanning both extremes of every field, plus the
// encoded repeat-offset forms, whose bit 29 marker is why the offset field needs 30 bits.
const uint32_t kLLens[] = { 0u, 1u, 2u, 3u, 255u, 65535u, 131070u, kMaxLLen };
const uint32_t kMLens[] = { kzstdgpu_MinMatchLength, 4u, 5u, 258u, 32768u, 32770u, 98303u, 98306u, 131073u, kMaxMLen };
const uint32_t kOffss[] = {
    0u, 1u, 2u, 3u, 65535u,
    (1u << 29u) - 1u,           // largest resolved offset the window assert admits
    0x27ffffffu | (1u << 27u),  // encoded repeat type 1
    0x27ffffffu | (2u << 27u),  // encoded repeat type 2
    0x27ffffffu | (3u << 27u),  // encoded repeat type 3
    kMaxOffs
};

} // namespace

TEST(SeqRecordPacking, RecordIsTwoDwords)
{
    EXPECT_EQ(2u, kzstdgpu_SeqRecordDwordCount);
    EXPECT_EQ(8u, zstdgpu_ArenaBytesForCounts(0u, 1u) - zstdgpu_ArenaBytesForCounts(0u, 0u));
}

TEST(SeqRecordPacking, RoundTripsEveryFieldCombination)
{
    for (uint32_t llen : kLLens)
        for (uint32_t mlen : kMLens)
            for (uint32_t offs : kOffss)
            {
                const uint32_t d0 = zstdgpu_SeqPackDword0(llen, mlen);
                const uint32_t d1 = zstdgpu_SeqPackDword1(mlen, offs);

                EXPECT_EQ(llen, zstdgpu_SeqUnpackLLen(d0)) << "llen=" << llen << " mlen=" << mlen;
                EXPECT_EQ(mlen, zstdgpu_SeqUnpackMLen(d0, d1)) << "llen=" << llen << " mlen=" << mlen;
                EXPECT_EQ(offs, zstdgpu_SeqUnpackOffs(d1)) << "offs=" << offs << " mlen=" << mlen;
            }
}

/**
 *  `[Finalise Sequence Offsets]` rewrites the offset in place. That dword also carries the match
 *  length's high two bits, so the rewrite must be masked -- whole-dword arithmetic there would
 *  corrupt the match length only for sequences whose offset resolution borrows, which would pass
 *  small tests and fail sporadically on real content.
 */
TEST(SeqRecordPacking, InPlaceOffsetRewritePreservesMatchLength)
{
    for (uint32_t mlen : kMLens)
        for (uint32_t offs : kOffss)
        {
            const uint32_t llen = 12345u;
            const uint32_t d0 = zstdgpu_SeqPackDword0(llen, mlen);
            uint32_t d1 = zstdgpu_SeqPackDword1(mlen, offs);

            const uint32_t resolved = offs & ((1u << 29u) - 1u);
            d1 = zstdgpu_SeqReplaceOffs(d1, resolved);

            EXPECT_EQ(resolved, zstdgpu_SeqUnpackOffs(d1));
            EXPECT_EQ(mlen, zstdgpu_SeqUnpackMLen(d0, d1)) << "mlen corrupted by offset rewrite";
            EXPECT_EQ(llen, zstdgpu_SeqUnpackLLen(d0));
        }
}

/** The exact encoded -> resolved -> minus-three sequence performed on the GPU and in the reference. */
TEST(SeqRecordPacking, ResolveThenUnbiasKeepsOtherFields)
{
    for (uint32_t mlen : kMLens)
    {
        const uint32_t llen = 777u;
        const uint32_t d0 = zstdgpu_SeqPackDword0(llen, mlen);
        uint32_t d1 = zstdgpu_SeqPackDword1(mlen, 0x27ffffffu | (3u << 27u));

        const uint32_t resolved = 4096u;
        d1 = zstdgpu_SeqReplaceOffs(d1, resolved);
        d1 = zstdgpu_SeqReplaceOffs(d1, zstdgpu_SeqUnpackOffs(d1) - kzstdgpu_MinMatchLength);

        EXPECT_EQ(resolved - kzstdgpu_MinMatchLength, zstdgpu_SeqUnpackOffs(d1));
        EXPECT_EQ(mlen, zstdgpu_SeqUnpackMLen(d0, d1));
        EXPECT_EQ(llen, zstdgpu_SeqUnpackLLen(d0));
    }
}

/** Records are addressed downwards from the arena top and must not overlap. */
TEST(SeqRecordPacking, RecordsAreContiguousAndDescending)
{
    const uint32_t top = 1024u;
    for (uint32_t i = 0; i < 16u; ++i)
    {
        EXPECT_EQ(top - (i + 1u) * 2u, zstdgpu_SeqRecordDword0(top, i));
        EXPECT_EQ(zstdgpu_SeqRecordDword0(top, i) + 1u, zstdgpu_SeqRecordDword1(top, i));
        // Record i sits immediately above record i + 1.
        EXPECT_EQ(zstdgpu_SeqRecordDword1(top, i + 1u) + 1u, zstdgpu_SeqRecordDword0(top, i));
    }
}


/**
 *  The arena bound saturates rather than wrapping, and the scratch ceiling rejects the saturated
 *  value.
 *
 *  `(R/3) * size` exceeds a uint32 well below the 4 GiB decompressed limit. A wrapped value would
 *  produce an arena far SMALLER than the content provably needs, the joint occupancy predicate
 *  would fire, the predicated work would be silently skipped, and the decode would return wrong
 *  output with exit code 0 -- the exact failure mode this scheme exists to eliminate. Saturation
 *  turns that into an unsatisfiable requirement that every allocating path now checks.
 *
 *  These live in a unit test because the adversarial corpus no longer reaches this path: the file
 *  that used to trip the ceiling (`public\fuzz\fail\fail_09_bad_blocksize.zst`) now sizes under it
 *  since the record narrowed to 8 bytes. The behaviour is pure host arithmetic, so it does not need
 *  a GPU or a corpus to pin.
 */

// Largest decompressed size whose arena bound still fits a uint32, found by bisection rather than
// hard-coded, so narrowing the record cannot leave the expected threshold stale.
static uint32_t LargestNonSaturatingSize()
{
    uint32_t lo = 1u;
    uint32_t hi = 0xffffffffu;
    while (lo < hi)
    {
        const uint32_t mid = lo + ((hi - lo + 1u) / 2u);
        if (zstdgpu_DecompressedSizeToArenaByteCount(mid) != 0xffffffffu)
        {
            lo = mid;
        }
        else
        {
            hi = mid - 1u;
        }
    }
    return lo;
}

TEST(ArenaBound, IsExactAndMonotonicBelowSaturation)
{
    const uint32_t last = LargestNonSaturatingSize();
    ASSERT_GT(last, 0u);

    // The documented factor: ceil(8 * size / 3), dword-aligned, plus guard and one spare dword.
    for (uint32_t size : { 1u, 2u, 3u, 4u, 5u, 1024u, 1048576u, 1073741824u })
    {
        if (size > last)
        {
            continue;
        }

        const uint64_t recordBytes = (uint64_t)kzstdgpu_SeqRecordDwordCount * sizeof(uint32_t);
        const uint64_t bound      = (recordBytes * size + (kzstdgpu_MinMatchLength - 1u)) / kzstdgpu_MinMatchLength;
        const uint64_t expected   = ((bound + 3u) & ~(uint64_t)3u) + kzstdgpu_ArenaGuardBytes + sizeof(uint32_t);

        EXPECT_EQ(expected, (uint64_t)zstdgpu_DecompressedSizeToArenaByteCount(size)) << "size " << size;
    }

    // Never rounds down: the bound must cover the worst case exactly, not approximately.
    EXPECT_GE((uint64_t)zstdgpu_DecompressedSizeToArenaByteCount(3u),
              (uint64_t)kzstdgpu_SeqRecordDwordCount * sizeof(uint32_t));

    // Monotonic up to the saturation point, so a larger request can never ask for a smaller arena.
    uint32_t prev = zstdgpu_DecompressedSizeToArenaByteCount(1u);
    for (uint32_t size = 2u; size < 4096u; ++size)
    {
        const uint32_t cur = zstdgpu_DecompressedSizeToArenaByteCount(size);
        EXPECT_GE(cur, prev) << "size " << size;
        prev = cur;
    }
}

TEST(ArenaBound, SaturatesInsteadOfWrapping)
{
    const uint32_t last = LargestNonSaturatingSize();

    // Just below the threshold the value is a real bound; at and above it, it is the saturated
    // sentinel. A wrapping implementation would instead return a small number here.
    EXPECT_NE(0xffffffffu, zstdgpu_DecompressedSizeToArenaByteCount(last));
    EXPECT_EQ(0xffffffffu, zstdgpu_DecompressedSizeToArenaByteCount(last + 1u));

    for (uint32_t size : { last + 1u, 2000000000u, 3000000000u, 4000000000u, 0xfffffffeu, 0xffffffffu })
    {
        EXPECT_EQ(0xffffffffu, zstdgpu_DecompressedSizeToArenaByteCount(size)) << "size " << size;
    }

    // The sentinel is what makes the request rejectable. It sits one byte BELOW the ceiling, and
    // only reaches it once rounded up to the 64 KiB heap alignment -- which is precisely why the
    // ceiling comparison is inclusive. Pin both halves of that reasoning, since an exclusive test
    // would leave rejection resting on the other stage buffers rather than on the check.
    const uint64_t saturated = zstdgpu_DecompressedSizeToArenaByteCount(last + 1u);
    const uint64_t aligned   = (saturated + 0xffffull) & ~0xffffull;

    EXPECT_LT(saturated, kzstdgpu_MaxScratchHeapByteCount);
    EXPECT_GE(aligned, kzstdgpu_MaxScratchHeapByteCount);
}
