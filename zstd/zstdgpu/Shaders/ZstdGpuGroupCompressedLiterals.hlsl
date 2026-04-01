/**
 * ZstdGpuGroupCompressedLiterals.hlsl
 *
 * A compute shader that groups compressed literals by identical Huffman table index
 * by using the input prefix sum of the number of literals per every Huffman table index
 * and the relative index of the literal in the group of literals with the same Huffman table index
 *
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

#include "../zstdgpu_shaders.h"

StructuredBuffer<uint32_t>                              ZstdLitStreamCountPrefix    : register(t0);
StructuredBuffer<zstdgpu_CompressedLiteralHuffmanBucket> ZstdLitStreamHuffmanBuckets : register(t1);
StructuredBuffer<zstdgpu_Counters>                      ZstdCounters                : register(t2);
RWStructuredBuffer<uint32_t>                            ZstdLitStreamMap            : register(u0);

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
};

ConstantBuffer<Consts> Constants : register(b0);

[RootSignature("SRV(t0), SRV(t1), SRV(t2), UAV(u0), RootConstants(b0, num32BitConstants=2)")]
[numthreads(32, 1, 1)]
void main(uint2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
    const uint32_t groupId = zstdgpu_ConvertTo32BitGroupId(groupId2, Constants.tgOffset);
    const uint32_t litStreamId = groupId * 32 + i;
    if (litStreamId >= ZstdCounters[0].HUF_Streams)
        return;

    const zstdgpu_CompressedLiteralHuffmanBucket bucket = ZstdLitStreamHuffmanBuckets[litStreamId];

    uint32_t groupStart = 0;
    if (bucket.huffmanBucketIndex != 0)
    {
        groupStart = ZstdLitStreamCountPrefix[bucket.huffmanBucketIndex - 1];
    }
    const uint32_t groupOffset = bucket.huffmanBucketOffset;

    ZstdLitStreamMap[groupStart + groupOffset] = litStreamId;
}
