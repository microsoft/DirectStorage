from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import sys
from pathlib import Path

import pytest

FACTORY_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = FACTORY_ROOT.parent
TOOLS = FACTORY_ROOT / "tools"


def load(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / filename)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


hlk = load("hlk_content_set", "hlk_content_set.py")
process_set = load("process_set_for_hlk_tests", "process-set.py")
reader = load("validate_hlk_archive_for_hlk_tests", "validate_hlk_archive.py")


def wrapper(root: Path, name: str, helper: str) -> Path:
    path = root / name
    path.write_text(f'@echo off\r\n"{sys.executable}" "{Path(__file__).with_name(helper)}" %*\r\n', encoding="utf-8")
    return path


def initialize(root: Path) -> dict[str, bytes]:
    source_root = root / "originals" / "sample"
    source_root.mkdir(parents=True)
    files = {
        "a.dds": b"texture",
        "b.ply": b"model-data",
        "c.txt": b"text-data",
        "d.bin": bytes(range(251)) * 3,
    }
    for name, data in files.items():
        (source_root / name).write_bytes(data)
    document = process_set.create_manifest(root, "sample")
    process_set.write_json_atomic(process_set.manifest_path(root, "sample"), document)
    return files


def archive_payloads(path: Path) -> tuple[list[int], list[bytes]]:
    entries, _ = reader.parse_archive(path)
    payloads = []
    with path.open("rb") as stream:
        for entry in entries:
            stream.seek(entry.offset)
            payloads.append(stream.read(entry.size))
    return [entry.content_type for entry in entries], payloads


def fake_zstd_decode(data: bytes) -> bytes:
    assert data[:4] == b"FZST"
    size = struct.unpack_from("<I", data, 4)[0]
    assert len(data) == 8 + size
    return data[8:]


def test_builds_lockstep_hlk_triplet_and_manifest(tmp_path: Path) -> None:
    originals = initialize(tmp_path)
    zstd = wrapper(tmp_path, "zstd.cmd", "fake_zstd.py")
    gdeflate = wrapper(tmp_path, "gdeflate.cmd", "fake_gdeflate.py")
    manifest_path = hlk.process(tmp_path, "sample", "dstoragetest", zstd, gdeflate, 1, False)
    document = process_set.load_manifest(manifest_path)
    process_set.verify_files(tmp_path, "sample", document)
    records = {item["payload_codec"]: item for item in document["archives"]}
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


def test_hlk_triplet_is_deterministic_and_requires_overwrite(tmp_path: Path) -> None:
    initialize(tmp_path)
    zstd = wrapper(tmp_path, "zstd.cmd", "fake_zstd.py")
    gdeflate = wrapper(tmp_path, "gdeflate.cmd", "fake_gdeflate.py")
    hlk.process(tmp_path, "sample", "dstoragetest", zstd, gdeflate, 1, False)
    root = tmp_path / "dstorage" / "sample"
    before = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in root.glob("dstoragetest.*")}
    with pytest.raises(FileExistsError, match="--overwrite"):
        hlk.process(tmp_path, "sample", "dstoragetest", zstd, gdeflate, 1, False)
    hlk.process(tmp_path, "sample", "dstoragetest", zstd, gdeflate, 1, True)
    after = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in root.glob("dstoragetest.*")}
    assert after == before


def test_content_type_mapping_matches_internal_hlk_contract() -> None:
    assert hlk.content_type("texture.DDS") == "texture"
    assert hlk.content_type("model.ply") == "geometry"
    assert hlk.content_type("readme.txt") == "text"
    assert hlk.content_type("blob.bin") == "unknown"
    assert hlk.content_type("readme.txt") == "text"
    assert hlk.content_type("blob.bin") == "unknown"


