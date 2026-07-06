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

#include "zstdgpu_shared_structs.h"

#ifndef ZSTDGPU_API
#   if defined(__cplusplus)
#       define ZSTDGPU_API extern "C"
#   else
#       define ZSTDGPU_API
#   endif
#endif

struct zstdgpu_CountFramesAndBlocksInfo
{
    uint32_t rawBlockCount;
    uint32_t rleBlockCount;
    uint32_t cmpBlockCount;
    uint32_t frameCount;
    uint64_t frameByteCount;
};

/*
 *  NB: The reason function returns early when encounters a block without "Frame_Content_Size" is
 *      to compute the size of such frame it's required to traverse all blocks, accumulate sizes of
 *      Raw and RLE blocks (which are present), fully decompress compressed blocks to compute their
 *      uncompressed sizes because they are not present.
 */

/**
 *  Traverses a memory block containing zstd frames on CPU and counts how many frames and blocks there are.
 *  During traversal, the function jumps over every block in every frame to determine the end of the parent zstd frame
 *  because zstd doesn't store the size of compressed frames.
 *
 *  The results of this function are stored in `zstdgpu_CountFramesAndBlocksInfo` and could be used to allocate
 *  required number of output structures to call `zstdgpu_CollectFrames` and `zstdgpu_CollectBlocks`.
 *
 *  This function is provided for convinience, in case if caller isn't aware of the structure of the memory
 *  block, e.g. how many zstd frames are and where do they start, so it needs to call `zstdgpu_CollectFrames`.
 *
 *  NB: `memoryBlockSize` must be a multiple of 4 bytes,
 *      `contentSizeInBytes` must be the size of the actual compressed content excluding unused bytes to pad to `memoryBlockSize`
 */
ZSTDGPU_API void zstdgpu_CountFramesAndBlocks(zstdgpu_CountFramesAndBlocksInfo *outInfo, const void *memoryBlock, uint32_t memoryBlockSizeInBytes, uint32_t contentSizeInBytes);

/**
 *  Traverses a memory block containing zstd frames on CPU and extracts information for every zstd frame.
 *  During traversal, the function jumps over every block in every frame to determine the end of the parent zstd frame
 *  because zstd doesn't store the size of compressed frames.
 *
 *  The results of this function are stored in two arrays:
 *      - an array of `zstdgpu_OffsetAndSize` to store the offset and size of each zstd frame
 *      - an array of `zstdgpu_FrameInfo` to store other extracted information about each zstd frame.
 *  both arrays can be used to call `zstdgpu_CollectBlocks`.
 *
 *  This function is provided for convinience, in case if caller isn't aware of the structure of the memory
 *  block, e.g. how many zstd frames are and where do they start, so it needs to call `zstdgpu_CollectFrames`.
 *
 *  NB: `memoryBlockSize` must be a multiple of 4 bytes,
 *      `contentSizeInBytes` must be the size of the actual compressed content excluding unused bytes to pad to `memoryBlockSize`
 */
ZSTDGPU_API void zstdgpu_CollectFrames(zstdgpu_OffsetAndSize *outFrames, zstdgpu_FrameInfo *outFrameInfos, uint32_t frameCount, const void *memoryBlock, uint32_t memoryBlockSizeInBytes, uint32_t contentSizeInBytes);

/**
 *  Traverses a zstd frame specified by `frameIndex` and outputs information for each encountered block into
 *  `outBlocks{Type}` arrays.
 *
 *  During traversal, the function jumps over every blocks in a given zstd frame to determine its end position
 *  because zstd doesn't store the size of compressed frames.
 *
 *  This function requires to know the number of zstd frames in the memory blob, their start positions and sizes
 *  (`zstdgpu_OffsetAndSize`) and various auxiliary information (`zstdgpu_FrameInfo`) such as starts for each type of block
 *  in the output arrays.
 *
 *  It is safe to call this function from multiple threads with different `frameIndex`. To extract data for multiple blocks
 *  the caller needs to call this function for `frameIndex` in range [0..`frameCount`]
 *
 *  NB: `memoryBlockSize` must be a multiple of 4 bytes,
 *      `contentSizeInBytes` must be the size of the actual compressed content excluding unused bytes to pad to `memoryBlockSize`
 *
 *  NB2: For RLE block `zstdgpu_OffsetAndSize::offs` stores the actual 8-bit symbol. At the same time `zstdgpu_OffsetAndSize::size`
 *       stores the number of times the symbol has to be repeated in the decompressed stream.
 */
ZSTDGPU_API void zstdgpu_CollectBlocks(zstdgpu_OffsetAndSize *outBlocksRaw, zstdgpu_OffsetAndSize *outBlocksRLE, zstdgpu_OffsetAndSize *outBlocksCmp, const zstdgpu_OffsetAndSize *frames, const zstdgpu_FrameInfo *frameInfos, uint32_t frameIndex, uint32_t frameCount, const void *memoryBlock, uint32_t memoryBlockSizeInBytes, uint32_t contentSizeInBytes);

