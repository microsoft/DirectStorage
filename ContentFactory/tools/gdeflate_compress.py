#!/usr/bin/env python3
"""Generate deterministic GDeflate derivatives at selected levels for a Content Factory set."""

from __future__ import annotations

import hashlib
import os
import shutil
import tempfile
from pathlib import Path
from functools import partial
from typing import Any

from factory_common import (
    directory_is_empty,
    checked_content_path,
    run_command,
)
from factory_contracts import parse_gdeflate_header


# Match the native tool default; HLK encoding keeps its separate fixed level.
DEFAULT_LEVEL = 9


run_tool = partial(run_command, operation="GDeflateContentTool", text=True)


# Require an integer compression level supported by the GDeflate tool.
def validate_level(value: int) -> int:
    # Select one supported level before starting the native tool.
    if type(value) is not int or not 1 <= value <= 12:
        raise ValueError("GDeflate level must be an integer from 1 through 12")
    return value


# Reject empty, duplicate, or invalid level selections and return sorted levels.
def validate_levels(values: list[int] | tuple[int, ...]) -> tuple[int, ...]:
    # Stable, unique levels keep output names and processing order predictable.
    if not values:
        raise ValueError("GDeflate levels cannot be empty")
    checked = tuple(validate_level(value) for value in values)
    if len(set(checked)) != len(checked):
        raise ValueError("GDeflate levels cannot contain duplicates")
    return tuple(sorted(checked))


# Choose the default, an explicit level list, or the complete supported sweep.
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


# Preserve source subfolders and include the GDeflate level in the output name.
def output_relative_path(source: str, level: int) -> str:
    source_path = Path(source)
    # Preserve source subfolders and include the compression level in the name.
    filename = f"{source_path.name}-level{level}.gdeflate"
    return (Path("generated") / "gdeflate" / source_path.parent / filename).as_posix()


# Encode each selected level, verify its output, and collect file and header metadata.
def build_derivatives(
    input_directory: Path,
    sources: list[dict[str, Any]],
    output_root: Path,
    executable: Path,
    levels: tuple[int, ...],
) -> list[dict[str, Any]]:
    derivative_records: list[dict[str, Any]] = []
    for original_file in sources:
        # Discovery owns path validation; originals must stay unchanged during processing.
        source_path = input_directory / original_file["path"]
        if original_file["size"] == 0:
            raise ValueError(f"GDeflate source cannot be empty: {original_file['path']}")
        # Generate only the levels selected for this source.
        for level in levels:
            relative = output_relative_path(original_file["path"], level)
            output = output_root / Path(relative).relative_to(Path("generated") / "gdeflate")
            output.parent.mkdir(parents=True, exist_ok=True)
            run_tool([
                str(executable), "--input", str(source_path), "--output", str(output),
                "--level", str(level),
            ])
            # Native compression checks the written file; Python checks its header and metadata.
            data = output.read_bytes()
            header = parse_gdeflate_header(data)
            if header["level"] != level or header["uncompressed_size"] != original_file["size"]:
                raise RuntimeError(f"GDeflate metadata mismatch: {relative}")
            # Record the exact file bytes, including the native tool's file header.
            derivative_records.append({
                "path": relative,
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "source": original_file["path"],
                "format": "gdeflate",
                "parameters": {"compression_level": level},
                "metadata": {
                    "header_size": header["header_size"],
                    "compressed_size": header["compressed_size"],
                    "uncompressed_size": header["uncompressed_size"],
                },
                "validation": "pass",
            })
    return derivative_records


# Generate and verify GDeflate files before replacing outputs and returning records.
def process(input_directory: Path, sources: list[dict[str, Any]], executable: Path, levels: tuple[int, ...], overwrite: bool) -> list[dict[str, Any]]:
    """Install verified outputs using preflight-validated inputs; return report records."""
    output_directory = checked_content_path(input_directory, "generated/gdeflate", "gdeflate output directory")
    if not directory_is_empty(output_directory) and not overwrite:
        raise FileExistsError("GDeflate outputs already exist; use --overwrite")
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".gdeflate.stage.", dir=output_directory.parent) as temporary:
        staging = Path(temporary)
        records = build_derivatives(input_directory, sources, staging, executable, levels)
        # Complete generation before replacing old outputs; failed installation is not rolled back.
        checked_content_path(input_directory, "generated/gdeflate", "gdeflate output directory")
        if output_directory.exists():
            shutil.rmtree(output_directory)
        os.replace(staging, output_directory)
    return records
