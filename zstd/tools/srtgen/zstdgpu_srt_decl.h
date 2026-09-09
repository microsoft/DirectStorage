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
 * zstdgpu_srt_decl.h
 *
 * Declarative definition of every kernel's Shader Resource Table (SRT) - a mechanism to describe
 * resource bindings from different perspectives:
 *      - how resources are used on GPU (in a shader)
 *      - how resources are used on CPU (in a C++ code)
 *
 * This mechanism also aims to avoid limitation of reflection API not carrying certain information
 * about resource usage on GPU/in a shader.
 *
 * This file is the single source of truth for kernel resource bindings.
 * Its macro lists are expanded by zstdgpu_srt_tool.c which emits:
 *
 *  1. ZstdGpuSrt_BindGroup_<GroupName>.h that contains:
 *      - a fragment of HLSL root signature for a list of resources
 *      - a declaration of HLSL resources with explicit register/space assignment
 *      - a helper function to assign HLSL resources to SRT-based structure (a way to access resources in a unified way in C++ / HLSL)
 *      - a helper function to assign C++ resources to SRT-based structures
 *
 *  2. ZstdGpuSrt_<SrtName>.h that contains
 *      - a list of ZstdGpuSrt_BindGroup_<GroupName>.h it relies on
 *      - the final HLSL root signature
 *      - declaration of HLSL resources which aren't part of any bind group
 *      - declaration of HLSL constant buffer / constant buffer structure containing all the constants
 *      - helper functions to fully initialise SRT-based structure from HLSL side (from resource bindings) and C++ side (a structure holding all resources)
 *
 *  3. zstdgpu_srt_structs.h that contains SRT-based structs shared betweeb C++ / HLSL
 *
 *  4. zstdgpu_srt_bind.h that creation of D3D12 descriptor tables and snippets to fully bind SRT-declared resources to a D3D12 command list
 */

#ifndef ZSTDGPU_SRT_DECL_H
#define ZSTDGPU_SRT_DECL_H

