"""Codec stage contracts: Zstd, GDeflate, and GACL; real native tests stay separate."""

from __future__ import annotations

import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from conftest import assert_report_files, create_content_set, load_tool_module, make_python_tool

import pytest

FACTORY_ROOT = Path(__file__).resolve().parents[1]
TOOLS = FACTORY_ROOT / "tools"
zstd = load_tool_module("zstd_stage_tests", "zstd_compress.py")
gdeflate = load_tool_module("gdeflate_stage_tests", "gdeflate_compress.py")


# ZSTD derivative stage.
class ZstdCompressTests(unittest.TestCase):
    processor = TOOLS / "zstd_compress.py"

    # Create two original files and a fake launcher without an initial report.
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        source_root = self.root
        (source_root / "nested").mkdir(parents=True)
        (source_root / "small.bin").write_bytes(bytes(range(251)) * 3)
        (source_root / "nested" / "large.bin").write_bytes(bytes(range(256)) * 400)
        self.fake_zstd = self.root / "generated" / "fake-zstd.cmd"
        self.fake_zstd.parent.mkdir(parents=True, exist_ok=True)
        self.fake_zstd.write_text(
            f'@echo off\n"{sys.executable}" "{Path(__file__).with_name("fake_zstd.py")}" %*\n',
            encoding="utf-8", newline="\r\n")

    # Exercise the internal Zstd stage with resolved settings rather than a removed CLI.
    def run_processor(self, *args: object, executable=None) -> subprocess.CompletedProcess[str]:
        options = list(args)
        blocks = chunks = None
        matrix = "--zstd-shader-matrix" in options
        for flag, target in [("--block-sizes-kb", "blocks"), ("--chunk-sizes-kb", "chunks")]:
            if flag in options:
                start = options.index(flag) + 1
                end = next((index for index in range(start, len(options)) if str(options[index]).startswith("--")), len(options))
                values = [int(value) for value in options[start:end]]
                if target == "blocks": blocks = values
                else: chunks = values
        try:
            blocks, chunks = zstd.resolve_sizes(matrix, blocks, chunks)
            path = zstd.process(self.root, executable or self.fake_zstd, blocks, chunks, "--overwrite" in options)
            return subprocess.CompletedProcess(options, 0, f"Wrote {path}\n", "")
        except (OSError, RuntimeError, ValueError) as error:
            return subprocess.CompletedProcess(options, 1, "", str(error))

    # Check default Zstd output parameters, independently calculated hashes, and generation metadata.
    def test_default_produces_one_configuration_and_records_verified_metadata(self):
        result = self.run_processor()
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "generated" / "manifest.json").read_text(encoding="utf-8"))
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
        assert_report_files(self.root.resolve(), manifest)

    # Check generated compression metadata and deterministic replacement of an unreadable old report.
    def test_compression_level_metadata_round_trip_and_report_independence(self):
        self.assertEqual(self.run_processor().returncode, 0)
        report_path = self.root / "generated" / "manifest.json"
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual({r["parameters"]["compression_level"] for r in report["derivatives"]}, {19})
        assert_report_files(self.root, report)
        report_path.write_bytes(b"not json and not a source inventory")
        result = self.run_processor("--overwrite")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report, json.loads(report_path.read_text(encoding="utf-8")))
        self.assertTrue(result.stdout.startswith("Wrote "))
        self.assertNotIn("Verified", result.stdout)

    # Check that the shader-matrix flag generates all nine size pairs for each source.
    def test_full_matrix_requires_explicit_shader_flag(self):
        result = self.run_processor("--zstd-shader-matrix")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "generated" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(len(manifest["derivatives"]), 18)
        self.assertEqual({r["parameters"]["compression_level"] for r in manifest["derivatives"]}, {19})
        self.assertEqual(
            {(r["parameters"]["block_size_kb"], r["parameters"]["chunk_size_kb"])
             for r in manifest["derivatives"]},
            {(block, chunk) for block in (4, 8, 16) for chunk in (64, 128, 256)},
        )

    # Check that shader-matrix selection rejects explicit size options.
    def test_shader_matrix_rejects_explicit_sizes(self):
        result = self.run_processor("--zstd-shader-matrix", "--block-sizes-kb", 4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be combined", result.stderr)

    # Check that chunked Zstd records report the expected frame count per source.
    def test_chunking_records_frame_count(self):
        result = self.run_processor("--block-sizes-kb", 4, "--chunk-sizes-kb", 64)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "generated" / "manifest.json").read_text(encoding="utf-8"))
        by_source = {record["source"]: record for record in manifest["derivatives"]}
        self.assertEqual(by_source["small.bin"]["metadata"]["frame_count"], 1)
        self.assertEqual(by_source["nested/large.bin"]["metadata"]["frame_count"], 2)

    # Check overwrite refusal and byte-identical Zstd regeneration when overwrite is enabled.
    def test_refuses_existing_outputs_without_overwrite_and_is_deterministic(self):
        self.assertEqual(self.run_processor().returncode, 0)
        manifest_path = self.root / "generated" / "manifest.json"
        first_manifest = manifest_path.read_bytes()
        first_hashes = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "generated" / "zstd").rglob("*.zst")
        }
        result = self.run_processor()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("use --overwrite", result.stderr)
        self.assertEqual(self.run_processor("--overwrite").returncode, 0)
        self.assertEqual(first_manifest, manifest_path.read_bytes())
        second_hashes = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "generated" / "zstd").rglob("*.zst")
        }
        self.assertEqual(first_hashes, second_hashes)

    # Check that a failing Zstd command leaves the prior manifest and output unchanged.
    def test_failed_compression_preserves_existing_outputs_and_manifest(self):
        self.assertEqual(self.run_processor().returncode, 0)
        manifest_path = self.root / "generated" / "manifest.json"
        before_manifest = manifest_path.read_bytes()
        output = next((self.root / "generated" / "zstd").rglob("*.zst"))
        before_output = output.read_bytes()
        bad = self.root / "generated" / "bad-zstd.cmd"
        bad.write_text("@echo off\r\nexit /b 9\r\n", encoding="utf-8")
        result = self.run_processor("--overwrite", executable=bad)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before_manifest, manifest_path.read_bytes())
        self.assertEqual(before_output, output.read_bytes())

    # Check rejection of duplicate or invalid sizes, invalid matrices, and empty sources.
    def test_rejects_empty_sources_and_invalid_size_lists(self):
        module = load_tool_module("zstd_compress", self.processor)
        with self.assertRaises(ValueError):
            module.validate_sizes([4, 4], "block sizes")
        with self.assertRaises(ValueError):
            module.validate_sizes([0], "chunk sizes")
        with self.assertRaises(ValueError):
            module.validate_matrix((128,), (64,))
        empty = self.root / "empty.bin"
        empty.write_bytes(b"")
        result = self.run_processor("--block-sizes-kb", 4, "--chunk-sizes-kb", 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be empty", result.stderr)


# GDEFLATE derivative stage.
class GDeflateCompressTests(unittest.TestCase):
    processor = TOOLS / "gdeflate_compress.py"

    # Require and locate the configured GDeflate executable for this test class.
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("GDEFLATE_CONTENT_TOOL")
        if not value:
            raise RuntimeError("Set GDEFLATE_CONTENT_TOOL to the built executable")
        cls.tool = Path(value).resolve()
        if not cls.tool.is_file():
            raise FileNotFoundError(cls.tool)

    # Create two GDeflate original files without inventory or report setup.
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        sources = self.root
        (sources / "nested").mkdir(parents=True)
        (sources / "a.bin").write_bytes(b"gdeflate" * 1000)
        (sources / "nested" / "b.bin").write_bytes(bytes(range(256)) * 100)

    # Exercise the internal GDeflate stage with an explicitly resolved level selection.
    def run_processor(self, *args: object, executable=None) -> subprocess.CompletedProcess[str]:
        options = list(args)
        try:
            if "--level" in options:
                raise ValueError("unrecognized arguments: --level")
            levels = None
            if "--levels" in options:
                start = options.index("--levels") + 1
                end = next((index for index in range(start, len(options)) if str(options[index]).startswith("--")), len(options))
                levels = [int(value) for value in options[start:end]]
            selected = gdeflate.resolve_levels(levels, "--all-levels" in options)
            path = gdeflate.process(self.root, executable or self.tool, selected, "--overwrite" in options)
            return subprocess.CompletedProcess(options, 0, f"Wrote {path}\n", "")
        except (OSError, RuntimeError, ValueError) as error:
            return subprocess.CompletedProcess(options, 1, "", str(error))

    # Check default level-9 outputs, metadata, hashes, and native and manifest verification.
    def test_generates_one_default_level_and_updates_manifest(self):
        result = self.run_processor()
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads((self.root / "generated" / "manifest.json").read_text(encoding="utf-8"))
        records = [item for item in manifest["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 2)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, {9})
        self.assertEqual(manifest["tools"]["GDeflateContentTool"]["version"], "1.0.0")
        for record in records:
            output = self.root / Path(record["path"])
            self.assertTrue(output.is_file())
            self.assertEqual(record["size"], output.stat().st_size)
            self.assertEqual(record["sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
        assert_report_files(self.root.resolve(), manifest)

    # Check level-6 overwrite refusal and byte-identical regeneration.
    def test_selected_level_is_deterministic_and_requires_overwrite(self):
        command = ("--levels", 6)
        self.assertEqual(self.run_processor(*command).returncode, 0)
        manifest_path = self.root / "generated" / "manifest.json"
        before_manifest = manifest_path.read_bytes()
        before = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "generated" / "gdeflate").rglob("*.gdeflate")
        }
        refused = self.run_processor(*command)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("use --overwrite", refused.stderr)
        self.assertEqual(self.run_processor(*command, "--overwrite").returncode, 0)
        self.assertEqual(before_manifest, manifest_path.read_bytes())
        after = {
            path.relative_to(self.root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (self.root / "generated" / "gdeflate").rglob("*.gdeflate")
        }
        self.assertEqual(before, after)

    # Check sorted records for an explicit level list and deterministic overwrite results.
    def test_explicit_list_is_sorted_and_deterministic(self):
        # Two sources times three selected levels yield six recorded outputs.
        command = ("--levels", 9, 1, 6)
        result = self.run_processor(*command)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest_path = self.root / "generated" / "manifest.json"
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        records = [item for item in manifest_data["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 6)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, {1, 6, 9})
        self.assertEqual([item["path"] for item in records], sorted(item["path"] for item in records))
        before = manifest_path.read_bytes()
        result = self.run_processor(*command, "--overwrite")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(before, manifest_path.read_bytes())

    # Check all-level generation and replacement with a single selected level.
    def test_explicit_all_levels_and_replacement(self):
        # Request all levels intentionally, then check a later overwrite replaces the selection.
        result = self.run_processor("--all-levels")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest_path = self.root / "generated" / "manifest.json"
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        records = [item for item in manifest_data["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 24)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, set(range(1, 13)))
        result = self.run_processor("--levels", 6, "--overwrite")
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest_data = json.loads(manifest_path.read_text(encoding="utf-8"))
        records = [item for item in manifest_data["derivatives"] if item["format"] == "gdeflate"]
        self.assertEqual(len(records), 2)
        self.assertEqual({item["parameters"]["compression_level"] for item in records}, {6})
        self.assertEqual(len(list((self.root / "generated" / "gdeflate").rglob("*.gdeflate"))), 2)

    # Check that failed GDeflate generation preserves the previous manifest and output.
    def test_failed_generation_preserves_existing_state(self):
        self.assertEqual(self.run_processor("--levels", 1).returncode, 0)
        manifest_path = self.root / "generated" / "manifest.json"
        before_manifest = manifest_path.read_bytes()
        output = next((self.root / "generated" / "gdeflate").rglob("*.gdeflate"))
        before_output = output.read_bytes()
        bad = self.root / "generated" / "bad-tool.cmd"
        bad.write_text("@echo off\r\nif \"%1\"==\"--version\" (echo BadTool 1.0.0 & exit /b 0)\r\nexit /b 7\r\n")
        result = self.run_processor("--levels", 1, "--overwrite", executable=bad)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before_manifest, manifest_path.read_bytes())
        self.assertEqual(before_output, output.read_bytes())

    # Check rejection of invalid level selections, the removed option, and empty input.
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
        empty = self.root / "empty.bin"
        empty.write_bytes(b"")
        result = self.run_processor("--levels", 1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be empty", result.stderr)


# GACL DDS parsing and conditioning stage.
gacl = load_tool_module("gacl_compress", "gacl_compress.py")
process_set = load_tool_module("process_set_for_gacl", "content_report.py")


# Write a deterministic 8-by-8 legacy or DX10 DDS fixture and return its bytes.
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


# Create a launcher and fake GACL tool that wraps, verifies, and reports transforms.
def make_fake_tool(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
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


# Create a DDS source without inventory or report setup.
def initialize_set(root: Path) -> Path:
    source = root / "image.dds"
    make_dds(source)
    return source


# Check first-mip metadata for legacy BC1 and DX10 BC5 DDS inputs.
def test_parse_legacy_and_dx10_dds(tmp_path: Path) -> None:
    legacy = make_dds(tmp_path / "legacy.dds")
    info = gacl.parse_dds_first_mip(legacy)
    assert (info.format, info.format_name, info.width, info.height, info.data_size) == ("BC1", "DXT1", 8, 8, 32)

    dx10 = make_dds(tmp_path / "dx10.dds", dxgi=83)
    info = gacl.parse_dds_first_mip(dx10)
    assert (info.format, info.format_name, info.array_size, info.data_offset, info.data_size) == (
        "BC5", "DXGI_FORMAT_BC5_UNORM", 1, 148, 64
    )


# Check that DDS parsing records all mip levels but selects only the first mip.
def test_multi_mip_dds_extracts_only_first_mip(tmp_path: Path) -> None:
    path = tmp_path / "mips.dds"
    data = bytearray(make_dds(path))
    struct.pack_into("<I", data, 28, 4)
    data.extend(b"lower-mips")
    info = gacl.parse_dds_first_mip(bytes(data))
    assert info.mip_count == 4
    assert info.data_offset == 128
    assert info.data_size == 32


# Check that DX10 array parsing records the array size and selects item zero.
def test_dx10_array_records_array_size_but_extracts_item_zero(tmp_path: Path) -> None:
    path = tmp_path / "array.dds"
    data = bytearray(make_dds(path, dxgi=83))
    struct.pack_into("<I", data, 140, 3)
    info = gacl.parse_dds_first_mip(bytes(data))
    assert info.array_size == 3
    assert info.data_offset == 148
    assert info.data_size == 64


# Check that DX10 BC7 input is recognized with the expected first-mip size.
def test_bc7_dx10_uses_production_supported_zstd_transform(tmp_path: Path) -> None:
    data = make_dds(tmp_path / "bc7.dds", dxgi=98)
    info = gacl.parse_dds_first_mip(data)
    assert (info.format, info.format_name, info.data_size) == ("BC7", "DXGI_FORMAT_BC7_UNORM", 64)


# Check GACL output parameters, container metadata, tool version, and file verification.
def test_process_adds_verified_manifest_record(tmp_path: Path) -> None:
    initialize_set(tmp_path)
    tool = tmp_path / "generated" / "fake_gacl.cmd"
    make_fake_tool(tool)
    manifest_path = gacl.process(tmp_path, tool, 19, 65536, False)
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
    assert_report_files(tmp_path, manifest)


# Check GACL overwrite refusal and replacement with updated compression parameters.
def test_existing_outputs_require_overwrite(tmp_path: Path) -> None:
    initialize_set(tmp_path)
    tool = tmp_path / "generated" / "fake_gacl.cmd"
    make_fake_tool(tool)
    gacl.process(tmp_path, tool, 19, 65536, False)
    with pytest.raises(FileExistsError, match="--overwrite"):
        gacl.process(tmp_path, tool, 19, 65536, False)
    gacl.process(tmp_path, tool, 13, 32768, True)
    manifest = json.loads((process_set.manifest_path(tmp_path)).read_text(encoding="utf-8"))
    record = next(item for item in manifest["derivatives"] if item["format"] == "gacl")
    assert record["parameters"]["zstd_level"] == 13
    assert record["parameters"]["target_block_size"] == 32768


# Check that BC7 processing records the Zstd-only transform and DDS format.
def test_bc7_process_adds_zstd_only_transform_record(tmp_path: Path) -> None:
    source = tmp_path / "image.dds"
    make_dds(source, dxgi=98)
    tool = tmp_path / "generated" / "fake_gacl.cmd"
    make_fake_tool(tool)
    manifest_path = gacl.process(tmp_path, tool, 19, 65536, False)
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    record = manifest_data["derivatives"][0]
    assert record["parameters"]["texture_format"] == "BC7"
    assert record["parameters"]["transform_id"] == 7
    assert record["parameters"]["transform_name"] == "GACL_SHUFFLE_TRANSFORM_ZSTD_ONLY"
    assert record["metadata"]["dds_format"] == "DXGI_FORMAT_BC7_UNORM"
    assert_report_files(tmp_path, manifest_data)


# Check that GACL processing rejects an input directory without supported DDS sources.
def test_non_dds_input_directory_is_rejected(tmp_path: Path) -> None:
    source = tmp_path / "blob.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b"data")
    tool = tmp_path / "generated" / "fake_gacl.cmd"
    make_fake_tool(tool)
    with pytest.raises(ValueError, match="no supported"):
        gacl.process(tmp_path, tool, 19, 65536, False)


# Check codec output paths preserve nested source directories beneath generated without a set name.
@pytest.mark.parametrize("source", ["input.bin", "nested/input.bin"])
def test_codec_output_paths_are_input_relative(source: str) -> None:
    source_path = Path(source)
    assert zstd.output_relative_path(source, 16, 256) == (
        Path("generated/zstd") / source_path.parent / f"{source_path.name}-blk16K-chunk256K.zst"
    ).as_posix()
    assert gdeflate.output_relative_path(source, 9) == (
        Path("generated/gdeflate") / source_path.parent / f"{source_path.name}-level9.gdeflate"
    ).as_posix()
    assert gacl.output_relative_path(source) == (
        Path("generated/gacl") / source_path.parent / f"{source_path.name}.gacl"
    ).as_posix()


# Keep encoding modules internal rather than maintaining duplicate command parsers.
@pytest.mark.parametrize("stage", [zstd, gdeflate, gacl])
def test_encoding_stage_has_no_command_entrypoint(stage) -> None:
    assert not hasattr(stage, "main")
    assert not hasattr(stage, "argparse")


# Check overwrite accepts a changed Zstd executable version and records the encoder actually used.
def test_zstd_overwrite_records_changed_version(tmp_path: Path) -> None:
    source = tmp_path / "input.bin"
    content = bytes(range(251)) * 3
    source.write_bytes(content)
    manifest_path = process_set.manifest_path(tmp_path)
    fake_source = Path(__file__).with_name("fake_zstd.py").read_text(encoding="utf-8")
    assert fake_source.count('print("fake-zstd 1.0")') == 1
    generated = tmp_path / "generated"
    generated.mkdir()
    encoder_script = generated / "versioned-zstd.py"
    encoder_script.write_text(fake_source, encoding="utf-8")
    encoder = generated / "versioned-zstd.cmd"
    encoder.write_text(
        f'@echo off\n"{sys.executable}" "{encoder_script}" %*\n',
        encoding="utf-8", newline="\r\n",
    )
    assert zstd.process(tmp_path, encoder, (16,), (256,), False) == manifest_path
    first = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert first["tools"]["zstd"]["version"] == "1.0"
    output = tmp_path / first["derivatives"][0]["path"]
    first_manifest = manifest_path.read_bytes()
    first_output = output.read_bytes()
    encoder_script.write_text(fake_source.replace('print("fake-zstd 1.0")', 'print("fake-zstd 2.0")'), encoding="utf-8")
    with pytest.raises(FileExistsError, match="--overwrite"):
        zstd.process(tmp_path, encoder, (16,), (256,), False)
    assert manifest_path.read_bytes() == first_manifest
    assert output.read_bytes() == first_output
    assert zstd.process(tmp_path, encoder, (16,), (256,), True) == manifest_path
    updated = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert updated["tools"]["zstd"]["version"] == "2.0"
    assert updated["sources"] == first["sources"]
    assert updated["derivatives"] == first["derivatives"]
    assert output.read_bytes() == first_output
    assert source.read_bytes() == content
    assert_report_files(tmp_path, updated)
    assert not (generated / ".factory.lock").exists()
    assert not list(generated.glob(".zstd.stage.*"))
    assert not list(generated.glob(".zstd.backup.*"))


# Check standalone generation rehashes changed originals and ignores any missing, corrupt, or malicious old report.
@pytest.mark.parametrize("codec", ["zstd", "gdeflate", "gacl"])
@pytest.mark.parametrize("old_report", ["missing", "corrupt", "malicious"])
def test_standalone_generation_uses_only_fresh_sources(tmp_path: Path, codec: str, old_report: str) -> None:
    input_directory = tmp_path / "original inputs"
    input_directory.mkdir()
    source = input_directory / ("image.dds" if codec == "gacl" else "input.bin")
    original = make_dds(source) if codec == "gacl" else b"original bytes"
    source.write_bytes(original)
    if codec == "zstd":
        module = zstd
        tool = make_python_tool(input_directory, "fake_zstd.py", "fresh-zstd")
        options = ((16,), (256,))
    elif codec == "gdeflate":
        module = gdeflate
        tool = make_python_tool(input_directory, "fake_gdeflate.py", "fresh-gdeflate")
        options = ((9,),)
    else:
        module = gacl
        tool = input_directory / "generated" / "fresh-gacl.cmd"
        make_fake_tool(tool)
        options = (19, 65536)
    path = module.process(input_directory, tool, *options, False)
    first = json.loads(path.read_text(encoding="utf-8"))
    assert_report_files(input_directory, first)
    output = input_directory / first["derivatives"][0]["path"]
    output.write_bytes(b"corrupt previous stage output")
    changed = original[:-1] + bytes([(original[-1] + 1) % 256])
    source.write_bytes(changed)
    outside = tmp_path / "outside.bin"
    outside.write_bytes(b"must not be consumed, overwritten or deleted")
    if old_report == "missing":
        path.unlink()
    elif old_report == "corrupt":
        path.write_bytes(b"not a JSON report")
    else:
        path.write_text(json.dumps({"sources": [{"path": "../outside.bin"}],
                                   "derivatives": [{"path": "../outside.bin", "format": codec}],
                                   "archives": [], "tools": {"unselected": {"version": "stale"}}}), encoding="utf-8")
    before_report = path.read_bytes() if path.exists() else None
    with pytest.raises(FileExistsError, match="--overwrite"):
        module.process(input_directory, tool, *options, False)
    assert output.read_bytes() == b"corrupt previous stage output"
    assert (path.read_bytes() if path.exists() else None) == before_report
    assert module.process(input_directory, tool, *options, True) == path
    current = json.loads(path.read_text(encoding="utf-8"))
    assert current["sources"][0]["path"] == source.name
    assert current["sources"][0]["sha256"] == hashlib.sha256(changed).hexdigest()
    assert current["sources"] != first["sources"]
    assert {record["format"] for record in current["derivatives"]} == {codec}
    assert current["archives"] == []
    assert set(current["tools"]) == {"content_report.py", {"zstd": "zstd", "gdeflate": "GDeflateContentTool", "gacl": "GACLContentTool"}[codec]}
    assert source.read_bytes() == changed
    assert outside.read_bytes() == b"must not be consumed, overwritten or deleted"
    assert_report_files(input_directory, current)


# Check codecs finish temporary generation before deleting existing outputs and preserve state on build failure.
@pytest.mark.parametrize("codec", ["zstd", "gdeflate", "gacl"])
def test_codec_build_failure_preserves_old_output_and_report(tmp_path: Path, monkeypatch, codec: str) -> None:
    create_content_set(tmp_path, {"source.bin": b"original"})
    generated = tmp_path / "generated"
    final = generated / codec
    final.mkdir(parents=True)
    (final / "marker.bin").write_bytes(b"old generation")
    path = generated / "manifest.json"
    path.write_bytes(b"old report bytes")
    tool = generated / "unused-tool.exe"
    tool.write_bytes(b"never executed")
    module = load_tool_module("build_failure_" + codec, codec + "_compress.py")
    assert module.content_report is process_set
    monkeypatch.setattr(module, "zstd_version" if codec == "zstd" else "tool_version", lambda *args: "1.0.0")
    shared = process_set.create_manifest(tmp_path)
    before = json.loads(json.dumps(shared))
    stages = []

    # Confirm the old generation remains available throughout a failed staged build.
    def fail_build(input_directory, report, stage, *args):
        stages.append(stage)
        assert report is shared
        assert (final / "marker.bin").read_bytes() == b"old generation"
        assert path.read_bytes() == b"old report bytes"
        assert not (generated / ".factory.lock").exists()
        assert not list(generated.glob("*.backup.*"))
        (stage / "partial.bin").write_bytes(b"partial replacement")
        raise RuntimeError("injected build failure")

    monkeypatch.setattr(module, "build_derivatives" if codec == "gdeflate" else "build_variants", fail_build)
    options = ((16,), (256,)) if codec == "zstd" else ((9,),) if codec == "gdeflate" else (19, 65536)
    with pytest.raises(RuntimeError, match="injected build failure"):
        module.process(tmp_path, tool, *options, True, report=shared)
    assert shared == before
    assert path.read_bytes() == b"old report bytes"
    assert (final / "marker.bin").read_bytes() == b"old generation"
    assert len(stages) == 1 and not stages[0].exists()
    assert not (generated / ".factory.lock").exists()
    assert not list(generated.glob(f".{codec}.backup.*"))


# Check installation/report failures need no rollback and a subsequent overwrite repairs codec output and report.
@pytest.mark.parametrize("codec", ["zstd", "gdeflate", "gacl"])
@pytest.mark.parametrize("failure", ["install", "report_before", "report_after"])
def test_codec_publication_failure_and_overwrite_recovery(tmp_path: Path, monkeypatch, codec: str, failure: str) -> None:
    create_content_set(tmp_path, {"source.bin": b"original"})
    generated = tmp_path / "generated"
    final = generated / codec
    final.mkdir(parents=True)
    (final / "marker.bin").write_bytes(b"old generation")
    path = generated / "manifest.json"
    path.write_bytes(b"old report bytes")
    tool = generated / "unused-tool.exe"
    tool.write_bytes(b"never executed")
    module = load_tool_module("publication_" + codec, codec + "_compress.py")
    monkeypatch.setattr(module, "zstd_version" if codec == "zstd" else "tool_version", lambda *args: "1.0.0")
    shared = process_set.create_manifest(tmp_path)
    derivatives = shared["derivatives"]
    tools = shared["tools"]
    record = {"path": f"generated/{codec}/marker.bin", "format": codec}
    stages = []

    # Build a complete replacement without codecs while proving old output survives the build.
    def build(input_directory, report, stage, *args):
        stages.append(stage)
        assert report is shared
        assert not (generated / ".factory.lock").exists()
        assert not list(generated.glob(f".{codec}.backup.*"))
        (stage / "marker.bin").write_bytes(b"new generation")
        return [record]

    monkeypatch.setattr(module, "build_derivatives" if codec == "gdeflate" else "build_variants", build)
    original_replace = module.os.replace
    original_write = process_set.write_json_atomic
    triggered = []

    # Fail only final codec-directory installation after the existing directory has been deleted.
    def fail_install(source, destination):
        if Path(destination) == final:
            triggered.append(failure)
            assert not final.exists()
            assert (Path(source) / "marker.bin").read_bytes() == b"new generation"
            raise OSError("injected publication failure")
        return original_replace(source, destination)

    # Fail before or after atomic report publication without restoring output or shared report snapshots.
    def fail_write(report_path, report):
        triggered.append(failure)
        assert report is shared
        assert (final / "marker.bin").read_bytes() == b"new generation"
        assert report["derivatives"] is derivatives
        assert report["tools"] is tools
        if failure == "report_after":
            original_write(report_path, report)
        raise OSError("injected publication failure")

    options = ((16,), (256,)) if codec == "zstd" else ((9,),) if codec == "gdeflate" else (19, 65536)
    with monkeypatch.context() as publication:
        if failure == "install":
            publication.setattr(module.os, "replace", fail_install)
        else:
            publication.setattr(process_set, "write_json_atomic", fail_write)
        with pytest.raises(OSError, match="injected publication failure"):
            module.process(tmp_path, tool, *options, True, report=shared)
    assert triggered == [failure]
    if failure == "install":
        assert not final.exists()
        assert derivatives == []
    else:
        assert (final / "marker.bin").read_bytes() == b"new generation"
        assert derivatives == [record]
    if failure == "report_after":
        assert json.loads(path.read_text(encoding="utf-8")) == shared
    else:
        assert path.read_bytes() == b"old report bytes"
    assert all(not stage.exists() for stage in stages)
    assert not (generated / ".factory.lock").exists()
    assert not list(generated.glob(f".{codec}.backup.*"))
    recovered = process_set.create_manifest(tmp_path)
    shared = recovered
    assert module.process(tmp_path, tool, *options, True, report=recovered) == path
    assert (final / "marker.bin").read_bytes() == b"new generation"
    assert json.loads(path.read_text(encoding="utf-8")) == recovered
    assert recovered["derivatives"] == [record]
    assert (tmp_path / "source.bin").read_bytes() == b"original"
    assert all(not stage.exists() for stage in stages)


# Check successful codec publication appends directly to existing shared containers and removes stale codec files.
@pytest.mark.parametrize("codec", ["zstd", "gdeflate", "gacl"])
def test_codec_success_appends_shared_report_in_place(tmp_path: Path, monkeypatch, codec: str) -> None:
    create_content_set(tmp_path, {"source.bin": b"original"})
    generated = tmp_path / "generated"
    final = generated / codec
    final.mkdir(parents=True)
    (final / "stale.bin").write_bytes(b"obsolete output")
    tool = generated / "unused-tool.exe"
    tool.write_bytes(b"never executed")
    module = load_tool_module("shared_append_" + codec, codec + "_compress.py")
    monkeypatch.setattr(module, "zstd_version" if codec == "zstd" else "tool_version", lambda *args: "1.0.0")
    shared = process_set.create_manifest(tmp_path)
    previous = {"path": "generated/earlier/output.bin", "format": "earlier"}
    archive = {"path": "generated/hlk/earlier", "format": "none"}
    shared["derivatives"].append(previous)
    shared["archives"].append(archive)
    shared["tools"]["earlier.py"] = {"version": "old"}
    derivatives, archives, tools = shared["derivatives"], shared["archives"], shared["tools"]
    record = {"path": f"generated/{codec}/marker.bin", "format": codec}
    stages = []

    # Return current stage records while old codec output remains present until generation finishes.
    def build(input_directory, report, stage, *args):
        assert report is shared
        assert (final / "stale.bin").read_bytes() == b"obsolete output"
        stages.append(stage)
        (stage / "marker.bin").write_bytes(b"complete replacement")
        return [record]

    monkeypatch.setattr(module, "build_derivatives" if codec == "gdeflate" else "build_variants", build)
    options = ((16,), (256,)) if codec == "zstd" else ((9,),) if codec == "gdeflate" else (19, 65536)
    path = module.process(tmp_path, tool, *options, True, report=shared)
    assert shared["derivatives"] is derivatives and shared["archives"] is archives and shared["tools"] is tools
    assert derivatives == [previous, record]
    assert derivatives[0] is previous and derivatives[1] is record
    assert archives == [archive] and archives[0] is archive
    assert tools["earlier.py"] == {"version": "old"}
    tool_name = {"zstd": "zstd", "gdeflate": "GDeflateContentTool", "gacl": "GACLContentTool"}[codec]
    assert tools[tool_name] == {"version": "1.0.0"}
    assert json.loads(path.read_text(encoding="utf-8")) == shared
    assert sorted(item.name for item in final.iterdir()) == ["marker.bin"]
    assert all(not stage.exists() for stage in stages)
    assert not (generated / ".factory.lock").exists()
    assert not list(generated.glob(f".{codec}.backup.*"))


# Check first-generation report failure can retain completed codec output, and standalone overwrite can recover.
@pytest.mark.parametrize("codec", ["zstd", "gdeflate", "gacl"])
def test_first_generation_report_failure_leaves_recoverable_outputs(tmp_path: Path, monkeypatch, codec: str) -> None:
    create_content_set(tmp_path, {"source.bin": b"original"})
    generated = tmp_path / "generated"
    generated.mkdir()
    tool = generated / "unused-tool.exe"
    tool.write_bytes(b"never executed")
    module = load_tool_module("first_report_failure_" + codec, codec + "_compress.py")
    monkeypatch.setattr(module, "zstd_version" if codec == "zstd" else "tool_version", lambda *args: "1.0.0")
    record = {"path": f"generated/{codec}/marker.bin", "format": codec}
    stages = []

    # Produce a replacement in the temporary directory without running a real codec.
    def build(input_directory, report, stage, *args):
        stages.append(stage)
        (stage / "marker.bin").write_bytes(b"completed generation")
        return [record]

    # Fail after output installation while observing the newly built standalone report.
    def fail_report(path, report):
        assert report["derivatives"] == [record]
        assert (generated / codec / "marker.bin").read_bytes() == b"completed generation"
        raise OSError("injected first report failure")

    monkeypatch.setattr(module, "build_derivatives" if codec == "gdeflate" else "build_variants", build)
    options = ((16,), (256,)) if codec == "zstd" else ((9,),) if codec == "gdeflate" else (19, 65536)
    with monkeypatch.context() as publication:
        publication.setattr(process_set, "write_json_atomic", fail_report)
        with pytest.raises(OSError, match="injected first report failure"):
            module.process(tmp_path, tool, *options, False)
    assert not (generated / "manifest.json").exists()
    assert (generated / codec / "marker.bin").read_bytes() == b"completed generation"
    assert all(not stage.exists() for stage in stages)
    path = module.process(tmp_path, tool, *options, True)
    assert json.loads(path.read_text(encoding="utf-8"))["derivatives"] == [record]
    assert (tmp_path / "source.bin").read_bytes() == b"original"
    assert not (generated / ".factory.lock").exists()
    assert not list(generated.glob(f".{codec}.backup.*"))


# Check GDeflate compresses once per derivative without a second Python verification invocation.
def test_gdeflate_generation_does_not_repeat_native_verification(tmp_path: Path, monkeypatch) -> None:
    (tmp_path / "source.bin").write_bytes(b"original")
    tool = tmp_path / "generated" / "unused-tool.exe"
    tool.parent.mkdir()
    tool.write_bytes(b"never executed")
    monkeypatch.setattr(gdeflate, "tool_version", lambda *args: "1.0.0")
    calls = []

    # Emulate native compression writing a valid envelope; all verification switches are forbidden here.
    def compress(arguments):
        calls.append(arguments)
        assert "--verify" not in arguments
        source = Path(arguments[arguments.index("--input") + 1])
        output = Path(arguments[arguments.index("--output") + 1])
        level = int(arguments[arguments.index("--level") + 1])
        data = source.read_bytes()
        output.write_bytes(struct.pack("<IHHIIQQ", 0x31464447, 1, 32, level, 0, len(data), len(data)) + data)

    monkeypatch.setattr(gdeflate, "run_tool", compress)
    path = gdeflate.process(tmp_path, tool, (1, 9), False)
    assert len(calls) == 2
    assert {record["parameters"]["compression_level"] for record in json.loads(path.read_text())["derivatives"]} == {1, 9}


# Check GACL still explicitly verifies each written first-mip output using the native verification mode.
def test_gacl_generation_retains_native_verification_call(tmp_path: Path, monkeypatch) -> None:
    source = tmp_path / "image.dds"
    make_dds(source)
    tool = tmp_path / "generated" / "unused-tool.exe"
    tool.parent.mkdir()
    tool.write_bytes(b"never executed")
    monkeypatch.setattr(gacl, "tool_version", lambda *args: "1.0.0")
    calls = []

    # Emulate GACL output creation and capture the independent verification request without a codec.
    def run(arguments):
        calls.append(arguments)
        if "--output" in arguments:
            Path(arguments[arguments.index("--output") + 1]).write_bytes(b"encoded first mip")

    monkeypatch.setattr(gacl, "run_tool", run)
    gacl.process(tmp_path, tool, 19, 65536, False)
    assert len(calls) == 2
    assert calls[1][1] == "--verify"
    assert calls[1][calls[1].index("--format") + 1] == "BC1"
    assert Path(calls[1][calls[1].index("--original") + 1]).suffix == ".payload"
    assert not list((tmp_path / "generated" / "gacl").rglob("*.payload"))


# Check standalone stages report only their own invocation rather than merging prior codec history.
def test_standalone_report_does_not_merge_prior_codec_history(tmp_path: Path) -> None:
    create_content_set(tmp_path, {"source.bin": b"original"})
    zstd_tool = make_python_tool(tmp_path, "fake_zstd.py", "single-zstd")
    gdeflate_tool = make_python_tool(tmp_path, "fake_gdeflate.py", "single-gdeflate")
    path = zstd.process(tmp_path, zstd_tool, (16,), (256,), False)
    old = json.loads(path.read_text(encoding="utf-8"))
    zstd_output = tmp_path / old["derivatives"][0]["path"]
    old_bytes = zstd_output.read_bytes()
    assert gdeflate.process(tmp_path, gdeflate_tool, (9,), False) == path
    report = json.loads(path.read_text(encoding="utf-8"))
    assert {record["format"] for record in report["derivatives"]} == {"gdeflate"}
    assert set(report["tools"]) == {"content_report.py", "GDeflateContentTool"}
    assert report["archives"] == []
    assert zstd_output.read_bytes() == old_bytes
    assert_report_files(tmp_path, report)


if __name__ == "__main__":
    unittest.main()
