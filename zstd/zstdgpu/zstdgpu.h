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
 *
 *  NB3: This function is a standalone host-side convenience. The decode pipeline does NOT consume
 *       its output -- blocks are discovered on the GPU by `ParseFrames`, and block ordinals are
 *       GPU-derived. Nothing in the library, the demo or the tests calls this to decode; it exists
 *       for a caller that wants to enumerate blocks itself. Do not read its presence as evidence
 *       that block collection happens on the host.
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
    kzstdgpu_StatusSuccess                  = 0,
    kzstdgpu_StatusInvalidArgument          = 1,
    kzstdgpu_StatusResourceCreationFailed   = 2, /**< A D3D12 heap, resource or descriptor heap could not be created, or a resource could not be mapped (for example, after device removal). */
    kzstdgpu_StatusBusy                     = 3, /**< A session already has `maxBatchesInFlight` batches the caller has not retired (`zstdgpu_RetireSessionBatches`). Nothing was recorded. */
    kzstdgpu_StatusForceInt                 = 0x7fffffff
} zstdgpu_Status;

typedef struct zstdgpu_PersistentContextImpl *zstdgpu_PersistentContext;
typedef struct zstdgpu_PerRequestContextImpl *zstdgpu_PerRequestContext;
typedef struct zstdgpu_SessionContextImpl *zstdgpu_SessionContext;

ZSTDGPU_API uint32_t zstdgpu_GetPersistentContextRequiredMemorySizeInBytes(void);
ZSTDGPU_API uint32_t zstdgpu_GetPerRequestContextRequiredMemorySizeInBytes(void);

ZSTDGPU_API zstdgpu_Status zstdgpu_CreatePersistentContext(zstdgpu_PersistentContext *outPersistentContext, struct ID3D12Device *device, void *memoryBlock, uint32_t memoryBlockSizeInBytes);
ZSTDGPU_API zstdgpu_Status zstdgpu_DestroyPersistentContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PersistentContext inPersistentContext);

ZSTDGPU_API zstdgpu_Status zstdgpu_CreatePerRequestContext(zstdgpu_PerRequestContext *outPerRequestContext, zstdgpu_PersistentContext inPersistentContext, void *memoryBlock, uint32_t memoryBlockSizeInBytes);
ZSTDGPU_API zstdgpu_Status zstdgpu_DestroyPerRequestContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_PerRequestContext inPerRequestContext);

