# Using DirectStorage
The dstorage.h header file contains inline comments that describe some more detail of methods and parameters.  While more extensive documentation will become available over time, a quick introduction follows:

## Basic Concepts
As with D3D12, the DirectStorage API is a "nano-COM" API.  The following interfaces are provided:
* IDStorageFactory - this can be used to obtain all other DirectStorage objects
* IDStorageFile - a file that is opened, ready to be used with DirectStorage
* IDStorageQueue - all operations are enqueued on a queue
* IDStorageStatusArray - an object that can store results of operations
* IDStorageCustomDecompressionQueue - if the game uses custom CPU decompression, then this is how DirectStorage communicates with the game.
* IDStorageCompressionCodec - an object that can be used to compress/decompress buffers using built-in compression formats.

### Creating instances of DirectStorage interfaces

DStorageGetFactory() can be used to return:
* IDStorageFactory
* IDStorageCustomDecompressionQueue / IDStorageCustomDecompressionQueue1

DStorageCreateCompressionCodec() can be used to return:
* IDStorageCompressionCodec

Example:

```cpp
IDStorageFactory* factory;
HRESULT hr = S_OK;
hr = DStorageGetFactory(IID_PPV_ARGS(&factory));

IDStorageCompressionCodec* compression;
constexpr uint32_t DEFAULT_THREAD_COUNT = 0;
hr = DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, DEFAULT_THREAD_COUNT, IID_PPV_ARGS(&compression));
```
using cppwinrt:
```cpp
com_ptr<IDStorageFactory> factory;
check_hresult(DStorageGetFactory(IID_PPV_ARGS(factory.put())));

com_ptr<IDStorageCompressionCodec> compression;
constexpr uint32_t DEFAULT_THREAD_COUNT = 0;
check_hresult(DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, DEFAULT_THREAD_COUNT, IID_PPV_ARGS(compression.put())));
```


## Queues and Operations
An application enqueues operations onto an IDStorageQueue.  There are four types of operations:
* "Request" - load some data from a file or memory
* "Signal" - signal an ID3D12Fence
* "Status" - record the status of the last batch
* "SetEvent" - sets an event object to a signaled state

Each enqueued operation takes up on slot in the queue, with the number of slots being specified at creation time.  This slot remains in use until the operation has completed. If Enqueue is called when there are no free slots then it will block until a slot becomes available.

Requests do not start processing until they are submitted.  The queue's Submit() function can be used to explicitly submit work.  Requests are also automatically submitted when the queue is half full.  Submitting will wake up the worker thread, so care should be taken to avoid calling it more than necessary.

Requests may complete in any order - so games must be careful to avoid making any assumptions about the order in which they complete.  The signal and status operations execute when all operations before them have completed, but otherwise have no impact on when requests may start executing.

Requests can read from memory or files.  Each individual queue can only service requests of on source type - so if a memory request is enqueued to a file queue then it will fail.  Memory sources allow DirectStorage's decompression functionality to be used with the data that is already present in memory.

Each individual queue is also bound to its source type and destination device.  The source type can be file or memory.  The destination device specifies whether requests on this queue go to system memory or GPU memory.

## Request Destinations
Each request must specify its request destination type.  For some destination types, DirectStorage has expectations of the layout of the data. For developers who are used to Xbox DirectStorage, this is one of the large differences between the two APIs.  This is necessary because the graphics driver is responsible for determining the actual layout of the resources in memory, and so the assets must be stored on disk in an unswizzled format.

### DSTORAGE_REQUEST_DESTINATION_MEMORY
This destination type is for requests that target system memory. DirectStorage treats this as a simple array of bytes that are written to a pointer/size.

### DSTORAGE_REQUEST_DESTINATION_BUFFER
This destination type is for populating a portion of a buffer ID3D12Resource.  This takes a resource, offset and size for the destination.  Internally, DirectStorage uses CopyBufferRegion to put this in the correct final location.

### DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION
This destination type is for populating a region of a single subresource of a texture - the region could also be the entire subresource if desired. The source data is expected to be laid out in a format suitable for CopyTextureRegion. ID3D12Device::GetCopyableFootprints() can be used to retrieve the layout information. The request requires the resource, subresource index and destination region.

### DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES
This destination is intended for populating mip tails - or entire textures - in a single request. This request takes a resource and the first subresource to populate. This subresource, and all remaining ones, are populated - so to populate an entire texture resource, set this value to 0.

As for texture regions, the data is expected to be laid out as described by GetCopyableFootprints(). DirectStorage will call CopyTextureRegion() for each subresource.

### DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE
This destination is intended for populating a specified number of contiguous subresources in a single request. This request takes a resource, the first subresource to populate in the resource, and the number of subresources to copy. The index of the first subresource plus the number of subresources to copy shouldn't exceed the total number of subresources.

As for texture regions, the data is expected to be laid out as described by GetCopyableFootprints(). DirectStorage will call CopyTextureRegion() for each subresource.

### DSTORAGE_REQUEST_DESTINATION_TILES
This destination type is for populating a region of tiles in a tiled resource.  This takes a resource and a region (in the form of a start coordinate and size). The data is expected to be arranged as suitable for passing to CopyTiles().

## Compressed Assets
The data that is read for each request must be possible to decompress in its entirety. This means that it is not, in general, possible to apply a compression algorithm over a complete file, but instead each section of the file must be compressed in isolation. See the [Bulk Load Demo](../Samples/BulkLoadDemo/README.md) sample for details of how a file containing data to be read by multiple requests might be arranged.

## Uncompressed Data Flow
![Staging Buffer Diagram](uncompressed.png)
DirectStorage maintains two staging buffers - one system memory buffer, and another in an upload heap. The above diagram shows the possible ways the data for a request can flow between staging buffers.

DirectStorage requires that the uncompressed data for a request can fit into a staging buffer. Requests that need to go through a staging buffer with an uncompressed size that is larger than the staging buffer size will fail. The staging buffer can be resized with the SetStagingBufferSize() method on the factory.

## Custom Compressed Data Flow
![Staging Buffer Diagram](customcompression.png)
Custom decompression always reads the compressed data into a system memory staging buffer which will be used as a source buffer by the title's supplied decompression logic.  The decompressed result is placed into an upload heap if the destination is VRAM or directly into the caller's buffer for memory destinations.

## Compressed Data Flow
![Staging Buffer Diagram](compression.png)
DirectStorage supports decompressing built-in formats (example: DSTORAGE_COMPRESSION_FORMAT_GDEFLATE) using the GPU which frees up the CPU for other tasks.

DirectStorage maintains two additional staging buffers in VRAM to coordinate the GPU decompression workload. Each of these staging buffers is allocated to the size set via IDStorageFactory::SetStagingBufferSize(). The above diagram shows the possible ways the data for a request can flow between staging buffers.

* "Input Staging Buffer" - source buffer filled with compressed data.
* "Output Staging Buffer" - destination buffer for the resulting uncompressed data.

## Choosing a Staging Buffer size
Choosing a good Staging Buffer size is key to getting the best performance out of DirectStorage.  A too small of a size could greatly reduce performance because requests will end up waiting for staging memory to become available before being able to be processed. Choosing a too large of a size may take away from your application's rendering budget.

Setting a staging buffer size is done by calling:

```cpp
IDStorageFactory::SetStagingBufferSize(UINT32 size)
```

**Important:** Even though dstorage.h contains an enum called **DSTORAGE_STAGING_BUFFER_SIZE** you are __not__ limited to 32MB! The enum is providing a small set of _common_ sizes.

**Note:** Performance diagrams like the one shown below can be generated on your system by building and running the [GpuDecompressionBenchmark Sample](../Samples/GpuDecompressionBenchmark/README.md)

The following diagram shows how staging buffer sizes directly impact IO bandwidth.

![Staging Buffer Size vs Bandwidth](../Samples/GpuDecompressionBenchmark/stagingbuffersizevsbandwidth.png)

The following diagram shows how staging buffer sizes directly impact CPU usage.

![Staging Buffer Size vs Process Cycles](../Samples/GpuDecompressionBenchmark/stagingbuffersizevsprocesscycles.png)


## Getting Started Compressing Content for GPU Decompression
DirectStorage provides a compression codec interface IDStorageCompressionCodec which is used for general purpose compression/decompression and is obtained by calling DStorageCreateCompressionCodec( ).

