"""Regression tests for shared helpers; subprocess calls are mocked here."""

import json
import subprocess
from pathlib import Path

from unittest.mock import patch

import pytest

import factory_common as common
import factory_contracts as contracts
from conftest import load_tool_module


# Check that silent native failures report the executable, exit code, and empty stderr.
def test_silent_native_crash_reports_exit_code_and_executable() -> None:
    # Simulate a tool crash with no output; the error must still explain what failed.
    result = subprocess.CompletedProcess(["codec.exe"], 3221225477, b"", b"")
    with patch.object(common.subprocess, "run", return_value=result):
        with pytest.raises(RuntimeError) as error:
            common.run_command(["codec.exe"], operation="compression")
    # Check that a silent Windows crash is recognizable from the message alone.
    message = str(error.value)
    assert "codec.exe" in message
    assert "3221225477 (0xC0000005)" in message
    assert "stderr: <empty>" in message


# Check that oversized tool failure output is truncated to a bounded message.
def test_error_output_is_bounded() -> None:
    # Simulate a noisy failure without starting a real codec process.
    result = subprocess.CompletedProcess(["codec.exe"], 1, b"x" * 10000, b"")
    with patch.object(common.subprocess, "run", return_value=result):
        with pytest.raises(RuntimeError) as error:
            common.run_command(["codec.exe"], operation="compression")
    # Avoid flooding logs with partial binary output from a failed tool.
    assert "<truncated>" in str(error.value)
    assert len(str(error.value)) < 5000


# Check that the command wrapper preserves binary output, input, and execution settings.
def test_binary_tool_output_is_not_decoded_or_modified() -> None:
    # Use non-text bytes to catch accidental decoding of compressed output.
    payload = b"\x00\xff\x80compressed"
    result = subprocess.CompletedProcess(["codec.exe"], 0, payload, b"")
    with patch.object(common.subprocess, "run", return_value=result) as run:
        assert common.run_command(["codec.exe"], operation="compression", input_data=b"source").stdout == payload
    # The wrapper must preserve binary input and the requested execution settings.
    run.assert_called_once_with(
        ["codec.exe"], input=b"source", capture_output=True, text=False, timeout=300, check=False)


# Check version text parsing and contextual timeout reporting.
def test_text_tool_output_and_timeout_are_preserved() -> None:
    # Mock version output and a timeout to check both success and failure messages.
    result = subprocess.CompletedProcess(["codec.exe", "--version"], 0, "tool 1.2.3\n", "")
    with patch.object(common.subprocess, "run", return_value=result):
        assert common.tool_version(Path("codec.exe"), "codec") == "1.2.3"
    # Report timeouts with the same executable context as other failures.
    with patch.object(common.subprocess, "run", side_effect=subprocess.TimeoutExpired("codec.exe", 30)):
        with pytest.raises(RuntimeError, match="timed out after 30 seconds"):
            common.run_command(["codec.exe"], operation="compression", timeout=30)


# Check that a failed atomic replacement preserves the old file and removes temporary files.
def test_failed_atomic_replace_preserves_existing_file(tmp_path: Path) -> None:
    # Force the final rename to fail and check the previous file survives.
    path = tmp_path / "output"
    path.write_bytes(b"old")
    with patch.object(common.os, "replace", side_effect=OSError("replace denied")):
        with pytest.raises(OSError, match="replace denied"):
            common.write_bytes_atomic(path, b"new")
    # A failed install must not change the previous bytes or leave a temporary file.
    assert path.read_bytes() == b"old"
    assert list(tmp_path.iterdir()) == [path]


# Check single-invocation helpers no longer expose dynamic loading, locks, or derivative transactions.
def test_shared_helpers_have_no_lock_or_transaction_api() -> None:
    for name in ("load_module", "set_lock", "DerivativeTransaction", "replace_derivative_records"):
        assert not hasattr(common, name)


# Check every regular helper import shares its production singleton regardless of the fixture alias.
def test_tool_fixture_uses_regular_shared_imports() -> None:
    import importlib

    for name in ("content_report", "zstd_compress", "gdeflate_compress", "gacl_compress",
                 "hlk_content_set", "create_hlk_archive", "validate_hlk_archive"):
        imported = importlib.import_module(name)
        assert load_tool_module("first_alias_" + name, name + ".py") is imported
        assert load_tool_module("second_alias_" + name, name + ".py") is imported
    report = importlib.import_module("content_report")
    for name in ("zstd_compress", "gdeflate_compress", "gacl_compress", "hlk_content_set"):
        assert importlib.import_module(name).content_report is report


# Check default, shader, and custom Zstd sizes and rejection of invalid combinations.
def test_shared_zstd_resolver_preserves_default_matrix_and_custom_sizes() -> None:
    # Check preset selection and invalid combinations without running Zstd.
    zstd = load_tool_module("zstd_for_common_tests", "zstd_compress.py")
    assert zstd.resolve_sizes(False, None, None) == ((16,), (256,))
    assert zstd.resolve_sizes(True, None, None) == ((4, 8, 16), (64, 128, 256))
    assert zstd.resolve_sizes(False, [8, 4], [128]) == ((4, 8), (128,))
    # Invalid settings should be rejected by the resolver, before tool execution.
    with pytest.raises(ValueError, match="duplicate"):
        zstd.resolve_sizes(False, [8, 4, 8], [128])
    with pytest.raises(ValueError, match="cannot be combined"):
        zstd.resolve_sizes(True, [4], None)
    with pytest.raises(ValueError, match="positive integer"):
        zstd.resolve_sizes(False, [], None)
    with pytest.raises(ValueError, match="cannot exceed chunk size"):
        zstd.resolve_sizes(False, [128], [64])