struct zstdgpu_CountLiteralAndSequenceInfo
{
    uint32_t decodedLiteralsByteCount;
    uint32_t sequenceCount;
};

/**
 *  Scans compressed block interiors on CPU and extracts aggregate literal/sequence statistics.
 *  For each compressed block, parses the literal section header (to get regenerated size for
 *  compressed/treeless literals) and the sequence section header (to get sequence count).
 *
 *  Takes the already-discovered frame offsets from a prior `zstdgpu_CollectFrames` call.
 *
 *  Results can be passed to `zstdgpu_SetupBlockInfoConstants` to enable merging all GPU
 *  stages into a single command list submission without CPU fences.
 *
 *  NB: `memoryBlockSize` must be a multiple of 4 bytes.
 */
ZSTDGPU_API void zstdgpu_CountCompressedLiteralsAndSequences(zstdgpu_CountLiteralAndSequenceInfo *outInfo, const zstdgpu_OffsetAndSize *frames, uint32_t frameCount, const void *memoryBlock, uint32_t memoryBlockSizeInBytes);

typedef enum zstdgpu_Status
{
    kzstdgpu_StatusSuccess          = 0,
    kzstdgpu_StatusInvalidArgument  = 1,
    kzstdgpu_StatusForceInt         = 0x7fffffff
} zstdgpu_Status;

typedef struct zstdgpu_PersistentContextImpl *zstdgpu_PersistentContext;
typedef struct zstdgpu_PerRequestContextImpl *zstdgpu_PerRequestContext;

ZSTDGPU_API uint32_t zstdgpu_GetPersistentContextRequiredMemorySizeInBytes(void);
ZSTDGPU_API uint32_t zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes(void);

ZSTDGPU_API zstdgpu_Status zstdgpu_CreatePersistentContext(zstdgpu_PersistentContext *outPersistentContext, struct ID3D12Device *device, void *memoryBlock, uint32_t memoryBlockSizeInBytes);
ZSTDGPU_API zstdgpu_Status zstdgpu_DestroyPersistentContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PersistentContext inPersistentContext);

ZSTDGPU_API zstdgpu_Status zstdgpu_CreatePerRequestContext(zstdgpu_PerRequestContext *outPerRequestContext, zstdgpu_PersistentContext inPersistentContext, void *memoryBlock, uint32_t memoryBlockSizeInBytes);
ZSTDGPU_API zstdgpu_Status zstdgpu_DestroyPerRequestContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PerRequestContext inPerRequestContext);

/**
 *  @brief      A callback that is going to be called during execution of `zstdgpu_SubmitWithExternalMemory`
 *              or `zstdgpu_SubmitWithInteralMemory` with `stageIndex == 0` in order to let the calling
 *              site of those two functions to load into CPU-mapped memory of UPLOAD or GPU_UPLOAD heap -- a memory block
 *              containing compressed Zstd frames and a buffer with the offset and the size of each compressed Zstd frame in
 *              the memory block with compressed Zstd frames.
 *
 *              The primary reason this callback exists is to give calling site an opportunity to load compressed data
 *              directly from disk into UPLOAD or GPU_UPLOAD heap memory, avoiding 2nd copy on CPU if the calling site
 *              is able to load compressed data directly from disk.
 *
 *  @param[in]  dstFramesMemory             A pointer to CPU-mapped memory of the UPLOAD/GPU_UPLOAD heap where the compressed Zstd frames must be uploaded.
 *  @param[in]  dstFramesMemorySizeInBytes  The size of CPU-mapped memory of the UPLOAD/GPU_UPLOAD heap where the compressed Zstd frames must be uploaded.
 *  @param[in]  dstFrames                   A pointer to CPU-mapped memory of the UPLOAD/GPU_UPLOAD heap where the offset (relative to `dstFramesMemory`) and the size of each Zstd frame must be uploaded.
 *  @param[in]  dstFrameCount               The total number of Zstd frames for which `zstdgpu_OffsetAndSize` structures must be uploaded.
 *  @param[in]  uploadUserdata              A pointer to user data passed into `zstdgpu_SetupInputsAs`
 */
typedef void zstdgpu_UploadFrames(void *dstFramesMemory, uint32_t dstFramesMemorySizeInBytes, zstdgpu_OffsetAndSize *dstFrames, uint32_t dstFrameCount, void *uploadUserdata);

