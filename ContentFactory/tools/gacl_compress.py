#!/usr/bin/env python3
"""Generate verified BC1/BC3/BC4/BC5/BC7 GACL derivatives for one Content Factory set."""

from __future__ import annotations

import hashlib
import os
import shutil
import tempfile
import struct
from dataclasses import dataclass
from pathlib import Path
from functools import partial
from typing import Any

from factory_common import (
    directory_is_empty,
    checked_content_path,
    run_command,
)
from factory_contracts import TRANSFORM_CONTRACT


DDS_MAGIC = 0x20534444
FOURCC_DX10 = 0x30315844
DDSCAPS2_VOLUME = 0x00200000
DDSCAPS2_CUBEMAP = 0x00000200
DDSCAPS2_CUBEMAP_FACES = 0x0000FC00
DDS_RESOURCE_MISC_TEXTURECUBE = 0x00000004
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
TRANSFORM_IDS = {name: transform[0] for name, transform in TRANSFORM_CONTRACT.items()}
TRANSFORM_NAMES = {name: transform[1] for name, transform in TRANSFORM_CONTRACT.items()}


# Describe the first image's top mip; the payload is read separately.
@dataclass(frozen=True)
class DDSFirstMipInfo:
    format: str
    format_name: str
    width: int
    height: int
    mip_count: int
    array_size: int
    data_offset: int
    data_size: int


# Read one little-endian DDS field after checking that its bytes are present.
def read_u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("truncated DDS header")
    return struct.unpack_from("<I", data, offset)[0]


# Identify a supported DDS format and locate array item zero, mip zero block data.
def parse_dds_first_mip(data: bytes) -> DDSFirstMipInfo:
    # Check the basic DDS header before reading dimensions and format fields.
    if len(data) < 128 or read_u32(data, 0) != DDS_MAGIC or read_u32(data, 4) != 124:
        raise ValueError("not a valid DDS file")
    width = read_u32(data, 16)
    height = read_u32(data, 12)
    mip_count = max(1, read_u32(data, 28))
    if width == 0 or height == 0:
        raise ValueError("DDS dimensions must be non-zero")
    # This stage selects a 2D image, not a cubemap face; reject both cube header forms.
    if read_u32(data, 112) & (DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_FACES):
        raise ValueError("GACL stage requires a 2D DDS texture; cubemaps are unsupported")
    fourcc = read_u32(data, 84)
    offset = 128
    array_size = 1
    # Read the extra DX10 header only when the file says it is present.
    if fourcc == FOURCC_DX10:
        if len(data) < 148:
            raise ValueError("truncated DDS DX10 header")
        if read_u32(data, 136) & DDS_RESOURCE_MISC_TEXTURECUBE:
            raise ValueError("GACL stage requires a 2D DDS texture; cubemaps are unsupported")
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
        if read_u32(data, 112) & DDSCAPS2_VOLUME:
            raise ValueError("GACL stage requires a 2D DDS texture; legacy volume textures are unsupported")
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
    return DDSFirstMipInfo(
        format=format_name,
        format_name=exact_name,
        width=width,
        height=height,
        mip_count=mip_count,
        array_size=array_size,
        data_offset=offset,
        data_size=size,
    )


run_tool = partial(run_command, operation="GACLContentTool", text=True)

# Preserve source subfolders when naming the conditioned GACL derivative.
def output_relative_path(source: str) -> str:
    source_path = Path(source)
    filename = f"{source_path.name}.gacl"
    return (Path("generated") / "gacl" / source_path.parent / filename).as_posix()


# Condition supported DDS first-mip payloads, verify them, and collect output records.
def build_variants(
    input_directory: Path,
    sources: list[dict[str, Any]],
    output_root: Path,
    executable: Path,
    zstd_level: int,
    target_block_size: int,
) -> list[dict[str, Any]]:
    derivative_records: list[dict[str, Any]] = []
    for original_file in sources:
        # Only DDS inputs participate in this texture-conditioning stage.
        if Path(original_file["path"]).suffix.lower() != ".dds":
            continue
        # Discovery owns path validation; originals must stay unchanged during processing.
        source_path = input_directory / original_file["path"]
        source_data = source_path.read_bytes()
        mip_info = parse_dds_first_mip(source_data)
        # Pass the first mip's block bytes to the codec, not the DDS container.
        payload = source_data[mip_info.data_offset:mip_info.data_offset + mip_info.data_size]
        relative = output_relative_path(original_file["path"])
        output = output_root / Path(relative).relative_to(Path("generated") / "gacl")
        output.parent.mkdir(parents=True, exist_ok=True)
        payload_path = output.with_suffix(output.suffix + ".payload")
        payload_path.write_bytes(payload)
        # Generate the conditioned stream and compare its decoded payload to the original.
        try:
            run_tool([
                str(executable), "--input", str(payload_path), "--output", str(output),
                "--format", mip_info.format, "--zstd-level", str(zstd_level),
                "--target-block-size", str(target_block_size),
            ])
            run_tool([
                str(executable), "--verify", "--input", str(output),
                "--original", str(payload_path), "--format", mip_info.format,
            ])
        # The extracted payload is temporary and must not remain beside the derivative.
        finally:
            payload_path.unlink(missing_ok=True)
        data = output.read_bytes()
        # Record the transform and the exact mip selection needed to interpret this output.
        derivative_records.append({
            "path": relative,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "source": original_file["path"],
            "format": "gacl",
            "parameters": {
                "texture_format": mip_info.format,
                "zstd_level": zstd_level,
                "target_block_size": target_block_size,
                "transform_id": TRANSFORM_IDS[mip_info.format],
                "transform_name": TRANSFORM_NAMES[mip_info.format],
                "transform_version": 1,
            },
            "metadata": {
                "dds_format": mip_info.format_name,
                "width": mip_info.width,
                "height": mip_info.height,
                "mip_count": mip_info.mip_count,
                "array_size": mip_info.array_size,
                "array_item": 0,
                "mip": 0,
                "uncompressed_size": mip_info.data_size,
                "compressed_size": len(data),
                "container": "raw_zstd_stream",
            },
            "validation": "pass",
        })
    if not derivative_records:
        raise ValueError("content set contains no supported BC1/BC3/BC4/BC5/BC7 DDS sources")
    return derivative_records


# Check the requested Zstd level and target compressed-block size.
def validate_options(zstd_level: int, target_block_size: int) -> None:
    if zstd_level < 1 or zstd_level > 22:
        raise ValueError("Zstd level must be from 1 through 22")
    if target_block_size < 1 or target_block_size > 1024 * 1024:
        raise ValueError("target block size must be from 1 through 1048576")


# Generate and verify GACL files before replacing outputs and returning records.
def process(
    input_directory: Path,
    sources: list[dict[str, Any]],
    executable: Path,
    zstd_level: int,
    target_block_size: int,
    overwrite: bool,
) -> list[dict[str, Any]]:
    """Install verified outputs using preflight-validated inputs; return report records."""
    output_directory = checked_content_path(input_directory, "generated/gacl", "gacl output directory")
    if not directory_is_empty(output_directory) and not overwrite:
        raise FileExistsError("GACL outputs already exist; use --overwrite")
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".gacl.stage.", dir=output_directory.parent) as temporary:
        staging = Path(temporary)
        records = build_variants(input_directory, sources, staging, executable, zstd_level, target_block_size)
        # Complete generation before replacing old outputs; failed installation is not rolled back.
        checked_content_path(input_directory, "generated/gacl", "gacl output directory")
        if output_directory.exists():
            shutil.rmtree(output_directory)
        os.replace(staging, output_directory)
    return records
