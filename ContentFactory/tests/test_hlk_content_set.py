from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

from conftest import assert_report_files, create_content_set, load_tool_module, make_python_tool

import pytest

FACTORY_ROOT = Path(__file__).resolve().parents[1]
TOOLS = FACTORY_ROOT / "tools"


hlk = load_tool_module("hlk_content_set", "hlk_content_set.py")
process_set = load_tool_module("process_set_for_hlk_tests", "content_report.py")
reader = load_tool_module("validate_hlk_archive_for_hlk_tests", "validate_hlk_archive.py")


# Read archive entry types and payload bytes through the independent reader.
def archive_payloads(path: Path) -> tuple[list[int], list[bytes]]:
    entries, _ = reader.parse_archive(path)
    payloads = []
    with path.open("rb") as stream:
        for entry in entries:
            stream.seek(entry.offset)
            payloads.append(stream.read(entry.size))
    return [entry.content_type for entry in entries], payloads


# Validate a single fake Zstd frame and return its uncompressed bytes.
def fake_zstd_decode(data: bytes) -> bytes:
    assert data[:4] == b"FZST"
    size = struct.unpack_from("<I", data, 4)[0]
    assert len(data) == 8 + size
    return data[8:]


# Check lockstep HLK archives, decoded payloads, manifest records, and file verification.
def test_builds_lockstep_hlk_triplet_and_manifest(tmp_path: Path) -> None:
    originals = create_content_set(tmp_path, {"a.dds": b"texture", "b.ply": b"model-data", "c.txt": b"text-data", "d.bin": bytes(range(251)) * 3})
    zstd = make_python_tool(tmp_path, "fake_zstd.py", "zstd")
    gdeflate = make_python_tool(tmp_path, "fake_gdeflate.py", "gdeflate")
    manifest_path = hlk.process(tmp_path, "dstoragetest", zstd, gdeflate, 1, False)
    manifest_data = json.loads((manifest_path).read_text(encoding="utf-8"))
    assert_report_files(tmp_path, manifest_data)
    records = {item["payload_codec"]: item for item in manifest_data["archives"]}
    assert set(records) == {"uncompressed", "gdeflate", "zstd"}
    assert {item["archive_group"] for item in records.values()} == {"dstoragetest"}
    expected_sources = sorted(originals)
    expected_types = [1, 2, 3, 0]
    expected_payloads = [originals[name] for name in expected_sources]

    uncompressed_types, uncompressed = archive_payloads(tmp_path / records["uncompressed"]["path"])
    gdeflate_types, gdeflate_payloads = archive_payloads(tmp_path / records["gdeflate"]["path"])
    zstd_types, zstd_payloads = archive_payloads(tmp_path / records["zstd"]["path"])
    assert uncompressed_types == gdeflate_types == zstd_types == expected_types
    assert uncompressed == expected_payloads
    assert gdeflate_payloads == expected_payloads
    assert [fake_zstd_decode(payload) for payload in zstd_payloads] == expected_payloads
    for record in records.values():
        assert [entry["source"] for entry in record["entries"]] == expected_sources


# Check HLK overwrite refusal and byte-identical regeneration of the archive triplet.
def test_hlk_triplet_is_deterministic_and_requires_overwrite(tmp_path: Path) -> None:
    create_content_set(tmp_path, {"a.dds": b"texture", "b.ply": b"model-data", "c.txt": b"text-data", "d.bin": bytes(range(251)) * 3})
    zstd = make_python_tool(tmp_path, "fake_zstd.py", "zstd")
    gdeflate = make_python_tool(tmp_path, "fake_gdeflate.py", "gdeflate")
    hlk.process(tmp_path, "dstoragetest", zstd, gdeflate, 1, False)
    root = tmp_path / "generated" / "hlk"
    before = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in root.glob("dstoragetest.*")}
    with pytest.raises(FileExistsError, match="--overwrite"):
        hlk.process(tmp_path, "dstoragetest", zstd, gdeflate, 1, False)
    hlk.process(tmp_path, "dstoragetest", zstd, gdeflate, 1, True)
    after = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in root.glob("dstoragetest.*")}
    assert after == before


# Check extension-based mapping to texture, geometry, text, and unknown content types.
def test_content_type_mapping_matches_internal_hlk_contract() -> None:
    assert hlk.content_type("texture.DDS") == "texture"
    assert hlk.content_type("model.ply") == "geometry"
    assert hlk.content_type("readme.txt") == "text"
    assert hlk.content_type("blob.bin") == "unknown"