/**
 *  @brief      This function sets up the inputs for Zstd GPU decompressor in the form of a memory block with compressed Zstd frames and
 *              control structures, one for each compressed Zstd frame, specifying where each Zstd frame is located in provided memory block.
 *              Both memory block with compressed Zstd frames and control structures for each Zstd frame are located either on disk or in CPU memory
 *              and supplied into a Zstd GPU decompressor via loading from disk or via memory copy into the CPU-mapped memory in UPLOAD/GPU_UPLOAD heap provided by `zstdgpu_UploadFrames` callback.
 *
 *  @param[out] outStageCount           A pointer to a `uint32_t` variable receiving the total number of stages Zstd GPU Decompressor needs
 *  @param[in]  inPerRequestContext     A context holding necessary state per decompression request.
 *  @param[in]  frameCount              The total number of compressed Zstd frames that are going to be decompressed.
 *  @param[in]  framesMemorySizeInBytes The total number of bytes the block with compressed Zstd frames requires (it is fine to have a buffer with non-adjacent frames)
 *  @param[in]  uploadCallback          A pointer to a callback that is going to be called during the execution of `zstdgpu_SubmitWithExternalMemory` or `zstdgpu_SubmitWithInteralMemory` with `stageIndex == 0`
 *                                      in order to let the calling site of those two functions to upload the memory block with compressed Zstd frames and the offset and the size for each compressed Zstd frame in that block.
 *  @param[in]  uploadUserdata          A pointer to a userdata passed into `uploadCallback`
 *
 *  @see `zstdgpu_UploadFrames`
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupInputsAsFramesInCpuMemory(uint32_t *outStageCount, zstdgpu_PerRequestContext inPerRequestContext, uint32_t frameCount, uint32_t framesMemorySizeInBytes, zstdgpu_UploadFrames *uploadCallback, void *uploadUserdata);


/**
 *  @brief      This function sets up the inputs for Zstd GPU decompressor in the form of a memory block with compressed Zstd frames and
 *              control structures, one for each compressed Zstd frame, specifying where each Zstd frame is located in provided memory block.
 *              Both memory block with compressed Zstd frames and control structures for each Zstd frame are located either in GPU_UPLOAD or DEFAULT heap
 *
 *  @param[out] outStageCount           A pointer to a `uint32_t` variable receiving the total number of stages Zstd GPU Decompressor needs
 *  @param[in]  inPerRequestContext     A context holding necessary state per decompression request.
 *  @param[in]  framesMemory            A pointer to a ID3D12Resource in GPU_UPLOAD or DEFAULT heap where the compressed Zstd frames are placed.
 *  @param[in]  framesMemorySizeInBytes The total number of bytes the block with compressed Zstd frames contains (it is fine to have a buffer with non-adjacent Zstd frames)
 *                                      NOTE: the size must a multiple of 4 bytes
 *  @param[in]  frames                  A pointer to a ID3D12Resource in GPU_UPLOAD or DEFAULT heap where the offset (relative to `framesMemory` start) and the size of each Zstd frame are placed in the form of `zstdgpu_OffsetAndSize` structures.
 *  @param[in]  frameCount              The total number `zstdgpu_OffsetAndSize` structures placed `frames` buffers.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupInputsAsFramesInGpuMemory(uint32_t *outStageCount, zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *framesMemory, uint32_t framesMemorySizeInBytes, struct ID3D12Resource *frames, uint32_t frameCount);

ZSTDGPU_API zstdgpu_Status zstdgpu_SetupOutputs(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *framesMemory, uint32_t framesMemorySizeInBytes, struct ID3D12Resource *frames, uint32_t frameCount);

ZSTDGPU_API zstdgpu_Status zstdgpu_SetupAllStageSubmission(zstdgpu_PerRequestContext inPerRequestContext);

/**
 *  @brief      Specifies the number of blocks of each type from a CPU pre-scan.
 *              When set, `zstdgpu_GetGpuMemoryRequirement` for stage 1 uses these counts instead of
 *              reading from GPU counter readback, enabling stages 0 and 1 to be recorded into
 *              the same command list without a CPU fence.
 *              Can be called before or after `zstdgpu_SetupInputs*` functions.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupFrameInfoConstants(zstdgpu_PerRequestContext inPerRequestContext, uint32_t rawBlockCount, uint32_t rleBlockCount, uint32_t cmpBlockCount);

/**
 *  @brief      Specifies the total decoded literal byte count and sequence count.
 *              When set, `zstdgpu_GetGpuMemoryRequirement` for stage 2 uses these counts instead of
 *              reading from GPU counter readback, enabling stages 1 and 2 to be recorded into
 *              the same command list without a CPU fence.
 *              Can be called before or after `zstdgpu_SetupInputs*` functions.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupBlockInfoConstants(zstdgpu_PerRequestContext inPerRequestContext, uint32_t literalsByteCount, uint32_t sequenceCount);

/**
 *  @brief      Returns 1 if a CPU readback/fence is required after the given stage before proceeding
 *              to the next stage, 0 otherwise. Use this to determine whether to submit the command list
 *              and wait for GPU idle between stages.
 */
