# Content Factory

Generate compressed files and matching DirectStorage archives once, then reuse them in tests.

## Run

Keep the complete portable package together: `ContentFactory.exe`, `_internal`, and `bin`. Python and the Zstd, GDeflate, and GACL tools are included; no separate installation or tool paths are needed. See `BUILDING.md` to build the package.

```powershell
.\ContentFactory.exe --config C:\requests\content.json
```

## Configure

Copy `examples/request.json` and edit it. See `CONFIGURATION.md` for every field, constraints, units, and additional examples. All processing settings belong in JSON:

```json
{
  "input_directory": "D:/test-content",
  "stages": ["all"],
  "overwrite": false,
  "zstd": { "level": 19, "block_sizes_kb": [16], "chunk_sizes_kb": [256] },
  "gdeflate": { "levels": [9] },
  "gacl": { "zstd_level": 19, "target_block_size": 65536 },
  "hlk": { "archive_prefix": "dstoragetest", "alignment": 1, "zstd_level": 19 }
}
```

`input_directory` is required. Absolute paths are accepted; relative paths resolve from the request file's directory. Other fields are optional and use the defaults below.

| Group | Settings and defaults |
|---|---|
| Top level | `stages`: `["all"]`, or any combination of `zstd`, `gdeflate`, `gacl`, `hlk`; `overwrite`: `false`. |
| `zstd` | `level`: `19` (1–22). `block_sizes_kb`: `[16]`, `chunk_sizes_kb`: `[256]`. Alternatively, `shader_matrix: true` selects all nine 4/8/16 KiB block × 64/128/256 KiB chunk combinations. |
| `gdeflate` | `levels`: `[9]`. Alternatively, `all_levels: true` selects levels 1–12. |
| `gacl` | `zstd_level`: `19`; `target_block_size`: `65536` bytes. |
| `hlk` | `archive_prefix`: `"dstoragetest"`; `alignment`: `1` byte, a positive power of two; `zstd_level`: `19` (1–22). |

Use either a preset or explicit lists, not both. Lists must be nonempty; unknown fields, duplicate keys, and incorrect types are rejected.

Keep request files, tools, and unrelated documents outside the input directory: all regular files outside its root `generated` folder are treated as originals. Originals are read recursively and left unchanged.

## Outputs

```text
input-directory/
|-- model.bin
|-- textures/wood.dds
`-- generated/
    |-- zstd/
    |-- gdeflate/
    |-- gacl/
    |-- hlk/
    `-- manifest.json
```

The root `generated` folder is excluded from input discovery. Selected stages run in Zstd, GDeflate, GACL, then HLK order; generated files preserve original subdirectories. The launcher inventories originals once, validates the selected stages and required tools, then writes a checkpoint after each successful stage. Internal stages receive resolved inputs and return records; they do not load or write reports.

HLK produces matching uncompressed, GDeflate-level-9, and Zstd archives at the selected `hlk.zstd_level`. The shared `tools/hlk_archive.py` module writes and parses the archive format; the HLK stage reopens each archive and compares its stored payloads before publication. Keep `manifest.json` with the outputs: it records paths, sizes, hashes, encoding settings, texture details and archive membership for the **current invocation only**. Unselected outputs may remain on disk without appearing in the new report.

Removed bookkeeping and redundant discovery checks, including restoration pointers, are recorded in [Maintenance notes](MAINTENANCE_NOTES.md).

## Limitations and failures

- Run one command per input directory at a time; keep original files and the directory structure unchanged throughout processing. Discovery rejects linked roots, ancestors, and original entries; stages trust that inventory when opening originals. Output-path guards remain in place.
- Existing selected outputs require `overwrite: true`. New content is checked before replacement, but installation/report-write failure can leave incomplete outputs; fix the error and rerun with overwrite enabled.
- GACL handles DDS array item 0 / mip 0. BC1/3/4/5 use conditioning plus Zstd; BC7 is Zstd-only. Non-DDS files are skipped; cubemaps, volume textures, and unsupported or truncated DDS inputs fail.
- GACL archives and BC7 mode-split strategies are not implemented. The larger-input GDeflate issue remains unresolved.
- Local build/smoke coverage does not establish clean-machine portability, unrestricted native correctness, or GPU correctness.