def test_manifest_rejects_incomplete_or_non_lockstep_hlk_groups(tmp_path: Path) -> None:
    initialize(tmp_path)
    zstd = wrapper(tmp_path, "zstd.cmd", "fake_zstd.py")
    gdeflate = wrapper(tmp_path, "gdeflate.cmd", "fake_gdeflate.py")
    manifest_path = hlk.process(tmp_path, "sample", "dstoragetest", zstd, gdeflate, 1, False)
    document = process_set.load_manifest(manifest_path)

    incomplete = json.loads(json.dumps(document))
    incomplete["archives"] = incomplete["archives"][:-1]
    with pytest.raises(ValueError, match="all three"):
        process_set.validate_manifest(incomplete)

    mismatch = json.loads(json.dumps(document))
    target = next(item for item in mismatch["archives"] if item["payload_codec"] == "zstd")
    target["entries"][0]["content_type"] = "unknown" if target["entries"][0]["content_type"] != "unknown" else "text"
    with pytest.raises(ValueError, match="not lockstep"):
        process_set.validate_manifest(mismatch)



@pytest.mark.parametrize("returned_level", [9, 12])
def test_hlk_requests_default_gdeflate_level_and_rejects_mismatch(tmp_path: Path, monkeypatch, returned_level: int) -> None:
    # Inspect the native call and wrapper without launching any executable.
    source = tmp_path / "input.bin"
    source.write_bytes(b"sample")
    calls = []
    payload = b"encoded-payload"

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
        assert hlk.compress_gdeflate(tmp_path / "unused-tool.exe", source, tmp_path) == payload
    else:
        with pytest.raises(RuntimeError, match="HLK payload contract"):
            hlk.compress_gdeflate(tmp_path / "unused-tool.exe", source, tmp_path)
    assert len(calls) == 2
    assert calls[1][1] == "--verify"


@pytest.mark.parametrize("existing", [False, True])
@pytest.mark.parametrize("failure", ["archive_install", "manifest_write", "post_verify"])
def test_hlk_commit_failure_restores_archives_and_manifest(tmp_path: Path, monkeypatch, existing: bool, failure: str) -> None:
    # No codec runs: inject publication failures after constructing valid archive containers.
    initialize(tmp_path)
    placeholder = tmp_path / "unused-tool.exe"
    placeholder.write_bytes(b"never executed")
    monkeypatch.setattr(hlk, "verify_tool", lambda *args: None)
    monkeypatch.setattr(hlk, "compress_zstd", lambda tool, data: b"zstd:" + data)
    monkeypatch.setattr(hlk, "compress_gdeflate", lambda tool, source, workspace: b"gdeflate:" + source.read_bytes())
    if existing:
        hlk.process(tmp_path, "sample", "dstoragetest", placeholder, placeholder, 1, False)
    manifest_path = process_set.manifest_path(tmp_path, "sample")
    old_manifest = manifest_path.read_bytes()
    output_root = tmp_path / "dstorage" / "sample"
    before = {path.name: path.read_bytes() for path in output_root.glob("*") if path.is_file()}
    triggered = []
    original_loader = hlk.load_module
    original_replace = hlk.os.replace

    def fail_once():
        if not triggered:
            triggered.append(failure)
            raise OSError("injected publication failure")

    if failure == "archive_install":
        def replace_with_failure(source, destination):
            if Path(destination).parent == output_root and Path(destination).name == "dstoragetest.gdeflate" and ".hlk.stage." in str(source):
                fail_once()
            return original_replace(source, destination)
        monkeypatch.setattr(hlk.os, "replace", replace_with_failure)
    else:
        def load_with_failure(name, filename):
            module = original_loader(name, filename)
            if filename == "process-set.py":
                if failure == "manifest_write":
                    original_write = module.write_json_atomic
                    def write_with_failure(path, document):
                        fail_once()
                        return original_write(path, document)
                    module.write_json_atomic = write_with_failure
                else:
                    original_verify = module.verify_files
                    calls = []
                    def verify_with_failure(*args):
                        calls.append(True)
                        if len(calls) == 2:
                            fail_once()
                        return original_verify(*args)
                    module.verify_files = verify_with_failure
            return module
        monkeypatch.setattr(hlk, "load_module", load_with_failure)
    with pytest.raises(RuntimeError, match="rolled back"):
        hlk.process(tmp_path, "sample", "dstoragetest", placeholder, placeholder, 1, existing)
    assert triggered == [failure]
    assert manifest_path.read_bytes() == old_manifest
    assert {path.name: path.read_bytes() for path in output_root.glob("*") if path.is_file()} == before
    assert not list(output_root.glob(".hlk.stage.*"))
    assert not list((tmp_path / "manifests").glob("*.lock"))
