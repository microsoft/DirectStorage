#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
from argparse import Namespace
from pathlib import Path

import pytest

FACTORY_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = FACTORY_ROOT.parent
TOOLS = FACTORY_ROOT / "tools"


def load(name: str, filename: str):
    # Load the command-line scripts by path for direct calls from these tests.
    spec = importlib.util.spec_from_file_location(name, TOOLS / filename)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


orchestrator = load("run_content_factory", "run-content-factory.py")
process_set = load("process_set_for_orchestration_tests", "process-set.py")


def fake_zstd(root: Path) -> Path:
    # Create a test-only launcher; this is not a real Zstd encoder.
    root.mkdir(parents=True, exist_ok=True)
    # Use a Windows launcher to call the test stand-in with the current Python.
    wrapper = root / "fake-zstd.cmd"
    wrapper.write_text(
        f'@echo off\r\n"{sys.executable}" "{Path(__file__).with_name("fake_zstd.py")}" %*\r\n',
        encoding="utf-8",
    )
    return wrapper


def fake_gdeflate(root: Path) -> Path:
    # Create a test-only launcher for checking orchestration, not compression quality.
    root.mkdir(parents=True, exist_ok=True)
    # Use the same Python for the GDeflate test stand-in.
    wrapper = root / "fake-gdeflate.cmd"
    wrapper.write_text(
        f'@echo off\r\n"{sys.executable}" "{Path(__file__).with_name("fake_gdeflate.py")}" %*\r\n',
        encoding="utf-8",
    )
    return wrapper


def arguments(root: Path, **overrides) -> Namespace:
    # Start with a small test configuration and let each test override what it needs.
    values = {
        "set_name": "sample",
        "root": root,
        "sources": None,
        "stages": ["zstd"],
        "overwrite": False,
        "zstd_exe": fake_zstd(root),
        "zstd_shader_matrix": False,
        "block_sizes_kb": [4],
        "chunk_sizes_kb": [64],
        "allow_version_change": False,
        "gdeflate_exe": None,
        "gdeflate_levels": None,
        "gdeflate_all_levels": False,
        "gacl_exe": None,
        "gacl_zstd_level": 19,
        "gacl_target_block_size": 65536,
        "hlk_archive_prefix": "dstoragetest",
        "hlk_alignment": 1,
    }
    # Keep each test's custom settings in its call rather than copying the option list.
    values.update(overrides)
    return Namespace(**values)


def initialize(root: Path) -> None:
    # Create one source file and its inventory before testing later stages.
    source = root / "originals" / "sample" / "data.bin"
    source.parent.mkdir(parents=True)
    source.write_bytes(bytes(range(251)) * 3)
    document = process_set.create_manifest(root, "sample")
    process_set.write_json_atomic(process_set.manifest_path(root, "sample"), document)


def test_runs_zstd_and_final_verification(tmp_path: Path) -> None:
    # A codec-only run records its derivative without producing a mixed archive.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(tmp_path))
    document = process_set.load_manifest(manifest_path)
    process_set.verify_files(tmp_path, "sample", document)
    assert len(document["derivatives"]) == 1
    assert document["derivatives"][0]["format"] == "zstd"
    assert document["derivatives"][0]["parameters"]["compression_level"] == 19
    assert document["archives"] == []

def test_default_zstd_sizes_produce_one_variant_per_source(tmp_path: Path) -> None:
    # Omit size lists to check that normal runs do not produce nine copies.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["zstd"], block_sizes_kb=None, chunk_sizes_kb=None))
    document = process_set.load_manifest(manifest_path)
    assert len(document["derivatives"]) == 1
    assert document["derivatives"][0]["parameters"]["block_size_kb"] == 16
    assert document["derivatives"][0]["parameters"]["chunk_size_kb"] == 256


def test_shader_matrix_requires_explicit_flag(tmp_path: Path) -> None:
    # Request shader coverage explicitly and expect all nine size combinations.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["zstd"], block_sizes_kb=None, chunk_sizes_kb=None,
        zstd_shader_matrix=True))
    document = process_set.load_manifest(manifest_path)
    assert len(document["derivatives"]) == 9


def test_shader_matrix_rejects_custom_sizes(tmp_path: Path) -> None:
    # The shader preset must not silently override custom sizes.
    initialize(tmp_path)
    with pytest.raises(ValueError, match="cannot be combined"):
        orchestrator.run(arguments(tmp_path, stages=["zstd"], zstd_shader_matrix=True))


