//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#pragma once

#include "../Core/Math/Common.h"
#include "../Model/ModelLoader.h"

//
// This describes the on-disk format of a .marc file.
//
// Broadly, the file is laid out as follows:
//
// Header
// Textures
// Unstructured GPU Data
// CPU Data
//

namespace marc
{
    using Math::Matrix4;
    using Renderer::MaterialConstantData;

    constexpr uint16_t CURRENT_MARC_FILE_VERSION = 1u;

    //
    // Supported compression formats.  See Region.
    //
    enum class Compression : uint16_t
    {
        None = 0,
        GDeflate = 1,
        Zlib = 2,
    };

    //
    // A pointer/offset.  On disk, this is an offset relative to the containing
    // region (or the start of the file if this Ptr is stored in the header.)
    // After the data has been loaded, the offsets are fixed up and converted
    // into typed pointers.
    //
    template<typename T>
    union Ptr
    {
        uint32_t Offset;
        T* Ptr;
    };

    //
    // An array - stored as a Ptr, with overloaded array access operators.
    //
    template<typename T>
    struct Array
    {
        Ptr<T> Data;

        T& operator[] (size_t index)
        {
            return Data.Ptr[index];
        }

        T const& operator[] (size_t index) const
        {
            return Data.Ptr[index];
        }
    };

    //
    // A 'Region' describes a part of the file that can be read with a single
    // DirectStorage request.  Each region can choose its own compression
    // format.
    //    
    template<typename T>
    struct Region
    {
        Compression Compression;
        Ptr<T> Data; // (on disk this is compressed, in memory it is uncompressed)
        uint32_t CompressedSize;
        uint32_t UncompressedSize;
    };

    // A Region that's loaded into GPU memory (and therefore cannot have a typed
    // pointer)
    using GpuRegion = Region<void>;

    //
    // The Header is the fixed sized part of the file that contains references
    // to the three main regions.
    //
    struct Header
    {
        char Id[4];       // "MARC"
        uint16_t Version; // CURRENT_MARC_FILE_VERSION

        // The unstructured GPU data is read entirely into a D3D12 buffer
        // resource.
        GpuRegion UnstructuredGpuData;
        Region<struct CpuMetadataHeader> CpuMetadata;
        Region<struct CpuDataHeader> CpuData;

        float BoundingSphere[4];
        float MinPos[3];
        float MaxPos[3];
    };

    //
    // Metadata about each texture, provides the information required in order
    // to load each texture.
    //
    // Each region is loaded by a single request.  Since DirectStorage requests
    // cannot be greated than the staging buffer size, care must be taken to
    // ensure that no regions are larger than this.  Sometimes the entire MIP
    // chain will not fit in the staging buffer, and so each texture is stored
    // as a number of single MIPs, and then the remaining MIPs.
    //
    struct TextureMetadata
    {
        // The name of the file the texture was generated from.
        Ptr<char> Name;
        uint32_t NumSingleMips;
        Array<GpuRegion> SingleMips;
        GpuRegion RemainingMips;
    };

    //
    // The CPU metadata stores all the information required to load the rest of
    // the data.  The metadata can be persisted between content loading.
    //
    struct CpuMetadataHeader
    {
        uint32_t NumTextures;
        Array<TextureMetadata> Textures;
        Array<D3D12_RESOURCE_DESC> TextureDescs;

        uint32_t NumMaterials;
    };

    //
    // Information about a material. This is a version of
    // Renderer::MaterialTextureData from MiniEngine's Model/ModelLoader.h that
    // uses indices to textures rather than filenames.
    //
    struct Material
    {
        uint16_t TextureIndex[kNumTextures];
        uint32_t AddressModes;
    };

    //
    // CPU content; as this needs to be fixed up to refer to final GPU locations
    // (ie indices into descriptor heaps) this data must be refreshed each time
    // the content is loaded.
    //
    // The fields in here all correspond to fields in the Model class in
    // Model/Model.h.
    //
    struct CpuDataHeader
    {
        uint32_t NumSceneGraphNodes;
        Array<GraphNode> SceneGraph;

        uint32_t NumMeshes;
        Ptr<uint8_t> Meshes;

        uint32_t MaterialConstantsGpuOffset;
        Array<Material> Materials;

        uint32_t NumAnimations;
        Array<AnimationSet> Animations;

        uint32_t NumAnimationCurves;
        Array<AnimationCurve> AnimationCurves;

        Ptr<uint8_t> KeyFrameData;

        uint32_t NumJoints;
        Array<uint16_t> JointIndices;
        Array<Matrix4> JointIBMs;
    };
} // namespace marc