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
from factory_common import bundled_encoder_path, validate_executable
from factory_request import create_parser, parse_request
from factory_contracts import ZSTD_COMPRESSION_LEVEL, validate_zstd_level

# The remaining processing stages include the matching HLK archive triplet.
STAGE_MODULES = {
    "zstd": zstd_compress,
    "gdeflate": gdeflate_compress,
    "gacl": gacl_compress,
    "hlk": hlk_content_set,
}
ALL_STAGES = list(STAGE_MODULES)


# Validate every selected stage and required tool path before generating files.
def preflight(args: argparse.Namespace, stages: list[str], has_dds: bool) -> dict[str, Any]:
    """Validate selected-stage arguments without running tools or creating outputs."""
    prepared: dict[str, tuple] = {}
    required_tools: dict[str, Path] = {}
    zstd = getattr(args, "zstd_exe", None) or bundled_encoder_path("zstd.exe")
    gdeflate = getattr(args, "gdeflate_exe", None) or bundled_encoder_path("GDeflateContentTool.exe")
    gacl = getattr(args, "gacl_exe", None) or bundled_encoder_path("GACLContentTool.exe")
    # Gather each selected stage's requirements before doing any processing.
    for stage in stages:
        # There is nothing for GACL to process when the set has no DDS textures.
        if stage == "gacl" and not has_dds:
            continue
        # Reuse each stage's own validation rules instead of duplicating them here.
        module = STAGE_MODULES[stage]
        if stage == "zstd":
            validate_zstd_level(getattr(args, "zstd_level", ZSTD_COMPRESSION_LEVEL))
            blocks, chunks = module.resolve_sizes(args.zstd_shader_matrix, args.block_sizes_kb, args.chunk_sizes_kb)
            prepared[stage] = (zstd, blocks, chunks, args.overwrite)
            required_tools["Zstd"] = zstd
        elif stage == "gdeflate":
            levels = module.resolve_levels(args.gdeflate_levels, args.gdeflate_all_levels)
            prepared[stage] = (gdeflate, levels, args.overwrite)
            required_tools["GDeflate"] = gdeflate
        elif stage == "gacl":
            module.validate_options(args.gacl_zstd_level, args.gacl_target_block_size)
            prepared[stage] = (gacl, args.gacl_zstd_level, args.gacl_target_block_size, args.overwrite)
            required_tools["GACL"] = gacl
        elif stage == "hlk":
            module.validate_options(args.hlk_archive_prefix, args.hlk_alignment)
            validate_zstd_level(getattr(args, "hlk_zstd_level", ZSTD_COMPRESSION_LEVEL))
            prepared[stage] = (args.hlk_archive_prefix, zstd, gdeflate, args.hlk_alignment, args.overwrite)
            required_tools["Zstd"] = zstd
            required_tools["GDeflate"] = gdeflate
    # A complete package supplies every selected encoder; never fall back to PATH.
    for name, path in required_tools.items():
        try:
            validate_executable(path)
        except FileNotFoundError as error:
            raise FileNotFoundError(f"{name} bundled tool unavailable: {path}. Use the complete ContentFactory package.") from error
    return prepared


# Run prepared stage arguments in fixed order without resolving settings again.
def process_selected_stages(input_directory: Path, prepared: dict[str, tuple], report: dict[str, Any], zstd_level: int = ZSTD_COMPRESSION_LEVEL, hlk_zstd_level: int = ZSTD_COMPRESSION_LEVEL) -> None:
    for stage in ALL_STAGES:
        if stage in prepared:
            settings = {"compression_level": zstd_level} if stage == "zstd" else {"zstd_level": hlk_zstd_level} if stage == "hlk" else {}
            STAGE_MODULES[stage].process(input_directory, *prepared[stage], report=report, **settings)


# Discover originals, generate and verify selected outputs, and record this invocation.
def run(args: argparse.Namespace) -> Path:
    input_directory = content_report.validate_input_directory(args.input_directory)
    stages = ALL_STAGES if args.stages == ["all"] else args.stages
    if "all" in stages:
        raise ValueError("all cannot be combined with other stages")
    if not stages or set(stages) - set(ALL_STAGES):
        raise ValueError("select valid Content Factory stages")
    if len(stages) != len(set(stages)):
        raise ValueError("stages cannot contain duplicates")
    manifest = content_report.manifest_path(input_directory)

    # Discover the current originals once; previous reports never control processing.
    report = content_report.create_manifest(input_directory)
    has_dds = any(Path(original_file["path"]).suffix.lower() == ".dds"
                  for original_file in report["sources"])
    prepared = preflight(args, stages, has_dds)
    if not prepared:
        # An invocation with no applicable stage produces no new report or outputs.
        raise ValueError("no selected stage applies to the input files")
    process_selected_stages(input_directory, prepared, report, getattr(args, "zstd_level", ZSTD_COMPRESSION_LEVEL), getattr(args, "hlk_zstd_level", ZSTD_COMPRESSION_LEVEL))
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
