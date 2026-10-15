"""Direct in-memory archive writing and independent HLK structure/payload checks."""

import struct
import tempfile
import unittest
from pathlib import Path

import pytest

from conftest import load_tool_module

writer = load_tool_module("direct_archive_writer", "create_hlk_archive.py")
reader = load_tool_module("direct_archive_reader", "validate_hlk_archive.py")


# Verify archive bytes independently of writer bookkeeping.
class ArchiveTests(unittest.TestCase):
    # Create a disposable output path without payload or JSON entry-list files.
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    # Check direct payload bytes, table content types, offsets, padding, and deterministic output.
    def test_direct_write_parse_and_payload_bytes(self):
        entries = [(0, b"texture"), (2, b"geometry"), (1, b"text")]
        first = self.root / "first.bin"
        second = self.root / "second.bin"
        writer.create_archive(entries, first, 4096)
        writer.create_archive(entries, second, 4096)
        parsed, file_size = reader.parse_archive(first)
        data = first.read_bytes()
        self.assertEqual(data, second.read_bytes())
        self.assertEqual(file_size, len(data))
        self.assertEqual(struct.unpack_from("<II", data), (1, 3))
        self.assertEqual([entry.content_type for entry in parsed], [0, 2, 1])
        for entry, (_, payload) in zip(parsed, entries):
            self.assertEqual(entry.offset % 4096, 0)
            self.assertEqual(entry.size, len(payload))
            self.assertEqual(data[entry.offset:entry.offset + entry.size], payload)
        self.assertFalse(list(self.root.glob("*.json")))
        self.assertEqual({p.name for p in self.root.iterdir()}, {"first.bin", "second.bin"})

    # Check empty payloads retain table entries without byte corruption or overlap.
    def test_empty_payload_is_valid(self):
        path = self.root / "archive.bin"
        writer.create_archive([(3, b""), (1, b"text")], path, 1)
        entries, _ = reader.parse_archive(path)
        self.assertEqual([entry.size for entry in entries], [0, 4])
        self.assertEqual(path.read_bytes()[entries[1].offset:], b"text")

    # Check writer failure leaves an existing destination unchanged and removes temporary files.
    def test_invalid_input_does_not_modify_existing_output(self):
        output = self.root / "archive.bin"
        output.write_bytes(b"existing")
        for entries, alignment in [([], 1), ([(1, b"text")], 3), ([(99, b"text")], 1), ([(True, b"text")], 1), ([(1, "not bytes")], 1)]:
            with self.assertRaises(ValueError):
                writer.create_archive(entries, output, alignment)
            self.assertEqual(output.read_bytes(), b"existing")
        self.assertEqual([p.name for p in self.root.iterdir()], ["archive.bin"])


# Verify required structure checks on arbitrary stored archives independently of generation.
@pytest.mark.parametrize("data, message", [
    (b"short", "header"),
    (struct.pack("<II", 2, 0), "version"),
    (struct.pack("<II", 1, 1), "entry table"),
    (struct.pack("<IIIII", 1, 1, 7, 20, 0), "content type"),
    (struct.pack("<IIIII", 1, 1, 0, 19, 0), "table"),
    (struct.pack("<IIIII", 1, 1, 0, 20, 1), "beyond"),
    (struct.pack("<IIIIIIII", 1, 2, 0, 32, 2, 1, 33, 1) + b"ab", "overlaps"),
])
def test_reader_rejects_malformed_archives(tmp_path: Path, data: bytes, message: str) -> None:
    path = tmp_path / "malformed.bin"
    path.write_bytes(data)
    with pytest.raises(ValueError, match=message):
        reader.parse_archive(path)


# Check the binary format's 32-bit offset limit is enforced before any output write.
def test_writer_rejects_offset_overflow(tmp_path: Path) -> None:
    output = tmp_path / "archive.bin"
    with pytest.raises(ValueError, match="32-bit"):
        writer.create_archive([(3, b"data")], output, 1 << 32)
    assert not output.exists()


# Keep archive writing/reading as internal functions rather than duplicate public commands.
def test_archive_modules_have_no_command_entrypoints() -> None:
    assert not hasattr(writer, "main") and not hasattr(reader, "main")
    assert not hasattr(writer, "load_entries")
