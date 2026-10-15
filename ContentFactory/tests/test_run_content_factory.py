#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
from argparse import Namespace
from pathlib import Path

from conftest import assert_report_files, create_content_set, load_tool_module, make_python_tool

import pytest

FACTORY_ROOT = Path(__file__).resolve().parents[1]
TOOLS = FACTORY_ROOT / "tools"


orchestrator = load_tool_module("run_content_factory", "run-content-factory.py")
process_set = load_tool_module("process_set_for_orchestration_tests", "content_report.py")


# Build a small orchestration argument namespace with per-test overrides.
def arguments(directory: Path, **overrides) -> Namespace:
    # Start with a small test configuration and let each test override what it needs.
    values = {
        "input_directory": directory,
        "stages": ["zstd"],
        "overwrite": False,
        "zstd_exe": overrides.get("zstd_exe") or make_python_tool(directory, "fake_zstd.py", "fake_zstd"),
        "zstd_shader_matrix": False,
        "block_sizes_kb": [4],
        "chunk_sizes_kb": [64],
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


# Check Zstd orchestration, recorded level metadata, final verification, and absence of archives.
def test_runs_zstd_and_final_verification(tmp_path: Path) -> None:
    # A codec-only run records its derivative without producing a mixed archive.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(tmp_path))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert_report_files(tmp_path, manifest_data)
    assert len(manifest_data["derivatives"]) == 1
    assert manifest_data["derivatives"][0]["format"] == "zstd"
    assert manifest_data["derivatives"][0]["parameters"]["compression_level"] == 19
    assert manifest_data["archives"] == []

# Check that default Zstd sizes create one derivative per source with the expected sizes.
def test_default_zstd_sizes_produce_one_variant_per_source(tmp_path: Path) -> None:
    # Omit size lists to check that normal runs do not produce nine copies.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["zstd"], block_sizes_kb=None, chunk_sizes_kb=None))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert len(manifest_data["derivatives"]) == 1
    assert manifest_data["derivatives"][0]["parameters"]["block_size_kb"] == 16
    assert manifest_data["derivatives"][0]["parameters"]["chunk_size_kb"] == 256


# Check that explicit shader-matrix selection creates nine Zstd variants.
def test_shader_matrix_requires_explicit_flag(tmp_path: Path) -> None:
    # Request shader coverage explicitly and expect all nine size combinations.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["zstd"], block_sizes_kb=None, chunk_sizes_kb=None,
        zstd_shader_matrix=True))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert len(manifest_data["derivatives"]) == 9


# Check rejection of combining the shader preset with custom size lists.
def test_shader_matrix_rejects_custom_sizes(tmp_path: Path) -> None:
    # The shader preset must not silently override custom sizes.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    with pytest.raises(ValueError, match="cannot be combined"):
        orchestrator.run(arguments(tmp_path, stages=["zstd"], zstd_shader_matrix=True))


# Check that a non-DDS set skips GACL without requiring its executable.
def test_gacl_is_skipped_for_non_dds_set(tmp_path: Path) -> None:
    # A set with no DDS textures should not need a GACL tool path.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(tmp_path, stages=["zstd", "gacl"], gacl_exe=None))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert {item["format"] for item in manifest_data["derivatives"]} == {"zstd"}


# Check that all-stage preflight rejects a missing GDeflate tool before any codec runs.
def test_all_requires_gdeflate_tool_before_zstd(tmp_path: Path, monkeypatch) -> None:
    # Make any tool execution fail the test; a missing later tool must be caught first.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    # Fail the test if preflight unexpectedly launches a codec process.
    def unexpected_execution(*args, **kwargs):
        pytest.fail("preflight must not execute codec tools")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    with pytest.raises(FileNotFoundError, match="GDeflate"):
        orchestrator.run(arguments(tmp_path, stages=["all"]))
    assert not (tmp_path / "generated" / "zstd").exists()


