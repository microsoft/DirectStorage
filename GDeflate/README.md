
# Sample Code
GDeflate is a new compression stream format that closely matches the DEFLATE format. The key difference lies in the way the bits in the compressed bitstream are stored. The GDeflate stream is essentially a reformatted version of any DEFLATE stream where the data is ordered in a particular way to efficiently extract 32 way parallelism without increasing the size of the input stream. This means that GDeflate can get very high decompression throughput on the GPU while still maintaining the exact same compression ratio of DEFLATE (with some small caveats about end effects).

Details on the bitstream can be found in [GDeflate Reference Implementation](GDeflate/README.md)

## 3rdParty\libdeflate
Builds a static library using an updated libdeflate implementation that supports GDeflate.

## GDeflate
Builds a static library for a GDeflate CPU compressor/decompressor.

## Shaders
HLSL source to the GDeflate GPU decompressor

## GDeflateDemo
Demo application that links with both static libraries above and demonstrates how to compress using the CPU codec library and decompress using both the CPU and GPU.

```
GDeflateDemo [options] [source file path or directory] [destination directory]

/compress      Compress a single file or multiple files using the CPU.
/decompress    Decompress a single file or multiple files using the CPU.
/decompressgpu Decompress a single file or multiple files using the GPU.
/demo          Compress a single file or multiple files using the CPU and
               decompress the result first using the CPU and then with the GPU.
```

## GDeflateTest
 Tests and compares the outputs from the GDeflate Reference Implementation and the DirectStorage runtime to ensure they are compatible.

# Build

1. Install [Visual Studio](http://www.visualstudio.com/downloads) 2019 or higher.
2. Launch a Developer Command Prompt
3. Navigate into the GDeflate subdirectory
4. Configure CMake using the command line, VSCode, or Visual Studio

Note: This code can be built using linux as long as you have CMake 3.19 and ninja-build installed.

## Command line
```
cmake --preset Debug
cmake --build --preset Debug
```

## VSCode
Launch VSCode in the GDeflate directory root.  CMake generation will happen automatically.

## Visual Studio
Launch Visual Studio and choose 'Open a local folder' and select the GDeflate directory root.

### Troubleshooting builds
If you see the following error message you may need to update submodules for the repo:
```
Preset CMake variables:

 

  CMAKE_BUILD_TYPE="Debug"
  CMAKE_TOOLCHAIN_FILE:FILEPATH="D:/prj/DirectStorage.Samples/GDeflate/vcpkg/scripts/buildsystems/vcpkg.cmake"

 

CMake Error at C:/Program Files/CMake/share/cmake-3.23/Modules/CMakeDetermineSystem.cmake:130 (message):
  Could not find toolchain file:
  D:/prj/DirectStorage.Samples/GDeflate/vcpkg/scripts/buildsystems/vcpkg.cmake
Call Stack (most recent call first):
  CMakeLists.txt:5 (project)

 


CMake Error: CMake was unable to find a build program corresponding to "Ninja".  CMAKE_MAKE_PROGRAM is not set.  You probably need to select a different build tool.
CMake Error: CMAKE_C_COMPILER not set, after EnableLanguage
CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage
-- Configuring incomplete, errors occurred!
```

Update submodules by using the following git commands
```
git submodule init
git submodule update
```