/**
 *  @brief      A callback that is going to be called during execution of `zstdgpu_SubmitWithExternalMemory`
 *              or `zstdgpu_SubmitWithInternalMemory` with `stageIndex == 0` in order to let the calling
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
 *  @param[in]  uploadCallback          A pointer to a callback that is going to be called during the execution of `zstdgpu_SubmitWithExternalMemory` or `zstdgpu_SubmitWithInternalMemory` with `stageIndex == 0`
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

/**
 *  @brief      Sets up the caller-supplied output resources for a decompression request.
 *
 *  @param[in]  inPerRequestContext     A context holding necessary state per decompression request.
 *  @param[in]  framesMemory            A pointer to a writable, UAV-capable ID3D12Resource where the uncompressed Zstd frames are written.
 *  @param[in]  framesMemorySizeInBytes The total number of writable bytes in `framesMemory`. It must cover all output ranges described by `frames`.
 *  @param[in]  frames                  A pointer to an ID3D12Resource containing the offset (relative to `framesMemory`) and decompressed size of each output frame as `zstdgpu_OffsetAndSize` structures.
 *  @param[in]  frameCount              The total number of `zstdgpu_OffsetAndSize` structures in `frames`. It must match the number of input frames.
 *  @param[in]  frameStatus             A pointer to an ID3D12Resource (DEFAULT heap, UAV-capable) sized to hold one UINT32 per input frame which receives an HRESULT of the decompression status.
 *                                      NOTE: See zstdgpu_shared_structs.h for the status code definitions.
 *                                      NOTE: The accepted frames of one request must decompress to at most
 *                                            `kzstdgpu_MaxRequestUncompressedByteSize` bytes in total; otherwise
 *                                            they report `kzstdgpu_FrameStatus_RequestTooLarge`.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupOutputs(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *framesMemory, uint32_t framesMemorySizeInBytes, struct ID3D12Resource *frames, uint32_t frameCount, struct ID3D12Resource *frameStatus);

ZSTDGPU_API zstdgpu_Status zstdgpu_SetupAllStageSubmission(zstdgpu_PerRequestContext inPerRequestContext);

/**
 *  @brief      Decodes the request's frames up to an output budget, so that a frame larger than the scratch
 *              memory can afford is decoded over several requests.
 *
 *  Scratch is sized from `outputBudgetBytes` (output bytes this request produces) and `blockSlotCount`
 *  (an over-provisioned allowance of blocks), not from the frames' sizes or block counts, so the caller never
 *  parses a frame. Within a request:
 *
 *      - frame 0 resumes from `resumeRecord` when `resumeFirstFrame` is non-zero;
 *      - every frame between the first and the last is decoded whole;
 *      - the last frame is decoded up to `lastFrameOutputLimit` bytes from its start (pass its full size to
 *        decode it whole); a single-frame request can be both resumed and limited.
 *
 *  The caller plans requests from frame sizes alone: `outputBudgetBytes` is the sum, over the request's frames,
 *  of the bytes the request takes each frame to -- from the previous request's limit for a resumed frame, and
 *  to `lastFrameOutputLimit` for the last. Decoding stops at a block boundary, so the block that crosses the
 *  limit is written whole: up to one block (at most 128 KiB) past the limit. The next request resumes after
 *  that block, but the caller plans it as if the extra bytes had not been written; they were written to their
 *  final place and are never written again.
 *
 *  Requirements:
 *      - `resumeRecord` is a caller-owned buffer of at least `kzstdgpu_ResumeRecordDwordCount` (`zstdgpu_structs.h`) `uint32_t`s in
 *        `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`, kept unchanged between the requests of one frame. Its
 *        contents need no initialisation for a request whose `resumeFirstFrame` is 0.
 *      - A resumed frame's earlier output must still be in the destination at the same frame-relative
 *        offsets (matches read it as history), and its compressed bytes must still be in the input.
 *      - `blockSlotCount` covers every block the request takes: `zstdgpu_PartialDecodeBlockSlotCount` gives an
 *        allowance that covers real content. A request that needs more fails loudly with
 *        `kzstdgpu_FrameStatus_BlockCountExceeded`: on every frame, with nothing decoded, when the frames before
 *        the last one alone exceed it, and otherwise on the last frame alone, whose window ran out of slots
 *        before reaching its limit.
 *
 *  Wait for the GPU to finish a request before recording the next request on the same context or into the same
 *  memory: the library re-places its scratch resources in the same heaps for every request and records no
 *  aliasing barriers between them. To record many partial requests into one command list, use a session
 *  (`zstdgpu_CreateSessionContext`), which plans and records them itself.
 *
 *  The request-size limit (`kzstdgpu_MaxRequestUncompressedByteSize`, see `zstdgpu_SetupOutputs`) counts only the
 *  part of the resumed and the last frame that the request decodes, so a frame larger than the limit can be decoded
 *  in pieces.
 *
 *  Pass a NULL `resumeRecord` to return to whole-frame decoding. Call before sizing or submitting.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupPartialDecode(zstdgpu_PerRequestContext inPerRequestContext,
                                                      struct ID3D12Resource *resumeRecord,
                                                      uint32_t resumeFirstFrame,
                                                      uint32_t lastFrameOutputLimit,
                                                      uint32_t outputBudgetBytes,
                                                      uint32_t blockSlotCount);

/**
 *  @brief      A block-slot allowance for `zstdgpu_SetupPartialDecode` covering real content: one slot per
 *              frame plus one per 8 KiB of output budget, and a few for the resumed frame's carried tables.
 */
ZSTDGPU_API uint32_t zstdgpu_PartialDecodeBlockSlotCount(uint32_t outputBudgetBytes, uint32_t frameCount);

