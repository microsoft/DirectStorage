/**
 * Copyright (C) 2025, by Pavel Martishevsky
 *
 * This header is distributed under the MIT License. See notice at the end of this file.
 */

#pragma once

#ifndef D3D12AID_H
#define D3D12AID_H

#ifdef _MSC_VER
#   define D3D12AID_INLINE __forceinline
#else
#   ifdef __cplusplus
#       define D3D12AID_INLINE inline
#   else
#       define D3D12AID_INLINE
#   endif
#endif

#ifdef __cplusplus
#   define D3D12AID_EXTERN extern "C"
#else
#   define D3D12AID_EXTERN extern
#endif

#ifndef D3D12AID_API
#   ifdef D3D12AID_API_STATIC
#       define D3D12AID_API static D3D12AID_INLINE
#   else
#       define D3D12AID_API D3D12AID_EXTERN
#   endif
#endif

#ifndef D3D12AID_CHECK
#   define D3D12AID_CHECK(call)                             \
        do                                                  \
        {                                                   \
            HRESULT hr = call;                              \
            if (S_OK != hr)                                 \
            {                                               \
                printf("S_OK != 0x%08lx " #call "\n", hr);  \
                if (IsDebuggerPresent())                    \
                    __debugbreak();                         \
            }                                               \
        }                                                   \
        while(0)
#endif

#ifndef D3D12AID_ASSERT
#   define D3D12AID_ASSERT(cond)                                \
        do                                                      \
        {                                                       \
            if (!(cond))                                        \
            {                                                   \
                puts("Assert with condition "#cond" failed.");  \
                if (IsDebuggerPresent())                        \
                    __debugbreak();                             \
            }                                                   \
        }                                                       \
        while(0)
#endif

#ifndef D3D12AID_SAFE_RELEASE
#   define D3D12AID_SAFE_RELEASE(p)     \
        if (NULL != p)                  \
        {                               \
            p->Release();               \
            p = NULL;                   \
        }
#endif

#ifndef D3D12AID_IID_PPV_ARGS
#   if defined(IID_GRAPHICS_PPV_ARGS)
#       define D3D12AID_IID_PPV_ARGS IID_GRAPHICS_PPV_ARGS
#   elif defined(IID_PPV_ARGS)
#       define D3D12AID_IID_PPV_ARGS IID_PPV_ARGS
#   else
#       error It is expected IID_GRAPHICS_PPV_ARGS or IID_PPV_ARGS is defined.
#   endif
#endif

D3D12AID_API ID3D12QueryHeap *d3d12aid_QueryHeap_CreateTimestamps(ID3D12Device *device, uint32_t count)
{
    D3D12_QUERY_HEAP_DESC desc;
    ID3D12QueryHeap *heap = NULL;

    desc.Type      = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    desc.Count     = count;
    desc.NodeMask  = 0x1;

    D3D12AID_CHECK(device->CreateQueryHeap(&desc, D3D12AID_IID_PPV_ARGS(&heap)));
    return heap;
}

D3D12AID_API D3D12_HEAP_PROPERTIES *d3d12aid_HeapProps_InitCustom_WithNodeMask(D3D12_HEAP_PROPERTIES *outProps, D3D12_CPU_PAGE_PROPERTY cpuPageProperty, D3D12_MEMORY_POOL memoryPool, uint32_t creationNodeMask, uint32_t visibleNodeMask)
{
    outProps->Type                  = D3D12_HEAP_TYPE_CUSTOM;
    outProps->CPUPageProperty       = cpuPageProperty;
    outProps->MemoryPoolPreference  = memoryPool;
    outProps->CreationNodeMask      = creationNodeMask;
    outProps->VisibleNodeMask       = visibleNodeMask;
    return outProps;
}

D3D12AID_API D3D12_HEAP_PROPERTIES *d3d12aid_HeapProps_InitCustom(D3D12_HEAP_PROPERTIES *outProps, D3D12_CPU_PAGE_PROPERTY cpuPageProperty, D3D12_MEMORY_POOL memoryPool)
{
    return d3d12aid_HeapProps_InitCustom_WithNodeMask(outProps, cpuPageProperty, memoryPool, 0x1u, 0x1u);
}

D3D12AID_API D3D12_HEAP_PROPERTIES *d3d12aid_HeapProps_InitTyped_WithNodeMask(D3D12_HEAP_PROPERTIES *outProps, D3D12_HEAP_TYPE heapType, uint32_t creationNodeMask, uint32_t visibleNodeMask)
{
    outProps->Type                  = heapType;
    outProps->CPUPageProperty       = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    outProps->MemoryPoolPreference  = D3D12_MEMORY_POOL_UNKNOWN;
    outProps->CreationNodeMask      = creationNodeMask;
    outProps->VisibleNodeMask       = visibleNodeMask;
    return outProps;
}

D3D12AID_API D3D12_HEAP_PROPERTIES *d3d12aid_HeapProps_InitTyped(D3D12_HEAP_PROPERTIES *outProps, D3D12_HEAP_TYPE heapType)
{
    return d3d12aid_HeapProps_InitTyped_WithNodeMask(outProps, heapType, 0x1u, 0x1u);
}

D3D12AID_API ID3D12Heap *d3d12aid_Heap_Create_WithHeapPropsAndFlags(ID3D12Device *device, uint64_t sizeInBytes, uint64_t alignment, const D3D12_HEAP_PROPERTIES *heapProps, D3D12_HEAP_FLAGS heapFlags)
{
    ID3D12Heap *heap = NULL;
    D3D12_HEAP_DESC desc;
    desc.SizeInBytes    = sizeInBytes;
    desc.Properties     = *heapProps;
    desc.Alignment      = alignment;
    desc.Flags          = heapFlags;
    D3D12AID_CHECK(device->CreateHeap(&desc, D3D12AID_IID_PPV_ARGS(&heap)));
    return heap;
}

D3D12AID_API ID3D12Heap *d3d12aid_Heap_Create_WithHeapTypeAndFlags(ID3D12Device *device, uint64_t sizeInBytes, uint64_t alignment, D3D12_HEAP_TYPE heapType, D3D12_HEAP_FLAGS heapFlags)
{
    D3D12_HEAP_PROPERTIES heapProps;
    return d3d12aid_Heap_Create_WithHeapPropsAndFlags(
        device,
        sizeInBytes,
        alignment,
        d3d12aid_HeapProps_InitTyped(&heapProps, heapType),
        heapFlags
    );
}

D3D12AID_API ID3D12Heap *d3d12aid_Heap_Create(ID3D12Device *device, uint64_t sizeInBytes, uint64_t alignment, D3D12_HEAP_TYPE heapType)
{
    return d3d12aid_Heap_Create_WithHeapTypeAndFlags(device, sizeInBytes, alignment, heapType, D3D12_HEAP_FLAG_NONE);
}

D3D12AID_API ID3D12DescriptorHeap *d3d12aid_DescriptorHeap_Create(ID3D12Device *device, uint32_t descCount, D3D12_DESCRIPTOR_HEAP_TYPE heapType, D3D12_DESCRIPTOR_HEAP_FLAGS heapFlags)
{
    ID3D12DescriptorHeap *heap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    desc.Type           = heapType;
    desc.NumDescriptors = descCount;
    desc.Flags          = heapFlags;
    desc.NodeMask       = 0x1u;
    D3D12AID_CHECK(device->CreateDescriptorHeap(&desc, D3D12AID_IID_PPV_ARGS(&heap)));
    return heap;
}

D3D12AID_API D3D12_CPU_DESCRIPTOR_HANDLE d3d12aid_DescriptorHeap_GetCpuStart(ID3D12DescriptorHeap *heap)
{
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetCPUDescriptorHandleForHeapStart();
#else
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    heap->GetCPUDescriptorHandleForHeapStart(&handle);
    return handle;
#endif
}

D3D12AID_API D3D12_GPU_DESCRIPTOR_HANDLE d3d12aid_DescriptorHeap_GetGpuStart(ID3D12DescriptorHeap *heap)
{
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetGPUDescriptorHandleForHeapStart();
#else
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    heap->GetGPUDescriptorHandleForHeapStart(&handle);
    return handle;
#endif
}

D3D12AID_API D3D12_RESOURCE_DESC *d3d12aid_Resource_InitAsBuffer(D3D12_RESOURCE_DESC *outDesc, uint64_t sizeInBytes)
{
    outDesc->Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    outDesc->Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    outDesc->Width              = sizeInBytes;
    outDesc->Height             = 1;
    outDesc->DepthOrArraySize   = 1;
    outDesc->MipLevels          = 1;
    outDesc->Format             = DXGI_FORMAT_UNKNOWN;
    outDesc->SampleDesc.Count   = 1;
    outDesc->SampleDesc.Quality = 0;
    outDesc->Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    outDesc->Flags              = D3D12_RESOURCE_FLAG_NONE;
    return outDesc;
}

D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreateCommitted_Passthrough(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, const D3D12_HEAP_PROPERTIES *heapProps, D3D12_HEAP_FLAGS heapFlags)
{
    ID3D12Resource *resource = NULL;
    /**
     *  NOTE:   according to https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createcommittedresource :
     *          "When you create a resource together with a D3D12_HEAP_TYPE_UPLOAD heap, you must set InitialResourceState to D3D12_RESOURCE_STATE_GENERIC_READ."
     *          "When you create a resource together with a D3D12_HEAP_TYPE_READBACK heap, you must set InitialResourceState to D3D12_RESOURCE_STATE_COPY_DEST."
     *
     *          We also limit the possibility to choose Heap Type to HEAP_TYPE_DEFAULT/HEAP_TYPE_GPU_UPLOAD to prevent the following validation error:
     *              D3D12 ERROR: ID3D12Device::CreateCommittedResource2: A texture resource cannot be created on a D3D12_HEAP_TYPE_UPLOAD
     *              or D3D12_HEAP_TYPE_READBACK heap. Investigate CopyTextureRegion to copy texture data in CPU accessible buffers,
     *              or investigate D3D12_HEAP_TYPE_CUSTOM and WriteToSubresource for UMA adapter optimizations, or investigate
     *              D3D12_HEAP_TYPE_GPU_UPLOAD to create texture resource on CPU accessible heaps.
     *              [ STATE_CREATION ERROR #638: CREATERESOURCEANDHEAP_INVALIDHEAPPROPERTIES]
     */
    D3D12AID_ASSERT((heapProps->Type == D3D12_HEAP_TYPE_UPLOAD && state == D3D12_RESOURCE_STATE_GENERIC_READ && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                 || (heapProps->Type == D3D12_HEAP_TYPE_READBACK && state == D3D12_RESOURCE_STATE_COPY_DEST && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                 || ((heapProps->Type != D3D12_HEAP_TYPE_UPLOAD) && (heapProps->Type != D3D12_HEAP_TYPE_READBACK)));
    /**
     *  NOTE:   The below check make sure certain heap flags aren't set to prevent the following debug validation layer error:
     *
     *          ERROR: ID3D12Device::CreateCommittedResource: When creating a committed resource,
     *          D3D12_HEAP_FLAGS must not include
     *              D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES,
     *              D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES, or
     *              D3D12_HEAP_FLAG_DENY_BUFFERS.
     *          These flags will be set automatically to correspond with the committed resource type.
     *          [ STATE_CREATION_ERROR #640: CREATERESOURCEANDHEAP_INVALIDHEAPMISCFLAGS]
     */
    D3D12AID_ASSERT(0 == (heapFlags & (D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_BUFFERS)));
    /**
     *  NOTE:   Below we checks "clear" value is specified only for RT/DS resources.
     */
    D3D12AID_ASSERT((NULL == clear) == (0 == (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))));
    D3D12AID_CHECK(device->CreateCommittedResource(heapProps, heapFlags, desc, state, clear, D3D12AID_IID_PPV_ARGS(&resource)));
    return resource;
}
/**
 *  NOTE:   The most generic helper for committed resource creation, could use it like this:
 *
 *          d3d12aid_Resource_CreateCommitted_WithStateHeapPropsAndFlags(device,
 *              d3d12aid_Resource_InitAs{}(&resourceDesc, ...),
 *              initialState,
 *              d3d12aid_HeapProps_Init{Typed,Custom}{_WithNodeMask}(&heapPropsDesc, ...),
 *              heapFlags
 *          );
 */
D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreateCommitted_WithStateHeapPropsAndFlags(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_HEAP_PROPERTIES *heapProps, D3D12_HEAP_FLAGS heapFlags)
{
    /**
     *  NOTE:   this checks only non-RT and non-DS textures are created through this function because RT/DS textures require initial clear value.
     */
    D3D12AID_ASSERT(0 == (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)));
    return d3d12aid_Resource_CreateCommitted_Passthrough(device, desc, state, NULL, heapProps, heapFlags);
}

D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreateCommitted_WithStateHeapTypeAndFlags(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, D3D12_HEAP_TYPE heapType, D3D12_HEAP_FLAGS heapFlags)
{
    D3D12_HEAP_PROPERTIES heapProps;
    return d3d12aid_Resource_CreateCommitted_WithStateHeapPropsAndFlags(device, desc, state, d3d12aid_HeapProps_InitTyped(&heapProps, heapType), heapFlags);
}

D3D12AID_API ID3D12Resource *d3d12aid_Buffer_CreateCommitted_Upload_WithHeapFlags(ID3D12Device *device, uint64_t sizeInBytes, D3D12_HEAP_FLAGS heapFlags)
{
    D3D12_RESOURCE_DESC desc;
    return d3d12aid_Resource_CreateCommitted_WithStateHeapTypeAndFlags(device, d3d12aid_Resource_InitAsBuffer(&desc, sizeInBytes), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD, heapFlags);
}

D3D12AID_API ID3D12Resource *d3d12aid_Buffer_CreateCommitted_Readback_WithHeapFlags(ID3D12Device *device, uint64_t sizeInBytes, D3D12_HEAP_FLAGS heapFlags)
{
    D3D12_RESOURCE_DESC desc;
    return d3d12aid_Resource_CreateCommitted_WithStateHeapTypeAndFlags(device, d3d12aid_Resource_InitAsBuffer(&desc, sizeInBytes), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK, heapFlags);
}

D3D12AID_API ID3D12Resource *d3d12aid_Buffer_CreateCommitted_Upload(ID3D12Device *device, uint64_t sizeInBytes)
{
    return d3d12aid_Buffer_CreateCommitted_Upload_WithHeapFlags(device, sizeInBytes, D3D12_HEAP_FLAG_NONE);
}

D3D12AID_API ID3D12Resource *d3d12aid_Buffer_CreateCommitted_Readback(ID3D12Device *device, uint64_t sizeInBytes)
{
    return d3d12aid_Buffer_CreateCommitted_Readback_WithHeapFlags(device, sizeInBytes, D3D12_HEAP_FLAG_NONE);
}

D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreateCommitted_WithStateAndHeapFlags(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, D3D12_HEAP_FLAGS heapFlags)
{
    return d3d12aid_Resource_CreateCommitted_WithStateHeapTypeAndFlags(device,desc, state, D3D12_HEAP_TYPE_DEFAULT, heapFlags);
}

D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreateCommitted_WithState(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state)
{
    return d3d12aid_Resource_CreateCommitted_WithStateHeapTypeAndFlags(device,desc, state, D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_NONE);
}

D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreateCommitted_WithHeapTypeAndFlags(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_HEAP_TYPE heapType, D3D12_HEAP_FLAGS heapFlags)
{
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    else if (heapType == D3D12_HEAP_TYPE_READBACK)
    {
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    return d3d12aid_Resource_CreateCommitted_WithStateHeapTypeAndFlags(device, desc, state, heapType, heapFlags);
}

D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreateCommitted_WithHeapType(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc, D3D12_HEAP_TYPE heapType)
{
    return d3d12aid_Resource_CreateCommitted_WithHeapTypeAndFlags(device, desc, heapType, D3D12_HEAP_FLAG_NONE);
}

D3D12AID_API ID3D12Resource *d3d12aid_Resource_CreatePlaced(ID3D12Device *device, ID3D12Heap *heap, uint64_t heapOffset, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initState)
{
    ID3D12Resource *resource = NULL;
    D3D12AID_CHECK(device->CreatePlacedResource(heap, heapOffset, desc, initState, NULL, D3D12AID_IID_PPV_ARGS(&resource)));
    return resource;
}

D3D12AID_API void *d3d12aid_Resource_MapSubresourceReadRange(ID3D12Resource *resource, uint32_t subresourceIdx, size_t offsetInBytes, size_t sizeInBytes)
{
    D3D12_RANGE range;
    void *mappedMemory = NULL;

    range.Begin = offsetInBytes;
    range.End   = offsetInBytes + sizeInBytes;

    D3D12AID_CHECK(resource->Map(subresourceIdx, &range, &mappedMemory));
    return mappedMemory;
}

D3D12AID_API void *d3d12aid_Resource_MapSubresourceRead(ID3D12Resource *resource, uint32_t subresourceIdx)
{
    void *mappedMemory = NULL;

    /**
     *  https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map
     *  "This indicates the region the CPU might read, and the coordinates are subresource-relative.
     *   A null pointer indicates the entire subresource might be read by the CPU."
     */
    D3D12AID_CHECK(resource->Map(subresourceIdx, NULL, &mappedMemory));
    return mappedMemory;
}

D3D12AID_API void *d3d12aid_Resource_MapRead(ID3D12Resource *resource)
{
    return d3d12aid_Resource_MapSubresourceRead(resource, 0);
}

D3D12AID_API void *d3d12aid_Resource_MapSubresourceWrite(ID3D12Resource *resource, uint32_t subresourceIdx)
{
    D3D12_RANGE range;
    void *mappedMemory = NULL;

    /**
     *  https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map
     *  "It is valid to specify the CPU won't read any data by passing a range where End is less than or equal to Begin."
     */
    range.Begin = 0;
    range.End   = 0;

    D3D12AID_CHECK(resource->Map(subresourceIdx, &range, &mappedMemory));
    return mappedMemory;
}

D3D12AID_API void *d3d12aid_Resource_MapWrite(ID3D12Resource *resource)
{
    return d3d12aid_Resource_MapSubresourceWrite(resource, 0);
}

D3D12AID_API void d3d12aid_Resource_TransitionBarrier(D3D12_RESOURCE_BARRIER *outBarrier, ID3D12Resource *resource, D3D12_RESOURCE_STATES prevState, D3D12_RESOURCE_STATES nextState)
{
    outBarrier->Type                    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    outBarrier->Flags                   = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    outBarrier->Transition.pResource    = resource;
    outBarrier->Transition.Subresource  = 0;
    outBarrier->Transition.StateBefore  = prevState;
    outBarrier->Transition.StateAfter   = nextState;
}

D3D12AID_API void d3d12aid_Resource_AliasingBarrier(D3D12_RESOURCE_BARRIER *outBarrier, ID3D12Resource *prevResource, ID3D12Resource *nextResource)
{
    outBarrier->Type                        = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    outBarrier->Flags                       = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    outBarrier->Aliasing.pResourceBefore    = prevResource;
    outBarrier->Aliasing.pResourceAfter     = nextResource;
}

D3D12AID_API void d3d12aid_Resource_UavBarrier(D3D12_RESOURCE_BARRIER *outBarrier, ID3D12Resource *resource)
{
    outBarrier->Type                        = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    outBarrier->Flags                       = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    outBarrier->UAV.pResource               = resource;
}

D3D12AID_API D3D12_SHADER_RESOURCE_VIEW_DESC *d3d12aid_SRV_InitAsInvalidBuffer(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint32_t elemStart, uint32_t elemCount)
{
    outDesc->Format                     = DXGI_FORMAT_UNKNOWN;
    outDesc->ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    outDesc->Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outDesc->Buffer.FirstElement        = elemStart;
    outDesc->Buffer.NumElements         = elemCount;
    outDesc->Buffer.StructureByteStride = 0;
    outDesc->Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;
    return outDesc;
}

D3D12AID_API D3D12_SHADER_RESOURCE_VIEW_DESC *d3d12aid_SRV_InitAsStructBuffer_WithRange(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t structSizeInBytes, uint32_t structStart, uint32_t structCount)
{
    D3D12AID_ASSERT(0 == sizeInBytes % structSizeInBytes);
    d3d12aid_SRV_InitAsInvalidBuffer(outDesc, structStart, structCount);
    outDesc->Buffer.StructureByteStride = structSizeInBytes;
    return outDesc;
}

D3D12AID_API D3D12_SHADER_RESOURCE_VIEW_DESC *d3d12aid_SRV_InitAsStructBuffer(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t structSizeInBytes)
{
    return d3d12aid_SRV_InitAsStructBuffer_WithRange(outDesc, sizeInBytes, structSizeInBytes, 0, (uint32_t)(sizeInBytes / structSizeInBytes));
}

D3D12AID_API D3D12_SHADER_RESOURCE_VIEW_DESC *d3d12aid_SRV_InitAsTypedBuffer_WithRange(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes, DXGI_FORMAT elemFormat, uint32_t elemSizeInBytes, uint32_t elemStart, uint32_t elemCount)
{
    D3D12AID_ASSERT(0 == sizeInBytes % elemSizeInBytes);
    d3d12aid_SRV_InitAsInvalidBuffer(outDesc, elemStart, elemCount);
    outDesc->Format = elemFormat;
    return outDesc;
}

D3D12AID_API D3D12_SHADER_RESOURCE_VIEW_DESC *d3d12aid_SRV_InitAsTypedBuffer(D3D12_SHADER_RESOURCE_VIEW_DESC *outDesc, uint64_t sizeInBytes, DXGI_FORMAT elemFormat, uint32_t elemSizeInBytes)
{
    return d3d12aid_SRV_InitAsTypedBuffer_WithRange(outDesc, sizeInBytes, elemFormat, elemSizeInBytes, 0, (uint32_t)(sizeInBytes / elemSizeInBytes));
}

D3D12AID_API D3D12_UNORDERED_ACCESS_VIEW_DESC *d3d12aid_UAV_InitAsInvalidBuffer(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint32_t elemStart, uint32_t elemCount)
{
    outDesc->Format                     = DXGI_FORMAT_UNKNOWN;
    outDesc->ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
    outDesc->Buffer.FirstElement        = elemStart;
    outDesc->Buffer.NumElements         = elemCount;
    outDesc->Buffer.StructureByteStride = 0;
    outDesc->Buffer.CounterOffsetInBytes= 0;
    outDesc->Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_NONE;
    return outDesc;
}

D3D12AID_API D3D12_UNORDERED_ACCESS_VIEW_DESC *d3d12aid_UAV_InitAsStructBuffer_WithRange(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t structSizeInBytes, uint32_t structStart, uint32_t structCount)
{
    D3D12AID_ASSERT(0 == sizeInBytes % structSizeInBytes);
    d3d12aid_UAV_InitAsInvalidBuffer(outDesc, structStart, structCount);
    outDesc->Buffer.StructureByteStride = structSizeInBytes;
    return outDesc;
}

D3D12AID_API D3D12_UNORDERED_ACCESS_VIEW_DESC *d3d12aid_UAV_InitAsStructBuffer(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes, uint32_t structSizeInBytes)
{
    return d3d12aid_UAV_InitAsStructBuffer_WithRange(outDesc, sizeInBytes, structSizeInBytes, 0, (uint32_t)(sizeInBytes / structSizeInBytes));
}

D3D12AID_API D3D12_UNORDERED_ACCESS_VIEW_DESC *d3d12aid_UAV_InitAsTypedBuffer_WithRange(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes, DXGI_FORMAT elemFormat, uint32_t elemSizeInBytes, uint32_t elemStart, uint32_t elemCount)
{
    D3D12AID_ASSERT(0 == sizeInBytes % elemSizeInBytes);
    d3d12aid_UAV_InitAsInvalidBuffer(outDesc, elemStart, elemCount);
    outDesc->Format = elemFormat;
    return outDesc;
}

D3D12AID_API D3D12_UNORDERED_ACCESS_VIEW_DESC *d3d12aid_UAV_InitAsTypedBuffer(D3D12_UNORDERED_ACCESS_VIEW_DESC *outDesc, uint64_t sizeInBytes, DXGI_FORMAT elemFormat, uint32_t elemSizeInBytes)
{
    return d3d12aid_UAV_InitAsTypedBuffer_WithRange(outDesc, sizeInBytes, elemFormat, elemSizeInBytes, 0, (uint32_t)(sizeInBytes / elemSizeInBytes));
}

D3D12AID_API void d3d12aid_SRV_Create(D3D12_CPU_DESCRIPTOR_HANDLE destHandle, ID3D12Device *device, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc)
{
    device->CreateShaderResourceView(resource, desc, destHandle);
}

D3D12AID_API void d3d12aid_UAV_Create(D3D12_CPU_DESCRIPTOR_HANDLE destHandle, ID3D12Device *device, ID3D12Resource *resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc)
{
    device->CreateUnorderedAccessView(resource, NULL, desc, destHandle);
}


D3D12AID_API ID3D12RootSignature *d3d12aid_RootSignature_Create(ID3D12Device *device, const void *shaderBytecode, size_t shaderBytecodeSizeInBytes)
{
    ID3D12RootSignature *rs = NULL;
    D3D12AID_CHECK(device->CreateRootSignature(0x1, shaderBytecode, shaderBytecodeSizeInBytes, D3D12AID_IID_PPV_ARGS(&rs)));
    return rs;
}

D3D12AID_API ID3D12PipelineState *d3d12aid_PipelineState_CreateCompute(ID3D12Device *device, ID3D12RootSignature *rs, const void *shaderBytecode, size_t shaderBytecodeSizeInBytes)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
    ID3D12PipelineState *ps = NULL;
    desc.pRootSignature                   = rs;
    desc.CS                               = { shaderBytecode, shaderBytecodeSizeInBytes };
    desc.NodeMask                         = 0x1;
    desc.CachedPSO.pCachedBlob            = NULL;
    desc.CachedPSO.CachedBlobSizeInBytes  = 0;
    desc.Flags                            = D3D12_PIPELINE_STATE_FLAG_NONE;
    D3D12AID_CHECK(device->CreateComputePipelineState(&desc, D3D12AID_IID_PPV_ARGS(&ps)));
    return ps;
}

struct d3d12aid_ComputeRsPs
{
    ID3D12RootSignature *rs;
    ID3D12PipelineState *ps;
};

typedef struct d3d12aid_ComputeRsPs d3d12aid_ComputeRsPs;

D3D12AID_API void d3d12aid_ComputeRsPs_Create(d3d12aid_ComputeRsPs *outComputeRsPs, ID3D12Device *device, const void *shaderBytecode, size_t shaderBytecodeSizeInBytes)
{
    outComputeRsPs->rs = d3d12aid_RootSignature_Create(device, shaderBytecode, shaderBytecodeSizeInBytes);
    outComputeRsPs->ps = d3d12aid_PipelineState_CreateCompute(device, outComputeRsPs->rs, shaderBytecode, shaderBytecodeSizeInBytes);
}

D3D12AID_API void d3d12aid_ComputeRsPs_Release(d3d12aid_ComputeRsPs *computeRsPs)
{
    D3D12AID_SAFE_RELEASE(computeRsPs->ps);
    D3D12AID_SAFE_RELEASE(computeRsPs->rs);
}

D3D12AID_API void d3d12aid_ComputeRsPs_Set(const d3d12aid_ComputeRsPs *computeRsPs, ID3D12GraphicsCommandList *cmdList)
{
    cmdList->SetPipelineState(computeRsPs->ps);
    cmdList->SetComputeRootSignature(computeRsPs->rs);
}

struct d3d12aid_Timestamps
{
    ID3D12QueryHeap *heap;
    ID3D12Resource  *readbackBuf;
    const uint64_t  *readbackMem;

    uint32_t         allocatedSlotCount;
    uint32_t         activatedSlotStart;
    uint32_t         activatedSlotCount;

    uint32_t         latencyFrameIndex;
    uint32_t         latencyFrameCount;

    uint32_t         padding;
};

typedef d3d12aid_Timestamps d3d12aid_Timestamps;

D3D12AID_API void d3d12aid_Timestamps_Create(d3d12aid_Timestamps *outTimestamps, ID3D12Device *device, uint32_t perFrameTimestampCount, uint32_t latencyFrameCount)
{
    uint32_t timestampCount = perFrameTimestampCount * latencyFrameCount;

    outTimestamps->heap = d3d12aid_QueryHeap_CreateTimestamps(device, timestampCount);

    /**
     *  NOTE:   we allocate buffer sufficient to read the entire heap
     *          the reason why we don't allocate smaller buffer that could read only per-frame `count` timestamps is
     *          simplificity: we don't want to introduce any synchronisation
     */
    outTimestamps->readbackBuf = d3d12aid_Buffer_CreateCommitted_Readback(device, sizeof(uint64_t) * timestampCount);

    /** NOTE: we map the readback buffer during initialisation and unmap when destroying it. This is because D3D12 allows persistent mapping */
    outTimestamps->readbackMem = (uint64_t *)d3d12aid_Resource_MapRead(outTimestamps->readbackBuf);

    outTimestamps->allocatedSlotCount  = perFrameTimestampCount;
    outTimestamps->activatedSlotStart  = 0;
    outTimestamps->activatedSlotCount  = 0;
    outTimestamps->latencyFrameIndex   = 0;
    outTimestamps->latencyFrameCount   = latencyFrameCount;
}

D3D12AID_API void d3d12aid_Timestamps_Release(d3d12aid_Timestamps *timestamps)
{
    timestamps->readbackBuf->Unmap(0, NULL);

    D3D12AID_SAFE_RELEASE(timestamps->readbackBuf);
    D3D12AID_SAFE_RELEASE(timestamps->heap);
}

D3D12AID_API void d3d12aid_Timestamps_AdvanceFrame(d3d12aid_Timestamps *timestamps, ID3D12GraphicsCommandList *cmdList)
{
    uint32_t queryStart = (timestamps->latencyFrameIndex ++ % timestamps->latencyFrameCount) * timestamps->allocatedSlotCount;

    cmdList->ResolveQueryData(timestamps->heap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart, timestamps->activatedSlotCount, timestamps->readbackBuf, queryStart * sizeof(uint64_t));

    timestamps->activatedSlotStart = (timestamps->latencyFrameIndex % timestamps->latencyFrameCount) * timestamps->allocatedSlotCount;
    timestamps->activatedSlotCount = 0;
}

D3D12AID_API uint32_t d3d12aid_Timestamps_Push(d3d12aid_Timestamps *timestamps, ID3D12GraphicsCommandList *cmdList)
{
    D3D12AID_ASSERT(timestamps->activatedSlotCount < timestamps->allocatedSlotCount);
    cmdList->EndQuery(timestamps->heap, D3D12_QUERY_TYPE_TIMESTAMP, timestamps->activatedSlotStart + timestamps->activatedSlotCount);
    return timestamps->activatedSlotCount ++;
}

D3D12AID_API uint64_t d3d12aid_Timestamps_Get(d3d12aid_Timestamps *timestamps, uint32_t latencyFrameIndex, uint32_t timestampIndex)
{
    D3D12AID_ASSERT(latencyFrameIndex < timestamps->latencyFrameCount);
    D3D12AID_ASSERT(timestampIndex < timestamps->allocatedSlotCount);
    return timestamps->readbackMem[latencyFrameIndex  * timestamps->allocatedSlotCount + timestampIndex];
}

D3D12AID_API uint64_t d3d12aid_Timestamps_GetDelta(d3d12aid_Timestamps *timestamps, uint32_t latencyFrameIndex, uint32_t timestampStartIndex, uint32_t timestampStopIndex)
{
    D3D12AID_ASSERT(latencyFrameIndex < timestamps->latencyFrameCount);
    D3D12AID_ASSERT(timestampStartIndex < timestamps->allocatedSlotCount);
    D3D12AID_ASSERT(timestampStopIndex < timestamps->allocatedSlotCount);
    const uint64_t *data = &timestamps->readbackMem[latencyFrameIndex  * timestamps->allocatedSlotCount];
    return data[timestampStopIndex] - data[timestampStartIndex];
}

D3D12AID_API uint64_t d3d12aid_Timestamps_GetScopeDelta(d3d12aid_Timestamps *timestamps, uint32_t latencyFrameIndex, uint32_t timestampStartIndex)
{
    D3D12AID_ASSERT(latencyFrameIndex < timestamps->latencyFrameCount);
    D3D12AID_ASSERT(timestampStartIndex + 1 < timestamps->allocatedSlotCount);
    const uint64_t *data = &timestamps->readbackMem[latencyFrameIndex  * timestamps->allocatedSlotCount];
    return data[timestampStartIndex + 1] - data[timestampStartIndex];
}

#define d3d12aid_Timestamp_PushScope(outTimestampIndex, timestamps, cmdList, scope) \
    do                                                                              \
    {                                                                               \
        outTimestampIndex = d3d12aid_Timestamps_Push(&timestamps, cmdList);         \
        scope                                                                       \
        d3d12aid_Timestamps_Push(&timestamps, cmdList);                             \
    }                                                                               \
    while (0)

struct d3d12aid_DescriptorStack
{
    ID3D12Device               *device;
    ID3D12DescriptorHeap       *heap;
    ID3D12DescriptorHeap       *heapRead;
    D3D12_DESCRIPTOR_HEAP_TYPE  heapType;
    uint32_t                    descCount;
    uint32_t                    descSize;
    uint32_t                    descUsed;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStartRead;
};

typedef struct d3d12aid_DescriptorStack d3d12aid_DescriptorStack;

D3D12AID_API void d3d12aid_DescriptorStack_Create(d3d12aid_DescriptorStack *outStack, ID3D12Device *device, uint32_t descCount, D3D12_DESCRIPTOR_HEAP_TYPE heapType)
{
    D3D12_DESCRIPTOR_HEAP_FLAGS heapFlags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (heapType <= D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
    {
        heapFlags |= D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    }
    device->AddRef();
    outStack->device        = device;
    outStack->heap          = d3d12aid_DescriptorHeap_Create(device, descCount, heapType, heapFlags);
    outStack->heapRead      = NULL;
    outStack->heapType      = heapType;
    outStack->descCount     = descCount;
    outStack->descSize      = device->GetDescriptorHandleIncrementSize(heapType);
    outStack->descUsed      = 0;
    outStack->cpuStart      = d3d12aid_DescriptorHeap_GetCpuStart(outStack->heap);
    outStack->gpuStart      = { 0 };
    outStack->cpuStartRead  = { 0 };
    if (heapFlags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
    {
        outStack->gpuStart = d3d12aid_DescriptorHeap_GetGpuStart(outStack->heap);
    }
    if (heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        outStack->heapRead      = d3d12aid_DescriptorHeap_Create(device, descCount, heapType, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
        outStack->cpuStartRead  = d3d12aid_DescriptorHeap_GetCpuStart(outStack->heapRead);
    }
}

#ifndef D3D12AID_DESCRIPTOR_STACK_PUSH_CBV_SRV_UAV
#   define D3D12AID_DESCRIPTOR_STACK_PUSH_CBV_SRV_UAV(stack, method, ...)                   \
    do                                                                                      \
    {                                                                                       \
        D3D12AID_ASSERT(stack->heapType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);         \
        D3D12AID_ASSERT(stack->descUsed < stack->descCount);                                \
        const uint32_t descIdx = stack->descUsed ++;                                        \
        const uint32_t descOfs = stack->descSize * descIdx;                                 \
        D3D12_CPU_DESCRIPTOR_HANDLE dstHandle       = { stack->cpuStart.ptr + descOfs };    \
        D3D12_GPU_DESCRIPTOR_HANDLE dstHandleGpu    = { stack->gpuStart.ptr + descOfs };    \
        D3D12_CPU_DESCRIPTOR_HANDLE dstHandleRead   = { stack->cpuStartRead.ptr + descOfs };\
        stack->device->method(__VA_ARGS__, dstHandle);                                      \
        stack->device->method(__VA_ARGS__, dstHandleRead);                                  \
        return dstHandleGpu;                                                                \
    }                                                                                       \
    while (0)
#endif

D3D12AID_API D3D12_GPU_DESCRIPTOR_HANDLE d3d12aid_DescriptorStack_PushUav(d3d12aid_DescriptorStack *inoutStack, ID3D12Resource *resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *view)
{
    D3D12AID_DESCRIPTOR_STACK_PUSH_CBV_SRV_UAV(inoutStack, CreateUnorderedAccessView, resource, NULL, view);
}

D3D12AID_API D3D12_GPU_DESCRIPTOR_HANDLE d3d12aid_DescriptorStack_PushSrv(d3d12aid_DescriptorStack *inoutStack, ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *view)
{
    D3D12AID_DESCRIPTOR_STACK_PUSH_CBV_SRV_UAV(inoutStack, CreateShaderResourceView, resource, view);
}

D3D12AID_API D3D12_GPU_DESCRIPTOR_HANDLE d3d12aid_DescriptorStack_PushCbv(d3d12aid_DescriptorStack *inoutStack, const D3D12_CONSTANT_BUFFER_VIEW_DESC *view)
{
    D3D12AID_DESCRIPTOR_STACK_PUSH_CBV_SRV_UAV(inoutStack, CreateConstantBufferView, view);
}

#undef D3D12AID_DESCRIPTOR_STACK_PUSH_CBV_SRV_UAV

D3D12AID_API void d3d12aid_DescriptorStack_Release(d3d12aid_DescriptorStack *stack)
{
    D3D12AID_SAFE_RELEASE(stack->heapRead);
    D3D12AID_SAFE_RELEASE(stack->heap);
    D3D12AID_SAFE_RELEASE(stack->device);
}

#ifndef D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT
#define D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT 3
#endif

struct d3d12aid_MappedBuffer
{
    void           *bufMem[D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT];
    ID3D12Resource *bufCpu[D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT];
    ID3D12Resource *bufGpu;

    uint32_t        offsInBytes;
    uint32_t        sizeInBytes;

    uint32_t        frameCount;

    D3D12_HEAP_TYPE heapType;
};

typedef struct d3d12aid_MappedBuffer d3d12aid_MappedBuffer;

D3D12AID_API void d3d12aid_MappedBuffer_Create(d3d12aid_MappedBuffer *outBuffer, ID3D12Device *device, uint32_t frameCount, uint32_t sizeInBytes, D3D12_HEAP_TYPE heapType)
{
    D3D12_RESOURCE_DESC desc;
    d3d12aid_Resource_InitAsBuffer(&desc, sizeInBytes);

    D3D12AID_ASSERT(heapType == D3D12_HEAP_TYPE_UPLOAD || heapType == D3D12_HEAP_TYPE_READBACK);
    D3D12AID_ASSERT(frameCount <= D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT);

    for (uint32_t i = 0; i < D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT; ++i)
        outBuffer->bufCpu[i] = NULL;

    for (uint32_t i = 0; i < D3D12AID_MAPPED_BUFFER_LATENCY_FRAME_MAX_COUNT; ++i)
        outBuffer->bufMem[i] = NULL;

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        outBuffer->bufCpu[i] = d3d12aid_Resource_CreateCommitted_WithHeapType(device, &desc, heapType);
    }

    if (heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            outBuffer->bufMem[i] = d3d12aid_Resource_MapWrite(outBuffer->bufCpu[i]);
        }
    }
    else if (heapType == D3D12_HEAP_TYPE_READBACK)
    {
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            outBuffer->bufMem[i] = d3d12aid_Resource_MapRead(outBuffer->bufCpu[i]);
        }
    }

    if (heapType == D3D12_HEAP_TYPE_READBACK)
    {
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    outBuffer->bufGpu = d3d12aid_Resource_CreateCommitted_WithHeapType(device, &desc, D3D12_HEAP_TYPE_DEFAULT);

    outBuffer->offsInBytes  = 0;
    outBuffer->sizeInBytes  = sizeInBytes;
    outBuffer->frameCount   = frameCount;
    outBuffer->heapType     = heapType;
}

D3D12AID_API void d3d12aid_MappedBuffer_Release(d3d12aid_MappedBuffer *inoutBuffer)
{
    for (uint32_t i = 0; i < inoutBuffer->frameCount; ++i)
    {
        inoutBuffer->bufCpu[i]->Unmap(0, NULL);
        inoutBuffer->bufMem[i] = NULL;
    }
    for (uint32_t i = 0; i < inoutBuffer->frameCount; ++i)
    {
        D3D12AID_SAFE_RELEASE(inoutBuffer->bufCpu[i]);
    }
    D3D12AID_SAFE_RELEASE(inoutBuffer->bufGpu);
}

D3D12AID_API void d3d12aid_MappedBuffer_Append(d3d12aid_MappedBuffer *inoutBuffer, uint32_t frameIndex, const void *data, uint32_t sizeInBytes)
{
    if (inoutBuffer->heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        D3D12AID_ASSERT(frameIndex < inoutBuffer->frameCount);
        D3D12AID_ASSERT(frameIndex < inoutBuffer->frameCount);
        D3D12AID_ASSERT(inoutBuffer->sizeInBytes - inoutBuffer->offsInBytes >= sizeInBytes);

        memcpy((char *)inoutBuffer->bufMem[frameIndex] + inoutBuffer->offsInBytes, data, sizeInBytes);
        inoutBuffer->offsInBytes += sizeInBytes;
    }
}

D3D12AID_API void d3d12aid_MappedBuffer_Skip(d3d12aid_MappedBuffer* inoutBuffer, uint32_t frameIndex, uint32_t sizeInBytes)
{
    if (inoutBuffer->heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        D3D12AID_ASSERT(frameIndex < inoutBuffer->frameCount);
        D3D12AID_ASSERT(frameIndex < inoutBuffer->frameCount);
        D3D12AID_ASSERT(inoutBuffer->sizeInBytes - inoutBuffer->offsInBytes <= sizeInBytes);

        memset((char*)inoutBuffer->bufMem[frameIndex] + inoutBuffer->offsInBytes, 0, sizeInBytes);
        inoutBuffer->offsInBytes += sizeInBytes;
    }
}

D3D12AID_API void d3d12aid_MappedBuffer_Transfer(ID3D12GraphicsCommandList *cmdList, d3d12aid_MappedBuffer *inoutBuffer, uint32_t frameIndex)
{
    if (inoutBuffer->heapType == D3D12_HEAP_TYPE_UPLOAD)
    {
        if (inoutBuffer->offsInBytes == inoutBuffer->sizeInBytes)
        {
            cmdList->CopyResource(inoutBuffer->bufGpu, inoutBuffer->bufCpu[frameIndex]);
        }
        else
        {
            cmdList->CopyBufferRegion(inoutBuffer->bufGpu, 0, inoutBuffer->bufCpu[frameIndex], 0, inoutBuffer->offsInBytes);
            inoutBuffer->offsInBytes = 0;
        }
    }
    else if (inoutBuffer->heapType == D3D12_HEAP_TYPE_READBACK)
    {
        cmdList->CopyResource(inoutBuffer->bufCpu[frameIndex], inoutBuffer->bufGpu);
    }
}

D3D12AID_API void d3d12aid_MappedBuffer_BeginTransfer(D3D12_RESOURCE_BARRIER *outBarrier, d3d12aid_MappedBuffer *inoutBuffer, D3D12_RESOURCE_STATES prevState)
{
    D3D12AID_ASSERT(inoutBuffer->heapType == D3D12_HEAP_TYPE_READBACK);
    d3d12aid_Resource_TransitionBarrier(outBarrier, inoutBuffer->bufGpu, prevState, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

D3D12AID_API void d3d12aid_MappedBuffer_EndTransfer(D3D12_RESOURCE_BARRIER *outBarrier, d3d12aid_MappedBuffer *inoutBuffer, D3D12_RESOURCE_STATES nextState)
{
    D3D12AID_ASSERT(inoutBuffer->heapType == D3D12_HEAP_TYPE_UPLOAD);
    d3d12aid_Resource_TransitionBarrier(outBarrier, inoutBuffer->bufGpu, D3D12_RESOURCE_STATE_COPY_DEST, nextState);
}

#ifndef D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT
#define D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT 3
#endif

#ifndef D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME
#define D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME 1
#endif

#ifdef D3D12AID_CMD_QUEUE_ALLOC_MAX_COUNT
#error `D3D12AID_CMD_QUEUE_ALLOC_MAX_COUNT` must not be defined
#endif

#define D3D12AID_CMD_QUEUE_ALLOC_MAX_COUNT (D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT * D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME)

struct d3d12aid_CmdQueue
{
    uint32_t                    cmdAllocCount;
    uint32_t                    cmdListCount;

    uint64_t                    fenceValue;
    HANDLE                      fenceEvent;
    ID3D12Fence                *fence;
    ID3D12CommandQueue         *queue;

    ID3D12CommandAllocator     *cmdAllocs[D3D12AID_CMD_QUEUE_ALLOC_MAX_COUNT];
    uint64_t                    cmdAllocReuseFenceValues[D3D12AID_CMD_QUEUE_ALLOC_MAX_COUNT];

    ID3D12GraphicsCommandList  *cmdLists[D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME];
    uint32_t                    cmdAllocIndices[D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME];

#if (D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME % 2) != 0
    uint32_t                    padding;
#endif
};

typedef struct d3d12aid_CmdQueue d3d12aid_CmdQueue;

D3D12AID_API void d3d12aid_CmdQueue_Create(d3d12aid_CmdQueue *outQueue, ID3D12Device *device, uint32_t frameCount, uint32_t listCountPerFrame, D3D12_COMMAND_LIST_TYPE cmdQueueType)
{
    D3D12AID_ASSERT(frameCount <= D3D12AID_CMD_QUEUE_LATENCY_FRAME_MAX_COUNT);
    D3D12AID_ASSERT(listCountPerFrame <= D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME);
    D3D12_COMMAND_QUEUE_DESC queueDesc;

    queueDesc.Type      = cmdQueueType;
    queueDesc.Priority  = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags     = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask  = 0x1;

    D3D12AID_CHECK(device->CreateCommandQueue(&queueDesc, D3D12AID_IID_PPV_ARGS(&outQueue->queue)));

    outQueue->cmdAllocCount = frameCount * listCountPerFrame;

    outQueue->cmdListCount = listCountPerFrame;

    /** create a fence with `outQueue->fenceValue` value set (originally set to zero, but could be set to anything) */
    outQueue->fenceValue = 0;

    D3D12AID_CHECK(device->CreateFence(outQueue->fenceValue, D3D12_FENCE_FLAG_NONE, D3D12AID_IID_PPV_ARGS(&outQueue->fence)));

    /** query back the completed value and advanced at the same time*/
    outQueue->fenceValue = outQueue->fence->GetCompletedValue();

    /** initially all submit fences are set to the last completed value of the fence */
    for (uint32_t i = 0; i < outQueue->cmdAllocCount; ++i)
        outQueue->cmdAllocReuseFenceValues[i] = outQueue->fenceValue;

    outQueue->fenceValue += 1ull;

    /** intially allocator index matches the index of the command list */
    for (uint32_t i = 0; i < outQueue->cmdListCount; ++i)
        outQueue->cmdAllocIndices[i] = i;

    /** initialise the allocators with the `Type` of the queue they are bound to */
    for (uint32_t i = 0; i < outQueue->cmdAllocCount; ++i)
        D3D12AID_CHECK(device->CreateCommandAllocator(queueDesc.Type, D3D12AID_IID_PPV_ARGS(&outQueue->cmdAllocs[i])));

    for (uint32_t i = outQueue->cmdAllocCount; i < D3D12AID_CMD_QUEUE_ALLOC_MAX_COUNT; ++i)
        outQueue->cmdAllocs[i] = NULL;

    /** initialise the lists with the `Type` and `NodeMask` of the queue they are bound to */
    for (uint32_t i = 0; i < outQueue->cmdListCount; ++i)
    {
        D3D12AID_CHECK(device->CreateCommandList(queueDesc.NodeMask, queueDesc.Type, outQueue->cmdAllocs[i], NULL, D3D12AID_IID_PPV_ARGS(&outQueue->cmdLists[i])));

        outQueue->cmdLists[i]->Close();
    }

    for (uint32_t i = outQueue->cmdListCount; i < D3D12AID_CMD_QUEUE_LIST_MAX_COUNT_PER_FRAME; ++i)
        outQueue->cmdLists[i] = NULL;

    outQueue->fenceEvent = CreateEventW(NULL, 0, 0, L"d3d12aid_CmdQueue_FenceEvent");
}

D3D12AID_API uint64_t d3d12aid_CmdQueue_GpuSignal(d3d12aid_CmdQueue *queue)
{
    const uint64_t submitFenceValue = queue->fenceValue ++;
    D3D12AID_CHECK(queue->queue->Signal(queue->fence, submitFenceValue));
    return submitFenceValue;
}

D3D12AID_API uint64_t d3d12aid_CmdQueue_CpuSignal(d3d12aid_CmdQueue *queue)
{
    const uint64_t submitFenceValue = queue->fenceValue ++;
    D3D12AID_CHECK(queue->fence->Signal(submitFenceValue));
    return submitFenceValue;
}

D3D12AID_API void d3d12aid_CmdQueue_GpuWaitForFence(d3d12aid_CmdQueue *queue, uint64_t fenceId)
{
    D3D12AID_ASSERT(fenceId < queue->fenceValue);
    D3D12AID_CHECK(queue->queue->Wait(queue->fence, fenceId));
}

D3D12AID_API void d3d12aid_CmdQueue_CpuWaitForFence(d3d12aid_CmdQueue *queue, uint64_t fenceId)
{
    D3D12AID_ASSERT(fenceId < queue->fenceValue);
    uint64_t fenceIdCompleted = queue->fence->GetCompletedValue();
    if (fenceId > fenceIdCompleted)
    {
        D3D12AID_CHECK(queue->fence->SetEventOnCompletion(fenceId, queue->fenceEvent));
        WaitForSingleObjectEx(queue->fenceEvent, INFINITE, FALSE);
    }
}

D3D12AID_API void d3d12aid_CmdQueue_CpuWaitForGpuIdle(d3d12aid_CmdQueue *queue)
{
    d3d12aid_CmdQueue_CpuWaitForFence(queue, queue->fenceValue - 1ull);
}

D3D12AID_API void d3d12aid_CmdQueue_Release(d3d12aid_CmdQueue *queue)
{
    d3d12aid_CmdQueue_CpuWaitForGpuIdle(queue);

    for (uint32_t i = 0; i < queue->cmdListCount; ++i)
        D3D12AID_SAFE_RELEASE(queue->cmdLists[i]);

    for (uint32_t i = 0; i < queue->cmdAllocCount; ++i)
        D3D12AID_SAFE_RELEASE(queue->cmdAllocs[i]);

    CloseHandle(queue->fenceEvent);

    D3D12AID_SAFE_RELEASE(queue->fence);
    D3D12AID_SAFE_RELEASE(queue->queue);
}

D3D12AID_API uint64_t d3d12aid_CmdQueue_SubmitMultiCmdLists(d3d12aid_CmdQueue *queue, uint32_t cmdListIdStart, uint32_t cmdListIdCount)
{
    D3D12AID_ASSERT(cmdListIdStart < queue->cmdListCount);
    D3D12AID_ASSERT(cmdListIdStart + cmdListIdCount <= queue->cmdListCount);

    uint64_t *allocReuseFenceValues = queue->cmdAllocReuseFenceValues;
    uint32_t *allocIndices = queue->cmdAllocIndices;

    uint64_t submitFenceValue = 0;

    /** close the command lists we are going to submit */
    for (uint32_t i = cmdListIdStart; i < cmdListIdStart + cmdListIdCount; ++i)
        D3D12AID_CHECK(queue->cmdLists[i]->Close());

    queue->queue->ExecuteCommandLists(cmdListIdCount, (ID3D12CommandList * const *)&queue->cmdLists[cmdListIdStart]);

    submitFenceValue = d3d12aid_CmdQueue_GpuSignal(queue);

    /** for each allocator associated with the submitted command list .. */
    for (uint32_t i = cmdListIdStart; i < cmdListIdStart + cmdListIdCount; ++i)
    {
        const uint32_t allocatorIdx = allocIndices[i];

        /** 1. update its fence value to track when it was submitted */
        allocReuseFenceValues[allocatorIdx] = submitFenceValue;

        /** 2. choose next available allocator index */
        allocIndices[i] = (allocatorIdx + queue->cmdListCount) % queue->cmdAllocCount;
    }
    return submitFenceValue;
}

D3D12AID_API ID3D12GraphicsCommandList **d3d12aid_CmdQueue_StartMultiCmdLists(d3d12aid_CmdQueue *queue, uint32_t cmdListIdStart, uint32_t cmdListIdCount)
{
    D3D12AID_ASSERT(cmdListIdStart < queue->cmdListCount);
    D3D12AID_ASSERT(cmdListIdStart + cmdListIdCount <= queue->cmdListCount);

    const uint64_t *allocReuseFenceValues = queue->cmdAllocReuseFenceValues;
    const uint32_t *allocIndices = queue->cmdAllocIndices;

    uint64_t lastAllocReuseFenceValueToWait = 0;

    /** iterate over all command buffers to start, fetch their allocator indices [0..maxQueueFrameCount-1] and figure the last submitted fence */
    for (uint32_t i = cmdListIdStart; i < cmdListIdStart + cmdListIdCount; ++i)
    {
        const uint32_t allocatorIdx = allocIndices[i];

        if (lastAllocReuseFenceValueToWait < allocReuseFenceValues[allocatorIdx])
            lastAllocReuseFenceValueToWait = allocReuseFenceValues[allocatorIdx];
    }

    d3d12aid_CmdQueue_CpuWaitForFence(queue, lastAllocReuseFenceValueToWait);

    /** iterate over all command buffers to start, fetch their allocator indices [0..maxQueueFrameCount-1], reset corresponding allocators and reuse them for command lists */
    for (uint32_t i = cmdListIdStart; i < cmdListIdStart + cmdListIdCount; ++i)
    {
        const uint32_t allocatorIdx = allocIndices[i];

        D3D12AID_CHECK(queue->cmdAllocs[allocatorIdx]->Reset());
        D3D12AID_CHECK(queue->cmdLists[i]->Reset(queue->cmdAllocs[allocatorIdx], NULL));
    }
    return &queue->cmdLists[cmdListIdStart];
}

D3D12AID_API uint64_t d3d12aid_CmdQueue_SubmitCmdList(d3d12aid_CmdQueue *queue, uint32_t cmdListId)
{
    return d3d12aid_CmdQueue_SubmitMultiCmdLists(queue, cmdListId, 1u);
}

D3D12AID_API ID3D12GraphicsCommandList *d3d12aid_CmdQueue_StartCmdList(d3d12aid_CmdQueue *queue, uint32_t cmdListId)
{
    return d3d12aid_CmdQueue_StartMultiCmdLists(queue, cmdListId, 1)[0];
}

D3D12AID_API bool d3d12aid_CmdQueue_IsFenceCompleted(d3d12aid_CmdQueue *queue, uint64_t fenceId)
{
    return fenceId <= queue->fence->GetCompletedValue();

}

#endif /** D3D12AID_H */

/**
 * Copyright (c) 2025 Pavel Martishevsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