# Check one level-9 GDeflate compression call and rejection of mismatched headers.
@pytest.mark.parametrize("returned_level", [9, 12])
def test_hlk_requests_default_gdeflate_level_and_rejects_mismatch(tmp_path: Path, monkeypatch, returned_level: int) -> None:
    # Inspect the native call and wrapper without launching any executable.
    source = tmp_path / "input.bin"
    source.write_bytes(b"sample")
    calls = []
    payload = b"encoded-payload"

    # Capture native calls and write a fake GDeflate envelope with the selected returned level.
    def fake_run_tool(arguments, **kwargs):
        calls.append(arguments)
        if "--output" in arguments:
            assert arguments[arguments.index("--level") + 1] == "9"
            output = Path(arguments[arguments.index("--output") + 1])
            header = struct.pack("<IHHIIQQ", 0x31464447, 1, 32, returned_level, 0,
                                 source.stat().st_size, len(payload))
            output.write_bytes(header + payload)

    monkeypatch.setattr(hlk, "run_tool", fake_run_tool)
    if returned_level == 9:
        assert hlk.compress_gdeflate(tmp_path / "generated" / "unused-tool.exe", source, tmp_path) == payload
    else:
        with pytest.raises(RuntimeError, match="HLK payload contract"):
            hlk.compress_gdeflate(tmp_path / "generated" / "unused-tool.exe", source, tmp_path)
    assert len(calls) == 1
    assert "--verify" not in calls[0]


# Prepare in-process encoders so lifecycle failure regressions cannot launch real codecs.
def prepare_hlk_without_codecs(directory: Path, monkeypatch) -> Path:
    create_content_set(directory, {"input.bin": b"old input"})
    tool = directory / "generated" / "unused-tool.exe"
    tool.parent.mkdir(exist_ok=True)
    tool.write_bytes(b"never executed")
    monkeypatch.setattr(hlk, "verify_tool", lambda *args: "test codec 1.0.0")
    monkeypatch.setattr(hlk, "compress_zstd", lambda executable, data: b"zstd:" + data)
    monkeypatch.setattr(hlk, "compress_gdeflate", lambda executable, source, workspace: b"gdeflate:" + source.read_bytes())
    return tool


# Check a failed staged archive build preserves all three old outputs, report bytes, and shared report lists.
def test_hlk_build_failure_preserves_existing_triplet_and_report(tmp_path: Path, monkeypatch) -> None:
    tool = prepare_hlk_without_codecs(tmp_path, monkeypatch)
    path = hlk.process(tmp_path, "dstoragetest", tool, tool, 1, False)
    output = tmp_path / "generated" / "hlk"
    before = {item.name: item.read_bytes() for item in output.glob("dstoragetest.*")}
    report_bytes = path.read_bytes()
    shared = process_set.create_manifest(tmp_path)
    original_report = json.loads(json.dumps(shared))
    stages = []
    original_build = hlk.build_archives

    # Complete archive construction before failing, confirming nothing old was deleted during generation.
    def fail_build(staging, sources, *args):
        stages.append(staging)
        assert sources is shared["sources"]
        assert {item.name: item.read_bytes() for item in output.glob("dstoragetest.*")} == before
        assert path.read_bytes() == report_bytes
        assert not (tmp_path / "generated" / ".factory.lock").exists()
        original_build(staging, sources, *args)
        raise RuntimeError("injected staged archive failure")

    monkeypatch.setattr(hlk, "build_archives", fail_build)
    with pytest.raises(RuntimeError, match="injected staged archive failure"):
        hlk.process(tmp_path, "dstoragetest", tool, tool, 1, True, report=shared)
    assert {item.name: item.read_bytes() for item in output.glob("dstoragetest.*")} == before
    assert path.read_bytes() == report_bytes
    assert shared == original_report
    assert len(stages) == 1 and not stages[0].exists()
    assert not list(output.glob("*.backup*"))


