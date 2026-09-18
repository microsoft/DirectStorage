# SRT header generator (offline tooling)

This folder contains the offline code generator for the zstdgpu **Shader Resource
Table (SRT)** binding headers. It is **not** part of any build — the headers it
produces are generated once and checked into source control at:

```
zstd\zstdgpu\srt_headers\
```

Both the HLSL shaders and the C++ library consume those committed headers
directly, so a normal build of `zstd.sln` does **not** need this tool, a C
compiler pre-pass, or `stb_ds.h`.

## Files

| File                   | Purpose                                                             |
|------------------------|---------------------------------------------------------------------|
| `zstdgpu_srt_decl.h`   | The single source of truth: declarative SRT bindings for every kernel. |
| `zstdgpu_srt_tool.c`   | Host tool that expands `zstdgpu_srt_decl.h` into the binding headers. |
| `stb_ds.h`             | Third-party single-header container library used only by the tool.   |
| `generate.cmd`         | Builds and runs the tool, writing into `zstd\zstdgpu\srt_headers`.    |

## When to regenerate

Only when the SRT schema changes — i.e. when you edit `zstdgpu_srt_decl.h`
(adding/removing/retyping a bound resource for a kernel). The generated headers
are otherwise stable.

## How to regenerate

From a Developer or plain command prompt:

```
zstd\tools\srtgen\generate.cmd
```

The script locates the VC x64 host toolchain via `vswhere`, compiles the tool
with `/Zc:preprocessor` (required), runs it, and overwrites the contents of
`zstd\zstdgpu\srt_headers`. Review the diff and commit the updated headers.

## Notes

- `/Zc:preprocessor` is mandatory: the tool stringifies macro parameters that
  share names with struct members, which requires MSVC's conformant preprocessor.
- The generated per-SRT headers cross-include their bind-group headers by bare
  filename, so the `srt_headers` folder must be kept together and on the include
  path (it already is, via the project's `IncludePath`).
- Build intermediates land in `zstd\tools\srtgen\.build\` and can be deleted
  freely.
