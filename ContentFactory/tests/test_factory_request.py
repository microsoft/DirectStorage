"""Portable tool discovery and JSON processing-request regression tests."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

import factory_common
import factory_request
from conftest import load_tool_module, make_python_tool


# Parse a JSON request exactly as the factory launcher does.
def request(tmp_path: Path, data: dict, *overrides: str):
    config = tmp_path / "request.json"
    config.write_text(json.dumps(data), encoding="utf-8")
    return factory_request.parse_request(factory_request.create_parser(), ["--config", str(config), *overrides])


# Check a JSON-only request resolves input paths from its own location rather than the working directory.
def test_json_request_defaults_and_relative_input(tmp_path: Path, monkeypatch) -> None:
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(elsewhere)
    args = request(tmp_path, {"input_directory": "assets"})
    assert args.input_directory == tmp_path / "assets"
    assert args.stages == ["all"] and args.overwrite is False
    assert args.zstd_exe.name == "zstd.exe"
    assert args.gdeflate_exe.name == "GDeflateContentTool.exe"
    assert args.gacl_exe.name == "GACLContentTool.exe"
    assert args.zstd_exe.parent == args.gdeflate_exe.parent == args.gacl_exe.parent
    assert args.zstd_exe.parent.name == "bin"


# Check every supported JSON group maps to the intended processing option.
def test_grouped_json_settings(tmp_path: Path) -> None:
    args = request(tmp_path, {
        "input_directory": "assets", "stages": ["zstd", "gdeflate", "gacl", "hlk"], "overwrite": True,
        "zstd": {"block_sizes_kb": [4, 8], "chunk_sizes_kb": [64, 128]},
        "gdeflate": {"all_levels": True},
        "gacl": {"zstd_level": 17, "target_block_size": 32768},
        "hlk": {"archive_prefix": "custom", "alignment": 4096},
    })
    assert args.block_sizes_kb == [4, 8] and args.chunk_sizes_kb == [64, 128]
    assert args.gdeflate_all_levels is True and args.gdeflate_levels is None
    assert args.gacl_zstd_level == 17 and args.gacl_target_block_size == 32768
    assert args.hlk_archive_prefix == "custom" and args.hlk_alignment == 4096
    assert args.overwrite is True


# Reject processing options on the command line instead of maintaining JSON/CLI precedence.
@pytest.mark.parametrize("options", [["assets"], ["--overwrite"], ["--stages", "zstd"], ["--zstd-exe", "outside.exe"]])
def test_only_json_config_is_accepted_on_cli(tmp_path: Path, options: list[str]) -> None:
    config = tmp_path / "request.json"
    config.write_text('{"input_directory":"assets"}', encoding="utf-8")
    with pytest.raises(SystemExit) as error:
        factory_request.parse_request(factory_request.create_parser(), ["--config", str(config), *options])
    assert error.value.code == 2


# Check JSON types reject booleans-as-numbers, nulls, malformed lists, and misplaced settings.
@pytest.mark.parametrize("options", [
    {"overwrite": 1}, {"overwrite": None}, {"stages": "all"}, {"stages": []}, {"stages": [1]},
    {"gacl": {"zstd_level": True}}, {"gacl": {"target_block_size": "65536"}},
    {"gdeflate": {"levels": [True]}}, {"gdeflate": {"levels": []}}, {"gdeflate": {"all_levels": 1}},
    {"zstd": {"shader_matrix": "false"}}, {"zstd": {"block_sizes_kb": [16.0]}},
    {"hlk": {"alignment": False}}, {"hlk": {"archive_prefix": ""}},
    {"zstd": []}, {"zstd": {"compression_level": 19}}, {"tools": {}},
    {"gacl_exe": "outside.exe"}, {"input_directory": None},
])
def test_invalid_request_values_rejected(tmp_path: Path, options: dict) -> None:
    data = {"input_directory": "assets", **options}
    with pytest.raises(ValueError):
        request(tmp_path, data)


# Check missing input, non-object requests, malformed JSON, and duplicate keys fail without processing.
@pytest.mark.parametrize("body", ["{}", "[]", "null", "not json", '{"input_directory":"a","input_directory":"b"}',
                                  '{"input_directory":"a","zstd":{"shader_matrix":false,"shader_matrix":true}}'])
def test_invalid_request_document_rejected(tmp_path: Path, body: str) -> None:
    path = tmp_path / "request.json"
    path.write_text(body, encoding="utf-8")
    with pytest.raises(ValueError):
        factory_request.parse_request(factory_request.create_parser(), ["--config", str(path)])
    assert not (tmp_path / "generated").exists()


# Check packaged executable discovery is independent of working directory, executable PATH, and PyInstaller internal paths.
def test_frozen_tools_resolve_next_to_application(tmp_path: Path, monkeypatch) -> None:
    application = tmp_path / "portable" / "ContentFactory.exe"
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setattr(sys, "executable", str(application))
    monkeypatch.setenv("PATH", str(tmp_path / "unrelated"))
    assert factory_common.bundled_tool("zstd.exe") == application.parent / "bin" / "zstd.exe"


# Check source execution uses ContentFactory/bin and never searches the caller's PATH.
def test_source_tools_resolve_from_project(monkeypatch) -> None:
    monkeypatch.delattr(sys, "frozen", raising=False)
    assert factory_common.bundled_tool("zstd.exe") == Path(factory_common.__file__).resolve().parents[1] / "bin" / "zstd.exe"


# Check JSON-configured processing uses bundled defaults and produces output without executable-path options.
def test_json_processing_with_simulated_bundle(tmp_path: Path, monkeypatch) -> None:
    launcher = load_tool_module("json_request_launcher", "run-content-factory.py")
    originals = tmp_path / "assets"
    originals.mkdir()
    (originals / "input.bin").write_bytes(b"original input")
    fake_zstd = make_python_tool(originals, "fake_zstd.py", "zstd")
    monkeypatch.setattr(factory_request, "bundled_tool", lambda filename: fake_zstd)
    args = request(tmp_path, {"input_directory": "assets", "stages": ["zstd"]})
    report = launcher.run(args)
    data = json.loads(report.read_text(encoding="utf-8"))
    assert len(data["derivatives"]) == 1 and data["derivatives"][0]["format"] == "zstd"
    assert (originals / data["derivatives"][0]["path"]).is_file()


# Check parsing does not execute encoders and invalid JSON fails before creating outputs.
def test_main_rejects_bad_config_before_processing(tmp_path: Path, monkeypatch, capsys) -> None:
    launcher = load_tool_module("invalid_json_launcher", "run-content-factory.py")
    config = tmp_path / "request.json"
    config.write_text('{"input_directory":"assets","overwite":true}', encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["ContentFactory.exe", "--config", str(config)])
    # A malformed request must never reach processing or tool execution.
    def unexpected_run(args):
        pytest.fail("invalid request reached processing")
    monkeypatch.setattr(launcher, "run", unexpected_run)
    with pytest.raises(SystemExit) as error:
        launcher.main()
    assert error.value.code == 1
    assert "unknown request fields" in capsys.readouterr().err


# Check incomplete portable packages fail clearly without looking for system-installed tools.
def test_missing_bundled_tool_reports_package_error(tmp_path: Path) -> None:
    launcher = load_tool_module("missing_bundle_launcher", "run-content-factory.py")
    args = SimpleNamespace(zstd_shader_matrix=False, block_sizes_kb=None, chunk_sizes_kb=None,
                           zstd_exe=tmp_path / "bin" / "zstd.exe", overwrite=False)
    args.zstd_exe = None
    from unittest.mock import patch
    with patch.object(launcher, "bundled_tool", return_value=tmp_path / "bin" / "zstd.exe"):
        with pytest.raises(FileNotFoundError, match="complete ContentFactory package"):
            launcher.preflight(args, ["zstd"], False)