# Check that HLK orchestration records the complete three-codec archive group.
def test_hlk_stage_creates_lockstep_archives(tmp_path: Path) -> None:
    # Use test codecs to check that the complete three-archive group is recorded.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(
        tmp_path,
        stages=["hlk"],
        gdeflate_exe=make_python_tool(tmp_path, "fake_gdeflate.py", "fake_gdeflate"),
    ))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    # The manifest must include all three members of the HLK triplet.
    assert {record["payload_codec"] for record in manifest_data["archives"]} == {
        "uncompressed", "gdeflate", "zstd"
    }

# Check rejection of duplicate stages and combining all with individual stages.
def test_duplicate_and_mixed_all_stages_are_rejected(tmp_path: Path) -> None:
    # Reject ambiguous stage selections rather than processing anything twice.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    with pytest.raises(ValueError, match="cannot be combined"):
        orchestrator.run(arguments(tmp_path, stages=["all", "zstd"]))
    with pytest.raises(ValueError, match="duplicates"):
        orchestrator.run(arguments(tmp_path, stages=["zstd", "zstd"]))


# Check that a missing input directory is rejected without creating it or a manifest.
def test_missing_input_directory_is_rejected(tmp_path: Path) -> None:
    missing = tmp_path / "missing-input"
    args = arguments(tmp_path, input_directory=missing)
    with pytest.raises(FileNotFoundError, match="input directory does not exist"):
        orchestrator.run(args)
    assert not missing.exists()
    assert not (missing / "generated" / "manifest.json").exists()


# Check automatic inventory creation without copying or modifying existing originals.
def test_existing_inputs_are_used_in_place(tmp_path: Path, monkeypatch) -> None:
    originals = tmp_path
    (originals / "nested").mkdir(parents=True)
    first = originals / "first.bin"
    second = originals / "nested" / "second.bin"
    first.write_bytes(b"first")
    second.write_bytes(b"second")
    before = {path: (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_ino)
              for path in (first, second)}
    # Reject any copying attempt; the factory must consume these files directly.
    def unexpected_copy(*args, **kwargs):
        pytest.fail("factory must not copy originals")
    monkeypatch.setattr("shutil.copyfile", unexpected_copy)
    manifest_path = orchestrator.run(arguments(tmp_path))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert [record["path"] for record in manifest_data["sources"]] == ["first.bin", "nested/second.bin"]
    assert {path: (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_ino)
            for path in (first, second)} == before
    assert sorted(record["path"] for record in manifest_data["sources"]) == ["first.bin", "nested/second.bin"]
    assert not (tmp_path / "generated" / ".factory.lock").exists()


# Check that repeated runs retain originals and require explicit generated-output replacement.
def test_existing_directory_keeps_report_and_inputs(tmp_path: Path) -> None:
    create_content_set(tmp_path, {"data.bin": b"original"})
    manifest = orchestrator.run(arguments(tmp_path))
    before = manifest.read_bytes() if manifest.exists() else None
    original = tmp_path / "data.bin"
    with pytest.raises(FileExistsError, match="--overwrite"):
        orchestrator.run(arguments(tmp_path))
    if before is None:
        assert not manifest.exists()
    else:
        assert manifest.read_bytes() == before
    assert original.read_bytes() == b"original"


# Check preflight failure leaves originals intact and creates no inventory or codec outputs.
def test_later_missing_tool_is_rejected_before_inventory_is_written(tmp_path: Path, monkeypatch) -> None:
    originals = tmp_path
    originals.mkdir(parents=True, exist_ok=True)
    source = originals / "input.bin"
    source.write_bytes(b"input")
    # Fail if tool execution is reached before missing-tool preflight.
    def unexpected_execution(*args, **kwargs):
        pytest.fail("preflight must not execute codec tools")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    with pytest.raises(FileNotFoundError, match="GDeflate"):
        orchestrator.run(arguments(tmp_path, stages=["zstd", "gdeflate"], gdeflate_exe=None))
    assert source.read_bytes() == b"input"
    assert not (tmp_path / "generated" / "manifest.json").exists()
    assert not (tmp_path / "generated" / "zstd").exists()


