# Content Factory tooling

The Phase 2 Content Factory prepares deterministic codec inputs and matching DirectStorage HLK archives. Production content stays outside the public repository. The factory-specific GitHub Actions workflow and standalone synthetic source generator have been removed.

## Folder layout

Give the factory one existing input directory. Originals stay in place; generated outputs, the processing report, and temporary staging directories live under its reserved `generated` subfolder:

```text
input-directory/
|-- model.bin
|-- textures/
|   `-- wood.dds
`-- generated/
    |-- zstd/
    |-- gdeflate/
    |-- gacl/
    |-- hlk/
    `-- manifest.json
```

Input discovery is recursive and preserves relative subdirectories. The root `generated` subtree is excluded (case-insensitively), so reruns do not compress previous results or temporary files. A nested original directory such as `textures/generated/` is still input. Paths use `/` in manifests and are sorted ordinally.

## Run the portable package

Use the complete built folder. It contains `ContentFactory.exe`, its Python runtime files, and `bin/zstd.exe`, `bin/GDeflateContentTool.exe`, and `bin/GACLContentTool.exe`. End users do not install Python or encoder tools and do not supply tool paths. Keep the executable, runtime files, and `bin` together. Building this package is a developer task described in `BUILDING.md`.

```powershell
.\ContentFactory.exe --config C:\requests\content.json
```

Tools are resolved relative to the application directory, never from the caller's working directory or `PATH`. A missing selected encoder is reported as an incomplete-package error. Running from source uses `ContentFactory/bin`; encoding paths are determined internally by the package.

## JSON requests

A request file avoids long command lines. It is separate from the generated processing report. Copy `examples/request.json`, edit `input_directory`, and select settings:

```json
{
  "input_directory": "../assets",
  "stages": ["all"],
  "overwrite": false,
  "zstd": { "block_sizes_kb": [16], "chunk_sizes_kb": [256] },
  "gdeflate": { "levels": [9] },
  "gacl": { "zstd_level": 19, "target_block_size": 65536 },
  "hlk": { "archive_prefix": "dstoragetest", "alignment": 1 }
}
```

`input_directory` is required in JSON. Absolute paths such as `D:/test-content` are accepted directly; relative paths resolve from the JSON file's directory. Missing options use the usual defaults: all stages, overwrite off, Zstd 16/256 KiB, GDeflate 9, GACL level 19/64 KiB, HLK prefix `dstoragetest` and alignment 1. Encoder paths are not part of the JSON format.

| JSON group | Supported fields |
|---|---|
| Top level | `input_directory`, `stages`, `overwrite` |
| `zstd` | `shader_matrix`, `block_sizes_kb`, `chunk_sizes_kb` |
| `gdeflate` | `levels`, `all_levels` |
| `gacl` | `zstd_level`, `target_block_size` |
| `hlk` | `archive_prefix`, `alignment` |

Unknown fields, duplicate keys, and incorrect JSON types fail before generation. Lists must be nonempty when supplied. Preset/list combinations still follow the stage's option rules: use the Zstd matrix or explicit sizes, and all GDeflate levels or an explicit level list.

The only processing command option is `--config`. Keep all stages, overwrite permission, and encoding/packaging settings in the JSON file; there is no second CLI settings interface.

## Process once, run tests repeatedly

Each invocation discovers originals directly, generates and checks selected outputs, and writes a report. No inventory setup is required. Keep executable tools and unrelated documents outside the input directory: every regular file outside the reserved output tree is an original. Keep request JSON files outside the input directory for the same reason.

## Stage settings

Select any combination of `zstd`, `gdeflate`, `gacl`, and `hlk` in `stages`, or use `["all"]`. Execution order is fixed. Stage modules and archive writer/reader functions are internal, not additional public commands.

- **Zstd:** fixed compression level 19. The default is a 16 KiB target block / 256 KiB input chunk. Use `zstd.shader_matrix: true` for the nine 4/8/16 KiB by 64/128/256 KiB combinations, or explicit size lists.
- **GDeflate:** level 9 by default. Set `gdeflate.levels` to a list or `gdeflate.all_levels: true` for 1–12. Outputs preserve relative subdirectories. The native encoder handles one file per call and checks both in-memory and written-file round trips; it does not produce a second report.
- **GACL:** level 19 / 64 KiB target by default. Processes supported DDS array item 0 / mip 0; BC1/3/4/5 use conditioning plus Zstd, BC7 uses Zstd only. Unsupported/truncated formats fail explicitly; non-DDS files are skipped.
- **HLK:** prefix `dstoragetest`, alignment 1 by default. Produces matching uncompressed, GDeflate-level-9, and Zstd-level-19 archives. Payload bytes and content types are passed directly to the internal archive writer; the independent reader checks structure and stored bytes. No temporary payload files or JSON entry list is needed for archive assembly.

Before generation, preflight checks selected settings and package encoders. A missing selected encoder reports an incomplete package, not a fallback to system tools. `overwrite: true` permits replacing selected outputs. Tool versions remain recorded.

## Generate, verify, and record

Newly encoded content is decoded and byte-compared with its original input before installation. Texture and GDeflate headers are parsed as required for their formats. Archive construction independently checks its table and stored payload bytes. Input paths, output containment, executable/settings checks, overwrite protection, and generation checks remain enforced.

`generated/manifest.json` records the **current processing invocation**. The factory shares one in-memory report across its selected stages; the selected stages append results to that invocation’s report. Unselected stage outputs may remain on disk but are not claimed as fresh results in this report. Reports contain no timestamps and use stable sorting/serialization.

The scripts do not parse or validate a saved report, compare originals against a previous inventory, rehash previously generated files, or perform a final saved-file reconciliation. `overwrite: true` permits replacement of selected output files/directories; it permits an intentional rebuild with changed originals, encoder versions, or damaged prior outputs. A missing or malformed previous report does not block generation. Previous reports are not read or preserved by processing.

Paths must be canonical relative paths. Output and report parent components may not be symbolic links or junctions; resolved paths must remain inside the input directory. These checks assume directory links are not changed concurrently by another process. Keep originals unchanged while a processing invocation is running.

## Single-run operation and failures

Run only one processing command against an input directory at a time. There are no file locks, backups, rollback transactions, or saved-report snapshots. Outputs remain disposable and can be rebuilt from unchanged originals.

Each stage generates and checks replacements in a temporary folder before touching existing outputs. A generation/verification failure leaves prior outputs intact. Once installation begins, failures may leave incomplete replacement outputs or an outdated report; fix the error and rerun with `overwrite: true` in the request. Successful earlier stages remain installed if a later stage fails. The report is still written via a small temporary-file replacement helper.

The archive writer receives data bytes, not original-file paths. Filesystem path safety is enforced at the factory's original reads and output destinations. Archive offset/size limits, content types, alignment, independent structure checks, and payload comparisons remain enforced. All workspaces are under `generated` and excluded from discovery.

## Processing report

The JSON report includes:

- Original files: relative paths, byte sizes, and SHA-256 fingerprints.
- Generated files: original-file relationships, encoding settings, format metadata, sizes, hashes, and generation-check results.
- Archives: package names, ordered membership, content types, and stored-file details.
- Tools: encoder versions and optional pinned revisions.

GACL records selected texture dimensions, array item, mip, transform identity, and Zstd settings. HLK binary archives do not embed this descriptive metadata, so retain the report with the generated content for consumers. The report describes this invocation; it is not an input to processing.

## Archive layout and low-level tools

The HLK triplet uses the same entry count, order, and content types in every archive. DDS maps to texture, PLY to geometry, TXT to text, and other extensions to unknown. GDeflate entries use level 9 and omit the standalone tool's 32-byte wrapper. Zstd entries are single level-19 frames with a 256 KiB window.

The binary format has an 8-byte version/count header followed by 12-byte type/offset/size entries and raw payloads. It contains no filenames, codec identifiers, decoded sizes, hashes, or transform metadata; those are recorded in the processing report. Entry order is significant.

The internal writer accepts ordered `(content_type, payload_bytes)` entries. It checks types, alignment, and the format's 32-bit limits, then writes directly. No standalone archive command, payload-list JSON, or temporary payload files are used. The independent reader still checks version, table bounds, types, ordered non-overlapping payloads, and file bounds; the factory additionally compares stored payload bytes with expected data.

## Build the complete package

Developers use `build-package.py` to compile all three tools from existing local dependencies and freeze the Python launcher/runtime into one portable folder. See `BUILDING.md` for approved build prerequisites, directory safety, and commands. End users do not build individual encoders or install Python.

No dependencies are downloaded by the builder. Native configure/build and PyInstaller freezing have not been executed in the source-only validation below; an approved build and clean-machine smoke test are required before claiming portability is verified.

## Validation scope

The Content Factory tests and encoding stand-ins are preserved on branch `users/rohanborkar/content-factory-tests-cache`; they are not included in this initial implementation branch. The saved test snapshot is commit `d3fd1b4`.

Before the tests were separated, Python/stand-in checks passed and a six-original comparison confirmed 134 generated payloads and the report remained byte-identical, with deterministic rebuilding and unchanged originals. These checks do not establish native or GPU/runtime correctness. The single-file native worker, native package build, real encoders, and PyInstaller distribution still require approved compilation/runtime validation and a clean-machine smoke test.

The known larger-input GDeflate issue remains unresolved. The GACL archive contract and BC7 mode-split coverage remain open.

## Reading the code

Start with `tools/run-content-factory.py`: discover originals, preflight and resolve settings once, then call regular imported stage modules in order with one shared report. There is no saved-report load or final repeated hash scan.

Each internal encoding stage generates and verifies files in a temporary directory, replaces its selected outputs, appends current results, and writes the report. There is no transaction class or report-merging abstraction. `content_report.py` handles original discovery, path safety, fingerprints, and atomic JSON writing; `factory_common.py` retains only filesystem and tool-execution helpers.

HLK prepares and verifies the complete triplet before replacing its three archives. Installation is sequential and not rolled back. GDeflate native compression already verifies the written file, so Python checks the wrapper/metadata without launching an extra `--verify` process. GACL retains its written-file verification because its native encode comparison precedes file writing.
