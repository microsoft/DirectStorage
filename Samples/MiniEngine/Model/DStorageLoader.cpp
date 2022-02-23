//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"

#include "Renderer.h"
#include "GraphicsCore.h"
#include "../H3DArchive/H3DArchive.h"
#include "DStorageLoader.h"

#include <zlib.h>

#define USE_PIX
#include <pix3.h>

#include <algorithm>


using Microsoft::WRL::ComPtr;

//
// Global state used by DStorageLoader
//

static ComPtr<IDStorageFactory> g_factory;
static ComPtr<IDStorageQueue> g_dsSystemMemoryQueue;
static ComPtr<IDStorageQueue> g_dsGpuQueue;
static ComPtr<IDStorageCustomDecompressionQueue> g_customDecompressionQueue;
static HANDLE g_customDecompressionQueueEvent;
static TP_WAIT* g_threadpoolWait;

static ComPtr<ID3D12Fence> g_fence;
static uint64_t g_fenceValue = 0;

//
// Support code for DStorageLoadH3DAInto
//

static DSTORAGE_COMPRESSION_FORMAT GetFormat(H3DCompression compression)
{
    switch (compression)
    {
        case H3DCompression::None:
            return DSTORAGE_COMPRESSION_FORMAT_NONE;

        case H3DCompression::Zlib:
            return DSTORAGE_CUSTOM_COMPRESSION_0;

        default:
            std::abort();
    }

}

static DSTORAGE_REQUEST BuildDStorageRequest(
    IDStorageFile* file,
    uint64_t offset,
    size_t uncompressedSize,
    size_t compressedSize,
    DSTORAGE_COMPRESSION_FORMAT compression,
    void* dest)
{
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r.Options.CompressionFormat = compression;
    r.Source.File.Source = file;
    r.Source.File.Offset = offset;
    r.Source.File.Size = (uint32_t)compressedSize;
    r.Destination.Memory.Buffer = dest;
    r.Destination.Memory.Size = (uint32_t)uncompressedSize;
    r.UncompressedSize = r.Destination.Memory.Size;
    return r;
}

static DSTORAGE_REQUEST BuildDStorageBufferRequest(
    IDStorageFile* file,
    uint64_t offset,
    size_t uncompressedSize,
    size_t compressedSize,
    DSTORAGE_COMPRESSION_FORMAT compression,
    ID3D12Resource* dest)
{
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
    r.Options.CompressionFormat = compression;
    r.Source.File.Source = file;
    r.Source.File.Offset = offset;
    r.Source.File.Size = (uint32_t)compressedSize;
    r.Destination.Buffer.Resource = dest;
    r.Destination.Buffer.Offset = 0;
    r.Destination.Buffer.Size = (uint32_t)uncompressedSize;
    r.UncompressedSize = r.Destination.Buffer.Size;
    return r;
}

static DSTORAGE_REQUEST BuildDStorageWholeTextureRequest(
    IDStorageFile* file,
    uint64_t offset,
    size_t uncompressedSize,
    size_t compressedSize,
    DSTORAGE_COMPRESSION_FORMAT compression,
    ID3D12Resource* resource)
{
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
    r.Options.CompressionFormat = compression;
    r.Source.File.Source = file;
    r.Source.File.Offset = offset;
    r.Source.File.Size = (uint32_t)compressedSize;
    r.Destination.MultipleSubresources.Resource = resource;
    r.Destination.MultipleSubresources.FirstSubresource = 0;
    r.UncompressedSize = (uint32_t)uncompressedSize;
    return r;
}

class DStorageLoader
{
    // These resources need to live as long as the DirectStorage requests are
    // executing.
    struct State
    {
        ComPtr<IDStorageFile> m_file;  
        HANDLE m_event;

        ~State()
        {
            if (m_event != INVALID_HANDLE_VALUE)
                CloseHandle(m_event);
        }
    };

    std::unique_ptr<State> m_state;

    H3DArchiveHeader m_header{};
    H3DArchivedTexture const* m_archivedTextures;