def test_gacl_is_skipped_for_non_dds_set(tmp_path: Path) -> None:
    # A set with no DDS textures should not need a GACL tool path.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(tmp_path, stages=["zstd", "gacl"], gacl_exe=None))
    document = process_set.load_manifest(manifest_path)
    assert {item["format"] for item in document["derivatives"]} == {"zstd"}


def test_all_requires_gdeflate_tool_before_zstd(tmp_path: Path, monkeypatch) -> None:
    # Make any tool execution fail the test; a missing later tool must be caught first.
    initialize(tmp_path)
    def unexpected_execution(*args, **kwargs):
        pytest.fail("preflight must not execute codec tools")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    with pytest.raises(ValueError, match="--gdeflate-exe"):
        orchestrator.run(arguments(tmp_path, stages=["all"]))
    assert not (tmp_path / "zstd" / "sample").exists()


def test_hlk_stage_creates_lockstep_archives(tmp_path: Path) -> None:
    # Use test codecs to check that the complete three-archive group is recorded.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(
        tmp_path,
        stages=["hlk"],
        gdeflate_exe=fake_gdeflate(tmp_path),
    ))
    document = process_set.load_manifest(manifest_path)
    # The manifest must include all three members of the HLK triplet.
    assert {record["payload_codec"] for record in document["archives"]} == {
        "uncompressed", "gdeflate", "zstd"
    }

def test_duplicate_and_mixed_all_stages_are_rejected(tmp_path: Path) -> None:
    # Reject ambiguous stage selections rather than processing anything twice.
    initialize(tmp_path)
    with pytest.raises(ValueError, match="cannot be combined"):
        orchestrator.run(arguments(tmp_path, stages=["all", "zstd"]))
    with pytest.raises(ValueError, match="duplicates"):
        orchestrator.run(arguments(tmp_path, stages=["zstd", "zstd"]))


def test_missing_manifest_requires_sources(tmp_path: Path) -> None:
    # Do not create an empty set when neither originals nor a manifest were supplied.
    with pytest.raises(FileNotFoundError, match="provide --sources"):
        orchestrator.run(arguments(tmp_path, sources=None, stages=["zstd"]))


def test_sources_initialize_new_set_and_copy_files(tmp_path: Path) -> None:
    # Check that external inputs are copied unchanged into a new set.
    external = tmp_path / "external"
    external.mkdir()
    first = external / "first.bin"
    second = external / "second.bin"
    first.write_bytes(b"first")
    second.write_bytes(b"second")
    manifest_path = orchestrator.run(arguments(
        tmp_path,
        sources=[first, second],
        stages=["zstd"],
    ))
    document = process_set.load_manifest(manifest_path)
    # Check source order and copied bytes independently of processing outputs.
    assert [item["path"] for item in document["sources"]] == ["first.bin", "second.bin"]
    assert (tmp_path / "originals/sample/first.bin").read_bytes() == b"first"
    assert (tmp_path / "originals/sample/second.bin").read_bytes() == b"second"


def test_sources_reject_existing_set_and_duplicate_names(tmp_path: Path) -> None:
    # Protect existing sets and reject filenames that would collide after copying.
    initialize(tmp_path)
    external = tmp_path / "external.bin"
    external.write_bytes(b"external")
    with pytest.raises(FileExistsError, match="new content set"):
        orchestrator.run(arguments(tmp_path, sources=[external], stages=["zstd"]))

    # Different input directories can still contain names that collide after copying.
    other_root = tmp_path / "other"
    a = tmp_path / "a" / "same.bin"
    b = tmp_path / "b" / "same.bin"
    a.parent.mkdir(); b.parent.mkdir(); a.write_bytes(b"a"); b.write_bytes(b"b")
    with pytest.raises(ValueError, match="unique"):
        orchestrator.run(arguments(other_root, sources=[a, b], stages=["zstd"]))



