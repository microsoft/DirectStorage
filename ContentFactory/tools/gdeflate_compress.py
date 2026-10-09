#!/usr/bin/env python3
"""Generate deterministic GDeflate derivatives at selected levels for a Content Factory set."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path
from functools import partial
from typing import Any

from factory_common import (
    directory_is_empty, load_module, set_lock as common_set_lock,
    run_command, tool_version as common_tool_version, validate_executable, write_bytes_atomic,
)
from factory_contracts import parse_gdeflate_header


# Match the native tool default; HLK encoding keeps its separate fixed level.
DEFAULT_LEVEL = 9


def load_driver():
    return load_module("process_set", "process-set.py")


run_tool = partial(run_command, operation="GDeflateContentTool", text=True)

tool_version = partial(common_tool_version, label="GDeflateContentTool")


def validate_level(value: int) -> int:
    # Select one supported level before starting the native tool.
    if type(value) is not int or not 1 <= value <= 12:
        raise ValueError("GDeflate level must be an integer from 1 through 12")
    return value


def validate_levels(values: list[int] | tuple[int, ...]) -> tuple[int, ...]:
    # Stable, unique levels keep output names and processing order predictable.
    if not values:
        raise ValueError("GDeflate levels cannot be empty")
    checked = tuple(validate_level(value) for value in values)
    if len(set(checked)) != len(checked):
        raise ValueError("GDeflate levels cannot contain duplicates")
    return tuple(sorted(checked))


def resolve_levels(
    levels: list[int] | tuple[int, ...] | None = None,
    all_levels: bool = False,
) -> tuple[int, ...]:
    # A list covers one or several levels; the full sweep is an explicit alternative.
    if levels is not None and all_levels:
        raise ValueError("level list and all-levels cannot be combined")
    if all_levels:
        return tuple(range(1, 13))
    return validate_levels((DEFAULT_LEVEL,) if levels is None else levels)


def output_relative_path(source: str, set_name: str, level: int) -> str:
    source_path = Path(source)
    # Preserve source subfolders and include the compression level in the name.
    filename = f"{source_path.name}-level{level}.gdeflate"
    parent = source_path.parent.as_posix()
    return f"gdeflate/{set_name}/{parent + '/' if parent != '.' else ''}{filename}"


def read_header(path: Path) -> dict[str, int]:
    return parse_gdeflate_header(path.read_bytes())


def build_derivatives(
    root: Path,
    document: dict[str, Any],
    output_root: Path,
    executable: Path,
    levels: tuple[int, ...],
) -> list[dict[str, Any]]:
    set_name = document["set_name"]
    records: list[dict[str, Any]] = []
    for source in document["sources"]:
        source_path = root / "originals" / set_name / Path(source["path"])
        if source["size"] == 0:
            raise ValueError(f"GDeflate source cannot be empty: {source['path']}")
        # Generate only the levels selected for this source.
        for level in levels:
            relative = output_relative_path(source["path"], set_name, level)
            output = output_root / Path(relative).relative_to(Path("gdeflate") / set_name)
            output.parent.mkdir(parents=True, exist_ok=True)
            run_tool([
                str(executable), "--input", str(source_path), "--output", str(output),
                "--level", str(level),
            ])
            # Check the generated stream before trusting the header or recording it.
            run_tool([str(executable), "--verify", str(output)])
            header = read_header(output)
            if header["level"] != level or header["uncompressed_size"] != source["size"]:
                raise RuntimeError(f"GDeflate metadata mismatch: {relative}")
            # Record the exact file bytes, including the native tool's file header.
            data = output.read_bytes()
            records.append({
                "path": relative,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "source": source["path"],
                "format": "gdeflate",
                "parameters": {"compression_level": level},
                "metadata": {
                    "header_size": header["header_size"],
                    "compressed_size": header["compressed_size"],
                    "uncompressed_size": header["uncompressed_size"],
                },
                "validation": "pass",
            })
    return sorted(records, key=lambda item: item["path"])


set_lock = partial(common_set_lock, stage="gdeflate", label="GDeflate")


def process(root: Path, set_name: str, executable: Path, levels: tuple[int, ...], overwrite: bool) -> Path:
    driver = load_driver()
    root = root.resolve()
    set_name = driver.validate_set_name(set_name)
    levels = validate_levels(levels)
    executable = validate_executable(executable)
    # Record the encoder version so outputs can be traced back to their tool.
    version = tool_version(executable)
    manifest = driver.manifest_path(root, set_name)

    with set_lock(root, set_name):
        document = driver.load_manifest(manifest)
        driver.verify_files(root, set_name, document)
        existing = [item for item in document["derivatives"] if item["format"] == "gdeflate"]
        final_root = root / "gdeflate" / set_name
        if final_root.is_symlink():
            raise ValueError(f"GDeflate output root cannot be a symbolic link: {final_root}")
        if (existing or not directory_is_empty(final_root)) and not overwrite:
            raise FileExistsError("GDeflate outputs already exist; use --overwrite")

        # Keep existing outputs intact while building their replacements.
        parent = root / "gdeflate"
        parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=f".{set_name}.gdeflate.stage.", dir=parent))
        backup = Path(tempfile.mkdtemp(prefix=f".{set_name}.gdeflate.backup.", dir=parent))
        backup.rmdir()
        old_manifest = manifest.read_bytes()
        old_tree_moved = False
        new_tree_installed = False
        try:
            records = build_derivatives(root, document, stage, executable, levels)
            updated = dict(document)
            # Update only GDeflate records and retain other codecs' results.
            updated["derivatives"] = sorted(
                [item for item in document["derivatives"] if item["format"] != "gdeflate"] + records,
                key=lambda item: item["path"],
            )
            updated["tools"] = dict(document["tools"])
            updated["tools"]["GDeflateContentTool"] = {"version": version}
            driver.validate_manifest(updated)
            # Back up the previous tree before installing the staged outputs.
            if final_root.exists():
                os.replace(final_root, backup)
                old_tree_moved = True
            os.replace(stage, final_root)
            new_tree_installed = True
            try:
                driver.write_json_atomic(manifest, updated)
                driver.verify_files(root, set_name, updated)
            # Try to restore the old tree and manifest if committing the new set fails.
            except Exception as original_error:
                rollback_errors: list[str] = []
                try:
                    if new_tree_installed and final_root.exists():
                        shutil.rmtree(final_root)
                    if old_tree_moved and backup.exists():
                        os.replace(backup, final_root)
                    write_bytes_atomic(manifest, old_manifest)
                except Exception as rollback_error:
                    rollback_errors.append(str(rollback_error))
                if rollback_errors:
                    raise RuntimeError(
                        f"GDeflate commit failed ({original_error}); rollback also failed: {'; '.join(rollback_errors)}"
                    ) from original_error
                raise
            shutil.rmtree(backup, ignore_errors=True)
            return manifest
        # Remove staging files and recover any old tree that was not replaced.
        finally:
            shutil.rmtree(stage, ignore_errors=True)
            if backup.exists() and not final_root.exists():
                os.replace(backup, final_root)
            elif backup.exists():
                shutil.rmtree(backup, ignore_errors=True)


def main() -> int:
    # Exact option names prevent the removed --level from abbreviating --levels.
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("set_name")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--gdeflate-exe", type=Path, required=True)
    # One-item or multi-item lists and complete coverage are mutually exclusive.
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--levels", nargs="+", type=int, help="one or more levels from 1 through 12 (default: 9)")
    selection.add_argument("--all-levels", action="store_true", help="explicitly generate all levels 1 through 12")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    try:
        levels = resolve_levels(args.levels, args.all_levels)
        output = process(args.root, args.set_name, args.gdeflate_exe, levels, args.overwrite)
        print(f"Updated {output}")
        return 0
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