/**
 *  @brief      Specifies the number of blocks of each type from a CPU pre-scan.
 *              When set, `zstdgpu_GetGpuMemoryRequirement` for stage 1 uses these counts instead of
 *              reading from GPU counter readback, enabling stages 0 and 1 to be recorded into
 *              the same command list without a CPU fence.
 *              Can be called before or after `zstdgpu_SetupInputs*` functions.
 *
 *  NB: under single submission (`zstdgpu_SetupAllStageSubmission`) this is effectively REQUIRED.
 *      Block count cannot be bounded by the decompressed size -- a block may regenerate as little
 *      as zero bytes -- so without these counts the library has only an estimate, and when content
 *      exceeds it every frame of the batch fails with `kzstdgpu_FrameStatus_BlockCountExceeded`.
 *      The sizing entry points therefore refuse that combination unless the caller opts in with
 *      `zstdgpu_SetupAllowEstimatedBlockCounts`.
 *
 *      Supplying them is cheap: `zstdgpu_CountFramesAndBlocks` is a block-header hop that does not
 *      descend into literal or sequence sections.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupFrameInfoConstants(zstdgpu_PerRequestContext inPerRequestContext, uint32_t rawBlockCount, uint32_t rleBlockCount, uint32_t cmpBlockCount);

/**
 *  @brief      Permits single submission to size per-block metadata from an ESTIMATED block count
 *              when no exact count is available.
 *
 *  Only call this if the compressed bytes are unreachable from the CPU -- the
 *  `zstdgpu_SetupInputsAsFramesInGpuMemory` case, where a host pre-scan is impossible. Any caller
 *  that can reach the bytes should call `zstdgpu_CountFramesAndBlocks` and
 *  `zstdgpu_SetupFrameInfoConstants` instead, which is both sound and nearly free.
 *
 *  WHAT THIS OPTS INTO: the estimate is `(decompressedBytes + 4095) / 4096`, which is NOT a bound.
 *  Real content can exceed it by a wide margin. When it is exceeded the stage count checks fire,
 *  the predicated work is skipped, and every frame of the batch reports
 *  `kzstdgpu_FrameStatus_BlockCountExceeded` -- nothing in the batch is decoded.
 *
 *  Callers must therefore check the frame status buffer and be prepared to retry a failed batch,
 *  for example with staged submission, which sizes from counts read back from the GPU. Adversarial
 *  input can force this failure.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupAllowEstimatedBlockCounts(zstdgpu_PerRequestContext inPerRequestContext);

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
 *
 *  @note       Returns `kzstdgpu_StatusResourceCreationFailed` if the stage is sized from counters read back after the
 *              previous stage and that stage failed with `kzstdgpu_StatusResourceCreationFailed`.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_GetGpuMemoryRequirement(uint64_t *outDefaultHeapByteCount, uint64_t *outUploadHeapByteCount, uint64_t *outReadbackHeapByteCount, uint32_t *outShaderVisibleDescriptorCount, zstdgpu_PerRequestContext inPerRequestContext, uint32_t stageIndex);

/**
 *  @brief      Returns memory requirement for each heap type for all submission stages for a given `zstdgpu_PerRequestContext`
 *
 *  @note       It's valid to call this function only when `zstdgpu_IsAnyStageReadbackRequired` returns `0`
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_GetAllStageGpuMemoryRequirement(uint64_t *outDefaultHeapByteCount, uint64_t *outUploadHeapByteCount, uint64_t *outReadbackHeapByteCount, uint32_t *outShaderVisibleDescriptorCount, zstdgpu_PerRequestContext inPerRequestContext);

/**
 *  @brief      Submits all required decompression commands into a command list for a given `stageIndex` with externally
 *              supplied memory.
 *
 *  @note       The caller must wait for GPU idle before calling this function if `zstdgpu_IsReadbackRequired` return `1`
 *              for `stageIndex` supplied into this function.
 *
 *  @note       The caller must compute required memory for a given `stageIndex` via `zstdgpu_GetGpuMemoryRequirement`
 *
 *  @note       Returns `kzstdgpu_StatusResourceCreationFailed` without recording any commands if a resource could not be
 *              created in the supplied heaps or mapped (for example, after device removal). Do not submit later stages
 *              of the request after a failure.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitWithExternalMemory(zstdgpu_PerRequestContext inPerRequestContext,
                                                            uint32_t stageIndex,
                                                            struct ID3D12GraphicsCommandList *cmdList,
                                                            struct ID3D12Heap *defaultHeap,
                                                            uint64_t defaultHeapOffsetInBytes,
                                                            struct ID3D12Heap *uploadHeap,
                                                            uint64_t uploadHeapOffsetInBytes,
                                                            struct ID3D12Heap *readbackHeap,
                                                            uint64_t readbackHeap_OffsetInBytes,
                                                            struct ID3D12DescriptorHeap *shaderVisibleHeap,
                                                            uint32_t shaderVisibileHeapOffsetInDescriptors);

/**
 *  Sessions: single-submission decompression into caller-owned memory.
 *
 *  A session binds, for its lifetime, one command list and the memory the library needs: a range of a DEFAULT heap
 *  (scratch), a range of an UPLOAD heap (constant tables, written once at creation) and a range of a shader-visible
 *  descriptor heap. All of it is sized up front from the arguments below, never from content, so the caller never
 *  parses a frame:
 *
 *      - `outputBudgetBytes`: the decompressed bytes one dispatch produces. A batch with more output is split into
 *        several dispatches, all recorded by one `zstdgpu_SubmitAllStagesWithExternalMemory` call; a frame larger
 *        than the budget is decoded over several of them. Scratch grows with the budget: 8/3 bytes per byte for
 *        literals and sequences, plus per-block metadata for the block slots.
 *      - `maxFramesPerBatch`: the most frames one batch may hold (at most 2^20).
 *      - `blockSlotsPerMiB`: per-block metadata allowance per MiB of budget (0 = default, one slot per 8 KiB of
 *        output). Content with smaller blocks than that fails loudly with `kzstdgpu_FrameStatus_BlockCountExceeded`.
 *        The resulting block slots per dispatch (frames + budget-derived slots) are capped at 2^19.
 *      - `maxBatchesInFlight`: how many batches may be recorded before the caller retires the oldest
 *        (`zstdgpu_RetireSessionBatches`). Only the descriptor ring grows with it.
 *
 *  One persistent context may serve many sessions (for example one per DirectStorage queue). Sessions never share
 *  memory or command lists, so batches on different sessions may run concurrently.
 */

