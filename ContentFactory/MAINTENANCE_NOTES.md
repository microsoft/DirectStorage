# Simplification notes

These notes record deliberately removed bookkeeping and redundant checks so they can be reconsidered without restoring an older implementation wholesale.

## Original-file discovery

Discovery reads a local directory and constructs its own relative file paths. It does not accept file-path records from an old report.

### Removed

- **General relative-path spelling validator:** removed `content_report.validate_relative_path()` and its call from `inventory_sources()`, together with the `PurePosixPath` import. It checked type, emptiness, absolute paths, dot/parent segments, backslashes, colons, and normalized spelling. The caller already had a path returned by directory traversal, converted with `relative_to(input_directory).as_posix()`; it was not validating an arbitrary JSON path.
- **Repeated full-path checks during inventory:** replaced `checked_content_path()` for every child with a direct symbolic-link/junction check on that entry. The root is checked once per inventory, and directory entries are checked before traversal continues into their children. Inventory no longer repeatedly resolves the root and child or rechecks every ancestor for every entry.

This removes the separate portable path-spelling policy. Ordinary discovered paths have the same report representation; names allowed by the host filesystem are no longer independently screened by that removed validator. Discovery still rejects linked entries.

### Retained

- Existing-directory check and link/junction checks on the input root and its ancestors.
- Explicit failure when a nested directory cannot be read, rather than silently publishing a partial inventory.
- Direct link/junction rejection for every discovered directory/file, and regular-file filtering.
- Root `generated` exclusion, deterministic ordering, and rejection of an inventory with no original files.
- Original sizes and chunked SHA-256 calculation.
- Guarded report/output paths, overwrite protection, and temporary-file replacement.

The launcher now relies on inventory for the single input-root validation; stages no longer validate the root again. This is not a concurrent-filesystem security boundary: originals and directory structure must remain stable during processing.

### Restoration pointers

If inventory later accepts arbitrary externally supplied file paths, validate those paths at that boundary. The previous spelling validator was:

```python
from pathlib import PurePosixPath


def validate_relative_path(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")
    path = PurePosixPath(value)
    if (not path.parts or path.is_absolute() or ".." in path.parts
            or "." in path.parts or "\\" in value or ":" in value
            or value != path.as_posix()):
        raise ValueError(f"{field} must be a safe normalized relative path")
    return value
```

Its former inventory call, immediately after constructing `relative`, was:

```python
validate_relative_path(relative, "original path")
```

For full per-entry ancestor/containment checks, replace the direct link check with the still-existing shared helper:

```python
checked_content_path(
    input_directory,
    path.relative_to(input_directory).as_posix(),
    "original file",
)
```

Cached tests for the removed generic validator should be restored if that function returns. Root, redirected-original, unreadable-folder, and output-path tests remain applicable.

## Tool-version bookkeeping

Removed informational version probes and report provenance:

- `content_report.TOOL_VERSION` and the report's `tools` section.
- `factory_common.tool_version()` and its regular-expression import.
- Zstd/GDeflate/GACL stage version helpers and launches of `--version` during processing.
- HLK's `verify_tool()` helper and separate encoder-banner report records. Encoder path validation remains.

These identified tools but did not verify compressed content. Processing now relies on actual encode/decode/reconstruction comparisons and archive checks, rather than requiring a recognizable version banner.

Native workers retain manual `--version` commands. Binary wrapper/archive versions, the required GACL shuffle-version API argument, and generated GACL transform metadata are unchanged.

If provenance is needed later, prefer one package/build identifier in the report. Independently restore component queries only when they serve a defined compatibility or troubleshooting need; update tests and documentation with the report fields.

## Archive module consolidation

Combined `create_hlk_archive.py` and `validate_hlk_archive.py` into `tools/hlk_archive.py`. The header, entry table, version, and content-type definitions now have one home. `ArchiveEntry`, `align_up()`, `create_archive()`, and `parse_archive()` remain separate definitions in that module.

The writer and parser algorithms are unchanged. `hlk_content_set.py` imports the combined module directly instead of receiving separate writer/reader modules as function arguments. It still reopens and parses the actual stored file, then compares each payload with its expected bytes before publication.

This removes independently duplicated format definitions, not read-back verification. Cached fixed-byte tests specify the expected header/table/padding using literal bytes independently of the module's struct definitions. They also verify that changed stored payload bytes are rejected by the stage even when structural parsing succeeds.