    std::map<std::string, ComPtr<ID3D12Resource>> m_textures;

public:
    DStorageLoadResult LoadInto(ComPtr<IDStorageFile> file, ModelH3D* model)
    {
        m_state.reset(new State{ std::move(file), CreateEvent(nullptr, FALSE, FALSE, nullptr) });
        ASSERT(m_state->m_event != INVALID_HANDLE_VALUE);

        PIXScopedEvent(0, "DStorageLoader::LoadInto");
        m_header = LoadHeader();
        auto cpuData = LoadCpuData(m_header.cpuDataOffset, m_header.uncompressedCpuDataSize, m_header.compressedCpuDataSize, GetFormat(m_header.compression));
        auto* meshes = reinterpret_cast<ModelH3D::Mesh*>(&cpuData[m_header.meshesOffset]);
        auto* materials = reinterpret_cast<ModelH3D::Material*>(&cpuData[m_header.materialsOffset]);
        m_archivedTextures = reinterpret_cast<H3DArchivedTexture*>(&cpuData[m_header.archivedTexturesOffset]);

        model->m_Header = *reinterpret_cast<ModelH3D::Header*>(&cpuData[0]);

        model->m_pMesh = new ModelH3D::Mesh[model->m_Header.meshCount];
        memcpy(model->m_pMesh, meshes, sizeof(*meshes) * model->m_Header.meshCount);

        model->m_pMaterial = new ModelH3D::Material[model->m_Header.materialCount];
        memcpy(model->m_pMaterial, materials, sizeof(*materials) * model->m_Header.materialCount);

        model->PostLoadMeshes();

        LoadGeometryDataIntoAsync(model);

        // The geometry data is quite large so takes a while to load and
        // decompress, so we submit it now so we can start work on right away.
        g_dsGpuQueue->Submit();

        LoadArchivedTexturesAsync();

        g_dsGpuQueue->EnqueueSignal(g_fence.Get(), ++g_fenceValue);
        g_dsGpuQueue->Submit();

        CreateGeometryViews(model);
        ConfigureMaterials(model);
        
        // The file object needs to be kept alive until all requests that use it
        // have been completed.  So we set up a wait callback when that happens
        // that can clean up this state.
        ASSERT_SUCCEEDED(g_fence->SetEventOnCompletion(g_fenceValue, m_state->m_event));
        auto event = m_state->m_event;
        auto wait = CreateThreadpoolWait(CleanupState, m_state.release(), nullptr);
        SetThreadpoolWait(wait, event, nullptr);

        DStorageLoadResult result{};
        result.Succeeded = true;
        result.Fence = g_fence;
        result.FenceValue = g_fenceValue;
        return result;
    }

private:
    static void CALLBACK CleanupState(
        TP_CALLBACK_INSTANCE*,
        void* voidState,
        TP_WAIT* wait,
        TP_WAIT_RESULT)
    {
        State* state = reinterpret_cast<State*>(voidState);
        delete state;

        CloseThreadpoolWait(wait);
    }

    H3DArchiveHeader LoadHeader()
    {
        // Synchronously read the archive header
        H3DArchiveHeader archiveHeader{};
        
        g_dsSystemMemoryQueue->EnqueueRequest(&BuildDStorageRequest(
            m_state->m_file.Get(), 
            0, 
            sizeof(archiveHeader), 
            sizeof(archiveHeader), 
            DSTORAGE_COMPRESSION_FORMAT_NONE, 
            &archiveHeader));
        g_dsSystemMemoryQueue->EnqueueSignal(g_fence.Get(), ++g_fenceValue);
        g_dsSystemMemoryQueue->Submit();

        WaitForFence();

        return archiveHeader;
    }

    std::vector<uint8_t> LoadCpuData(
        uint64_t offset, 
        uint64_t uncompressedSize, 
        uint64_t compressedSize,
        DSTORAGE_COMPRESSION_FORMAT format)
    {
        // Synchronously read the CPU data
        std::vector<uint8_t> buffer(uncompressedSize);

        g_dsSystemMemoryQueue->EnqueueRequest(&BuildDStorageRequest(m_state->m_file.Get(), offset, uncompressedSize, compressedSize, format, buffer.data()));
        g_dsSystemMemoryQueue->EnqueueSignal(g_fence.Get(), ++g_fenceValue);
        g_dsSystemMemoryQueue->Submit();

        WaitForFence();

        return buffer;
    }

    void LoadGeometryDataIntoAsync(ModelH3D* model)
    {
        auto offset = m_header.geometryDataOffset;
        auto uncompressedSize = m_header.uncompressedGeometryDataSize;
        auto compressedSize = m_header.compressedGeometryDataSize;
        auto compression = GetFormat(m_header.compression);
        
        model->m_GeometryBuffer.Create(L"Geometry Buffer", (uint32_t)uncompressedSize, 1);
        g_dsGpuQueue->EnqueueRequest(&BuildDStorageBufferRequest(m_state->m_file.Get(), offset, uncompressedSize, compressedSize, compression, model->m_GeometryBuffer.GetResource()));
    }

