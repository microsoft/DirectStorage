# JSON configuration

Run the complete package with one request file:

```powershell
.\ContentFactory.exe --config C:\requests\content.json
```

All processing settings go in JSON. There are no command-line settings overrides. Keep the request outside the input directory so it is not treated as original content.

## Minimal request

```json
{
  "input_directory": "D:/test-content"
}
```

This runs all applicable stages with default settings. Originals are left unchanged; outputs and `manifest.json` go under the input directory's reserved `generated` folder. The root `generated` subtree is excluded from discovery.

Absolute paths are accepted directly. Relative input paths resolve from the request file's directory, not the command's working directory. Use `/` in Windows JSON paths, or escape backslashes: `"D:\\test-content"`.

## All settings

Every field except `input_directory` is optional. Numbers must be JSON integers, booleans must be `true` or `false`, and supplied lists must be nonempty. Unknown fields and duplicate keys are rejected.

| Field | Default | Meaning / constraints |
|---|---|---|
| `input_directory` | Required | Existing directory containing originals. Files are discovered recursively. Redirected input roots and content paths are rejected. |
| `stages` | `["all"]` | `["all"]`, or unique names selected from `zstd`, `gdeflate`, `gacl`, `hlk`. Do not mix `all` with individual names. Execution order is always Zstd → GDeflate → GACL → HLK. |
| `overwrite` | `false` | Permit replacement of existing selected outputs. Set `true` for a deliberate rebuild. |
| `zstd.level` | `19` | Zstd compression level, 1–22. Levels 20–22 automatically enable the encoder's `--ultra` mode and can require more memory. |
| `zstd.shader_matrix` | `false` | Generate nine block/chunk combinations: blocks 4/8/16 KiB × chunks 64/128/256 KiB. Cannot be combined with explicit size lists. |
| `zstd.block_sizes_kb` | `[16]` | Unique positive integer target block sizes in KiB. Each must be no larger than every selected chunk size. |
| `zstd.chunk_sizes_kb` | `[256]` | Unique positive integer input chunk sizes in KiB. Each selected block/chunk pair produces a generated file. |
| `gdeflate.levels` | `[9]` | Unique compression levels, each 1–12. Cannot be combined with `all_levels: true`. |
| `gdeflate.all_levels` | `false` | Generate all levels 1–12 instead of an explicit list. |
| `gacl.zstd_level` | `19` | Zstd level for conditioned texture payloads, 1–22. Independent of `zstd.level` and `hlk.zstd_level`. |
| `gacl.target_block_size` | `65536` | Target compressed-block size in **bytes**, 1–1048576 inclusive. |
| `hlk.archive_prefix` | `"dstoragetest"` | Filename prefix shared by the three archives. Must be a safe filename, without path separators, Windows-invalid/control characters, or trailing dot/space. |
| `hlk.alignment` | `1` | Payload alignment in bytes; positive power of two, subject to the archive's 32-bit offset/size limits. |
| `hlk.zstd_level` | `19` | Zstd level for archive payloads, 1–22. Independent of individual Zstd file settings; levels above 19 enable `--ultra`. HLK GDeflate payloads use level 9. |

Zstd sizes use **KiB** (1024 bytes); GACL target size and archive alignment use **bytes**. Target compressed-block size is a compression setting, not a guarantee of exact output length.

Settings groups must be JSON objects. Options for stages that are not selected do not cause those stages to run. Encoder locations are supplied by the package, not by JSON.

## Examples

### Zstd files with custom level and sizes

```json
{
  "input_directory": "D:/test-content",
  "stages": ["zstd"],
  "zstd": {
    "level": 12,
    "block_sizes_kb": [8, 16],
    "chunk_sizes_kb": [128, 256]
  }
}
```

This produces four variants per original. The report records the chosen compression level and block/chunk settings.

### Shader coverage matrix

```json
{
  "input_directory": "../assets",
  "stages": ["zstd"],
  "overwrite": true,
  "zstd": { "level": 19, "shader_matrix": true }
}
```

### Texture files and archives

```json
{
  "input_directory": "D:/textures",
  "stages": ["gacl", "hlk"],
  "gacl": { "zstd_level": 12, "target_block_size": 65536 },
  "hlk": { "archive_prefix": "textures", "alignment": 4096, "zstd_level": 9 }
}
```

GACL generates compressed selected texture payloads; HLK independently encodes the originals into uncompressed, GDeflate, and Zstd archives. It does not package the GACL outputs. GACL is skipped by the factory if no original filename ends in `.dds`; selecting only GACL in that case reports no applicable stage.

For a complete editable request, copy `examples/request.json`.

## Outputs, rebuilding, and limits

- `generated/zstd`, `generated/gdeflate`, and `generated/gacl` contain individual generated files; original subdirectories are preserved.
- `generated/hlk` contains `<prefix>.uncompressed`, `<prefix>.gdeflate`, and `<prefix>.zstd` with matching ordered membership.
- `generated/manifest.json` describes **this invocation only**, including encoding settings. Unselected outputs may remain on disk without appearing in the new report.
- Use one invocation per input directory at a time. Keep originals unchanged while it runs. New bytes are checked before replacement, but installation/report-write failure is not rolled back; fix the problem and rerun with `overwrite: true`.
- GACL currently handles DDS array item 0 / mip 0; supported formats are BC1/3/4/5/7. Volume textures and unsupported/truncated inputs fail. BC7 uses Zstd without conditioning. GACL archives and BC7 mode-split strategies are not implemented.
- The known larger-input GDeflate issue remains unresolved. A successful small-input smoke test is not unrestricted native/runtime validation.