# Check that the removed import option cannot accidentally start processing.
def test_sources_cli_option_is_removed(tmp_path: Path, monkeypatch, capsys) -> None:
    # Fail if a removed command option reaches the orchestration workflow.
    def unexpected_run(args):
        pytest.fail("removed sources option must fail at argument parsing")
    monkeypatch.setattr(orchestrator, "run", unexpected_run)
    monkeypatch.setattr(sys, "argv", ["run-content-factory.py", str(tmp_path), "--sources", "external.bin"])
    with pytest.raises(SystemExit) as error:
        orchestrator.main()
    assert error.value.code == 2
    assert "error:" in capsys.readouterr().err


# Check that invalid later-stage options leave the manifest and earlier-stage outputs untouched.
@pytest.mark.parametrize("options, message", [
    ({"stages": ["zstd", "gdeflate"], "gdeflate_levels": [13]}, "GDeflate level"),
    ({"stages": ["zstd", "gdeflate"], "gdeflate_levels": [1, 1]}, "duplicates"),
    ({"stages": ["zstd", "gdeflate"], "gdeflate_levels": [1, 6], "gdeflate_all_levels": True}, "cannot be combined"),
    ({"stages": ["zstd", "hlk"], "hlk_alignment": 3}, "power of two"),
    ({"stages": ["zstd"], "block_sizes_kb": [128], "chunk_sizes_kb": [64]}, "exceed chunk"),
])
def test_invalid_later_options_do_not_generate_earlier_stage(tmp_path: Path, monkeypatch, options, message) -> None:
    # Bad later-stage options must not change the manifest or create Zstd outputs.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest = tmp_path / "generated" / "manifest.json"
    # Remember the inventory bytes to detect any work performed before rejection.
    before = manifest.read_bytes() if manifest.exists() else None
    # Fail the test if invalid options reach tool execution.
    def unexpected_execution(*args, **kwargs):
        pytest.fail("invalid options must be rejected before tool execution")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    with pytest.raises(ValueError, match=message):
        orchestrator.run(arguments(tmp_path, **options))
    if before is None:
        assert not manifest.exists()
    else:
        assert manifest.read_bytes() == before
    assert not (tmp_path / "generated" / "zstd").exists()


# Check that a nonexistent executable is rejected before stage generation.
def test_nonexistent_tool_is_rejected_before_generation(tmp_path: Path, monkeypatch) -> None:
    # A path argument alone is not enough; the tool file must exist.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    # Fail the test if nonexistent-tool preflight launches any tool.
    def unexpected_execution(*args, **kwargs):
        pytest.fail("preflight must not execute tools")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    with pytest.raises(FileNotFoundError, match="bundled tool unavailable"):
        orchestrator.run(arguments(tmp_path, stages=["zstd", "gdeflate"],
                                   gdeflate_exe=tmp_path / "missing.exe"))
    assert not (tmp_path / "generated" / "zstd").exists()


# Check rejection of the removed mixed-archive stage.
def test_mixed_archive_stage_is_no_longer_supported(tmp_path: Path) -> None:
    # Reject the removed stage rather than silently packing derivative files.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    with pytest.raises(ValueError, match="valid Content Factory stages"):
        orchestrator.run(arguments(tmp_path, stages=["archive"]))


# Check that GDeflate orchestration records one level-9 derivative by default.
def test_gdeflate_stage_produces_one_default_level(tmp_path: Path) -> None:
    # Orchestration defaults to level 9; HLK independently uses the same default preset.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["gdeflate"], gdeflate_exe=make_python_tool(tmp_path, "fake_gdeflate.py", "fake_gdeflate")))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert len(manifest_data["derivatives"]) == 1
    assert manifest_data["derivatives"][0]["parameters"]["compression_level"] == 9


# Check that explicit GDeflate levels and all-level selection produce the requested records.
@pytest.mark.parametrize("selection, expected", [
    ({"gdeflate_levels": [6]}, {6}),
    ({"gdeflate_levels": [9, 1, 6]}, {1, 6, 9}),
    ({"gdeflate_all_levels": True}, set(range(1, 13))),
])
def test_explicit_gdeflate_selection(tmp_path: Path, selection, expected) -> None:
    # Codec stand-ins check stage wiring; native tests separately check real streams.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["gdeflate"], gdeflate_exe=make_python_tool(tmp_path, "fake_gdeflate.py", "fake_gdeflate"), **selection))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert len(manifest_data["derivatives"]) == len(expected)
    assert {item["parameters"]["compression_level"] for item in manifest_data["derivatives"]} == expected


