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
 *
 * Contains definitions of various macros, constants, structures and helper functions
 */

#pragma once

#ifndef ZSTDGPU_STRUCTS_H
#define ZSTDGPU_STRUCTS_H

#include "zstdgpu_shared_structs.h"

#ifndef ZSTDGPU_UNUSED
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_UNUSED(x) (void)0
#   else
#       define ZSTDGPU_UNUSED(x) (void)(sizeof(x)) /** we prefer sizeof to avoid side effects */
#   endif
#endif

#ifndef ZSTDGPU_ASSERT
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_ASSERT(cond) ZSTDGPU_UNUSED(cond)
#   else
#       ifdef NDEBUG
#           define ZSTDGPU_ASSERT(cond) ZSTDGPU_UNUSED(cond)
#       else
#           define ZSTDGPU_ASSERT(cond) assert(cond)
#       endif
#   endif
#endif

#ifndef ZSTDGPU_BREAK
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_BREAK()
#   else
#       define ZSTDGPU_BREAK() __debugbreak()
#   endif
#endif

#ifndef ZSTDGPU_PRAGMA_GNUC
#   if defined(__clang__) || defined(__GNUC__)  || defined(__hlsl_dx_compiler)
#       define ZSTDGPU_PRAGMA_GNUC(x) _Pragma(#x)
#   else
#       define ZSTDGPU_PRAGMA_GNUC(x)
#   endif
#endif /* ZSTDGPU_PRAGMA_GNUC */

#ifndef ZSTDGPU_PRAGMA_MSVC
#   if defined(_MSC_VER)
#       define ZSTDGPU_PRAGMA_MSVC(x) __pragma(x)
#   else
#define ZSTDGPU_PRAGMA_MSVC(x)
#   endif
#endif /* ZSTDGPU_PRAGMA_MSVC */

#ifndef ZSTDGPU_WARN_PUSH_DXC
#   ifdef __hlsl_dx_compiler
#      define ZSTDGPU_WARN_PUSH_DXC()  ZSTDGPU_PRAGMA_GNUC(dxc diagnostic push)
#      define ZSTDGPU_WARN_STOP_DXC(w) ZSTDGPU_PRAGMA_GNUC(dxc diagnostic ignored #w)
#      define ZSTDGPU_WARN_POP_DXC()   ZSTDGPU_PRAGMA_GNUC(dxc diagnostic pop)
#   else
#      define ZSTDGPU_WARN_PUSH_DXC()
#      define ZSTDGPU_WARN_STOP_DXC(w)
#      define ZSTDGPU_WARN_POP_DXC()
#   endif
#endif /* ZSTDGPU_WARN_PUSH_DXC */

#ifndef ZSTDGPU_WARN_DISABLE_DXC
#   define ZSTDGPU_WARN_DISABLE_DXC(w, statement) ZSTDGPU_WARN_PUSH_DXC() ZSTDGPU_WARN_STOP_DXC(w) statement ZSTDGPU_WARN_POP_DXC()
#endif /* ZSTDGPU_WARN_DISABLE_DXC */

#ifndef ZSTDGPU_WARN_PUSH_MSVC
#   ifdef _MSC_VER
#      define ZSTDGPU_WARN_PUSH_MSVC()  ZSTDGPU_PRAGMA_MSVC(warning(push))
#      define ZSTDGPU_WARN_STOP_MSVC(w) ZSTDGPU_PRAGMA_MSVC(warning(disable : w))
#      define ZSTDGPU_WARN_POP_MSVC()   ZSTDGPU_PRAGMA_MSVC(warning(pop))
#   else
#      define ZSTDGPU_WARN_PUSH_MSVC()
#      define ZSTDGPU_WARN_STOP_MSVC(w)
#      define ZSTDGPU_WARN_POP_MSVC()
#   endif
#endif /* ZSTDGPU_WARN_PUSH_MSVC */

#ifndef ZSTDGPU_WARN_DISABLE_MSVC
#   define ZSTDGPU_WARN_DISABLE_MSVC(w, statement) ZSTDGPU_WARN_PUSH_MSVC() ZSTDGPU_WARN_STOP_MSVC(w) statement ZSTDGPU_WARN_POP_MSVC()
#endif /* ZSTDGPU_WARN_DISABLE_MSVC */

#ifndef ZSTDGPU_GLC
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_GLC ZSTDGPU_WARN_DISABLE_DXC(-Wignored-attributes, globallycoherent)
#   else
#       define ZSTDGPU_GLC
#   endif
#endif /* ZSTDGPU_GLC */

#ifndef ZSTDGPU_ENUM
#   ifdef _MSC_VER
#       define ZSTDGPU_ENUM(e)          ZSTDGPU_WARN_DISABLE_MSVC(26812, zstdgpu_##e)
#       define ZSTDGPU_ENUM_CONST(ec)   ZSTDGPU_WARN_DISABLE_MSVC(26812, kzstdgpu_##ec)
#   else
#       define ZSTDGPU_ENUM(e)          zstdgpu_##e
#       define ZSTDGPU_ENUM_CONST(ec)   kzstdgpu_##ec
#   endif
#endif /* ZSTDGPU_ENUM */

#ifndef ZSTDGPU_INTENDED_SHIFT
#   ifdef _MSC_VER
#       define ZSTDGPU_INTENDED_SHIFT(e) ZSTDGPU_WARN_DISABLE_MSVC(26450, (e))
#   else
#       define ZSTDGPU_INTENDED_SHIFT(e) (e)
#   endif
#endif /* ZSTDGPU_INTENDED_SHIFT */

#ifndef ZSTDGPU_INTENDED_MUL32
#   ifdef _MSC_VER
#       define ZSTDGPU_INTENDED_MUL32(e) ZSTDGPU_WARN_DISABLE_MSVC(26451, (e))
#   else
#       define ZSTDGPU_INTENDED_MUL32(e) (e)
#   endif
#endif /* ZSTDGPU_INTENDED_MUL32 */

#ifndef ZSTDGPU_RO_BUFFER
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_RO_BUFFER(type) StructuredBuffer<type>
#   else
#       define ZSTDGPU_RO_BUFFER(type) const type *
#   endif
#endif

#ifndef ZSTDGPU_RW_BUFFER
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_RW_BUFFER(type) RWStructuredBuffer<type>
#   else
#       define ZSTDGPU_RW_BUFFER(type) type *
#   endif
#endif

#ifndef ZSTDGPU_RW_BUFFER_GLC
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_RW_BUFFER_GLC(type) ZSTDGPU_GLC RWStructuredBuffer<type>
#   else
#       define ZSTDGPU_RW_BUFFER_GLC(type) type *
#   endif
#endif

#ifndef ZSTDGPU_RO_TYPED_BUFFER
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_RO_TYPED_BUFFER(ShaderType, StorageType) Buffer<ShaderType>
#   else
#       define ZSTDGPU_RO_TYPED_BUFFER(ShaderType, StorageType) const StorageType *
#   endif
#endif

#ifndef ZSTDGPU_RW_TYPED_BUFFER
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_RW_TYPED_BUFFER(ShaderType, StorageType) RWBuffer<ShaderType>
#   else
#       define ZSTDGPU_RW_TYPED_BUFFER(ShaderType, StorageType) StorageType *
#   endif
#endif

#ifndef ZSTDGPU_RW_TYPED_BUFFER_GLC
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_RW_TYPED_BUFFER_GLC(ShaderType, StorageType) ZSTDGPU_GLC RWBuffer<ShaderType>
#   else
#       define ZSTDGPU_RW_TYPED_BUFFER_GLC(ShaderType, StorageType) StorageType *
#   endif
#endif

#ifndef ZSTDGPU_RO_BYTE_BUFFER
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_RO_RAW_BUFFER(type) ByteAddressBuffer /* no type, it's opaquue in HLSL side */
#   else
#       define ZSTDGPU_RO_RAW_BUFFER(type) const type *
#   endif
#endif

#ifndef ZSTDGPU_PARAM_IN
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_PARAM_IN(type) in type
#   else
#       define ZSTDGPU_PARAM_IN(type) const type &
#   endif
#endif

#ifndef ZSTDGPU_PARAM_INOUT
#   ifdef __hlsl_dx_compiler
#       define ZSTDGPU_PARAM_INOUT(type) inout type
#   else
#       define ZSTDGPU_PARAM_INOUT(type) type &
#   endif
#endif

// Opaque LDS address types: offset on HLSL, pointer on C++.
// Using these instead of raw uint32_t / uint32_t* prevents accidental type
// mismatches when storing intermediate LDS addresses in local variables.
#ifdef __hlsl_dx_compiler
    typedef uint32_t        zstdgpu_lds_uintptr_t;
    typedef uint32_t        zstdgpu_lds_const_uintptr_t;
#else
    typedef uint32_t *      zstdgpu_lds_uintptr_t;
    typedef const uint32_t* zstdgpu_lds_const_uintptr_t;
#endif

#ifndef ZSTDGPU_PARAM_LDS_IN
#   define ZSTDGPU_PARAM_LDS_IN(type) zstdgpu_lds_const_uintptr_t
#endif

#ifndef ZSTDGPU_PARAM_LDS_INOUT
#   define ZSTDGPU_PARAM_LDS_INOUT(type) zstdgpu_lds_uintptr_t
#endif

#ifndef ZSTDGPU_BRANCH
#   ifdef __hlsl_dx_compiler
#      define ZSTDGPU_BRANCH [branch]
#   else
#      define ZSTDGPU_BRANCH
#   endif
#endif

#ifndef ZSTDGPU_LOOP
#   ifdef __hlsl_dx_compiler
#      define ZSTDGPU_LOOP [loop]
#   else
#      define ZSTDGPU_LOOP
#   endif
#endif

static const uint32_t kzstdgpu_MaxCount_ThreadGroupsPerDimensionLog2 = 15;

static const uint32_t kzstdgpu_MaxCount_BlockSizeBits   = 17u;

static const uint32_t kzstdgpu_MaxCount_LiteralBytes    = 1u << kzstdgpu_MaxCount_BlockSizeBits;

static const uint32_t kzstdgpu_MaxCount_HuffmanWeights              = 256;
#if 1 // ASSUME_11BIT_HUFFMAN_CODES
static const uint32_t kzstdgpu_MaxCount_HuffmanWeightBits           = 11;
#else
static const uint32_t kzstdgpu_MaxCount_HuffmanWeightBits           = 16;
#endif

static const uint32_t kzstdgpu_MaxCount_HuffmanWeightRanks          = kzstdgpu_MaxCount_HuffmanWeightBits + 1;
static const uint32_t kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits  = kzstdgpu_MaxCount_HuffmanWeights / 32;
static const uint32_t kzstdgpu_MaxCount_HuffmanWeightsAllDigitBits  = kzstdgpu_MaxCount_HuffmanWeightsOneDigitBits * 5;

static const uint32_t kzstdgpu_MaxCount_FseProbs = 256;

static const uint32_t kzstdgpu_MaxCount_FseElems = 512;
static const uint32_t kzstdgpu_MaxCount_FseElemsOneDigitBits = kzstdgpu_MaxCount_FseElems / 32;
static const uint32_t kzstdgpu_MaxCount_FseElemsAllDigitBits = kzstdgpu_MaxCount_FseElemsOneDigitBits * 8;

static const uint32_t kzstdgpu_FseProbMaxAccuracy_HufW = 7;
static const uint32_t kzstdgpu_FseProbMaxAccuracy_LLen = 9;
static const uint32_t kzstdgpu_FseProbMaxAccuracy_Offs = 8;
static const uint32_t kzstdgpu_FseProbMaxAccuracy_MLen = 9;

