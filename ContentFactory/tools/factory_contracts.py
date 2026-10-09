"""Shared Python wire-format definitions; native codecs keep their own contracts."""

from __future__ import annotations

import struct

# Common default for shader, HLK, and GACL Zstd content; explicit GACL overrides remain supported.
ZSTD_COMPRESSION_LEVEL = 19

# These numbers are stored in archive entries; keep their meanings consistent.
CONTENT_TYPES = {"unknown": 0, "texture": 1, "geometry": 2, "text": 3}
# Pair each texture format with the transform recorded in its manifest.
TRANSFORM_CONTRACT = {
    "BC1": (1, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC1_224"),
    "BC3": (2, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC3_116224"),
    "BC4": (3, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC4_116"),
    "BC5": (4, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC5_116116"),
    "BC7": (7, "GACL_SHUFFLE_TRANSFORM_ZSTD_ONLY"),
}
# Describe the native tool's little-endian file header, not the compressed payload.
GDEFLATE_HEADER = struct.Struct("<IHHIIQQ")
GDEFLATE_MAGIC = 0x31464447
GDEFLATE_VERSION = 1


def parse_gdeflate_header(data: bytes) -> dict[str, int]:
    # Check the header is complete before unpacking fields from it.
    if len(data) < GDEFLATE_HEADER.size:
        raise RuntimeError("GDeflate header is truncated")
    magic, version, header_size, level, reserved, original_size, compressed_size = GDEFLATE_HEADER.unpack_from(data)
    # Reject files with the wrong format marker or unsupported header layout.
    if (magic, version, header_size, reserved) != (
        GDEFLATE_MAGIC, GDEFLATE_VERSION, GDEFLATE_HEADER.size, 0,
    ):
        raise RuntimeError("invalid GDeflate header")
    if not 1 <= level <= 12:
        raise RuntimeError("invalid GDeflate level in header")
    # The recorded payload length must account for every byte after the header.
    if len(data) != header_size + compressed_size:
        raise RuntimeError("GDeflate file size does not match header")
    # Return the fields each caller needs for its own size and level checks.
    return {
        "level": level,
        "header_size": header_size,
        "uncompressed_size": original_size,
        "compressed_size": compressed_size,
    }
