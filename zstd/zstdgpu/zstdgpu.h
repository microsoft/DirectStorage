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
    kzstdgpu_StatusForceInt                 = 0x7fffffff
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
 *  @brief      Supplies the per-frame resume state used to continue a frame across submissions.
 *
 *  `resumeState` must be a buffer of `4 * frameCount` `uint32_t`s in `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`.
 *  Pass `clearResumeState` of 1 for the first slice of a sequence and 0 for every subsequent slice:
 *  the library reads the carried state at the start of a decode and rewrites it at the end, so
 *  clearing mid-sequence would silently restart every frame.
 *
 *  The clear is explicit rather than inferred from `blockStart == 0` because a batch mixes frames
 *  that are starting with frames that are continuing, and a wrong guess produces plausible-looking
 *  wrong output rather than a failure.
 *
 *  It must be caller-owned because the library's scratch heaps do not survive between submissions,
 *  and resuming is precisely a cross-submission operation. Passing NULL (the default) makes the
 *  library allocate and zero its own, which reproduces whole-frame behaviour exactly.
 *
 *  The two pieces of state it carries are the ones a mid-frame slice cannot reconstruct:
 *
 *      - the output cursor. The caller cannot compute this, because a compressed block's
 *        decompressed size is only discovered by decoding it.
 *      - the repeat offsets at the frame's last decoded sequence. zstd's `1/4/8` defaults apply only
 *        at the true start of a frame.
 *
 *  Note this does NOT remove the requirement that preceding slices' output is still present in the
 *  destination buffer: matches read it as history.
 *
 *  Can be called before or after the `zstdgpu_SetupInputs*` functions.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupResumeState(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *resumeState, uint32_t clearResumeState);

