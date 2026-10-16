#!/usr/bin/env python3
"""Build the lockstep uncompressed, GDeflate, and Zstd archives consumed by DirectStorage HLK tests."""

from __future__ import annotations

import hashlib
import os
import tempfile
from pathlib import Path
from typing import Any

from factory_common import (
    reject_symlink_components, run_command, validate_executable,
)
import content_report
import create_hlk_archive
import validate_hlk_archive
from factory_contracts import CONTENT_TYPES, ZSTD_COMPRESSION_LEVEL, parse_gdeflate_header


# Classify known source extensions; other files remain unknown.
TYPE_BY_EXTENSION = {
    ".dds": "texture",
    ".ply": "geometry",
    ".txt": "text",
}


# Run an HLK codec command with captured binary output and checked exit status.
def run_tool(arguments: list[str], *, input_data: bytes | None = None, timeout: int = 300):
    return run_command(arguments, operation="HLK tool", input_data=input_data, timeout=timeout)

# Validate a codec executable and read its version banner.
def verify_tool(executable: Path, version_argument: str = "--version") -> str:
    executable = validate_executable(executable)
    result = run_tool([str(executable), version_argument], timeout=30)
    version = result.stdout.decode(errors="replace").strip()
    if not version:
        raise RuntimeError(f"HLK codec returned an empty version banner: {executable}")
    return version

# Map a source extension to the archive content type, defaulting to unknown.
def content_type(path: str) -> str:
    return TYPE_BY_EXTENSION.get(Path(path).suffix.lower(), "unknown")


# Encode one bounded-window Zstd frame and compare its decoded bytes with the source.
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


# Encode a verified level-nine GDeflate file and return its payload without the wrapper.
def compress_gdeflate(executable: Path, source_path: Path, temporary_root: Path) -> bytes:
    # Use a path-based filename so temporary outputs from different sources do not collide.
    output = temporary_root / f"{hashlib.sha256(source_path.as_posix().encode()).hexdigest()}.gdeflate"
    run_tool([
        str(executable), "--input", str(source_path), "--output", str(output),
        "--level", "9", "--overwrite",
    ])
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


# Validate archive structure and compare each stored entry with its expected payload.
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


# Check the archive filename prefix and power-of-two alignment.
def validate_options(archive_prefix: str, alignment: int) -> None:
    if (not archive_prefix or archive_prefix in {".", ".."}
            or any(character in archive_prefix for character in '/\\:<>"|?*')
            or any(ord(character) < 32 for character in archive_prefix)
            or archive_prefix.endswith((".", " "))):
        raise ValueError("archive prefix must be a safe file name")
    if alignment <= 0 or alignment & (alignment - 1):
        raise ValueError("alignment must be a positive power of two")


# Prepare lockstep original, GDeflate, and Zstd payloads with their entry types.
def prepare_payloads(
    input_directory: Path,
    sources: list[dict[str, Any]],
    staging_directory: Path,
    zstd_executable: Path,
    gdeflate_executable: Path,
) -> dict[str, list[bytes]]:
    temporary_codec = staging_directory / "codec"
    temporary_codec.mkdir()
    original_payloads: list[bytes] = []
    gdeflate_payloads: list[bytes] = []
    zstd_payloads: list[bytes] = []
    for source in sources:
        source_path = reject_symlink_components(input_directory, source["path"], "original file")
        original = source_path.read_bytes()
        original_payloads.append(original)
        gdeflate_payloads.append(compress_gdeflate(gdeflate_executable, source_path, temporary_codec))
        zstd_payloads.append(compress_zstd(zstd_executable, original))
    payloads_by_codec = {
        "uncompressed": original_payloads,
        "gdeflate": gdeflate_payloads,
        "zstd": zstd_payloads,
    }
    return payloads_by_codec


# Build and verify each archive, then collect its ordered manifest record.
def build_archives(
    staging_directory: Path,
    sources: list[dict[str, Any]],
    payloads_by_codec: dict[str, list[bytes]],
    filenames: dict[str, str],
    relative_paths: dict[str, str],
    archive_prefix: str,
    alignment: int,
    creator: Any,
    reader: Any,
) -> tuple[dict[str, Path], list[dict[str, Any]]]:
    staged_archive_paths: dict[str, Path] = {}
    archive_records: list[dict[str, Any]] = []
    for codec, payloads in payloads_by_codec.items():
        if len(payloads) != len(sources):
            raise ValueError("archive payload count does not match original-file count")
        # Payload bytes are already available; pass them straight to the internal writer.
        entries = [(CONTENT_TYPES[content_type(source["path"])], payload)
                   for source, payload in zip(sources, payloads)]
        archive_path = staging_directory / filenames[codec]
        creator.create_archive(entries, archive_path, alignment)
        verify_archive_payloads(archive_path, payloads, reader)
        staged_archive_paths[codec] = archive_path
        data = archive_path.read_bytes()
        archive_records.append({
            "path": relative_paths[codec],
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "format": "dstorage",
            "archive_group": archive_prefix,
            "payload_codec": codec,
            "entries": [
                {"index": index, "source": source["path"], "content_type": content_type(source["path"])}
                for index, source in enumerate(sources)
            ],
            "validation": "pass",
        })
    return staged_archive_paths, archive_records


# Generate and check the complete triplet before replacing archives and writing the report.
def process(
    input_directory: Path,
    archive_prefix: str,
    zstd_executable: Path,
    gdeflate_executable: Path,
    alignment: int,
    overwrite: bool,
    *, report: dict[str, Any] | None = None,
) -> Path:
    validate_options(archive_prefix, alignment)
    input_directory = content_report.validate_input_directory(input_directory)
    zstd_version = verify_tool(zstd_executable)
    gdeflate_version = verify_tool(gdeflate_executable)
    zstd_executable = zstd_executable.resolve()
    gdeflate_executable = gdeflate_executable.resolve()
    manifest_path = content_report.manifest_path(input_directory)

    manifest_data = report if report is not None else content_report.create_manifest(input_directory)
    sources = manifest_data["sources"]
    filenames = {codec: f"{archive_prefix}.{codec}" for codec in ("uncompressed", "gdeflate", "zstd")}
    archive_root = reject_symlink_components(input_directory, "generated/hlk", "archive directory")
    relative_paths = {codec: f"generated/hlk/{name}" for codec, name in filenames.items()}
    output_paths = {codec: reject_symlink_components(input_directory, relative, "archive output")
                    for codec, relative in relative_paths.items()}
    if any(path.exists() for path in output_paths.values()) and not overwrite:
        raise FileExistsError("HLK archive triplet already exists; use --overwrite")
    archive_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".hlk.stage.", dir=archive_root) as temporary:
        staging = Path(temporary)
        payloads = prepare_payloads(input_directory, sources, staging, zstd_executable, gdeflate_executable)
        staged_paths, records = build_archives(
            staging, sources, payloads, filenames, relative_paths, archive_prefix,
            alignment, create_hlk_archive, validate_hlk_archive,
        )
        # Verify the whole triplet before replacing any archive; installation has no rollback.
        for codec, destination in output_paths.items():
            reject_symlink_components(input_directory, relative_paths[codec], "archive output")
            os.replace(staged_paths[codec], destination)
    manifest_data["archives"].extend(records)
    manifest_data["archives"].sort(key=lambda record: record["path"])
    manifest_data["tools"].update({
        "makehlkcontent-compatible": {"version": "1"},
        "hlk-zstd": {"version": zstd_version},
        "hlk-gdeflate": {"version": gdeflate_version},
    })
    content_report.write_json_atomic(manifest_path, manifest_data)
    return manifest_path
