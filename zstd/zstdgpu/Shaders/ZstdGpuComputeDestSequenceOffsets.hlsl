/**
 * ZstdGpuComputeDestSequenceOffsets.hlsl
 *
 * An experimental shader to compute offsets of 'sequence' bytes in the destination stream.
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

struct Consts
{
    uint32_t tgOffset;
    uint32_t workItemCount;
};

ConstantBuffer<Consts>          Constants                           : register(b0);

#include "../zstdgpu_srt_decl_bind.h"
ZSTDGPU_COMPUTE_DEST_SEQUENCE_OFFSETS_SRT()
#include "../zstdgpu_srt_decl_undef.h"

#define NUM_THREADS 256

[RootSignature("DescriptorTable(SRV(t0, numDescriptors=7), UAV(u0, numDescriptors=1)), RootConstants(b0, num32BitConstants=2)")]
[numthreads(NUM_THREADS, 1, 1)]
void main(uint2 groupId2 : SV_GroupId, uint i : SV_GroupThreadId)
{
#if defined(__XBOX_SCARLETT) || defined(__XBOX_ONE)
    const uint32_t groupId = groupId2.x;
#else
    const uint32_t groupId = (Constants.tgOffset + groupId2.y * 65535 + groupId2.x);
#endif
    i += groupId * NUM_THREADS;

    zstdgpu_ComputeDestSequenceOffsets_SRT srt;

    #include "../zstdgpu_srt_decl_copy.h"
    ZSTDGPU_COMPUTE_DEST_SEQUENCE_OFFSETS_SRT()
    #include "../zstdgpu_srt_decl_undef.h"

    const uint32_t seqIdx = i;
    const uint32_t seqStreamCnt = srt.inCounters[0].Seq_Streams;
    const uint32_t seqStreamIdx = zstdgpu_BinarySearch(srt.inPerSeqStreamSeqStart, 0, seqStreamCnt, seqIdx);
    const uint32_t seqIdxBeg = srt.inPerSeqStreamSeqStart[seqStreamIdx];

    uint32_t seqSize = srt.inDecompressedSequenceMLen[seqIdx];
    uint32_t seqOffs = 0;
    ZSTDGPU_BRANCH if (seqIdx > seqIdxBeg)
    {
        seqOffs = srt.inDecompressedSequenceMLen[seqIdx - 1];
    }
    seqSize -= seqOffs;

    const uint32_t blockIdx = srt.inSeqRefs[seqStreamIdx].blockId;

    const uint32_t frameCnt = srt.inCounters[0].Frames;
    const uint32_t frameIdx = zstdgpu_BinarySearch(srt.inPerFrameBlockCountAll, 0, frameCnt, blockIdx);

    const zstdgpu_OffsetAndSize dstFrameOffsAndSize = srt.inUnCompressedFramesRefs[frameIdx];

    const uint32_t firstFrameBlockIdx = srt.inPerFrameBlockCountAll[frameIdx];

    uint32_t firstFrameBlockOfs = 0;
    // NOTE(pamartis): Without `ZSTDGPU_BRANCH`, there's out-of-bounds `ZstdInBlockSizePrefix` access detected by validation layer when
    // DXC used: "Version: dxcompiler.dll: 1.6 - 1.6.2112.16 (e8295973c); dxil.dll: 1.6(101.6.2112.13)"
    ZSTDGPU_BRANCH if (firstFrameBlockIdx > 0)
    {
        firstFrameBlockOfs = srt.inBlockSizePrefix[firstFrameBlockIdx - 1];
    }

    uint32_t blockOfs = 0;
    // NOTE(pamartis): Without `ZSTDGPU_BRANCH`, there's out-of-bounds `ZstdInBlockSizePrefix` access detected by validation layer when
    // DXC used: "Version: dxcompiler.dll: 1.6 - 1.6.2112.16 (e8295973c); dxil.dll: 1.6(101.6.2112.13)"
    ZSTDGPU_BRANCH if (blockIdx > 0)
    {
        blockOfs = srt.inBlockSizePrefix[blockIdx - 1];
    }
    const uint32_t blockByteBeg = dstFrameOffsAndSize.offs + (blockOfs - firstFrameBlockOfs) + seqOffs;

    srt.inoutDestSequenceOffsets[i] = blockByteBeg;
}
