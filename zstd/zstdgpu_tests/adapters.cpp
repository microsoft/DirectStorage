//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "adapters.h"

#include <dxcore.h>
#include <windows.h>
#include <winrt/base.h>

#include <vector>

using winrt::check_hresult;
using winrt::com_ptr;

std::string GetAdapterPropertyAsString(IDXCoreAdapter* adapter, DXCoreAdapterProperty property)
{
    std::string value;
    size_t valueByteSize = 0;
    check_hresult(adapter->GetPropertySize(property, &valueByteSize));
    value.resize(valueByteSize - 1); // exclude null terminator size
    check_hresult(adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, valueByteSize, value.data()));
    return value;
}

std::vector<AdapterDesc> EnumerateAdapters()
{
    std::vector<AdapterDesc> adapterDescriptions;

    com_ptr<IDXCoreAdapterFactory> adapterFactory;
    check_hresult(::DXCoreCreateAdapterFactory(adapterFactory.put()));

    com_ptr<IDXCoreAdapterList> d3D12CoreComputeAdapters;
    GUID attributes[]{DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE};
    check_hresult(adapterFactory->CreateAdapterList(_countof(attributes), attributes, d3D12CoreComputeAdapters.put()));

    com_ptr<IDXCoreAdapter> preferredAdapter;
    const uint32_t count{d3D12CoreComputeAdapters->GetAdapterCount()};
    for (uint32_t i = 0; i < count; ++i)
    {
        com_ptr<IDXCoreAdapter> candidateAdapter;
        check_hresult(d3D12CoreComputeAdapters->GetAdapter(i, candidateAdapter.put()));

        AdapterDesc desc{};
        check_hresult(candidateAdapter->GetProperty(
            DXCoreAdapterProperty::InstanceLuid,
            sizeof(desc.InstanceLuid),
            &desc.InstanceLuid));

        check_hresult(candidateAdapter->GetProperty(DXCoreAdapterProperty::IsHardware, &desc.IsHardware));

        check_hresult(
            candidateAdapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, &desc.DedicatedAdapterMemory));

        check_hresult(
            candidateAdapter->GetProperty(DXCoreAdapterProperty::DedicatedSystemMemory, &desc.DedicatedSystemMemory));

        check_hresult(
            candidateAdapter->GetProperty(DXCoreAdapterProperty::SharedSystemMemory, &desc.SharedSystemMemory));

        check_hresult(candidateAdapter->GetProperty(DXCoreAdapterProperty::IsIntegrated, &desc.IsIntegrated));

        check_hresult(candidateAdapter->GetProperty(DXCoreAdapterProperty::IsDetachable, &desc.IsDetachable));

        desc.Name = GetAdapterPropertyAsString(candidateAdapter.get(), DXCoreAdapterProperty::DriverDescription);

        com_ptr<ID3D12Device5> device;
        check_hresult(D3D12CreateDevice(candidateAdapter.get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(device.put())));
        desc.Device = device;

        adapterDescriptions.push_back(desc);
    }

    return adapterDescriptions;
}

std::optional<AdapterDesc> FindAdapterByName(std::string const& name)
{
    auto adapters = EnumerateAdapters();
    for (auto const& adapter : adapters)
    {
        if (_strcmpi(adapter.Name.c_str(), name.c_str()) == 0)
        {
            return adapter;
        }
    }
    return std::nullopt;
}

std::optional<AdapterDesc> FindAdapterByLUID(LUID luid)
{
    auto adapters = EnumerateAdapters();
    for (auto const& adapter : adapters)
    {
        if (adapter.InstanceLuid.LowPart == luid.LowPart && adapter.InstanceLuid.HighPart == luid.HighPart)
        {
            return adapter;
        }
    }
    return std::nullopt;
}

AdapterDesc FindDefaultAdapter()
{
    auto adapters = EnumerateAdapters();
    // This returns the first adapter found, which is typically the high-performance GPU on systems with multiple GPUs.
    // Order is determined by DXCore enumeration.
    return adapters[0];
}