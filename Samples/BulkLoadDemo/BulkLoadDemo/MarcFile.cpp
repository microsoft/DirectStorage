//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "MarcFile.h"

#include "DStorageLoader.h"
#include "MultiHeap.h"

#include <GraphicsCommon.h>
#include <Renderer.h>
#include <d3dx12.h>
#include <wrl/wrappers/corewrappers.h>

using Graphics::g_Device;
using namespace Math;

static DSTORAGE_COMPRESSION_FORMAT ToCompressionFormat(marc::Compression compression)
{
    using marc::Compression;

    switch (compression)
    {
    case Compression::None:
        return DSTORAGE_COMPRESSION_FORMAT_NONE;

    case Compression::GDeflate:
        return DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;

    case Compression::Zlib:
        return CUSTOM_COMPRESSION_FORMAT_ZLIB;

    default:
        throw std::runtime_error("Unknown marc::Compression value");
    }
}

// Converts a given Ptr inside a region from an Offset to a pointer.
template<typename T1, typename T2>
void Fixup(MemoryRegion<T1>& region, marc::Ptr<T2>& ptr)
{
    auto offset = ptr.Offset;
    char* data = region.Data() + offset;

    ptr.Ptr = reinterpret_cast<T2*>(data);
}

MarcFile::MarcFile(std::filesystem::path const& path)
    : m_headerLoaded(EventWait::Create<MarcFile, &MarcFile::OnHeaderLoaded>(this))
    , m_cpuMetadataLoaded(EventWait::Create<MarcFile, &MarcFile::OnCpuMetadataLoaded>(this))
    , m_cpuDataLoaded(EventWait::Create<MarcFile, &MarcFile::OnCpuDataLoaded>(this))
    , m_gpuDataLoaded(EventWait::Create<MarcFile, &MarcFile::OnGpuDataLoaded>(this))
{
    CheckHR(g_dsFactory->OpenFile(path.wstring().c_str(), IID_PPV_ARGS(&m_file)));
    CheckHR(g_dsFactory->CreateStatusArray(
        static_cast<uint32_t>(StatusArrayEntry::NumEntries),
        nullptr,
        IID_PPV_ARGS(&m_statusArray)));
}

MarcFile::~MarcFile()
{
    m_cpuDataLoaded.Close();
    m_gpuDataLoaded.Close();

    // All requests created for this instance are tagged with 'this', so we can
    // cancel any outstanding requests.
    g_dsSystemMemoryQueue->CancelRequestsWithTag(0xFFFFFFFFFFFFll, reinterpret_cast<uint64_t>(this));
    g_dsGpuQueue->CancelRequestsWithTag(0xFFFFFFFFFFFFll, reinterpret_cast<uint64_t>(this));
}

//
// Starts the metadata loading process.  To load the metadata we need to load
// the header and then the CPU metadata region.
//
void MarcFile::StartMetadataLoad()
{
    std::unique_lock lock{m_mutex};

    ValidateState(InternalState::FileOpen);

    EnqueueRead(0, &m_header);

    m_headerLoaded.SetThreadpoolWait();
    g_dsSystemMemoryQueue->EnqueueStatus(m_statusArray.Get(), static_cast<uint32_t>(StatusArrayEntry::Metadata));
    g_dsSystemMemoryQueue->EnqueueSetEvent(m_headerLoaded);
    g_dsSystemMemoryQueue->Submit();

    m_state = InternalState::LoadingHeader;
}

//
// This is called on the threadpool when the m_headerLoaded event is set.  Now
// that the header is loaded we have enough data to load the metadata
// region.
//
void MarcFile::OnHeaderLoaded()
{
    std::unique_lock lock{m_mutex};

    ValidateState(InternalState::LoadingHeader);

    m_status = m_statusArray->GetHResult(static_cast<uint32_t>(StatusArrayEntry::Metadata));

    if (m_header.Version != marc::CURRENT_MARC_FILE_VERSION || FAILED(m_status))
    {
        m_state = InternalState::Error;
        return;
    }

    m_cpuMetadata = EnqueueReadMemoryRegion<marc::CpuMetadataHeader>(m_header.CpuMetadata);

    m_cpuMetadataLoaded.SetThreadpoolWait();
    g_dsSystemMemoryQueue->EnqueueSetEvent(m_cpuMetadataLoaded);
    g_dsSystemMemoryQueue->Submit();

    m_state = InternalState::LoadingCpuMetadata;
}