# Check sequential archive installation/report failures can retain new files and overwrite reruns recover all outputs.
@pytest.mark.parametrize("failure", ["archive_install", "report_before", "report_after"])
def test_hlk_publication_failure_and_overwrite_recovery(tmp_path: Path, monkeypatch, failure: str) -> None:
    tool = prepare_hlk_without_codecs(tmp_path, monkeypatch)
    path = hlk.process(tmp_path, "dstoragetest", tool, tool, 1, False)
    output = tmp_path / "generated" / "hlk"
    before = {item.name: item.read_bytes() for item in output.glob("dstoragetest.*")}
    old_report = path.read_bytes()
    (tmp_path / "input.bin").write_bytes(b"new input, new archives")
    shared = process_set.create_manifest(tmp_path)
    archives = shared["archives"]
    tools = shared["tools"]
    stages = []
    original_build = hlk.build_archives
    original_replace = hlk.os.replace
    original_write = process_set.write_json_atomic
    destinations = []
    triggered = []

    # Observe completed staged files without replacing report lists or tool dictionaries.
    def build(staging, sources, *args):
        stages.append(staging)
        assert sources is shared["sources"]
        assert {item.name: item.read_bytes() for item in output.glob("dstoragetest.*")} == before
        assert not (tmp_path / "generated" / ".factory.lock").exists()
        return original_build(staging, sources, *args)

    # Fail the second sequential replacement so the first replacement remains installed.
    def replace(source, destination):
        destination = Path(destination)
        if destination.parent == output and destination.name in before:
            destinations.append(destination.name)
            if failure == "archive_install" and len(destinations) == 2:
                triggered.append(failure)
                raise OSError("injected archive publication failure")
        return original_replace(source, destination)

    # Fail report publication either before or after the atomic write, without restoring prior archives.
    def write(report_path, report):
        triggered.append(failure)
        assert report is shared
        assert report["archives"] is archives
        assert report["tools"] is tools
        if failure == "report_after":
            original_write(report_path, report)
        raise OSError("injected archive publication failure")

    monkeypatch.setattr(hlk, "build_archives", build)
    with monkeypatch.context() as publication:
        publication.setattr(hlk.os, "replace", replace)
        if failure != "archive_install":
            publication.setattr(process_set, "write_json_atomic", write)
        with pytest.raises(OSError, match="injected archive publication failure"):
            hlk.process(tmp_path, "dstoragetest", tool, tool, 1, True, report=shared)
    assert triggered == [failure]
    if failure == "archive_install":
        assert len(destinations) == 2
        assert (output / destinations[0]).read_bytes() != before[destinations[0]]
        assert (output / destinations[1]).read_bytes() == before[destinations[1]]
        assert archives == []
    else:
        assert len(destinations) == 3
        assert all((output / name).read_bytes() != old for name, old in before.items())
        assert len(archives) == 3
    if failure == "report_after":
        assert json.loads(path.read_text(encoding="utf-8")) == shared
    else:
        assert path.read_bytes() == old_report
    assert all(not stage.exists() for stage in stages)
    assert not (tmp_path / "generated" / ".factory.lock").exists()
    assert not list(output.glob("*.backup*"))
    monkeypatch.setattr(hlk, "build_archives", original_build)
    assert hlk.process(tmp_path, "dstoragetest", tool, tool, 1, True) == path
    recovered = json.loads(path.read_text(encoding="utf-8"))
    assert len(recovered["archives"]) == 3
    assert_report_files(tmp_path, recovered)
    assert all((output / name).read_bytes() != old for name, old in before.items())
    assert (tmp_path / "input.bin").read_bytes() == b"new input, new archives"


# Check archive names rejected by generated-path rules fail before codec execution.
@pytest.mark.parametrize("prefix", ["bad:name", "bad/name", "bad\\name", "bad*name", "bad?name", "bad|name", "bad<name", 'bad"name', "trailing.", "trailing ", "bad\nname"])
def test_hlk_rejects_unsafe_prefix_before_running_tools(tmp_path: Path, monkeypatch, prefix: str) -> None:
    # A bad prefix must fail before executable validation, inventory reads, or output creation.
    def unexpected_tool(*args, **kwargs):
        pytest.fail("unsafe archive prefix reached a codec")
    monkeypatch.setattr(hlk, "verify_tool", unexpected_tool)
    with pytest.raises(ValueError, match="safe file name"):
        hlk.process(tmp_path, prefix, tmp_path / "missing-zstd", tmp_path / "missing-gdeflate", 1, False)
    assert not (tmp_path / "generated").exists()