static const uint32_t kzstdgpu_FseElemMaxCount_HufW = 1u << kzstdgpu_FseProbMaxAccuracy_HufW;
static const uint32_t kzstdgpu_FseElemMaxCount_LLen = 1u << kzstdgpu_FseProbMaxAccuracy_LLen;
static const uint32_t kzstdgpu_FseElemMaxCount_Offs = 1u << kzstdgpu_FseProbMaxAccuracy_Offs;
static const uint32_t kzstdgpu_FseElemMaxCount_MLen = 1u << kzstdgpu_FseProbMaxAccuracy_MLen;

static const uint32_t kzstdgpu_FseDefaultProbCount_LLen = 36;
static const uint32_t kzstdgpu_FseDefaultProbCount_Offs = 29;
static const uint32_t kzstdgpu_FseDefaultProbCount_MLen = 53;

static const uint32_t kzstdgpu_FseDefaultProbAccuracy_LLen = 6;
static const uint32_t kzstdgpu_FseDefaultProbAccuracy_Offs = 5;
static const uint32_t kzstdgpu_FseDefaultProbAccuracy_MLen = 6;

// 256 dense RLE entries at the beginning of FSE element buffers (positions 0-255).
// RLE table with symbol S uses fseTableIndex = S. Real tables start at index 256.
static const uint32_t kzstdgpu_FseRleTableCount = 256;

// FSE element packing: symbol(8) | bitcnt(8) | nstate(16) -> uint32_t
static inline uint32_t zstdgpu_PackFseElem(uint32_t symbol, uint32_t bitcnt, uint32_t nstate)
{
    return (nstate << 16) | (bitcnt << 8) | symbol;
}

static inline uint32_t zstdgpu_FseElem_Symbol(uint32_t packed) { return packed & 0xffu; }
static inline uint32_t zstdgpu_FseElem_Bitcnt(uint32_t packed) { return (packed >> 8) & 0xffu; }
static inline uint32_t zstdgpu_FseElem_NState(uint32_t packed) { return packed >> 16; }

// We define some special indices to classify FSE table indices referred by compressed blocks:
//  "Unused" - special index showing the compressed block doesn't use FSE table
//  "Repeat" - special index showing the compressed block uses FSE table from previous block that uses some
static const uint32_t kzstdgpu_FseProbTableIndex_Unused = 0x3fffffff;
static const uint32_t kzstdgpu_FseProbTableIndex_Repeat = kzstdgpu_FseProbTableIndex_Unused - 1;

typedef struct zstdgpu_Counters
{
    uint32_t FseHufW;
    uint32_t FseLLen;
    uint32_t FseOffs;
    uint32_t FseMLen;
    uint32_t DecompressLiteralsGroups;
    uint32_t DecompressSequencesGroups;

    uint32_t HUF_WgtStreams;
    uint32_t Seq_Streams_DecodedItems;
    uint32_t HUF_Streams_DecodedBytes;
    uint32_t Seq_Streams;
    uint32_t HUF_Streams;
    uint32_t RAW_Streams;
    uint32_t RLE_Streams;
    uint32_t Blocks_RAW;
    uint32_t Blocks_RLE;
    uint32_t Blocks_CMP;
    uint32_t BlocksBytes_RAW;
    uint32_t BlocksBytes_RLE;
    uint32_t Frames;
    uint32_t Frames_UncompressedByteSize;
    uint32_t Frames_ExecuteSequences;
} zstdgpu_Counters;

static const uint32_t kzstdgpu_DispatchSlot_FseHufW                      = 0;
static const uint32_t kzstdgpu_DispatchSlot_FseLLen                      = 1;
static const uint32_t kzstdgpu_DispatchSlot_FseOffs                      = 2;
static const uint32_t kzstdgpu_DispatchSlot_FseMLen                      = 3;
static const uint32_t kzstdgpu_DispatchSlot_DecompressHuffmanWeights     = 4;
static const uint32_t kzstdgpu_DispatchSlot_DecodeHuffmanWeights         = 5;
static const uint32_t kzstdgpu_DispatchSlot_GroupCompressedLiterals      = 6;
static const uint32_t kzstdgpu_DispatchSlot_HUF_WgtStreams               = 7;
static const uint32_t kzstdgpu_DispatchSlot_DecompressLiterals           = 8;
static const uint32_t kzstdgpu_DispatchSlot_DecompressSequences          = 9;
static const uint32_t kzstdgpu_DispatchSlot_FinaliseSequenceOffsets      = 10;
static const uint32_t kzstdgpu_DispatchSlot_PrefixSequenceOffsets        = 11;
static const uint32_t kzstdgpu_DispatchSlot_ComputePrefixSum             = 12;
static const uint32_t kzstdgpu_DispatchSlot_PrefixBlockSizes             = 13;
static const uint32_t kzstdgpu_DispatchSlot_MemcpyRAW                    = 14;
static const uint32_t kzstdgpu_DispatchSlot_MemsetRLE                    = 15;
static const uint32_t kzstdgpu_DispatchSlot_ParseCompressedBlocks        = 16;
static const uint32_t kzstdgpu_DispatchSlot_Memset_CmpBlockLookback      = 17;
static const uint32_t kzstdgpu_DispatchSlot_Memset_TableIndexLookback    = 18;
static const uint32_t kzstdgpu_DispatchSlot_Memset_LitStreamEnd          = 19;
static const uint32_t kzstdgpu_DispatchSlot_Memset_AllBlockLookback      = 20;
static const uint32_t kzstdgpu_DispatchSlot_Count                        = 21;

#if defined(_GAMING_XBOX) || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
static const uint32_t kzstdgpu_DispatchSlot_CmdsPerSlot                  = 1;
#else
static const uint32_t kzstdgpu_DispatchSlot_CmdsPerSlot                  = 2;
#endif

static const uint32_t kzstdgpu_DispatchSlot_CmdStrideInUInt32            = 5;  // tgOffset, workItemCount, X, Y, Z
static const uint32_t kzstdgpu_DispatchSlot_StrideInUInt32               = kzstdgpu_DispatchSlot_CmdsPerSlot * kzstdgpu_DispatchSlot_CmdStrideInUInt32;  // 10 (PC), 5 (Xbox)

// NOTE(pamartis):
//      We use macro here to make sure we can use them to compile-out
//      `groupshared` variables in HLSL which doesn't give other options.
//
#if defined(_GAMING_XBOX_SCARLETT) || defined(__XBOX_SCARLETT)
#    define kzstdgpu_WaveSize_Min 32
#elif defined(_GAMING_XBOX_XBOXONE) || defined(__XBOX_ONE)
#    define kzstdgpu_WaveSize_Min 64
#else
#    define kzstdgpu_WaveSize_Min 4
#endif

// NOTE(pamartis): for initialization, we aim one-wave threadgroups. On PC we choose 32 lanes.
#if defined(_GAMING_XBOX) || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
static const uint32_t kzstdgpu_TgSizeX_InitCounters = 64;
#else
static const uint32_t kzstdgpu_TgSizeX_InitCounters = 32;
#endif

#if defined(_GAMING_XBOX) || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
static const uint32_t kzstdgpu_TgSizeX_PrefixSum = 64;
#else
static const uint32_t kzstdgpu_TgSizeX_PrefixSum = 32;
#endif

static const uint32_t kzstdgpu_TgSizeX_ParseCompressedBlocks = 32;
static const uint32_t kzstdgpu_TgSizeX_Memset = 64;

// NOTE(pamartis): The rationale behind the below choice of TG sizes is the following
//      On Xbox, we aim widest available wave size to help with cross-lane operations
//      and rely on single wave per TG, so we don't waste time on cross-wave synchronisation
//
//      On PC, we choose TG size to be of the maximal possible wave size supported by D3D12
//      and not smaller because we want to avoid "disabled" lanes at the start of TG.
//      This might limit the occupancy on AMD hardware to 80% if wave size chosen by the driver
//      is going to be 64. That is due to 16 threadgroup per CU limit which will translate
//      to 32 waves (two 64-wide wave per TG) per CU instead of 40 waves per CU.
//
#if defined(_GAMING_XBOX_SCARLETT) || defined(__XBOX_SCARLETT)
#   define kzstdgpu_TgSizeX_InitFseTable 64
#elif defined(_GAMING_XBOX_XBOXONE) || defined(__XBOX_ONE)
#   define kzstdgpu_TgSizeX_InitFseTable 64
#else
#   define kzstdgpu_TgSizeX_InitFseTable 128
#endif

#if defined(_GAMING_XBOX) || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
static const uint32_t kzstdgpu_TgSizeX_PrefixSum_LiteralCount = 64;
#else
static const uint32_t kzstdgpu_TgSizeX_PrefixSum_LiteralCount = 32;
#endif

#if defined(_GAMING_XBOX) || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
static const uint32_t kzstdgpu_TgSizeX_PrefixSequenceOffsets = 64;
#else
static const uint32_t kzstdgpu_TgSizeX_PrefixSequenceOffsets = 32;
#endif


#if defined(_GAMING_XBOX_SCARLETT) || defined(__XBOX_SCARLETT)
static const uint32_t kzstdgpu_TgSizeX_DecompressHuffmanWeights = 1;
#else
static const uint32_t kzstdgpu_TgSizeX_DecompressHuffmanWeights = 8;
#endif

static const uint32_t kzstdgpu_TgSizeX_DecodeHuffmanWeights = 32;

// NOTE(pamartis): Decompressing Literals should be as small as possible (divergent workload)
// but not too small to make sure Huffman table initialisation isn't repeated too often
// TODO(pamartis) Try threadgroup sizes that less than wave size to see whether reducing
// divergency more efficient than having unfilled waves...
#if defined(_GAMING_XBOX_XBOXONE) || defined(__XBOX_ONE)
static const uitn32_t kzstdgpu_TgSizeX_DecompressLiterals = 64;
#else
static const uint32_t kzstdgpu_TgSizeX_DecompressLiterals = 32;
#endif

#if defined(_GAMING_XBOX) || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
static const uint32_t kzstdgpu_TgSizeX_FinaliseSequenceOffsets = 64;
#else
static const uint32_t kzstdgpu_TgSizeX_FinaliseSequenceOffsets = 256;
#endif

#if defined(_GAMING_XBOX) || defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
static const uint32_t kzstdgpu_TgSizeX_MemsetMemcpy = 64;
#else
static const uint32_t kzstdgpu_TgSizeX_MemsetMemcpy = 32;
#endif

#define ZSTDGPU_TG_COUNT(elemCount, tgSize) (((elemCount) + (tgSize) - 1) / (tgSize))

static inline uint32_t zstdgpu_AlignUp(uint32_t offset, uint32_t alignment)
{
    ZSTDGPU_ASSERT(0 == (alignment & (alignment - 1)));
    const uint32_t alignmentBits = alignment - 1;
    return (offset + alignmentBits) & ~alignmentBits;
}

#ifdef __hlsl_dx_compiler
#   define ZSTDGPU_FOR_WORK_ITEMS(workItemId, workItemCount, groupThreadId, groupThreadCount)   \
        for (ZSTDGPU_WARN_DISABLE_DXC(-Wfor-redefinition, uint32_t workItemId = groupThreadId); workItemId < workItemCount; workItemId += groupThreadCount)
#else
#   define ZSTDGPU_FOR_WORK_ITEMS(workItemId, workItemCount, groupThreadId, groupThreadCount)   \
        for (uint32_t workItemId = 0; workItemId < workItemCount; workItemId += 1u)
#endif