//
// This is called on the threadpool once the m_cpuMetadataLoaded event is set.
// The various Ptr's within need to be fixed up, and we can calculate some
// device-specific information (eg allocation infos) and allocate some scratch
// memory (the non-shader visible descriptor heap) that will be kept resident
// with the MarcFile.
//
void MarcFile::OnCpuMetadataLoaded()
{
    std::unique_lock lock{m_mutex};

    ValidateState(InternalState::LoadingCpuMetadata);

    Fixup(m_cpuMetadata, m_cpuMetadata->Textures.Data);
    Fixup(m_cpuMetadata, m_cpuMetadata->TextureDescs.Data);

    for (uint32_t i = 0; i < m_cpuMetadata->NumTextures; ++i)
    {
        Fixup(m_cpuMetadata, m_cpuMetadata->Textures[i].Name);
        Fixup(m_cpuMetadata, m_cpuMetadata->Textures[i].SingleMips.Data);
    }

    ComPtr<ID3D12Device4> device4;
    CheckHR(Graphics::g_Device->QueryInterface(IID_PPV_ARGS(&device4)));
    if (!IsOk())
        return;

    m_textureAllocationInfos.resize(m_cpuMetadata->NumTextures);

    m_overallTextureAllocationInfo = device4->GetResourceAllocationInfo1(
        0,
        m_cpuMetadata->NumTextures,
        m_cpuMetadata->TextureDescs.Data.Ptr,
        m_textureAllocationInfos.data());

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = m_cpuMetadata->NumTextures;

    CheckHR(g_Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptorHeap)));
    if (!IsOk())
        std::abort();

    m_state = InternalState::MetadataReady;
}

//
// Starts the content loading process.  All of the DirectStorage requests can be
// enqueued immediately.  Once both sets of work have been completed the final
// fixups can be applied to the data, and a MiniEngine Model class can be
// instantiated.
//
void MarcFile::StartContentLoad(
    std::vector<MultiHeapAllocation> const& texturesAllocations,
    DescriptorHandle textureHandles,
    MultiHeapAllocation buffersAllocation)
{
    std::unique_lock lock{m_mutex};

    ValidateState(InternalState::MetadataReady);

    m_state = InternalState::LoadingContent;

    m_textureHandles = textureHandles;

    LoadCpuData();
    LoadGpuData(texturesAllocations, buffersAllocation);
}

void MarcFile::LoadCpuData()
{
    // assumes mutex is locked

    m_cpuData = EnqueueReadMemoryRegion<marc::CpuDataHeader>(m_header.CpuData);
    g_dsSystemMemoryQueue->EnqueueStatus(m_statusArray.Get(), static_cast<uint32_t>(StatusArrayEntry::CpuData));

    m_cpuDataLoaded.SetThreadpoolWait();
    g_dsSystemMemoryQueue->EnqueueSetEvent(m_cpuDataLoaded);

    g_dsSystemMemoryQueue->Submit();
}

void MarcFile::LoadGpuData(
    std::vector<MultiHeapAllocation> const& texturesAllocations,
    MultiHeapAllocation buffersAllocation)
{
    m_textures.reserve(m_cpuMetadata->NumTextures);
    for (uint32_t i = 0; i < m_cpuMetadata->NumTextures; ++i)
    {
        m_textures.push_back(EnqueueReadTexture(
            texturesAllocations[i].Heap.Get(),
            texturesAllocations[i].Offset,
            m_cpuMetadata->TextureDescs[i],
            m_cpuMetadata->Textures[i]));
    }

    m_gpuBuffer = EnqueueReadBufferRegion(
        buffersAllocation.Heap.Get(),
        buffersAllocation.Offset,
        m_header.UnstructuredGpuData);

    g_dsGpuQueue->EnqueueStatus(m_statusArray.Get(), static_cast<uint32_t>(StatusArrayEntry::GpuData));

    m_gpuDataLoaded.SetThreadpoolWait();
    g_dsGpuQueue->EnqueueSetEvent(m_gpuDataLoaded);

    g_dsGpuQueue->Submit();
}

