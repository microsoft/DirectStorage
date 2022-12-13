/*
 * SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define USE_WAVE_INTRINSICS // Enable on machines with WaveOps support (SM 6.0 and above)
//#define USE_WAVE_MATCH      // Enable use of the WaveMatch() intrinsics (requires shader  model 6.5)
//#define SIMD_WIDTH <width>  // SIMD width of the machine (required when USE_WAVE_INTRINSICS)

#define NUM_BITSTREAMS 32          // GDeflate interleaves 32 compressed bitstreams
#define NUM_THREADS NUM_BITSTREAMS // Thread blocks are sized to match that

#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)
#define IN_REGISTER_DECODER
#define SINGLE_WAVE
#endif

// Raw input and output buffers
ByteAddressBuffer input : register(t0);
RWByteAddressBuffer control : register(u0);
RWByteAddressBuffer output : register(u1);
RWByteAddressBuffer scratch : register(u2);
// Control buffer format: numStreams, [stream0, stream0 inPos, stream0 outPos], ...
// Scratch buffer format: stream0 tileIdx, stream1 tileIdx, ...

uint ControlStreamOffset(uint streamIndex)
{
    return 4 + streamIndex * 8;
}

uint ControlStreamInOffset(uint streamIndex)
{
    return ControlStreamOffset(streamIndex);
}

uint ScratchStreamTileIndexOffset(uint streamIndex)
{
    return streamIndex * 4;
}

#include "tilestream.hlsl"

uint ControlStreamOutOffset(uint streamIndex)
{
    return ControlStreamInOffset(streamIndex) + 4;
}

inline uint32_t mask(uint32_t n)
{
    return (1u << n) - 1u;
}

inline uint32_t ltMask(uint tid)
{
    return mask(tid);
}

inline uint32_t extract(uint32_t data, uint32_t pos, uint32_t n, uint32_t base = 0)
{
    return ((data >> pos) & mask(n)) + base;
}

groupshared uint32_t g_tmp[NUM_THREADS];

#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)

inline uint32_t vote(bool p, uint tid)
{
    return (uint32_t)WaveActiveBallot(p);
}

inline uint32_t shuffle(uint32_t value, uint idx, uint tid)
{
    return WaveReadLaneAt(value, idx);
}

inline uint32_t broadcast(uint32_t value, uint idx, uint tid)
{
    return WaveReadLaneAt(value, idx);
}

inline bool all(bool p, uint tid)
{
    return (uint32_t)WaveActiveAllTrue(p);
}

uint32_t scan(uint32_t value, uint tid)
{
    return WavePrefixSum(value);
}

#else

groupshared uint32_t g_tmp1[NUM_THREADS];
groupshared uint32_t g_tmp2[NUM_THREADS];
groupshared uint32_t g_tmp3[NUM_THREADS];

inline uint32_t vote(bool p, uint tid)
{
#ifdef USE_WAVE_INTRINSICS
    g_tmp1[tid / SIMD_WIDTH] = (uint32_t)WaveActiveBallot(p);
    GroupMemoryBarrierWithGroupSync();
    uint32_t ballot = g_tmp1[0];
    [unroll] for (uint i = 1; i < NUM_THREADS / SIMD_WIDTH; i++) ballot |= g_tmp1[i] << (SIMD_WIDTH * i);
    GroupMemoryBarrierWithGroupSync();
    return ballot;
#else
    g_tmp1[tid] = p ? (1u << tid) : 0;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint i = NUM_THREADS / 2; i > 0; i >>= 1)
    {
        if (tid < i)
            g_tmp1[tid] |= g_tmp1[tid + i];
        GroupMemoryBarrierWithGroupSync();
    }
    uint ballot = g_tmp1[0];
    GroupMemoryBarrierWithGroupSync();
    return ballot;
#endif
}

inline uint32_t shuffle(uint32_t value, uint idx, uint tid)
{
    g_tmp1[tid] = value;
    GroupMemoryBarrierWithGroupSync();
    uint32_t res = g_tmp1[idx];
    GroupMemoryBarrierWithGroupSync();
    return res;
}

inline uint32_t broadcast(uint32_t value, uint idx, uint tid)
{
    GroupMemoryBarrierWithGroupSync();
    if (tid == idx)
        g_tmp1[0] = value;
    GroupMemoryBarrierWithGroupSync();
    return g_tmp1[0];
}

bool all(bool p, uint tid)
{
    return vote(p, tid) == (1 << NUM_THREADS) - 1;
}

// Prefix sum
inline uint32_t scan(uint32_t value, uint tid)
{
#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH == 16)
    uint32_t sum = WavePrefixSum(value);
    if (tid == SIMD_WIDTH - 1)
        g_tmp1[0] = sum + value;
    GroupMemoryBarrierWithGroupSync();
    if (tid >= SIMD_WIDTH)
        sum += g_tmp1[0];
    return sum;
#else
    uint32_t sum = value;

    [unroll] for (uint i = 1; i < NUM_THREADS; i *= 2) sum += tid >= i ? shuffle(sum, tid - i, tid) : 0;

    return sum - value;
#endif
}

#endif

// Segmented prefix sum
uint32_t scan16(uint32_t value, uint tid)
{
#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH == 16)
    return WavePrefixSum(value) + value;
#else
    [unroll] for (uint i = 1; i < NUM_THREADS / 2; i *= 2)
    {
        value += (tid & 15) >= i ? shuffle(value, tid - i, tid) : 0;
    }
#endif
    return value;
}

uint32_t match(uint32_t value, uint tid)
{
#if defined(USE_WAVE_MATCH) && defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)
    return (uint32_t)WaveMatch(value);
#else
    uint32_t mask = 0;

#if defined(USE_WAVE_INTRINSICS) && (SIMD_WIDTH >= NUM_THREADS)
    [unroll] for (uint i = 0; i < NUM_THREADS; i++)
    {
        mask |= (WaveReadLaneAt(value, i) == value ? 1u : 0) << i;
    }
#else
    g_tmp1[tid] = value;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint i = 0; i < NUM_THREADS; i++)
    {
        GroupMemoryBarrierWithGroupSync();
        mask |= g_tmp1[i] == value ? (1u << i) : 0;
    }
    GroupMemoryBarrierWithGroupSync();
#endif

    return mask;
#endif
}

inline uint32_t ReadOutputByte(uint32_t offset)
{
    uint32_t offsetMod4 = offset & 3;
    offset -= offsetMod4;
    uint32_t shift = offsetMod4 << 3;
    return (output.Load(offset) >> shift) & 0xff;
}

inline void StoreByte(uint32_t offset, uint32_t data)
{
    uint32_t offsetMod4 = offset & 3;
    offset -= offsetMod4;
    uint32_t shift = offsetMod4 << 3;
    output.InterlockedOr(offset, (data & 0xff) << shift);
}

struct BitReader
{
    static const uint kWidth = NUM_BITSTREAMS;

    uint base, cnt;
    uint64_t buf;

    // Reset bit reader - assume base pointer is word-aligned
    void init(uint i, uint tid)
    {
        cnt = kWidth;
        buf = (uint64_t)input.Load(i + tid * 4);
        base = i + kWidth * 4;
    }

    // Refill bit buffer if needed and advance shared base pointer
    void refill(bool p, uint tid)
    {
        p &= cnt < kWidth;
        uint32_t ballot = vote(p, tid);
        uint offset = countbits(ballot & ltMask(tid)) * 4;
        if (p)
        {
            buf |= (uint64_t)input.Load(base + offset) << cnt;
            cnt += kWidth;
        }
        base += countbits(ballot) * 4;
    }

    // Remove n bits from the bit buffer
    void eat(uint n, uint tid, bool p)
    {
        if (p)
        {
            buf >>= n;
            cnt -= n;
        }
        refill(p, tid);
    }

    // Return n bits from the bit buffer without changing reader state (up to 32 bits at a time)
    uint32_t peek(uint n)
    {
        return (uint32_t)buf & mask(n);
    }

    uint32_t peek()
    {
        return (uint32_t)buf;
    }

    // Return n bits from the bit buffer and remove them
    uint32_t read(uint n, uint tid, bool p)
    {
        uint32_t bits = p ? (uint32_t)buf & mask(n) : 0;

        eat(n, tid, p);
        return bits;
    }
};

// Scratch storage for code length array
groupshared struct Scratch
{
    uint32_t data[64];
    void clear(uint tid)
    {
        data[tid] = data[tid + NUM_THREADS] = 0;
    } // Clear first 64 words

    // Returns a nibble of data
    uint32_t get4b(uint i)
    {
        return (data[i / 8] >> (4 * (i % 8))) & 15;
    }
} g_buf;

void set4b(uint32_t nibbles, uint32_t n, uint32_t i)
{
    // Expand nibbles
    nibbles |= (nibbles << 4);
    nibbles |= (nibbles << 8);
    nibbles |= (nibbles << 16);
    nibbles &= ~((int)0xf0000000 >> (28 - n * 4));

    uint32_t base = i / 8;
    uint32_t shift = i % 8;

    InterlockedOr(g_buf.data[base], nibbles << (shift * 4));
    if (shift + n > 8)
        InterlockedOr(g_buf.data[base + 1], nibbles >> ((8 - shift) * 4));
}

// Symbol table
groupshared struct SymbolTable
{
    static const uint32_t kMaxSymbols = 288 + 32;
    static const uint32_t kDistanceCodesBase = 288;

    uint symbols[kMaxSymbols]; // Can be stored in uint16_t

    // Scatter symbols according to in-register lengths and their corresponding offsets
    uint32_t scatter(uint sym, uint len, uint offset, uint tid)
    {
        uint32_t mask = match(len, tid);
        if (len != 0)
            symbols[offset + countbits(mask & ltMask(tid))] = sym;
        return mask;
    }

    // Init symbol table from an array of code lengths in shared memory
    // hlit is at least 257
    // Assumes offsets contain literal/length offsets in lower numbered threads and distance code offsets in
    // higher-numbered threads
    void init(uint hlit, uint offsets, uint tid)
    {
        if (tid != 15 && tid != 31)
            g_tmp[tid + 1] = offsets;

#if SIMD_WIDTH < NUM_THREADS
        GroupMemoryBarrierWithGroupSync();
#endif
        // 8 unconditional iterations, fully unroll
        [unroll] for (uint32_t i = 0; i < 256 / NUM_THREADS; i++)
        {
            uint32_t sym = i * NUM_THREADS + tid;
            uint32_t len = g_buf.get4b(sym);
            uint32_t match = scatter(sym, len, g_tmp[len], tid);
            if (tid == firstbitlow(match))
                g_tmp[len] += countbits(match);
#if SIMD_WIDTH < NUM_THREADS
            GroupMemoryBarrierWithGroupSync();
#endif
        }

        // Bounds check on the last iteration for literals
        uint32_t sym = 8 * NUM_THREADS + tid;
        uint32_t len = sym < hlit ? g_buf.get4b(sym) : 0;
        scatter(sym, len, g_tmp[len], tid);

        // Scatter distance codes (assumes source array is padded with 0)
        len = g_buf.get4b(tid + hlit);
        scatter(tid, len, kDistanceCodesBase + g_tmp[16 + len], tid);
    }

} g_lut;

#ifdef IN_REGISTER_DECODER
#define LVAL(name, index) name
#define RVAL(name, index) WaveReadLaneAt(name, (index))
#else
#define LVAL(name, index) name[index]
#define RVAL(name, index) name[index]
#endif

#define DECLARE(type, name, size) type LVAL(name, size)

// Maintains state of a pair of decoders (in higher and lower numbered threads)
struct DecoderPair
{
    static const uint kMaxCodeLen = 15;

    // Aligned so that both can be indexed with (len-1)
    DECLARE(uint32_t, baseCodes, NUM_THREADS); // Base codes for each code length + sentinel code
    DECLARE(uint, offsets, NUM_THREADS);       // Offsets into the symbol table

    uint offset(uint i)
    {
        return RVAL(offsets, i);
    }

    // Build two decoders in parallel
    void init(uint counts, uint maxlen, uint tid)
    { // counts contain a histogram of code lengths

        // Calculate offsets into the symbol table
        LVAL(offsets, tid) = scan16(counts, tid);

        // Calculate base codes
#ifndef IN_REGISTER_DECODER
        g_tmp1[tid] = counts;
        GroupMemoryBarrierWithGroupSync();
#endif

        uint32_t baseCode = 0;
        [unroll] for (uint32_t i = 1; i < maxlen; i++)
        {
            uint lane = tid & 15;
#ifndef IN_REGISTER_DECODER
            uint count = g_tmp1[(tid & 16) + i];
#else
            uint count = shuffle(counts, (tid & 16) + i, tid);
#endif
            if (lane >= i)
                baseCode += count << (lane - i);
        }

        // Left-align and fill in sentinel values
        uint lane = tid & 15;
        uint tmp = baseCode << (32 - lane);
        LVAL(baseCodes, tid) = tmp < baseCode || (lane >= maxlen) ? 0xffffffff : tmp;
    }

    // Maps a code to its length (base selects decoder)
    uint len4code(uint32_t code, uint base = 0)
    {
        uint len = 1;
        if (code >= RVAL(baseCodes, 7 + base))
            len = 8;
        if (code >= RVAL(baseCodes, len + 3 + base))
            len += 4;
        if (code >= RVAL(baseCodes, len + 1 + base))
            len += 2;
        if (code >= RVAL(baseCodes, len + base))
            len += 1;
        return len;
    }

    // Maps a code and its length to a symbol id (base selects decoder)
    uint id4code(uint32_t code, uint len, uint base = 0)
    {
        uint i = len + base - 1;
        return RVAL(offsets, i) + ((code - RVAL(baseCodes, i)) >> (32 - len));
    }

    // Decode a huffman-coded symbol
    uint decode(uint32_t bits, out uint len, bool isdist = false)
    {
        uint32_t code = reversebits(bits);
        len = len4code(code, isdist ? 16 : 0);
        return g_lut.symbols[id4code(code, len, isdist ? 16 : 0) + (isdist ? 288 : 0)];
    }
};

// Declare global decoder if not using in-register decoders
#ifndef IN_REGISTER_DECODER
groupshared DecoderPair dec;
#endif

// Calculate a histogram from in-register code lengths (each thread maps to a length)
uint32_t GetHistogram(uint32_t cnt, uint32_t len, uint32_t maxlen, uint tid)
{
    g_tmp[tid] = 0;
#if SIMD_WIDTH < NUM_THREADS
    GroupMemoryBarrierWithGroupSync();
#endif
    if (len != 0 && tid < cnt)
        InterlockedAdd(g_tmp[len], 1);
#if SIMD_WIDTH < NUM_THREADS
    GroupMemoryBarrierWithGroupSync();
#endif
    return g_tmp[tid & 15];
}

// Read and sort code length code lengths
uint ReadLenCodes(inout BitReader br, uint hclen, uint tid)
{
    static const uint lane4id[32] = {3, 17, 15, 13, 11, 9, 7, 5, 4, 6, 8, 10, 12, 14, 16, 18,
                                     0,  1,  2,  0,  0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0};

    uint len = br.read(3, tid, tid < hclen); // Read reordered code length code lengths in
                                             // the first hclen threads (up to 19)
    len = shuffle(len, lane4id[tid], tid);   // Restore original order
    len &= tid < 19 ? 0xf : 0;               // Zero-out the garbage
    return len;
}

// Update histograms
// (distance codes are histogrammed in the higher numbered threads, literal/length codes - in lower numbered threads)
void UpdateHistograms(uint32_t len, int i, int n, int hlit)
{
    uint32_t cnt = max(min(hlit - i, n), 0);
    if (cnt != 0)
        InterlockedAdd(g_tmp[len], cnt);

    cnt = max(min(i + n - hlit, n), 0);
    if (cnt != 0)
        InterlockedAdd(g_tmp[16 + len], cnt);
}

// Unpack code lengths and create a histogram of lengths.
// Returns a histogram of literal/length code lengths in lower numbered threads,
// and a histogram of distance code lengths in higher numbered threads.
uint UnpackCodeLengths(inout BitReader br, uint hlit, uint hdist, uint hclen, uint tid, uint dst)
{
    uint len = ReadLenCodes(br, hclen, tid);

#ifdef IN_REGISTER_DECODER
    DecoderPair dec;
#endif

    // Init decoder
    uint cnts = GetHistogram(19, len, 7, tid);
    dec.init(cnts, 7, tid);
    g_lut.scatter(tid, len, dec.offset(len - 1), tid);

    uint32_t count = hlit + hdist;
    uint32_t baseOffset = 0;
    uint32_t lastlen = ~0;

    // Clear codelens array (4 bit lengths)
    g_buf.clear(tid);
    g_tmp[tid] = 0;

#if SIMD_WIDTH < NUM_THREADS
    GroupMemoryBarrierWithGroupSync();
#endif

    // Decode code length codes and expand into a shared memory array
    do
    {
        uint len;
        uint32_t bits = br.peek(7 + 7);
        uint sym = dec.decode(bits, len);
        uint idx = sym <= 15 ? 0 : (sym - 15);

        static const uint base[4] = {1, 3, 3, 11};
        static const uint xlen[4] = {0, 2, 3, 7};

        uint n = base[idx] + ((bits >> len) & mask(xlen[idx]));

        // Scan back to find the nearest lane which contains a valid symbol
        uint lane = firstbithigh(vote(sym != 16, tid) & ltMask(tid));

        uint codelen = sym;
        if (sym > 16)
            codelen = 0;
        uint prevlen = shuffle(codelen, lane, tid);

        if (sym == 16)
            codelen = lane == ~0 ? lastlen : prevlen;

        lastlen = broadcast(codelen, NUM_THREADS - 1, tid);
#if SIMD_WIDTH < NUM_THREADS
        GroupMemoryBarrierWithGroupSync();
#endif
        baseOffset = scan(n, tid) + baseOffset;

        if (baseOffset < count && codelen != 0)
        {
            UpdateHistograms(codelen, baseOffset, n, hlit);
            set4b(codelen, n, baseOffset);
        }

        br.eat(len + xlen[idx], tid, baseOffset < count);

        baseOffset = broadcast(baseOffset + n, NUM_THREADS - 1, tid);
#if SIMD_WIDTH < NUM_THREADS
        GroupMemoryBarrierWithGroupSync();
#endif

    } while (all(baseOffset < count));

#if SIMD_WIDTH < NUM_THREADS
    GroupMemoryBarrierWithGroupSync(); // Needed for HW with SIMD width < 16
#endif

    return g_tmp[tid];
}

void WriteOutput(uint32_t dst, uint32_t offset, uint32_t dist, uint32_t length, uint32_t byte, bool iscopy, uint tid)
{
    dst += offset;
    // Output literals
    if (!iscopy && length != 0)
        StoreByte(dst, byte);

    // Fill in copy destinations
    uint32_t mask = vote(iscopy, tid);
    uint32_t msk = mask;

    while (mask)
    {
        uint32_t lane = firstbitlow(mask);

#if !defined(USE_WAVE_INTRINSICS) || (SIMD_WIDTH < NUM_THREADS)
        g_tmp1[tid] = dist;
        g_tmp2[tid] = length;
        g_tmp3[tid] = dst;

        GroupMemoryBarrierWithGroupSync();

        uint32_t off = g_tmp1[lane];
        uint32_t len = g_tmp2[lane];
        uint32_t output = g_tmp3[lane];
#else
        uint32_t off = broadcast(dist, lane, tid);
        uint32_t len = broadcast(length, lane, tid);
        uint32_t output = broadcast(dst, lane, tid);
#endif

        // Copy using all threads in the wave
        for (uint32_t i = tid; i < len; i += NUM_THREADS)
        {
            uint32_t data = ReadOutputByte(output + i % off - off);
            StoreByte(i + output, data);
        }

        mask &= mask - 1;
    }
}

// Translate a symbol to its value
uint TranslateSymbol(inout BitReader br, int sym, uint len, uint32_t bits, bool isdist, uint tid, bool p)
{
    // Tables for distance/length decoding DEFLATE64
    static const uint32_t baseDist[] = 
    {    1,    2,    3,     4,     5,     7,     9,    13,
        17,   25,   33,    49,    65,    97,   129,   193,
       257,  385,  513,   769,  1025,  1537,  2049,  3073,
      4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153 };

    static const uint32_t baseLength[] = 
    {  0,   3,   4,   5,   6,  7,  8,  9,
      10,  11,  13,  15,  17, 19, 23, 27,
      31,  35,  43,  51,  59, 67, 83, 99,
     115, 131, 163, 195, 227,  3,  0 };

    static const uint32_t extraDist[] = 
    { 0,  0,  0,  0,  1,  1,  2,  2,
      3,  3,  4,  4,  5,  5,  6,  6,
      7,  7,  8,  8,  9,  9, 10, 10,
     11, 11, 12, 12, 13, 13, 14, 14 };

    static const uint32_t extraLength[] = 
    {0, 0, 0, 0, 0,  0, 0, 0,
     0, 1, 1, 1, 1,  2, 2, 2,
     2, 3, 3, 3, 3,  4, 4, 4,
     4, 5, 5, 5, 5, 16, 0 };

    uint32_t base = isdist ? baseDist[sym] : (sym >= 256 ? baseLength[sym - 256] : 1);
    uint32_t n = isdist ? extraDist[sym] : (sym >= 256 ? extraLength[sym - 256] : 0);

    br.eat(len + n, tid, isdist || p);

    return base + ((bits >> len) & mask(n));
}

// Assumes code lengths have been stored in the shared memory array
uint CompressedBlock(inout BitReader br, uint hlit, uint counts, uint dst, uint tid)
{
    // Init decoders
#ifdef IN_REGISTER_DECODER
    DecoderPair dec;
#endif

    dec.init(counts, 15, tid);
    g_lut.init(hlit, RVAL(dec.offsets, tid), tid);

    // Initial round - no copy processing
    uint32_t len;
    uint32_t sym = dec.decode(br.peek(15 + 16), len, false);

    uint32_t eob = vote(sym == 256, tid);
    bool oob = (eob & ltMask(tid)) != 0;

    // Translate current symbol
    uint32_t value = TranslateSymbol(br, sym, len, br.peek(), false, tid, !oob);

    // Compute output pointers for the current round
    uint32_t length = oob ? 0 : value;
    uint32_t offset = scan(length, tid);

    // Copy predicate for the next round
    bool iscopy = sym > 256;
    uint32_t byte = sym;

    // Translate all symbols in the block
    while (eob == 0)
    {
        sym = dec.decode(br.peek(15 + 16), len, iscopy);

        // Set predicates based on the current symbol
        eob = vote(sym == 256, tid);    // end of block symbol
        oob = (eob & ltMask(tid)) != 0; // true in threads which looked at symbols past the end of the block

        // Translate current symbol
        value = TranslateSymbol(br, sym, len, br.peek(), iscopy, tid, !oob);

        WriteOutput(dst, offset, value, length, byte, iscopy, tid);

        // Advance output pointers
        dst += broadcast(offset + length, NUM_THREADS - 1, tid);
#if SIMD_WIDTH < NUM_THREADS
        GroupMemoryBarrierWithGroupSync();
#endif
        // Compute output pointers for the current round
        length = iscopy || oob ? 0 : value;
        offset = scan(length, tid);

        iscopy = sym > 256; // Current symbol is a copy, transition to the new state
        byte = sym;
    }

    // One last round of copy processing
    sym = dec.decode(br.peek(15 + 16), len, true);
    iscopy &= !oob;
    uint32_t dist = TranslateSymbol(br, sym, len, br.peek(), iscopy, tid, false);
    WriteOutput(dst, offset, dist, length, byte, iscopy, tid);

    uint res = dst + broadcast(offset + length, NUM_THREADS - 1, tid); // Advance destination pointer
#if SIMD_WIDTH < NUM_THREADS
    GroupMemoryBarrierWithGroupSync(); // THIS BARRIER IS REQUIRED
#endif
    return res;
}

// Uncompressed block (raw copy)
uint32_t UncompressedBlock(inout BitReader br, uint32_t dst, uint32_t size, uint tid)
{
    uint32_t nrounds = size / NUM_THREADS;

    // Full rounds with no bounds checking
    while (nrounds--)
    {
        StoreByte(dst + tid, br.read(8, tid, true));
        dst += NUM_THREADS;
    }

    uint32_t rem = size % NUM_THREADS;

    // Last partial round with bounds check
    if (rem != 0)
    {
        uint32_t byte = br.read(8, tid, tid < rem);
        if (tid < rem)
            StoreByte(dst + tid, byte);
        dst += rem;
    }

    return dst;
}

// Initialize fixed code lengths, return a histogram
uint FixedCodeLengths(uint tid)
{
    g_buf.data[tid] = tid < 18 ? 0x88888888 : 0x99999999;
    g_buf.data[tid + 32] = tid < 3 ? 0x77777777 : (tid < 4 ? 0x88888888 : 0x55555555);

    // Threads can be synchronized later..
    return tid == 7 ? 24 : (tid == 8 ? 152 : (tid == 9 ? 112 : tid == 16 + 5 ? 32 : 0));
}

// This is main entry point for tile decompressor
void DecompressTile(in TileParams params, uint tid)
{
    // Init bit reader
    BitReader br;
    br.init(params.inPos, tid);

    bool done;
    uint32_t dst = params.outPos;

    // Clear destination to 0
    for (uint32_t i = tid; i < (params.outSize + 3) / 4; i += NUM_THREADS)
        output.Store(dst + i * 4, 0);

    // .. for each block
    do
    {
        // Read block header and broadcast to all threads
        uint32_t header = broadcast(br.peek(), 0, tid);
#if SIMD_WIDTH < NUM_THREADS
        GroupMemoryBarrierWithGroupSync();
#endif
        done = extract(header, 0, 1) != 0;

        // Parse block type
        uint32_t btype = extract(header, 1, 2);

        br.eat(3, tid, tid == 0);

        uint counts, size, hlit, hdist;

        switch (btype)
        {

        case 2: // Dynamic huffman block
            hlit = extract(header, 3, 5, 257);
            hdist = extract(header, 8, 5, 1);
            br.eat(14, tid, tid == 0);
            counts = UnpackCodeLengths(br, hlit, hdist, extract(header, 13, 4, 4), tid, dst);
            // Falls through to the following case
        case 1: // Fixed huffman block
            if (btype == 1)
                counts = FixedCodeLengths(tid);

            dst = CompressedBlock(br, btype == 1 ? 288 : hlit, counts, dst, tid);
            break;

        case 0: // Uncompressed block
            size = broadcast(br.read(16, tid, tid == 0), 0, tid);
            GroupMemoryBarrierWithGroupSync();
            dst = UncompressedBlock(br, dst, size, tid);
            break;

        default:; // Should never happen
        }

    } while (!done);
}

groupshared uint g_bcst;

void CopyUncompressedTile(uint tid, uint streamInPos, uint streamOutPos, uint totalSize, uint tileIdx)
{
    uint streamOffset = kDefaultTileSize * tileIdx;

    uint inTileStart = streamInPos + streamOffset;
    uint outTileStart = streamOutPos + streamOffset;

    for (uint i = 0; i < kDefaultTileSize; i += sizeof(uint) * NUM_THREADS)
    {
        uint offset = i + (sizeof(uint) * tid);

        if ((streamOffset + offset) < totalSize)
        {
            uint inPos = inTileStart + offset;
            uint outPos = outTileStart + offset;

            output.Store(outPos, input.Load(inPos));
        }
    }
}

void CopyUncompressedStream(uint tid, uint streamIdx, uint streamInPos, uint streamOutPos)
{
    uint size = input.Load(streamInPos);
    streamInPos += sizeof(uint);

    uint numTiles = (size + kDefaultTileSize - 1) / kDefaultTileSize;

    while (true)
    {
        uint tileIdx = ~0;

        // Leader grabs the tile index
        if (tid == 0)
        {
            scratch.InterlockedAdd(ScratchStreamTileIndexOffset(streamIdx), 1u, tileIdx);
        }

        // Broadcast tile index
        tileIdx = broadcast(tileIdx, 0, tid);
#if SIMD_WIDTH < NUM_THREADS
        GroupMemoryBarrierWithGroupSync();
#endif

        if (tileIdx >= numTiles)
            break;

        CopyUncompressedTile(tid, streamInPos, streamOutPos, size, tileIdx);
    }
}

// Main entry point - each thread group processes a page/tile and uses a work
// stealing scheme such that it runs until all streams have been decompressed
[numthreads(NUM_THREADS, 1, 1)] 
void CSMain(uint tid : SV_GroupThreadID)
{
    // Read the control buffer to determine how many streams are left
    // for decompressing.
    int numStreamsLeft = 0;
    if (tid == 0)
        numStreamsLeft = control.Load(0); // This load needs to be atomic across the thread group

    numStreamsLeft = broadcast(numStreamsLeft, 0, tid);
#if SIMD_WIDTH < NUM_THREADS
    GroupMemoryBarrierWithGroupSync();
#endif

    [allow_uav_condition] while (numStreamsLeft > 0)
    {
        // Read the input and output positions of the
        // current stream and construct a TileStream to
        // access the tiles.
        uint streamIdx = numStreamsLeft - 1;
        const uint streamInPos = control.Load(ControlStreamInOffset(streamIdx));
        uint streamOutPos = control.Load(ControlStreamOutOffset(streamIdx));
        const TileStream tileStream = TileStream::construct(streamInPos);

        // Grab a tile and decompress it until no tiles remain
        [allow_uav_condition] while (true)
        {
            uint tileIdx = ~0;

            // Leader grabs the tile index
            if (tid == 0)
            {
                scratch.InterlockedAdd(ScratchStreamTileIndexOffset(streamIdx), 1u, tileIdx);
            }

            // Broadcast tile index from leader
            tileIdx = broadcast(tileIdx, 0, tid);
#if SIMD_WIDTH < NUM_THREADS
            GroupMemoryBarrierWithGroupSync();
#endif
            if (tileIdx >= tileStream.GetNumTiles())
                break;

            TileParams params = tileStream.GetTileParams(streamInPos, streamOutPos, tileIdx);
            DecompressTile(params, tid);
        }

        // First thread in a partition does the CAS
        if (tid == 0)
        {
            int prevNumStreamsLeft;
            control.InterlockedCompareExchange(0, numStreamsLeft, numStreamsLeft - 1, prevNumStreamsLeft);

            if (prevNumStreamsLeft == numStreamsLeft)
                --numStreamsLeft;
            else
                numStreamsLeft = prevNumStreamsLeft;
        }

        // Broadcast num streams from leader
        numStreamsLeft = broadcast(numStreamsLeft, 0, tid);

#if SIMD_WIDTH < NUM_THREADS
        GroupMemoryBarrierWithGroupSync();
#endif
    }
}