IDStorageCompressionCodec has the following methods: ( See the dstorage.h header for more details. )

```cpp
size_t CompressBufferBound(size_t uncompressedDataSize)

HRESULT CompressBuffer(
        const void* uncompressedData,
        size_t uncompressedDataSize,
        DSTORAGE_COMPRESSION compressionSetting,
        void* compressedBuffer,
        size_t compressedBufferSize,
        size_t* compressedDataSize)

HRESULT DecompressBuffer(
        const void* compressedData,
        size_t compressedDataSize,
        void* uncompressedBuffer,
        size_t uncompressedBufferSize,
        size_t* uncompressedDataSize)
```

The following snippet is an example on how to compress content using IDStorageCompressionCodec:

using cppwinrt:
```cpp
com_ptr<IDStorageCompressionCodec> compression;
constexpr uint32_t DEFAULT_THREAD_COUNT = 0;
check_hresult(DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, DEFAULT_THREAD_COUNT, IID_PPV_ARGS(compression.put())));

// Read uncompressed content into a buffer
std::vector<uint8_t> uncompressedContent = ReadUncompressedContent(...);

// Allocate a compressed buffer by calling CompressBound to get a supported size
auto bound = compression->CompressBufferBound(uncompressedContent.size());
std::vector<uint8_t> compressedContent;
compressedContents.resize(static_cast<size_t>(bound));

// Compress content
size_t compressedContentSize = 0;
check_hresult(compression->CompressBuffer(uncompressedContent.data(),
                                          uncompressedContent.size(),
                                          DSTORAGE_COMPRESSION_DEFAULT,
                                          compressedContent.data(),
                                          compressedContent.size(),
                                          &compressedContentSize));

```

## Runtime Configuration
The default behavior of DirectStorage aims to provide the best performance on the system it is running on.  However, there are cases where games may want to change this behavior.

The DStorageSetConfiguration function allows games to control aspects of DirectStorage's runtime configuration. This function must be called before the IDStorageFactory is created (eg before the first call to DStorageGetFactory()).

### DSTORAGE_CONFIGURATION
A DSTORAGE_CONFIGURATION structure is passed to DStorageSetConfiguration(). The structure is designed such that zero-initializing the structure produces default behavior.

### NumSubmitThreads
Submitting IO requests can sometimes take a long time. To enable the DirectStorage worker thread to do other work during this time, DirectStorage uses a separate submission thread. By default DirectStorage uses 1 submission thread.  When running on Windows 10 it may be desirable to allow DirectStorage to use more submission threads to achieve a higher bandwidth/request count(by using additional CPU time).

### NumBuiltInCpuDecompressionThreads
DirectStorage will always use CPU decompression for DSTORAGE_REQUEST_DESTINATION_MEMORY requests.

This can be used to specify the maximum number of threads the runtime will use.  Specifying 0 means to use the system's best guess at a good value.

Specifying DSTORAGE_DISABLE_BUILTIN_CPU_DECOMPRESSION means no decompression threads will be created and the title is fully responsible for performing the decompression.  To do this, use IDStorageCustomDecompressionQueue1::GetRequests1() with the DSTORAGE_GET_REQUEST_FLAG_SELECT_BUILTIN or DSTORAGE_GET_REQUEST_FLAG_SELECT_ALL flag.

### ForceMappingLayer and DisableBypassIO
During development it may be useful to force DirectStorage to only make use of the Windows 10 I/O stack.  Forcing the mapping layer to be used and toggling of BypassIO can be done to achieve this.

# Best Practices
The recommendations and best practices can be grouped into the following list of Do's and Don'ts when using DirectStorage.

## Do's
The pipeline model along with the use of notifications leads to several Do's for how to use DirectStorage.
* Choose a large enough staging buffer to ensure that you get the optimal IO bandwidth.
  * Looking at your individual request sizes and amount of requests can help with choosing a good value.
* Submit as many requests at a time as you can to DirectStorage
  * The only limit on the number of requests in flight is the size of the queues the title creates.
