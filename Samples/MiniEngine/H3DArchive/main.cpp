//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "pch.h"

#include "H3DArchive.h"

#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <string>
#include <sstream>

#include <DirectXTex.h>
#include <zlib.h>

using Microsoft::WRL::ComPtr;

struct Model
{
    ModelH3D::Header header;
    std::unique_ptr<ModelH3D::Mesh[]> meshes;
    std::unique_ptr<ModelH3D::Material[]> materials;
    std::unique_ptr<char[]> geometryData;
    size_t geometryDataSize;
};

static std::optional<Model> TryReadModel(char const* filename)
{
    std::ifstream source(filename, std::ios::in | std::ios::binary);
    if (!source)
    {
        std::cout << "Could not open " << filename << std::endl;
        return std::nullopt;
    }

    Model m{};
    source.read((char *)&m.header, sizeof(m.header));

    m.meshes = std::make_unique<ModelH3D::Mesh[]>(m.header.meshCount);
    source.read((char *)m.meshes.get(), sizeof(ModelH3D::Mesh) * m.header.meshCount);

    m.materials = std::make_unique<ModelH3D::Material[]>(m.header.materialCount);
    source.read((char *)m.materials.get(), sizeof(ModelH3D::Material) * m.header.materialCount);

    m.geometryDataSize = m.header.vertexDataByteSize + m.header.indexDataByteSize + m.header.vertexDataByteSizeDepth + m.header.indexDataByteSize;

    m.geometryData = std::make_unique<char[]>(m.geometryDataSize);
    source.read((char *)m.geometryData.get(), m.geometryDataSize);

    if (!source)
        return std::nullopt;

    return m;
}

template<typename T>
void WriteStruct(std::ostream& s, T const& data)
{
    s.write((char *)&data, sizeof(data));
}

template<typename T>
void WriteArray(std::ostream& s, T const* data, size_t count)
{
    s.write((char *)data, sizeof(*data) * count);
}

void FixupMaterialTextures(std::filesystem::path const& basePath, Model* model)
{
    using std::filesystem::path;

    for (auto i = 0u; i < model->header.materialCount; ++i)
    {
        ModelH3D::Material& material = model->materials[i];

        path diffusePath = path(material.texDiffusePath);
        if (!diffusePath.empty())
            diffusePath.replace_extension("dds");

        path specularPath = path(material.texSpecularPath);
        if (!specularPath.empty())
            specularPath.replace_extension("dds");

        if (specularPath.empty() || !exists(basePath / specularPath))
        {
            specularPath.replace_filename(diffusePath.stem().string() + "_specular.dds");
            if (!exists(basePath / specularPath))
                specularPath.clear();
        }

        path normalPath = path(material.texNormalPath);
        if (normalPath.empty() || !normalPath.empty())
            normalPath.replace_extension("dds");

        if (!exists(basePath / normalPath))
        {
            normalPath.replace_filename(diffusePath.stem().string() + "_normal.dds");
            if (!exists(basePath / normalPath))
                normalPath.clear();
        }

        /*
        if (!diffusePath.empty())
            std::cout << material.texDiffusePath << " --> " << diffusePath << std::endl;

        if (!specularPath.empty())
            std::cout << material.texSpecularPath << " --> " << specularPath << std::endl;
        
        if (!normalPath.empty())
            std::cout << material.texNormalPath << " --> " << normalPath << std::endl;
        */

        strcpy_s(material.texDiffusePath, diffusePath.string().c_str());
        strcpy_s(material.texSpecularPath, specularPath.string().c_str());
        strcpy_s(material.texNormalPath, normalPath.string().c_str());        
    }
}