#ifndef __hlsl_dx_compiler
typedef struct uint32_t4
{
    uint32_t x, y, z, w;
} uint32_t4;
static inline void GroupMemoryBarrierWithGroupSync(void) { }
static inline void DeviceMemoryBarrierWithGroupSync(void) { }
static inline bool WaveIsFirstLane(void) { return true; }
static inline uint32_t WaveActiveCountBits(bool bit) { return bit ? 1 : 0; }
static inline uint32_t WavePrefixCountBits(bool bit) { (void)bit; return 0; }
static inline uint32_t WaveGetLaneCount(void) { return 1; }
static inline uint32_t WaveGetLaneIndex(void) { return 0; }
static inline uint32_t4 WaveActiveBallot(bool b) { uint32_t4 mask = {}; mask.x = b ? 1 : 0; return mask; }
static inline bool WaveActiveAnyTrue(bool bit) { return bit; }
static inline bool WaveActiveAllTrue(bool bit) { return bit; }
template <typename T> static inline T WaveReadLaneFirst(T x) { return x; }
template <typename T> static inline T WaveReadLaneAt(T x, uint32_t index) { (void)index; return x; }
template <typename T> static inline T WaveActiveSum(T x) { return x; }
template <typename T> static inline T WaveActiveMax(T x) { return x; }
template <typename T> static inline T WaveActiveMin(T x) { return x; }
template <typename T> static inline T WaveActiveBitOr(T x) { return x; }
template <typename T> static inline T WaveActiveBitAnd(T x) { return x; }
template <typename T> static inline T WaveActiveBitXor(T x) { return x; }
template <typename T> static inline T WavePrefixSum(T x) { (void)x; return 0; }
static inline void InterlockedAdd(uint32_t & dst, uint32_t x) { dst += x; }
static inline void InterlockedOr (uint32_t & dst, uint32_t x) { dst |= x; }
static inline void InterlockedMin(uint32_t & dst, uint32_t x) { dst = dst < x ? dst : x; }
static inline void InterlockedMax(uint32_t & dst, uint32_t x) { dst = dst > x ? dst : x; }
static inline void InterlockedAdd(uint32_t & dst, uint32_t x, uint32_t & ret) { ret = dst; dst += x; }
static inline void InterlockedCompareStore(uint32_t & dst, uint32_t compare, uint32_t x) { if (dst == compare) dst = x; }
#endif

static inline uint32_t zstdgpu_ByteOffsetLoadU32(ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer, uint32_t offset)
{
#ifdef __hlsl_dx_compiler
    return buffer.Load(offset);
#else
    return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(buffer) + offset);
#endif
}

static inline uint64_t zstdgpu_ByteOffsetLoadU64(ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer, uint32_t offset)
{
#ifdef __hlsl_dx_compiler
    uint2 v = buffer.Load2(offset);
    return v.x | (uint64_t(v.y) << 32);
#else
    const uint32_t* p32 = reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(buffer) + offset);
    return p32[0] | (uint64_t(p32[1]) << 32);
#endif
}

static inline uint32_t zstdgpu_BitFieldExtractU32(uint32_t x, uint32_t start, uint32_t count)
{
    const uint32_t mask = ~(~0u << count);
    const uint32_t bcnt = (x >> start) & mask;
    return bcnt;
}

static inline uint32_t zstdgpu_LoBitFieldMaskU32(uint32_t width)
{
    ZSTDGPU_ASSERT(width <= 31u);
#if 1
    return (1u << width) - 1u;
#else
    return ~(~0u << width);
#endif
}

static inline uint64_t zstdgpu_LoBitFieldMaskU64(uint32_t width)
{
    ZSTDGPU_ASSERT(width <= 63u);
#if 0
    return (1ull << width) - 1ull;
#else
    return ~(~0ull << width);
#endif
}

static inline uint64_t zstdgpu_LoBitFieldWideMaskU64(uint32_t width)
{
    ZSTDGPU_ASSERT(width <= 64u);
#if 0
    return (1ull << width) - 1ull;
#else
    const uint32_t bit = width >> 6u;
    return ~((~0ull << bit) << (width - bit));
#endif
}

static inline uint32_t zstdgpu_HiBitFieldMaskU32(uint32_t width)
{
    ZSTDGPU_ASSERT(width <= 31u);
    /**NOTE(pamartis): disable the warning because the overflow here is intended behaviour */
    return ZSTDGPU_INTENDED_SHIFT(~0u << 1u) << (31u - width);
}

static inline uint64_t zstdgpu_HiBitFieldMaskU64(uint32_t width)
{
    ZSTDGPU_ASSERT(width <= 63u);
    return (~0ull << 1u) << (63u - width);
}

static inline uint32_t zstdgpu_FindFirstBitHiU32(uint32_t v)
{
#ifdef __hlsl_dx_compiler
    return firstbithigh(v);
#else
    unsigned long index = 0;
    uint32_t found = _BitScanReverse(&index, v);
    ZSTDGPU_ASSERT(0 != found);
    return (uint32_t)index;
#endif
}

// The input to this function must not be 0.
static inline uint32_t zstdgpu_FindFirstBitHiU32_Nonzero(uint32_t v)
{
#ifdef __hlsl_dx_compiler
    // HLSL's firstbithigh is like _BitScanReverse and returns -1 for an input of 0.
    // However, the FirstbitHi DXIL instruction is like a count-leading-zeros, but it returns -1
    // (instead of 32) for an input of 0. DXC expands firstbithigh into four DXIL instructions
    // (FirstbitHi, sub, icmp, select). So the following formulation actually saves a DXIL instruction,
    // and should be good on at least AMD RDNA3 ({v,s}_{ctz,clz}_i32_b32 match DXIL FirstBit{Lo,Hi} semantics),
    // especially since its current shader compiler doesn't seem to emit the SALU version of clz_i32_u32.
    // This method does not yield the same answer for an input of 0.
    //
    // ISA for HLSL firstbithigh:
    //          v_clz_i32_u32   v0, s10
    //          v_sub_nc_u32    v1, 31, v0
    //          v_cmp_ne_i32    vcc_lo, -1, v0
    //          v_cndmask_b32   v0, -1, v1, vcc_lo  // final result in v0
    //
    // ISA for the following:
    //          s_brev_b32    s13, s10
    //          s_ctz_i32_b32 s13, s13
    //          s_sub_u32     s13, 31, s13          // final result in s13
    return 31 - firstbitlow(reversebits(v));
#else
    unsigned long index = 0;
    uint32_t found = _BitScanReverse(&index, v);
    ZSTDGPU_ASSERT(0 != found);
    return found ? (uint32_t)index : 32u; // found should be true, but do this to match GPU behavior
#endif
}

static inline uint32_t zstdgpu_FindFirstBitHiU64(uint64_t x)
{
#if defined(__hlsl_dx_compiler)
    return firstbithigh(x);
#elif defined(_M_X64)
    unsigned long index = 0;
    uint32_t found = _BitScanReverse64(&index, x);
    ZSTDGPU_ASSERT(0 != found);
    return (uint32_t)index;
#else
    for (uint32_t i = 63u; ; --i)
    {
        if (0 != (x & (1ull << i)) || i == 0)
        {
            return i;
        }
    }
#endif
}

static inline uint32_t zstdgpu_CountBitsU32(uint32_t v)
{
#ifdef __hlsl_dx_compiler
    return countbits(v);
#else
    return __popcnt(v);
#endif
}

static inline uint32_t zstdgpu_MinU32(uint32_t a, uint32_t b)
{
#ifdef __hlsl_dx_compiler
    return min(a, b);
#else
    return a < b ? a : b;
#endif
}

static inline uint32_t zstdgpu_MaxU32(uint32_t a, uint32_t b)
{
#ifdef __hlsl_dx_compiler
    return max(a, b);
#else
    return a > b ? a : b;
#endif
}

static inline uint32_t zstdgpu_MaxI32(int32_t a, int32_t b)
{
#ifdef __hlsl_dx_compiler
    return max(a, b);
#else
    return a > b ? a : b;
#endif
}

static inline uint32_t zstdgpu_Encode30BitLookbackSelf(uint32_t x)
{
    ZSTDGPU_ASSERT(x <= ~0xc0000000u);
    return (x & ~0xc0000000u) | 0x40000000u;
}

static inline uint32_t zstdgpu_Encode30BitLookbackFull(uint32_t x)
{
    ZSTDGPU_ASSERT(x <= ~0xc0000000u);
    return (x & ~0xc0000000u) | 0x80000000u;
}

static inline uint32_t zstdgpu_Decode30BitLookbackValue(uint32_t x)
{
    return x & ~0xc0000000u;
}

static inline uint32_t zstdgpu_Decode30BitLookbackFlags(uint32_t x)
{
    return x & 0xc0000000u;
}

static inline uint32_t zstdgpu_Check30BitLookbackFull(uint32_t flags)
{
    return flags == 0x80000000u ? 1u : 0u;
}

static inline uint32_t zstdgpu_Encode31BitLookbackFull(uint32_t x)
{
    ZSTDGPU_ASSERT(x <= ~0x80000000u);
    return (x & ~0x80000000u) | 0x80000000u;
}

static inline uint32_t zstdgpu_Decode31BitLookbackValue(uint32_t x)
{
    return x & ~0x80000000u;
}

static inline uint32_t zstdgpu_Decode31BitLookbackFlags(uint32_t x)
{
    return x & 0x80000000u;
}

static inline uint32_t zstdgpu_EncodeRawLitOffset(uint32_t x)
{
    ZSTDGPU_ASSERT(x <= ~0xc0000000u);
    return (x & ~0xc0000000u) | 0x40000000u;
}

static inline uint32_t zstdgpu_EncodeRleLitOffset(uint32_t x)
{
    ZSTDGPU_ASSERT(x <= 0x000000ffu);
    return (x & ~0xc0000000u) | 0x80000000u;
}

static inline uint32_t zstdgpu_EncodeCmpLitOffset(uint32_t x)
{
    ZSTDGPU_ASSERT(x <= ~0xc0000000u);
    return (x & ~0xc0000000u) | 0xc0000000u;
}

static inline uint32_t zstdgpu_DecodeLitOffsetType(uint32_t x)
{
    const uint32_t type = x & 0xc0000000u;
    ZSTDGPU_ASSERT(type != 0);
    return type;
}

static inline uint32_t zstdgpu_DecodeLitOffset(uint32_t x)
{
    const uint32_t offset = x & ~0xc0000000u;
    ZSTDGPU_ASSERT((x & 0xc0000000u) != 0);
    return offset;
}

static inline uint32_t zstdgpu_CheckLitOffsetTypeRaw(uint32_t x)
{
    return x == 0x40000000u ? 1u : 0u;
}

static inline uint32_t zstdgpu_CheckLitOffsetTypeRle(uint32_t x)
{
    return x == 0x80000000u ? 1u : 0u;
}

static inline uint32_t zstdgpu_CheckLitOffsetTypeCmp(uint32_t x)
{
    return x == 0xc0000000u ? 1u : 0u;
}

static inline uint32_t zstdgpu_EncodeSeqRepeatOffset(uint32_t x)
{
    // NOTE(pamartis):
    //      - we set bit 29 to mark this uint32_t as "encoded"
    //      - we set bits [28:27] to the "repeated offset"
    //      - we set all other bits to 1 to support an unknown number of "1 byte" subtractions
    return (x << 27u) | 0x27ffffffu;
}

static inline uint32_t zstdgpu_DecodeSeqRepeatOffset(uint32_t x)
{
    return (x >> 27u) & 3u;
}

static inline uint32_t zstdgpu_DecodeSeqRepeatOffsetSubtractedBytes(uint32_t x)
{
    return ~(x | 0xf8000000u);
}

static inline uint32_t zstdgpu_DecodeSeqRepeatOffsetEncoded(uint32_t x)
{
    return x & 0x20000000;
}

static inline uint32_t zstdgpu_DecodeSeqRepeatOffsetAndApplyPreviousOffsets(uint32_t offset, uint32_t prevOffs1, uint32_t prevOffs2, uint32_t prevOffs3)
{
    const uint32_t idx = zstdgpu_DecodeSeqRepeatOffset(offset);
    const uint32_t cnt = zstdgpu_DecodeSeqRepeatOffsetSubtractedBytes(offset);
    offset = (idx == 3u) ? prevOffs3 : ((idx == 2u) ? prevOffs2 : prevOffs1);
    return offset - cnt;
}

