"""Shared Python wire-format definitions; native codecs keep their own contracts."""

from __future__ import annotations

import struct

# Default Zstd compression level. Only used when request doesn't supply a level.
ZSTD_COMPRESSION_LEVEL = 19

# Archive content type identifiers. Think of these as format contracts
CONTENT_TYPES = {"unknown": 0, "texture": 1, "geometry": 2, "text": 3}

# GACL transform identifiers. These map supported texture formats into two things:
#    A numeric transform ID
#    A descriptive transform name

# Example: TRANSFORM_CONTRACT["BC3"]
# Returns (2, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC3_116224")
TRANSFORM_CONTRACT = {
    "BC1": (1, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC1_224"),
    "BC3": (2, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC3_116224"),
    "BC4": (3, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC4_116"),
    "BC5": (4, "GACL_SHUFFLE_TRANSFORM_ZSTD_BC5_116116"),
    "BC7": (7, "GACL_SHUFFLE_TRANSFORM_ZSTD_ONLY"),
}

# GDeflate wrapper layout
GDEFLATE_HEADER = struct.Struct(
    "<"  # Little-endian, without alignment padding.
    "I"  # magic:           uint32
    "H"  # version:         uint16
    "H"  # header_size:     uint16
    "I"  # level:           uint32
    "I"  # reserved:        uint32
    "Q"  # original_size:   uint64
    "Q"  # compressed_size: uint64
)
GDEFLATE_MAGIC = 0x31464447
GDEFLATE_VERSION = 1


# Inspect a GDeflate file and return the fields our parsers need. Data must contain the entire file, not just the header as we validate total file length
def parse_gdeflate_header(data: bytes) -> dict[str, int]:
    # Check header size, make sure it's complete 
    if len(data) < GDEFLATE_HEADER.size:
        raise RuntimeError("GDeflate header is truncated")

    # Unpack the seven fields and reject files with wrong format markers / unsupported header layouts 
    magic, version, header_size, level, reserved, original_size, compressed_size = GDEFLATE_HEADER.unpack_from(data)

    if (magic, version, header_size, reserved) != (
        GDEFLATE_MAGIC, GDEFLATE_VERSION, GDEFLATE_HEADER.size, 0,
    ):
        raise RuntimeError("invalid GDeflate header")
    if not 1 <= level <= 12:
        raise RuntimeError("invalid GDeflate level in header")
    if len(data) != header_size + compressed_size:
        raise RuntimeError("GDeflate file size does not match header")
        
    # Return the fields each caller needs for its own size and level checks.
    return {
        "level": level,
        "header_size": header_size,
        "uncompressed_size": original_size,
        "compressed_size": compressed_size,
    }


# Validate a zstd level. Must be 1 to 22 and an int
# Strings, bools, floats rejected
def validate_zstd_level(level: int) -> None:
    if type(level) is not int or not 1 <= level <= 22:
        raise ValueError("Zstd compression level must be an integer from 1 to 22")
