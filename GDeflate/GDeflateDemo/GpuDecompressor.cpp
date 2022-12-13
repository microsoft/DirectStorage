/*
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "GpuDecompressor.h"

#include "CompressedFile.h"

GpuDecompressor::GpuDecompressor(ID3D12Device* device, DeviceInfo deviceInfo, std::filesystem::path const& shaderPath)
    : m_nextFenceValue(1)
    , m_dispatchSize((deviceInfo.SIMDLaneCount / deviceInfo.SIMDWidth) * 8)
{
    m_device.copy_from(device);
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    winrt::check_hresult(device->CreateCommandQueue(&desc, IID_PPV_ARGS(m_commandQueue.put())));

    winrt::check_hresult(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(m_commandAllocator.put())));

    winrt::check_hresult(
        device->CreateCommandList(0, desc.Type, m_commandAllocator.get(), nullptr, IID_PPV_ARGS(m_commandList.put())));

    winrt::check_hresult(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())));

    m_fenceEvent.reset(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    auto byteCode = CompileShader(shaderPath, deviceInfo);
    std::cout << "Shader compiled successfully, bytecode size = " << byteCode.size() << " bytes\n";

    m_rootSignature = CreateRootSignature(device);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = m_rootSignature.get();
    pipelineDesc.CS.pShaderBytecode = byteCode.data();
    pipelineDesc.CS.BytecodeLength = byteCode.size();
    winrt::check_hresult(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(m_pipelineState.put())));

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.NumDescriptors = 1;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    winrt::check_hresult(m_device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(m_gpuVisibleDescHeap.put())));

    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    winrt::check_hresult(m_device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(m_cpuVisibleDescHeap.put())));
}

BufferVector GpuDecompressor::Decompress(BufferVector const& compressedData)
{
    std::vector<Stream> streams;
    streams.reserve(compressedData.size());

    uint64_t inputBufferSize = 0;
    uint64_t outputBufferSize = 0;

    // Construct stream entries for the control buffer from all of the compressed data
    Stream stream{};
    for (size_t s = 0; s < compressedData.size(); ++s)
    {
        streams.push_back(stream);
        CompressedFileHeader const* header = reinterpret_cast<CompressedFileHeader const*>(compressedData[s].data());
        uint32_t compressedSize = static_cast<uint32_t>(compressedData[s].size() - sizeof(CompressedFileHeader));
        uint32_t uncompressedSize = static_cast<uint32_t>(header->UncompressedSize);

        inputBufferSize = stream.InputOffset + compressedSize;
        outputBufferSize = stream.OutputOffset + uncompressedSize;

        stream.InputOffset = DWORD_ALIGN(stream.InputOffset + compressedSize);
        stream.OutputOffset = DWORD_ALIGN(stream.OutputOffset + uncompressedSize);
    }

    uint64_t controlBufferSize = CalculateControlBufferSize(streams.size());
    uint64_t scratchBufferSize = GetRequiredScratchBufferSize(static_cast<uint16_t>(streams.size()));
    uint64_t uploadBufferSize = controlBufferSize + inputBufferSize;

    std::cout << "GPU decompression buffer sizes\n";
    std::cout << "Input Buffer:   " << inputBufferSize << " bytes\n";
    std::cout << "Control Buffer: " << controlBufferSize << " bytes\n";
    std::cout << "Scratch Buffer: " << scratchBufferSize << " bytes\n";
    std::cout << "Output Buffer:  " << outputBufferSize << " bytes\n\n";

    m_buffers = CreateBuffers(
        m_device.get(),
        inputBufferSize,
        outputBufferSize,
        controlBufferSize,
        uploadBufferSize,
        scratchBufferSize);

    // Copy compressed data into upload buffer
    uint8_t* uploadBuffer = nullptr;
    winrt::check_hresult(m_buffers.UploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&uploadBuffer)));
    for (size_t s = 0; s < streams.size(); ++s)
    {
        auto& stream = streams[s];
        uint32_t compressedSize = static_cast<uint32_t>(compressedData[s].size() - sizeof(CompressedFileHeader));
        memcpy(
            uploadBuffer + stream.InputOffset,
            compressedData[s].data() + sizeof(CompressedFileHeader),
            compressedSize);
    }

    // Copy control buffer into upload buffer
    uint32_t* controlData = reinterpret_cast<uint32_t*>(uploadBuffer + inputBufferSize);
    *controlData = static_cast<uint32_t>(streams.size());
    memcpy(controlData + 1, streams.data(), streams.size() * sizeof(Stream));

    m_buffers.UploadBuffer->Unmap(0, nullptr);

    // Copy into input buffer
    m_commandList->CopyBufferRegion(m_buffers.InputBuffer.get(), 0, m_buffers.UploadBuffer.get(), 0, inputBufferSize);

    // Copy into control buffer
    m_commandList->CopyBufferRegion(
        m_buffers.ControlBuffer.get(),
        0,
        m_buffers.UploadBuffer.get(),
        inputBufferSize,
        controlBufferSize);

    // Transition InputBuffer from D3D12_RESOURCE_STATE_COPY_DEST after the CopyBufferRegion
    // to D3D12_RESOURCE_STATE_COMMON and transition ControlBuffer from 
    // D3D12_RESOURCE_STATE_COPY_DEST after the CopyBufferRegion to 
    // D3D12_RESOURCE_STATE_UNORDERED_ACCESS.
    D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(
            m_buffers.InputBuffer.get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(
            m_buffers.ControlBuffer.get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    m_commandList->ResourceBarrier(_countof(barriers), barriers);

    // Clear scratch buffer
    ClearScratchBuffer(scratchBufferSize);

    // Decompress input buffer to output buffer
    m_commandList->SetComputeRootSignature(m_rootSignature.get());
    m_commandList->SetPipelineState(m_pipelineState.get());
    m_commandList->SetComputeRootShaderResourceView(RootSRVInput, m_buffers.InputBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootUnorderedAccessView(RootUAVOutput, m_buffers.OutputBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootUnorderedAccessView(RootUAVControl, m_buffers.ControlBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootUnorderedAccessView(RootUAVScratch, m_buffers.ScratchBuffer->GetGPUVirtualAddress());
    m_commandList->Dispatch(m_dispatchSize, 1, 1);
    ExecuteCommandListSynchronously();

    // Readback the decompressed data from the output buffer to return
    winrt::com_ptr<ID3D12Resource> readbackBuffer;
    auto bufferHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(outputBufferSize, D3D12_RESOURCE_FLAG_NONE);
    winrt::check_hresult(m_device->CreateCommittedResource(
        &bufferHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(readbackBuffer.put())));

    m_commandList->CopyBufferRegion(readbackBuffer.get(), 0, m_buffers.OutputBuffer.get(), 0, outputBufferSize);
    ExecuteCommandListSynchronously();

    BufferVector uncompressedData;

    // Read the output buffer data and reconstruct the original uncompressed content
    uint8_t* outputBufferData = nullptr;
    winrt::check_hresult(readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&outputBufferData)));

    for (size_t s = 0; s < streams.size(); ++s)
    {
        auto& stream = streams[s];
        CompressedFileHeader const* header = reinterpret_cast<CompressedFileHeader const*>(compressedData[s].data());
        std::vector<uint8_t> buffer(header->UncompressedSize);

        memcpy(buffer.data(), outputBufferData + stream.OutputOffset, header->UncompressedSize);
        uncompressedData.push_back(std::move(buffer));
    }
    readbackBuffer->Unmap(0, nullptr);

    return uncompressedData;
}

std::unique_ptr<GpuDecompressor> GpuDecompressor::Create(
    ID3D12Device* device,
    DeviceInfo deviceInfo,
    std::filesystem::path const& shaderPath)
{
    return std::make_unique<GpuDecompressor>(device, deviceInfo, shaderPath);
}

void GpuDecompressor::ExecuteCommandListSynchronously()
{
    m_commandList->Close();
    ID3D12CommandList* commandLists[] = {m_commandList.get()};
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    m_commandQueue->Signal(m_fence.get(), m_nextFenceValue);
    m_fence->SetEventOnCompletion(m_nextFenceValue, m_fenceEvent.get());

    ++m_nextFenceValue;
    m_fenceEvent.wait();

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.get(), nullptr);
}

void GpuDecompressor::ClearScratchBuffer(uint64_t scratchBufferSize)
{
    ID3D12DescriptorHeap* heaps[] = {m_gpuVisibleDescHeap.get()};
    m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R32_UINT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = static_cast<uint32_t>(static_cast<uint32_t>(scratchBufferSize) / sizeof(uint32_t));

    m_device->CreateUnorderedAccessView(
        m_buffers.ScratchBuffer.get(),
        nullptr,
        &uavDesc,
        m_gpuVisibleDescHeap->GetCPUDescriptorHandleForHeapStart());

    m_device->CreateUnorderedAccessView(
        m_buffers.ScratchBuffer.get(),
        nullptr,
        &uavDesc,
        m_cpuVisibleDescHeap->GetCPUDescriptorHandleForHeapStart());

    uint32_t values[4]{0, 0, 0, 0};
    m_commandList->ClearUnorderedAccessViewUint(
        m_gpuVisibleDescHeap->GetGPUDescriptorHandleForHeapStart(),
        m_cpuVisibleDescHeap->GetCPUDescriptorHandleForHeapStart(),
        m_buffers.ScratchBuffer.get(),
        values,
        0,
        nullptr);
}

uint64_t GpuDecompressor::GetRequiredScratchBufferSize(uint16_t numStreams)
{
    return sizeof(uint32_t) * numStreams;
}

size_t GpuDecompressor::CalculateControlBufferSize(size_t numStreams)
{
    // [Total Streams] + [Stream Entry] + [Stream Entry]...
    return sizeof(uint32_t) + (numStreams * sizeof(Stream));
}

winrt::com_ptr<ID3D12Resource> GpuDecompressor::CreateBuffer(
    ID3D12Device* device,
    uint64_t size,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_FLAGS flags)
{
    winrt::com_ptr<ID3D12Resource> buffer;
    auto bufferHeapProps = CD3DX12_HEAP_PROPERTIES(heapType);
    auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
    winrt::check_hresult(device->CreateCommittedResource(
        &bufferHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(buffer.put())));
    return buffer;
}

GpuDecompressor::Buffers GpuDecompressor::CreateBuffers(
    ID3D12Device* device,
    uint64_t inputBufferSize,
    uint64_t outputBufferSize,
    uint64_t controlBufferSize,
    uint64_t uploadBufferSize,
    uint64_t scratchBufferSize)
{
    Buffers buffers{};

    buffers.InputBuffer = CreateBuffer(
        device,
        inputBufferSize,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    winrt::check_hresult(buffers.InputBuffer->SetName(L"Input Buffer"));

    buffers.OutputBuffer = CreateBuffer(
        device,
        outputBufferSize,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    winrt::check_hresult(buffers.OutputBuffer->SetName(L"Ouput Buffer"));

    buffers.ControlBuffer = CreateBuffer(
        device,
        controlBufferSize,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    winrt::check_hresult(buffers.ControlBuffer->SetName(L"Control Buffer"));

    buffers.UploadBuffer = CreateBuffer(
        device,
        uploadBufferSize,
        D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_FLAG_NONE);

    winrt::check_hresult(buffers.UploadBuffer->SetName(L"Upload Buffer"));

    buffers.ScratchBuffer = CreateBuffer(
        device,
        scratchBufferSize,
        D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    winrt::check_hresult(buffers.ScratchBuffer->SetName(L"Scratch Buffer"));

    return buffers;
}

winrt::com_ptr<ID3D12RootSignature> GpuDecompressor::CreateRootSignature(ID3D12Device* device)
{
    winrt::com_ptr<ID3D12RootSignature> rootSignature;
    std::vector<CD3DX12_ROOT_PARAMETER1> rootParameters(RootParametersCount);

    rootParameters[RootSRVInput].InitAsShaderResourceView(0);
    rootParameters[RootSRVCryptoCtx].InitAsShaderResourceView(1);
    rootParameters[RootUAVControl].InitAsUnorderedAccessView(0);
    rootParameters[RootUAVOutput].InitAsUnorderedAccessView(1);
    rootParameters[RootUAVScratch].InitAsUnorderedAccessView(2);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
    computeRootSignatureDesc.Init_1_1(static_cast<uint32_t>(rootParameters.size()), rootParameters.data(), 0, nullptr);

    winrt::com_ptr<ID3DBlob> signature;
    winrt::check_hresult(D3DX12SerializeVersionedRootSignature(
        &computeRootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1_1,
        signature.put(),
        nullptr));

    winrt::check_hresult(device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(rootSignature.put())));

    return rootSignature;
}

std::vector<uint8_t> GpuDecompressor::CompileShader(std::filesystem::path const& shaderPath, DeviceInfo const& info)
{
    std::vector<uint8_t> byteCode;

    // Build compiler arguments based on device's supported features
    std::vector<std::wstring> arguments;
    arguments.push_back(L"-O3");
    arguments.push_back(L"-WX");
    arguments.push_back(L"-Zi");

    if (info.SupportsWaveIntrinsics)
    {
        arguments.push_back(L"-DUSE_WAVE_INTRINSICS");
    }

    if (info.SupportsWaveMatch)
    {
        arguments.push_back(L"-DUSE_WAVE_MATCH");
    }

    if (info.Supports16BitTypes)
    {
        arguments.push_back(L"-enable-16bit-types");
    }

    std::wstringstream simdArg;
    simdArg << L"-DSIMD_WIDTH=" << info.SIMDWidth;
    arguments.push_back(simdArg.str());

    winrt::com_ptr<IDxcLibrary> library;
    winrt::check_hresult(DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(library.put())));

    winrt::com_ptr<IDxcCompiler> compiler;
    winrt::check_hresult(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())));

    winrt::com_ptr<IDxcIncludeHandler> includeHandler;
    winrt::check_hresult(library->CreateIncludeHandler(includeHandler.put()));

    winrt::com_ptr<IDxcBlobEncoding> sourceBlob;
    uint32_t codePage = CP_UTF8;
    winrt::check_hresult(library->CreateBlobFromFile(shaderPath.wstring().c_str(), &codePage, sourceBlob.put()));

    std::vector<wchar_t const*> pargs;
    std::transform(arguments.begin(), arguments.end(), back_inserter(pargs), [](auto const& a) { return a.c_str(); });

    winrt::com_ptr<IDxcOperationResult> result;
    HRESULT hr = compiler->Compile(
        sourceBlob.get(),
        shaderPath.wstring().c_str(),
        L"CSMain",
        info.SupportedShaderModel.c_str(),
        pargs.data(),
        static_cast<uint32_t>(pargs.size()),
        nullptr,
        0,
        includeHandler.get(),
        result.put());

    if (SUCCEEDED(hr))
    {
        winrt::check_hresult(result->GetStatus(&hr));
    }

    if (FAILED(hr))
    {
        if (result)
        {
            winrt::com_ptr<IDxcBlobEncoding> errorsBlob;
            HRESULT getErrorBufferResult = result->GetErrorBuffer(errorsBlob.put());
            if (SUCCEEDED(getErrorBufferResult))
            {
                std::cout << "Details: " << static_cast<const char*>(errorsBlob->GetBufferPointer()) << "\n\n";
            }
        }
        winrt::check_hresult(hr);
    }

    winrt::com_ptr<IDxcBlob> computeShader;
    winrt::check_hresult(result->GetResult(computeShader.put()));
    byteCode.resize(computeShader->GetBufferSize());
    memcpy(byteCode.data(), computeShader->GetBufferPointer(), computeShader->GetBufferSize());

    return byteCode;
}
