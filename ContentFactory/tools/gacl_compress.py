#!/usr/bin/env python3
"""Generate verified BC1/BC3/BC4/BC5/BC7 GACL derivatives for one Content Factory set."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import tempfile
from dataclasses import dataclass
from pathlib import Path
from functools import partial
from typing import Any

from factory_common import (
    directory_is_empty, load_module, set_lock as common_set_lock,
    run_command, tool_version as common_tool_version, validate_executable, write_bytes_atomic,
)
from factory_contracts import TRANSFORM_CONTRACT, ZSTD_COMPRESSION_LEVEL


DDS_MAGIC = 0x20534444
FOURCC_DX10 = 0x30315844
# Older DDS headers use four-character codes to describe their BC format.
FOURCC_FORMATS = {
    0x31545844: ("BC1", "DXT1", 8),
    0x35545844: ("BC3", "DXT5", 16),
    0x31495441: ("BC4", "ATI1", 8),
    0x55344342: ("BC4", "BC4U", 8),
    0x53344342: ("BC4", "BC4S", 8),
    0x32495441: ("BC5", "ATI2", 16),
    0x55354342: ("BC5", "BC5U", 16),
    0x53354342: ("BC5", "BC5S", 16),
}
# DX10 DDS headers use numeric DXGI formats instead.
DXGI_FORMATS = {
    70: ("BC1", "DXGI_FORMAT_BC1_TYPELESS", 8),
    71: ("BC1", "DXGI_FORMAT_BC1_UNORM", 8),
    72: ("BC1", "DXGI_FORMAT_BC1_UNORM_SRGB", 8),
    76: ("BC3", "DXGI_FORMAT_BC3_TYPELESS", 16),
    77: ("BC3", "DXGI_FORMAT_BC3_UNORM", 16),
    78: ("BC3", "DXGI_FORMAT_BC3_UNORM_SRGB", 16),
    79: ("BC4", "DXGI_FORMAT_BC4_TYPELESS", 8),
    80: ("BC4", "DXGI_FORMAT_BC4_UNORM", 8),
    81: ("BC4", "DXGI_FORMAT_BC4_SNORM", 8),
    82: ("BC5", "DXGI_FORMAT_BC5_TYPELESS", 16),
    83: ("BC5", "DXGI_FORMAT_BC5_UNORM", 16),
    84: ("BC5", "DXGI_FORMAT_BC5_SNORM", 16),
    97: ("BC7", "DXGI_FORMAT_BC7_TYPELESS", 16),
    98: ("BC7", "DXGI_FORMAT_BC7_UNORM", 16),
    99: ("BC7", "DXGI_FORMAT_BC7_UNORM_SRGB", 16),
}

# Use the same transform names and IDs that the manifest validator checks.
TRANSFORM_IDS = {name: item[0] for name, item in TRANSFORM_CONTRACT.items()}
TRANSFORM_NAMES = {name: item[1] for name, item in TRANSFORM_CONTRACT.items()}


@dataclass(frozen=True)
# Describe the first image's top mip; the payload is read separately.
class DDSFirstMip:
    format: str
    format_name: str
    width: int
    height: int
    mip_count: int
    array_size: int
    data_offset: int
    data_size: int


def load_driver():
    return load_module("process_set", "process-set.py")

def read_u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("truncated DDS header")
    return struct.unpack_from("<I", data, offset)[0]


def parse_dds_first_mip(data: bytes) -> DDSFirstMip:
    # Check the basic DDS header before reading dimensions and format fields.
    if len(data) < 128 or read_u32(data, 0) != DDS_MAGIC or read_u32(data, 4) != 124:
        raise ValueError("not a valid DDS file")
    width = read_u32(data, 16)
    height = read_u32(data, 12)
    mip_count = max(1, read_u32(data, 28))
    if width == 0 or height == 0:
        raise ValueError("DDS dimensions must be non-zero")
    fourcc = read_u32(data, 84)
    offset = 128
    array_size = 1
    # Read the extra DX10 header only when the file says it is present.
    if fourcc == FOURCC_DX10:
        if len(data) < 148:
            raise ValueError("truncated DDS DX10 header")
        dxgi = read_u32(data, 128)
        try:
            format_name, exact_name, bytes_per_block = DXGI_FORMATS[dxgi]
        except KeyError as error:
            raise ValueError(f"unsupported DDS DXGI format: {dxgi}") from error
        resource_dimension = read_u32(data, 132)
        array_size = read_u32(data, 140)
        if resource_dimension != 3 or array_size == 0:
            raise ValueError("GACL stage requires a non-empty DDS_TEXTURE2D resource")
        offset = 148
    else:
        try:
            format_name, exact_name, bytes_per_block = FOURCC_FORMATS[fourcc]
        except KeyError as error:
            raise ValueError(f"unsupported DDS FourCC: 0x{fourcc:08X}") from error
    # BC textures use 4-by-4 pixel blocks, including partial blocks at the edges.
    blocks_x = (width + 3) // 4
    blocks_y = (height + 3) // 4
    size = blocks_x * blocks_y * bytes_per_block
    if offset + size > len(data):
        raise ValueError("DDS first mip exceeds file bounds")
    return DDSFirstMip(format_name, exact_name, width, height, mip_count, array_size, offset, size)


run_tool = partial(run_command, operation="GACLContentTool", text=True)

tool_version = partial(common_tool_version, label="GACLContentTool")

def output_relative_path(source: str, set_name: str) -> str:
    source_path = Path(source)
    filename = f"{source_path.name}.gacl"
    parent = source_path.parent.as_posix()
    return f"gacl/{set_name}/{parent + '/' if parent != '.' else ''}{filename}"


def build_variants(
    root: Path,
    document: dict[str, Any],
    output_root: Path,
    executable: Path,
    zstd_level: int,
    target_block_size: int,
) -> list[dict[str, Any]]:
    set_name = document["set_name"]
    records: list[dict[str, Any]] = []
    for source in document["sources"]:
        # Only DDS inputs participate in this texture-conditioning stage.
        if Path(source["path"]).suffix.lower() != ".dds":
            continue
        source_path = root / "originals" / set_name / Path(source["path"])
        source_data = source_path.read_bytes()
        info = parse_dds_first_mip(source_data)
        # Pass the first mip's block bytes to the codec, not the DDS container.
        payload = source_data[info.data_offset:info.data_offset + info.data_size]
        relative = output_relative_path(source["path"], set_name)
        output = output_root / Path(relative).relative_to(Path("gacl") / set_name)
        output.parent.mkdir(parents=True, exist_ok=True)
        payload_path = output.with_suffix(output.suffix + ".payload")
        payload_path.write_bytes(payload)
        # Generate the conditioned stream and compare its decoded payload to the original.
        try:
            run_tool([
                str(executable), "--input", str(payload_path), "--output", str(output),
                "--format", info.format, "--zstd-level", str(zstd_level),
                "--target-block-size", str(target_block_size),
            ])
            run_tool([
                str(executable), "--verify", "--input", str(output),
                "--original", str(payload_path), "--format", info.format,
            ])
        # The extracted payload is temporary and must not remain beside the derivative.
        finally:
            payload_path.unlink(missing_ok=True)
        data = output.read_bytes()
        # Record the transform and the exact mip selection needed to interpret this output.
        records.append({
            "path": relative,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "source": source["path"],
            "format": "gacl",
            "parameters": {
                "texture_format": info.format,
                "zstd_level": zstd_level,
                "target_block_size": target_block_size,
                "transform_id": TRANSFORM_IDS[info.format],
                "transform_name": TRANSFORM_NAMES[info.format],
                "transform_version": 1,
            },
            "metadata": {
                "dds_format": info.format_name,
                "width": info.width,
                "height": info.height,
                "mip_count": info.mip_count,
                "array_size": info.array_size,
                "array_item": 0,
                "mip": 0,
                "uncompressed_size": info.data_size,
                "compressed_size": len(data),
                "container": "raw_zstd_stream",
            },
            "validation": "pass",
        })
    if not records:
        raise ValueError("content set contains no supported BC1/BC3/BC4/BC5/BC7 DDS sources")
    return sorted(records, key=lambda item: item["path"])


set_lock = partial(common_set_lock, stage="gacl", label="GACL")


def validate_options(zstd_level: int, target_block_size: int) -> None:
    if zstd_level < 1 or zstd_level > 22:
        raise ValueError("Zstd level must be from 1 through 22")
    if target_block_size < 1 or target_block_size > 1024 * 1024:
        raise ValueError("target block size must be from 1 through 1048576")


def process(
    root: Path,
    set_name: str,
    executable: Path,
    zstd_level: int,
    target_block_size: int,
    overwrite: bool,
) -> Path:
    # Validate settings and identify the encoder before changing any outputs.
    validate_options(zstd_level, target_block_size)
    driver = load_driver()
    root = root.resolve()
    set_name = driver.validate_set_name(set_name)
    executable = validate_executable(executable)
    version = tool_version(executable)
    manifest = driver.manifest_path(root, set_name)

    with set_lock(root, set_name):
        document = driver.load_manifest(manifest)
        driver.verify_files(root, set_name, document)
        existing = [item for item in document["derivatives"] if item["format"] == "gacl"]
        final_root = root / "gacl" / set_name
        if final_root.is_symlink():
            raise ValueError(f"GACL output root cannot be a symbolic link: {final_root}")
        if (existing or not directory_is_empty(final_root)) and not overwrite:
            raise FileExistsError("GACL outputs already exist; use --overwrite")

        # Build replacements separately so the previous texture outputs remain available.
        parent = root / "gacl"
        parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=f".{set_name}.gacl.stage.", dir=parent))
        backup = Path(tempfile.mkdtemp(prefix=f".{set_name}.gacl.backup.", dir=parent))
        backup.rmdir()
        old_manifest = manifest.read_bytes()
        old_tree_moved = False
        new_tree_installed = False
        try:
            records = build_variants(root, document, stage, executable, zstd_level, target_block_size)
            updated = dict(document)
            # Replace GACL records only; leave other codec records unchanged.
            updated["derivatives"] = sorted(
                [item for item in document["derivatives"] if item["format"] != "gacl"] + records,
                key=lambda item: item["path"],
            )
            updated["tools"] = dict(document["tools"])
            updated["tools"]["GACLContentTool"] = {"version": version}
            driver.validate_manifest(updated)
            # Save the previous tree before installing the new one.
            if final_root.exists():
                os.replace(final_root, backup)
                old_tree_moved = True
            os.replace(stage, final_root)
            new_tree_installed = True
            try:
                driver.write_json_atomic(manifest, updated)
                driver.verify_files(root, set_name, updated)
            # Try to restore the previous outputs and manifest together on failure.
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
                        f"GACL commit failed ({original_error}); rollback also failed: {'; '.join(rollback_errors)}"
                    ) from original_error
                raise
            shutil.rmtree(backup, ignore_errors=True)
            return manifest
        # Clean up temporary directories after success or failure.
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
    # These options control conditioning of the selected DDS payloads.
    parser.add_argument("--gacl-exe", type=Path, required=True)
    parser.add_argument("--zstd-level", type=int, default=ZSTD_COMPRESSION_LEVEL)
    parser.add_argument("--target-block-size", type=int, default=64 * 1024)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    try:
        output = process(
            args.root, args.set_name, args.gacl_exe, args.zstd_level,
            args.target_block_size, args.overwrite,
        )
        print(f"Updated {output}")
        return 0
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