template<typename T>
static std::remove_reference_t<T> Compress(H3DCompression compression, T&& source, char const* name)
{
    if (compression == H3DCompression::None)
    {
        return std::move(source);
    }
    else if (compression == H3DCompression::Zlib)
    {
        auto numBlocks = GetH3DZlibBlockCount(static_cast<uint64_t>(source.size()));

        using block = std::vector<uint8_t>;
        std::vector<block> blocks;

        for (auto i = 0; i < numBlocks; ++i)
        {
            uint64_t sourceOffset = i * ZLIB_BLOCK_SIZE;
            uint8_t* sourceStart = reinterpret_cast<uint8_t*>(source.data()) + (i * ZLIB_BLOCK_SIZE);
            uint64_t sourceSize = std::min(ZLIB_BLOCK_SIZE, source.size() - sourceOffset);

            auto maxSize = compressBound((uLong)sourceSize);
            
            uLongf compressedSize = maxSize;
            block compressedBlock(maxSize);
            compress((Bytef*)compressedBlock.data(), &compressedSize, (Bytef*)sourceStart, (uLong)sourceSize);
            
            compressedBlock.resize(compressedSize);

            blocks.push_back(std::move(compressedBlock));
        }        

        size_t totalBlocksSize = std::accumulate(blocks.begin(), blocks.end(), (size_t)0,
            [] (size_t total, block const& b)
            {
                return total + b.size();
            });
            
        size_t offsetTableSize = numBlocks * sizeof(uint32_t);

        std::remove_reference_t<T> dest;
        dest.resize(offsetTableSize + totalBlocksSize);

        uint32_t* blockOffsets = reinterpret_cast<uint32_t*>(dest.data());
        size_t nextBlockOffset = offsetTableSize;

        for (auto i = 0; i < numBlocks; ++i)
        {            
            // Update the offset entry for this block
            blockOffsets[i] = (uint32_t)nextBlockOffset;

            // Copy the block itself
            std::copy(blocks[i].begin(), blocks[i].end(), dest.begin() + nextBlockOffset);            

            nextBlockOffset += blocks[i].size();
        }

        auto ratio = (double)dest.size() / (double)source.size();
        std::cout << " zlib compressed " << name << " (1:" << ratio << ") " << numBlocks << " blocks " << std::endl;

        return dest;
    }
    else
    {
        std::abort();
    }
}

static std::string Compress(H3DCompression compression, std::stringstream sourceStream, char const* name)
{
    auto source = sourceStream.str();
    return Compress(compression, std::move(source), name);
}

class TextureWriter
{
    struct Texture
    {
        std::streampos location;
        H3DArchivedTexture entry;        
    };

    std::vector<Texture> m_textures;
    bool m_error = false;

    ComPtr<ID3D12Device> m_device;

public:
    explicit TextureWriter(Model const& model)
    {
        // the same materials may refer to the same texture, but we only want to include
        // each one once.
        std::set<std::string> seen;

        for (auto i = 0u; i < model.header.materialCount; ++i)
        {
            CollectMaterial(&seen, model.materials[i]);
        }

        if (auto hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)); FAILED(hr))
        {
            std::cout << "Failed to create D3D12 device: 0x" << std::hex << hr << std::endl;
            m_error = true;
        }
    }

    operator bool() const
    {
        return !m_error;
    }

    size_t GetCount() const
    {
        return m_textures.size();
    }

    void WriteCpuData(std::ostream& cpuStream)
    {
        for (auto& t : m_textures)
        {
            t.location = cpuStream.tellp();
            WriteStruct(cpuStream, t.entry);
            // We'll patch up these entries when we actually write out the textures.
        }
    }

    void WriteGpuData(std::ostream& gpuStream, std::filesystem::path const& basePath, H3DCompression compression)
    {
        for (auto& t : m_textures)
        {
            WriteTexture(gpuStream, basePath, &t.entry, compression);
        }    
    }

    void FixupCpuData(std::ostream& cpuStream)
    {
        auto startPos = cpuStream.tellp();

        for (auto& t : m_textures)
        {
            cpuStream.seekp(t.location);
            WriteStruct(cpuStream, t.entry);
        }

        cpuStream.seekp(startPos);    
    }