static inline void zstdgpu_DecodeSeqRepeatOffsetsAndApplyPreviousOffsets(ZSTDGPU_PARAM_INOUT(uint32_t) offset1,
                                                                         ZSTDGPU_PARAM_INOUT(uint32_t) offset2,
                                                                         ZSTDGPU_PARAM_INOUT(uint32_t) offset3,
                                                                         uint32_t nonZeroPrevOffset1,
                                                                         uint32_t nonZeroPrevOffset2,
                                                                         uint32_t nonZeroPrevOffset3)
{
    /* We propagate only if destination is an "encoded" offset or "invalid" (zero)*/
    ZSTDGPU_BRANCH if (zstdgpu_DecodeSeqRepeatOffsetEncoded(offset1) > 0)
        offset1 = zstdgpu_DecodeSeqRepeatOffsetAndApplyPreviousOffsets(offset1, nonZeroPrevOffset1, nonZeroPrevOffset2, nonZeroPrevOffset3);
    else if (offset1 == 0)
        offset1 = nonZeroPrevOffset1;

    ZSTDGPU_BRANCH if (zstdgpu_DecodeSeqRepeatOffsetEncoded(offset2) > 0)
        offset2 = zstdgpu_DecodeSeqRepeatOffsetAndApplyPreviousOffsets(offset2, nonZeroPrevOffset1, nonZeroPrevOffset2, nonZeroPrevOffset3);
    else if (offset2 == 0)
        offset2 = nonZeroPrevOffset2;

    ZSTDGPU_BRANCH if (zstdgpu_DecodeSeqRepeatOffsetEncoded(offset3) > 0)
        offset3 = zstdgpu_DecodeSeqRepeatOffsetAndApplyPreviousOffsets(offset3, nonZeroPrevOffset1, nonZeroPrevOffset2, nonZeroPrevOffset3);
    else if (offset3 == 0)
        offset3 = nonZeroPrevOffset3;
}

static inline void zstdgpu_Init_OffsetAndSize(ZSTDGPU_PARAM_INOUT(zstdgpu_OffsetAndSize) outOffsetAndSize)
{
    outOffsetAndSize.offs = 0;
    outOffsetAndSize.size = 0;
}

#ifndef ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS
#   define ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS(name)                                                                 \
        static inline uint32_t zstdgpu_##name##_Get(ZSTDGPU_PARAM_INOUT(zstdgpu_##name) inoutBitBuffer, uint32_t bitcnt)\
        {                                                                                                               \
            zstdgpu_##name##_Refill(inoutBitBuffer, bitcnt);                                                            \
            const uint32_t bits = zstdgpu_##name##_Top(inoutBitBuffer, bitcnt);                                         \
            zstdgpu_##name##_Pop(inoutBitBuffer, bitcnt);                                                               \
            return bits;                                                                                                \
        }                                                                                                               \
        static inline uint32_t zstdgpu_##name##_GetNoRefill(ZSTDGPU_PARAM_INOUT(zstdgpu_##name) inoutBitBuffer, uint32_t bitcnt)\
        {                                                                                                               \
            const uint32_t bits = zstdgpu_##name##_Top(inoutBitBuffer, bitcnt);                                         \
            zstdgpu_##name##_Pop(inoutBitBuffer, bitcnt);                                                               \
            return bits;                                                                                                \
        }
#else
#   error `ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS` must not be defined.
#endif

typedef struct zstdgpu_Forward_BitBuffer
{
    ZSTDGPU_RO_BUFFER(uint32_t) buffer;

    uint64_t bitbuf;    // VGPRs storing valid bits that are not consumed yet
    uint32_t offset;    // VGPR storing the offset in dwords to the start of the next dword fetch
    uint32_t bitcnt;    // VGPR storing the number of valid bits in `bitbuf`
    uint32_t datasz;    // VGPR as it store any memory block size varying per lane
    uint32_t bytesz;    // must be SGPR and must store the entire compressed buffer
} zstdgpu_Forward_BitBuffer;

static inline void zstdgpu_Forward_BitBuffer_Init(ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) outBuffer, ZSTDGPU_RO_BUFFER(uint32_t) buffer, uint32_t datasz, uint32_t bytesz)
{
    ZSTDGPU_ASSERT(0 == (bytesz & 3));

    outBuffer.buffer = buffer;
    outBuffer.bitbuf = 0;
    outBuffer.offset = 0;
    outBuffer.bitcnt = 0;
    outBuffer.datasz = datasz;
    outBuffer.bytesz = bytesz;
}

static inline void zstdgpu_Forward_BitBuffer_Refill(ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) inoutBuffer, uint32_t bitcnt)
{
    ZSTDGPU_ASSERT(bitcnt <= 32);

    if (inoutBuffer.bitcnt < bitcnt)
    {
        ZSTDGPU_ASSERT(inoutBuffer.offset <= (inoutBuffer.bytesz >> 2) - 1);

        inoutBuffer.bitbuf |= (uint64_t)inoutBuffer.buffer[inoutBuffer.offset] << inoutBuffer.bitcnt;
#if 0
        // TODO:    We currently rely on the fact that bits outside the bounds ("invalid") are never used.
        //          so they are present in "bitbuf" because we fetch the last dword and treat all bits as they are "valid"
        //          However, to improve validation we need to mask out "invalid" bits at the tail and register the current
        //          number of bits to make sure "refill" can be checked to.
        if (inoutBuffer.offset == (inoutBuffer.datasz - 1u) >> 2)
        {
            ZSTDGPU_BREAK();
        }
#endif

        inoutBuffer.bitcnt += 32;
        inoutBuffer.offset += 1;
    }
}

static inline uint32_t zstdgpu_Forward_BitBuffer_Top(ZSTDGPU_PARAM_IN(zstdgpu_Forward_BitBuffer) inBuffer, uint32_t bitcnt)
{
    // potentially two v_and_b32 on AMD because "bitcnt" is folded into a literal mask
    ZSTDGPU_ASSERT(inBuffer.bitcnt >= bitcnt);
    return (uint32_t)(inBuffer.bitbuf & ~(~0ull << bitcnt));
}

static inline void zstdgpu_Forward_BitBuffer_Pop(ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) inoutBuffer, uint32_t bitcnt)
{
    ZSTDGPU_ASSERT(inoutBuffer.bitcnt >= bitcnt);

    // potentially v_lshrrev_b64 + v_sub_nc_b32 on AMD
    inoutBuffer.bitbuf >>= bitcnt;
    inoutBuffer.bitcnt  -= bitcnt;
}

ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS(Forward_BitBuffer)

static inline uint32_t zstdgpu_Forward_BitBuffer_GetByteOffset(ZSTDGPU_PARAM_IN(zstdgpu_Forward_BitBuffer) inBuffer)
{
    ZSTDGPU_ASSERT(0 == (inBuffer.bitcnt & 7));
    return (inBuffer.offset << 2) - (inBuffer.bitcnt >> 3);
}

static inline void zstdgpu_Forward_BitBuffer_Skip(ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) inoutBuffer, uint32_t bytecnt)
{
    ZSTDGPU_ASSERT(0 == (inoutBuffer.bitcnt & 7));
    ZSTDGPU_ASSERT(inoutBuffer.datasz >= zstdgpu_Forward_BitBuffer_GetByteOffset(inoutBuffer) + bytecnt);

#if 1
    const uint32_t leftbytecnt = inoutBuffer.bitcnt >> 3;
    ZSTDGPU_BRANCH if (leftbytecnt < bytecnt)
    {
        bytecnt -= leftbytecnt;
        zstdgpu_Forward_BitBuffer_Pop(inoutBuffer, inoutBuffer.bitcnt);

        inoutBuffer.offset += bytecnt >> 2;
        inoutBuffer.bitcnt = (bytecnt & 3) << 3;
        inoutBuffer.bitbuf = (uint64_t)inoutBuffer.buffer[inoutBuffer.offset] >> inoutBuffer.bitcnt;
        inoutBuffer.offset += 1;
        inoutBuffer.bitcnt = 32 - inoutBuffer.bitcnt;

    }
    else
    {
        zstdgpu_Forward_BitBuffer_Pop(inoutBuffer, bytecnt << 3);
    }
#else
    zstdgpu_Forward_BitBuffer_Refill(inoutBuffer, (bytecnt & 3) * 8);
    zstdgpu_Forward_BitBuffer_Pop(inoutBuffer, (bytecnt & 3) * 8);
    bytecnt &= ~3;

    while (bytecnt > 0)
    {
        zstdgpu_Forward_BitBuffer_Refill(inoutBuffer, 32);
        zstdgpu_Forward_BitBuffer_Pop(inoutBuffer, 32);
        bytecnt -= 4;
    }
#endif
}

static inline void zstdgpu_Forward_BitBuffer_InitWithSegment(ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) outBuffer, ZSTDGPU_RO_BUFFER(uint32_t) buffer, ZSTDGPU_PARAM_IN(zstdgpu_OffsetAndSize) segment, uint32_t bytesz)
{
    // init as normal buffer but set `dstdatasz == offset + srcdatasz` because we're going to skip `offset` bytes
    zstdgpu_Forward_BitBuffer_Init(outBuffer, buffer, segment.offs + segment.size, bytesz);
    zstdgpu_Forward_BitBuffer_Skip(outBuffer, segment.offs);
}

static inline void zstdgpu_Forward_BitBuffer_ByteAlign(ZSTDGPU_PARAM_INOUT(zstdgpu_Forward_BitBuffer) inoutBuffer)
{
    zstdgpu_Forward_BitBuffer_Pop(inoutBuffer, inoutBuffer.bitcnt & 7);
}

//#define ZSTDGPU_USE_REVERSED_BIT_BUFFER_BITBUF 1

typedef struct zstdgpu_Backward_BitBuffer_V0
{
    ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer;

    uint64_t bitbuf;    // VGPRs storing valid bits that are not consumed yet
    uint32_t bitcnt;    // VGPR storing the number of valid bits in `bitbuf`
    uint32_t bitcntLast;
    uint32_t lastByteOffset;    // offset of last load (pre-decrement before next load)
    uint32_t baseByteOffset;
    bool     hadlastrefill;
} zstdgpu_Backward_BitBuffer_V0;

typedef struct zstdgpu_Backward_BitBuffer
{
    ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer;

    uint64_t    bitbuf;
    uint32_t    offset;
    uint32_t    bitpos;
} zstdgpu_Backward_BitBuffer;

typedef struct zstdgpu_Backward_CmpBitBuffer
{
    zstdgpu_Backward_BitBuffer_V0   bbref;
    zstdgpu_Backward_BitBuffer      bbtst;
} zstdgpu_Backward_CmpBitBuffer;

#ifndef __hlsl_dx_compiler

static inline uint32_t reversebits(uint32_t x)
{
    x = ((x >> 1) & 0x55555555) | ((x & 0x55555555) << 1);
    x = ((x >> 2) & 0x33333333) | ((x & 0x33333333) << 2);
    x = ((x >> 4) & 0x0F0F0F0F) | ((x & 0x0F0F0F0F) << 4);
    x = ((x >> 8) & 0x00FF00FF) | ((x & 0x00FF00FF) << 8);
    x = ( x >> 16             ) | ( x               << 16);
    return x;
}

#endif

