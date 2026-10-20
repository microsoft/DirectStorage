"""Read and write DirectStorage HLK content archives.

Writing accepts ordered in-memory payloads. Parsing reopens the stored file and
checks its recorded entry ranges; callers compare those ranges with expected bytes.


Ordered list of payload bytes
    ↓
Calculate their positions inside the archive
    ↓
Write the header and entry table
    ↓
Write payloads with alignment padding
    ↓
Replace the destination with the completed archive


Layout of the archive:
┌────────────────────────────────────┐
│ Header                             │ 8 bytes
│   Version                          │
│   Number of entries                │
├────────────────────────────────────┤
│ Entry table                        │ 12 bytes per entry
│   Content type                     │
│   Payload offset                   │
│   Payload size                     │
├────────────────────────────────────┤
│ Optional zero padding              │
├────────────────────────────────────┤
│ Payload 0                          │
├────────────────────────────────────┤
│ Zero padding                       │
├────────────────────────────────────┤
│ Payload 1                          │
├────────────────────────────────────┤
│ Zero padding                       │
└────────────────────────────────────┘
"""

from __future__ import annotations

import os
import struct
import tempfile
from dataclasses import dataclass
from pathlib import Path

from factory_contracts import CONTENT_TYPES

HEADER = struct.Struct(
    "<"  # Little-endian, without alignment padding.
    "I"  # version:        uint32
    "I"  # entry_count:    uint32
)
ENTRY = struct.Struct(
    "<"  # Little-endian, without alignment padding.
    "I"  # content_type:   uint32
    "I"  # payload_offset: uint32, measured from the start of the archive
    "I"  # payload_size:   uint32, excluding alignment padding
)
VERSION = 1
VALID_TYPES = set(CONTENT_TYPES.values())
UINT32_MAX = (1 << 32) - 1


@dataclass(frozen=True)
# Keep each table entry's position and byte range for structure and payload checks.
class ArchiveEntry:
    index: int
    content_type: int
    offset: int
    size: int


# Round a byte position upward to the next permitted boundry.
def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


# This is the complete archive-writing operation.
# Entries is an ordered list of (content_type, payload_bytes). All expected to be in memory
# Output is the destination archive path
# Alignment is the boundry on which each payload begins
def create_archive(entries: list[tuple[int, bytes]], output: Path, alignment: int) -> None:

    # Check alignment (must be 1 or power of 2)
    if type(alignment) is not int or alignment <= 0 or alignment & (alignment - 1):
        raise ValueError("alignment must be a positive power of two")
    # Check entries, we require at least one
    if not entries:
        raise ValueError("archive must contain at least one entry")

    # Find where the first payload can begin, based on alignment
    offset = align_up(HEADER.size + len(entries) * ENTRY.size, alignment)
    table = []

    # Calculate the complete entry table
    for content_type, payload in entries:
        # Ensure it's a valid content type and require bytes
        if type(content_type) is not int or content_type not in VALID_TYPES:
            raise ValueError(f"invalid archive content type: {content_type!r}")
        if not isinstance(payload, bytes):
            raise ValueError("archive payload must be bytes")

        # Calculate length and check positions/sizes
        size = len(payload)
        if offset > UINT32_MAX or size > UINT32_MAX or offset + size > UINT32_MAX:
            raise ValueError("archive exceeds the HLK format's 32-bit offset/size limit")

        # Record position and advance to next aligned position
        table.append((content_type, offset, size))
        offset = align_up(offset + size, alignment)

    # Create temp sibling file and try to write the header + entry table
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("wb") as stream:
            stream.write(HEADER.pack(VERSION, len(table)))  # Header
            for entry in table:
                stream.write(ENTRY.pack(*entry))            # Each entry table
            stream.write(b"\0" * (table[0][1] - stream.tell())) # Pad to the first payload

            # Write each payload with its padding
            for (_, payload), (_, entry_offset, size) in zip(entries, table):
                if stream.tell() != entry_offset:
                    raise RuntimeError("internal archive offset calculation mismatch")
                stream.write(payload)
                padding = align_up(entry_offset + size, alignment) - stream.tell()
                if padding:
                    stream.write(b"\0" * padding)
        # Replace the completed archive
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


# Validate the archive header and ordered payload ranges and return its entry table.
# Returns (entries, file_size). The returned entries contain locations, not bytes
def parse_archive(path: Path) -> tuple[list[ArchiveEntry], int]:
    
    # Measure and open file
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        
        # Validate the header before trusting its entry count.
        raw_header = stream.read(HEADER.size)
        if len(raw_header) != HEADER.size:
            raise ValueError("archive header is truncated")
        # Check version
        version, count = HEADER.unpack(raw_header)
        if version != VERSION:
            raise ValueError(f"unsupported archive version {version}")

        # Check that the complete table fits before beginning
        table_end = HEADER.size + count * ENTRY.size
        if table_end > file_size:
            raise ValueError("entry table extends beyond the archive")

        # Establish the earliest valid payload position
        entries: list[ArchiveEntry] = []
        # Require payloads to stay ordered and avoid overlap with the table or each other.
        previous_end = table_end
        
        # Read each entry. For each we check content type and byte ranges
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

            # Save entry, advance boundary 
            entries.append(ArchiveEntry(index, content_type, offset, size))
            previous_end = offset + size

    return entries, file_size
