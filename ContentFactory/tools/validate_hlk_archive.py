#!/usr/bin/env python3
"""Validate the structure of a DirectStorage HLK content archive.

Archive layout:
8-byte header (<version, entry count>) followed by 12-byte entries
(<content type, payload offset, payload size>) and payload bytes.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

from factory_contracts import CONTENT_TYPES as CONTENT_TYPE_NAMES

# Read the on-disk layout independently of the archive writer.
HEADER = struct.Struct("<II")
ENTRY = struct.Struct("<III")
VERSION = 1
VALID_TYPES = set(CONTENT_TYPE_NAMES.values())


@dataclass(frozen=True)
# Keep each table entry's position and byte range for structure and payload checks.
class ArchiveEntry:
    index: int
    content_type: int
    offset: int
    size: int


def parse_archive(path: Path) -> tuple[list[ArchiveEntry], int]:
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        # Validate the header before trusting its entry count.
        raw_header = stream.read(HEADER.size)
        if len(raw_header) != HEADER.size:
            raise ValueError("archive header is truncated")
        version, count = HEADER.unpack(raw_header)
        if version != VERSION:
            raise ValueError(f"unsupported archive version {version}")

        # The complete table must fit before any payload can begin.
        table_end = HEADER.size + count * ENTRY.size
        if table_end > file_size:
            raise ValueError("entry table extends beyond the archive")

        entries: list[ArchiveEntry] = []
        # Require payloads to stay ordered and avoid overlap with the table or each other.
        previous_end = table_end
        for index in range(count):
            raw_entry = stream.read(ENTRY.size)
            if len(raw_entry) != ENTRY.size:
                raise ValueError(f"entry {index} is truncated")
            content_type, offset, size = ENTRY.unpack(raw_entry)
            if content_type not in VALID_TYPES:
                raise ValueError(f"entry {index} has invalid content type {content_type}")
            if offset < previous_end:
                raise ValueError(f"entry {index} overlaps the archive header, table, or previous entry")
            # Every recorded byte range must end inside the actual archive file.
            if offset + size > file_size:
                raise ValueError(f"entry {index} extends beyond the archive")
            entries.append(ArchiveEntry(index, content_type, offset, size))
            previous_end = offset + size

    return entries, file_size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()

    # Read-only validation does not extract payloads or write reports.
    entries, file_size = parse_archive(args.archive)
    print(f"Validated {len(entries)} entries in {args.archive} ({file_size} bytes)")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
