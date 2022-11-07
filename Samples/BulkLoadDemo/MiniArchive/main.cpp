//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"

#include "../BulkLoadDemo/MarcFileFormat.h"
#include "../Model/ModelLoader.h"
#include "../Model/TextureConvert.h"
#include "../Model/glTF.h"

#include <DirectXTex.h>
#include <dstorage.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

//
// Global state used for compressing
//

static ComPtr<IDStorageCompressionCodec> g_bufferCompression;

template<typename T>
static std::remove_reference_t<T> Compress(marc::Compression compression, T&& source)
{
    if (compression == marc::Compression::None)
    {
        return source;
    }
    else
    {
        size_t maxSize;
        if (compression == marc::Compression::GDeflate)
            maxSize = g_bufferCompression->CompressBufferBound(static_cast<uint32_t>(source.size()));
        else if (compression == marc::Compression::Zlib)
            maxSize = static_cast<size_t>(compressBound(static_cast<uLong>(source.size())));
        else
            throw std::runtime_error("Unknown Compression type");

        std::remove_reference_t<T> dest;
        dest.resize(maxSize);

        size_t actualCompressedSize = 0;

        HRESULT compressionResult = S_OK;

        if (compression == marc::Compression::GDeflate)
        {
            compressionResult = g_bufferCompression->CompressBuffer(
                reinterpret_cast<const void*>(source.data()),
                static_cast<uint32_t>(source.size()),
                DSTORAGE_COMPRESSION_BEST_RATIO,
                reinterpret_cast<void*>(dest.data()),
                static_cast<uint32_t>(dest.size()),
                &actualCompressedSize);
        }
        else if (compression == marc::Compression::Zlib)
        {
            uLong destSize = static_cast<uLong>(dest.size());
            int result = compress(
                reinterpret_cast<Bytef*>(dest.data()),
                &destSize,
                reinterpret_cast<Bytef const*>(source.data()),
                static_cast<uLong>(source.size()));

            if (result == Z_OK)
                compressionResult = S_OK;
            else
                compressionResult = E_FAIL;

            actualCompressedSize = destSize;
        }

        if (FAILED(compressionResult))
        {
            std::cout << "Failed to compress data using CompressBuffer, hr = 0x" << std::hex << compressionResult
                      << std::endl;
            std::abort();
        }

        dest.resize(actualCompressedSize);

        return dest;
    }
}

static std::string Compress(marc::Compression compression, std::stringstream sourceStream)
{
    auto source = sourceStream.str();
    return Compress(compression, std::move(source));
}

template<typename T>
void WriteArray(std::ostream& s, T const* data, size_t count)
{
    s.write((char*)data, sizeof(*data) * count);
}

template<typename CONTAINER>
marc::Array<typename CONTAINER::value_type> WriteArray(std::ostream& s, CONTAINER const& data)
{
    auto pos = s.tellp();
    WriteArray(s, data.data(), data.size());

    marc::Array<typename CONTAINER::value_type> array;
    array.Data.Offset = static_cast<uint32_t>(pos);

    return array;
}

static std::streampos PadToAlignment(std::ostream& s, uint64_t alignment)
{
    auto pos = s.tellp();

    if (pos % alignment)
    {
        // it's not aligned!
        uint64_t desiredOffset = ((pos / alignment) + 1) * alignment;
        uint64_t padding = desiredOffset - pos;
        while (padding)
        {
            s.put(0);
            --padding;
        }
        return s.tellp();
    }
    else
    {
        return pos;
    }
}

template<typename CONTAINER>
marc::Ptr<typename CONTAINER::value_type> WriteElementAlignedArray(
    std::ostream& s,
    CONTAINER const& data,
    uint64_t alignment)
{
    auto pos = PadToAlignment(s, alignment);

    for (auto& element : data)
    {
        s.write(reinterpret_cast<char const*>(&element), sizeof(element));
        PadToAlignment(s, alignment);
    }

    assert(pos % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);

    marc::Ptr<typename CONTAINER::value_type> ptr;
    ptr.Offset = static_cast<uint32_t>(pos);

    return ptr;
}