ZSTDGPU_SRT_BIND_GROUP_BEGIN(ParseFrames, Stage0|Stage1)
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , CompressedData                )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_OffsetAndSize         , FramesRefs                    )

    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerFrameBlockCountRAW         )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerFrameBlockCountRLE         )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerFrameBlockCountCMP         )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerFrameBlockCountAll         )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , RawBlockSizePrefix            )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , RleBlockSizePrefix            )

    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_OffsetAndSize         , BlocksRAWRefs                 )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_OffsetAndSize         , BlocksRLERefs                 )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_OffsetAndSize         , BlocksCMPRefs                 )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , BlockSizePrefix               )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , GlobalBlockIndexPerRawBlock   )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , GlobalBlockIndexPerRleBlock   )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , GlobalBlockIndexPerCmpBlock   )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(LiteralStreams, Stage2)
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , LitGroupEndPerHuffmanTable    )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_LitStreamInfo         , LitRefs                       )
    ZSTDGPU_SRT_BUF_RO_BYTE(CompressedData)
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , HufWIdToHufLitId              )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , HufLitIdToLitStreamId         )
    ZSTDGPU_SRT_BUF_RW_TYPED(uint32_t, uint8_t              , DecompressedLiterals          )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(HuffmanTable, Stage2)
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , HuffmanTableInfo              )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , HuffmanTableCodeAndSymbol     )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , HuffmanTableRankIndex         )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(LiteralDwords, Stage2)
    ZSTDGPU_SRT_BUF_RW_STRUCT_ALIAS(uint32_t                , DecompressedLiterals, Dwords  )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(HuffmanWeights, Stage2)
    ZSTDGPU_SRT_BUF_RO_TYPED(uint32_t, uint8_t              , DecompressedHuffmanWeights    )
    ZSTDGPU_SRT_BUF_RO_TYPED(uint32_t, uint8_t              , DecompressedHuffmanWeightCount)
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(HuffmanWeightsWrite, Stage2)
    ZSTDGPU_SRT_BUF_RW_TYPED(uint32_t, uint8_t              , DecompressedHuffmanWeights    )
    ZSTDGPU_SRT_BUF_RW_TYPED(uint32_t, uint8_t              , DecompressedHuffmanWeightCount)
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(HuffmanTableWrite, Stage2)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , HuffmanTableInfo              )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , HuffmanTableCodeAndSymbol     )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , HuffmanTableRankIndex         )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(SequenceOutputs, Stage2)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , DecompressedSequenceLLen      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , DecompressedSequenceMLen      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , DecompressedSequenceOffs      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , BlockSizePrefix               )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerSeqStreamFinalOffset1      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerSeqStreamFinalOffset2      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerSeqStreamFinalOffset3      )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(LiteralBytes, Stage2)
    ZSTDGPU_SRT_BUF_RO_TYPED(uint32_t, uint8_t              , CompressedData                )
    ZSTDGPU_SRT_BUF_RO_TYPED(uint32_t, uint8_t              , DecompressedLiterals          )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(FseProbsRead, Stage2)
    ZSTDGPU_SRT_BUF_RO_TYPED(int32_t, int16_t               , FseProbs                      )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(FrameOutput, Stage2)
    ZSTDGPU_SRT_BUF_RW_TYPED(uint32_t, uint8_t             , UnCompressedFramesData         )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(FseInit, Stage0|Stage1)
    ZSTDGPU_SRT_BUF_RO_TYPED(int32_t, int16_t              , FseProbsDefault                )
    ZSTDGPU_SRT_BUF_RW_TYPED(int32_t, int16_t              , FseProbs                       )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_FseInfo              , FseInfos                       )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                     , FseElems                       )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BIND_GROUP_BEGIN(ParseBlocksWrite, Stage1)
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_FseInfo               , FseInfos                      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_CompressedBlockData   , CompressedBlocks              )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_OffsetAndSize         , HufRefs                       )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_LitStreamInfo         , LitRefs                       )
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_OffsetAndSize         , SeqStreamToRef                )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , SeqStreamToLLenFseId          )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , SeqStreamToOffsFseId          )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , SeqStreamToMLenFseId          )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , SeqStreamToBlockId            )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , BlockSizePrefix               )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerFrameSeqStreamMinIdx       )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerSeqStreamSeqStart          )
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , SeqCountPrefixLookback        )
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , BlockSeqCountPrefixLookback   )
    ZSTDGPU_SRT_BUF_RW_TYPED(int32_t, int16_t               , FseProbs                      )
    ZSTDGPU_SRT_BUF_RW_TYPED(uint32_t, uint8_t              , DecompressedHuffmanWeightCount)
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , LitStreamCountPrefixLookback  )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , HufWIdToHufLitId              )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , HufLitIdToLitStreamId         )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , HufLitIdToHufWId_DBG          )
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , HufLitCompactionLookback      )
ZSTDGPU_SRT_BIND_GROUP_END()