def test_later_missing_tool_is_rejected_before_sources_are_copied(tmp_path: Path, monkeypatch) -> None:
    # A preflight error must leave no copied source tree or generated set manifest.
    source = tmp_path / "source.bin"
    source.write_bytes(b"input")
    root = tmp_path / "outputs"
    root.mkdir()  # Test-only launcher directory; no factory set outputs exist.
    def unexpected_execution(*args, **kwargs):
        pytest.fail("preflight must not execute codec tools")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    options = arguments(root, sources=[source], stages=["zstd", "gdeflate"], gdeflate_exe=None)
    # The test helper creates its fake tool launcher; the orchestrator must not create set outputs.
    with pytest.raises(ValueError, match="--gdeflate-exe"):
        orchestrator.run(options)
    assert not (root / "originals").exists()
    assert not (root / "manifests").exists()
    assert not (root / "zstd").exists()


@pytest.mark.parametrize("options, message", [
    ({"stages": ["zstd", "gdeflate"], "gdeflate_levels": [13]}, "GDeflate level"),
    ({"stages": ["zstd", "gdeflate"], "gdeflate_levels": [1, 1]}, "duplicates"),
    ({"stages": ["zstd", "gdeflate"], "gdeflate_levels": [1, 6], "gdeflate_all_levels": True}, "cannot be combined"),
    ({"stages": ["zstd", "hlk"], "hlk_alignment": 3}, "power of two"),
    ({"stages": ["zstd"], "block_sizes_kb": [128], "chunk_sizes_kb": [64]}, "exceed chunk"),
])
def test_invalid_later_options_do_not_generate_earlier_stage(tmp_path: Path, monkeypatch, options, message) -> None:
    # Bad later-stage options must not change the manifest or create Zstd outputs.
    initialize(tmp_path)
    manifest = tmp_path / "manifests" / "sample.json"
    # Remember the inventory bytes to detect any work performed before rejection.
    before = manifest.read_bytes()
    def unexpected_execution(*args, **kwargs):
        pytest.fail("invalid options must be rejected before tool execution")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    with pytest.raises(ValueError, match=message):
        orchestrator.run(arguments(tmp_path, **options))
    assert manifest.read_bytes() == before
    assert not (tmp_path / "zstd" / "sample").exists()


def test_nonexistent_tool_is_rejected_before_generation(tmp_path: Path, monkeypatch) -> None:
    # A path argument alone is not enough; the tool file must exist.
    initialize(tmp_path)
    def unexpected_execution(*args, **kwargs):
        pytest.fail("preflight must not execute tools")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    with pytest.raises(FileNotFoundError, match="tool executable"):
        orchestrator.run(arguments(tmp_path, stages=["zstd", "gdeflate"],
                                   gdeflate_exe=tmp_path / "missing.exe"))
    assert not (tmp_path / "zstd" / "sample").exists()



def test_mixed_archive_stage_is_no_longer_supported(tmp_path: Path) -> None:
    # Reject the removed stage rather than silently packing derivative files.
    initialize(tmp_path)
    with pytest.raises(ValueError, match="valid Content Factory stages"):
        orchestrator.run(arguments(tmp_path, stages=["archive"]))


def test_gdeflate_stage_produces_one_default_level(tmp_path: Path) -> None:
    # Orchestration defaults to level 9; HLK independently uses the same default preset.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["gdeflate"], gdeflate_exe=fake_gdeflate(tmp_path)))
    document = process_set.load_manifest(manifest_path)
    assert len(document["derivatives"]) == 1
    assert document["derivatives"][0]["parameters"]["compression_level"] == 9



@pytest.mark.parametrize("selection, expected", [
    ({"gdeflate_levels": [6]}, {6}),
    ({"gdeflate_levels": [9, 1, 6]}, {1, 6, 9}),
    ({"gdeflate_all_levels": True}, set(range(1, 13))),
])
def test_explicit_gdeflate_selection(tmp_path: Path, selection, expected) -> None:
    # Codec stand-ins check stage wiring; native tests separately check real streams.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["gdeflate"], gdeflate_exe=fake_gdeflate(tmp_path), **selection))
    document = process_set.load_manifest(manifest_path)
    assert len(document["derivatives"]) == len(expected)
    assert {item["parameters"]["compression_level"] for item in document["derivatives"]} == expected


def test_gdeflate_sweep_does_not_change_hlk_level(tmp_path: Path) -> None:
    # Derivative selection may span all levels; the HLK payload remains level 9.
    initialize(tmp_path)
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["gdeflate", "hlk"], gdeflate_exe=fake_gdeflate(tmp_path),
        gdeflate_all_levels=True))
    document = process_set.load_manifest(manifest_path)
    assert len(document["derivatives"]) == 12
    assert {record["payload_codec"] for record in document["archives"]} == {
        "uncompressed", "gdeflate", "zstd"}