/**
 *  @brief      Decode only blocks `[blockStartPerFrame, blockStartPerFrame + blockLimitPerFrame)` of
 *              every frame. A limit of 0 means "to the end of the frame", so the default
 *              `(0, 0)` is the whole frame.
 *
 *  This is intra-frame slicing. A slice starting at block 0 needs no carried state, so it is correct
 *  as-is. A slice starting later requires both of the following:
 *
 *      - the output cursor and the repeat offsets, both carried on the GPU via
 *        `zstdgpu_SetupResumeState`. Neither can be supplied host-side: a compressed block's
 *        decompressed size is only discovered by decoding it, and zstd's `1/4/8` repeat-offset
 *        defaults apply only at the true start of a frame. (Advancing the frame's destination
 *        offset in `zstdgpu_SetupOutputs` is an alternative way to carry the cursor, but only for
 *        content whose decoded size the caller already knows, i.e. RAW and RLE blocks.)
 *      - the preceding slices' output still being present in the destination buffer, because
 *        matches read it as history through absolute destination addresses. A slice decoded into a
 *        fresh buffer sees zeros there.
 *
 *  EVERY BLOCK ORDINAL IS A LEGAL CUT -- no host-side legality scan is needed.
 *
 *  zstd reuses entropy tables across blocks within a frame: a sequences section may select FSE
 *  `Repeat_Mode`, and a `Treeless_Literals_Block` reuses the Huffman table of the last
 *  `Compressed_Literals_Block`. A slice beginning at such a block has none of those tables in its
 *  own dispatch.
 *
 *  The library resolves this itself. Compressed blocks *before* the window are carried through the
 *  parse as "entropy only": their section headers are parsed so their Huffman weights and FSE tables
 *  are built, and their literal payloads are skipped outright. They emit zero literals, zero
 *  sequences and zero output bytes, so they consume no arena scratch and contribute nothing to any
 *  destination offset -- they exist solely so in-window blocks can resolve `Repeat_Mode` and
 *  `Treeless` against them. Only blocks before the window need this, because table definitions flow
 *  forwards; blocks after the window are still skipped entirely.
 *
 *  The cost is bounded by table size (FSE <= 512 entries, Huffman <= 256), not by payload size, and
 *  grows with the slice's start ordinal within its frame.
 *
 *  Blocks beyond the limit are still walked -- zstd block boundaries are only discoverable
 *  sequentially -- but emit nothing, so the scratch consumed is that of the decoded prefix rather
 *  than of the whole frame. Memory requirements are unaffected and remain sized for the whole frame,
 *  so a sliced decode never needs more memory than an unsliced one.
 *
 *  With multiple frames each frame is truncated independently and its output still lands at that
 *  frame's own destination.
 *
 *  NB: this window is a single scalar applied uniformly to every frame in the batch. There is no
 *      way to slice one frame while decoding its neighbours whole -- use
 *      `zstdgpu_SetupBlockWindowPerFrame` for that.
 *
 *  NB: validating a partial decode requires a reference that truncates at the same block boundary.
 *      Comparing against a whole-frame reference is not meaningful, so callers that validate must
 *      leave this at 0.
 *
 *  Can be called before or after the `zstdgpu_SetupInputs*` functions.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupBlockLimitPerFrame(zstdgpu_PerRequestContext inPerRequestContext, uint32_t blockStartPerFrame, uint32_t blockLimitPerFrame);

/**
 *  @brief      Supplies a PER-FRAME block window, replacing the batch-uniform scalars set by
 *              `zstdgpu_SetupBlockLimitPerFrame`.
 *
 *  `blockWindowPerFrame` must be a buffer of `2 * frameCount` `uint32_t`s in
 *  `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`, holding `(blockStart, blockLimit)` per frame in
 *  frame order. Passing NULL (the default) restores the scalar window, so a caller that never slices
 *  sees no behaviour change.
 *
 *  Per-frame semantics match the scalar API exactly: `blockLimit == 0` means "to the end of the
 *  frame", so `(0, 0)` is the whole frame and is what a frame that is not being sliced carries.
 *
 *  `kzstdgpu_BlockWindowSkipFrame` as a frame's `blockStart` means "decode nothing from this frame".
 *  It is needed because a batch is a contiguous frame range while frames finish at different slice
 *  counts, so an already-completed frame still sits inside the range. It is NOT equivalent to a
 *  start past the frame's last block: that still pays to parse every compressed block for entropy
 *  tables no in-window block ever uses.
 *
 *  WHY THIS EXISTS. The scalar window is applied identically to every frame in a batch, so a batch
 *  cannot slice one frame while decoding its neighbours whole. That forces one frame per batch
 *  whenever anything is sliced, and makes packing whole frames alongside a slice piece impossible.
 *
 *  The window is consumed by the frame parse, which runs in both the counting and the collecting
 *  dispatch, and both receive the same buffer -- so the emitted records and the counts that size
 *  them cannot disagree.
 *
 *  NB: a host pre-scan that counts whole frames (`zstdgpu_CountFramesAndBlocks`) remains a valid
 *      upper bound for sizing, because windowing a frame can only ever emit fewer blocks than the
 *      frame contains.
 *
 *  Can be called before or after the `zstdgpu_SetupInputs*` functions.
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SetupBlockWindowPerFrame(zstdgpu_PerRequestContext inPerRequestContext, struct ID3D12Resource *blockWindowPerFrame);

/**
 *  @brief      Converts a per-slice DECOMPRESSED byte budget into the number of block ordinals a
 *              slice may span, for use as `blockLimitPerFrame`.
 *
 *  The budget is in decompressed bytes because that is the axis the caller actually has: scratch is
 *  sized from decompressed size, so a caller with a scratch ceiling converts it to a decompressed
 *  budget and slices against that. It never has to count the frame's blocks, and no block header is
 *  read on the host.
 *
 *  THE IDENTITY THAT REMOVES THE HOST PARSE: a zstd block decodes to at most 128 KiB, so a slice
 *  spanning K block ordinals decodes at most `K * 128 KiB` -- whatever the content is. K is
 *  therefore derivable from the byte budget alone, and slice boundaries are plain arithmetic
 *  (`0, K, 2K, ...`). This is the same kind of provable bound the scratch sizing uses, and for the
 *  same reason: a compressed block's decompressed size is only discoverable by decoding it.
 *
 *  A budget below one block rounds up to one, since a block is the indivisible unit of slicing.
 *
 *  NB: the bound is on the SLICE, not on the frame. The number of slices a frame needs is
 *      `ceil(blockCount / K)`, and `zstdgpu_CountFramesAndBlocks` yields that block count exactly,
 *      so a caller that wants it never has to estimate. Alternatively just iterate until a slice
 *      reports it reached the end of the frame.
 */
ZSTDGPU_API uint32_t zstdgpu_SliceBlockCountForDecompressedBudget(uint64_t decompressedByteBudget);

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
 *  @brief      Submits all required decompression commands into a command list for all stages (single-submission mode)
 *              with externally supplied memory.
 *
 *  @note       It's valid to call this function only when `zstdgpu_IsAnyStageReadbackRequired` returns `0`
 *
 *  @note       The caller must compute required memory via `zstdgpu_GetAllStageGpuMemoryRequirement`
 *
 *  @note       Returns `kzstdgpu_StatusResourceCreationFailed` without recording any commands if a resource could not be
 *              created in the supplied heaps or mapped (for example, after device removal).
 */
ZSTDGPU_API zstdgpu_Status zstdgpu_SubmitAllStagesWithExternalMemory(zstdgpu_PerRequestContext inPerRequestContext,
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
