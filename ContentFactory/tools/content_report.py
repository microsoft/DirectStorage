#!/usr/bin/env python3
"""Discover original files and write a Content Factory generation report.

Originals stay in place. The generated/manifest.json file records generated
files and archives; the generated subtree is never inventoried as input.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
from typing import Any

from factory_common import reject_symlink_components, write_bytes_atomic

TOOL_VERSION = "0.2.0"


# Require an original-file directory with no redirected root or ancestor components.
def validate_input_directory(input_directory: Path) -> Path:
    lexical = input_directory if input_directory.is_absolute() else Path.cwd() / input_directory
    # Inspect the unresolved chain: resolve() would hide ancestor junctions and symlinks.
    for component in reversed((lexical, *lexical.parents)):
        if component.is_symlink() or getattr(component, "is_junction", lambda: False)():
            raise ValueError(f"input directory cannot contain a symbolic link or junction: {component}")
    resolved = lexical.resolve()
    if not resolved.is_dir():
        raise FileNotFoundError(f"input directory does not exist: {resolved}")
    return resolved


# Compute a file digest using bounded reads rather than loading the full file.
def sha256_file(path: Path) -> str:
    # Hash in small reads rather than loading the whole file into memory.
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


# Inventory originals recursively without following links or rediscovering generated outputs.
def inventory_sources(input_directory: Path) -> list[dict[str, Any]]:
    input_directory = validate_input_directory(input_directory)
    files = []
    # Never publish an incomplete inventory if a nested directory cannot be read.
    def fail_inventory(error: OSError) -> None:
        raise error
    for directory, folders, names in os.walk(input_directory, followlinks=False, onerror=fail_inventory):
        directory = Path(directory)
        if directory == input_directory:
            # Reserve this whole subtree, including temporary processing files.
            folders[:] = [name for name in folders if name.casefold() != "generated"]
            names = [name for name in names if name.casefold() != "generated"]
        for name in folders + names:
            path = directory / name
            reject_symlink_components(input_directory, path.relative_to(input_directory).as_posix(), "original file")
        files.extend(directory / name for name in names if (directory / name).is_file())
    files.sort(key=lambda path: path.relative_to(input_directory).as_posix())
    if not files:
        raise ValueError(f"input directory contains no original files: {input_directory}")
    records = []
    for path in files:
        relative = path.relative_to(input_directory).as_posix()
        validate_relative_path(relative, "original path")
        records.append({"path": relative, "size": path.stat().st_size, "sha256": sha256_file(path)})
    return records


# Record current originals and leave generated-file/archive records empty.
def create_manifest(input_directory: Path) -> dict[str, Any]:
    return {
        "sources": inventory_sources(input_directory),
        "derivatives": [],
        "archives": [],
        "tools": {"content_report.py": {"version": TOOL_VERSION}},
    }


# Require a canonical relative path without aliases, parent segments, or drive syntax.
def validate_relative_path(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")
    # Original-file paths use forward slashes and stay relative to the input directory.
    path = PurePosixPath(value)
    if (not path.parts or path.is_absolute() or ".." in path.parts or "." in path.parts or "\\" in value
            or ":" in value or value != path.as_posix()):
        raise ValueError(f"{field} must be a safe normalized relative path")
    return value


# Locate the input-directory manifest while rejecting linked or escaping components.
def manifest_path(input_directory: Path) -> Path:
    return reject_symlink_components(input_directory, "generated/manifest.json", "manifest path")


# Serialize stable UTF-8 JSON and atomically replace its destination.
def write_json_atomic(path: Path, manifest_data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Use sorted keys and a final newline so repeated writes produce the same bytes.
    data = (json.dumps(manifest_data, indent=2, sort_keys=True) + "\n").encode("utf-8")
    write_bytes_atomic(path, data)