static inline void zstdgpu_Backward_BitBuffer_V0_InitWithSegment(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer_V0) outBuffer, ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer, ZSTDGPU_PARAM_IN(zstdgpu_OffsetAndSize) segment)
{
    const uint32_t datasz = segment.offs + segment.size;

    const uint32_t baseByteOffset = segment.offs & -4;
    const uint32_t lastByteOffset = (datasz - 1u) & -4;

    // Firstly, we assume that all the bytes read from the last dword are valid and drop only highest bits
    uint32_t bitcnt = (datasz & 3u) << 3u; // Possible: 0, 8, 16, 24
    uint32_t bitmsk = ~0u >> (32u - bitcnt);
    uint32_t bitbuf = zstdgpu_ByteOffsetLoadU32(buffer, lastByteOffset) & bitmsk;

    // Secondly, we search for the highest set bit to see how many bits are valid
    bitcnt = zstdgpu_FindFirstBitHiU32_Nonzero(bitbuf);
#ifdef ZSTDGPU_USE_REVERSED_BIT_BUFFER_BITBUF
    bitbuf <<= 32u - bitcnt;
    bitbuf = reversebits(bitbuf);
#else
    //bitmsk = ~0u >> (32u - bitcnt);
    bitmsk = (1u << bitcnt) - 1u;
    bitbuf &= bitmsk;

    const uint32_t bitcntLast = (segment.offs & 0x3u) << 3u;
    {
        const uint32_t lobitcnt = baseByteOffset == lastByteOffset ? bitcntLast : 0;
        bitcnt  -= lobitcnt;
        bitbuf >>= lobitcnt;
    }
#endif

    outBuffer.buffer = buffer;
    outBuffer.bitbuf = (uint64_t)bitbuf;
    outBuffer.bitcnt = bitcnt;
    outBuffer.bitcntLast = bitcntLast;
    outBuffer.lastByteOffset = lastByteOffset;
    outBuffer.baseByteOffset = baseByteOffset;
    outBuffer.hadlastrefill = baseByteOffset == lastByteOffset;
    //outBuffer.bytesz = bytesz;
}

static inline bool zstdgpu_Backward_BitBuffer_V0_CanRefill(ZSTDGPU_PARAM_IN(zstdgpu_Backward_BitBuffer_V0) inBuffer, uint32_t bitcnt)
{
    ZSTDGPU_ASSERT(bitcnt <= 32);
    if (inBuffer.bitcnt >= bitcnt)
    {
        return true;
    }
    else
    {
        return !inBuffer.hadlastrefill;
    }
}

// FIXME/TODO(pamartis): This refill variant generate too many instruction on GPU, need improvement
static inline void zstdgpu_Backward_BitBuffer_V0_Refill(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer_V0) inoutBuffer, uint32_t bitcnt)
{
    ZSTDGPU_ASSERT(bitcnt <= 32);

    if (inoutBuffer.bitcnt < bitcnt)
    {
        ZSTDGPU_ASSERT(inoutBuffer.hadlastrefill == false);

        ZSTDGPU_ASSERT(inoutBuffer.lastByteOffset >= 4);
        inoutBuffer.lastByteOffset -= 4;
        const uint32_t nextByteOffset = inoutBuffer.lastByteOffset;
        ZSTDGPU_ASSERT(nextByteOffset >= inoutBuffer.baseByteOffset);

        // how many bits we need to remove
        const uint32_t lobitcnt = nextByteOffset > inoutBuffer.baseByteOffset ? 0u : inoutBuffer.bitcntLast;
        const uint32_t hibitcnt = 32u - lobitcnt;

        const uint32_t loadValue = zstdgpu_ByteOffsetLoadU32(inoutBuffer.buffer, nextByteOffset);
        #ifdef ZSTDGPU_USE_REVERSED_BIT_BUFFER_BITBUF
            inoutBuffer.bitbuf |= (uint64_t)reversebits(loadValue) << inoutBuffer.bitcnt;
        #else
            inoutBuffer.bitbuf = (inoutBuffer.bitbuf << hibitcnt) | (loadValue >> lobitcnt);
        #endif

        inoutBuffer.bitcnt += hibitcnt;
        inoutBuffer.hadlastrefill = !(nextByteOffset > inoutBuffer.baseByteOffset);
    }
}

static inline uint32_t zstdgpu_Backward_BitBuffer_V0_Top(ZSTDGPU_PARAM_IN(zstdgpu_Backward_BitBuffer_V0) inBuffer, uint32_t bitcnt)
{
    ZSTDGPU_ASSERT(inBuffer.bitcnt >= bitcnt);
#ifdef ZSTDGPU_USE_REVERSED_BIT_BUFFER_BITBUF
    // EXPERIMENT: store "bitbuf" reversed (by employing "v_bfrev_b32" on AMD)
    // potentially two v_and_b32 on AMD because "bitcnt" is folded into a literal mask
    return (uint32_t)(inBuffer.bitbuf & ~(~0ull << bitcnt));
#else
    // potentially v_lshrrev_b64 + v_sub_nc_b32 on AMD
    return (uint32_t)(inBuffer.bitbuf >> (inBuffer.bitcnt - bitcnt));
#endif
}

static inline void zstdgpu_Backward_BitBuffer_V0_Pop(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer_V0) inoutBuffer, uint32_t bitcnt)
{
    ZSTDGPU_ASSERT(inoutBuffer.bitcnt >= bitcnt);
#ifdef ZSTDGPU_USE_REVERSED_BIT_BUFFER_BITBUF
    // EXPERIMENT: store "bitbuf" reversed (by employing "v_bfrev_b32" on AMD)
    // potentially v_lshrrev_b64 + v_sub_nc_b32 on AMD
    inoutBuffer.bitbuf >>= bitcnt;
    inoutBuffer.bitcnt  -= bitcnt;
#else
    // potentially v_sub_nc_b32 + v_lshrrev_b64 + two v_and_b32 on AMD because there's no v_bfm_b64 for mask
    inoutBuffer.bitbuf &= ~(~0ull << (inoutBuffer.bitcnt - bitcnt));
    inoutBuffer.bitcnt  -= bitcnt;
#endif
}

ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS(Backward_BitBuffer_V0)

static inline void zstdgpu_Backward_BitBuffer_V0_Refill_Huffman(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer_V0) inoutBuffer, uint32_t bitcnt, uint32_t extrabits)
{
    if (inoutBuffer.hadlastrefill == false)
    {
        zstdgpu_Backward_BitBuffer_V0_Refill(inoutBuffer, bitcnt);
    }

    if (inoutBuffer.bitcnt < bitcnt)
    {
        inoutBuffer.bitcnt += extrabits;    // simply increment counter because upper bits are zeros
    }
}

static inline uint32_t zstdgpu_Backward_BitBuffer_V0_Get_Huffman(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer_V0) inoutBuffer, uint32_t bitcnt, uint32_t extrabits)
{
    if (inoutBuffer.hadlastrefill == false)
    {
        zstdgpu_Backward_BitBuffer_V0_Refill(inoutBuffer, bitcnt);
    }

    if (inoutBuffer.bitcnt < bitcnt)
    {
        inoutBuffer.bitcnt += extrabits;    // simply increment counter because upper bits are zeros
        inoutBuffer.bitbuf <<= extrabits;
    }

    uint32_t result = zstdgpu_Backward_BitBuffer_V0_Top(inoutBuffer, bitcnt);
    zstdgpu_Backward_BitBuffer_V0_Pop(inoutBuffer, bitcnt);
    return result;
}

// NOTE(jweinste): Backwards bitstream that loads aligned 64-bit elements (instead of 32-bit elements) per-lane needing refill.
struct zstdgpu_HuffmanStream
{
    ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer;
    uint32_t finalByteOffset;
    uint32_t lastByteOffset;

    uint32_t dataSpare;
    uint32_t numBitsSpare; // always strictly < maxBitsPerCode

    // Available bits are stored against the high-end of the U64.
    // This initially felt natural since the bitstream is read from MSB to LSB,
    // and it allows for Peek() to not need a 64-bit shift (although there might be more ALU elsewhere).
    //
    // Example: if there is one 5-bit code left (numBits0 = 5) with value 0b11111, then data0 = 0xF800'0000'0000'0000 (not 0x1F).
    //
    // @last_peek: On the (potentially more than just the) last call to RefillAndPeek(), there might be < maxBitsPerCode available
    // (only the actual number of bits for the last code), but every U64 that at contains at least one useful byte would already have
    // been loaded. When that last U64 is read (which may be in InitWithSegment()), we set numBits0 to UINT_MAX so the next
    // (and any subsequent reasonable amount) of RefillAndPeek() do not emit a load.
    uint64_t data0;
    uint32_t numBits0;

    uint32_t maxBitsPerCode;
    uint32_t _32MinusMaxBitsPerCode;
};

static inline void zstdgpu_HuffmanStream_InitWithSegment(ZSTDGPU_PARAM_INOUT(zstdgpu_HuffmanStream) stream, ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer, ZSTDGPU_PARAM_IN(zstdgpu_OffsetAndSize) segment, ZSTDGPU_PARAM_IN(uint32_t) maxBitsPerCode)
{
    // NOTE(jweinste): we could just load a single DWORD here to reduce codesize/ALU here in InitWithSegment(),
    // but we want to ensure that the DWORDx2 loads in RefillAndPeek() are 8-byte aligned.
    // Alignment might improve cache behavior, but it is mainly to potentially limit how many "OOB" bytes we read.

    const uint32_t lastByteIdx = (segment.offs + segment.size) - 1; // Byte index containing the flag.
    const uint32_t finalByteOffsetForU64 = segment.offs & -8;
    const uint32_t lastByteOffsetForU64 = lastByteIdx & -8;
    uint64_t data64 = zstdgpu_ByteOffsetLoadU64(buffer, lastByteOffsetForU64);

    // How many extra bytes we read.
    const uint32_t oobByteCount = 7 - (lastByteIdx & 7);
    // Shift left by [0:56] to put the byte with the flag on top.
    const uint32_t oobBitCount = oobByteCount * 8;
    data64 <<= oobBitCount;
    // Count number of leading zero bits above the flag (result in [0:7]).
    // There is no 64-bit version of v_clz_i32_u32 and uint32_t(data64 >> 32) is free since U64 is a pair of VGPRs.
    // Add one (reverse-subtract by 32, not 31) to also shift out the flag itself.
    const uint32_t nonDataBitCount = 32 - zstdgpu_FindFirstBitHiU32_Nonzero(uint32_t(data64 >> 32));
    data64 <<= nonDataBitCount;
    const uint32_t keptBitCount = 64 - (oobBitCount + nonDataBitCount); // could be 0

    stream.buffer          = buffer;
    stream.finalByteOffset = finalByteOffsetForU64;
    stream.lastByteOffset  = lastByteOffsetForU64;

    stream.dataSpare    = 0;
    stream.numBitsSpare = 0;

    stream.data0    = data64;
    stream.numBits0 = (finalByteOffsetForU64 == lastByteOffsetForU64) ? uint32_t(-1) : keptBitCount; // see @last_peek comment

    stream.maxBitsPerCode         = maxBitsPerCode;
    stream._32MinusMaxBitsPerCode = 32 - maxBitsPerCode;

    ZSTDGPU_ASSERT(1 <= maxBitsPerCode && maxBitsPerCode <= 11);
}

static inline uint32_t zstdgpu_HuffmanStream_RefillAndPeek(ZSTDGPU_PARAM_INOUT(zstdgpu_HuffmanStream) stream)
{
    // Need refill?
    if (stream.numBits0 < stream.maxBitsPerCode)
    {
        ZSTDGPU_ASSERT(stream.numBitsSpare == 0);
        ZSTDGPU_ASSERT(((stream.finalByteOffset | stream.lastByteOffset) & 7) == 0);
        ZSTDGPU_ASSERT(stream.finalByteOffset < stream.lastByteOffset);
        // Do refill.
        const uint32_t loadByteOffset = stream.lastByteOffset - sizeof(uint64_t);
        stream.dataSpare      = uint32_t(stream.data0 >> 32);
        stream.numBitsSpare   = stream.numBits0;
        stream.lastByteOffset = loadByteOffset;
        stream.data0          = zstdgpu_ByteOffsetLoadU64(stream.buffer, loadByteOffset);
        stream.numBits0       = (stream.finalByteOffset == loadByteOffset) ? uint32_t(-1) : 64; // see @last_peek comment
    }

    // Do Peek.
    const uint32_t k = stream._32MinusMaxBitsPerCode;
    const uint32_t data0_hiShift = k + stream.numBitsSpare;
    const uint32_t data0_hi = uint32_t(stream.data0 >> 32); // High U32 extract is free. U64 shift slower than U32. maxBitsPerCode <= 11.
    ZSTDGPU_ASSERT(data0_hiShift < 32u);
    return (stream.dataSpare >> k) | (data0_hi >> data0_hiShift);
}

