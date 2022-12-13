/*
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifdef WIN32

struct DeviceInfo
{
    std::wstring Description;
    bool Supports16BitTypes;
    bool SupportsWaveIntrinsics;
    bool SupportsWaveMatch;
    bool SupportsGpuDecompression;
    uint32_t SIMDWidth;
    uint32_t SIMDLaneCount;
    std::wstring SupportedShaderModel;
};

#define DWORD_ALIGN(count) ((count + 3) & ~3)

using BufferVector = std::vector<std::vector<uint8_t>>;

class GpuDecompressor
{
    winrt::com_ptr<ID3D12Device> m_device;
    winrt::com_ptr<ID3D12CommandQueue> m_commandQueue;
    winrt::com_ptr<ID3D12CommandAllocator> m_commandAllocator;
    winrt::com_ptr<ID3D12GraphicsCommandList> m_commandList;

    winrt::com_ptr<ID3D12Fence> m_fence;
    uint64_t m_nextFenceValue;
    wil::unique_event m_fenceEvent;

    winrt::com_ptr<ID3D12RootSignature> m_rootSignature;
    winrt::com_ptr<ID3D12PipelineState> m_pipelineState;
    uint32_t m_dispatchSize;

    winrt::com_ptr<ID3D12DescriptorHeap> m_gpuVisibleDescHeap;
    winrt::com_ptr<ID3D12DescriptorHeap> m_cpuVisibleDescHeap;

    enum RootParameters : uint32_t
    {
        RootSRVInput = 0,
        RootSRVCryptoCtx, // unused
        RootUAVControl,
        RootUAVOutput,
        RootUAVScratch,
        RootParametersCount
    };

    struct Stream
    {
        uint32_t InputOffset;
        uint32_t OutputOffset;
    };

    struct Buffers
    {
        winrt::com_ptr<ID3D12Resource> InputBuffer;
        winrt::com_ptr<ID3D12Resource> OutputBuffer;
        winrt::com_ptr<ID3D12Resource> ControlBuffer;
        winrt::com_ptr<ID3D12Resource> ScratchBuffer;
        winrt::com_ptr<ID3D12Resource> UploadBuffer;
    };
    Buffers m_buffers;

public:
    GpuDecompressor(ID3D12Device* device, DeviceInfo deviceInfo, std::filesystem::path const& shaderPath);
    BufferVector Decompress(BufferVector const& compressedData);

    static std::unique_ptr<GpuDecompressor> Create(
        ID3D12Device* device,
        DeviceInfo deviceInfo,
        std::filesystem::path const& shaderPath);

private:
    void ExecuteCommandListSynchronously();

    void ClearScratchBuffer(uint64_t scratchBufferSize);

    static uint64_t GetRequiredScratchBufferSize(uint16_t numStreams);

    static size_t CalculateControlBufferSize(size_t numStreams);

    static winrt::com_ptr<ID3D12Resource> CreateBuffer(
        ID3D12Device* device,
        uint64_t size,
        D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_FLAGS flags);

    static Buffers CreateBuffers(
        ID3D12Device* device,
        uint64_t inputBufferSize,
        uint64_t outputBufferSize,
        uint64_t controlBufferSize,
        uint64_t uploadBufferSize,
        uint64_t scratchBufferSize);

    static winrt::com_ptr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device);

    static std::vector<uint8_t> CompileShader(std::filesystem::path const& shaderPath, DeviceInfo const& info);
};

#endif