# Check all-level derivatives alongside a complete HLK archive triplet.
def test_gdeflate_sweep_does_not_change_hlk_level(tmp_path: Path) -> None:
    # Derivative selection may span all levels; the HLK payload remains level 9.
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    manifest_path = orchestrator.run(arguments(
        tmp_path, stages=["gdeflate", "hlk"], gdeflate_exe=make_python_tool(tmp_path, "fake_gdeflate.py", "fake_gdeflate"),
        gdeflate_all_levels=True))
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert len(manifest_data["derivatives"]) == 12
    assert {record["payload_codec"] for record in manifest_data["archives"]} == {
        "uncompressed", "gdeflate", "zstd"}


# Check that the removed singular GDeflate option is rejected before processing.
def test_removed_singular_gdeflate_option_is_not_an_abbreviation(tmp_path: Path, monkeypatch, capsys) -> None:
    # Argument parsing must reject the old spelling before any set processing.
    # Fail the test if a removed CLI option reaches processing.
    def unexpected_processing(args):
        pytest.fail("removed options must not reach the processing stages")
    monkeypatch.setattr(orchestrator, "run", unexpected_processing)
    monkeypatch.setattr(sys, "argv", ["run-content-factory.py", str(tmp_path), "--gdeflate-level", "6"])
    with pytest.raises(SystemExit) as error:
        orchestrator.main()
    assert error.value.code == 2
    assert "error:" in capsys.readouterr().err


# Check factory CLI keeps GACL level 19 and reports the successfully written path.
def test_orchestrator_cli_defaults_gacl_to_level_19(tmp_path: Path, monkeypatch, capsys) -> None:
    captured = []
    # Capture the CLI GACL level without running orchestration.
    def fake_run(args):
        captured.append(args.gacl_zstd_level)
        return Path("manifest.json")
    monkeypatch.setattr(orchestrator, "run", fake_run)
    config = tmp_path / "request.json"
    config.write_text(json.dumps({"input_directory": str(tmp_path)}), encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["run-content-factory.py", "--config", str(config)])
    assert orchestrator.main() == 0
    assert captured == [19]
    assert capsys.readouterr().out == "Wrote manifest.json\n"


# Check rejection of symlinked generated and codec output directories without changing the manifest.
@pytest.mark.parametrize("codec", ["zstd", "gdeflate", "gacl", "hlk"])
@pytest.mark.parametrize("linked_component", ["generated", "codec"])
def test_codec_rejects_symlinked_output_parent_before_writing(tmp_path: Path, monkeypatch, codec: str, linked_component: str) -> None:
    create_content_set(tmp_path, {"data.bin": bytes(range(251)) * 3})
    tool = tmp_path / "generated" / "unused-tool.exe"
    tool.parent.mkdir(exist_ok=True)
    tool.write_bytes(b"not executed")
    module = load_tool_module("symlink_" + codec, "hlk_content_set.py" if codec == "hlk" else codec + "_compress.py")
    version_helper = "verify_tool" if codec == "hlk" else "zstd_version" if codec == "zstd" else "tool_version"
    monkeypatch.setattr(module, version_helper, lambda *args: "1.0.0")
    parent = tmp_path / "generated"
    if linked_component == "codec":
        parent /= codec
    original = Path.is_symlink
    monkeypatch.setattr(Path, "is_symlink", lambda path: path == parent or original(path))
    manifest = tmp_path / "generated" / "manifest.json"
    before = manifest.read_bytes() if manifest.exists() else None
    with pytest.raises(ValueError, match="symbolic links"):
        if codec == "zstd":
            module.process(tmp_path, tool, (16,), (256,), False)
        elif codec == "gdeflate":
            module.process(tmp_path, tool, (9,), False)
        elif codec == "gacl":
            module.process(tmp_path, tool, 19, 65536, False)
        else:
            module.process(tmp_path, "dstoragetest", tool, tool, 1, False)
    if before is None:
        assert not manifest.exists()
    else:
        assert manifest.read_bytes() == before
    assert not list((tmp_path / "generated").glob(f".{codec}.stage.*"))
    assert not list((tmp_path / "generated").glob(f".{codec}.backup.*"))
    if codec == "hlk":
        assert not list((tmp_path / "generated" / "hlk").glob(".hlk.stage.*"))
    assert not (tmp_path / "generated" / ".factory.lock").exists()