    void CreateGeometryViews(ModelH3D* model)
    {
        size_t offset = 0;
        model->m_VertexBuffer = model->m_GeometryBuffer.VertexBufferView(offset, model->m_Header.vertexDataByteSize, model->m_VertexStride);
        offset += model->m_Header.vertexDataByteSize;

        model->m_IndexBuffer = model->m_GeometryBuffer.IndexBufferView(offset, model->m_Header.indexDataByteSize, false);
        offset += model->m_Header.indexDataByteSize;

        model->m_VertexBufferDepth = model->m_GeometryBuffer.VertexBufferView(offset, model->m_Header.vertexDataByteSizeDepth, model->m_VertexStride);
        offset += model->m_Header.vertexDataByteSizeDepth;

        model->m_IndexBufferDepth = model->m_GeometryBuffer.IndexBufferView(offset, model->m_Header.indexDataByteSize, false);
        offset += model->m_Header.indexDataByteSize;
    }

    void LoadArchivedTexturesAsync()
    {
        // This arranges for all the textures in the archive to be loaded into D3D resources.
        // Later, we'll add these to the texture manager based on how each material wants to use the texture.

        auto* const device = Graphics::g_Device;

        for (auto i = 0u; i < m_header.archivedTexturesCount; ++i)
        {
            H3DArchivedTexture const& archivedTexture = m_archivedTextures[i];

            ComPtr<ID3D12Resource> resource;

            D3D12_HEAP_PROPERTIES heapProperties = { D3D12_HEAP_TYPE_DEFAULT };

            ASSERT_SUCCEEDED(device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &archivedTexture.desc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&resource)));

            std::wstring name(archivedTexture.path, archivedTexture.path + strlen(archivedTexture.path));
            resource->SetName(name.c_str());

            // This will asynchronously populate the texture with the data from
            // the file.    
            g_dsGpuQueue->EnqueueRequest(&BuildDStorageWholeTextureRequest(
                m_state->m_file.Get(), 
                m_header.texturesOffset + archivedTexture.offset, 
                archivedTexture.uncompressedSize, 
                archivedTexture.compressedSize,
                GetFormat(m_header.compression),
                resource.Get()));

            m_textures[archivedTexture.path] = std::move(resource);
        }
    }

    void ConfigureMaterials(ModelH3D* model)
    {
        // This is similar to ModelH3D::LoadTextures, except that we already have
        // GPU resources configured for the textures in m_textures.
        auto const& header = model->m_Header;

        model->m_TextureReferences.resize(header.materialCount * 3);
        model->m_SRVs = Renderer::s_TextureHeap.Alloc(header.materialCount * 6);
        model->m_SRVDescriptorSize = Renderer::s_TextureHeap.GetDescriptorSize();

        DescriptorHandle SRVs = model->m_SRVs;
        TextureRef* MatTextures = model->m_TextureReferences.data();

        for (uint32_t materialIdx = 0; materialIdx < header.materialCount; ++materialIdx)
        {
            using namespace Graphics;

            ModelH3D::Material const& material = model->m_pMaterial[materialIdx];

            MatTextures[0] = GetTexture(material.texDiffusePath, kWhiteOpaque2D, true);
            MatTextures[1] = GetTexture(material.texSpecularPath, kBlackOpaque2D, true);
            MatTextures[2] = GetTexture(material.texNormalPath, kDefaultNormalMap, false);

            uint32_t DestCount = 6;
            uint32_t SourceCounts[] = { 1, 1, 1, 1, 1, 1 };
            D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[6] =
            {
                MatTextures[0].GetSRV(),
                MatTextures[1].GetSRV(),
                GetDefaultTexture(kBlackTransparent2D),
                MatTextures[2].GetSRV(),
                GetDefaultTexture(kBlackTransparent2D),
                GetDefaultTexture(kBlackCubeMap)                
            };

            Graphics::g_Device->CopyDescriptors(1, &SRVs, &DestCount,
                DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            SRVs += (model->m_SRVDescriptorSize * 6);
            MatTextures += 3;
        }
    }

    TextureRef GetTexture(char const* path, Graphics::eDefaultTexture defaultTexture, bool forceSRGB)
    {
        std::string filename = path;

        auto entry = m_textures.find(filename);

        ID3D12Resource* resource;
        if (entry != m_textures.end())
            resource = entry->second.Get();
        else
            resource = nullptr;

        return TextureManager::LoadFromResource(filename.c_str(), resource, defaultTexture, forceSRGB);
    }

    void WaitForFence() const
    {
        ASSERT_SUCCEEDED(g_fence->SetEventOnCompletion(g_fenceValue, m_state->m_event));
        WaitForSingleObject(m_state->m_event, INFINITE);
    }
};

