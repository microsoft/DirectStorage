#!/usr/bin/env python3
"""
Generate histogram PNGs from zstdgpu_demo performance CSV output.

Usage:
    python generate_histogram.py --input <csv_file> --output <png_file> [--title <title>]

Consumes the wide-format CSV emitted by zstdgpu_demo's --out-csv flag:
    RunIdx, Stage 0 (us), Stage 0 :: <scope> (us), ..., Readback 0 (us),
    Stage 1 (us), ..., Stage 2 (us), Bandwidth (GB/s)

Plots a histogram of the 'Bandwidth (GB/s)' column. Per-stage timing columns
are preserved in the CSV but not plotted here (the histogram is the summary
view; users wanting per-stage detail can read the CSV directly).
"""

import argparse
import csv
import math
import sys


def try_import_matplotlib():
    try:
        import matplotlib
        matplotlib.use("Agg")  # Non-interactive backend for CI
        import matplotlib.pyplot as plt
        return plt
    except ImportError:
        print(
            "WARNING: matplotlib not installed. Install with: pip install matplotlib",
            file=sys.stderr,
        )
        return None


# Match Pavel's --out-csv column header verbatim ("Bandwidth (GB/s)").
# Kept case-insensitive and whitespace-tolerant in case the schema spelling
# drifts upstream — the eyeballed match is "bandwidth".
def _is_bandwidth_column(col_name: str) -> bool:
    return col_name is not None and "bandwidth" in col_name.strip().lower()


def main():
    parser = argparse.ArgumentParser(description="Generate histogram from zstdgpu perf CSV")
    parser.add_argument("--input", required=True, help="Path to input CSV file")
    parser.add_argument("--output", required=True, help="Path to output PNG file")
    parser.add_argument("--title", default="Bandwidth", help="Chart title")
    args = parser.parse_args()

    plt = try_import_matplotlib()
    if plt is None:
        return 1

    data = []
    skipped_non_finite = 0
    bandwidth_col = None
    with open(args.input, newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            print(f"CSV has no header row: {args.input}", file=sys.stderr)
            return 1
        # Pick the bandwidth column by name (resilient to header drift).
        for col in reader.fieldnames:
            if _is_bandwidth_column(col):
                bandwidth_col = col
                break
        if bandwidth_col is None:
            print(
                f"No Bandwidth column found in {args.input} "
                f"(headers: {reader.fieldnames})",
                file=sys.stderr,
            )
            return 1

        for row in reader:
            raw = row.get(bandwidth_col, "")
            if raw is None or raw == "":
                continue
            try:
                val = float(raw)
            except ValueError:
                # Pavel may write empty strings for skipped iterations; ignore.
                continue
            if math.isfinite(val):
                data.append(val)
            else:
                skipped_non_finite += 1

    if skipped_non_finite > 0:
        print(
            f"WARNING: Skipped {skipped_non_finite} non-finite (Inf/NaN) value(s) from {args.input}",
            file=sys.stderr,
        )

    if not data:
        print(
            f"No finite Bandwidth (GB/s) data found in {args.input}",
            file=sys.stderr,
        )
        return 1

    plt.figure()
    plt.hist(data, bins=20)
    plt.xlabel("Bandwidth (GB/s)")
    plt.ylabel("Count")
    plt.title(args.title)
    plt.savefig(args.output)
    plt.close()
    print(f"Generated: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
