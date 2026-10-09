#!/usr/bin/env python3
"""Test-only deterministic stand-in for GDeflateContentTool."""

import argparse
import struct
from pathlib import Path

HEADER = struct.Struct("<IHHIIQQ")
MAGIC = 0x31464447


def decode(path: Path) -> bytes:
    data = path.read_bytes()
    magic, version, header_size, level, reserved, original_size, compressed_size = HEADER.unpack_from(data)
    if (magic, version, header_size, reserved) != (MAGIC, 1, HEADER.size, 0):
        raise ValueError("invalid header")
    payload = data[header_size:]
    if compressed_size != len(payload) or original_size != len(payload):
        raise ValueError("invalid sizes")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--version", action="store_true")
    parser.add_argument("--input")
    parser.add_argument("--output")
    parser.add_argument("--level", type=int, default=1)
    parser.add_argument("--verify")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    if args.version:
        print("GDeflateContentTool 1.0.0")
        return 0
    if args.verify:
        decode(Path(args.verify))
        print("Verified 1 file(s)")
        return 0
    source = Path(args.input).read_bytes()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(HEADER.pack(MAGIC, 1, HEADER.size, args.level, 0, len(source), len(source)) + source)
    print(f"Compressed 1 file(s) at level {args.level}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
