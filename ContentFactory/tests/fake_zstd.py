#!/usr/bin/env python3
"""Test-only deterministic stand-in for the Zstd CLI process contract."""

import struct
import sys

MAGIC = b"FZST"


def main() -> int:
    if "--version" in sys.argv:
        print("fake-zstd 1.0")
        return 0
    data = sys.stdin.buffer.read()
    if "-d" in sys.argv:
        output = bytearray()
        offset = 0
        while offset < len(data):
            if data[offset:offset + 4] != MAGIC or offset + 8 > len(data):
                print("invalid fake frame", file=sys.stderr)
                return 1
            size = struct.unpack_from("<I", data, offset + 4)[0]
            offset += 8
            if offset + size > len(data):
                print("truncated fake frame", file=sys.stderr)
                return 1
            output += data[offset:offset + size]
            offset += size
        sys.stdout.buffer.write(output)
        return 0
    stream_size = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--stream-size=")), None)
    block_size = next((arg.split("=", 1)[1] for arg in sys.argv if arg.startswith("--target-compressed-block-size=")), None)
    window_log = next((arg.split("=", 1)[1].split("=", 1)[1] for arg in sys.argv
                       if arg.startswith("--zstd=wlog=")), None)
    # Both shader derivatives and single-frame HLK entries explicitly use level 19.
    if ((block_size is not None and "-19" not in sys.argv)
            or (block_size is None and "-19" not in sys.argv)
            or stream_size is None or int(stream_size) != len(data)
            or (block_size is None and window_log != "18") or (block_size is not None and int(block_size) <= 0)):
        print("missing or invalid framing options", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(MAGIC + struct.pack("<I", len(data)) + data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
