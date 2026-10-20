#!/usr/bin/env python3
"""Generate deterministic multi-frame Zstd variants for one file. Relies on a bundled zstd.exe.

Choose block/chunk configurations
    ↓
Read an original file
    ↓
Split it into chunks
    ↓
Compress each chunk into a Zstd frame
    ↓
Concatenate the frames
    ↓
Decode and compare with the original
    ↓
Write to a staging directory
    ↓
Install the completed output directory
    ↓
Update the report
"""

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
    checked_content_path,
    run_command,
    validate_executable,
)


from factory_contracts import ZSTD_COMPRESSION_LEVEL, validate_zstd_level

# Use the shared content policy while preserving the stage-level constant.
COMPRESSION_LEVEL = ZSTD_COMPRESSION_LEVEL

# Normal sets get one variant
DEFAULT_BLOCK_SIZES_KB = (16,)
DEFAULT_CHUNK_SIZES_KB = (256,)
SHADER_BLOCK_SIZES_KB = (4, 8, 16)
SHADER_CHUNK_SIZES_KB = (64, 128, 256)


# Utilize run_command to run zstd.exe on a loaded set of data using certain arguments
def run_zstd(arguments: list[str], *, data: bytes | None = None, timeout: int = 120):
    return run_command(arguments, operation="zstd", input_data=data, timeout=timeout)

# Checks a requested size list, then returns it as a sorted tuple.
def validate_sizes(values: list[int] | tuple[int, ...], name: str) -> tuple[int, ...]:
    # Rejects empty lists, non-int values, zero/negative sizes, duplicate sizes
    if not values or any(type(value) is not int or value <= 0 for value in values):
        raise ValueError(f"{name} must contain positive integer KiB values")
    if len(set(values)) != len(values):
        raise ValueError(f"{name} cannot contain duplicate values")
    return tuple(sorted(values))

# Checks every block/chunk pairing
def validate_matrix(block_sizes_kb: tuple[int, ...], chunk_sizes_kb: tuple[int, ...]) -> None:
    for block_kb in block_sizes_kb:
        for chunk_kb in chunk_sizes_kb:
            if block_kb > chunk_kb:
                raise ValueError(f"block size {block_kb} KiB cannot exceed chunk size {chunk_kb} KiB")

# This turns the request into concrete settings. The launcher calls this during preflight, before encoding begins.
def resolve_sizes(
    shader_matrix: bool,
    block_sizes_kb: list[int] | tuple[int, ...] | None,
    chunk_sizes_kb: list[int] | tuple[int, ...] | None,
) -> tuple[tuple[int, ...], tuple[int, ...]]:
    # Reject conflicting ways to choose sizes
    if shader_matrix and (block_sizes_kb is not None or chunk_sizes_kb is not None):
        raise ValueError("--zstd-shader-matrix cannot be combined with explicit Zstd sizes")
    # Select the lists
    blocks = SHADER_BLOCK_SIZES_KB if shader_matrix else (
        DEFAULT_BLOCK_SIZES_KB if block_sizes_kb is None else block_sizes_kb)
    chunks = SHADER_CHUNK_SIZES_KB if shader_matrix else (
        DEFAULT_CHUNK_SIZES_KB if chunk_sizes_kb is None else chunk_sizes_kb)
    
    # Validate and sort
    blocks = validate_sizes(blocks, "block sizes")
    chunks = validate_sizes(chunks, "chunk sizes")
    validate_matrix(blocks, chunks)
    return blocks, chunks

"""
Builds the generated filename while preserving the original’s subdirectories.
Example input: 
    Original: textures/wood.dds
    Block:    8 KiB
    Chunk:    128 KiB

Example output:
    generated/zstd/textures/wood.dds-blk8K-chunk128K.zst
"""
def output_relative_path(source: str, block_kb: int, chunk_kb: int) -> str:
    source_path = Path(source)
    # Put the settings in the output name so variants do not overwrite one another.
    filename = f"{source_path.name}-blk{block_kb}K-chunk{chunk_kb}K.zst"
    return (Path("generated") / "zstd" / source_path.parent / filename).as_posix()

