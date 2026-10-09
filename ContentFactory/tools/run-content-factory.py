#!/usr/bin/env python3
"""Run Content Factory inventory, derivative, archive, and verification stages in order."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path
from typing import Any

from factory_common import load_module, validate_executable
from factory_contracts import ZSTD_COMPRESSION_LEVEL

# The remaining processing stages include the matching HLK archive triplet.
ALL_STAGES = ["zstd", "gdeflate", "gacl", "hlk"]


def preflight(args: argparse.Namespace, stages: list[str], has_dds: bool) -> dict[str, Any]:
    """Validate selected-stage arguments without running tools or creating outputs."""
    modules: dict[str, Any] = {}
    required_tools: dict[str, Path | None] = {}
    # Gather each selected stage's requirements before doing any processing.
    for stage in stages:
        # There is nothing for GACL to process when the set has no DDS textures.
        if stage == "gacl" and not has_dds:
            continue
        # Reuse each stage's own validation rules instead of duplicating them here.
        filename = {"zstd": "zstd_compress.py", "gdeflate": "gdeflate_compress.py",
                    "gacl": "gacl_compress.py", "hlk": "hlk_content_set.py"}[stage]
        module = load_module(f"{stage}_for_orchestration", filename)
        modules[stage] = module
        if stage == "zstd":
            module.resolve_sizes(args.zstd_shader_matrix, args.block_sizes_kb, args.chunk_sizes_kb)
            required_tools["--zstd-exe"] = args.zstd_exe
        elif stage == "gdeflate":
            module.resolve_levels(args.gdeflate_levels, args.gdeflate_all_levels)
            required_tools["--gdeflate-exe"] = args.gdeflate_exe
        elif stage == "gacl":
            module.validate_options(args.gacl_zstd_level, args.gacl_target_block_size)
            required_tools["--gacl-exe"] = args.gacl_exe
        elif stage == "hlk":
            module.validate_options(args.hlk_archive_prefix, args.hlk_alignment)
            required_tools["--zstd-exe"] = args.zstd_exe
            required_tools["--gdeflate-exe"] = args.gdeflate_exe
    # Report all missing tool arguments together; file checks do not execute them.
    missing = [name for name, path in required_tools.items() if path is None]
    if missing:
        raise ValueError(f"{', '.join(missing)} required for the requested stages")
    for path in required_tools.values():
        validate_executable(path)
    return modules


def run(args: argparse.Namespace) -> Path:
    driver = load_module("process_set_for_orchestration", "process-set.py")
    root = args.root.resolve()
    set_name = driver.validate_set_name(args.set_name)
    # Expand the default selection, but reject mixed or repeated stage names.
    stages = ALL_STAGES if args.stages == ["all"] else args.stages
    if "all" in stages:
        raise ValueError("all cannot be combined with other stages")
    if not stages or set(stages) - set(ALL_STAGES):
        raise ValueError("select valid Content Factory stages")
    if len(stages) != len(set(stages)):
        raise ValueError("stages cannot contain duplicates")

    # Either validate new input files or verify the set already on disk.
    manifest_path = driver.manifest_path(root, set_name)
    if args.sources is not None:
        if not args.sources:
            raise ValueError("--sources requires at least one file")
        source_root = root / "originals" / set_name
        if manifest_path.exists() or source_root.exists():
            raise FileExistsError("--sources can initialize only a new content set")
        # Copied inputs share one directory, so filenames must not collide.
        names: set[str] = set()
        resolved_sources: set[Path] = set()
        for source in args.sources:
            if source.is_symlink() or not source.is_file():
                raise ValueError(f"source must be a regular non-symlink file: {source}")
            resolved = source.resolve()
            if resolved in resolved_sources:
                raise ValueError(f"source paths must reference unique files: {source}")
            if source.name in names:
                raise ValueError(f"source file names must be unique: {source.name}")
            resolved_sources.add(resolved)
            names.add(source.name)
        has_dds = any(source.suffix.lower() == ".dds" for source in args.sources)
    else:
        if not manifest_path.exists():
            raise FileNotFoundError("manifest does not exist; provide --sources to initialize the set")
        document = driver.load_manifest(manifest_path)
        driver.verify_files(root, set_name, document)
        has_dds = any(Path(item["path"]).suffix.lower() == ".dds" for item in document["sources"])

    # Check later stages too, before copying sources or creating any outputs.
    modules = preflight(args, stages, has_dds)
    if args.sources is not None:
        # Only now create the new set and its initial inventory.
        source_root.mkdir(parents=True)
        try:
            for source in args.sources:
                shutil.copyfile(source, source_root / source.name)
            document = driver.create_manifest(root, set_name)
            driver.validate_manifest(document)
            driver.write_json_atomic(manifest_path, document)
        # Remove the copied inputs if set initialization does not finish.
        except Exception:
            shutil.rmtree(source_root, ignore_errors=True)
            raise

    document = driver.load_manifest(manifest_path)
    driver.verify_files(root, set_name, document)
    # Run codec stages in dependency order, regardless of the command-line order.
    if "zstd" in modules:
        module = modules["zstd"]
        blocks, chunks = module.resolve_sizes(args.zstd_shader_matrix, args.block_sizes_kb, args.chunk_sizes_kb)
        manifest_path = module.process(root, set_name, args.zstd_exe, blocks, chunks,
                                       args.overwrite, args.allow_version_change)
    if "gdeflate" in modules:
        module = modules["gdeflate"]
        manifest_path = module.process(root, set_name, args.gdeflate_exe,
                                       module.resolve_levels(args.gdeflate_levels, args.gdeflate_all_levels), args.overwrite)
    # This stage was left out during preflight if there were no DDS sources.
    if "gacl" in modules:
        manifest_path = modules["gacl"].process(
            root, set_name, args.gacl_exe, args.gacl_zstd_level,
            args.gacl_target_block_size, args.overwrite)
    # Build the matching HLK triplet from originals using its own codec settings.
    if "hlk" in modules:
        manifest_path = modules["hlk"].process(
            root, set_name, args.hlk_archive_prefix, args.zstd_exe,
            args.gdeflate_exe, args.hlk_alignment, args.overwrite)
    # Finish by checking every file referenced by the updated manifest.
    document = driver.load_manifest(manifest_path)
    driver.verify_files(root, set_name, document)
    return manifest_path


def main() -> int:
    # Require full option names, including --gdeflate-levels for a single level.
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("set_name")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--sources", nargs="+", type=Path, metavar="FILE")
    parser.add_argument("--stages", nargs="+", choices=["all", *ALL_STAGES], default=["all"])
    parser.add_argument("--overwrite", action="store_true")
    # Zstd defaults to one configuration; the full matrix must be requested.
    parser.add_argument("--zstd-exe", type=Path)
    parser.add_argument(
        "--zstd-shader-matrix", action="store_true",
        help="explicitly generate all 4/8/16 KiB block by 64/128/256 KiB chunk Zstd variants",
    )
    parser.add_argument("--block-sizes-kb", nargs="+", type=int)
    parser.add_argument("--chunk-sizes-kb", nargs="+", type=int)
    parser.add_argument("--allow-version-change", action="store_true")
    # Keep each native codec's settings separate from the archive settings.
    parser.add_argument("--gdeflate-exe", type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--gdeflate-levels", nargs="+", type=int, help="one or more derivative levels from 1 through 12 (default: 9)")
    selection.add_argument("--gdeflate-all-levels", action="store_true", help="all derivative levels 1 through 12")
    parser.add_argument("--gacl-exe", type=Path)
    parser.add_argument("--gacl-zstd-level", type=int, default=ZSTD_COMPRESSION_LEVEL)
    parser.add_argument("--gacl-target-block-size", type=int, default=64 * 1024)
    # These settings affect only the matching HLK archive triplet.
    parser.add_argument("--hlk-archive-prefix", default="dstoragetest")
    parser.add_argument("--hlk-alignment", type=int, default=1)
    args = parser.parse_args()
    try:
        output = run(args)
        print(f"Verified {output}")
        return 0
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