ZSTDGPU_SRT_BEGIN(ParseFrames, Direct)
    ZSTDGPU_SRT_USE_BIND_GROUP(ParseFrames)

    ZSTDGPU_SRT_CONST(uint32_t                              , frameCount                    )
    ZSTDGPU_SRT_CONST(uint32_t                              , compressedBufferSizeInBytes   )
    ZSTDGPU_SRT_CONST(uint32_t                              , countBlocksOnly               )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(Memset, Indirect)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , Dest                          )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , value                         )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(DecompressLiterals, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(LiteralStreams)
    ZSTDGPU_SRT_USE_BIND_GROUP(HuffmanTable)
    ZSTDGPU_SRT_USE_BIND_GROUP(LiteralDwords)

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(InitHuffmanTableAndDecompressLiterals, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(LiteralStreams)
    ZSTDGPU_SRT_USE_BIND_GROUP(HuffmanWeights)

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(PrefixSum, Indirect)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , InCountsOutPrefix             )
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , InCountsOutPrefixLookback     )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , outputInclusive               )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(PropagateFseIndex, Indirect)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , FseIds                        )
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , FseIndexLookback              )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(ComputePrefixSum, Indirect)
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , HufWIdToHufLitId              )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , HufLitIdToLitStreamId         )

    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , LitGroupEndPerHuffmanTable    )
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , LitGroupEndPerHuffmanTableLookback)
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_Counters              , Counters                      )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , literalsPerGroup              )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(PrefixSequenceOffsets, Indirect)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerSeqStreamFinalOffset1      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerSeqStreamFinalOffset2      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , PerSeqStreamFinalOffset3      )
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , PerSeqStreamFinalOffset1Lookback)
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , PerSeqStreamFinalOffset2Lookback)
    ZSTDGPU_SRT_BUF_RWGLC_STRUCT(uint32_t                   , PerSeqStreamFinalOffset3Lookback)

    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerFrameSeqStreamMinIdx       )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerFrameBlockCountAll         )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , SeqStreamToBlockId            )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , frameCount                    )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(UpdateDispatchArgs, Direct)
    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , DispatchArgs                  )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , DispatchCnts                  )
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , Predicate                     )

    ZSTDGPU_SRT_CONST(uint32_t                              , decompressSequences_StreamsPerTG)
    ZSTDGPU_SRT_CONST(uint32_t                              , stage                         )
    ZSTDGPU_SRT_CONST(uint32_t                              , cmpBlockCountMax              )
    ZSTDGPU_SRT_CONST(uint32_t                              , rawBlockCountMax              )
    ZSTDGPU_SRT_CONST(uint32_t                              , rleBlockCountMax              )
    ZSTDGPU_SRT_CONST(uint32_t                              , litByteCountMax               )
    ZSTDGPU_SRT_CONST(uint32_t                              , seqElemCountMax               )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(DecompressHuffmanWeights, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(HuffmanWeightsWrite)

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RO_BYTE(CompressedData)
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_OffsetAndSize         , HufRefs                       )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_FseInfo               , FseInfos                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , FseElems                      )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(DecodeHuffmanWeights, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(HuffmanWeightsWrite)

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , CompressedData                )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_OffsetAndSize         , HufRefs                       )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , compressedBufferSizeInBytes   )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(InitHuffmanTable, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(HuffmanWeights)
    ZSTDGPU_SRT_USE_BIND_GROUP(HuffmanTableWrite)

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , fseCompressed                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(DecompressSequences, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(SequenceOutputs)

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RO_BYTE(CompressedData)
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_OffsetAndSize         , SeqStreamToRef                )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , SeqStreamToLLenFseId          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , SeqStreamToOffsFseId          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , SeqStreamToMLenFseId          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , SeqStreamToBlockId            )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_FseInfo               , FseInfos                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerSeqStreamSeqStart          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , FseElems                      )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(FinaliseSequenceOffsets, Indirect)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , DecompressedSequenceOffs      )

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerSeqStreamFinalOffset1      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerSeqStreamFinalOffset2      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerSeqStreamFinalOffset3      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerSeqStreamSeqStart          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerFrameBlockCountAll         )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerFrameSeqStreamMinIdx       )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , SeqStreamToBlockId            )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(InitFseTable, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(FseProbsRead)

    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , FseElems                      )

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_FseInfo               , FseInfos                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , tableType                     )

    ZSTDGPU_SRT_CONST_INLINE(uint32_t                       , tableStartIndex               )
    ZSTDGPU_SRT_CONST_INLINE(uint32_t                       , tableDataStart                )
    ZSTDGPU_SRT_CONST_INLINE(uint32_t                       , tableDataCount                )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(ComputeDestBlockOffsets, Indirect)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , BlockDestOffs                 )

    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , BlockSizePrefix               )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerFrameBlockCountAll         )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_OffsetAndSize         , UnCompressedFramesRefs        )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , frameCount                    )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(ExecuteSequences, Direct)
    ZSTDGPU_SRT_USE_BIND_GROUP(LiteralBytes)
    ZSTDGPU_SRT_USE_BIND_GROUP(FrameOutput)

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )

    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerFrameBlockCountCMP         )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , BlockSizePrefix               )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , BlockDestOffs                 )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , DecompressedSequenceLLen      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , DecompressedSequenceMLen      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , DecompressedSequenceOffs      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , GlobalBlockIndexPerCmpBlock   )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerSeqStreamSeqStart          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_CompressedBlockData   , CompressedBlocks              )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(ComputeDestSequenceOffsets, Direct)
    ZSTDGPU_SRT_BUF_RW_STRUCT(uint32_t                      , DestSequenceOffsets           )

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , BlockDestOffs                 )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , DecompressedSequenceMLen      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerSeqStreamSeqStart          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , SeqStreamToBlockId            )

    ZSTDGPU_SRT_CONST(uint32_t                              , tgOffset                      )
    ZSTDGPU_SRT_CONST(uint32_t                              , workItemCount                 )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(MemsetMemcpy, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(FrameOutput)

    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_Counters              , Counters                      )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , CompressedData                )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , BlockDestOffs                 )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , BlockSizePrefixTyped          )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_OffsetAndSize         , BlocksRefsTyped               )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , GlobalBlockIndexTyped         )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , flags                         )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(InitResources, Direct)
    ZSTDGPU_SRT_USE_BIND_GROUP(FseInit)

    ZSTDGPU_SRT_BUF_RW_STRUCT(zstdgpu_Counters              , Counters                      )

    ZSTDGPU_SRT_CONST(uint32_t                              , initResourcesStage            )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_BEGIN(ParseCompressedBlocks, Indirect)
    ZSTDGPU_SRT_USE_BIND_GROUP(ParseBlocksWrite)

    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , CompressedData                )
    ZSTDGPU_SRT_BUF_RO_STRUCT(zstdgpu_OffsetAndSize         , BlocksCMPRefs                 )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , PerFrameBlockCountCMP         )
    ZSTDGPU_SRT_BUF_RO_STRUCT(uint32_t                      , GlobalBlockIndexPerCmpBlock   )

    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , tgOffset                      )
    ZSTDGPU_SRT_CONST_INDIRECT(uint32_t                     , workItemCount                 )
    ZSTDGPU_SRT_CONST(uint32_t                              , compressedBufferSizeInBytes   )
    ZSTDGPU_SRT_CONST(uint32_t                              , frameCount                    )

    ZSTDGPU_SRT_CONST_INLINE(uint32_t                       , compressedBlockCount          )
