# Content Factory tooling

The Phase 2 Content Factory prepares deterministic codec inputs and matching DirectStorage HLK archives. Production content stays outside the public repository. The factory-specific GitHub Actions workflow and standalone synthetic source generator have been removed.

## Folder layout

Use a format-first root with one matching directory per set:

```text
content-factory/
├── originals/<set-name>/
├── zstd/<set-name>/
├── gdeflate/<set-name>/
├── gacl/<set-name>/
├── dstorage/<set-name>/
└── manifests/<set-name>.json
```

Set names may contain letters, digits, `.`, `_`, and `-`. Source trees may contain nested directories; paths in manifests always use `/` separators and are sorted ordinally.

## Create a set manifest

Put source files under `originals/<set-name>/`, then run:

```powershell
python ContentFactory\tools\process-set.py <set-name> --root C:\content-factory
```

The driver inventories every source file, computes SHA-256 values, creates the format directories, validates the document, and atomically writes `manifests/<set-name>.json`.

The driver establishes the source and manifest contract. Codec stages append verified `derivatives` records for Zstd, GDeflate, and GACL, plus ordered `archives` records for DirectStorage packages.

Shader Zstd derivatives use explicit **compression level 19**, regardless of the CLI default or `ZSTD_CLEVEL`. By default, the stage produces one 16 KiB target-block / 256 KiB frame-chunk variant per source:

```powershell
python ContentFactory\tools\zstd_compress.py <set-name> `
  --root C:\content-factory `
  --zstd-exe C:\tools\zstd.exe
```

For sets specifically targeting the Zstd shader and metacommand matrix, explicitly request all 4/8/16 KiB block × 64/128/256 KiB chunk combinations with `--zstd-shader-matrix`. Alternatively supply `--block-sizes-kb` and `--chunk-sizes-kb` for selected combinations; the matrix flag and explicit sizes cannot be combined. The factory orchestrator accepts the same options. Each chunk is one Zstd frame; the concatenated stream is decoded and byte-compared with its source before transactional installation. Use `--overwrite` for a deterministic rebuild. A changed Zstd version requires the additional explicit `--allow-version-change` flag. New derivative records include `parameters.compression_level: 19`; older records without a level remain readable, but their setting is unknown. Use `--overwrite` to regenerate older files at level 19. All Content Factory Zstd paths now default to level 19 through a shared policy constant. HLK still uses one frame per entry and a 256 KiB window; GACL still supports an explicit `--zstd-level` / orchestrator `--gacl-zstd-level` override.

Generate GDeflate variants with the built content tool:

```powershell
python ContentFactory\tools\gdeflate_compress.py <set-name> `
  --root C:\content-factory `
  --gdeflate-exe C:\build\GDeflateContentTool.exe
```

By default, the processor creates one level-9 output per source, matching the native tool default. Use `--levels 6` for one level, `--levels 1 6 9` for an explicit subset, or `--all-levels` for all levels 1 through 12. List/all modes are mutually exclusive; lists must be non-empty, unique and in range. The orchestrator exposes `--gdeflate-levels 6`, `--gdeflate-levels 1 6 9`, and `--gdeflate-all-levels`. The singular Python options have been removed, and full option names are required. Each output is verified by the native tool and its 32-byte header is checked. Output-tree and manifest replacement remains transactional; `--overwrite` replaces the entire existing GDeflate selection, rather than appending to it. Explicit sweeps affect only derivatives; the native executable still takes `--level` for each compression call, and HLK encoding stays fixed at level 9, matching the native default.

Generate BC1/BC3/BC4/BC5/BC7 GACL derivatives with the reduced production tool:

```powershell
python ContentFactory\tools\gacl_compress.py <set-name> `
  --root C:\content-factory `
  --gacl-exe C:\build\GACLContentTool.exe
```