# Check linked input directories are rejected before inventory creation.
def test_input_directory_rejects_symlink(tmp_path: Path, monkeypatch) -> None:
    original = Path.is_symlink
    monkeypatch.setattr(Path, "is_symlink", lambda path: path == tmp_path or original(path))
    with pytest.raises(ValueError, match="symbolic link"):
        orchestrator.run(arguments(tmp_path))
    assert not (tmp_path / "generated" / "manifest.json").exists()


# Check orchestration discovers originals once and all selected stages commit into the same report object.
def test_orchestrator_discovers_sources_once_and_shares_report(tmp_path: Path, monkeypatch) -> None:
    create_content_set(tmp_path, {"data.bin": b"original"})
    discoveries = []
    shared_reports = []
    original_create = process_set.create_manifest
    # Record fresh source discovery while preserving hashing of the actual originals.
    def create_once(input_directory):
        report = original_create(input_directory)
        discoveries.append(report)
        return report
    monkeypatch.setattr(process_set, "create_manifest", create_once)
    fake_zstd = arguments(tmp_path).zstd_exe
    fake_gdeflate = make_python_tool(tmp_path, "fake_gdeflate.py", "fake_gdeflate")
    # Observe regular-import stage singletons receiving the same newly discovered report.
    for stage_name in ("zstd", "gdeflate"):
        module = orchestrator.STAGE_MODULES[stage_name]
        assert module.content_report is process_set
        original_process = module.process

        # Capture each stage's keyword-only shared report while retaining its original process function.
        def stage(*args, report=None, _process=original_process, **kwargs):
            shared_reports.append(report)
            return _process(*args, report=report, **kwargs)

        monkeypatch.setattr(module, "process", stage)
    path = orchestrator.run(arguments(tmp_path, stages=["zstd", "gdeflate"], zstd_exe=fake_zstd, gdeflate_exe=fake_gdeflate))
    assert len(discoveries) == 1
    assert len(shared_reports) == 2
    assert all(report is discoveries[0] for report in shared_reports)
    disk = json.loads(path.read_text(encoding="utf-8"))
    assert disk == discoveries[0]
    assert {record["format"] for record in disk["derivatives"]} == {"zstd", "gdeflate"}
    assert_report_files(tmp_path, disk)


# Check a new invocation ignores corrupt unselected outputs and an unreadable old report.
def test_unselected_corrupt_output_does_not_block_fresh_selected_stage(tmp_path: Path) -> None:
    create_content_set(tmp_path, {"data.bin": b"original"})
    path = orchestrator.run(arguments(tmp_path))
    report = json.loads(path.read_text(encoding="utf-8"))
    output = tmp_path / report["derivatives"][0]["path"]
    output.write_bytes(b"corrupt old unselected output")
    path.write_bytes(b"invalid old report")
    result = orchestrator.run(arguments(tmp_path, stages=["gdeflate"], gdeflate_exe=make_python_tool(tmp_path, "fake_gdeflate.py", "fake_gdeflate")))
    current = json.loads(result.read_text(encoding="utf-8"))
    assert {record["format"] for record in current["derivatives"]} == {"gdeflate"}
    assert set(current["tools"]) == {"content_report.py", "GDeflateContentTool"}
    assert current["archives"] == []
    assert output.read_bytes() == b"corrupt old unselected output"
    assert_report_files(tmp_path, current)


