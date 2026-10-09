#!/usr/bin/env python3
"""Create a deterministic DirectStorage HLK-compatible content archive.

Input is a JSON manifest with an ``entries`` array. Each entry supplies a
``path`` and ``content_type`` (0 unknown, 1 texture, 2 geometry, 3 text).
Paths are resolved relative to the manifest unless ``--source-root`` is set.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import tempfile
from pathlib import Path

from factory_contracts import CONTENT_TYPES as CONTENT_TYPE_NAMES

# The header stores version and count; the table stores type, offset, and size.
HEADER = struct.Struct("<II")
ENTRY = struct.Struct("<III")
VERSION = 1
VALID_TYPES = set(CONTENT_TYPE_NAMES.values())
UINT32_MAX = (1 << 32) - 1





def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def load_entries(manifest: Path, source_root: Path | None) -> list[tuple[int, Path]]:
    document = json.loads(manifest.read_text(encoding="utf-8"))
    # Input order determines archive entry order.
    raw_entries = document.get("entries")
    if not isinstance(raw_entries, list) or not raw_entries:
        raise ValueError("manifest must contain a non-empty entries array")

    # Resolve all payload paths relative to one root and keep them inside it.
    root = source_root.resolve() if source_root else manifest.parent.resolve()
    entries: list[tuple[int, Path]] = []
    seen: set[Path] = set()
    for index, item in enumerate(raw_entries):
        if not isinstance(item, dict):
            raise ValueError(f"entry {index} is not an object")
        # Accept friendly type names as well as their numeric archive values.
        content_type = item.get("content_type")
        if isinstance(content_type, str):
            content_type = CONTENT_TYPE_NAMES.get(content_type.lower())
        if content_type not in VALID_TYPES:
            raise ValueError(f"entry {index} has invalid content_type {content_type!r}")
        raw_path = item.get("path")
        if not isinstance(raw_path, str) or not raw_path:
            raise ValueError(f"entry {index} has no path")
        # Reject paths outside the root, duplicate sources, and missing files.
        source = (root / raw_path).resolve()
        try:
            source.relative_to(root)
        except ValueError as error:
            raise ValueError(f"entry {index} escapes source root: {raw_path}") from error
        if source in seen:
            raise ValueError(f"entry {index} duplicates source path: {raw_path}")
        if not source.is_file():
            raise FileNotFoundError(source)
        seen.add(source)
        entries.append((content_type, source))
    return entries


def create_archive(manifest: Path, output: Path, source_root: Path | None, alignment: int) -> None:
    if alignment <= 0 or alignment & (alignment - 1):
        raise ValueError("alignment must be a positive power of two")

    inputs = load_entries(manifest, source_root)
    # Calculate payload offsets before writing any archive bytes.
    table_end = HEADER.size + len(inputs) * ENTRY.size
    offset = align_up(table_end, alignment)
    table: list[tuple[int, int, int]] = []

    for content_type, source in inputs:
        size = source.stat().st_size
        # The archive format has 32-bit offsets and sizes; reject anything that will not fit.
        if size > UINT32_MAX or offset > UINT32_MAX or offset + size > UINT32_MAX:
            raise ValueError("archive exceeds the HLK format's 32-bit offset/size limit")
        table.append((content_type, offset, size))
        offset = align_up(offset + size, alignment)

    output.parent.mkdir(parents=True, exist_ok=True)
    # Write to a sibling temporary file so readers do not see an unfinished archive.
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("wb") as stream:
            # Write the header and table first, then pad to the first payload.
            stream.write(HEADER.pack(VERSION, len(table)))
            for entry in table:
                stream.write(ENTRY.pack(*entry))
            stream.write(b"\0" * (table[0][1] - stream.tell()))
            # Copy payloads in order and check that sizes stayed stable while reading.
            for (_, source), (_, entry_offset, size) in zip(inputs, table):
                if stream.tell() != entry_offset:
                    raise RuntimeError("internal archive offset calculation mismatch")
                with source.open("rb") as input_stream:
                    while chunk := input_stream.read(1024 * 1024):
                        stream.write(chunk)
                if stream.tell() != entry_offset + size:
                    raise RuntimeError(f"source size changed while reading: {source}")
                # Fill alignment gaps with zero bytes for repeatable output.
                padding = align_up(stream.tell(), alignment) - stream.tell()
                if padding:
                    stream.write(b"\0" * padding)
        # Publish only the completed archive; clean up the temporary file on failure.
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)

    print(f"Created {output} with {len(table)} entries ({output.stat().st_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--alignment", type=int, default=1)
    args = parser.parse_args()
    create_archive(args.manifest.resolve(), args.output.resolve(), args.source_root, args.alignment)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