GACL defaults to Zstd level 19. Phase 2 conditions array item 0 / mip 0 and records the DDS format, dimensions, transform identity/version, and constrained-Zstd settings in the manifest. Every raw `.gacl` stream is decompressed, reverse-transformed where applicable, and byte-compared before installation. BC7 uses production-supported transform ID 7 (`GACL_SHUFFLE_TRANSFORM_ZSTD_ONLY`); the pinned experimental BC7 split/join transforms remain deferred as a future compression optimization.

Create the existing HLK-style content triplet from the ordered source set:

```powershell
python ContentFactory\tools\hlk_content_set.py <set-name> `
  --root C:\content-factory `
  --zstd-exe C:\tools\zstd.exe `
  --gdeflate-exe C:\build\GDeflateContentTool.exe
```

This produces lockstep `dstoragetest.uncompressed`, `dstoragetest.gdeflate`, and `dstoragetest.zstd` files. Zstd entries use standalone level-19 frames with a 256 KiB window; GDeflate entries use fixed level 9 with the wrapper removed. Content types derive from source extensions. Derivative-level selection does not change these HLK settings. Mixed-derivative packaging and custom content-type mapping files have been removed. `.gacl` derivatives remain, but there is currently no GACL archive generator; its consumer/archive contract is pending spec review.

Run the complete workflow in dependency order:

```powershell
python ContentFactory\tools\run-content-factory.py <set-name> `
  --root C:\content-factory `
  --stages all `
  --zstd-exe C:\tools\zstd.exe `
  --gdeflate-exe C:\build\GDeflateContentTool.exe `
  --gacl-exe C:\build\GACLContentTool.exe
```

Before copying new sources or generating outputs, the orchestrator checks every selected stage's options and required executable paths. Preflight does not execute codecs or probe their versions; executable/version failures may still occur during processing. The GACL stage requires its executable only when the set contains DDS sources. Invalid later-stage options therefore do not leave earlier-stage outputs behind.

The orchestrator normally generates one 16/256 KiB Zstd configuration per source; add `--zstd-shader-matrix` only for Zstd shader/metacommand coverage sets.

Initialize a new set using `--sources FILE [FILE ...]`, or place originals under `originals/<set-name>/` and inventory them with `ContentFactory\tools\process-set.py <set-name> --root <root>`. `--stages all` selects only `zstd gdeflate gacl hlk`; the generic `archive` stage and its options are no longer accepted. The standalone synthetic generator is removed.

Use `--overwrite` for intentional deterministic rebuilds. The manifest contains no timestamps and uses stable sorting and serialization, so unchanged sources produce byte-identical output.

## Verify a set

```powershell
python ContentFactory\tools\process-set.py <set-name> --root C:\content-factory --verify
```

Verification checks:

- Manifest schema version and strict structure
- Safe normalized relative paths
- Sorted, unique source inventory
- Current source sizes and SHA-256 values
- Derivative and archive references
- Recorded derivative/archive size and SHA-256 values

Verification fails if sources were added, removed, renamed, or modified after manifest creation.

## Manifest contract

`ContentFactory/tools/content-set-manifest.schema.json` is the machine-readable schema. Important sections are:

- `sources` — original relative path, byte size, and SHA-256
- `derivatives` — source relationship, output format, codec parameters, optional format metadata, and validation state
- `archives` — source-backed HLK triplets with archive group, payload codec, ordered membership, and content type. Legacy generic derivative records remain readable, but no current script generates them.
- `tools` — tool versions and optional pinned revisions

Zstd derivative parameters include target block size, frame chunk size, and explicit compression level. GACL derivative metadata is expected to carry DXGI format, dimensions, array item, mip, transform ID/version/options, and Zstd settings. HLK-compatible archives do not store that metadata internally, so the manifest remains authoritative.

## Archive layout and low-level tools

The HLK triplet uses the same entry count, order, and content types in every archive. DDS maps to texture, PLY to geometry, TXT to text, and other extensions to unknown. GDeflate entries use level 9 and omit the standalone tool's 32-byte wrapper. Zstd entries are single level-19 frames with a 256 KiB window.

The binary format has an 8-byte version/count header followed by 12-byte type/offset/size entries and raw payloads. It contains no filenames, codec identifiers, decoded sizes, hashes, or transform metadata; those remain in the set manifest. Entry order is significant.