# Check the shared fixture creates originals without any preliminary report or generated directories.
def test_create_content_set_leaves_report_and_codec_outputs_absent(tmp_path: Path) -> None:
    files = {"input.bin": b"input", "nested/other.bin": b"other"}
    assert create_content_set(tmp_path, files) == files
    assert not (tmp_path / "generated").exists()
    for relative, content in files.items():
        assert (tmp_path / relative).read_bytes() == content


# Check removed root, named-set, and version override controls cannot silently reenter the CLI.
@pytest.mark.parametrize("legacy_arguments", [
    ["--root", "unused"],
    ["sample"],
    ["--allow-version-change"],
])
def test_orchestrator_rejects_legacy_layout_controls(tmp_path: Path, monkeypatch, capsys, legacy_arguments: list[str]) -> None:
    monkeypatch.setattr(sys, "argv", ["run-content-factory.py", str(tmp_path), *legacy_arguments])
    with pytest.raises(SystemExit) as error:
        orchestrator.main()
    assert error.value.code == 2
    assert "error:" in capsys.readouterr().err


# Check rerunning inputs with spaces and dots keeps generated artifacts separate and original bytes intact.
def test_nested_inputs_and_generated_artifacts_remain_separate(tmp_path: Path) -> None:
    input_directory = tmp_path / "input directory.with.dots"
    source = input_directory / "nested folder.v1" / "input file.bin"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"nested input")
    before = source.stat()
    generated = input_directory / "generated"
    generated.mkdir()
    (generated / "stale.bin").write_bytes(b"not an original")
    args = arguments(input_directory)
    manifest_path = orchestrator.run(args)
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert manifest_path == generated / "manifest.json"
    assert [record["path"] for record in manifest_data["sources"]] == ["nested folder.v1/input file.bin"]
    assert [record["path"] for record in manifest_data["derivatives"]] == [
        "generated/zstd/nested folder.v1/input file.bin-blk4K-chunk64K.zst",
    ]
    output = input_directory / manifest_data["derivatives"][0]["path"]
    before_output = output.read_bytes()
    before_manifest = manifest_path.read_bytes()
    with pytest.raises(FileExistsError):
        orchestrator.run(args)
    assert manifest_path.read_bytes() == before_manifest
    assert output.read_bytes() == before_output
    assert (generated / "stale.bin").read_bytes() == b"not an original"
    assert source.read_bytes() == b"nested input"
    assert source.stat().st_mtime_ns == before.st_mtime_ns
    assert source.stat().st_ino == before.st_ino
    assert_report_files(input_directory, manifest_data)


# Check an overwrite invocation replaces previous stage history with only its currently selected outputs.
def test_overwrite_report_contains_only_latest_selected_stages(tmp_path: Path) -> None:
    create_content_set(tmp_path, {"data.bin": b"original"})
    zstd_tool = make_python_tool(tmp_path, "fake_zstd.py", "latest-zstd")
    gdeflate_tool = make_python_tool(tmp_path, "fake_gdeflate.py", "latest-gdeflate")
    first_path = orchestrator.run(arguments(tmp_path, stages=["zstd", "gdeflate"], zstd_exe=zstd_tool, gdeflate_exe=gdeflate_tool))
    first = json.loads(first_path.read_text(encoding="utf-8"))
    assert {record["format"] for record in first["derivatives"]} == {"zstd", "gdeflate"}
    prior_gdeflate = {record["path"]: (tmp_path / record["path"]).read_bytes()
                      for record in first["derivatives"] if record["format"] == "gdeflate"}
    (tmp_path / "data.bin").write_bytes(b"changed original")
    path = orchestrator.run(arguments(tmp_path, stages=["zstd"], zstd_exe=zstd_tool, overwrite=True))
    current = json.loads(path.read_text(encoding="utf-8"))
    assert current["sources"] != first["sources"]
    assert {record["format"] for record in current["derivatives"]} == {"zstd"}
    assert set(current["tools"]) == {"content_report.py", "zstd"}
    assert current["archives"] == []
    assert {name: (tmp_path / name).read_bytes() for name in prior_gdeflate} == prior_gdeflate
    assert_report_files(tmp_path, current)


