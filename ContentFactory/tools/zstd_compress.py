#!/usr/bin/env python3
"""Generate deterministic multi-frame Zstd variants for one Content Factory set."""

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
    directory_is_empty, load_module, set_lock as common_set_lock,
    run_command, tool_version as common_tool_version, validate_executable, write_bytes_atomic,
)


from factory_contracts import ZSTD_COMPRESSION_LEVEL

# Use the shared content policy while preserving the stage-level constant.
COMPRESSION_LEVEL = ZSTD_COMPRESSION_LEVEL

# Normal sets get one variant; shader coverage sets can request the full matrix.
DEFAULT_BLOCK_SIZES_KB = (16,)
DEFAULT_CHUNK_SIZES_KB = (256,)
SHADER_BLOCK_SIZES_KB = (4, 8, 16)
SHADER_CHUNK_SIZES_KB = (64, 128, 256)


def load_driver():
    return load_module("process_set", "process-set.py")

def run_zstd(arguments: list[str], *, data: bytes | None = None, timeout: int = 120):
    return run_command(arguments, operation="zstd", input_data=data, timeout=timeout)

def zstd_version(zstd_exe: Path) -> str:
    return common_tool_version(zstd_exe, "zstd", r"\bv?(\d+\.\d+(?:\.\d+)?)\b", timeout=30)

def validate_sizes(values: list[int] | tuple[int, ...], name: str) -> tuple[int, ...]:
    # Reject empty, invalid, or repeated sizes before choosing their order.
    if not values or any(type(value) is not int or value <= 0 for value in values):
        raise ValueError(f"{name} must contain positive integer KiB values")
    if len(set(values)) != len(values):
        raise ValueError(f"{name} cannot contain duplicate values")
    return tuple(sorted(values))


def validate_matrix(block_sizes_kb: tuple[int, ...], chunk_sizes_kb: tuple[int, ...]) -> None:
    for block_kb in block_sizes_kb:
        for chunk_kb in chunk_sizes_kb:
            if block_kb > chunk_kb:
                raise ValueError(f"block size {block_kb} KiB cannot exceed chunk size {chunk_kb} KiB")


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


def output_relative_path(source: str, set_name: str, block_kb: int, chunk_kb: int) -> str:
    source_path = Path(source)
    # Put the settings in the output name so variants do not overwrite one another.
    filename = f"{source_path.name}-blk{block_kb}K-chunk{chunk_kb}K.zst"
    parent = source_path.parent.as_posix()
    return f"zstd/{set_name}/{parent + '/' if parent != '.' else ''}{filename}"


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


def decompress_stream(zstd_exe: Path, compressed: bytes) -> bytes:
    result = run_zstd([str(zstd_exe), "-q", "-d", "-c"], data=compressed)
    return result.stdout


def build_variants(
    root: Path,
    document: dict[str, Any],
    output_root: Path,
    zstd_exe: Path,
    block_sizes_kb: tuple[int, ...],
    chunk_sizes_kb: tuple[int, ...],
) -> list[dict[str, Any]]:
    set_name = document["set_name"]
    records: list[dict[str, Any]] = []
    for source in document["sources"]:
        source_path = root / "originals" / set_name / Path(source["path"])
        source_data = source_path.read_bytes()
        if not source_data:
            raise ValueError(f"Zstd source cannot be empty: {source['path']}")
        # Split the original into the requested frame-sized chunks.
        for chunk_kb in chunk_sizes_kb:
            chunk_bytes = chunk_kb * 1024
            chunks = [source_data[offset:offset + chunk_bytes] for offset in range(0, len(source_data), chunk_bytes)]
            for block_kb in block_sizes_kb:
                relative = output_relative_path(source["path"], set_name, block_kb, chunk_kb)
                output = output_root / Path(relative).relative_to(Path("zstd") / set_name)
                # Join the frames and check that decoding returns the complete original.
                encoded = b"".join(compress_chunk(zstd_exe, chunk, block_kb * 1024) for chunk in chunks)
                if decompress_stream(zstd_exe, encoded) != source_data:
                    raise RuntimeError(f"Zstd round trip mismatch: {relative}")
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(encoded)
                # Record settings and a hash only after the round trip succeeds.
                records.append({
                    "path": relative,
                    "size": len(encoded),
                    "sha256": hashlib.sha256(encoded).hexdigest(),
                    "source": source["path"],
                    "format": "zstd",
                    "parameters": {"block_size_kb": block_kb, "chunk_size_kb": chunk_kb,
                                   "compression_level": COMPRESSION_LEVEL},
                    "metadata": {"frame_count": len(chunks), "uncompressed_size": len(source_data)},
                    "validation": "pass",
                })
    return sorted(records, key=lambda item: item["path"])