* Submit requests in batches
  * Assets tend to be multiple blocks of data within the title package and notifications happens in FIFO order. This leads to submit requests in batches.
  * Enqueue read requests for all the data needed to create the in-game asset, then enqueue one IDStorageStatusArray entry or ID3D12Fence at the end. When the IDStorageStatusArray entry or ID3D12Fence is signaled all the data needed for the asset is guaranteed to be available.
* Read in 32-KiB or larger block sizes
  * The recommendation is to read at least 32-KiB with each read request. Smaller reads will negatively impact overall bandwidth.
* Size your queues correctly
  * There is a significant penalty when a read is enqueued and the queue is full. The Enqueue(Request/Status/Signal) functions suspend the thread until a slot becomes available. The suspension could easily be several milliseconds.
  * The recommendation is ~2x your expected maximum number of elements in the queue at a single point in time. This allows enough buffer space to handle possible variations in timing.
  * Remember as soon as a request is completed its slot is available for a new request.
* If built-in formats requiring CPU decompression are being decompressed by your own job system, always use GetRequests1 to ensure that these requests get serviced.
  * Specifying DSTORAGE_GET_REQUEST_FLAG_SELECT_ALL will allow your system to get both built-in and custom formats in a single call which could be more efficient.

## Dont's
Win32 and DirectStorage have slightly different usage cases. Some patterns that were common in Win32 will negatively impact performance in DirectStorage.
* Don't use DirectStorage with dependency change assets
  * A dependency chain is where you must load asset A before you know the next asset to load.
  * Because of the inherent latency in a single DirectStorage read you have a significant impact on performance using this pattern.
  * Consider the case when waiting 8ms for a single 512-KiB read. This comes out to a bandwidth of 64MB/s, the same speed as the rotational drive in an Xbox One X.
* Don't fear the auto submit
  * Auto submit happens when the number of unsubmitted requests is greater or equal to half the size of the queue.
  * The reason for auto-submit is to help avoid bubbles in the pipeline. There are a significant number of ready requests based on the title's chosen size of the queue so the drive should be notified to start processing.
  * The cost for an auto-submit is the same as a single call to Submit().
  * There is no effect on notification of completed reads. Notification is entirely based on the IDStorageStatusArray entry or the ID3D12Fence markers.
* Don't constantly create IDStorageQueue objects for each batch of assets.
  * Creation of a new IDStorageQueue requires at least some form of memory allocation. Just the overhead of memory allocation can be enough to affect bandwidth. Too many memory allocations and the title can't submit requests fast enough to keep the drive busy.
  * The recommendation is to create most of your IDStorageQueue objects during title startup as opposed for each batch of assets.
  * IDStorageQueue only have a request limit equal to the size of the queue at creation time.
* Don't enqueue an IDStorageQueue entry for ID3D12Fence for every read unless absolutely needed.
  * An IDStorageQueue entry or an ID3D12Fence request should be used around batches.
  * However, read errors are bound to the following IDStorageQueue/ID3D12Fence entry, the error could be from any of the previous reads. IN this case it may be useful at development time to include the extra IDStorageQueue entries to help locate content errors.
* Don't worry about 4-KiB alignment restrictions
  * Win32 has a restriction that asynchronous requests be aligned on a 4-KiB boundary and be a multiple of 4-KiB in size.
  * DirectStorage does not have a 4-KiB alignment or size restriction. This means you don't need to pad your data which just adds extra size to your package and internal buffers.

# SDK Path

By default, DirectStorage expects `dstoragecore.dll` to be in the same folder as the game executable.

If you want to place the dll in another folder, you will need to specify the path to the dll by setting the symbol `DStorageSDKPath` to the path and exporting it in your exe.

For example, if your dll is stored in the folder `.\DirectStorage`, here are two methods you could use:

## Method A: __declspec(dllexport) keyword

You can export the constant in your code through the `__desclspec(dllexport)` keyword:

```
extern "C" {
    __declspec(dllexport) extern const char* DStorageSDKPath = u8".\\DirectStorage\\";
}
```

## Method B: Module-definition file

You can also export the constant via a `.def` file:
```
EXPORTS
    DStorageSDKPath DATA PRIVATE
```

And then declare the constant in code:
```
extern "C" extern LPCSTR DStorageSDKPath = ".\\DirectStorage\\";
```