private:
    void CollectMaterial(std::set<std::string>* seen, ModelH3D::Material const& material)
    {
        CollectTexture(seen, material.texDiffusePath);
        CollectTexture(seen, material.texSpecularPath);
        CollectTexture(seen, material.texEmissivePath);
        CollectTexture(seen, material.texNormalPath);
        CollectTexture(seen, material.texLightmapPath);
        CollectTexture(seen, material.texReflectionPath);
    }

    bool CollectTexture(std::set<std::string>* seen, std::string path)
    {
        if (path.empty())
            return false;

        if (seen->count(path) > 0)
            return false;

        seen->insert(path);

        Texture t{};
        strcpy_s(t.entry.path, path.c_str());

        m_textures.push_back(t);
        return true;
    }

    void WriteTexture(std::ostream& gpuStream, std::filesystem::path const& basePath, H3DArchivedTexture* t, H3DCompression compression)
    {
        t->offset = gpuStream.tellp();

        std::filesystem::path ddsFile(t->path);
        ddsFile = basePath / ddsFile;

        TexMetadata metadata;
        ScratchImage image;
        if (auto hr = LoadFromDDSFile(ddsFile.wstring().c_str(), DDS_FLAGS_NONE, &metadata, image); FAILED(hr))
        {
            std::cout << ddsFile.c_str() << " failed to load 0x" << std::hex << hr << std::endl;
            m_error = true;
            return;
        }

        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        if (auto hr = PrepareUpload(m_device.Get(), image.GetImages(), image.GetImageCount(), image.GetMetadata(), subresources); FAILED(hr))
        {
            std::cout << ddsFile.c_str() << " failed to prepare layout 0x" << std::hex << hr << std::endl;
            m_error = true;
            return;
        }

        auto& desc = t->desc;
        desc.Width = static_cast<UINT>(metadata.width);
        desc.Height = static_cast<UINT>(metadata.height);
        desc.MipLevels = static_cast<UINT16>(metadata.mipLevels);
        desc.DepthOrArraySize = (metadata.dimension == TEX_DIMENSION_TEXTURE3D)
            ? static_cast<UINT16>(metadata.depth)
            : static_cast<UINT16>(metadata.arraySize);
        desc.Format = metadata.format;  
        desc.SampleDesc.Count = 1;
        desc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(metadata.dimension);

        auto subresourceCount = CD3DX12_RESOURCE_DESC(desc).Subresources(m_device.Get());

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
        std::vector<UINT> numRows(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);


        uint64_t totalBytes = 0;

        m_device->GetCopyableFootprints(
            &desc, 
            0, 
            subresourceCount, 
            0, 
            layouts.data(),
            numRows.data(),
            rowSizes.data(), 
            &totalBytes);

        //std::cout << " " << t->path << " " << desc.Width << "x" << desc.Height << " " << totalBytes << " bytes" << std::endl;

        std::vector<char> data(totalBytes);
        t->uncompressedSize = totalBytes;

        for (auto i = 0u; i < subresourceCount; ++i)
        {
            auto const& layout = layouts[i];
            auto const& subresource = subresources[i];

            //std::cout << "   " << i << " " << layout.Offset << std::endl;

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

        auto compressed = Compress(compression, data, t->path);
        t->compressedSize = compressed.size();

        gpuStream.write(compressed.data(), compressed.size());
    }
};

static std::stringstream BuildCpuData(H3DArchiveHeader* archiveHeader, Model const& model, TextureWriter* textures)
{
    std::stringstream s;
    WriteStruct(s, model.header);

    archiveHeader->meshesOffset = s.tellp();
    WriteArray(s, model.meshes.get(), model.header.meshCount);
    std::cout << model.header.meshCount << " meshes" << std::endl;

    archiveHeader->materialsOffset = s.tellp();
    WriteArray(s, model.materials.get(), model.header.materialCount);
    std::cout << model.header.materialCount << " materials" << std::endl;
    
    archiveHeader->archivedTexturesOffset = s.tellp();
    archiveHeader->archivedTexturesCount = textures->GetCount();
    textures->WriteCpuData(s);

    return s;
}

