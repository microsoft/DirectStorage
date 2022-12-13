/*
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include "CompressedFile.h"
#include "GpuDecompressor.h"

#include <GDeflate.h>

#ifdef WIN32
#define strcasecmp _strcmpi
#else
#include <strings.h>
#endif

void ShowHelp()
{
    std::cout << "Performs compression/decompression operations using GDeflate.\n";
    std::cout << "\n";
    std::cout << "GDeflateDemo [options] [source file path or directory] [destination directory]";
    std::cout << "\n";
    std::cout << "/compress      Compress a single file or multiple files using the CPU.\n";
    std::cout << "/decompress    Decompress a single file or multiple files using the CPU.\n";
#ifdef WIN32
    std::cout << "/decompressgpu Decompress a single file or multiple files using the GPU.\n";
#endif
    std::cout << "\n";
    std::cout << "/demo          Compress a single file or multiple files using the CPU and\n";
    std::cout << "               decompress the result "
#ifdef WIN32
                 "first using the CPU and then with the GPU."
#else
                 "using the CPU."
#endif
        ;
    std::cout << "\n";
    std::cout << "Example:\n";
    std::cout << "GDeflateDemo.exe /compress c:\\file.any c:\\output_directory\n";
    std::cout << "GDeflateDemo.exe /compress c:\\input_directory c:\\output_directory\n";
    std::cout << "\n";
    std::cout << "GDeflateDemo.exe /decompress c:\\file.compressed c:\\output_directory\n";
    std::cout << "GDeflateDemo.exe /decompress c:\\input_directory c:\\output_directory\n";
    std::cout << "\n";
    std::cout << "GDeflateDemo.exe /decompressgpu c:\\file.compressed c:\\output_directory\n";
    std::cout << "GDeflateDemo.exe /decompressgpu c:\\input_directory c:\\output_directory\n";
    std::cout << "\n";
}

enum class Operation
{
    None,
    Compress,
    DecompressCPU,
    DecompressGPU,
    Demo
};

struct Options
{
    bool ShowHelp = false;
    Operation Operation = Operation::None;
    std::filesystem::path SourcePath;
    std::filesystem::path DestinationPath;
    std::filesystem::path ShaderPath;
};

static Options ParseOptions(int argc, char** argv)
{
    // Expects:
    // argv[1] - option
    // argv[2] - source path
    // argv[3] - destination path
    Options options;
    if (argc < 4)
    {
        options.ShowHelp = true;
        std::cout << "\nToo few parameters were passed.\n\n";
        return options;
    }

    if ((strcasecmp(argv[1], "/compress") == 0) || (strcasecmp(argv[1], "-compress") == 0))
    {
        options.Operation = Operation::Compress;
    }
    else if ((strcasecmp(argv[1], "/decompress") == 0) || (strcasecmp(argv[1], "-decompress") == 0))
    {
        options.Operation = Operation::DecompressCPU;
    }
    else if ((strcasecmp(argv[1], "/decompressgpu") == 0) || (strcasecmp(argv[1], "-decompressgpu") == 0))
    {
        options.Operation = Operation::DecompressGPU;
    }
    else if ((strcasecmp(argv[1], "/demo") == 0) || (strcasecmp(argv[1], "-demo") == 0))
    {
        options.Operation = Operation::Demo;
    }
    else
    {
        options.ShowHelp = true;
        std::cout << "\nInvalid option '" << argv[1] << "' was specified.\n\n";
        return options;
    }

    // Detect if the specified source file or path exists.
    options.SourcePath = std::filesystem::weakly_canonical(argv[2]);
    if (!std::filesystem::exists(options.SourcePath))
    {
        std::cout << "\nThe specified source path " << options.SourcePath.string().c_str() << " is not found!\n\n";
        options.ShowHelp = true;
        return options;
    }

    // Detect if the specified destination path is valid.
    options.DestinationPath = std::filesystem::weakly_canonical(argv[3]);
    if (!std::filesystem::exists(options.DestinationPath))
    {
        // Attempt to create the full destination folder structure if needed.
        if (!std::filesystem::create_directories(options.DestinationPath))
        {
            std::cout << "\nThe specified destination path " << options.DestinationPath.string().c_str()
                      << " cannot be created!\n\n";
            options.ShowHelp = true;
            return options;
        }
    }

    if (options.Operation == Operation::DecompressGPU || options.Operation == Operation::Demo)
    {
#ifdef WIN32
        // Detect if the shaders required for decompression are present.
        auto currentPath = GetModulePath();

        options.ShaderPath = currentPath / "GDeflate.hlsl";
        if (!std::filesystem::exists(options.ShaderPath))
        {
            std::cout << "\nThe required shader file GDeflate.hlsl is not found!\n\n";
            options.ShowHelp = true;
            return options;
        }

        if (!std::filesystem::exists(currentPath / "tilestream.hlsl"))
        {
            std::cout << "\nThe required shader file tilestream.hlsl is not found!\n\n";
            options.ShowHelp = true;
            return options;
        }
#endif
    }

    return options;
}

static std::vector<uint8_t> ReadEntireFileContent(std::filesystem::path const& path)
{
    std::vector<uint8_t> contents;
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);

    if (!file.is_open())
        throw std::runtime_error("Content file is not open");

    auto fileSize = std::filesystem::file_size(path);

    contents.resize(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(&contents[0]), fileSize);
    file.close();

    return contents;
}

int CompressContent(std::vector<std::filesystem::path> const& sourcePaths, std::filesystem::path const& destinationPath)
{
    std::cout << "\nCompressing " << sourcePaths.size() << " file(s)\n";

    for (auto& sourcePath : sourcePaths)
    {
        auto compressedFilename = sourcePath.filename();
        compressedFilename += ".compressed";
        std::filesystem::path compressedFilePath = destinationPath / compressedFilename;

        auto fileContents = ReadEntireFileContent(sourcePath);
        size_t compressBounds = GDeflate::CompressBound(fileContents.size());
        std::vector<uint8_t> compressedContents(compressBounds);

        size_t outputSize = compressedContents.size();
        uint32_t flags = GDeflate::Flags::COMPRESS_SINGLE_THREAD;

        // DirectStorage exposes 3 compression setting values to use with the runtime's
        // built-in GDeflate compressor. Below is the mapping of GDeflate's compression
        // settings to DirectStorage's built-in compression settings.

        constexpr uint32_t FastestGDeflateCompressionLevel = 1;    // Maps to DSTORAGE_COMPRESSION_FASTEST
        constexpr uint32_t DefaultGDeflateCompressionLevel = 9;    // Maps to DSTORAGE_COMPRESSION_DEFAULT
        constexpr uint32_t BestRatioGDeflateCompressionLevel = 12; // Maps to DSTORAGE_COMPRESSION_BEST_RATIO

        std::cout << "Compressing " << sourcePath.string() << " to " << compressedFilePath.string() << "...\n";
        if (!GDeflate::Compress(
                compressedContents.data(),
                &outputSize,
                fileContents.data(),
                fileContents.size(),
                BestRatioGDeflateCompressionLevel,
                flags))
        {
            std::cout << "Compression failed!\n";
            return -1;
        }
        std::cout << "Uncompressed Size: " << fileContents.size() << " bytes,"
                  << "Compressed Size: " << outputSize << " bytes\n";
        compressedContents.resize(outputSize); // ensure compressed out is exactly what was compressed.

        std::ofstream compressedFile(compressedFilePath, std::ios::binary);
        // Write file header that contains the uncompressed size of the original data.
        CompressedFileHeader header{};
        InitializeHeader(&header, fileContents.size());
        compressedFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
        compressedFile.write(reinterpret_cast<const char*>(compressedContents.data()), compressedContents.size());
    }

    return 0;
}

int DecompressContent(
    std::vector<std::filesystem::path> const& sourcePaths,
    std::filesystem::path const& destinationPath)
{
    std::cout << "\nDecompressing " << sourcePaths.size() << " file(s) (using the CPU)\n";

    for (auto& sourcePath : sourcePaths)
    {
        auto fileContents = ReadEntireFileContent(sourcePath);
        CompressedFileHeader* header = reinterpret_cast<CompressedFileHeader*>(fileContents.data());
        if (!IsValidHeader(header))
        {
            std::cout << "Invalid compressed file format. The compressed file is expected to have\n"
                         "been compressed using this sample.\n";
            return -1;
        }

        size_t compressedDataSize = fileContents.size() - sizeof(CompressedFileHeader);
        std::vector<uint8_t> uncompressedContents(header->UncompressedSize);
        std::cout << "Decompressing " << sourcePath.string() << " to " << destinationPath.string() << "...\n";
        std::cout << "Compressed Size: " << compressedDataSize << " bytes, ";
        std::cout << "Uncompressed Size: " << header->UncompressedSize << " bytes\n";

        if (!GDeflate::Decompress(
                uncompressedContents.data(),
                uncompressedContents.size(),
                fileContents.data() + sizeof(CompressedFileHeader),
                compressedDataSize,
                1))
        {
            std::cout << "Decompression failed!\n";
            return -1;
        }

        std::filesystem::path uncompressedFilePath = destinationPath / sourcePath.filename();
        uncompressedFilePath.replace_extension("");
        std::cout << "Writing uncompressed result to " << uncompressedFilePath.string() << "...\n";
        std::ofstream uncompressedFile(uncompressedFilePath, std::ios::binary);
        uncompressedFile.write(reinterpret_cast<const char*>(uncompressedContents.data()), uncompressedContents.size());
    }
    return 0;
}

#ifdef WIN32

DeviceInfo GetDeviceInfo(ID3D12Device5* device)
{
    using namespace winrt;

    DeviceInfo info{};

    auto adapterLuid = device->GetAdapterLuid();
    com_ptr<IDXGIFactory6> factory;
    check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())));

    com_ptr<IDXGIAdapter1> adapter;
    check_hresult(factory->EnumAdapterByLuid(adapterLuid, IID_PPV_ARGS(adapter.put())));

    DXGI_ADAPTER_DESC1 adapterDesc{};
    check_hresult(adapter->GetDesc1(&adapterDesc));

    info.Description = adapterDesc.Description;

    // The Microsoft Basic Render Driver has the same limitations as a Warp device.
    // DXGI_ADAPTER_FLAG_SOFTWARE is not set for this device, so we must use the
    // adapter description.
    bool isWarpDevice = ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == DXGI_ADAPTER_FLAG_SOFTWARE) ||
                        (_wcsicmp(adapterDesc.Description, L"Microsoft Basic Render Driver") == 0);

    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    check_hresult(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));

    static wchar_t const* shaderModelName[] = {L"cs_6_0", L"cs_6_1", L"cs_6_2", L"cs_6_3", L"cs_6_4", L"cs_6_5"};
    uint32_t index = model.HighestShaderModel & 0xF;
    check_hresult((index >= _countof(shaderModelName) ? E_UNEXPECTED : S_OK));
    info.SupportedShaderModel = shaderModelName[index];

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
    check_hresult(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)));

    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 = {};
    check_hresult(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));

    info.SupportsWaveIntrinsics = options1.WaveOps;
    info.Supports16BitTypes = options4.Native16BitShaderOpsSupported;
    info.SupportsWaveMatch = model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    info.SIMDWidth = options1.WaveLaneCountMin;
    info.SIMDLaneCount = options1.TotalLaneCount;
    info.SupportsGpuDecompression =
        (info.SIMDWidth >= 4 && model.HighestShaderModel >= D3D_SHADER_MODEL_6_0 && !isWarpDevice &&
         options1.Int64ShaderOps);

    return info;
}

int DecompressContentUsingGPU(
    std::vector<std::filesystem::path> const& sourcePaths,
    std::filesystem::path const& destinationPath,
    std::filesystem::path const& shaderPath)
{
    using namespace winrt;

    std::cout << "\nDecompressing " << sourcePaths.size() << " file(s) (using the GPU)\n";

    if (sourcePaths.empty())
        return 0;

#ifdef _DEBUG
    com_ptr<ID3D12Debug1> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
    }
    else
    {
        std::cerr << "WARNING: D3D12 debug interface not available" << std::endl;
    }
#endif

    com_ptr<ID3D12Device5> device;
    check_hresult(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));

    DeviceInfo deviceInfo = GetDeviceInfo(device.get());
    std::wcout << L"Device: " << deviceInfo.Description << L"\n";
    std::wcout << L"Supported Shader Model:    " << deviceInfo.SupportedShaderModel << L"\n";
    std::cout << "SupportsGpuDecompression:  " << (deviceInfo.SupportsGpuDecompression ? "Yes" : "No") << "\n";
    std::cout << "Supports16BitTypes:        " << (deviceInfo.Supports16BitTypes ? "Yes" : "No") << "\n";
    std::cout << "SupportsWaveIntrinsics:    " << (deviceInfo.SupportsWaveIntrinsics ? "Yes" : "No") << "\n";
    std::cout << "SupportsWaveMatch:         " << (deviceInfo.SupportsWaveMatch ? "Yes" : "No") << "\n";

    if (!deviceInfo.SupportsGpuDecompression)
    {
        std::cout << "\n\nDevice does not support GPU decompression!\n";
        return -1;
    }

    auto GpuDecompressor = GpuDecompressor::Create(device.get(), deviceInfo, shaderPath);

    BufferVector buffers;
    for (auto& sourcePath : sourcePaths)
    {
        auto fileContents = ReadEntireFileContent(sourcePath);
        CompressedFileHeader* header = reinterpret_cast<CompressedFileHeader*>(fileContents.data());
        if (!IsValidHeader(header))
        {
            std::cout << "Invalid compressed file format. The compressed file " << sourcePath.string() << "\n"
                      << "is expected to have been compressed using this sample.\n";
            return -1;
        }
        buffers.push_back(std::move(fileContents));
    }

    auto uncompressedData = GpuDecompressor->Decompress(buffers);

    // Write uncompressed data to destination
    for (size_t i = 0; i < sourcePaths.size(); ++i)
    {
        auto& sourcePath = sourcePaths[i];
        auto& uncompressedBuffer = uncompressedData[i];

        std::filesystem::path uncompressedFilePath = destinationPath / sourcePath.filename();
        uncompressedFilePath.replace_extension("");
        std::cout << "Writing uncompressed result to " << uncompressedFilePath.string() << "...\n";
        std::ofstream uncompressedFile(uncompressedFilePath, std::ios::binary);
        uncompressedFile.write(reinterpret_cast<const char*>(uncompressedBuffer.data()), uncompressedBuffer.size());
    }

    return 0;
}

#endif

template<typename A, typename B>
bool EqualContents(A const& expected, B const& actual)
{
    return std::equal(std::begin(expected), std::end(expected), std::begin(actual), std::end(actual));
}

bool ValidateDecompressedContent(
    std::vector<std::filesystem::path> const& sourcePaths,
    std::filesystem::path const& destinationPath)
{
    bool matched = true;
    for (auto& sourcePath : sourcePaths)
    {
        auto sourceContents = ReadEntireFileContent(sourcePath);
        auto destinationContentPath = destinationPath / sourcePath.filename();
        auto destinationContents = ReadEntireFileContent(destinationContentPath);
        if (!EqualContents(sourceContents, destinationContents))
        {
            matched = false;
            std::cout << "ERROR: Decompressed content " << destinationContentPath.string()
                      << " did not match original content " << sourcePath.string() << "\n";
        }
    }
    return matched;
}

int DemoCompressionAndDecompression(
    std::vector<std::filesystem::path> const& sourcePaths,
    std::filesystem::path const& destinationPath,
    std::filesystem::path const& shaderPath)
{
    int result = 0;
    result = CompressContent(sourcePaths, destinationPath);
    if (result < 0)
        return result;

    // Collect the .compressed files output from the compression step above
    // and pass them along to be decompressed using the CPU and then the GPU.
    std::vector<std::filesystem::path> compressedSourcePaths;
    for (auto& sourcePath : sourcePaths)
    {
        auto compressedFilename = sourcePath.filename();
        compressedFilename += ".compressed";
        compressedSourcePaths.push_back(destinationPath / compressedFilename);
    }

    // Decompress using the CPU
    result = DecompressContent(compressedSourcePaths, destinationPath);
    if (result < 0)
        return result;

    std::cout << "Validating content decompressed with using the CPU...\n";
    if (!ValidateDecompressedContent(sourcePaths, destinationPath))
    {
        std::cout << "ERROR: Content decompressed using the CPU did not\n";
        std::cout << "match original uncompressed content\n ";
    }

#ifdef WIN32
    // Decompress using the GPU
    result = DecompressContentUsingGPU(compressedSourcePaths, destinationPath, shaderPath);
    if (result < 0)
        return result;

    std::cout << "Validating content decompressed with using the GPU...\n";
    if (!ValidateDecompressedContent(sourcePaths, destinationPath))
    {
        std::cout << "ERROR: Content decompressed using the GPU did not\n";
        std::cout << "match original uncompressed content\n ";
    }
#endif

    std::cout << "Validation complete!\n";

    return 0;
}

static std::vector<std::filesystem::path> CollectSourcePaths(std::filesystem::path const& path, bool forDecompression)
{
    std::vector<std::filesystem::path> sourcePaths;
    if (std::filesystem::is_directory(path))
    {
        for (const auto& dirEntry : std::filesystem::directory_iterator(path))
        {
            if (!dirEntry.is_regular_file() || (std::filesystem::file_size(dirEntry.path()) == 0))
                continue;

            bool addFile = true;

            if (forDecompression)
            {
                // Only pickup files with the .compressed extension if the source paths are to be used
                // for gpu decompression
                addFile = (dirEntry.path().extension() == ".compressed");
            }

            if (addFile)
            {
                sourcePaths.push_back(dirEntry.path());
            }
        }
    }
    else
    {
        sourcePaths.push_back(path);
    }

    return sourcePaths;
}

int main(int argc, char** argv)
{
    auto options = ParseOptions(argc, argv);
    if (options.ShowHelp)
    {
        ShowHelp();
        return 0;
    }

    std::vector<std::filesystem::path> sourcePaths = CollectSourcePaths(
        options.SourcePath,
        (options.Operation == Operation::DecompressCPU || options.Operation == Operation::DecompressGPU));

    switch (options.Operation)
    {
    case Operation::Compress:
        return CompressContent(sourcePaths, options.DestinationPath);
    case Operation::DecompressCPU:
        return DecompressContent(sourcePaths, options.DestinationPath);
#ifdef WIN32
    case Operation::DecompressGPU:
        return DecompressContentUsingGPU(sourcePaths, options.DestinationPath, options.ShaderPath);
#endif
    case Operation::Demo:
        return DemoCompressionAndDecompression(sourcePaths, options.DestinationPath, options.ShaderPath);
    default:
        assert(false);
    }

    return 0;
}