# Check standalone HLK overwrite records current versions without merging old derivative provenance.
def test_hlk_records_encoder_versions_separately_on_overwrite(tmp_path: Path, monkeypatch) -> None:
    create_content_set(tmp_path, {"input.bin": b"original"})
    zstd = make_python_tool(tmp_path, "fake_zstd.py", "zstd")
    gdeflate = make_python_tool(tmp_path, "fake_gdeflate.py", "gdeflate")
    versions = {zstd: "zstd 1.5.7", gdeflate: "GDeflateContentTool 0.1.0"}
    monkeypatch.setattr(hlk, "verify_tool", lambda executable: versions[executable])
    path = hlk.process(tmp_path, "archive", zstd, gdeflate, 1, False)
    first = json.loads((path).read_text(encoding="utf-8"))
    assert first["tools"]["hlk-zstd"]["version"] == versions[zstd]
    assert first["tools"]["hlk-gdeflate"]["version"] == versions[gdeflate]
    first["tools"]["zstd"] = {"version": "1.5.6"}
    process_set.write_json_atomic(path, first)
    versions[zstd] = "zstd 1.5.8"
    versions[gdeflate] = "GDeflateContentTool 0.2.0"
    hlk.process(tmp_path, "archive", zstd, gdeflate, 1, True)
    after = json.loads((path).read_text(encoding="utf-8"))
    assert after["tools"]["hlk-zstd"]["version"] == versions[zstd]
    assert after["tools"]["hlk-gdeflate"]["version"] == versions[gdeflate]
    assert "zstd" not in after["tools"]
    assert after["derivatives"] == []
    assert_report_files(tmp_path, after)



# Check a codec without a version cannot create an archive with missing provenance.
def test_hlk_rejects_empty_encoder_version_banner(tmp_path: Path, monkeypatch) -> None:
    from types import SimpleNamespace
    executable = tmp_path / "tool.exe"
    executable.write_bytes(b"placeholder")
    monkeypatch.setattr(hlk, "run_tool", lambda *args, **kwargs: SimpleNamespace(stdout=b"  \n"))
    with pytest.raises(RuntimeError, match="empty version banner"):
        hlk.verify_tool(executable)


# Check standalone HLK rebuilds changed originals despite corrupt old archives and missing or unreadable reports.
@pytest.mark.parametrize("old_report", ["missing", "corrupt"])
def test_hlk_overwrite_uses_fresh_sources_not_saved_report(tmp_path: Path, old_report: str) -> None:
    source = tmp_path / "source.bin"
    source.write_bytes(b"original")
    zstd_tool = make_python_tool(tmp_path, "fake_zstd.py", "fresh-hlk-zstd")
    gdeflate_tool = make_python_tool(tmp_path, "fake_gdeflate.py", "fresh-hlk-gdeflate")
    path = hlk.process(tmp_path, "dstoragetest", zstd_tool, gdeflate_tool, 1, False)
    first = json.loads(path.read_text(encoding="utf-8"))
    for record in first["archives"]:
        (tmp_path / record["path"]).write_bytes(b"corrupt old archive")
    source.write_bytes(b"changed original")
    if old_report == "missing":
        path.unlink()
    else:
        path.write_bytes(b"invalid old report")
    before_report = path.read_bytes() if path.exists() else None
    with pytest.raises(FileExistsError, match="overwrite"):
        hlk.process(tmp_path, "dstoragetest", zstd_tool, gdeflate_tool, 1, False)
    assert (path.read_bytes() if path.exists() else None) == before_report
    assert hlk.process(tmp_path, "dstoragetest", zstd_tool, gdeflate_tool, 1, True) == path
    current = json.loads(path.read_text(encoding="utf-8"))
    assert current["sources"] != first["sources"]
    assert current["derivatives"] == []
    assert {record["payload_codec"] for record in current["archives"]} == {"uncompressed", "zstd", "gdeflate"}
    assert source.read_bytes() == b"changed original"
    assert_report_files(tmp_path, current)
    for record in current["archives"]:
        entries, _ = reader.parse_archive(tmp_path / record["path"])
        assert len(entries) == 1
        assert record["entries"][0]["source"] == "source.bin"
        if record["payload_codec"] == "uncompressed":
            assert entries[0].size == len(b"changed original")
            payload = (tmp_path / record["path"]).read_bytes()[entries[0].offset:entries[0].offset + entries[0].size]
            assert payload == b"changed original"