//
// Custom decompression implementation
//

// Each decompression request is processed by a number of decompression tasks.
// This prevents single large requests from clogging up the system by allowing
// us to work on different parts of a request in parallel. 
struct DecompressionRequest
{
    DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST Request;

    // Tasks use this to determine which block they should work on
    std::atomic<uint32_t> NextBlock;

    // Tasks use this to determine when they are the last task to complete (and
    // so should notify that the request has completed)
    std::atomic<uint32_t> NumBlocksCompleted;

    // Error code
    std::atomic<int> ErrorCode;
};

static void CALLBACK OnDecompress(
    TP_CALLBACK_INSTANCE*,
    void* context)
{
    PIXScopedEvent(0, "OnDecompress");

    // Give this thread a high priority to ensure we perform decompression without
    // excessive context switching between available cores.
    auto oldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    auto* dr = reinterpret_cast<DecompressionRequest*>(context);

    auto blockCount = GetH3DZlibBlockCount(dr->Request.DstSize);

    // Determine which block we're processing.  NextBlock is atomic, so we know
    // that fetch_add will always return a unique number.
    auto block = dr->NextBlock.fetch_add(1);

    ASSERT(block < blockCount); // This would mean that too many tasks were scheduled

    auto* blockOffsets = reinterpret_cast<uint32_t const*>(dr->Request.SrcBuffer);
    auto* blockStart = reinterpret_cast<uint8_t const*>(dr->Request.SrcBuffer) + blockOffsets[block];

    // The size of the source data can be determined by adjacent offsets, apart
    // from the last block where it consumes all the data until the end of the
    // src buffer.
    bool isEndBlock = (block == blockCount-1);
    auto blockSrcLength = isEndBlock
        ? dr->Request.SrcSize - blockOffsets[block]
        : blockOffsets[block + 1] - blockOffsets[block];

    // The size of the destination data is always ZLIB_BLOCK_SIZE, except for
    // the last block.
    auto blockDstLength = isEndBlock ? (dr->Request.DstSize % ZLIB_BLOCK_SIZE) : ZLIB_BLOCK_SIZE;

    // The decompression destination buffer is memory in an upload heap.  This
    // is write-combined memory which is performant only for sequential writes.
    // The zlib uncompress() function performs a mixture of both reads and
    // writes on the output buffer. Passing in the DirectStorage destination
    // buffer directly will yield poor performance.  We avoid this performance
    // hit by decompressing to a buffer in regular memory and then memcpy'ing
    // the result to the upload heap.

    uint8_t decompressedData[ZLIB_BLOCK_SIZE];

    PIXScopedEvent(0, "block: %d  src: %d  dst: %d", (int)block, (int)blockSrcLength, (int)blockDstLength);

    // Perform the actual decompression
    uLong actualDstLength = (uLong)blockDstLength;
    auto uncompressResult = uncompress(decompressedData, &actualDstLength, blockStart, (uLong)blockSrcLength);
    ASSERT(actualDstLength == blockDstLength);

    if (uncompressResult == Z_OK)
    {
        auto* dst = reinterpret_cast<uint8_t*>(dr->Request.DstBuffer) + block * ZLIB_BLOCK_SIZE;
        memcpy(dst, decompressedData, actualDstLength);
    }
    else
    {
        dr->ErrorCode = uncompressResult;    
    }

    // NumBlocksCompleted tracks how many blocks have been decompressed.  When
    // this reaches blockCount-1 we know that that all blocks have been
    // decompressed.
    if (dr->NumBlocksCompleted.fetch_add(1) == (blockCount-1))
    {
        // This task was the last one to finish, so it can now report results of
        // the decompression operation back to DirectStorage using
        // SetRequestResults(). If this is not done then the original
        // DirectStorage request will never complete.

        DSTORAGE_CUSTOM_DECOMPRESSION_RESULT result{};
        result.Id = dr->Request.Id;
        result.Result = dr->ErrorCode == Z_OK ? S_OK : E_FAIL;

        g_customDecompressionQueue->SetRequestResults(1, &result);

        // As this is last task to complete for this request, it is this task's
        // responsability to deallocate it.
        delete dr;
    }

    // From this point dr may have been deleted - either by this task or another
    // one.
    dr = nullptr;

    // Restore the original thread's priority back to its original setting.
    SetThreadPriority(GetCurrentThread(), oldPriority);
}

