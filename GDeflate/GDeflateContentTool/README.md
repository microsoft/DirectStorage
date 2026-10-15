# GDeflate Content Tool

`GDeflateContentTool` is the Windows native single-file compression worker for Content Factory. It reads one regular file and writes one standalone GDeflate content file. Each compression request validates the compressed bytes in memory, then reads and decompresses the written file and compares it byte-for-byte with the source.

## Build

```powershell
cmake -S GDeflate -B out/gdeflate-content -A x64 `
  -DGDEFLATE_BUILD_DEMO=OFF `
  -DGDEFLATE_BUILD_TESTS=OFF `
  -DGDEFLATE_BUILD_CONTENT_TOOL=ON
cmake --build out/gdeflate-content --config Release --target GDeflateContentTool
```

The content-tool option defaults to ON, but the target is added only on Windows. Initialize `GDeflate/3rdparty/libdeflate` before configuring the build.

## Native worker

```powershell
GDeflateContentTool.exe `
  --input original.dds `
  --output generated/original.dds.gdeflate `
  --level 9
```

- `--input` must identify one non-empty regular file, not a directory.
- `--output` identifies one file. Missing parent directories are created.
- `--level` accepts unsigned decimal integers from `1` through `12`; the default is `9`. Signs, whitespace, trailing text, and overflow are rejected.
- `--overwrite` allows replacement of an existing output file. The input itself, including a hardlink alias, cannot be the output; output directories are rejected.
- `--version` prints the worker identity and exits.

The worker does not enumerate directories, produce JSON sidecars, or expose a separate verification command. Successful compression includes both internal round-trip checks. A pre-existing `.syncing` staging path is rejected rather than truncated.

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

## Content Factory

For the public JSON-driven interface, use:

```powershell
ContentFactory.exe --config request.json
```

Content Factory owns request processing and output orchestration. See `ContentFactory/README.md` for the request format and `ContentFactory/BUILDING.md` for packaging. The commands above are for building and invoking the native worker directly.