void MarcFile::OnCpuDataLoaded()
{
    Fixup(m_cpuData, m_cpuData->SceneGraph.Data);
    Fixup(m_cpuData, m_cpuData->Meshes);
    Fixup(m_cpuData, m_cpuData->Materials.Data);
    Fixup(m_cpuData, m_cpuData->Animations.Data);
    Fixup(m_cpuData, m_cpuData->AnimationCurves.Data);
    Fixup(m_cpuData, m_cpuData->KeyFrameData);
    Fixup(m_cpuData, m_cpuData->JointIndices.Data);
    Fixup(m_cpuData, m_cpuData->JointIBMs.Data);

    std::unique_lock lock{m_mutex};
    if (!IsOk())
        return;

    ValidateState(InternalState::LoadingContent, InternalState::GpuDataLoaded);

    for (uint32_t i = 0; i < m_cpuData->NumAnimationCurves; ++i)
    {
        if (m_cpuData->AnimationCurves[i].targetPath == AnimationCurve::kWeights)
        {
            // Blend shape weights are not supported
            m_status = E_NOTIMPL;
            m_state = InternalState::Error;
            return;
        }
    }

    if (m_state == InternalState::GpuDataLoaded)
        OnAllDataLoaded();
    else
        m_state = InternalState::CpuDataLoaded;
}

void MarcFile::OnGpuDataLoaded()
{
    std::unique_lock lock{m_mutex};
    if (!IsOk())
        return;

    ValidateState(InternalState::LoadingContent, InternalState::CpuDataLoaded);

    if (m_state == InternalState::CpuDataLoaded)
        OnAllDataLoaded();
    else
        m_state = InternalState::GpuDataLoaded;
}

//
// This is called when both the CPU and GPU data has finished loading.  At this
// point all of the data is there and we just need to fix it up and create the
// Model instance.
//
void MarcFile::OnAllDataLoaded()
{
    // assumes that m_mutex is already locked.

    CheckHR(m_statusArray->GetHResult(static_cast<uint32_t>(StatusArrayEntry::CpuData)));
    if (!IsOk())
        return;

    CheckHR(m_statusArray->GetHResult(static_cast<uint32_t>(StatusArrayEntry::GpuData)));
    if (!IsOk())
        return;

    FixupMaterials();

    m_state = InternalState::ContentLoaded;

    m_model = std::make_shared<Model>();
    m_model->m_BoundingSphere = BoundingSphere(*(XMFLOAT4*)&m_header.BoundingSphere);
    m_model->m_BoundingBox = AxisAlignedBox(Vector3(*(XMFLOAT3*)m_header.MinPos), Vector3(*(XMFLOAT3*)m_header.MaxPos));
    m_model->m_NumNodes = m_cpuData->NumSceneGraphNodes;
    m_model->m_NumMeshes = m_cpuData->NumMeshes;
    m_model->m_NumAnimations = m_cpuData->NumAnimations;
    m_model->m_NumJoints = m_cpuData->NumJoints;

    m_model->m_MaterialConstants = m_gpuBuffer->GetGPUVirtualAddress() + m_cpuData->MaterialConstantsGpuOffset;
    m_model->m_DataBuffer = m_gpuBuffer->GetGPUVirtualAddress();

    m_model->m_MeshData = m_cpuData->Meshes.Ptr;
    m_model->m_SceneGraph = m_cpuData->SceneGraph.Data.Ptr;
    m_model->m_KeyFrameData = m_cpuData->KeyFrameData.Ptr;
    m_model->m_CurveData = m_cpuData->AnimationCurves.Data.Ptr;
    m_model->m_Animations = m_cpuData->Animations.Data.Ptr;
    m_model->m_JointIndices = m_cpuData->JointIndices.Data.Ptr;
    m_model->m_JointIBMs = m_cpuData->JointIBMs.Data.Ptr;
}

static std::unordered_map<uint32_t, uint32_t> g_SamplerPermutations;

static D3D12_CPU_DESCRIPTOR_HANDLE GetSampler(uint32_t addressModes)
{
    SamplerDesc samplerDesc;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(addressModes & 0x3);
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(addressModes >> 2);
    return samplerDesc.CreateDescriptor();
}