/**
 *  @brief      The memory a session needs for the given footprint.
 *
 *  @param[out] outDefaultHeapByteCount         Bytes of the DEFAULT heap range passed to `zstdgpu_CreateSessionContext`.
 *  @param[out] outUploadHeapByteCount          Bytes of the UPLOAD heap range.
 *  @param[out] outShaderVisibleDescriptorCount Descriptors of the shader-visible CBV/SRV/UAV heap range.
 *  @param[out] outSessionMemorySizeInBytes     Bytes of the CPU memory block for the session context (may be NULL).
 *  @param[in]  outputBudgetBytes               At most `kzstdgpu_MaxRequestUncompressedByteSize`.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_GetSessionRequirement(uint64_t *outDefaultHeapByteCount,
                                                         uint64_t *outUploadHeapByteCount,
                                                         uint32_t *outShaderVisibleDescriptorCount,
                                                         uint32_t *outSessionMemorySizeInBytes,
                                                         zstdgpu_PersistentContext inPersistentContext,
                                                         uint32_t outputBudgetBytes,
                                                         uint32_t maxFramesPerBatch,
                                                         uint32_t blockSlotsPerMiB,
                                                         uint32_t maxBatchesInFlight);

/**
 *  @brief      The inverse of `zstdgpu_GetSessionRequirement`: the largest `outputBudgetBytes` whose DEFAULT heap
 *              requirement fits in `defaultHeapByteCount`. The other requirements do not depend on the budget.
 *
 *  @return     `kzstdgpu_StatusInvalidArgument` if not even a one-byte budget fits.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_GetSessionBudgetForScratch(uint32_t *outOutputBudgetBytes,
                                                              zstdgpu_PersistentContext inPersistentContext,
                                                              uint64_t defaultHeapByteCount,
                                                              uint32_t maxFramesPerBatch,
                                                              uint32_t blockSlotsPerMiB);

ZSTDGPU_API uint32_t zstdgpu_GetSessionContextRequiredMemorySizeInBytes(void);

/**
 *  @brief      Creates a session (see above). Its scratch resources are created in the supplied heap ranges here,
 *              once, and the constant tables are written to the upload range.
 *
 *  @param[in]  cmdList                 The command list every batch is recorded into (DIRECT or COMPUTE). It is
 *                                      AddRef'd; the caller resets and executes it as usual. A different command
 *                                      list needs a different session.
 *  @param[in]  defaultHeap, defaultHeapOffsetInBytes  A DEFAULT heap range of `outDefaultHeapByteCount` bytes that
 *                                      allows buffers; the offset must be 64 KiB aligned. Owned by the session until
 *                                      it is destroyed.
 *  @param[in]  uploadHeap, uploadHeapOffsetInBytes    Likewise, an UPLOAD heap range of `outUploadHeapByteCount` bytes.
 *  @param[in]  shaderVisibleHeap, shaderVisibleHeapOffsetInDescriptors  A shader-visible CBV/SRV/UAV descriptor
 *                                      range of `outShaderVisibleDescriptorCount` descriptors.
 *  @param[in]  memoryBlock             CPU memory of `zstdgpu_GetSessionContextRequiredMemorySizeInBytes` bytes, returned
 *                                      by `zstdgpu_DestroySessionContext`.
 *
 *  @return     `kzstdgpu_StatusResourceCreationFailed` if a resource could not be created in the supplied heaps.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_CreateSessionContext(zstdgpu_SessionContext *outSession,
                                                        zstdgpu_PersistentContext inPersistentContext,
                                                        struct ID3D12GraphicsCommandList *cmdList,
                                                        struct ID3D12Heap *defaultHeap,
                                                        uint64_t defaultHeapOffsetInBytes,
                                                        struct ID3D12Heap *uploadHeap,
                                                        uint64_t uploadHeapOffsetInBytes,
                                                        struct ID3D12DescriptorHeap *shaderVisibleHeap,
                                                        uint32_t shaderVisibleHeapOffsetInDescriptors,
                                                        uint32_t outputBudgetBytes,
                                                        uint32_t maxFramesPerBatch,
                                                        uint32_t blockSlotsPerMiB,
                                                        uint32_t maxBatchesInFlight,
                                                        void *memoryBlock,
                                                        uint32_t memoryBlockSizeInBytes);

/**
 *  @brief      Destroys a session. The GPU must have finished every batch recorded on it.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_DestroySessionContext(void **outMemoryBlock, uint32_t *outMemoryBlockSizeInBytes, zstdgpu_SessionContext inSession);

/**
 *  @brief      Tells the session that the GPU has finished every batch up to and including `completedBatchId` (as
 *              returned by `zstdgpu_SubmitAllStagesWithExternalMemory`), so their descriptor ring slots can be reused.
 *              Typically called with the batch id the caller associated with its last completed fence value.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_RetireSessionBatches(zstdgpu_SessionContext inSession, uint64_t completedBatchId);

/**
 *  @brief      Records the decompression of one batch of frames into the session's command list.
 *
 *  The batch is split into dispatches of at most the session's `outputBudgetBytes` of output, planned from
 *  `outFramesCpu` alone: whole frames while they fit, then part of the next frame, which the next dispatch resumes.
 *  All dispatches are recorded by this call, separated by UAV barriers, so the GPU never waits on the CPU. A dispatch
 *  stops at a block boundary, so it may write up to one block (128 KiB) past its planned end; that output is final
 *  and the next dispatch continues after it.
 *
 *  @param[out] outBatchId              The batch's id, increasing by one per batch, for `zstdgpu_RetireSessionBatches`.
 *  @param[in]  inFramesMemory          Compressed frames, GPU-resident (DEFAULT or GPU_UPLOAD heap).
 *  @param[in]  inFramesMemorySizeInBytes
 *  @param[in]  inFrames                `frameCount` `zstdgpu_OffsetAndSize` records into `inFramesMemory`.
 *  @param[in]  frameCount              At most the session's `maxFramesPerBatch`.
 *  @param[in]  outFramesMemory         Destination; holds the batch's entire decompressed output.
 *  @param[in]  outFramesMemorySizeInBytes
 *  @param[in]  outFrames               `frameCount` `zstdgpu_OffsetAndSize` records into `outFramesMemory`; the size is the
 *                                      frame's decompressed size.
 *  @param[in]  outFramesCpu            A CPU copy of the same records. Read only during this call.
 *  @param[in]  frameStatus             `frameCount` status codes (see zstdgpu_shared_structs.h), written by the GPU.
 *
 *  Resource states. On entry (the caller guarantees) and on exit (the library guarantees):
 *      - `inFramesMemory`, `inFrames`, `outFrames`: D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
 *      - `outFramesMemory`, `frameStatus`: D3D12_RESOURCE_STATE_UNORDERED_ACCESS.
 *  On exit, every write to `outFramesMemory` and `frameStatus` is complete (a global UAV barrier is recorded last),
 *  predication is off, the session's descriptor heap is bound, and the root signature and pipeline state are undefined.
 *  The session's own scratch is ordered by a UAV barrier at the start of every dispatch, so consecutive batches need no
 *  CPU synchronisation.
 *
 *  Every resource passed here must stay valid and unchanged until the GPU has finished the batch.
 *
 *  @return     `kzstdgpu_StatusBusy` when `maxBatchesInFlight` batches are not yet retired (nothing is recorded);
 *              `kzstdgpu_StatusInvalidArgument` for invalid arguments. Decode failures are reported per frame in
 *              `frameStatus`, never silently.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitAllStagesWithExternalMemory(uint64_t *outBatchId,
                                                                     zstdgpu_SessionContext inSession,
                                                                     struct ID3D12Resource *inFramesMemory,
                                                                     uint32_t inFramesMemorySizeInBytes,
                                                                     struct ID3D12Resource *inFrames,
                                                                     uint32_t frameCount,
                                                                     struct ID3D12Resource *outFramesMemory,
                                                                     uint32_t outFramesMemorySizeInBytes,
                                                                     struct ID3D12Resource *outFrames,
                                                                     const zstdgpu_OffsetAndSize *outFramesCpu,
                                                                     struct ID3D12Resource *frameStatus);

/**
 *  @brief      Submits all required decompression commands into a command list for a given `stageIndex` with internally
 *              allocated memory.
 *
 *  @note       The caller must wait for GPU idle before calling this function if `zstdgpu_IsReadbackRequired` return `1`
 *              for `stageIndex` supplied into this function.
 *
 *  @note       Returns `kzstdgpu_StatusResourceCreationFailed` without recording any commands if a heap, resource or
 *              descriptor heap could not be created, or a resource could not be mapped (for example, after device
 *              removal). Do not submit later stages of the request after a failure.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitWithInternalMemory(zstdgpu_PerRequestContext inPerRequestContext, uint32_t stageIndex, struct ID3D12GraphicsCommandList *cmdList);

/**
 *  @brief      Submits all required decompression commands into a command list for all stages (single-submission mode)
 *              with internally allocated memory.
 *
 *  @note       It's valid to call this function only when `zstdgpu_IsAnyStageReadbackRequired` returns `0`
 *
 *  @note       Returns `kzstdgpu_StatusResourceCreationFailed` without recording any commands if a heap, resource or
 *              descriptor heap could not be created, or a resource could not be mapped (for example, after device
 *              removal).
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitAllStagesWithInternalMemory(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12GraphicsCommandList *cmdList);