void Patch(std::ostream& s, std::streampos pos, void const* data, size_t size)
{
    auto oldPos = s.tellp();
    s.seekp(pos);
    s.write(reinterpret_cast<char const*>(data), size);
    s.seekp(oldPos);
}

template<typename T>
void Patch(std::ostream& s, std::streampos pos, T const& value)
{
    Patch(s, pos, &value, sizeof(value));
}

template<typename T>
void Patch(std::ostream& s, std::streampos pos, std::vector<T> const& values)
{
    Patch(s, pos, values.data(), values.size() * sizeof(T));
}

template<typename T>
class Fixup
{
    std::streampos m_pos;

public:
    Fixup() = default;

    Fixup(std::streampos fixupPos)
        : m_pos(fixupPos)
    {
    }

    void Set(std::ostream& stream, T const& value) const
    {
        Patch(stream, m_pos, value);
    }

    // Overload that only works when T is a marc::Ptr<> (SFINAE)
    void Set(std::ostream& stream, std::streampos value) const
    {
        T t;
        t.OffsetFromRegionStart = static_cast<uint64_t>(value);
        Set(stream, t);
    }
};

template<typename T, typename FIXUP>
Fixup<FIXUP> MakeFixup(std::streampos startPos, T const* src, FIXUP const* fixup)
{
    auto byteSrc = reinterpret_cast<uint8_t const*>(src);
    auto byteField = reinterpret_cast<uint8_t const*>(fixup);
    std::ptrdiff_t offset = byteField - byteSrc;

    if ((offset + sizeof(*fixup)) > sizeof(*src))
    {
        throw std::runtime_error("Fixup outside of src structure");
    }

    std::streampos fixupPos = startPos + offset;

    return Fixup<FIXUP>(fixupPos);
}

template<typename T, typename... FIXUPS>
std::tuple<Fixup<FIXUPS>...> WriteStruct(std::ostream& out, T const* src, FIXUPS const*... fixups)
{
    auto startPos = out.tellp();
    out.write(reinterpret_cast<char const*>(src), sizeof(*src));

    return std::make_tuple(MakeFixup(startPos, src, fixups)...);
}

static void Set(float dest[4], Math::BoundingSphere const& src)
{
    dest[0] = src.GetCenter().GetX();
    dest[1] = src.GetCenter().GetY();
    dest[2] = src.GetCenter().GetZ();
    dest[3] = src.GetRadius();
}

static void Set(float dest[3], Math::Vector3 const& src)
{
    dest[0] = src.GetX();
    dest[1] = src.GetY();
    dest[2] = src.GetZ();
}

namespace
{
    using namespace marc;

    class Exporter
    {
        std::ostream& m_out;
        Compression m_compression;
        TexConversionFlags m_extraTextureFlags;
        uint32_t m_stagingBufferSizeBytes;
        glTF::Asset const& m_asset;
        Renderer::ModelData const& m_modelData;

        ComPtr<ID3D12Device> m_device;

        std::streampos m_materialConstantsGpuOffset;

        struct TextureMetadata
        {
            std::vector<marc::GpuRegion> SingleMips;
            marc::GpuRegion RemainingMips;
        };

        std::vector<TextureMetadata> m_textureMetadata;
        std::vector<D3D12_RESOURCE_DESC> m_textureDescs;