static inline void zstdgpu_HuffmanStream_Consume(ZSTDGPU_PARAM_INOUT(zstdgpu_HuffmanStream) stream, ZSTDGPU_PARAM_IN(int) actualBitCount)
{
    ZSTDGPU_ASSERT(stream.maxBitsPerCode >= uint32_t(actualBitCount));

    int ns = stream.numBitsSpare;
    uint32_t data0Consumed = zstdgpu_MaxI32(0, actualBitCount - ns);
    stream.numBitsSpare    = zstdgpu_MaxI32(0, ns - actualBitCount);
    stream.dataSpare <<= actualBitCount;

    stream.numBits0 -= data0Consumed;
    stream.data0 <<= data0Consumed;
}

static inline void zstdgpu_Backward_BitBuffer_Init(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer) outBitBuffer, ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer, ZSTDGPU_PARAM_IN(zstdgpu_OffsetAndSize) segment)
{
    const uint32_t endbyte = segment.offs + segment.size - 1u;

    outBitBuffer.buffer = buffer;
    outBitBuffer.offset = endbyte & -4;
    outBitBuffer.bitbuf = zstdgpu_ByteOffsetLoadU32(buffer, outBitBuffer.offset);
    outBitBuffer.bitpos = 56u - ((endbyte & 3u) << 3u);
}

static inline bool zstdgpu_Backward_BitBuffer_CanRefill(ZSTDGPU_PARAM_IN(zstdgpu_Backward_BitBuffer) inBuffer, uint32_t bitcnt)
{
    ZSTDGPU_UNUSED(inBuffer);
    ZSTDGPU_UNUSED(bitcnt);
    ZSTDGPU_BREAK(); // TODO
    return true;
}

static inline void zstdgpu_Backward_BitBuffer_Refill(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer) inoutBuffer, uint32_t bitcnt)
{
    ZSTDGPU_UNUSED(bitcnt);

    // Refill is going to advance `offset` by 2 elements in case of 64 bits, but we don't handle it.
    ZSTDGPU_ASSERT(inoutBuffer.bitpos <= 63);

    // advance the offset by the number of full uints consumed in `bitbuf`
    // which is determined by the number of consumed bits in `bitpos`
    inoutBuffer.offset -= (inoutBuffer.bitpos >> 5u) * 4;

    // we mask `bitpos` so that it's either `32` or `0` to determine how many bits should be replaced
    // by newer bits loaded into lower 32-bits, then we load lower bits unconditionally,
    // so if `offset` was advanced above -- it's new bits, or it's just the same bits that are already loaded.
    //
    // NOTE (pamartis): on GPU, since bitbuf is usually 2 VGPRs, lower VGPR is alway "reloaded" but higher VGPR is either
    // stays the same of becomes lower VGPR (because of exactly 32-bit shift).
    //
    // TODO (pamartis): Because of the above, try storing bitbuf as 2 VGPRs as 2 explicit uint32_t values,
    // so "left shift" and "or" are avoided.
    inoutBuffer.bitbuf = (inoutBuffer.bitbuf << (inoutBuffer.bitpos & ~31u)) | zstdgpu_ByteOffsetLoadU32(inoutBuffer.buffer, inoutBuffer.offset);

    // we update `bitpos` so that it contains <= `31` bit
    inoutBuffer.bitpos &= 31u;
}

static inline uint32_t zstdgpu_Backward_BitBuffer_Top(ZSTDGPU_PARAM_IN(zstdgpu_Backward_BitBuffer) inBitBuffer, uint32_t bitcnt)
{
    ZSTDGPU_ASSERT(inBitBuffer.bitpos + bitcnt <= 64u);
    ZSTDGPU_ASSERT(bitcnt <= 64u - 31u);

    // shift consumed bits away
    const uint64_t remaining = inBitBuffer.bitbuf << inBitBuffer.bitpos;

    // return remaining >> (64u - bitcnt);
    // NOTE(pamartis): we replaced the above expression with the below expression to make sure `bitcnt == 0` works
    return (uint32_t)((remaining >> 1u) >> (63u - bitcnt));
}

static inline void zstdgpu_Backward_BitBuffer_Pop(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer) inoutBuffer, uint32_t bitcnt)
{
    inoutBuffer.bitpos += bitcnt;
}

static inline void zstdgpu_Backward_BitBuffer_InitWithSegment(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_BitBuffer) outBitBuffer, ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer, ZSTDGPU_PARAM_IN(zstdgpu_OffsetAndSize) segment)
{
    zstdgpu_Backward_BitBuffer_Init(outBitBuffer, buffer, segment);

    zstdgpu_Backward_BitBuffer_Refill(outBitBuffer, 32u);

    const uint64_t bitmask = zstdgpu_LoBitFieldWideMaskU64(64u - outBitBuffer.bitpos);
    outBitBuffer.bitbuf &= bitmask;
    const uint32_t highbit = zstdgpu_FindFirstBitHiU64(outBitBuffer.bitbuf);
    outBitBuffer.bitpos = 64u - highbit;
}

ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS(Backward_BitBuffer)

/*
 * Below is the implementation of the backward bit buffer that compares two implementation for correctness
 */
#ifndef ZSTDGPU_BACKWARD_BITBUF_REF
#   define ZSTDGPU_BACKWARD_BITBUF_REF(method) zstdgpu_Backward_BitBuffer_V0_##method
#else
#   error `ZSTDGPU_BACKWARD_BITBUF_REF` must not be defined.
#endif

#ifndef ZSTDGPU_BACKWARD_BITBUF_TST
#   define ZSTDGPU_BACKWARD_BITBUF_TST(method) zstdgpu_Backward_BitBuffer_##method
#else
#   error `ZSTDGPU_BACKWARD_BITBUF_TST` must not be defined.
#endif

static inline void zstdgpu_Backward_CmpBitBuffer_InitWithSegment(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_CmpBitBuffer) outBuffer, ZSTDGPU_RO_RAW_BUFFER(uint32_t) buffer, ZSTDGPU_PARAM_IN(zstdgpu_OffsetAndSize) segment)
{
    ZSTDGPU_BACKWARD_BITBUF_REF(InitWithSegment)(outBuffer.bbref, buffer, segment);
    ZSTDGPU_BACKWARD_BITBUF_TST(InitWithSegment)(outBuffer.bbtst, buffer, segment);
}

static inline bool zstdgpu_Backward_CmpBitBuffer_CanRefill(ZSTDGPU_PARAM_IN(zstdgpu_Backward_CmpBitBuffer) inBuffer, uint32_t bitcnt)
{
    const bool resultRef = ZSTDGPU_BACKWARD_BITBUF_REF(CanRefill)(inBuffer.bbref, bitcnt);
    const bool resultTst = ZSTDGPU_BACKWARD_BITBUF_TST(CanRefill)(inBuffer.bbtst, bitcnt);
    ZSTDGPU_ASSERT(resultRef == resultTst);
    return resultRef;
}

static inline void zstdgpu_Backward_CmpBitBuffer_Refill(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_CmpBitBuffer) inoutBuffer, uint32_t bitcnt)
{
    ZSTDGPU_BACKWARD_BITBUF_REF(Refill)(inoutBuffer.bbref, bitcnt);
    ZSTDGPU_BACKWARD_BITBUF_TST(Refill)(inoutBuffer.bbtst, bitcnt);
}

static inline uint32_t zstdgpu_Backward_CmpBitBuffer_Top(ZSTDGPU_PARAM_IN(zstdgpu_Backward_CmpBitBuffer) inBuffer, uint32_t bitcnt)
{
    const uint32_t resultRef = ZSTDGPU_BACKWARD_BITBUF_REF(Top)(inBuffer.bbref, bitcnt);
    const uint32_t resultTst = ZSTDGPU_BACKWARD_BITBUF_TST(Top)(inBuffer.bbtst, bitcnt);
    ZSTDGPU_ASSERT(resultRef == resultTst);
    return resultRef;
}

static inline void zstdgpu_Backward_CmpBitBuffer_Pop(ZSTDGPU_PARAM_INOUT(zstdgpu_Backward_CmpBitBuffer) inoutBuffer, uint32_t bitcnt)
{
    ZSTDGPU_BACKWARD_BITBUF_REF(Pop)(inoutBuffer.bbref, bitcnt);
    ZSTDGPU_BACKWARD_BITBUF_TST(Pop)(inoutBuffer.bbtst, bitcnt);
}

ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS(Backward_CmpBitBuffer)

#undef ZSTDGPU_BACKWARD_BITBUF_REF
#undef ZSTDGPU_BACKWARD_BITBUF_TST

#undef ZSTDGPU_BITBUF_DEFINE_STANDARD_METHODS

typedef struct zstdgpu_FseInfo
{
    uint32_t fseProbCountAndAccuracyLog2;   //<  [7:0] - `The number of probabilities` (== 256 is considered a corruption according to the reference parser).
                                            //< [15:8] - `accuracyLog2` defining the size of FSE table as `1 << accuracyLog2`
} zstdgpu_FseInfo;

static zstdgpu_FseInfo zstdgpu_CreateFseInfo(uint32_t symbolCount, uint32_t accuracyLog2)
{
    zstdgpu_FseInfo info;
    info.fseProbCountAndAccuracyLog2 = symbolCount | (accuracyLog2 << 8);
    return info;
}

static uint32_t zstdgpu_ComputeFseIndexHufW(uint32_t indexHufW, uint32_t /* cmpBlockCount */)
{
    return kzstdgpu_FseRleTableCount + indexHufW;
}

static uint32_t zstdgpu_ComputeFseIndexLLen(uint32_t indexLLen, uint32_t cmpBlockCount)
{
    return kzstdgpu_FseRleTableCount + cmpBlockCount + indexLLen;
}

static uint32_t zstdgpu_ComputeFseIndexOffs(uint32_t indexOffs, uint32_t cmpBlockCount)
{
    return kzstdgpu_FseRleTableCount + (2 * cmpBlockCount + 1) + indexOffs;
}

static uint32_t zstdgpu_ComputeFseIndexMLen(uint32_t indexMLen, uint32_t cmpBlockCount)
{
    return kzstdgpu_FseRleTableCount + (3 * cmpBlockCount + 2) + indexMLen;
}

static uint32_t zstdgpu_ComputeFseDataStartHufW(uint32_t indexHufW, uint32_t /* cmpBlockCount */)
{
    return kzstdgpu_FseRleTableCount + indexHufW * kzstdgpu_FseElemMaxCount_HufW;
}

static uint32_t zstdgpu_ComputeFseDataStartLLen(uint32_t indexLLen, uint32_t cmpBlockCount)
{
    return kzstdgpu_FseRleTableCount + cmpBlockCount * kzstdgpu_FseElemMaxCount_HufW + indexLLen * kzstdgpu_FseElemMaxCount_LLen;
}

static uint32_t zstdgpu_ComputeFseDataStartOffs(uint32_t indexOffs, uint32_t cmpBlockCount)
{
    return (kzstdgpu_FseRleTableCount + kzstdgpu_FseElemMaxCount_LLen) + cmpBlockCount * (kzstdgpu_FseElemMaxCount_HufW + kzstdgpu_FseElemMaxCount_LLen) + indexOffs * kzstdgpu_FseElemMaxCount_Offs;
}