# Compress one chunk
def compress_chunk(zstd_exe: Path, chunk: bytes, block_bytes: int, compression_level: int = ZSTD_COMPRESSION_LEVEL) -> bytes:
    # Always pass the selected level explicitly, including ultra levels above 19.
    result = run_zstd([
        str(zstd_exe),
        *(["--ultra"] if compression_level > 19 else []),
        f"-{compression_level}",
        f"--target-compressed-block-size={block_bytes}",
        f"--stream-size={len(chunk)}",
        "-q",
        "-c",
    ], data=chunk)
    if not result.stdout:
        raise RuntimeError("zstd compression produced an empty frame")
    return result.stdout

# This does the main file-processing work.
# NOTE: This is whole-buffer processing. During a variant, the code holds the original bytes, 
# chunk copies, encoded bytes, and decoded verification output. It is not a bounded-memory
# streaming implementation.
def build_variants(
    input_directory: Path,
    manifest_data: dict[str, Any],
    output_root: Path,
    zstd_exe: Path,
    block_sizes_kb: tuple[int, ...],
    chunk_sizes_kb: tuple[int, ...],
    compression_level: int = ZSTD_COMPRESSION_LEVEL,
) -> list[dict[str, Any]]:
    derivative_records: list[dict[str, Any]] = []

    # Read each original. Each file is read completely into memory. Empty originals are rejected.
    for original_file in manifest_data["sources"]:
        source_path = checked_content_path(input_directory, original_file["path"], "original file")
        source_data = source_path.read_bytes()
        if not source_data:
            raise ValueError(f"Zstd source cannot be empty: {original_file['path']}")
        # For each chunk size to process, break the file into chunks
        for chunk_kb in chunk_sizes_kb:
            chunk_bytes = chunk_kb * 1024
            chunks = [source_data[offset:offset + chunk_bytes] for offset in range(0, len(source_data), chunk_bytes)]
            # Process each block
            for block_kb in block_sizes_kb:
                relative = output_relative_path(original_file["path"], block_kb, chunk_kb)
                output = output_root / Path(relative).relative_to(Path("generated") / "zstd")
                # Join the frames and check that decoding returns the complete original.
                encoded = b"".join(compress_chunk(zstd_exe, chunk, block_kb * 1024, compression_level) for chunk in chunks)
                # Decode the complete result and compare. This checks the entire concatenated output, including frame order and the final partial chunk.
                if run_zstd([str(zstd_exe), "-q", "-d", "-c"], data=encoded).stdout != source_data:
                    raise RuntimeError(f"Zstd round trip mismatch: {relative}")
                # Write the staging file
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
                                   "compression_level": compression_level},
                    "metadata": {"frame_count": len(chunks), "uncompressed_size": len(source_data)},
                    "validation": "pass",
                })
    return sorted(derivative_records, key=lambda derivative: derivative["path"])


# This coordinates the stage.
def process(
    input_directory: Path,
    zstd_exe: Path,
    block_sizes_kb: tuple[int, ...],
    chunk_sizes_kb: tuple[int, ...],
    overwrite: bool,
    *, compression_level: int = ZSTD_COMPRESSION_LEVEL, report: dict[str, Any] | None = None,
) -> Path:

    # Resolve input directory, zstd.exe, block/chunk sizes, compression level, output directory
    input_directory = content_report.validate_input_directory(input_directory)
    zstd_exe = validate_executable(zstd_exe)
    validate_matrix(block_sizes_kb, chunk_sizes_kb)
    validate_zstd_level(compression_level)
    manifest = content_report.manifest_path(input_directory)
    manifest_data = report if report is not None else content_report.create_manifest(input_directory)
    output_directory = checked_content_path(input_directory, "generated/zstd", "zstd output directory")
    if not directory_is_empty(output_directory) and not overwrite:
        raise FileExistsError("Zstd outputs already exist; use --overwrite")
    output_directory.parent.mkdir(parents=True, exist_ok=True)

    # Generate into staging
    with tempfile.TemporaryDirectory(prefix=".zstd.stage.", dir=output_directory.parent) as temporary:
        staging = Path(temporary)
        records = build_variants(input_directory, manifest_data, staging, zstd_exe, block_sizes_kb, chunk_sizes_kb, compression_level)
        # Install the complete stage output
        checked_content_path(input_directory, "generated/zstd", "zstd output directory")
        if output_directory.exists():
            shutil.rmtree(output_directory)
        os.replace(staging, output_directory)
    manifest_data["derivatives"].extend(records)
    manifest_data["derivatives"].sort(key=lambda record: record["path"])
    content_report.write_json_atomic(manifest, manifest_data)
    return manifest