        Exporter(
            std::ostream& out,
            Compression compression,
            TexConversionFlags extraTextureFlags,
            uint32_t stagingBufferSizeBytes,
            glTF::Asset const& asset,
            Renderer::ModelData const& modelData)
            : m_out(out)
            , m_stagingBufferSizeBytes(stagingBufferSizeBytes)
            , m_compression(compression)
            , m_extraTextureFlags(extraTextureFlags)
            , m_asset(asset)
            , m_modelData(modelData)
        {
            if (auto hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)); FAILED(hr))
            {
                std::cout << "Failed to create D3D12 device: 0x" << std::hex << hr << std::endl;
                throw std::runtime_error("Failed to create D3D12 device");
            }
        }

        void Export()
        {
            Header header;
            header.Id[0] = 'M';
            header.Id[1] = 'A';
            header.Id[2] = 'R';
            header.Id[3] = 'C';
            header.Version = marc::CURRENT_MARC_FILE_VERSION;
            Set(header.BoundingSphere, m_modelData.m_BoundingSphere);
            Set(header.MinPos, m_modelData.m_BoundingBox.GetMin());
            Set(header.MaxPos, m_modelData.m_BoundingBox.GetMax());

            auto [fixupHeader] = WriteStruct(m_out, &header, &header);

            WriteTextures();
            header.UnstructuredGpuData = WriteUnstructuredGpuData();
            header.CpuMetadata = WriteCpuMetadata();
            header.CpuData = WriteCpuData();

            fixupHeader.Set(m_out, header);
        }

        void WriteTextures()
        {
            for (size_t i = 0; i < m_modelData.m_TextureNames.size(); ++i)
            {
                std::string const& textureName = m_modelData.m_TextureNames[i];
                uint8_t flags = m_modelData.m_TextureOptions[i];
                flags |= m_extraTextureFlags;
                WriteTexture(textureName, flags);
            }
        }

        marc::GpuRegion WriteTextureRegion(
            uint32_t currentSubresource,
            uint32_t numSubresources,
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> const& layouts,
            std::vector<UINT> const& numRows,
            std::vector<UINT64> const& rowSizes,
            uint64_t totalBytes,
            std::vector<D3D12_SUBRESOURCE_DATA> const& subresources,
            std::string const& name)
        {
            std::vector<char> data(totalBytes);

            for (auto i = 0u; i < numSubresources; ++i)
            {
                auto const& layout = layouts[i];
                auto const& subresource = subresources[currentSubresource + i];

                D3D12_MEMCPY_DEST memcpyDest{};
                memcpyDest.pData = data.data() + layout.Offset;
                memcpyDest.RowPitch = layout.Footprint.RowPitch;
                memcpyDest.SlicePitch = layout.Footprint.RowPitch * numRows[i];

                MemcpySubresource(
                    &memcpyDest,
                    &subresource,
                    static_cast<SIZE_T>(rowSizes[i]),
                    numRows[i],
                    layout.Footprint.Depth);
            }

            return WriteRegion<void>(data, name.c_str());
        }

        void WriteTexture(std::string const& name, uint8_t flags)
        {
            std::filesystem::path texturePath = m_asset.m_basePath;
            texturePath /= name;
            texturePath = absolute(texturePath);

            std::cout << "Converting " << name << std::endl;
            auto image = BuildDDS(texturePath.wstring().c_str(), flags);

            if (!image)
            {
                throw std::runtime_error("Texture load failed");
            }

            TexMetadata const& metadata = image->GetMetadata();

            std::vector<D3D12_SUBRESOURCE_DATA> subresources;
            if (auto hr = PrepareUpload(
                    m_device.Get(),
                    image->GetImages(),
                    image->GetImageCount(),
                    image->GetMetadata(),
                    subresources);
                FAILED(hr))
            {
                std::cout << texturePath.c_str() << " failed to prepare layout 0x" << std::hex << hr << std::endl;
                throw std::runtime_error("Texture preparation failed");
            }

            D3D12_RESOURCE_DESC desc{};
            desc.Width = static_cast<UINT>(metadata.width);
            desc.Height = static_cast<UINT>(metadata.height);
            desc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
            desc.DepthOrArraySize = (metadata.dimension == TEX_DIMENSION_TEXTURE3D)
                                        ? static_cast<UINT16>(metadata.depth)
                                        : static_cast<UINT16>(metadata.arraySize);
            desc.Format = metadata.format;
            desc.SampleDesc.Count = 1;
            desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(metadata.dimension);

            auto const totalSubresourceCount = CD3DX12_RESOURCE_DESC(desc).Subresources(m_device.Get());

            std::vector<GpuRegion> regions;

            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(totalSubresourceCount);
            std::vector<UINT> numRows(totalSubresourceCount);
            std::vector<UINT64> rowSizes(totalSubresourceCount);

            uint64_t totalBytes = 0;

            uint32_t currentSubresource = 0;

            m_device->GetCopyableFootprints(
                &desc,
                currentSubresource,
                totalSubresourceCount - currentSubresource,
                0,
                layouts.data(),
                numRows.data(),
                rowSizes.data(),
                &totalBytes);

            while (totalBytes > m_stagingBufferSizeBytes)
            {
                m_device->GetCopyableFootprints(
                    &desc,
                    currentSubresource,
                    1,
                    0,
                    layouts.data(),
                    numRows.data(),
                    rowSizes.data(),
                    &totalBytes);

                if (totalBytes > m_stagingBufferSizeBytes)
                {
                    std::cout << "Mip " << currentSubresource << " won't fit in the staging buffer.\n"
                              << "Try adding -stagingbuffersize=" << ((totalBytes + 1024 * 1024 - 1) / 1024 / 1024)
                              << " to the command-line" << std::endl;
                    std::exit(1);
                }

                std::stringstream regionName;
                regionName << name << " mip " << currentSubresource;

                regions.push_back(WriteTextureRegion(
                    currentSubresource,
                    1,
                    layouts,
                    numRows,
                    rowSizes,
                    totalBytes,
                    subresources,
                    regionName.str()));

                ++currentSubresource;

                if (currentSubresource < totalSubresourceCount)
                {
                    m_device->GetCopyableFootprints(
                        &desc,
                        currentSubresource,
                        totalSubresourceCount - currentSubresource,
                        0,
                        layouts.data(),
                        numRows.data(),
                        rowSizes.data(),
                        &totalBytes);
                }
            }

            marc::GpuRegion remainingMipsRegion{};

            if (currentSubresource < totalSubresourceCount)
            {
                std::stringstream regionName;
                regionName << name << " mips " << currentSubresource << " to " << totalSubresourceCount;
                remainingMipsRegion = WriteTextureRegion(
                    currentSubresource,
                    totalSubresourceCount - currentSubresource,
                    layouts,
                    numRows,
                    rowSizes,
                    totalBytes,
                    subresources,
                    regionName.str());
            }

            TextureMetadata textureMetadata;
            textureMetadata.SingleMips = std::move(regions);
            textureMetadata.RemainingMips = remainingMipsRegion;

            m_textureMetadata.push_back(std::move(textureMetadata));

            m_textureDescs.push_back(desc);
        }

        GpuRegion WriteUnstructuredGpuData()
        {
            std::stringstream s;

            WriteArray(s, m_modelData.m_GeometryData);

            m_materialConstantsGpuOffset = WriteElementAlignedArray(
                                               s,
                                               m_modelData.m_MaterialConstants,
                                               D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
                                               .Offset;

            return WriteRegion<void>(s.str(), "GPU Data");
        }

        template<typename T, typename C>
        Region<T> WriteRegion(C uncompressedRegion, char const* name)
        {
            size_t uncompressedSize = uncompressedRegion.size();

            C compressedRegion;

            Compression compression = m_compression;

            if (compression == Compression::None)
            {
                compressedRegion = std::move(uncompressedRegion);
            }
            else
            {
                compressedRegion = Compress(m_compression, uncompressedRegion);
                if (compressedRegion.size() > uncompressedSize)
                {
                    compression = Compression::None;
                    compressedRegion = std::move(uncompressedRegion);
                }
            }

            Region<T> r;
            r.Compression = compression;
            r.Data.Offset = static_cast<uint32_t>(m_out.tellp());
            r.CompressedSize = static_cast<uint32_t>(compressedRegion.size());
            r.UncompressedSize = static_cast<uint32_t>(uncompressedSize);

            if (r.Compression == Compression::None)
            {
                assert(r.CompressedSize == r.UncompressedSize);
            }

            m_out.write(compressedRegion.data(), compressedRegion.size());

            auto toString = [](Compression c)
            {
                switch (c)
                {
                case Compression::None:
                    return "Uncompressed";
                case Compression::GDeflate:
                    return "GDeflate";
                case Compression::Zlib:
                    return "Zlib";
                default:
                    throw std::runtime_error("Unknown compression format");
                }
            };

            std::cout << r.Data.Offset << ":  " << name << " " << toString(r.Compression) << " " << r.UncompressedSize
                      << " --> " << r.CompressedSize << "\n";

            return r;
        }

        Region<CpuMetadataHeader> WriteCpuMetadata()
        {
            std::stringstream s;

            CpuMetadataHeader header{};

            // Write placeholder header; we'll fix this up later when we've
            // written everything else out
            auto [fixupHeader] = WriteStruct(s, &header, &header);

            // Textures
            header.NumTextures = static_cast<uint32_t>(m_modelData.m_TextureNames.size());

            std::vector<marc::TextureMetadata> textureMetadata;
            textureMetadata.reserve(m_textureMetadata.size());

            // Write out the texture metadata, building up the blittable
            // versions as we go

            assert(m_textureMetadata.size() == m_modelData.m_TextureNames.size());
            for (size_t i = 0; i < m_textureMetadata.size(); ++i)
            {
                marc::TextureMetadata metadata{};
                metadata.Name = WriteArray(s, m_modelData.m_TextureNames[i]).Data;
                s.put(0); // null terminate the name string

                metadata.NumSingleMips = static_cast<uint32_t>(m_textureMetadata[i].SingleMips.size());
                metadata.SingleMips = WriteArray(s, m_textureMetadata[i].SingleMips);
                metadata.RemainingMips = m_textureMetadata[i].RemainingMips;
                textureMetadata.push_back(metadata);
            }

            header.Textures = WriteArray(s, textureMetadata);
            header.TextureDescs = WriteArray(s, m_textureDescs);

            header.NumMaterials = static_cast<uint32_t>(m_modelData.m_MaterialConstants.size());

            // Fixup the CPU data header
            fixupHeader.Set(s, header);

            return WriteRegion<CpuMetadataHeader>(s.str(), "CPU Metadata");
        }

        Region<CpuDataHeader> WriteCpuData()
        {
            std::stringstream s;

            CpuDataHeader header{};

            // Write placeholder header; we'll fix this up later when we've
            // written everything else out
            auto [fixupHeader] = WriteStruct(s, &header, &header);

            // Scene Graph

            header.NumSceneGraphNodes = static_cast<uint32_t>(m_modelData.m_SceneGraph.size());
            header.SceneGraph = WriteArray(s, m_modelData.m_SceneGraph);

            // Meshes.  
            //
            // Each "mesh" actually consists of a header - the Mesh struct -
            // followed by a number of Mesh::Draw structs.
            header.NumMeshes = static_cast<uint32_t>(m_modelData.m_Meshes.size());
            header.Meshes.Offset = static_cast<uint32_t>(s.tellp());

            for (size_t i = 0; i < m_modelData.m_Meshes.size(); ++i)
            {
                Mesh const* mesh = m_modelData.m_Meshes[i];
                s.write(
                    reinterpret_cast<char const*>(mesh),
                    sizeof(Mesh) + (sizeof(Mesh::Draw) * (mesh->numDraws - 1)));
            }

            // Materials
            header.MaterialConstantsGpuOffset = static_cast<uint32_t>(m_materialConstantsGpuOffset);
            assert(m_modelData.m_MaterialConstants.size() == m_modelData.m_MaterialTextures.size());
            header.Materials.Data.Offset = static_cast<uint32_t>(s.tellp());

            for (auto& materialTextureData : m_modelData.m_MaterialTextures)
            {
                Material m{};
                for (size_t i = 0; i < _countof(m.TextureIndex); ++i)
                {
                    m.TextureIndex[i] = materialTextureData.stringIdx[i];
                }

                m.AddressModes = materialTextureData.addressModes;

                WriteStruct(s, &m);
            }

            // Animations
            header.NumAnimations = static_cast<uint32_t>(m_modelData.m_Animations.size());
            header.Animations = WriteArray(s, m_modelData.m_Animations);

            // Animation Curves
            header.NumAnimationCurves = static_cast<uint32_t>(m_modelData.m_AnimationCurves.size());
            header.AnimationCurves = WriteArray(s, m_modelData.m_AnimationCurves);

            // Key Frame Data
            header.KeyFrameData = WriteArray(s, m_modelData.m_AnimationKeyFrameData).Data;

            // Joints
            header.NumJoints = static_cast<uint32_t>(m_modelData.m_JointIndices.size());
            header.JointIndices = WriteArray(s, m_modelData.m_JointIndices);
            header.JointIBMs = WriteArray(s, m_modelData.m_JointIBMs);

            // Fixup the CPU data header
            fixupHeader.Set(s, header);

            return WriteRegion<CpuDataHeader>(s.str(), "CPU Data");
        }

    public:
        static void Export(
            std::ostream& out,
            Compression compression,
            TexConversionFlags extraTextureFlags,
            uint32_t stagingBufferSizeBytes,
            glTF::Asset const& asset,
            Renderer::ModelData const& modelData)
        {
            Exporter exporter(out, compression, extraTextureFlags, stagingBufferSizeBytes, asset, modelData);
            exporter.Export();
        }
    };
} // namespace