# Check CLI success reports a generated report rather than claiming historical files were verified.
def test_orchestrator_cli_prints_wrote(tmp_path: Path, monkeypatch, capsys) -> None:
    path = tmp_path / "generated" / "manifest.json"
    monkeypatch.setattr(orchestrator, "run", lambda args: path)
    config = tmp_path / "request.json"
    config.write_text(json.dumps({"input_directory": str(tmp_path)}), encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["run-content-factory.py", "--config", str(config)])
    assert orchestrator.main() == 0
    assert capsys.readouterr().out == f"Wrote {path}\n"


# Check original-file redirect components are rejected before any protected source is read or output committed.
@pytest.mark.parametrize("codec", ["zstd", "gdeflate", "gacl", "hlk", "orchestrator"])
@pytest.mark.parametrize("linked_component", ["file", "directory"])
@pytest.mark.parametrize("link_kind", ["symlink", "junction"])
def test_generation_rejects_redirected_original_reads(tmp_path: Path, monkeypatch, codec: str, linked_component: str, link_kind: str) -> None:
    input_directory = tmp_path / "inputs"
    source = input_directory / "nested" / "texture.dds"
    source.parent.mkdir(parents=True)
    source.write_bytes(b"must not be read through a redirect")
    outside = tmp_path / "outside-original.bin"
    outside.write_bytes(b"outside originals remain untouched")
    linked = source if linked_component == "file" else source.parent
    attribute = "is_symlink" if link_kind == "symlink" else "is_junction"
    original_link_check = getattr(Path, attribute, lambda path: False)
    monkeypatch.setattr(Path, attribute, lambda path: path == linked or original_link_check(path), raising=False)
    original_open = Path.open
    protected_reads = []
    # Fail loudly if any code attempts to consume protected original bytes instead of rejecting the redirect.
    def open_guard(path, mode="r", *args, **kwargs):
        if path in (source, outside) and "r" in mode:
            protected_reads.append(path)
            pytest.fail("redirected original must be rejected before reading")
        return original_open(path, mode, *args, **kwargs)
    monkeypatch.setattr(Path, "open", open_guard)
    generated = input_directory / "generated"
    generated.mkdir()
    tool = generated / "unused-tool.exe"
    tool.write_bytes(b"never executed")
    # No encoder command may execute during this original-path safety test.
    def unexpected_execution(*args, **kwargs):
        pytest.fail("unsafe original paths must fail before codec execution")
    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    if codec == "orchestrator":
        invoke = lambda: orchestrator.run(arguments(input_directory, zstd_exe=tool))
    else:
        filename = "hlk_content_set.py" if codec == "hlk" else codec + "_compress.py"
        module = load_tool_module("safe_original_" + codec, filename)
        if codec == "hlk":
            monkeypatch.setattr(module, "verify_tool", lambda *args: "test encoder 1.0.0")
            invoke = lambda: module.process(input_directory, "dstoragetest", tool, tool, 1, False)
        else:
            monkeypatch.setattr(module, "zstd_version" if codec == "zstd" else "tool_version", lambda *args: "1.0.0")
            options = ((16,), (256,)) if codec == "zstd" else ((9,),) if codec == "gdeflate" else (19, 65536)
            invoke = lambda: module.process(input_directory, tool, *options, False)
    with pytest.raises(ValueError, match="symbolic links or junctions"):
        invoke()
    assert protected_reads == []
    assert not (generated / "manifest.json").exists()
    assert not (generated / ".factory.lock").exists()
    assert not any((generated / name).exists() for name in ("zstd", "gdeflate", "gacl", "hlk"))
    assert not list(generated.glob(".*.stage.*"))
    assert not list(generated.glob(".*.backup.*"))
    monkeypatch.setattr(Path, "open", original_open)
    assert source.read_bytes() == b"must not be read through a redirect"
    assert outside.read_bytes() == b"outside originals remain untouched"


