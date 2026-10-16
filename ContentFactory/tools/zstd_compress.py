#!/usr/bin/env python3
"""Generate deterministic multi-frame Zstd variants for one Content Factory set."""

from __future__ import annotations

import hashlib
import os
import shutil
import tempfile
import content_report
from pathlib import Path
from typing import Any

from factory_common import (
    directory_is_empty,
    reject_symlink_components,
    run_command,
    tool_version as common_tool_version,
    validate_executable,
)


from factory_contracts import ZSTD_COMPRESSION_LEVEL

# Use the shared content policy while preserving the stage-level constant.
COMPRESSION_LEVEL = ZSTD_COMPRESSION_LEVEL

# Normal sets get one variant; shader coverage sets can request the full matrix.
DEFAULT_BLOCK_SIZES_KB = (16,)
DEFAULT_CHUNK_SIZES_KB = (256,)
SHADER_BLOCK_SIZES_KB = (4, 8, 16)
SHADER_CHUNK_SIZES_KB = (64, 128, 256)


# Run the Zstd command with optional binary input and checked exit status.
def run_zstd(arguments: list[str], *, data: bytes | None = None, timeout: int = 120):
    return run_command(arguments, operation="zstd", input_data=data, timeout=timeout)

# Read the encoder banner and extract its version for the set manifest.
def zstd_version(zstd_exe: Path) -> str:
    return common_tool_version(zstd_exe, "zstd", r"\bv?(\d+\.\d+(?:\.\d+)?)\b", timeout=30)

# Reject empty, repeated, or nonpositive KiB sizes and return them in sorted order.
def validate_sizes(values: list[int] | tuple[int, ...], name: str) -> tuple[int, ...]:
    # Reject empty, invalid, or repeated sizes before choosing their order.
    if not values or any(type(value) is not int or value <= 0 for value in values):
        raise ValueError(f"{name} must contain positive integer KiB values")
    if len(set(values)) != len(values):
        raise ValueError(f"{name} cannot contain duplicate values")
    return tuple(sorted(values))


# Reject target block sizes larger than any requested input chunk size.
def validate_matrix(block_sizes_kb: tuple[int, ...], chunk_sizes_kb: tuple[int, ...]) -> None:
    for block_kb in block_sizes_kb:
        for chunk_kb in chunk_sizes_kb:
            if block_kb > chunk_kb:
                raise ValueError(f"block size {block_kb} KiB cannot exceed chunk size {chunk_kb} KiB")


# Choose default, shader-matrix, or custom sizes and validate their combinations.
def resolve_sizes(
    shader_matrix: bool,
    block_sizes_kb: list[int] | tuple[int, ...] | None,
    chunk_sizes_kb: list[int] | tuple[int, ...] | None,
) -> tuple[tuple[int, ...], tuple[int, ...]]:
    # The preset and custom sizes are alternatives, not overlapping options.
    if shader_matrix and (block_sizes_kb is not None or chunk_sizes_kb is not None):
        raise ValueError("--zstd-shader-matrix cannot be combined with explicit Zstd sizes")
    # Choose defaults only for omitted lists, then validate all combinations.
    blocks = SHADER_BLOCK_SIZES_KB if shader_matrix else (
        DEFAULT_BLOCK_SIZES_KB if block_sizes_kb is None else block_sizes_kb)
    chunks = SHADER_CHUNK_SIZES_KB if shader_matrix else (
        DEFAULT_CHUNK_SIZES_KB if chunk_sizes_kb is None else chunk_sizes_kb)
    blocks = validate_sizes(blocks, "block sizes")
    chunks = validate_sizes(chunks, "chunk sizes")
    validate_matrix(blocks, chunks)
    return blocks, chunks


# Preserve source subfolders and encode block and chunk settings in the filename.
def output_relative_path(source: str, block_kb: int, chunk_kb: int) -> str:
    source_path = Path(source)
    # Put the settings in the output name so variants do not overwrite one another.
    filename = f"{source_path.name}-blk{block_kb}K-chunk{chunk_kb}K.zst"
    return (Path("generated") / "zstd" / source_path.parent / filename).as_posix()