# Check explicit Zstd level 19 for shader and HLK compression commands.
def test_shader_and_hlk_zstd_use_shared_explicit_level_19() -> None:
    # Inspect command settings directly, independently of the codec stand-in.
    zstd = load_tool_module("zstd_level_tests", "zstd_compress.py")
    with patch.object(zstd, "run_zstd", return_value=subprocess.CompletedProcess([], 0, b"frame", b"")) as run:
        assert zstd.compress_chunk(Path("zstd.exe"), b"source", 16384) == b"frame"
    assert "-19" in run.call_args.args[0]
    assert run.call_args.kwargs["data"] == b"source"
    hlk = load_tool_module("hlk_level_tests", "hlk_content_set.py")
    with patch.object(hlk, "run_tool", side_effect=[
        subprocess.CompletedProcess([], 0, b"frame", b""),
        subprocess.CompletedProcess([], 0, b"source", b"")]) as run:
        assert hlk.compress_zstd(Path("zstd.exe"), b"source") == b"frame"
    assert "-19" in run.call_args_list[0].args[0]
    assert "--zstd=wlog=18" in run.call_args_list[0].args[0]


# Check default and explicit GDeflate level selection and rejection of invalid requests.
def test_gdeflate_level_resolver_keeps_single_default_and_explicit_coverage() -> None:
    # Resolve settings without running a codec; sweep requests are explicit and exclusive.
    gdeflate = load_tool_module("gdeflate_for_common_tests", "gdeflate_compress.py")
    assert gdeflate.resolve_levels() == (9,)
    assert gdeflate.resolve_levels(levels=[6]) == (6,)
    assert gdeflate.resolve_levels(levels=[9, 1, 6]) == (1, 6, 9)
    assert gdeflate.resolve_levels(all_levels=True) == tuple(range(1, 13))
    for options in ({"levels": []}, {"levels": [1, 1]}, {"levels": [13]},
                    {"levels": [True]}, {"levels": [1, 0]},
                    {"levels": [1, 6], "all_levels": True}):
        with pytest.raises(ValueError):
            gdeflate.resolve_levels(**options)


# Check GDeflate envelope metadata and rejection of bad sizes and unsupported levels.
def test_shared_gdeflate_header_checks_envelope_and_level() -> None:
    # Construct a small file envelope, then reject incomplete or inconsistent versions of it.
    header = contracts.GDEFLATE_HEADER
    encoded = header.pack(contracts.GDEFLATE_MAGIC, 1, header.size, 6, 0, 100, 3) + b"abc"
    assert contracts.parse_gdeflate_header(encoded) == {
        "level": 6, "header_size": header.size, "uncompressed_size": 100, "compressed_size": 3}
    # Short files, missing payload bytes, and extra bytes must all be rejected.
    for invalid in (encoded[:4], encoded[:-1], encoded + b"extra"):
        with pytest.raises(RuntimeError):
            contracts.parse_gdeflate_header(invalid)
    # A well-sized envelope can still contain an unsupported compression level.
    invalid_level = header.pack(contracts.GDEFLATE_MAGIC, 1, header.size, 13, 0, 100, 3) + b"abc"
    with pytest.raises(RuntimeError, match="level"):
        contracts.parse_gdeflate_header(invalid_level)


# Check that both native tool version queries retain a thirty-second timeout.
@pytest.mark.parametrize("label", ["GDeflateContentTool", "GACLContentTool"])
def test_native_version_timeout_stays_at_thirty_seconds(label: str) -> None:
    # Mock both native tool banners and keep their original version-query timeout.
    result = subprocess.CompletedProcess(["codec.exe", "--version"], 0, "tool 1.2.3\n", "")
    with patch.object(common.subprocess, "run", return_value=result) as run:
        assert common.tool_version(Path("codec.exe"), label) == "1.2.3"
    assert run.call_args.kwargs["timeout"] == 30


# Check report serialization round trips and the shared writer's exact JSON formatting.
def test_shared_json_writer_round_trip_and_bytes(tmp_path: Path) -> None:
    # Write a real manifest record and check both its contents and exact JSON bytes.
    content_report = load_tool_module("content_report_for_common_tests", "content_report.py")
    source = tmp_path / "input.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b"input")
    manifest_data = content_report.create_manifest(tmp_path)
    path = tmp_path / "generated" / "manifest.json"
    content_report.write_json_atomic(path, manifest_data)
    # Decode the emitted JSON and preserve the established report layout.
    restored = json.loads(path.read_text(encoding="utf-8"))
    assert restored == manifest_data
    assert path.read_bytes() == (json.dumps(manifest_data, indent=2, sort_keys=True) + "\n").encode("utf-8")


# Check report paths reject linked parents independently of the removed set-lock API.
def test_manifest_parent_symlink_is_rejected_before_writing(tmp_path: Path, monkeypatch) -> None:
    helper = load_tool_module("content_report_common_tests", "content_report.py")
    parent = tmp_path / "generated"
    original = Path.is_symlink
    monkeypatch.setattr(Path, "is_symlink", lambda path: path == parent or original(path))
    with pytest.raises(ValueError, match="symbolic links"):
        helper.manifest_path(tmp_path)
    assert not parent.exists()
