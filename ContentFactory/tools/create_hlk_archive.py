"""Write deterministic HLK-compatible archives from ordered in-memory payloads."""

from __future__ import annotations

import os
import struct
import tempfile
from pathlib import Path

from factory_contracts import CONTENT_TYPES

HEADER = struct.Struct("<II")
ENTRY = struct.Struct("<III")
VERSION = 1
VALID_TYPES = set(CONTENT_TYPES.values())
UINT32_MAX = (1 << 32) - 1


# Round an offset up to the requested payload alignment.
def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


# Write checked payload bytes directly, without a temporary entry list or payload files.
def create_archive(entries: list[tuple[int, bytes]], output: Path, alignment: int) -> None:
    if type(alignment) is not int or alignment <= 0 or alignment & (alignment - 1):
        raise ValueError("alignment must be a positive power of two")
    if not entries:
        raise ValueError("archive must contain at least one entry")
    offset = align_up(HEADER.size + len(entries) * ENTRY.size, alignment)
    table = []
    for content_type, payload in entries:
        if type(content_type) is not int or content_type not in VALID_TYPES:
            raise ValueError(f"invalid archive content type: {content_type!r}")
        if not isinstance(payload, bytes):
            raise ValueError("archive payload must be bytes")
        size = len(payload)
        if offset > UINT32_MAX or size > UINT32_MAX or offset + size > UINT32_MAX:
            raise ValueError("archive exceeds the HLK format's 32-bit offset/size limit")
        table.append((content_type, offset, size))
        offset = align_up(offset + size, alignment)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{output.name}.", suffix=".tmp", dir=output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with temporary.open("wb") as stream:
            stream.write(HEADER.pack(VERSION, len(table)))
            for entry in table:
                stream.write(ENTRY.pack(*entry))
            stream.write(b"\0" * (table[0][1] - stream.tell()))
            for (_, payload), (_, entry_offset, size) in zip(entries, table):
                if stream.tell() != entry_offset:
                    raise RuntimeError("internal archive offset calculation mismatch")
                stream.write(payload)
                padding = align_up(entry_offset + size, alignment) - stream.tell()
                if padding:
                    stream.write(b"\0" * padding)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
