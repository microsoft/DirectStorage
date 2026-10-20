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

Repeated root validation by callers is unchanged. This is not a concurrent-filesystem security boundary: originals and directory structure must remain stable during processing.

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