void MarcFile::FixupMaterials()
{
    using namespace Graphics;

    CreateTextureDescriptors();
    auto increment = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptors = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();

    std::vector<uint32_t> tableOffsets(m_cpuMetadata->NumMaterials);
    DescriptorHandle textureHandles = m_textureHandles;

    for (uint32_t matIdx = 0; matIdx < m_cpuMetadata->NumMaterials; ++matIdx)
    {
        marc::Material const& srcMat = m_cpuData->Materials[matIdx];

        uint32_t srvDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(textureHandles);

        uint32_t destCount = kNumTextures;
        uint32_t sourceCounts[kNumTextures] = {1, 1, 1, 1, 1};

        static D3D12_CPU_DESCRIPTOR_HANDLE defaultTextures[kNumTextures] = {
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kBlackTransparent2D),
            GetDefaultTexture(kDefaultNormalMap)};

        D3D12_CPU_DESCRIPTOR_HANDLE sourceTextures[kNumTextures];
        for (uint32_t j = 0; j < kNumTextures; ++j)
        {
            if (srcMat.TextureIndex[j] == 0xffff)
                sourceTextures[j] = defaultTextures[j];
            else
                sourceTextures[j] = CD3DX12_CPU_DESCRIPTOR_HANDLE(cpuDescriptors, srcMat.TextureIndex[j], increment);
        }

        g_Device->CopyDescriptors(
            1,
            &textureHandles,
            &destCount,
            destCount,
            sourceTextures,
            sourceCounts,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // See if this combination of samplers has been used before.  If not,
        // allocate more from the heap and copy in the descriptors.
        uint32_t addressModes = srcMat.AddressModes;
        auto samplerMapLookup = g_SamplerPermutations.find(addressModes);

        if (samplerMapLookup == g_SamplerPermutations.end())
        {
            DescriptorHandle SamplerHandles = Renderer::s_SamplerHeap.Alloc(kNumTextures);
            uint32_t SamplerDescriptorTable = Renderer::s_SamplerHeap.GetOffsetOfHandle(SamplerHandles);
            g_SamplerPermutations[addressModes] = SamplerDescriptorTable;
            tableOffsets[matIdx] = srvDescriptorTable | SamplerDescriptorTable << 16;

            D3D12_CPU_DESCRIPTOR_HANDLE SourceSamplers[kNumTextures];
            for (uint32_t j = 0; j < kNumTextures; ++j)
            {
                SourceSamplers[j] = GetSampler(addressModes & 0xF);
                addressModes >>= 4;
            }
            g_Device->CopyDescriptors(
                1,
                &SamplerHandles,
                &destCount,
                destCount,
                SourceSamplers,
                sourceCounts,
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        else
        {
            tableOffsets[matIdx] = srvDescriptorTable | samplerMapLookup->second << 16;
        }

        textureHandles += increment * kNumTextures;
    }

    // Update table offsets for each mesh
    uint8_t* meshPtr = m_cpuData->Meshes.Ptr;
    for (uint32_t i = 0; i < m_cpuData->NumMeshes; ++i)
    {
        Mesh& mesh = *(Mesh*)meshPtr;
        uint32_t offsetPair = tableOffsets[mesh.materialCBV];
        mesh.srvTable = offsetPair & 0xFFFF;
        mesh.samplerTable = offsetPair >> 16;
        mesh.pso = Renderer::GetPSO(mesh.psoFlags);
        meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
}

void MarcFile::CreateTextureDescriptors()
{
    auto increment = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptors = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t i = 0; i < m_cpuMetadata->NumTextures; ++i)
    {
        g_Device->CreateShaderResourceView(
            m_textures[i].Get(),
            nullptr,
            CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptors, i, increment));
    }
}

//
// Enqueues a read of a single, fixed-size, uncompressed piece of data.
//
template<typename T>
void MarcFile::EnqueueRead(uint64_t offset, T* dest)
{
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    r.Source.File.Source = m_file.Get();
    r.Source.File.Offset = offset;
    r.Source.File.Size = static_cast<uint32_t>(sizeof(T));
    r.Destination.Memory.Buffer = dest;
    r.Destination.Memory.Size = r.Source.File.Size;
    r.UncompressedSize = r.Destination.Memory.Size;
    r.CancellationTag = reinterpret_cast<uint64_t>(this);

    g_dsSystemMemoryQueue->EnqueueRequest(&r);
}

//
// Enqueues a read of a memory region. This allocates the buffer to store the
// region.
//
// The T template parameters specifies the type of the header of the region.
// Regions may be larger than sizeof(T).
//
template<typename T>
MemoryRegion<T> MarcFile::EnqueueReadMemoryRegion(marc::Region<T> const& region)
{
    MemoryRegion<T> dest(std::make_unique<char[]>(region.UncompressedSize));

    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
    r.Options.CompressionFormat = ToCompressionFormat(region.Compression);
    r.Source.File.Source = m_file.Get();
    r.Source.File.Offset = region.Data.Offset;
    r.Source.File.Size = region.CompressedSize;
    r.Destination.Memory.Buffer = dest.Data();
    r.Destination.Memory.Size = region.UncompressedSize;
    r.UncompressedSize = r.Destination.Memory.Size;
    r.CancellationTag = reinterpret_cast<uint64_t>(this);

    g_dsSystemMemoryQueue->EnqueueRequest(&r);

    return dest;
}

//
// Reads a given region into a D3D12 Buffer.  This buffer is placed at the
// specified heap+offest.
//
ComPtr<ID3D12Resource> MarcFile::EnqueueReadBufferRegion(
    ID3D12Heap* heap,
    uint64_t offset,
    marc::GpuRegion const& region)
{
    ComPtr<ID3D12Resource> resource;

    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(m_header.UnstructuredGpuData.UncompressedSize);
    CheckHR(g_Device->CreatePlacedResource(heap, offset, &bufferDesc, 
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&resource)));
    if (!IsOk())
        std::abort();

    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
    r.Options.CompressionFormat = ToCompressionFormat(region.Compression);
    r.Source.File.Source = m_file.Get();
    r.Source.File.Offset = region.Data.Offset;
    r.Source.File.Size = region.CompressedSize;
    r.Destination.Buffer.Offset = 0;
    r.Destination.Buffer.Resource = resource.Get();
    r.Destination.Buffer.Size = region.UncompressedSize;
    r.UncompressedSize = r.Destination.Buffer.Size;
    r.CancellationTag = reinterpret_cast<uint64_t>(this);

    g_dsGpuQueue->EnqueueRequest(&r);

    return resource;
}

