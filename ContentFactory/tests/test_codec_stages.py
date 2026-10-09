"""Codec stage contracts: Zstd, GDeflate, and GACL; real native tests stay separate."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import pytest

FACTORY_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = FACTORY_ROOT.parent
TOOLS = FACTORY_ROOT / "tools"
DRIVER = TOOLS / "process-set.py"


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def load(name: str, filename: str):
    return load_module(TOOLS / filename, name)


# ZSTD derivative stage.
class ZstdCompressTests(unittest.TestCase):
    processor = TOOLS / "zstd_compress.py"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        source_root = self.root / "originals" / "sample_set"
        (source_root / "nested").mkdir(parents=True)
        (source_root / "small.bin").write_bytes(bytes(range(251)) * 3)
        (source_root / "nested" / "large.bin").write_bytes(bytes(range(256)) * 400)
        self.fake_zstd = self.root / "fake-zstd.cmd"
        self.fake_zstd.write_text(
            f'@echo off\n"{sys.executable}" "{Path(__file__).with_name("fake_zstd.py")}" %*\n',
            encoding="utf-8", newline="\r\n")
        result = subprocess.run(
            [sys.executable, str(DRIVER), "sample_set", "--root", str(self.root)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def tearDown(self):
        self.temp.cleanup()

    def run_processor(self, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.processor), "sample_set", "--root", str(self.root),
             "--zstd-exe", str(self.fake_zstd), *map(str, args)],
            capture_output=True, text=True, check=False,
        )

    def test_default_produces_one_configuration_and_records_verified_metadata(self):
        result = self.run_processor()
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "manifests" / "sample_set.json").read_text(encoding="utf-8"))
        records = manifest["derivatives"]
        self.assertEqual(len(records), 2)
        self.assertEqual([record["path"] for record in records], sorted(record["path"] for record in records))
        self.assertEqual(manifest["tools"]["zstd"]["version"], "1.0")
        self.assertEqual(
            {(r["parameters"]["block_size_kb"], r["parameters"]["chunk_size_kb"]) for r in records},
            {(16, 256)},
        )
        for record in records:
            output = self.root / Path(record["path"])
            self.assertTrue(output.is_file())
            self.assertEqual(record["size"], output.stat().st_size)
            self.assertEqual(record["sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(record["validation"], "pass")
            self.assertEqual(record["parameters"]["compression_level"], 19)
        verify = subprocess.run(
            [sys.executable, str(DRIVER), "sample_set", "--root", str(self.root), "--verify"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(verify.returncode, 0, verify.stderr)

    def test_compression_level_metadata_round_trip_and_legacy_compatibility(self):
        # New manifests retain the level; legacy records remain readable without guessing it.
        self.assertEqual(self.run_processor().returncode, 0)
        manifest_path = self.root / "manifests" / "sample_set.json"
        module = load_module(self.processor, "zstd_metadata_tests")
        driver = module.load_driver()
        document = driver.load_manifest(manifest_path)
        self.assertEqual({r["parameters"]["compression_level"] for r in document["derivatives"]}, {19})
        driver.write_json_atomic(manifest_path, document)
        self.assertEqual(document, driver.load_manifest(manifest_path))
        import jsonschema
        schema = json.loads((FACTORY_ROOT / "tools" / "content-set-manifest.schema.json").read_text())
        jsonschema.validate(document, schema)
        for invalid in (True, 0, 23, "19"):
            document["derivatives"][0]["parameters"]["compression_level"] = invalid
            with self.assertRaisesRegex(ValueError, "Zstd compression level"):
                driver.validate_manifest(document)
        document["derivatives"][0]["parameters"]["compression_level"] = 19
        for record in document["derivatives"]:
            del record["parameters"]["compression_level"]
        driver.validate_manifest(document)
        driver.write_json_atomic(manifest_path, document)
        driver.verify_files(self.root.resolve(), "sample_set", driver.load_manifest(manifest_path))
        self.assertEqual(self.run_processor("--overwrite").returncode, 0)
        self.assertEqual({r["parameters"]["compression_level"] for r in driver.load_manifest(manifest_path)["derivatives"]}, {19})

    def test_full_matrix_requires_explicit_shader_flag(self):
        result = self.run_processor("--zstd-shader-matrix")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "manifests" / "sample_set.json").read_text(encoding="utf-8"))
        self.assertEqual(len(manifest["derivatives"]), 18)
        self.assertEqual({r["parameters"]["compression_level"] for r in manifest["derivatives"]}, {19})
        self.assertEqual(
            {(r["parameters"]["block_size_kb"], r["parameters"]["chunk_size_kb"])
             for r in manifest["derivatives"]},
            {(block, chunk) for block in (4, 8, 16) for chunk in (64, 128, 256)},
        )

    def test_shader_matrix_rejects_explicit_sizes(self):
        result = self.run_processor("--zstd-shader-matrix", "--block-sizes-kb", 4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be combined", result.stderr)

    def test_chunking_records_frame_count(self):
        result = self.run_processor("--block-sizes-kb", 4, "--chunk-sizes-kb", 64)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "manifests" / "sample_set.json").read_text(encoding="utf-8"))
        by_source = {record["source"]: record for record in manifest["derivatives"]}
        self.assertEqual(by_source["small.bin"]["metadata"]["frame_count"], 1)
        self.assertEqual(by_source["nested/large.bin"]["metadata"]["frame_count"], 2)

    def test_refuses_existing_outputs_without_overwrite_and_is_deterministic(self):
        self.assertEqual(self.run_processor().returncode, 0)
        manifest_path = self.root / "manifests" / "sample_set.json"
        first_manifest = manifest_path.read_bytes()
        first_hashes = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "zstd" / "sample_set").rglob("*.zst")
        }
        result = self.run_processor()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("use --overwrite", result.stderr)
        self.assertEqual(self.run_processor("--overwrite").returncode, 0)
        self.assertEqual(first_manifest, manifest_path.read_bytes())
        second_hashes = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "zstd" / "sample_set").rglob("*.zst")
        }
        self.assertEqual(first_hashes, second_hashes)

    def test_failed_compression_preserves_existing_outputs_and_manifest(self):
        self.assertEqual(self.run_processor().returncode, 0)
        manifest_path = self.root / "manifests" / "sample_set.json"
        before_manifest = manifest_path.read_bytes()
        output = next((self.root / "zstd" / "sample_set").rglob("*.zst"))
        before_output = output.read_bytes()
        bad = self.root / "bad-zstd.cmd"
        bad.write_text("@echo off\r\nexit /b 9\r\n", encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(self.processor), "sample_set", "--root", str(self.root),
             "--zstd-exe", str(bad), "--overwrite"],
            capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before_manifest, manifest_path.read_bytes())
        self.assertEqual(before_output, output.read_bytes())

    def test_rejects_empty_sources_and_invalid_size_lists(self):
        module = load_module(self.processor, "zstd_compress")
        with self.assertRaises(ValueError):
            module.validate_sizes([4, 4], "block sizes")
        with self.assertRaises(ValueError):
            module.validate_sizes([0], "chunk sizes")
        with self.assertRaises(ValueError):
            module.validate_matrix((128,), (64,))
        empty = self.root / "originals" / "sample_set" / "empty.bin"
        empty.write_bytes(b"")
        subprocess.run(
            [sys.executable, str(DRIVER), "sample_set", "--root", str(self.root), "--overwrite"],
            capture_output=True, text=True, check=False,
        )
        result = self.run_processor("--block-sizes-kb", 4, "--chunk-sizes-kb", 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be empty", result.stderr)


# GDEFLATE derivative stage.
class GDeflateCompressTests(unittest.TestCase):
    processor = TOOLS / "gdeflate_compress.py"

    @classmethod
    def setUpClass(cls):
        value = os.environ.get("GDEFLATE_CONTENT_TOOL")
        if not value:
            raise RuntimeError("Set GDEFLATE_CONTENT_TOOL to the built executable")
        cls.tool = Path(value).resolve()
        if not cls.tool.is_file():
            raise FileNotFoundError(cls.tool)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        sources = self.root / "originals" / "sample_set"
        (sources / "nested").mkdir(parents=True)
        (sources / "a.bin").write_bytes(b"gdeflate" * 1000)
        (sources / "nested" / "b.bin").write_bytes(bytes(range(256)) * 100)
        result = subprocess.run(
            [sys.executable, str(DRIVER), "sample_set", "--root", str(self.root)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def tearDown(self):
        self.temp.cleanup()

    def run_processor(self, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.processor), "sample_set", "--root", str(self.root),
             "--gdeflate-exe", str(self.tool), *map(str, args)],
            capture_output=True, text=True, check=False,
        )

    def test_generates_one_default_level_and_updates_manifest(self):
        result = self.run_processor()
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "manifests" / "sample_set.json").read_text(encoding="utf-8"))
        records = [item for item in manifest["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 2)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, {9})
        self.assertEqual(manifest["tools"]["GDeflateContentTool"]["version"], "1.0.0")
        for record in records:
            output = self.root / Path(record["path"])
            self.assertTrue(output.is_file())
            self.assertEqual(record["size"], output.stat().st_size)
            self.assertEqual(record["sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
            verify = subprocess.run([str(self.tool), "--verify", str(output)], capture_output=True, text=True)
            self.assertEqual(verify.returncode, 0, verify.stderr)
        verify = subprocess.run(
            [sys.executable, str(DRIVER), "sample_set", "--root", str(self.root), "--verify"],
            capture_output=True, text=True,
        )
        self.assertEqual(verify.returncode, 0, verify.stderr)

    def test_selected_level_is_deterministic_and_requires_overwrite(self):
        command = ("--levels", 6)
        self.assertEqual(self.run_processor(*command).returncode, 0)
        manifest_path = self.root / "manifests" / "sample_set.json"
        before_manifest = manifest_path.read_bytes()
        before = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "gdeflate" / "sample_set").rglob("*.gdeflate")
        }
        refused = self.run_processor(*command)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("use --overwrite", refused.stderr)
        self.assertEqual(self.run_processor(*command, "--overwrite").returncode, 0)
        self.assertEqual(before_manifest, manifest_path.read_bytes())
        after = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "gdeflate" / "sample_set").rglob("*.gdeflate")
        }
        self.assertEqual(before, after)

    def test_explicit_list_is_sorted_and_deterministic(self):
        # Two originals times three selected levels yield six recorded outputs.
        command = ("--levels", 9, 1, 6)
        result = self.run_processor(*command)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest_path = self.root / "manifests" / "sample_set.json"
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        records = [item for item in document["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 6)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, {1, 6, 9})
        self.assertEqual([item["path"] for item in records], sorted(item["path"] for item in records))
        before = manifest_path.read_bytes()
        result = self.run_processor(*command, "--overwrite")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(before, manifest_path.read_bytes())

    def test_explicit_all_levels_and_replacement(self):
        # Request all levels intentionally, then check a later overwrite replaces the selection.
        result = self.run_processor("--all-levels")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest_path = self.root / "manifests" / "sample_set.json"
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        records = [item for item in document["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 24)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, set(range(1, 13)))
        result = self.run_processor("--levels", 6, "--overwrite")
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        records = [item for item in document["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 2)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, {6})
        self.assertEqual(len(list((self.root / "gdeflate" / "sample_set").rglob("*.gdeflate"))), 2)

    def test_failed_generation_preserves_existing_state(self):
        self.assertEqual(self.run_processor("--levels", 1).returncode, 0)
        manifest_path = self.root / "manifests" / "sample_set.json"
        before_manifest = manifest_path.read_bytes()
        output = next((self.root / "gdeflate" / "sample_set").rglob("*.gdeflate"))
        before_output = output.read_bytes()
        bad = self.root / "bad-tool.cmd"
        bad.write_text("@echo off\r\nif \"%1\"==\"--version\" (echo BadTool 1.0.0 & exit /b 0)\r\nexit /b 7\r\n")
        result = subprocess.run(
            [sys.executable, str(self.processor), "sample_set", "--root", str(self.root),
             "--gdeflate-exe", str(bad), "--levels", "1", "--overwrite"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before_manifest, manifest_path.read_bytes())
        self.assertEqual(before_output, output.read_bytes())

    def test_rejects_invalid_level_and_empty_source(self):
        for level in (0, 13):
            result = self.run_processor("--levels", level)
            self.assertNotEqual(result.returncode, 0)
        # Lists must be valid and unique; selection modes cannot overlap.
        for options in (("--levels", 0, 9), ("--levels", 1, 1),
                        ("--levels", 1, 9, "--all-levels")):
            self.assertNotEqual(self.run_processor(*options).returncode, 0)
        # A removed singular option must not work as an argparse abbreviation.
        removed = self.run_processor("--level", 9)
        self.assertNotEqual(removed.returncode, 0)
        self.assertIn("unrecognized arguments: --level", removed.stderr)
        empty = self.root / "originals" / "sample_set" / "empty.bin"
        empty.write_bytes(b"")
        subprocess.run(
            [sys.executable, str(DRIVER), "sample_set", "--root", str(self.root), "--overwrite"],
            capture_output=True, text=True,
        )
        result = self.run_processor("--levels", 1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be empty", result.stderr)


# GACL DDS parsing and conditioning stage.
gacl = load("gacl_compress", "gacl_compress.py")
process_set = load("process_set_for_gacl", "process-set.py")


def make_dds(path: Path, *, fourcc: int = 0x31545844, dxgi: int | None = None) -> bytes:
    offset = 148 if dxgi is not None else 128
    bytes_per_block = 8
    if dxgi in (76, 77, 78, 82, 83, 84, 97, 98, 99):
        bytes_per_block = 16
    data = bytearray(offset + bytes_per_block * 4)
    struct.pack_into("<I", data, 0, gacl.DDS_MAGIC)
    struct.pack_into("<I", data, 4, 124)
    struct.pack_into("<I", data, 12, 8)
    struct.pack_into("<I", data, 16, 8)
    struct.pack_into("<I", data, 28, 1)
    struct.pack_into("<I", data, 84, gacl.FOURCC_DX10 if dxgi is not None else fourcc)
    if dxgi is not None:
        struct.pack_into("<IIIII", data, 128, dxgi, 3, 0, 1, 0)
    for index in range(offset, len(data)):
        data[index] = (index * 13 + 7) & 0xFF
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return bytes(data)


def make_fake_tool(path: Path) -> None:
    helper = path.with_suffix(".py")
    helper.write_text(
        """import argparse