For one archive, supply the low-level writer an explicit JSON payload list:

```json
{"entries": [{"path": "texture.bin", "content_type": "texture"}]}
```

```powershell
python ContentFactory\tools\create_hlk_archive.py payloads.json content.bin `
  --source-root path\to\payloads --alignment 1
python ContentFactory\tools\validate_hlk_archive.py content.bin
```

Types in the low-level writer's payload list may be `unknown`, `texture`, `geometry`, or `text`, or numeric values 0 through 3. This is part of the binary archive layout, not a separate custom mapping file. The independent reader checks version, table bounds, types, ordered non-overlapping payloads, and file bounds. Its CLI only validates; extraction and JSON report output are removed.

## Build the native tools

The native GDeflate tool uses the existing library in `GDeflate/`. The reduced GACL tool builds selected shuffle implementations and Zstd from an explicitly supplied GACL checkout; it does not need the full GACL application's DirectXTex, ONNX Runtime, or GoogleTest projects.

```powershell
cmake -S GDeflate -B out/gdeflate-content -A x64 `
  -DGDEFLATE_BUILD_CONTENT_TOOL=ON -DGDEFLATE_BUILD_DEMO=OFF -DGDEFLATE_BUILD_TESTS=OFF
cmake --build out/gdeflate-content --config Release --target GDeflateContentTool
cmake -S ContentFactory/GACLContentTool -B out/gacl-content -A x64 `
  -DGACL_ROOT=path\to\gacl
cmake --build out/gacl-content --config Release --target GACLContentTool
```

## Validation sources

`tests/test_codec_stages.py` combines the Zstd, GDeflate and GACL stage tests, with separate codec sections and unchanged native prerequisites. `tests/test_hlk_archive.py` keeps separate writer/reader classes and checks structure, stored payload bytes, deterministic rebuilding, and malformed tables. `tests/test_native_tools.py` retains separate real GDeflate/GACL classes and their original tool-environment requirements. Unit-test fixtures and codec stand-ins remain; they are not production compressed data. There is no experimental target, standalone source generator, or factory-specific GitHub Actions workflow. For Python-only validation, set `GDEFLATE_CONTENT_TOOL` to a small wrapper invoking `tests/fake_gdeflate.py`, then run pytest over `ContentFactory/tests` excluding `test_native_tools.py`. The native tests require real tool paths in `GDEFLATE_CONTENT_TOOL` and `GACL_CONTENT_TOOL`. The codec stand-ins do not produce production compressed streams.

The known larger-input GDeflate crash remains unresolved. Bounded validation excludes `test_all_supported_levels_round_trip` (a 220 KiB source); all levels are checked separately on smaller inputs. Do not treat these checks as full production-corpus or GPU/runtime sign-off. The GACL archive contract and BC7 mode-split coverage also remain open.

## Related tools

- `ContentFactory/tools/factory_common.py` supplies shared module loading, checked tool commands, atomic byte writes, executable checks, and stage-specific locks; output transactions remain in each stage.
- `ContentFactory/tools/factory_contracts.py` holds shared Python content types, GACL transform definitions, and GDeflate envelope parsing. The shared Zstd default is 19; HLK frame/window settings remain separate.
- `GDeflate/GDeflateContentTool` generates and verifies standalone GDeflate content.
- `ContentFactory/GACLContentTool` generates and verifies production BC1/3/4/5/BC7 GACL payloads.
- `ContentFactory/tools/hlk_content_set.py` creates the existing uncompressed/GDeflate/Zstd HLK triplet; GACL archive compatibility remains a spec-review question.
- `ContentFactory/tools/create_hlk_archive.py` is the deterministic low-level archive writer.
- `ContentFactory/tools/validate_hlk_archive.py` validates archive structure; its CLI does not extract payloads.

## Just ask any agent

Ask an agent to create or verify a Content Factory set, inspect its manifest, run codec validation, or explain why a recorded file no longer matches its source.