static void ShowUsage(char const* exeName)
{
    std::cout << "Usage: " << exeName << " [-gdeflate|-zlib] [-stagingbuffersize=X] [-bc] source.gltf dest.marc\n";
    std::cout << "\n\nStaging buffer size is in MiB.  Default is 256 MiB.\n";
}

int main(int argc, char** argv)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool useGDeflate = false;
    bool useZlib = false;
    bool useBC = false;
    uint32_t stagingBufferSizeMiB = 256;
    char const* sourceFilename = nullptr;
    char const* destFilename = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        char const* arg = argv[i];
        std::regex stagingBufferRegex{"-stagingbuffersize=([0-9]+)", std::regex_constants::icase};
        std::cmatch match;

        if (_strcmpi(arg, "-gdeflate") == 0)
            useGDeflate = true;
        else if (_strcmpi(arg, "-zlib") == 0)
            useZlib = true;
        else if (_strcmpi(arg, "-bc") == 0)
            useBC = true;
        else if (std::regex_match(arg, match, stagingBufferRegex))
            stagingBufferSizeMiB = atoi(match[1].first);
        else if (!sourceFilename)
            sourceFilename = arg;
        else if (!destFilename)
            destFilename = arg;
    }

    if (useZlib && useGDeflate)
    {
        std::cout << "Only one of -zlib or -gdeflate may be specified at a time." << std::endl;
        ShowUsage(argv[0]);
        return -1;
    }

    marc::Compression compression = marc::Compression::None;
    if (useGDeflate)
        compression = marc::Compression::GDeflate;
    else if (useZlib)
        compression = marc::Compression::Zlib;

    if (!(sourceFilename && destFilename))
    {
        ShowUsage(argv[0]);
        return -1;
    }

    std::filesystem::path sourcePath(sourceFilename);
    sourcePath.make_preferred();

    std::cout << "Source: " << sourcePath.string().c_str() << std::endl;

    Renderer::ModelData modelData;
    glTF::Asset asset(sourcePath.wstring());
    constexpr int sceneIndex = -1;
    constexpr bool compileTextures = false;
    if (!BuildModel(modelData, asset, sceneIndex, compileTextures))
    {
        std::cout << "Unable to read source gltf file" << std::endl;
        return -1;
    }

    if (useGDeflate)
    {
        // Get the buffer compression interface for DSTORAGE_COMPRESSION_FORMAT_GDEFLATE
        constexpr uint32_t NumCompressionThreads = 6;
        ASSERT_SUCCEEDED(DStorageCreateCompressionCodec(
            DSTORAGE_COMPRESSION_FORMAT_GDEFLATE,
            NumCompressionThreads,
            IID_PPV_ARGS(&g_bufferCompression)));
    }

    TexConversionFlags extraTextureFlags{};
    if (useBC)
    {
        extraTextureFlags = static_cast<TexConversionFlags>(extraTextureFlags | kDefaultBC);
    }

    std::filesystem::path destPath(destFilename);
    destPath.make_preferred();

    std::cout << "Dest: " << destPath.string().c_str() << std::endl;

    std::ofstream outStream(destPath, std::ios::out | std::ios::trunc | std::ios::binary);

    Exporter::Export(outStream, compression, extraTextureFlags, stagingBufferSizeMiB * 1024 * 1024, asset, modelData);

    outStream.close();

    return 0;
}