static void ShowUsage(char const* exeName)
{
    std::cout << "Usage: " << exeName << " [-zlib] source.h3d dest.h3da" << std::endl;
}

int main(int argc, char **argv)
{
    bool useZlib = false;
    char const* sourceFilename = nullptr;
    char const* destFilename = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        char const* arg = argv[i];

        if (_strcmpi(arg, "-zlib") == 0)
            useZlib = true;
        else if (!sourceFilename)
            sourceFilename = arg;
        else if (!destFilename)
            destFilename = arg;
    }

    if (!(sourceFilename && destFilename))
    {
        ShowUsage(argv[0]);
        return -1;
    }

    std::cout << "Source: " << sourceFilename << std::endl;
    auto model = TryReadModel(sourceFilename);
    if (!model)
    {
        std::cout << "Unable to read source h3d file" << std::endl;
        return -1;
    }

    std::filesystem::path basePath(sourceFilename);
    basePath.remove_filename();

    FixupMaterialTextures(basePath, &model.value());

    TextureWriter textures(*model);
    if (!textures)
    {
        return -1;
    }

    // Build everything up in memory, ready to be compressed
    H3DArchiveHeader archiveHeader{};
    archiveHeader.magic = GetH3DMagicNumber();
    if (useZlib)
        archiveHeader.compression = H3DCompression::Zlib;

    // CPU data
    std::stringstream cpuDataStream = BuildCpuData(&archiveHeader, *model, &textures);
    if (!textures)
        return -1;

    // Geometry Data
    std::stringstream geometryStream;
    WriteArray(geometryStream, model->geometryData.get(), model->geometryDataSize);
    std::cout << model->geometryDataSize / 1000 << " KiB of geometry data" << std::endl;    

    // Textures
    std::stringstream texturesStream;
    textures.WriteGpuData(texturesStream, basePath, archiveHeader.compression);
    textures.FixupCpuData(cpuDataStream);

    if (!textures)
        return -1;

    std::cout << archiveHeader.archivedTexturesCount << " textures" << std::endl;

    // Write to file
    std::cout << "Dest: " << destFilename << std::endl;

    std::ofstream dest(destFilename, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!dest)
    {
        std::cout << "Unable to open " << destFilename << " for writing" << std::endl;
        return -1;
    }

    // Write a placeholder archive header.  When we've figured everything else out,
    // we'll seek back to the beginning of the file and overwrite this part.
    WriteStruct(dest, archiveHeader);

    // Write the CPU data section
    archiveHeader.cpuDataOffset = dest.tellp();
    archiveHeader.uncompressedCpuDataSize = cpuDataStream.tellp();

    auto compressedCpuData = Compress(archiveHeader.compression, std::move(cpuDataStream), "CPU Data");
    archiveHeader.compressedCpuDataSize = compressedCpuData.size();
    dest.write(compressedCpuData.data(), compressedCpuData.size());
    
    // Write the geometry buffer
    archiveHeader.geometryDataOffset = dest.tellp();
    archiveHeader.uncompressedGeometryDataSize = geometryStream.tellp();

    auto compressedGeometry = Compress(archiveHeader.compression, std::move(geometryStream), "Geometry");
    archiveHeader.compressedGeometryDataSize = compressedGeometry.size();
    dest.write(compressedGeometry.data(), compressedGeometry.size());

    // Write the texture data
    archiveHeader.texturesOffset = dest.tellp();
    texturesStream.seekg(0);
    dest << texturesStream.rdbuf();

    // Fixup
    dest.seekp(0);
    WriteStruct(dest, archiveHeader);


    std::cout << "Save successful" << std::endl;

    return 0;
}