//
// Reads a texture, as described by the desc and textureMetadata.  The texture
// is placed at the giveen heap+offset.
//
ComPtr<ID3D12Resource> MarcFile::EnqueueReadTexture(
    ID3D12Heap* heap,
    uint64_t offset,
    D3D12_RESOURCE_DESC const& desc,
    marc::TextureMetadata const& textureMetadata)
{
    ComPtr<ID3D12Resource> resource;

    CheckHR(g_Device->CreatePlacedResource(
        heap, 
        offset,
        &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    if (!IsOk())
        std::abort();

#ifdef DEBUG
    std::string nname = textureMetadata.Name.Ptr;
    std::wstring name(nname.begin(), nname.end());
    resource->SetName(name.c_str());
#endif

    // See comment around TextureMetadata in MarcFileFormat.h for more
    // information on this structure.

    for (uint32_t i = 0; i < textureMetadata.NumSingleMips; ++i)
    {
        marc::GpuRegion const& region = textureMetadata.SingleMips[i];

        DSTORAGE_REQUEST r = BuildRequestForRegion(region);
        r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
        r.Destination.Texture.Resource = resource.Get();
        r.Destination.Texture.SubresourceIndex = i;

        D3D12_BOX destBox{};
        destBox.right = static_cast<uint32_t>(desc.Width >> i);
        destBox.bottom = desc.Height >> i;
        destBox.back = 1;

        r.Destination.Texture.Region = destBox;

        g_dsGpuQueue->EnqueueRequest(&r);
    }

    if (textureMetadata.RemainingMips.UncompressedSize != 0)
    {
        DSTORAGE_REQUEST r = BuildRequestForRegion(textureMetadata.RemainingMips);
        r.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES;
        r.Destination.MultipleSubresources.Resource = resource.Get();
        r.Destination.MultipleSubresources.FirstSubresource = textureMetadata.NumSingleMips;
        g_dsGpuQueue->EnqueueRequest(&r);
    }

    return resource;
}

//
// This constructs a DSTORAGE_REQUEST that will read all the data from the
// region, ready for the destination fields to be filled in.
//
template<typename T>
DSTORAGE_REQUEST MarcFile::BuildRequestForRegion(marc::Region<T> const& region)
{
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    r.Options.CompressionFormat = ToCompressionFormat(region.Compression);
    r.Source.File.Source = m_file.Get();
    r.Source.File.Offset = region.Data.Offset;
    r.Source.File.Size = region.CompressedSize;
    r.UncompressedSize = region.UncompressedSize;
    r.CancellationTag = reinterpret_cast<uint64_t>(this);

    return r;
}

//
// Releases all memory/resources used by the content of this MarcFile.  It is
// the caller's responsability to ensure that none of these resources are
// currently in use by the GPU.
//
void MarcFile::UnloadContent()
{
    std::unique_lock lock(m_mutex);
    if (!IsOk())
        return;

    ValidateState(InternalState::MetadataReady, InternalState::ContentLoaded);

    if (m_state == InternalState::ContentLoaded)
    {
        m_cpuData = {};
        m_textures.clear();
        m_gpuBuffer.Reset();
    }
    m_state = InternalState::MetadataReady;
}

//
// MarcFile exposes a more limited set of states through its public interface.
// This function converts between the internal and external states.
//
MarcFile::State MarcFile::GetState() const
{
    std::unique_lock lock(m_mutex);
    switch (m_state)
    {
    case InternalState::FileOpen:
    case InternalState::LoadingHeader:
    case InternalState::LoadingCpuMetadata:
        return State::Initializing;

    case InternalState::MetadataReady:
        return State::ReadyToLoadContent;

    case InternalState::LoadingContent:
    case InternalState::CpuDataLoaded:
    case InternalState::GpuDataLoaded:
        return State::ContentLoading;

    case InternalState::ContentLoaded:
        return State::ContentLoaded;

    case InternalState::Error:
    default:
        return State::Error;
    }
}

bool MarcFile::IsMetadataReady() const
{
    // assumes mutex is locked
    return StateIsOneOf(InternalState::MetadataReady, InternalState::LoadingContent, InternalState::ContentLoaded);
}

MarcFile::DataSize MarcFile::GetRequiredDataSize() const
{
    std::unique_lock lock(m_mutex);

    if (!IsOk())
        return {};

    if (!IsMetadataReady())
        throw std::runtime_error("GetRequiredDataSize called before metadata is ready");

    DataSize size{};
    size.TexturesByteCount =
        Math::AlignUp(m_overallTextureAllocationInfo.SizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    size.BuffersByteCount = m_header.UnstructuredGpuData.UncompressedSize;
    size.CpuByteCount = m_header.CpuData.UncompressedSize;
    size.GpuAlignment = m_overallTextureAllocationInfo.Alignment;
    size.NumTextureHandles = m_cpuMetadata->NumMaterials * kNumTextures;

    size.GDeflateByteCount = 0;
    size.ZLibByteCount = 0;

    auto accumulateSize = [&size](auto const& region)
    {
        switch (region.Compression)
        {
        case marc::Compression::None:
            size.UncompressedByteCount += region.UncompressedSize;
            break;

        case marc::Compression::GDeflate:
            size.GDeflateByteCount += region.UncompressedSize;
            break;

        case marc::Compression::Zlib:
            size.ZLibByteCount += region.UncompressedSize;
            break;

        default:
            std::abort();
        }
    };

    accumulateSize(m_header.UnstructuredGpuData);
    accumulateSize(m_header.CpuData);

    size.TexturesByteCount = 0;

    for (uint32_t textureIndex = 0; textureIndex < m_cpuMetadata->NumTextures; ++textureIndex)
    {
        auto& texture = m_cpuMetadata->Textures[textureIndex];

        for (uint32_t singleMipIndex = 0; singleMipIndex < texture.NumSingleMips; ++singleMipIndex)
        {
            accumulateSize(texture.SingleMips[singleMipIndex]);
            size.TexturesByteCount += texture.SingleMips[singleMipIndex].UncompressedSize;
        }

        accumulateSize(texture.RemainingMips);
        size.TexturesByteCount += texture.RemainingMips.UncompressedSize;
    }

    return size;
}

std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> const& MarcFile::GetTextureAllocationInfos() const
{
    return m_textureAllocationInfos;
}

std::shared_ptr<Model> MarcFile::GetModel()
{
    std::unique_lock lock(m_mutex);
    ValidateState(InternalState::ContentLoaded);

    return m_model;
}

void MarcFile::CheckHR(HRESULT hr)
{
    if (FAILED(hr))
    {
        m_state = InternalState::Error;
        m_status = hr;
    }
}

template<typename... States>
void MarcFile::ValidateState(States... states) const
{
    if (!StateIsOneOf(states...))
        throw std::runtime_error("Called in incorrect state");
}

template<typename... States>
bool MarcFile::StateIsOneOf(States... states) const
{
    for (InternalState state : {states...})
    {
        if (m_state == state)
            return true;
    }

    return false;
}

bool MarcFile::IsOk() const
{
    return m_state != InternalState::Error;
}
