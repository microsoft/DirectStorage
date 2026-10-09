#!/usr/bin/env python3
"""Build the lockstep uncompressed, GDeflate, and Zstd archives consumed by DirectStorage HLK tests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path
from functools import partial
from typing import Any

from factory_common import (
    load_module, set_lock as common_set_lock,
    run_command, validate_executable, write_bytes_atomic,
)
from factory_contracts import ZSTD_COMPRESSION_LEVEL, parse_gdeflate_header


# Classify known source extensions; other files remain unknown.
TYPE_BY_EXTENSION = {
    ".dds": "texture",
    ".ply": "geometry",
    ".txt": "text",
}


def run_tool(arguments: list[str], *, input_data: bytes | None = None, timeout: int = 300):
    return run_command(arguments, operation="HLK tool", input_data=input_data, timeout=timeout)

def verify_tool(executable: Path, version_argument: str = "--version") -> str:
    executable = validate_executable(executable)
    result = run_tool([str(executable), version_argument], timeout=30)
    return result.stdout.decode(errors="replace").strip()

def content_type(path: str) -> str:
    return TYPE_BY_EXTENSION.get(Path(path).suffix.lower(), "unknown")


def compress_zstd(executable: Path, source: bytes) -> bytes:
    # Share level 19 with other stages, but preserve the single-frame / 256 KiB window contract.
    result = run_tool([
        str(executable), f"-{ZSTD_COMPRESSION_LEVEL}", "--zstd=wlog=18", f"--stream-size={len(source)}", "-q", "-c",
    ], input_data=source)
    # Decode the frame immediately and compare the original bytes.
    restored = run_tool([str(executable), "-q", "-d", "-c"], input_data=result.stdout)
    if restored.stdout != source:
        raise RuntimeError("Zstd entry failed standalone-frame round-trip validation")
    return result.stdout


def compress_gdeflate(executable: Path, source_path: Path, temporary_root: Path) -> bytes:
    # Use a path-based filename so temporary outputs from different sources do not collide.
    output = temporary_root / f"{hashlib.sha256(source_path.as_posix().encode()).hexdigest()}.gdeflate"
    run_tool([
        str(executable), "--input", str(source_path), "--output", str(output),
        "--level", "9", "--overwrite",
    ])
    run_tool([str(executable), "--verify", str(output)])
    data = output.read_bytes()
    # Check the file envelope and the level-9 HLK setting.
    header = parse_gdeflate_header(data)
    if header["level"] != 9:
        raise RuntimeError("GDeflate output header does not match the HLK payload contract")
    # Archives store the compressed stream, without the native tool's file header.
    payload = data[header["header_size"]:]
    if header["uncompressed_size"] != source_path.stat().st_size:
        raise RuntimeError("GDeflate output sizes do not match the source or payload")
    return payload


def write_entry_manifest(path: Path, entries: list[dict[str, str]]) -> None:
    path.write_text(json.dumps({"entries": entries}, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def verify_archive_payloads(
    archive: Path,
    expected: list[bytes],
    archive_module: Any,
) -> None:
    # Check archive structure with the independent reader before comparing payloads.
    entries, _ = archive_module.parse_archive(archive)
    if len(entries) != len(expected):
        raise RuntimeError("HLK archive entry count mismatch")
    with archive.open("rb") as stream:
        for index, (entry, payload) in enumerate(zip(entries, expected)):
            stream.seek(entry.offset)
            if stream.read(entry.size) != payload:
                raise RuntimeError(f"HLK archive payload mismatch at entry {index}")


set_lock = partial(common_set_lock, stage="hlk", label="HLK")


def validate_options(archive_prefix: str, alignment: int) -> None:
    if not archive_prefix or Path(archive_prefix).name != archive_prefix or archive_prefix in {".", ".."}:
        raise ValueError("archive prefix must be a safe file name")
    if alignment <= 0 or alignment & (alignment - 1):
        raise ValueError("alignment must be a positive power of two")


def process(
    root: Path,
    set_name: str,
    archive_prefix: str,
    zstd_executable: Path,
    gdeflate_executable: Path,
    alignment: int,
    overwrite: bool,
) -> Path:
    validate_options(archive_prefix, alignment)
    # Use separate helpers for inventory, archive creation, and archive reading.
    driver = load_module("process_set_for_hlk", "process-set.py")
    creator = load_module("create_hlk_archive_for_hlk", "create_hlk_archive.py")
    reader = load_module("validate_hlk_archive_for_hlk", "validate_hlk_archive.py")
    root = root.resolve()
    set_name = driver.validate_set_name(set_name)
    verify_tool(zstd_executable)
    verify_tool(gdeflate_executable)
    zstd_executable = zstd_executable.resolve()
    gdeflate_executable = gdeflate_executable.resolve()
    manifest_path = driver.manifest_path(root, set_name)

    with set_lock(root, set_name):
        document = driver.load_manifest(manifest_path)
        driver.verify_files(root, set_name, document)
        # Use one source order for all three archives so matching entries line up.
        sources = sorted(document["sources"], key=lambda item: item["path"])
        output_root = root / "dstorage" / set_name
        output_root.mkdir(parents=True, exist_ok=True)
        # Each codec gets its own archive, grouped under the same prefix.
        filenames = {
            "uncompressed": f"{archive_prefix}.uncompressed",
            "gdeflate": f"{archive_prefix}.gdeflate",
            "zstd": f"{archive_prefix}.zstd",
        }
        final_paths = {codec: output_root / name for codec, name in filenames.items()}
        relative_paths = {codec: path.relative_to(root).as_posix() for codec, path in final_paths.items()}
        existing_records = {item["path"]: item for item in document["archives"]}
        if any(path.exists() or relative_paths[codec] in existing_records for codec, path in final_paths.items()) and not overwrite:
            raise FileExistsError("HLK content archives already exist; use --overwrite")

        # Keep new archives and recovery copies in a temporary working directory.
        stage = Path(tempfile.mkdtemp(prefix=f".{set_name}.hlk.stage.", dir=output_root))
        backups: dict[str, Path] = {}
        old_manifest = manifest_path.read_bytes()
        installed: list[Path] = []
        try:
            temporary_codec = stage / "codec"
            temporary_codec.mkdir()
            original_payloads: list[bytes] = []
            gdeflate_payloads: list[bytes] = []
            zstd_payloads: list[bytes] = []
            entry_specs: list[dict[str, str]] = []
            # Prepare all three payload forms from each original in the same order.
            for source in sources:
                source_path = root / "originals" / set_name / source["path"]
                original = source_path.read_bytes()
                original_payloads.append(original)
                gdeflate_payloads.append(compress_gdeflate(gdeflate_executable, source_path, temporary_codec))
                zstd_payloads.append(compress_zstd(zstd_executable, original))
                entry_specs.append({"path": source["path"], "content_type": content_type(source["path"])})

            staged_paths: dict[str, Path] = {}
            payload_sets = {
                "uncompressed": original_payloads,
                "gdeflate": gdeflate_payloads,
                "zstd": zstd_payloads,
            }
            # Build each archive from its staged payloads and verify the stored bytes.
            for codec, payloads in payload_sets.items():
                payload_root = stage / codec
                payload_root.mkdir()
                entries: list[dict[str, str]] = []
                for index, (source, payload, spec) in enumerate(zip(sources, payloads, entry_specs)):
                    payload_path = payload_root / f"{index:08d}.bin"
                    payload_path.write_bytes(payload)
                    entries.append({"path": payload_path.relative_to(stage).as_posix(), "content_type": spec["content_type"]})
                entry_manifest = stage / f"{codec}.json"
                write_entry_manifest(entry_manifest, entries)
                archive_path = stage / filenames[codec]
                creator.create_archive(entry_manifest, archive_path, stage, alignment)
                verify_archive_payloads(archive_path, payloads, reader)
                staged_paths[codec] = archive_path

            # Describe identical source membership for every archive in the triplet.
            records = []
            for codec, archive_path in staged_paths.items():
                data = archive_path.read_bytes()
                records.append({
                    "path": relative_paths[codec],
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                    "format": "dstorage",
                    "archive_group": archive_prefix,
                    "payload_codec": codec,
                    "entries": [
                        {"index": index, "source": source["path"], "content_type": spec["content_type"]}
                        for index, (source, spec) in enumerate(zip(sources, entry_specs))
                    ],
                    "validation": "pass",
                })
            updated = dict(document)
            replaced = set(relative_paths.values())
            updated["archives"] = sorted(
                [item for item in document["archives"] if item["path"] not in replaced] + records,
                key=lambda item: item["path"],
            )
            updated["tools"] = dict(document["tools"])
            updated["tools"]["makehlkcontent-compatible"] = {"version": "1"}
            driver.validate_manifest(updated)

            # Install the verified triplet and then update the consolidated manifest.
            try:
                for codec, final_path in final_paths.items():
                    if final_path.exists():
                        backup = stage / f"{codec}.backup"
                        os.replace(final_path, backup)
                        backups[codec] = backup
                    os.replace(staged_paths[codec], final_path)
                    installed.append(final_path)
                driver.write_json_atomic(manifest_path, updated)
                driver.verify_files(root, set_name, updated)
            # Remove partial replacements and attempt to restore all previous files.
            except Exception as original_error:
                rollback_errors = []
                for path in installed:
                    try:
                        path.unlink(missing_ok=True)
                    except Exception as error:
                        rollback_errors.append(str(error))
                for codec, backup in backups.items():
                    try:
                        os.replace(backup, final_paths[codec])
                    except Exception as error:
                        rollback_errors.append(str(error))
                try:
                    write_bytes_atomic(manifest_path, old_manifest)
                except Exception as error:
                    rollback_errors.append(str(error))
                if rollback_errors:
                    raise RuntimeError(
                        f"HLK commit failed ({original_error}); rollback also failed: {'; '.join(rollback_errors)}"
                    ) from original_error
                raise RuntimeError(f"HLK commit failed and was rolled back: {original_error}") from original_error
            return manifest_path
        # Release the temporary workspace when this operation exits.
        finally:
            shutil.rmtree(stage, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("set_name")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    # Both codec paths are required to build a complete triplet.
    parser.add_argument("--archive-prefix", default="dstoragetest")
    parser.add_argument("--zstd-exe", type=Path, required=True)
    parser.add_argument("--gdeflate-exe", type=Path, required=True)
    parser.add_argument("--alignment", type=int, default=1)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    try:
        output = process(
            args.root, args.set_name, args.archive_prefix, args.zstd_exe,
            args.gdeflate_exe, args.alignment, args.overwrite,
        )
        print(f"Updated {output}")
        return 0
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
