#!/usr/bin/env python3
"""Generate verified content and record the selected processing stages in order."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import content_report
import zstd_compress
import gdeflate_compress
import gacl_compress
import hlk_content_set
from factory_common import validate_executable
from factory_request import create_parser, parse_request
from factory_contracts import validate_zstd_level

# Fixed internal stages execute in this order, regardless of request ordering.
ALL_STAGES = ["zstd", "gdeflate", "gacl", "hlk"]


# Validate settings once and return resolved named options, never positional dispatch tuples.
def preflight(args: argparse.Namespace, stages: list[str], has_dds: bool) -> argparse.Namespace:
    """Validate applicable stages and tool paths without running tools or creating outputs."""
    options = argparse.Namespace(**vars(args))
    options.stages = [stage for stage in ALL_STAGES if stage in stages and (stage != "gacl" or has_dds)]
    required_tools = {}
    if "zstd" in options.stages:
        validate_zstd_level(options.zstd_level)
        options.block_sizes_kb, options.chunk_sizes_kb = zstd_compress.resolve_sizes(
            args.zstd_shader_matrix, args.block_sizes_kb, args.chunk_sizes_kb)
        required_tools["zstd_exe"] = "Zstd"
    if "gdeflate" in options.stages:
        options.gdeflate_levels = gdeflate_compress.resolve_levels(args.gdeflate_levels, args.gdeflate_all_levels)
        required_tools["gdeflate_exe"] = "GDeflate"
    if "gacl" in options.stages:
        gacl_compress.validate_options(args.gacl_zstd_level, args.gacl_target_block_size)
        required_tools["gacl_exe"] = "GACL"
    if "hlk" in options.stages:
        hlk_content_set.validate_options(args.hlk_archive_prefix, args.hlk_alignment)
        validate_zstd_level(options.hlk_zstd_level)
        required_tools["zstd_exe"] = "Zstd"
        required_tools["gdeflate_exe"] = "GDeflate"
    # Resolve each required encoder once, including tools shared by multiple stages.
    for field, label in required_tools.items():
        path = vars(options)[field]
        try:
            setattr(options, field, validate_executable(path))
        except FileNotFoundError as error:
            raise FileNotFoundError(f"{label} bundled tool unavailable: {path}. Use the complete ContentFactory package.") from error
    return options


# Discover once, call stages explicitly, and retain each completed stage's report on disk.
def run(args: argparse.Namespace) -> Path:
    stages = ALL_STAGES if args.stages == ["all"] else args.stages
    if "all" in stages:
        raise ValueError("all cannot be combined with other stages")
    if not stages or set(stages) - set(ALL_STAGES):
        raise ValueError("select valid Content Factory stages")
    if len(stages) != len(set(stages)):
        raise ValueError("stages cannot contain duplicates")
    # Inventory validates the root and reads the current originals; saved reports are never loaded.
    report = content_report.create_manifest(args.input_directory)
    input_directory = args.input_directory.resolve()
    manifest = content_report.manifest_path(input_directory)
    sources = report["sources"]
    has_dds = any(Path(source["path"]).suffix.lower() == ".dds" for source in sources)
    options = preflight(args, stages, has_dds)
    if not options.stages:
        raise ValueError("no selected stage applies to the input files")

    # Only the launcher mutates and writes reports, after a stage installs verified outputs.
    def record_completed(records: list[dict[str, Any]], section: str = "derivatives") -> None:
        report[section].extend(records)
        report[section].sort(key=lambda record: record["path"])
        content_report.write_json_atomic(manifest, report)

    if "zstd" in options.stages:
        records = zstd_compress.process(
            input_directory, sources, options.zstd_exe, options.block_sizes_kb,
            options.chunk_sizes_kb, options.overwrite, compression_level=options.zstd_level)
        record_completed(records)
    if "gdeflate" in options.stages:
        records = gdeflate_compress.process(
            input_directory, sources, options.gdeflate_exe, options.gdeflate_levels, options.overwrite)
        record_completed(records)
    if "gacl" in options.stages:
        records = gacl_compress.process(
            input_directory, sources, options.gacl_exe, options.gacl_zstd_level,
            options.gacl_target_block_size, options.overwrite)
        record_completed(records)
    if "hlk" in options.stages:
        records = hlk_content_set.process(
            input_directory, sources, options.hlk_archive_prefix, options.zstd_exe,
            options.gdeflate_exe, options.hlk_alignment, options.overwrite,
            zstd_level=options.hlk_zstd_level)
        record_completed(records, "archives")
    return manifest


# Parse factory options and coordinate the requested processing stages.
def main() -> int:
    # Redirected Windows streams may use a legacy encoding; reporting must not fail processing.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    parser = create_parser()
    try:
        args = parse_request(parser)
        output = run(args)
        print(f"Wrote {output}")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
