"""Regression tests for internal source discovery and generation-report helpers."""

import hashlib
import json
from pathlib import Path

import pytest

from conftest import load_tool_module

CONTENT_REPORT_SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "content_report.py"


# Load discovery helpers without depending on any report already on disk.
def module():
    return load_tool_module("content_report_tests", "content_report.py")


# Serialize a freshly discovered report through the internal helpers without a production entry point.
def write_report(directory: Path) -> Path:
    helper = module()
    report = helper.create_manifest(directory)
    path = helper.manifest_path(directory)
    helper.write_json_atomic(path, report)
    return path


# Check fresh records, deterministic JSON, and preservation of original file bytes and timestamps.
def test_discovery_and_report_serialization(tmp_path: Path) -> None:
    helper = module()
    (tmp_path / "nested").mkdir()
    first = tmp_path / "a.bin"
    second = tmp_path / "nested" / "z.txt"
    first.write_bytes(b"first")
    second.write_bytes(b"second")
    before = {path: (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_ino)
              for path in (first, second)}
    report = helper.create_manifest(tmp_path)
    assert set(report) == {"sources", "derivatives", "archives", "tools"}
    assert report["sources"] == [
        {"path": "a.bin", "size": 5, "sha256": hashlib.sha256(b"first").hexdigest()},
        {"path": "nested/z.txt", "size": 6, "sha256": hashlib.sha256(b"second").hexdigest()},
    ]
    assert report["derivatives"] == report["archives"] == []
    path = write_report(tmp_path)
    assert path == tmp_path / "generated" / "manifest.json"
    assert json.loads(path.read_text(encoding="utf-8")) == report
    assert path.read_bytes() == (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8")
    assert {path: (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_ino)
            for path in (first, second)} == before
    assert not (path.parent / ".factory.lock").exists()


# Check old report bytes are irrelevant even if malformed or inconsistent with current originals.
@pytest.mark.parametrize("old_report", [b"not json", b"null", b'{"sources":"unsafe", "schema_version":999}', b"{}"])
def test_current_discovery_ignores_existing_report(tmp_path: Path, old_report: bytes) -> None:
    helper = module()
    original = tmp_path / "input.bin"
    original.write_bytes(b"current original")
    path = helper.manifest_path(tmp_path)
    path.parent.mkdir()
    path.write_bytes(old_report)
    generated = path.parent / "zstd"
    generated.mkdir()
    output = generated / "previous.zst"
    output.write_bytes(b"previous result")
    write_report(tmp_path)
    report = json.loads(path.read_text(encoding="utf-8"))
    assert report["sources"][0]["sha256"] == hashlib.sha256(b"current original").hexdigest()
    assert report["derivatives"] == report["archives"] == []
    assert output.read_bytes() == b"previous result"


# Check discovery reflects additions, changes, removals, and renames rather than an old inventory.
def test_each_invocation_discovers_current_originals(tmp_path: Path) -> None:
    first = tmp_path / "first.bin"
    first.write_bytes(b"initial")
    write_report(tmp_path)
    first.write_bytes(b"updated")
    (tmp_path / "new.bin").write_bytes(b"new")
    path = write_report(tmp_path)
    report = json.loads(path.read_text(encoding="utf-8"))
    assert [record["path"] for record in report["sources"]] == ["first.bin", "new.bin"]
    assert report["sources"][0]["sha256"] == hashlib.sha256(b"updated").hexdigest()
    first.unlink()
    (tmp_path / "new.bin").rename(tmp_path / "renamed.bin")
    write_report(tmp_path)
    assert [record["path"] for record in json.loads(path.read_text(encoding="utf-8"))["sources"]] == ["renamed.bin"]


# Check recursive input discovery skips only the reserved root output tree in any casing.
@pytest.mark.parametrize("output_name", ["generated", "Generated", "GENERATED"])
def test_inventory_excludes_generated_tree_but_preserves_nested_originals(tmp_path: Path, output_name: str) -> None:
    helper = module()
    (tmp_path / "original.bin").write_bytes(b"original")
    nested = tmp_path / "assets" / "generated"
    nested.mkdir(parents=True)
    (nested / "legitimate.bin").write_bytes(b"nested original")
    output = tmp_path / output_name / "zstd"
    output.mkdir(parents=True)
    (output / "previous.zst").write_bytes(b"generated output")
    (tmp_path / output_name / ".factory.lock").write_bytes(b"lock")
    (tmp_path / output_name / "manifest.json").write_bytes(b"manifest")
    assert [record["path"] for record in helper.inventory_sources(tmp_path)] == [
        "assets/generated/legitimate.bin", "original.bin"]


# Check missing input, file input, and empty input fail before publishing a report.
@pytest.mark.parametrize("kind", ["missing", "file", "empty", "generated_only"])
def test_invalid_input_rejected_without_report(tmp_path: Path, kind: str) -> None:
    directory = tmp_path / "input"
    if kind == "file":
        directory.write_bytes(b"not a directory")
    elif kind != "missing":
        directory.mkdir()
        if kind == "generated_only":
            (directory / "generated").mkdir()
            (directory / "generated" / "old.bin").write_bytes(b"output")
    with pytest.raises((FileNotFoundError, ValueError)):
        write_report(directory)
    assert not (directory / "generated" / "manifest.json").exists()


# Check directory names do not need a separate logical-name contract.
def test_input_directory_with_spaces_and_dots(tmp_path: Path) -> None:
    directory = tmp_path / "content v2.0"
    directory.mkdir()
    (directory / "input.bin").write_bytes(b"input")
    path = write_report(directory)
    assert json.loads(path.read_text(encoding="utf-8"))["sources"][0]["path"] == "input.bin"


# Check discovery refuses linked originals or directory components before following them.
@pytest.mark.parametrize("linked_part", ["root", "file", "directory"])
def test_discovery_rejects_linked_input(tmp_path: Path, monkeypatch, linked_part: str) -> None:
    original = tmp_path / "input.bin"
    original.write_bytes(b"original")
    nested = tmp_path / "nested"
    nested.mkdir()
    linked = {"root": tmp_path, "file": original, "directory": nested}[linked_part]
    real = Path.is_symlink
    monkeypatch.setattr(Path, "is_symlink", lambda path: path == linked or real(path))
    with pytest.raises(ValueError, match="symbolic link"):
        write_report(tmp_path)
    assert not (tmp_path / "generated").exists()


# Check output-parent redirects cannot put the report outside the input directory.
def test_linked_report_parent_rejected(tmp_path: Path, monkeypatch) -> None:
    (tmp_path / "input.bin").write_bytes(b"input")
    parent = tmp_path / "generated"
    real = Path.is_symlink
    monkeypatch.setattr(Path, "is_symlink", lambda path: path == parent or real(path))
    with pytest.raises(ValueError, match="symbolic links"):
        write_report(tmp_path)
    assert not parent.exists()


# Check unreadable original folders abort rather than silently producing a partial report.
def test_inventory_propagates_nested_directory_read_errors(tmp_path: Path, monkeypatch) -> None:
    helper = module()
    (tmp_path / "input.bin").write_bytes(b"original")
    # Simulate traversal failure through the explicit walk callback.
    def unreadable_walk(top, *, followlinks, onerror):
        assert followlinks is False
        yield str(top), [], ["input.bin"]
        onerror(PermissionError("nested directory is unreadable"))
    monkeypatch.setattr(helper.os, "walk", unreadable_walk)
    with pytest.raises(PermissionError, match="unreadable"):
        write_report(tmp_path)
    assert not (tmp_path / "generated").exists()


# Check generated paths retain canonical relative-file safety independently of old report validation.
@pytest.mark.parametrize("path", ["../escape", "/absolute", "a//b", "a/./b", "a\\b", "a:", "", "a/"])
def test_original_path_safety(path: str) -> None:
    with pytest.raises(ValueError):
        module().validate_relative_path(path, "original path")


# Check current discovery/report code has no saved-manifest validators or file reconciliation API.
def test_no_manifest_validation_or_reconciliation_methods() -> None:
    helper = module()
    for method in ("validate_manifest", "load_manifest", "verify_files", "validate_file_record", "validate_record_keys"):
        assert not hasattr(helper, method)
    assert not (CONTENT_REPORT_SCRIPT.parent / "content-set-manifest.schema.json").exists()


# Check the report module is internal, imported once, and cannot run a processing CLI.
def test_content_report_has_no_cli_or_processing_entry_point(tmp_path: Path) -> None:
    import importlib

    helper = module()
    assert helper is importlib.import_module("content_report")
    assert not (CONTENT_REPORT_SCRIPT.parent / "process-set.py").exists()
    assert not hasattr(helper, "main")
    assert not hasattr(helper, "process")
    (tmp_path / "input.bin").write_bytes(b"input")
    report = helper.create_manifest(tmp_path)
    assert report["tools"] == {"content_report.py": {"version": helper.TOOL_VERSION}}
    assert not (tmp_path / "generated").exists()