static uint32_t zstdgpu_ComputeFseDataStartMLen(uint32_t indexMLen, uint32_t cmpBlockCount)
{
    return (kzstdgpu_FseRleTableCount + kzstdgpu_FseElemMaxCount_LLen + kzstdgpu_FseElemMaxCount_Offs) + cmpBlockCount * (kzstdgpu_FseElemMaxCount_HufW + kzstdgpu_FseElemMaxCount_LLen + kzstdgpu_FseElemMaxCount_Offs) + indexMLen * kzstdgpu_FseElemMaxCount_MLen;
}

static uint32_t zstdgpu_ComputeFseDataStartFromFseIndexLLen(uint32_t fseIndex, uint32_t cmpBlockCount)
{
    return (fseIndex < kzstdgpu_FseRleTableCount) ? fseIndex
                                                  : zstdgpu_ComputeFseDataStartLLen(fseIndex - zstdgpu_ComputeFseIndexLLen(0, cmpBlockCount), cmpBlockCount);
}

static uint32_t zstdgpu_ComputeFseDataStartFromFseIndexOffs(uint32_t fseIndex, uint32_t cmpBlockCount)
{
    return (fseIndex < kzstdgpu_FseRleTableCount) ? fseIndex
                                                  : zstdgpu_ComputeFseDataStartOffs(fseIndex - zstdgpu_ComputeFseIndexOffs(0, cmpBlockCount), cmpBlockCount);
}

static uint32_t zstdgpu_ComputeFseDataStartFromFseIndexMLen(uint32_t fseIndex, uint32_t cmpBlockCount)
{
    return (fseIndex < kzstdgpu_FseRleTableCount) ? fseIndex
                                                  : zstdgpu_ComputeFseDataStartMLen(fseIndex - zstdgpu_ComputeFseIndexMLen(0, cmpBlockCount), cmpBlockCount);
}

typedef struct zstdgpu_CompressedBlockData
{
    zstdgpu_OffsetAndSize literal;   //< The offset and the size of the uncompressed literal
                                    //< RAW/RLE literal -- the offset in the source compressed data stream
                                    //< CMP literal -- the offset in the uncompressed data stream

    uint32_t litStreamIndex;  //< CONSIDER REMOVING: The starting index of Huffman compressed literal streams in `compressedLiteralRefs`
    uint32_t seqStreamIndex;

    uint32_t fseTableIndexHufW; //< Can be an in range defined by 'zstdgpu_ComputeFseIndexHufW' or kzstdgpu_FseProbTableIndex_Unused/Repeat
    uint32_t fseTableIndexLLen; //< Can be an in range [0; 255] - meaning RLE, or in range defined by 'zstdgpu_ComputeFseIndexLLen' or kzstdgpu_FseProbTableIndex_Unused/Repeat
    uint32_t fseTableIndexOffs; //< Can be an in range [0; 255] - meaning RLE, or in range defined by 'zstdgpu_ComputeFseIndexOffs' or kzstdgpu_FseProbTableIndex_Unused/Repeat
    uint32_t fseTableIndexMLen; //< Can be an in range [0; 255] - meaning RLE, or in range defined by 'zstdgpu_ComputeFseIndexMLen' or kzstdgpu_FseProbTableIndex_Unused/Repeat
} zstdgpu_CompressedBlockData;

static inline void zstdgpu_Init_CompressedBlockData(ZSTDGPU_PARAM_INOUT(zstdgpu_CompressedBlockData) outBlockData)
{
    zstdgpu_Init_OffsetAndSize(outBlockData.literal);

    outBlockData.litStreamIndex = ~0u;
    outBlockData.seqStreamIndex = ~0u;

    outBlockData.fseTableIndexHufW = kzstdgpu_FseProbTableIndex_Unused;
    outBlockData.fseTableIndexLLen = kzstdgpu_FseProbTableIndex_Unused;
    outBlockData.fseTableIndexOffs = kzstdgpu_FseProbTableIndex_Unused;
    outBlockData.fseTableIndexMLen = kzstdgpu_FseProbTableIndex_Unused;
}

typedef struct zstdgpu_TableIndexLookback
{
    uint32_t fseTableIndexHufW; //< this index is a bit special:
                                //< it can be the index of FSE for Huffman Weights (which is identical to compressed Huffman Weights) or
                                //< `compresssedBlockCount - uncompressed Huffman Weights index`
    uint32_t fseTableIndexLLen;
    uint32_t fseTableIndexOffs;
    uint32_t fseTableIndexMLen;
} zstdgpu_TableIndexLookback;

typedef struct zstdgpu_LitStreamInfo
{
    zstdgpu_OffsetAndSize src;       //< offset and size of the compressed literal stream in the source compressed data
    zstdgpu_OffsetAndSize dst;       //< offset and size of the decompressed literal stream in the transient uncompressed literal buffer
} zstdgpu_LitStreamInfo;

typedef struct zstdgpu_CompressedLiteralHuffmanBucket
{
    uint32_t huffmanBucketIndex;    //< The index of the Huffman Weights table used to decode this literal stream
    uint32_t huffmanBucketOffset;   //< The offset of the this literal within a block of literals with the same Huffman table
} zstdgpu_CompressedLiteralHuffmanBucket;

typedef struct zstdgpu_SeqStreamInfo
{
    zstdgpu_OffsetAndSize    src;        //< The offset and size of compressed sequence in the source(original, compressed) blob (in bytes)

    // TODO: consider storing each of offsets in a separate stream, so the index propagation can be done per stream in parallel,
    // but this would require "compaction" to be done in "Parse" phase.
    // also need to remove indices from `zstdgpu_CompressedBlockData`
    uint32_t                fseLLen;    //< The index of FSE table to decode LLen
    uint32_t                fseOffs;    //< The index of FSE table to decode Offs
    uint32_t                fseMLen;    //< The index of FSE table to decode MLen
    uint32_t                blockId;    //< The index of parent compressed block
} zstdgpu_SeqStreamInfo;

static inline uint32_t zstdgpu_GetLookbackBlockCount(uint32_t elemCount)
{
    return ZSTDGPU_TG_COUNT(elemCount, kzstdgpu_WaveSize_Min);
}

static inline uint32_t zstdgpu_GetHufFseTableIndexLookbackUInt32Count(uint32_t compressedBlockCount)
{
    return zstdgpu_GetLookbackBlockCount(compressedBlockCount) * (sizeof(zstdgpu_TableIndexLookback) / sizeof(uint32_t));
}

static inline uint32_t zstdgpu_InitResources_GetDispatchSizeX(uint32_t initResourceStage)
{
    uint32_t maxThreads = 1; // Counter init is single-threaded (struct assignment)

    if (initResourceStage == 1)
    {
        // should be sufficient for default LLen Prob table
        if (maxThreads < kzstdgpu_FseDefaultProbCount_LLen)
            maxThreads = kzstdgpu_FseDefaultProbCount_LLen;

        // should be sufficient for default Offs Prob table
        if (maxThreads < kzstdgpu_FseDefaultProbCount_Offs)
            maxThreads = kzstdgpu_FseDefaultProbCount_Offs;

        // should be sufficient for default MLen Prob table
        if (maxThreads < kzstdgpu_FseDefaultProbCount_MLen)
            maxThreads = kzstdgpu_FseDefaultProbCount_MLen;

        // should be sufficient for RLE FSE table entries
        if (maxThreads < kzstdgpu_FseRleTableCount)
            maxThreads = kzstdgpu_FseRleTableCount;
    }
    return (maxThreads + kzstdgpu_TgSizeX_InitCounters - 1) / kzstdgpu_TgSizeX_InitCounters;
}

#define ZSTDGPU_PARSE_FRAMES_SRT()                                                                      \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , CompressedData                , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_OffsetAndSize                , FramesRefs                    , 1)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 0)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_FrameInfo                    , Frames                        , 1)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerFrameBlockCountRAW         , 2)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerFrameBlockCountRLE         , 3)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerFrameBlockCountCMP         , 4)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerFrameBlockCountAll         , 5)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerFrameBlockSizesRAW         , 6)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerFrameBlockSizesRLE         , 7)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , RawBlockSizePrefix            , 8)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , RleBlockSizePrefix            , 9)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_OffsetAndSize                , BlocksRAWRefs                 ,10)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_OffsetAndSize                , BlocksRLERefs                 ,11)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_OffsetAndSize                , BlocksCMPRefs                 ,12)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , BlockSizePrefix               ,13)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , GlobalBlockIndexPerRawBlock   ,14)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , GlobalBlockIndexPerRleBlock   ,15)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , GlobalBlockIndexPerCmpBlock   ,16)

#define ZSTDGPU_INIT_RESOURCES_SRT()                                                                    \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(int32_t, int16_t               , FseProbsDefault               , 0)    \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(int32_t, int16_t               , FseProbs                      , 0)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_FseInfo                      , FseInfos                      , 1)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 2)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , FseElems                      , 3)

#define ZSTDGPU_PARSE_COMPRESSED_BLOCKS_SRT()                                                           \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , CompressedData                , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_OffsetAndSize                , BlocksCMPRefs                 , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerFrameBlockCountCMP         , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , GlobalBlockIndexPerCmpBlock   , 3)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 0)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_FseInfo                      , FseInfos                      , 1)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_CompressedBlockData          , CompressedBlocks              , 2)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_OffsetAndSize                , HufRefs                       , 3)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , LitStreamEndPerHuffmanTable   , 4)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_LitStreamInfo                , LitRefs                       , 5)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_SeqStreamInfo                , SeqRefs                       , 6)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_CompressedLiteralHuffmanBucket,LitStreamBuckets              , 7)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , BlockSizePrefix               , 8)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerFrameSeqStreamMinIdx       , 9)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerSeqStreamSeqStart          , 10)   \
    \
    ZSTDGPU_RW_BUFFER_DECL_GLC(zstdgpu_TableIndexLookback       , TableIndexLookback            , 11)   \
    ZSTDGPU_RW_BUFFER_DECL_GLC(uint32_t                         , SeqCountPrefixLookback        , 12)   \
    ZSTDGPU_RW_BUFFER_DECL_GLC(uint32_t                         , BlockSeqCountPrefixLookback   , 13)   \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(int32_t, int16_t               , FseProbs                      , 14)   \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeightCount, 15)

#define ZSTDGPU_INIT_FSE_TABLE_SRT()                                                                    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_FseInfo                      , FseInfos                      , 0)    \
    \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(int32_t, int16_t               , FseProbs                      , 1)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , FseElems                      , 0)

#define ZSTDGPU_DECOMPRESS_HUFFMAN_WEIGHTS_SRT()                                                        \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 0)    \
    ZSTDGPU_RO_RAW_BUFFER_DECL(uint32_t                         , CompressedData                , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_OffsetAndSize                , HufRefs                       , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_FseInfo                      , FseInfos                      , 3)    \
    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , FseElems                      , 4)    \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeights    , 0)    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeightCount, 1)

#define ZSTDGPU_DECODE_HUFFMAN_WEIGHTS_SRT()                                                            \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , CompressedData                , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_OffsetAndSize                , HufRefs                       , 2)    \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeights    , 0)    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeightCount, 1)

#define ZSTDGPU_INIT_HUFFMAN_TABLE_SRT()                                                                \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeights    , 0)    \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeightCount, 1)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , HuffmanTableInfo              , 0)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , HuffmanTableCodeAndSymbol     , 1)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , HuffmanTableRankIndex         , 2)

#define ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_SRT()                                        \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , LitStreamEndPerHuffmanTable   , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , LitGroupEndPerHuffmanTable    , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , LitStreamRemap                , 3)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_LitStreamInfo                , LitRefs                       , 4)    \
    ZSTDGPU_RO_RAW_BUFFER_DECL(uint32_t                         , CompressedData                , 5)    \
    \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeights    , 6)    \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedHuffmanWeightCount, 7)    \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedLiterals          , 0)