# Check prepared preflight tuples use regular-import modules directly and perform no process execution.
def test_preflight_returns_only_prepared_argument_tuples(tmp_path: Path, monkeypatch) -> None:
    create_content_set(tmp_path, {"input.bin": b"original"})
    tool = tmp_path / "generated" / "unused-tool.exe"
    tool.parent.mkdir()
    tool.write_bytes(b"never executed")
    options = arguments(tmp_path, zstd_exe=tool, gdeflate_exe=tool, gacl_exe=tool,
                        overwrite=True, stages=["hlk", "gacl", "gdeflate", "zstd"])

    # Preflight may inspect tool paths and validate options, but must never invoke codec processes.
    def unexpected_execution(*args, **kwargs):
        pytest.fail("preflight cannot execute codec tools")

    monkeypatch.setattr("factory_common.subprocess.run", unexpected_execution)
    prepared = orchestrator.preflight(options, options.stages, True)
    assert prepared == {
        "zstd": (tool, (4,), (64,), True),
        "gdeflate": (tool, (9,), True),
        "gacl": (tool, 19, 65536, True),
        "hlk": ("dstoragetest", tool, tool, 1, True),
    }
    assert all(isinstance(value, tuple) for value in prepared.values())
    assert list(orchestrator.STAGE_MODULES) == ["zstd", "gdeflate", "gacl", "hlk"]
    assert not hasattr(orchestrator, "load_module")
    assert not hasattr(orchestrator, "set_lock")
    assert not (tmp_path / "generated" / "manifest.json").exists()
    assert sorted(item.name for item in tool.parent.iterdir()) == ["unused-tool.exe"]


# Check factory stage dispatch follows fixed order regardless of selection order and passes tuples/report unchanged.
def test_process_selected_stages_uses_fixed_order_and_shared_report(tmp_path: Path, monkeypatch) -> None:
    report = {"derivatives": [], "archives": [], "tools": {}}
    derivatives = report["derivatives"]
    archives = report["archives"]
    tools = report["tools"]
    selected = {name: (object(), object()) for name in reversed(orchestrator.ALL_STAGES)}
    calls = []
    for stage_name, module in orchestrator.STAGE_MODULES.items():

        # Capture the exact stage tuple and mutate existing shared containers as real stages do.
        def stage(input_directory, *args, report=None, _stage=stage_name):
            calls.append((_stage, input_directory, args, report))
            report["derivatives"].append(_stage)
            report["tools"][_stage] = {"version": "test"}

        monkeypatch.setattr(module, "process", stage)
    assert orchestrator.process_selected_stages(tmp_path, selected, report) is None
    assert [name for name, *_ in calls] == ["zstd", "gdeflate", "gacl", "hlk"]
    assert all(root == tmp_path and args == selected[name] and shared is report
               for name, root, args, shared in calls)
    assert report["derivatives"] is derivatives and report["archives"] is archives and report["tools"] is tools
    assert derivatives == ["zstd", "gdeflate", "gacl", "hlk"]


# Check stage dispatch stops at the first failure without rolling back an earlier stage's shared report additions.
def test_process_selected_stages_failure_keeps_earlier_stage_records(tmp_path: Path, monkeypatch) -> None:
    report = {"derivatives": [], "archives": [], "tools": {}}
    calls = []

    # Retain successful earlier-stage records even when a later stage raises.
    def first(input_directory, *args, report=None):
        calls.append("zstd")
        report["derivatives"].append({"format": "zstd"})

    # Propagate a later failure rather than attempting factory-wide snapshots or rollback.
    def second(input_directory, *args, report=None):
        calls.append("gdeflate")
        raise RuntimeError("injected later-stage failure")

    # No selected stage after the failure may execute.
    def later(*args, **kwargs):
        pytest.fail("later stages cannot run after a failed stage")

    monkeypatch.setattr(orchestrator.STAGE_MODULES["zstd"], "process", first)
    monkeypatch.setattr(orchestrator.STAGE_MODULES["gdeflate"], "process", second)
    monkeypatch.setattr(orchestrator.STAGE_MODULES["hlk"], "process", later)
    with pytest.raises(RuntimeError, match="later-stage failure"):
        orchestrator.process_selected_stages(tmp_path, {"hlk": (), "gdeflate": (), "zstd": ()}, report)
    assert calls == ["zstd", "gdeflate"]
    assert report["derivatives"] == [{"format": "zstd"}]