ZSTDGPU_SRT_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, SeqStreamMinIdx, Direct)
    ZSTDGPU_SRT_BIND(Dest, PerFrameSeqStreamMinIdx)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, BlockCountRawLookback, Direct)
    ZSTDGPU_SRT_BIND(Dest, PerFrameBlockCountRAWLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, BlockCountRleLookback, Direct)
    ZSTDGPU_SRT_BIND(Dest, PerFrameBlockCountRLELookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, BlockCountCmpLookback, Direct)
    ZSTDGPU_SRT_BIND(Dest, PerFrameBlockCountCMPLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, BlockCountAllLookback, Direct)
    ZSTDGPU_SRT_BIND(Dest, PerFrameBlockCountAllLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, RawBlockSizePrefixLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, RawBlockSizePrefixLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, RleBlockSizePrefixLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, RleBlockSizePrefixLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, LitGroupEndPerHuffmanTableLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, LitGroupEndPerHuffmanTableLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, PerSeqStreamFinalOffset1Lookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, PerSeqStreamFinalOffset1Lookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, PerSeqStreamFinalOffset2Lookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, PerSeqStreamFinalOffset2Lookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, PerSeqStreamFinalOffset3Lookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, PerSeqStreamFinalOffset3Lookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, SeqCountPrefixLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, SeqCountPrefixLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, BlockSeqCountPrefixLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, BlockSeqCountPrefixLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, LitStreamCountPrefixLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, LitStreamCountPrefixLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, HufLitCompactionLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, HufLitCompactionLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, FseIndexLookbackLLen, Indirect)
    ZSTDGPU_SRT_BIND(Dest, FseIndexLookbackLLen)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, FseIndexLookbackOffs, Indirect)
    ZSTDGPU_SRT_BIND(Dest, FseIndexLookbackOffs)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, FseIndexLookbackMLen, Indirect)
    ZSTDGPU_SRT_BIND(Dest, FseIndexLookbackMLen)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, HufWIdToHufLitId, Indirect)
    ZSTDGPU_SRT_BIND(Dest, HufWIdToHufLitId)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(Memset, BlockSizePrefixLookback, Indirect)
    ZSTDGPU_SRT_BIND(Dest, BlockSizePrefixLookback)
ZSTDGPU_SRT_PASS_END()