If a separately implemented compatibility reader is needed later, give it an independently sourced contract or fixture corpus. Merely splitting the functions into two files does not establish compatibility with the actual HLK consumer. Binary layout and output bytes are unchanged by this consolidation.

## Single-owner validation, orchestration, and reporting

The launcher now creates the inventory once, validates applicable stage settings, resolves each required encoder once, and calls the four stages explicitly in fixed order. `preflight()` returns named resolved options rather than a dictionary of module-specific positional tuples. The `STAGE_MODULES` dispatch map and `process_selected_stages()` helper were removed.

Removed from internal stage `process()` functions:

- Repeated input-root, executable-path, compression-level, level-selection, size-matrix, GACL-option, and HLK-option validation already performed by launcher preflight.
- Optional `report` arguments, original rediscovery/report fallback paths, and report-destination lookup.
- Mutation/sorting of the report's `derivatives` and `archives` sections and direct JSON writes.

Stages now take an explicit ordered `sources` inventory and return generated-file or archive records only after verified outputs are installed. The launcher appends/sorts those records and writes the report after each completed stage. A later stage failure therefore leaves the last successful checkpoint on disk. A report-write failure still leaves installed outputs in place and stops later stages; there is no rollback.

At the single-owner refactor step, original-read path checks, output path guards, overwrite permission, staging/replacement logic, encode/decode comparisons, DDS checks, and archive verification were retained. The subsequent trusted-inventory cleanup below removes repeated original-read checks only; discovery and output guards remain.

### Restoration pointers

The old independently callable stage APIs and report fallback behavior are available in commit `95d0ca637031eed9c1f776396a2ff1ba060d2884`. Restore a validated/report-owning wrapper only if a stage becomes a supported standalone entry point again; current internal callers must go through the launcher for request validation and report publication. Low-level settings-validation helpers remain because the launcher uses them. Low-level encoding functions remain directly testable.

Cached tests exercise old behavioral scenarios through the launcher boundary, and direct stage-contract tests assert no request validation, discovery, or report writes occur inside stages. Additional tests cover source-list identity, one root validation, unique tool checks, explicit order, per-stage report checkpoints, and failure/recovery behavior.

## Trusted inventory, parser-owned defaults, and report ordering

The request parser is the only owner of default compression levels and bundled encoder paths. Launcher preflight no longer supplies fallback values for missing level attributes or missing/`None` executable paths. It consumes a complete parsed request and still validates applicable settings and required executable files. Unknown/duplicate JSON-field detection and basic type checks are unchanged.

Zstd, GDeflate, and GACL builders now return generated-file records in processing order, without sorting. The launcher alone sorts completed report sections before each checkpoint. Original inventory ordering and the ordered HLK payload/member lists are unchanged. Generated output bytes, names, and final report order are not intended to change.

All four encoding paths now open originals using `input_directory / record["path"]`. They no longer call `checked_content_path()` again for each original read. Inventory still validates the input root and its ancestors, rejects discovered symbolic links/junctions, filters original files, and constructs the relative paths itself.

### Supported assumption and retained checks

Original files and the directory tree must remain stable from discovery until processing finishes. Stage inputs are trusted inventory records, not arbitrary externally supplied file paths. The tool does not detect a path being redirected after discovery. Do not use these internal stage functions as an untrusted-input interface.

Output/report path checks, guards immediately before output deletion/replacement, overwrite permission, temporary staging, encode/decode comparisons, DDS validation, archive structure/payload verification, and per-stage report checkpoints remain unchanged. No duplicate-JSON-key handling was removed in this pass.

### Restoration pointers

To recheck an original immediately before use, replace the direct join with the shared helper that remains in `factory_common.py`:

```python
source_path = checked_content_path(input_directory, original_file["path"], "original file")
```

The HLK payload loop names that record `source` rather than `original_file`. Restore this call in all four original-read loops if inventory becomes externally supplied or the stable-tree assumption changes. Such a check still does not provide a lock against concurrent changes.

If standalone stages need deterministic record ordering independently of the launcher, restore sorting in a public wrapper; avoid sorting both builders and launcher without a defined need. If callers are allowed to bypass request parsing, provide one explicit request-defaulting function rather than silently accepting partial requests at several layers.

The original-read checks and builder sorting before these simplifications are also available in commit `95d0ca637031eed9c1f776396a2ff1ba060d2884`; internal stage signatures there predate the single-owner refactor. Cached tests now construct complete requests and check processing-order records, launcher-only sorting, trusted original reads, and retained discovery/output protections.
