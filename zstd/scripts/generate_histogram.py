#!/usr/bin/env python3
"""
Generate histogram PNGs from zstdgpu_demo performance CSV output.

Usage:
    python generate_histogram.py --input <csv_file> --output <png_file> [--title <title>]

Supports two CSV formats based on profiling level:
  - prf-lvl 0 (OverallThroughput): column "Throughput_GBs"
  - prf-lvl 2 (PerStageTiming):    column "Microseconds"
"""

import argparse
import csv
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


def main():
    parser = argparse.ArgumentParser(description="Generate histogram from zstdgpu perf CSV")
    parser.add_argument("--input", required=True, help="Path to input CSV file")
    parser.add_argument("--output", required=True, help="Path to output PNG file")
    parser.add_argument("--title", default="Throughput", help="Chart title")
    args = parser.parse_args()

    plt = try_import_matplotlib()
    if plt is None:
        return 1

    data = []
    with open(args.input, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if "Throughput_GBs" in row:
                data.append(float(row["Throughput_GBs"]))
            elif "Microseconds" in row:
                data.append(float(row["Microseconds"]))

    if not data:
        print(f"No Throughput_GBs or Microseconds data found in {args.input}", file=sys.stderr)
        return 1

    plt.figure()
    plt.hist(data, bins=20)
    plt.xlabel("Throughput (GB/s)" if "throughput" in args.input.lower() else "Time (us)")
    plt.ylabel("Count")
    plt.title(args.title)
    plt.savefig(args.output)
    plt.close()
    print(f"Generated: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
