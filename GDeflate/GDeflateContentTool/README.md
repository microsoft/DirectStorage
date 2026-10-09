# GDeflate Content Tool

`GDeflateContentTool` creates deterministic, standalone GDeflate content files for DirectStorage infrastructure testing. Every generated file is decompressed on the CPU and compared byte-for-byte with its source before it is accepted.

## File format

Each `.gdeflate` file contains a 32-byte little-endian header followed by one GDeflate stream:

| Field | Type | Description |
|---|---:|---|
| magic | `uint32` | `GDF1` |
| version | `uint16` | `1` |
| header size | `uint16` | `32` |
| compression level | `uint32` | GDeflate level `1` through `12` |
| reserved | `uint32` | Must be zero |
| uncompressed size | `uint64` | Original byte length |
| compressed size | `uint64` | GDeflate payload length |

This format deliberately does not reuse `GDeflateDemo/CompressedFile.h`, whose on-disk size field is the platform-dependent C++ type `size_t`.

## Build

```powershell
cmake -S GDeflate -B out/gdeflate-content -A x64 `
  -DGDEFLATE_BUILD_DEMO=OFF `
  -DGDEFLATE_BUILD_TESTS=OFF `
  -DGDEFLATE_BUILD_CONTENT_TOOL=ON
cmake --build out/gdeflate-content --config Release --target GDeflateContentTool
```

Initialize `GDeflate/3rdparty/libdeflate` before configuring the build.

## Create content

Single file:

```powershell
GDeflateContentTool.exe `
  --input original.dds `
  --output original.dds.gdeflate `
  --level 9
```

Directory tree:

```powershell
GDeflateContentTool.exe `
  --input originals `
  --output gdeflate `
  --recursive `
  --level 9
```

Use `--overwrite` for an intentional deterministic rebuild. Directory processing writes `variants.json` beside the generated hierarchy. The sidecar excludes runtime timing measurements so identical inputs and tool versions produce byte-identical metadata.

The tool accepts every compression level exposed by the GDeflate library: `1` through `12`.

The existing `GDeflateDemo` highlights three DirectStorage presets within that range:

- `1` — fastest compression
- `9` — default
- `12` — highest compression

## Verify content

```powershell
GDeflateContentTool.exe --verify gdeflate --recursive
```

Verification checks the header, file length, compression level, and GDeflate stream. Generation additionally compares decompressed bytes with the source bytes. Source and payload SHA-256 values are recorded in `variants.json`.

Empty inputs are rejected because the GDeflate library requires a non-empty input buffer.