ZSTDGPU_SRT_PASS_BEGIN(PrefixSum, BlockCountRaw, Direct)
    ZSTDGPU_SRT_BIND(InCountsOutPrefix, PerFrameBlockCountRAW)
    ZSTDGPU_SRT_BIND(InCountsOutPrefixLookback, PerFrameBlockCountRAWLookback)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PrefixSum, BlockCountRle, Direct)
    ZSTDGPU_SRT_BIND(InCountsOutPrefix, PerFrameBlockCountRLE)
    ZSTDGPU_SRT_BIND(InCountsOutPrefixLookback, PerFrameBlockCountRLELookback)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PrefixSum, BlockCountCmp, Direct)
    ZSTDGPU_SRT_BIND(InCountsOutPrefix, PerFrameBlockCountCMP)
    ZSTDGPU_SRT_BIND(InCountsOutPrefixLookback, PerFrameBlockCountCMPLookback)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PrefixSum, BlockCountAll, Direct)
    ZSTDGPU_SRT_BIND(InCountsOutPrefix, PerFrameBlockCountAll)
    ZSTDGPU_SRT_BIND(InCountsOutPrefixLookback, PerFrameBlockCountAllLookback)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PrefixSum, BlockSizesRaw, Indirect)
    ZSTDGPU_SRT_BIND(InCountsOutPrefix, RawBlockSizePrefix)
    ZSTDGPU_SRT_BIND(InCountsOutPrefixLookback, RawBlockSizePrefixLookback)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PrefixSum, BlockSizesRle, Indirect)
    ZSTDGPU_SRT_BIND(InCountsOutPrefix, RleBlockSizePrefix)
    ZSTDGPU_SRT_BIND(InCountsOutPrefixLookback, RleBlockSizePrefixLookback)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PrefixSum, BlockSizesAll, Indirect)
    ZSTDGPU_SRT_BIND(InCountsOutPrefix, BlockSizePrefix)
    ZSTDGPU_SRT_BIND(InCountsOutPrefixLookback, BlockSizePrefixLookback)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(PropagateFseIndex, LLen, Indirect)
    ZSTDGPU_SRT_BIND(FseIds, SeqStreamToLLenFseId)
    ZSTDGPU_SRT_BIND(FseIndexLookback, FseIndexLookbackLLen)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PropagateFseIndex, Offs, Indirect)
    ZSTDGPU_SRT_BIND(FseIds, SeqStreamToOffsFseId)
    ZSTDGPU_SRT_BIND(FseIndexLookback, FseIndexLookbackOffs)
ZSTDGPU_SRT_PASS_END()
ZSTDGPU_SRT_PASS_BEGIN(PropagateFseIndex, MLen, Indirect)
    ZSTDGPU_SRT_BIND(FseIds, SeqStreamToMLenFseId)
    ZSTDGPU_SRT_BIND(FseIndexLookback, FseIndexLookbackMLen)
ZSTDGPU_SRT_PASS_END()


ZSTDGPU_SRT_PASS_BEGIN(MemsetMemcpy, MemcpyRAW, Indirect)
    ZSTDGPU_SRT_BIND(Counters, Counters)
    ZSTDGPU_SRT_BIND(CompressedData, CompressedData)
    ZSTDGPU_SRT_BIND(BlockDestOffs, BlockDestOffs)
    ZSTDGPU_SRT_BIND(BlockSizePrefixTyped, RawBlockSizePrefix)
    ZSTDGPU_SRT_BIND(BlocksRefsTyped, BlocksRAWRefs)
    ZSTDGPU_SRT_BIND(GlobalBlockIndexTyped, GlobalBlockIndexPerRawBlock)
ZSTDGPU_SRT_PASS_END()

ZSTDGPU_SRT_PASS_BEGIN(MemsetMemcpy, MemsetRLE, Indirect)
    ZSTDGPU_SRT_BIND(Counters, Counters)
    ZSTDGPU_SRT_BIND(CompressedData, CompressedData)
    ZSTDGPU_SRT_BIND(BlockDestOffs, BlockDestOffs)
    ZSTDGPU_SRT_BIND(BlockSizePrefixTyped, RleBlockSizePrefix)
    ZSTDGPU_SRT_BIND(BlocksRefsTyped, BlocksRLERefs)
    ZSTDGPU_SRT_BIND(GlobalBlockIndexTyped, GlobalBlockIndexPerRleBlock)
ZSTDGPU_SRT_PASS_END()

#endif /* ZSTDGPU_SRT_DECL_H */
