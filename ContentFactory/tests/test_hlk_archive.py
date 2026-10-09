"""Writer and independent-reader regression tests for the HLK archive format."""

from pathlib import Path
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest


FACTORY_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = FACTORY_ROOT.parent
CREATOR = FACTORY_ROOT / "tools" / "create_hlk_archive.py"
VALIDATOR = FACTORY_ROOT / "tools" / "validate_hlk_archive.py"
HEADER = struct.Struct("<II")
ENTRY = struct.Struct("<III")

# Load the independent reader for table checks without its removed report/extract CLI.
reader_spec = importlib.util.spec_from_file_location("hlk_reader_for_tests", VALIDATOR)
assert reader_spec and reader_spec.loader
reader = importlib.util.module_from_spec(reader_spec)
sys.modules[reader_spec.name] = reader
reader_spec.loader.exec_module(reader)


# Check archive creation, stable output, and writer input rejection.
class HlkArchiveCreatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.sources = self.root / "sources"
        self.sources.mkdir()
        (self.sources / "texture.bin").write_bytes(b"texture-data")
        (self.sources / "geometry.bin").write_bytes(b"geometry-data")

    def tearDown(self):
        self.temp.cleanup()

    def run_script(self, script: Path, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(script), *map(str, args)], capture_output=True, text=True, check=False)

    def write_manifest(self, entries: list[dict[str, object]]) -> Path:
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps({"entries": entries}), encoding="utf-8")
        return manifest

    def test_create_validate_and_check_payloads(self):
        # Validate through the CLI, then inspect bytes directly; no extraction files are created.
        manifest = self.write_manifest([
            {"path": "texture.bin", "content_type": "texture"},
            {"path": "geometry.bin", "content_type": "geometry"},
        ])
        archive = self.root / "content.bin"
        first = self.run_script(CREATOR, manifest, archive, "--source-root", self.sources, "--alignment", 16)
        self.assertEqual(first.returncode, 0, first.stderr)
        first_bytes = archive.read_bytes()
        validation = self.run_script(VALIDATOR, archive)
        self.assertEqual(validation.returncode, 0, validation.stderr)
        entries, file_size = reader.parse_archive(archive)
        self.assertEqual(file_size, len(first_bytes))
        for entry, expected in zip(entries, (b"texture-data", b"geometry-data")):
            self.assertEqual(entry.offset % 16, 0)
            self.assertEqual(first_bytes[entry.offset:entry.offset + entry.size], expected)
        second = self.run_script(CREATOR, manifest, archive, "--source-root", self.sources, "--alignment", 16)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first_bytes, archive.read_bytes())

    def test_rejects_path_traversal(self):
        outside = self.root / "outside.bin"
        outside.write_bytes(b"outside")
        manifest = self.write_manifest([{"path": "../outside.bin", "content_type": 1}])
        result = self.run_script(CREATOR, manifest, self.root / "content.bin", "--source-root", self.sources)
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_duplicate_sources_and_invalid_types(self):
        duplicate = self.write_manifest([
            {"path": "texture.bin", "content_type": 1},
            {"path": "texture.bin", "content_type": 1},
        ])
        self.assertNotEqual(self.run_script(CREATOR, duplicate, self.root / "duplicate.bin", "--source-root", self.sources).returncode, 0)

        invalid = self.write_manifest([{"path": "texture.bin", "content_type": 99}])
        self.assertNotEqual(self.run_script(CREATOR, invalid, self.root / "invalid.bin", "--source-root", self.sources).returncode, 0)

    def test_rejects_non_power_of_two_alignment(self):
        manifest = self.write_manifest([{"path": "texture.bin", "content_type": 1}])
        result = self.run_script(CREATOR, manifest, self.root / "content.bin", "--source-root", self.sources, "--alignment", 3)
        self.assertNotEqual(result.returncode, 0)


# Check the independent reader with both valid and malformed archives.
class HlkArchiveValidatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_validator(self, archive: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VALIDATOR), str(archive), *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def make_archive(self, entries: list[tuple[int, bytes]]) -> Path:
        archive = self.root / "content.bin"
        offset = HEADER.size + len(entries) * ENTRY.size
        table = []
        payload = bytearray()
        for content_type, data in entries:
            table.append(ENTRY.pack(content_type, offset, len(data)))
            payload.extend(data)
            offset += len(data)
        archive.write_bytes(HEADER.pack(1, len(entries)) + b"".join(table) + payload)
        return archive

    def test_valid_archive_structure_and_payloads(self):
        archive = self.make_archive([(1, b"texture"), (2, b"geometry"), (3, b"text")])
        result = self.run_validator(archive)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries, file_size = reader.parse_archive(archive)
        data = archive.read_bytes()
        self.assertEqual(len(entries), 3)
        self.assertEqual(file_size, len(data))
        self.assertEqual(entries[0].offset, HEADER.size + 3 * ENTRY.size)
        for entry, expected in zip(entries, (b"texture", b"geometry", b"text")):
            self.assertEqual(entry.size, len(expected))
            self.assertEqual(data[entry.offset:entry.offset + entry.size], expected)

    def test_removed_extraction_and_report_options_are_rejected(self):
        # The validator remains read-only and must not accept removed output options.
        archive = self.make_archive([(1, b"texture")])
        destination = self.root / "removed-output"
        for option in ("--extract", "--report"):
            self.assertNotEqual(self.run_validator(archive, option, str(destination)).returncode, 0)
            self.assertFalse(destination.exists())

    def test_rejects_truncated_table(self):
        archive = self.root / "truncated-table.bin"
        archive.write_bytes(HEADER.pack(1, 2) + ENTRY.pack(1, 32, 1))
        result = self.run_validator(archive)
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_out_of_bounds_payload(self):
        archive = self.root / "bad-bounds.bin"
        archive.write_bytes(HEADER.pack(1, 1) + ENTRY.pack(1, 20, 100) + b"x")
        result = self.run_validator(archive)
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_overlapping_entries(self):
        archive = self.root / "overlap.bin"
        table_end = HEADER.size + 2 * ENTRY.size
        archive.write_bytes(
            HEADER.pack(1, 2)
            + ENTRY.pack(1, table_end, 4)
            + ENTRY.pack(2, table_end + 2, 4)
            + b"abcdef"
        )
        result = self.run_validator(archive)
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_invalid_version_and_type(self):
        invalid_version = self.root / "bad-version.bin"
        invalid_version.write_bytes(HEADER.pack(2, 0))
        self.assertNotEqual(self.run_validator(invalid_version).returncode, 0)

        invalid_type = self.root / "bad-type.bin"
        table_end = HEADER.size + ENTRY.size
        invalid_type.write_bytes(HEADER.pack(1, 1) + ENTRY.pack(99, table_end, 1) + b"x")
        self.assertNotEqual(self.run_validator(invalid_type).returncode, 0)


if __name__ == "__main__":
    unittest.main()
