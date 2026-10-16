# GDeflate Content Tool

Windows single-file compression worker for Content Factory. Compression includes in-memory and written-file decode/byte comparisons. For factory usage, see `ContentFactory/README.md`.

## Build

Initialize `GDeflate/3rdparty/libdeflate`, then run from the repository root:

```powershell
cmake -S GDeflate -B out/gdeflate-content -A x64 `
  -DGDEFLATE_BUILD_DEMO=OFF -DGDEFLATE_BUILD_TESTS=OFF
cmake --build out/gdeflate-content --config Release --target GDeflateContentTool
```

## Invoke

```powershell
GDeflateContentTool.exe --input original.dds --output generated/original.gdeflate --level 9
```

Input must be one nonempty regular file. Levels are 1–12, default 9. Use `--overwrite` to replace an output; input/output aliases are rejected. `--version` prints the worker identity.

## Format

A 32-byte little-endian header precedes one GDeflate stream:

| Fields, in order | Types |
|---|---|
| Magic `GDF1`, version `1`, header size `32` | `uint32`, `uint16`, `uint16` |
| Compression level, reserved zero | `uint32`, `uint32` |
| Uncompressed size, compressed payload size | `uint64`, `uint64` |