set_lock = partial(common_set_lock, stage="zstd", label="Zstd")


def process(
    root: Path,
    set_name: str,
    zstd_exe: Path,
    block_sizes_kb: tuple[int, ...],
    chunk_sizes_kb: tuple[int, ...],
    overwrite: bool,
    allow_version_change: bool,
) -> Path:
    driver = load_driver()
    root = root.resolve()
    set_name = driver.validate_set_name(set_name)
    zstd_exe = validate_executable(zstd_exe)
    validate_matrix(block_sizes_kb, chunk_sizes_kb)
    # Record the actual encoder version used for this run.
    current_version = zstd_version(zstd_exe)

    manifest = driver.manifest_path(root, set_name)
    with set_lock(root, set_name):
        document = driver.load_manifest(manifest)
        driver.verify_files(root, set_name, document)
        # Require explicit approval before replacing outputs with a different Zstd version.
        existing_zstd = [item for item in document["derivatives"] if item["format"] == "zstd"]
        recorded_version = document["tools"].get("zstd", {}).get("version")
        if existing_zstd and recorded_version != current_version and not allow_version_change:
            raise ValueError(
                f"Zstd version changed from {recorded_version!r} to {current_version!r}; use --allow-version-change"
            )

        final_root = root / "zstd" / set_name
        if final_root.is_symlink():
            raise ValueError(f"Zstd output root cannot be a symbolic link: {final_root}")
        if (existing_zstd or not directory_is_empty(final_root)) and not overwrite:
            raise FileExistsError("Zstd outputs already exist; use --overwrite")

        # Build in a temporary folder and retain the old tree for rollback.
        parent = root / "zstd"
        parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=f".{set_name}.zstd.stage.", dir=parent))
        backup = Path(tempfile.mkdtemp(prefix=f".{set_name}.zstd.backup.", dir=parent))
        backup.rmdir()
        old_manifest = manifest.read_bytes()
        old_tree_moved = False
        new_tree_installed = False
        try:
            records = build_variants(root, document, stage, zstd_exe, block_sizes_kb, chunk_sizes_kb)
            updated = dict(document)
            # Replace only this codec's records; preserve outputs from other stages.
            updated["derivatives"] = sorted(
                [item for item in document["derivatives"] if item["format"] != "zstd"] + records,
                key=lambda item: item["path"],
            )
            updated["tools"] = dict(document["tools"])
            updated["tools"]["zstd"] = {"version": current_version}
            driver.validate_manifest(updated)

            # Move the previous tree aside before installing the verified new tree.
            if final_root.exists():
                os.replace(final_root, backup)
                old_tree_moved = True
            os.replace(stage, final_root)
            new_tree_installed = True
            try:
                driver.write_json_atomic(manifest, updated)
                driver.verify_files(root, set_name, updated)
            # If the manifest update fails, try to restore both old files and old records.
            except Exception as original_error:
                rollback_errors: list[str] = []
                try:
                    if new_tree_installed and final_root.exists():
                        shutil.rmtree(final_root)
                    if old_tree_moved and backup.exists():
                        os.replace(backup, final_root)
                    write_bytes_atomic(manifest, old_manifest)
                except Exception as rollback_error:
                    rollback_errors.append(str(rollback_error))
                if rollback_errors:
                    raise RuntimeError(
                        f"Zstd commit failed ({original_error}); rollback also failed: {'; '.join(rollback_errors)}"
                    ) from original_error
                raise
            shutil.rmtree(backup, ignore_errors=True)
            return manifest
        # Clean up scratch folders and recover the old tree if installation stopped early.
        finally:
            shutil.rmtree(stage, ignore_errors=True)
            if backup.exists() and not final_root.exists():
                os.replace(backup, final_root)
            elif backup.exists():
                shutil.rmtree(backup, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("set_name")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    # Expose the same preset and custom-size choices used by the orchestrator.
    parser.add_argument("--zstd-exe", type=Path, required=True)
    parser.add_argument(
        "--zstd-shader-matrix", action="store_true",
        help="explicitly generate all 4/8/16 KiB block by 64/128/256 KiB chunk variants",
    )
    parser.add_argument("--block-sizes-kb", nargs="+", type=int)
    parser.add_argument("--chunk-sizes-kb", nargs="+", type=int)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--allow-version-change", action="store_true")
    args = parser.parse_args()
    try:
        block_sizes, chunk_sizes = resolve_sizes(
            args.zstd_shader_matrix, args.block_sizes_kb, args.chunk_sizes_kb)
        output = process(
            args.root,
            args.set_name,
            args.zstd_exe,
            block_sizes,
            chunk_sizes,
            args.overwrite,
            args.allow_version_change,
        )
        print(f"Updated {output}")
        return 0
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