ZSTDGPU_API uint32_t zstdgpu_IsReadbackRequired(zstdgpu_PerRequestContext inPerRequestContext, uint32_t stageIndex);

/**
 *  @brief      Returns 1 if a CPU readback/fence is required after any stages, 0 otherwise. This API
 *              mainly exist to ensure `zstdgpu_PerRequestContext` has sufficient information to not
 *              require any readbacks, so calling code could check: `ZSTDGPU_ASSERT(0 == zstdgpu_IsAnyStageReadbackRequired(ctx))`
 */
ZSTDGPU_API uint32_t zstdgpu_IsAnyStageReadbackRequired(zstdgpu_PerRequestContext inPerRequestContext);

/**
 *  @brief      Returns memory requirement for each heap type per submission stage for a given `zstdgpu_PerRequestContext`
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_GetGpuMemoryRequirement(uint32_t *outDefaultHeapByteCount, uint32_t *outUploadHeapByteCount, uint32_t *outReadbackHeapByteCount, uint32_t *outShaderVisibleDescriptorCount, zstdgpu_PerRequestContext inPerRequestContext, uint32_t stageIndex);

/**
 *  @brief      Returns memory requirement for each heap type for all submission stages for a given `zstdgpu_PerRequestContext`
 *
 *  @note       It's valid to call this function only when `zstdgpu_IsAnyStageReadbackRequired` returns `0`
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_GetAllStageGpuMemoryRequirement(uint32_t *outDefaultHeapByteCount, uint32_t *outUploadHeapByteCount, uint32_t *outReadbackHeapByteCount, uint32_t *outShaderVisibleDescriptorCount, zstdgpu_PerRequestContext inPerRequestContext);

/**
 *  @brief      Submits all required decompression commands into a command list for a given `stageIndex` with externally
 *              supplied memory.
 *
 *  @note       The caller must wait for GPU idle before calling this function if `zstdgpu_IsReadbackRequired` return `1`
 *              for `stageIndex` supplied into this function.
 *
 *  @note       The caller must compute required memory for a given `stageIndex` via `zstdgpu_GetGpuMemoryRequirement`
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitWithExternalMemory(zstdgpu_PerRequestContext inPerRequestContext,
                                                            uint32_t stageIndex,
                                                            struct ID3D12GraphicsCommandList *cmdList,
                                                            struct ID3D12Heap *defaultHeap,
                                                            uint32_t defaultHeapOffsetInBytes,
                                                            struct ID3D12Heap *uploadHeap,
                                                            uint32_t uploadHeapOffsetInBytes,
                                                            struct ID3D12Heap *readbackHeap,
                                                            uint32_t readbackHeap_OffsetInBytes,
                                                            struct ID3D12DescriptorHeap *shaderVisibleHeap,
                                                            uint32_t shaderVisibileHeapOffsetInDescriptors);

/**
 *  @brief      Submits all required decompression commands into a command list for all stages (single-submission mode)
 *              with externally supplied memory.
 *
 *  @note       It's valid to call this function only when `zstdgpu_IsAnyStageReadbackRequired` returns `0`
 *
 *  @note       The caller must compute required memory via `zstdgpu_GetAllStageGpuMemoryRequirement`
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitAllStagesWithExternalMemory(zstdgpu_PerRequestContext inPerRequestContext,
                                                                     struct ID3D12GraphicsCommandList *cmdList,
                                                                     struct ID3D12Heap *defaultHeap,
                                                                     uint32_t defaultHeapOffsetInBytes,
                                                                     struct ID3D12Heap *uploadHeap,
                                                                     uint32_t uploadHeapOffsetInBytes,
                                                                     struct ID3D12Heap *readbackHeap,
                                                                     uint32_t readbackHeap_OffsetInBytes,
                                                                     struct ID3D12DescriptorHeap *shaderVisibleHeap,
                                                                     uint32_t shaderVisibileHeapOffsetInDescriptors);

/**
 *  @brief      Submits all required decompression commands into a command list for a given `stageIndex` with internally
 *              allocated memory.
 *
 *  @note       The caller must wait for GPU idle before calling this function if `zstdgpu_IsReadbackRequired` return `1`
 *              for `stageIndex` supplied into this function.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitWithInteralMemory(zstdgpu_PerRequestContext inPerRequestContext, uint32_t stageIndex, struct ID3D12GraphicsCommandList *cmdList);

/**
 *  @brief      Submits all required decompression commands into a command list for all stages (single-submission mode)
 *              with internally allocated memory.
 *
 *  @note       It's valid to call this function only when `zstdgpu_IsAnyStageReadbackRequired` returns `0`
 *
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitAllStagesWithInteralMemory(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12GraphicsCommandList *cmdList);