#define ZSTDGPU_DECOMPRESS_LITERALS_SRT()                                                               \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , LitStreamEndPerHuffmanTable   , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , LitGroupEndPerHuffmanTable    , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , LitStreamRemap                , 3)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_LitStreamInfo                , LitRefs                       , 4)    \
    ZSTDGPU_RO_RAW_BUFFER_DECL(uint32_t                         , CompressedData                , 5)    \
    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , HuffmanTableInfo              , 6)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , HuffmanTableCodeAndSymbol     , 7)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , HuffmanTableRankIndex         , 8)    \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedLiterals          , 0)    \
    \
    ZSTDGPU_RW_BUFFER_ALIAS_DECL(uint32_t                       , DecompressedLiterals, Dwords  , 1)

#define ZSTDGPU_DECOMPRESS_SEQUENCES_SRT()                                                              \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 0)    \
    ZSTDGPU_RO_RAW_BUFFER_DECL(uint32_t                         , CompressedData                , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_SeqStreamInfo                , SeqRefs                       , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_FseInfo                      , FseInfos                      , 3)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerSeqStreamSeqStart          , 4)    \
    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , FseElems                      , 5)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , DecompressedSequenceLLen      , 0)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , DecompressedSequenceMLen      , 1)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , DecompressedSequenceOffs      , 2)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , BlockSizePrefix               , 3)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerSeqStreamFinalOffset1      , 4)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerSeqStreamFinalOffset2      , 5)    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , PerSeqStreamFinalOffset3      , 6)

#define ZSTDGPU_FINALISE_SEQUENCE_OFFSETS_SRT()                                                         \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerSeqStreamFinalOffset1      , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerSeqStreamFinalOffset2      , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerSeqStreamFinalOffset3      , 3)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerSeqStreamSeqStart          , 4)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerFrameBlockCountAll         , 5)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerFrameSeqStreamMinIdx       , 6)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_SeqStreamInfo                , SeqRefs                       , 7)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , DecompressedSequenceOffs      , 0)

#define ZSTDGPU_EXECUTE_SEQUENCES_SRT()                                                                 \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(uint32_t, uint8_t              , CompressedData                , 0)    \
    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerFrameBlockCountCMP         , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerFrameBlockCountAll         , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , BlockSizePrefix               , 3)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_OffsetAndSize                , UnCompressedFramesRefs        , 4)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , DecompressedSequenceLLen      , 5)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , DecompressedSequenceMLen      , 6)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , DecompressedSequenceOffs      , 7)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , GlobalBlockIndexPerCmpBlock   , 8)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerSeqStreamSeqStart          , 9)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_CompressedBlockData          , CompressedBlocks              ,10)    \
    \
    ZSTDGPU_RO_TYPED_BUFFER_DECL(uint32_t, uint8_t              , DecompressedLiterals          ,11)    \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , UnCompressedFramesData        , 0)    \
    ZSTDGPU_RW_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 1)

#define ZSTDGPU_COMPUTE_DEST_SEQUENCE_OFFSETS_SRT()                                                     \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_Counters                     , Counters                      , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerFrameBlockCountAll         , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , BlockSizePrefix               , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_OffsetAndSize                , UnCompressedFramesRefs        , 3)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , DecompressedSequenceMLen      , 4)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerSeqStreamSeqStart          , 5)    \
    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_SeqStreamInfo                , SeqRefs                       , 6)    \
    \
    ZSTDGPU_RW_BUFFER_DECL(uint32_t                             , DestSequenceOffsets           , 0)

#define ZSTDGPU_MEMSET_MEMCPY_SRT()                                                                     \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , CompressedData                , 0)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , BlockSizePrefix               , 1)    \
    ZSTDGPU_RO_BUFFER_DECL(uint32_t                             , PerFrameBlockCountAll         , 2)    \
    ZSTDGPU_RO_BUFFER_DECL(zstdgpu_OffsetAndSize                , UnCompressedFramesRefs        , 3)    \
    \
    ZSTDGPU_RW_TYPED_BUFFER_DECL(uint32_t, uint8_t              , UnCompressedFramesData        , 0)

#define ZSTDGPU_SRT_LIST()                                                                                          \
    ZSTDGPU_SRT(InitResources                           , ZSTDGPU_INIT_RESOURCES_SRT())                             \
    ZSTDGPU_SRT(ParseFrames                             , ZSTDGPU_PARSE_FRAMES_SRT())                               \
    ZSTDGPU_SRT(ParseCompressedBlocks                   , ZSTDGPU_PARSE_COMPRESSED_BLOCKS_SRT())                    \
    ZSTDGPU_SRT(InitFseTable                            , ZSTDGPU_INIT_FSE_TABLE_SRT())                             \
    ZSTDGPU_SRT(DecompressHuffmanWeights                , ZSTDGPU_DECOMPRESS_HUFFMAN_WEIGHTS_SRT())                 \
    ZSTDGPU_SRT(DecodeHuffmanWeights                    , ZSTDGPU_DECODE_HUFFMAN_WEIGHTS_SRT())                     \
    ZSTDGPU_SRT(InitHuffmanTable                        , ZSTDGPU_INIT_HUFFMAN_TABLE_SRT())                         \
    ZSTDGPU_SRT(InitHuffmanTableAndDecompressLiterals   , ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_SRT()) \
    ZSTDGPU_SRT(DecompressLiterals                      , ZSTDGPU_DECOMPRESS_LITERALS_SRT())                        \
    ZSTDGPU_SRT(DecompressSequences                     , ZSTDGPU_DECOMPRESS_SEQUENCES_SRT())                       \
    ZSTDGPU_SRT(FinaliseSequenceOffsets                 , ZSTDGPU_FINALISE_SEQUENCE_OFFSETS_SRT())                  \
    ZSTDGPU_SRT(MemsetMemcpy                            , ZSTDGPU_MEMSET_MEMCPY_SRT())                              \
    ZSTDGPU_SRT(ExecuteSequences                        , ZSTDGPU_EXECUTE_SEQUENCES_SRT())                          \
    ZSTDGPU_SRT(ComputeDestSequenceOffsets              , ZSTDGPU_COMPUTE_DEST_SEQUENCE_OFFSETS_SRT())

#define ZSTDGPU_RO_RAW_BUFFER_DECL(type, name, index)                               ZSTDGPU_RO_RAW_BUFFER(type)                     in##name;

#define ZSTDGPU_RO_BUFFER_DECL(type, name, index)                                   ZSTDGPU_RO_BUFFER(type)                         in##name;
#define ZSTDGPU_RW_BUFFER_DECL(type, name, index)                                   ZSTDGPU_RW_BUFFER(type)                         inout##name;
#define ZSTDGPU_RW_BUFFER_DECL_GLC(type, name, index)                               ZSTDGPU_RW_BUFFER_GLC(type)                     inout##name;

#define ZSTDGPU_RO_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  ZSTDGPU_RO_TYPED_BUFFER(hlsl_type, type)        in##name;
#define ZSTDGPU_RW_TYPED_BUFFER_DECL(hlsl_type, type, name, index)                  ZSTDGPU_RW_TYPED_BUFFER(hlsl_type, type)        inout##name;
#define ZSTDGPU_RW_TYPED_BUFFER_DECL_GLC(hlsl_type, type, name, index)              ZSTDGPU_RW_TYPED_BUFFER_GLC(hlsl_type, type)    inout##name;

#define ZSTDGPU_RO_BUFFER_ALIAS_DECL(type, name, alias, index)                      ZSTDGPU_RO_BUFFER(type)                         in##name##_##alias;
#define ZSTDGPU_RW_BUFFER_ALIAS_DECL(type, name, alias, index)                      ZSTDGPU_RW_BUFFER(type)                         inout##name##_##alias;
#define ZSTDGPU_RW_BUFFER_ALIAS_DECL_GLC(type, name, alias, index)                  ZSTDGPU_RW_BUFFER_GLC(type)                     inout##name##_##alias;

#define ZSTDGPU_RO_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     ZSTDGPU_RO_TYPED_BUFFER(hlsl_type, type)        in##name##_##alias;
#define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL(hlsl_type, type, name, alias, index)     ZSTDGPU_RW_TYPED_BUFFER(hlsl_type, type)        inout##name##_##alias;
#define ZSTDGPU_RW_TYPED_BUFFER_ALIAS_DECL_GLC(hlsl_type, type, name, alias, index) ZSTDGPU_RW_TYPED_BUFFER_GLC(hlsl_type, type)    inout##name##_##alias;

typedef struct zstdgpu_ParseFrames_SRT
{
    ZSTDGPU_PARSE_FRAMES_SRT()

    uint32_t    frameCount;
    uint32_t    compressedBufferSizeInBytes;
    uint32_t    countBlocksOnly;
} zstdgpu_ParseFrames_SRT;

typedef struct zstdgpu_InitResources_SRT
{
    ZSTDGPU_INIT_RESOURCES_SRT()
    uint32_t    allBlockCount;
    uint32_t    cmpBlockCount;
    uint32_t    frameCount;
    uint32_t    initResourcesStage;
} zstdgpu_InitResources_SRT;

typedef struct zstdgpu_ParseCompressedBlocks_SRT
{
    ZSTDGPU_PARSE_COMPRESSED_BLOCKS_SRT()
    uint32_t    compressedBlockCount;
    uint32_t    compressedBufferSizeInBytes;
    uint32_t    frameCount;
} zstdgpu_ParseCompressedBlocks_SRT;

typedef struct zstdgpu_InitFseTable_SRT
{
    ZSTDGPU_INIT_FSE_TABLE_SRT()
    uint32_t    tableStartIndex;
    uint32_t    tableDataStart;
    uint32_t    tableDataCount;
} zstdgpu_InitFseTable_SRT;

typedef struct zstdgpu_DecompressHuffmanWeights_SRT
{
    ZSTDGPU_DECOMPRESS_HUFFMAN_WEIGHTS_SRT()
} zstdgpu_DecompressHuffmanWeights_SRT;

typedef struct zstdgpu_DecodeHuffmanWeights_SRT
{
    ZSTDGPU_DECODE_HUFFMAN_WEIGHTS_SRT()
    uint32_t    compressedBlockCount;
    uint32_t    compressedBufferSizeInBytes;
} zstdgpu_DecodeHuffmanWeights_SRT;

typedef struct zstdgpu_InitHuffmanTable_SRT
{
    ZSTDGPU_INIT_HUFFMAN_TABLE_SRT();
} zstdgpu_InitHuffmanTable_SRT;

typedef struct zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT
{
    ZSTDGPU_INIT_HUFFMAN_TABLE_AND_DECOMPRESS_LITERALS_SRT()
    uint32_t    huffmanTableSlotCount;
} zstdgpu_InitHuffmanTable_And_DecompressLiterals_SRT;

typedef struct zstdgpu_DecompressLiterals_SRT
{
    ZSTDGPU_DECOMPRESS_LITERALS_SRT()
    uint32_t    huffmanTableSlotCount;
} zstdgpu_DecompressLiterals_SRT;

typedef struct zstdgpu_DecompressSequences_SRT
{
    ZSTDGPU_DECOMPRESS_SEQUENCES_SRT();
} zstdgpu_DecompressSequences_SRT;

typedef struct zstdgpu_FinaliseSequenceOffsets_SRT
{
    ZSTDGPU_FINALISE_SEQUENCE_OFFSETS_SRT();
} zstdgpu_FinaliseSequenceOffsets_SRT;

typedef struct zstdgpu_ExecuteSequences_SRT
{
    ZSTDGPU_EXECUTE_SEQUENCES_SRT();
} zstdgpu_ExecuteSequences_SRT;

typedef struct zstdgpu_ComputeDestSequenceOffsets_SRT
{
    ZSTDGPU_COMPUTE_DEST_SEQUENCE_OFFSETS_SRT();
} zstdgpu_ComputeDestSequenceOffsets_SRT;

#include "zstdgpu_srt_decl_undef.h"

#endif // #define ZSTDGPU_STRUCTS_H