// The compressed data is split up into blocks, where each block
// decompresses to at most ZLIB_BLOCK_SIZE bytes.  We decompress each block
// in parallel, each one as a separate piece of work on the threadpool.
static void ScheduleDecompression(DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST const& request)
{
    // Allocate a DecompressionRequest on the heap.  The last task will
    // deallocate this.
    DecompressionRequest* r = new DecompressionRequest{ request, 0, 0, Z_OK };

    // Submit the decompression tasks - one per block
    auto blockCount = GetH3DZlibBlockCount(request.DstSize);
    for (auto i = 0u; i < blockCount; ++i)
    {
        TrySubmitThreadpoolCallback(OnDecompress, r, nullptr);
    }
}

// An event is signaled everytime a new request is added to the custom decompression
// queue. This event wakes up a threadpool thread and executes the following callback.
// The callback loops through all available requests and submits a decompression callback
// on a thread pool thread for each request.
static void CALLBACK OnCustomDecompressionRequest(
    TP_CALLBACK_INSTANCE*,
    void*,
    TP_WAIT* wait,
    TP_WAIT_RESULT)
{
    // Give this thread a high priority to ensure we perform decompression without
    // excessive context switching between available cores.
    auto oldPriority = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    PIXScopedEvent(0, "OnCustomDecompressionRequest");

    // Loop through all requests pending requests until no more remain.
    while (true)
    {
        DSTORAGE_CUSTOM_DECOMPRESSION_REQUEST requests[64];
        uint32_t numRequests = 0;

        // Pull off a batch of requests to process in this loop.
        ASSERT_SUCCEEDED(g_customDecompressionQueue->GetRequests(_countof(requests), requests, &numRequests));

        if (numRequests == 0)
            break;

        for (uint32_t i = 0; i < numRequests; ++i)
        {
            ScheduleDecompression(requests[i]);
        }
    }

    // Restore the original thread's priority back to its original setting.
    SetThreadPriority(GetCurrentThread(), oldPriority);

    // Re-register the custom decompression queue event with this callback
    // to be called when the next decompression requests become available for processing.
    SetThreadpoolWait(wait, g_customDecompressionQueueEvent, nullptr);
}

//
// Public entry points
//

void InitializeDStorage()
{
    ASSERT_SUCCEEDED(DStorageGetFactory(IID_PPV_ARGS(&g_factory)));

    // Create the system memory queue, used for reading data into system memory.  
    // Generally, this will be used for synchronous reading so we can get away with a small queue capacity.
    {
        DSTORAGE_QUEUE_DESC queueDesc{};
        queueDesc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
        queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
        queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;

        ASSERT_SUCCEEDED(g_factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&g_dsSystemMemoryQueue)));
    }

    // Create the GPU queue, used for reading GPU resources.  This will likely have more outstanding requests on it,
    // so a large queue capacity is warranted.
    {
        DSTORAGE_QUEUE_DESC queueDesc{};
        queueDesc.Device = Graphics::g_Device;
        queueDesc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
        queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
        queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;

        ASSERT_SUCCEEDED(g_factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&g_dsGpuQueue)));
    }

    // Configure custom decompression queue
    ASSERT_SUCCEEDED(g_factory.As(&g_customDecompressionQueue));
    g_customDecompressionQueueEvent = g_customDecompressionQueue->GetEvent();
    g_threadpoolWait = CreateThreadpoolWait(OnCustomDecompressionRequest, nullptr, nullptr);
    SetThreadpoolWait(g_threadpoolWait, g_customDecompressionQueueEvent, nullptr);

    // This is the fence that we ask DirectStorage to Signal when work has
    // completed.
    uint64_t fenceValue = 0;
    ASSERT_SUCCEEDED(Graphics::g_Device->CreateFence(fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));

}

void ShutdownDStorage()
{
    if (!g_factory)
        return;

    CloseThreadpoolWait(g_threadpoolWait);
    g_customDecompressionQueue.Reset();
    CloseHandle(g_customDecompressionQueueEvent);
    g_dsGpuQueue.Reset();
    g_dsSystemMemoryQueue.Reset();
    g_factory.Reset();
}


DStorageLoadResult DStorageLoadH3DAInto(ModelH3D* model, const std::wstring& filename)
{
    ComPtr<IDStorageFile> file;
    if (FAILED(g_factory->OpenFile(filename.c_str(), IID_PPV_ARGS(&file))))
    {
        return {false};
    }

    DStorageLoader loader;
    return loader.LoadInto(std::move(file), model);
}
