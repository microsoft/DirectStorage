# DirectStorage on Windows Samples
DirectStorage is a feature intended to allow games to make full use of high-speed storage (such as NVMe SSDs) that can can deliver multiple gigabytes a second of small (eg 64kb) data reads with minimal CPU overhead.  Although it is possible to saturate a drive using traditional ReadFile-based IO the CPU overhead of increases non-linearly as the size of individual reads decreases.  Additionally, most games choose to store their assets compressed on disk in order to reduce the install footprint, with these assets being decompressed on the fly as load time.  The CPU overhead of this becomes increasingly expensive as bandwidth increases.

Video game consoles such as the XBox Series X|S address these issues by offloading aspects of this to hardware - making use of the NVMe hardware queue to manage IO and hardware accelerated decompression.  As we expect to see more titles designed to take advantage of the possibilities offered by this architecture it becomes important that Windows has similar capabilities.

The DirectStorage API already exists on Xbox and in order to ease porting of titles between Xbox and Windows, the two APIs are as similar as possible.

DirectStorage only supports read operations.

You can find some good starting information in [Developer Guidance](Docs/DeveloperGuidance.md)

We invite you to join us at our [discord server](http://discord.gg/directx). See the [related links](##-Related-links) section for our full list of Direct3D 12 and DirectStorage-related links.

## API Samples
The Samples directory contains sample code that demonstrates how to use the DirectStorage APIs.

1. [HelloDirectStorage](Samples/HelloDirectStorage/README.md): This rudimentary sample serves to provide a quick and easy way to get acquainted with the DirectStorage runtime by reading the contents of a file and writing them out to a buffer on the GPU using DirectStorage.
    
    <img src="Samples/HelloDirectStorage/HelloDirectStorageRender.png" alt="HelloDirectStorage screenshot" height="200">

2. [BulkLoadDemo](Samples/BulkLoadDemo/README.md): This demonstrates using DirectStorage, GPU decompression and custom CPU decompression.
    
    <img src="Samples/BulkLoadDemo/BulkLoadDemo-screenshot.png" alt="BulkLoadDemo screenshot" height="200">

3. [GpuDecompressionBenchmark](Samples/GpuDecompressionBenchmark/README.md): This sample provides a quick way to see the DirectStorage runtime decompression performance by reading the contents of a file, compressing it and then decompressing multiple ways while measuring the bandwidth and CPU usage.  Decompression is performed using the GPU as well as the CPU for comparison.
    
    <img src="Samples/GpuDecompressionBenchmark/GpuDecompressionBenchmarkRender.png" alt="GpuDecompressionBenchmark screenshot" height="200">

## GDeflate Reference Implementation
This directory contains codec source and sample code that demonstrates how to use [GDeflate](GDeflate/README.md).

The repo uses submodules, so be sure to pass `--recurse-submodules` to the `git clone` command.

If you forget to do this then you can run `git submodule update --init`

## Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us the rights to use your contribution. For details, visit https://cla.opensource.microsoft.com.

When you submit a pull request, a CLA bot will automatically determine whether you need to provide a CLA and decorate the PR appropriately (e.g., status check, comment). Simply follow the instructions provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/). For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

## Trademarks

This project may contain trademarks or logos for projects, products, or services. Authorized use of Microsoft trademarks or logos is subject to and must follow 
[Microsoft's Trademark & Brand Guidelines](https://www.microsoft.com/en-us/legal/intellectualproperty/trademarks/usage/general). Use of Microsoft trademarks or logos in modified versions of this project must not cause confusion or imply Microsoft sponsorship. Any use of third-party trademarks or logos are subject to those third-party's policies.

# Build
Install [Visual Studio](http://www.visualstudio.com/downloads) 2019 or higher.

Open the following Visual Studio solutions and build
```
Samples\HelloDirectStorage\HelloDirectStorage.sln
Samples\BulkLoadDemo\BulkLoadDemo.sln
Samples\GpuDecompressionBenchmark\GpuDecompressionBenchmark.sln
```

## Related links
* https://aka.ms/directstorage
* [DirectX Landing Page](https://devblogs.microsoft.com/directx/landing-page/)
* [Discord server](http://discord.gg/directx)
* [Developer Guidance](Docs/DeveloperGuidance.md)
* [PIX on Windows](https://devblogs.microsoft.com/pix/documentation/)