# Encode one source chunk using the shared Zstd level and target block size.
def compress_chunk(zstd_exe: Path, chunk: bytes, block_bytes: int) -> bytes:
    # Explicit level 19 avoids depending on the CLI default or ZSTD_CLEVEL environment.
    result = run_zstd([
        str(zstd_exe),
        f"-{COMPRESSION_LEVEL}",
        f"--target-compressed-block-size={block_bytes}",
        f"--stream-size={len(chunk)}",
        "-q",
        "-c",
    ], data=chunk)
    if not result.stdout:
        raise RuntimeError("zstd compression produced an empty frame")
    return result.stdout


# Create each requested frame configuration and collect verified derivative metadata.
def build_variants(
    input_directory: Path,
    manifest_data: dict[str, Any],
    output_root: Path,
    zstd_exe: Path,
    block_sizes_kb: tuple[int, ...],
    chunk_sizes_kb: tuple[int, ...],
) -> list[dict[str, Any]]:
    derivative_records: list[dict[str, Any]] = []
    for original_file in manifest_data["sources"]:
        source_path = reject_symlink_components(input_directory, original_file["path"], "original file")
        source_data = source_path.read_bytes()
        if not source_data:
            raise ValueError(f"Zstd source cannot be empty: {original_file['path']}")
        # Split the original into the requested frame-sized chunks.
        for chunk_kb in chunk_sizes_kb:
            chunk_bytes = chunk_kb * 1024
            chunks = [source_data[offset:offset + chunk_bytes] for offset in range(0, len(source_data), chunk_bytes)]
            for block_kb in block_sizes_kb:
                relative = output_relative_path(original_file["path"], block_kb, chunk_kb)
                output = output_root / Path(relative).relative_to(Path("generated") / "zstd")
                # Join the frames and check that decoding returns the complete original.
                encoded = b"".join(compress_chunk(zstd_exe, chunk, block_kb * 1024) for chunk in chunks)
                if run_zstd([str(zstd_exe), "-q", "-d", "-c"], data=encoded).stdout != source_data:
                    raise RuntimeError(f"Zstd round trip mismatch: {relative}")
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(encoded)
                # Record settings and a hash only after the round trip succeeds.
                derivative_records.append({
                    "path": relative,
                    "size": len(encoded),
                    "sha256": hashlib.sha256(encoded).hexdigest(),
                    "source": original_file["path"],
                    "format": "zstd",
                    "parameters": {"block_size_kb": block_kb, "chunk_size_kb": chunk_kb,
                                   "compression_level": COMPRESSION_LEVEL},
                    "metadata": {"frame_count": len(chunks), "uncompressed_size": len(source_data)},
                    "validation": "pass",
                })
    return sorted(derivative_records, key=lambda derivative: derivative["path"])


# Generate and verify Zstd files before replacing outputs and writing the report.
def process(
    input_directory: Path,
    zstd_exe: Path,
    block_sizes_kb: tuple[int, ...],
    chunk_sizes_kb: tuple[int, ...],
    overwrite: bool,
    *, report: dict[str, Any] | None = None,
) -> Path:
    input_directory = content_report.validate_input_directory(input_directory)
    zstd_exe = validate_executable(zstd_exe)
    validate_matrix(block_sizes_kb, chunk_sizes_kb)
    # Record the actual encoder version used for this run.
    current_version = zstd_version(zstd_exe)

    manifest = content_report.manifest_path(input_directory)
    manifest_data = report if report is not None else content_report.create_manifest(input_directory)
    output_directory = reject_symlink_components(input_directory, "generated/zstd", "zstd output directory")
    if not directory_is_empty(output_directory) and not overwrite:
        raise FileExistsError("Zstd outputs already exist; use --overwrite")
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".zstd.stage.", dir=output_directory.parent) as temporary:
        staging = Path(temporary)
        records = build_variants(input_directory, manifest_data, staging, zstd_exe, block_sizes_kb, chunk_sizes_kb)
        # Complete generation before replacing old outputs; failed installation is not rolled back.
        reject_symlink_components(input_directory, "generated/zstd", "zstd output directory")
        if output_directory.exists():
            shutil.rmtree(output_directory)
        os.replace(staging, output_directory)
    manifest_data["derivatives"].extend(records)
    manifest_data["derivatives"].sort(key=lambda record: record["path"])
    manifest_data["tools"]["zstd"] = {"version": current_version}
    content_report.write_json_atomic(manifest, manifest_data)
    return manifest