from pathlib import Path
p=argparse.ArgumentParser(add_help=False)
p.add_argument('--version', action='store_true')
p.add_argument('--verify', action='store_true')
p.add_argument('--input')
p.add_argument('--output')
p.add_argument('--original')
p.add_argument('--format')
p.add_argument('--zstd-level')
p.add_argument('--target-block-size')
a=p.parse_args()
if a.version:
    print('GACLContentTool 1.0.0')
elif a.verify:
    if Path(a.input).read_bytes() != b'GACL' + Path(a.original).read_bytes():
        raise SystemExit(1)
    print('GACL verification passed')
else:
    Path(a.output).write_bytes(b'GACL' + Path(a.input).read_bytes())
    transform_ids={'BC1':1,'BC3':2,'BC4':3,'BC5':4,'BC7':7}
    print(f'transform_id={transform_ids[a.format]}')
""",
        encoding="utf-8",
    )
    path.write_text(f'@"{sys.executable}" "{helper}" %*\n', encoding="utf-8")


def initialize_set(root: Path, set_name: str = "textures") -> Path:
    source = root / "originals" / set_name / "image.dds"
    make_dds(source)
    manifest = process_set.create_manifest(root, set_name)
    process_set.write_json_atomic(process_set.manifest_path(root, set_name), manifest)
    return source


def test_parse_legacy_and_dx10_dds(tmp_path: Path) -> None:
    legacy = make_dds(tmp_path / "legacy.dds")
    info = gacl.parse_dds_first_mip(legacy)
    assert (info.format, info.format_name, info.width, info.height, info.data_size) == ("BC1", "DXT1", 8, 8, 32)

    dx10 = make_dds(tmp_path / "dx10.dds", dxgi=83)
    info = gacl.parse_dds_first_mip(dx10)
    assert (info.format, info.format_name, info.array_size, info.data_offset, info.data_size) == (
        "BC5", "DXGI_FORMAT_BC5_UNORM", 1, 148, 64
    )


def test_multi_mip_dds_extracts_only_first_mip(tmp_path: Path) -> None:
    path = tmp_path / "mips.dds"
    data = bytearray(make_dds(path))
    struct.pack_into("<I", data, 28, 4)
    data.extend(b"lower-mips")
    info = gacl.parse_dds_first_mip(bytes(data))
    assert info.mip_count == 4
    assert info.data_offset == 128
    assert info.data_size == 32


def test_dx10_array_records_array_size_but_extracts_item_zero(tmp_path: Path) -> None:
    path = tmp_path / "array.dds"
    data = bytearray(make_dds(path, dxgi=83))
    struct.pack_into("<I", data, 140, 3)
    info = gacl.parse_dds_first_mip(bytes(data))
    assert info.array_size == 3
    assert info.data_offset == 148
    assert info.data_size == 64


def test_bc7_dx10_uses_production_supported_zstd_transform(tmp_path: Path) -> None:
    data = make_dds(tmp_path / "bc7.dds", dxgi=98)
    info = gacl.parse_dds_first_mip(data)
    assert (info.format, info.format_name, info.data_size) == ("BC7", "DXGI_FORMAT_BC7_UNORM", 64)


def test_process_adds_verified_manifest_record(tmp_path: Path) -> None:
    initialize_set(tmp_path)
    tool = tmp_path / "fake_gacl.cmd"
    make_fake_tool(tool)
    manifest_path = gacl.process(tmp_path, "textures", tool, 19, 65536, False)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["tools"]["GACLContentTool"]["version"] == "1.0.0"
    assert len(manifest["derivatives"]) == 1
    record = manifest["derivatives"][0]
    assert record["format"] == "gacl"
    assert record["parameters"] == {
        "texture_format": "BC1",
        "zstd_level": 19,
        "target_block_size": 65536,
        "transform_id": 1,
        "transform_name": "GACL_SHUFFLE_TRANSFORM_ZSTD_BC1_224",
        "transform_version": 1,
    }
    assert record["metadata"]["container"] == "raw_zstd_stream"
    assert record["metadata"]["array_item"] == 0
    assert record["metadata"]["mip"] == 0
    output = tmp_path / record["path"]
    assert output.read_bytes().startswith(b"GACL")
    process_set.verify_files(tmp_path, "textures", manifest)


def test_existing_outputs_require_overwrite(tmp_path: Path) -> None:
    initialize_set(tmp_path)
    tool = tmp_path / "fake_gacl.cmd"
    make_fake_tool(tool)
    gacl.process(tmp_path, "textures", tool, 19, 65536, False)
    with pytest.raises(FileExistsError, match="--overwrite"):
        gacl.process(tmp_path, "textures", tool, 19, 65536, False)
    gacl.process(tmp_path, "textures", tool, 13, 32768, True)
    manifest = process_set.load_manifest(process_set.manifest_path(tmp_path, "textures"))
    record = next(item for item in manifest["derivatives"] if item["format"] == "gacl")
    assert record["parameters"]["zstd_level"] == 13
    assert record["parameters"]["target_block_size"] == 32768


def test_bc7_process_adds_zstd_only_transform_record(tmp_path: Path) -> None:
    source = tmp_path / "originals" / "textures" / "image.dds"
    make_dds(source, dxgi=98)
    manifest = process_set.create_manifest(tmp_path, "textures")
    process_set.write_json_atomic(process_set.manifest_path(tmp_path, "textures"), manifest)
    tool = tmp_path / "fake_gacl.cmd"
    make_fake_tool(tool)
    manifest_path = gacl.process(tmp_path, "textures", tool, 19, 65536, False)
    document = process_set.load_manifest(manifest_path)
    record = document["derivatives"][0]
    assert record["parameters"]["texture_format"] == "BC7"
    assert record["parameters"]["transform_id"] == 7
    assert record["parameters"]["transform_name"] == "GACL_SHUFFLE_TRANSFORM_ZSTD_ONLY"
    assert record["metadata"]["dds_format"] == "DXGI_FORMAT_BC7_UNORM"
    process_set.verify_files(tmp_path, "textures", document)


def test_non_dds_set_is_rejected(tmp_path: Path) -> None:
    source = tmp_path / "originals" / "textures" / "blob.bin"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"data")
    manifest = process_set.create_manifest(tmp_path, "textures")
    process_set.write_json_atomic(process_set.manifest_path(tmp_path, "textures"), manifest)
    tool = tmp_path / "fake_gacl.cmd"
    make_fake_tool(tool)
    with pytest.raises(ValueError, match="no supported"):
        gacl.process(tmp_path, "textures", tool, 19, 65536, False)




def test_gacl_cli_defaults_to_shared_level_19(tmp_path: Path, monkeypatch) -> None:
    calls = []
    def fake_process(root, set_name, executable, level, block_size, overwrite):
        calls.append((level, block_size))
        return tmp_path / "manifest.json"
    monkeypatch.setattr(gacl, "process", fake_process)
    monkeypatch.setattr(sys, "argv", ["gacl_compress.py", "sample", "--gacl-exe", "unused-tool.exe"])
    assert gacl.main() == 0
    assert calls == [(19, 65536)]

if __name__ == "__main__":
    unittest.main()