def test_removed_singular_gdeflate_option_is_not_an_abbreviation(monkeypatch, capsys) -> None:
    # Argument parsing must reject the old spelling before any set processing.
    def unexpected_processing(args):
        pytest.fail("removed options must not reach the processing stages")
    monkeypatch.setattr(orchestrator, "run", unexpected_processing)
    monkeypatch.setattr(sys, "argv", ["run-content-factory.py", "sample", "--gdeflate-level", "6"])
    with pytest.raises(SystemExit) as error:
        orchestrator.main()
    assert error.value.code == 2
    assert "unrecognized arguments: --gdeflate-level" in capsys.readouterr().err


@pytest.mark.parametrize("failure", ["manifest_write", "post_verify"])
def test_gdeflate_manifest_commit_failure_restores_previous_selection(tmp_path: Path, monkeypatch, failure: str) -> None:
    # Exercise the derivative rollback using an in-process encoder stand-in.
    gdeflate = load("gdeflate_transaction_tests", "gdeflate_compress.py")
    initialize(tmp_path)
    placeholder = tmp_path / "unused-tool.exe"
    placeholder.write_bytes(b"never executed")
    monkeypatch.setattr(gdeflate, "tool_version", lambda tool: "1.0.0")

    def build(root, document, stage, executable, levels):
        records = []
        for source in document["sources"]:
            for level in levels:
                relative = gdeflate.output_relative_path(source["path"], document["set_name"], level)
                output = stage / Path(relative).relative_to(Path("gdeflate") / document["set_name"])
                output.parent.mkdir(parents=True, exist_ok=True)
                data = (root / document["source_root"] / source["path"]).read_bytes()
                payload = struct.pack("<IHHIIQQ", 0x31464447, 1, 32, level, 0, len(data), len(data)) + data
                output.write_bytes(payload)
                records.append({"path": relative, "source": source["path"], "format": "gdeflate",
                                "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                                "parameters": {"compression_level": level},
                                "metadata": {"header_size": 32, "compressed_size": len(data),
                                             "uncompressed_size": len(data)}, "validation": "pass"})
        return records

    monkeypatch.setattr(gdeflate, "build_derivatives", build)
    manifest_path = gdeflate.process(tmp_path, "sample", placeholder, (9,), False)
    original_manifest = manifest_path.read_bytes()
    output_root = tmp_path / "gdeflate" / "sample"
    original_outputs = {path.relative_to(output_root).as_posix(): path.read_bytes()
                        for path in output_root.rglob("*") if path.is_file()}
    driver = gdeflate.load_driver()
    triggered = []
    if failure == "manifest_write":
        def fail_write(*args):
            triggered.append(True)
            raise OSError("injected manifest failure")
        monkeypatch.setattr(driver, "write_json_atomic", fail_write)
    else:
        original_verify = driver.verify_files
        def fail_updated_verify(root, set_name, document):
            if {record["parameters"]["compression_level"] for record in document["derivatives"]} == {1, 6}:
                triggered.append(True)
                raise OSError("injected verification failure")
            return original_verify(root, set_name, document)
        monkeypatch.setattr(driver, "verify_files", fail_updated_verify)
    monkeypatch.setattr(gdeflate, "load_driver", lambda: driver)
    with pytest.raises(OSError, match="injected"):
        gdeflate.process(tmp_path, "sample", placeholder, (1, 6), True)
    assert triggered == [True]
    assert manifest_path.read_bytes() == original_manifest
    assert {path.relative_to(output_root).as_posix(): path.read_bytes()
            for path in output_root.rglob("*") if path.is_file()} == original_outputs
    assert not list((tmp_path / "gdeflate").glob(".sample.gdeflate.*"))



def test_orchestrator_cli_defaults_gacl_to_level_19(monkeypatch) -> None:
    captured = []
    def fake_run(args):
        captured.append(args.gacl_zstd_level)
        return Path("manifest.json")
    monkeypatch.setattr(orchestrator, "run", fake_run)
    monkeypatch.setattr(sys, "argv", ["run-content-factory.py", "sample"])
    assert orchestrator.main() == 0
    assert captured == [19]
