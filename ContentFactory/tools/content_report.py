#!/usr/bin/env python3
"""Discover original files and write a Content Factory generation report.

Originals stay in place. The generated/manifest.json file records generated
files and archives; the generated subtree is never inventoried as input.

Input directory
    ↓
Check the directory and its ancestors
    ↓
Discover original files, excluding generated/
    ↓
Record relative paths, sizes, and hashes
    ↓
Create an in-memory report
    ↓
Encoding stages append their results
    ↓
Write generated/manifest.json
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any

from factory_common import checked_content_path, write_bytes_atomic

# Require an original-file directory with no redirected root or ancestor components.
def validate_input_directory(input_directory: Path) -> Path:
    lexical = input_directory if input_directory.is_absolute() else Path.cwd() / input_directory
    # No symbolic links or junctions allowed
    for component in reversed((lexical, *lexical.parents)):
        if component.is_symlink() or getattr(component, "is_junction", lambda: False)():
            raise ValueError(f"input directory cannot contain a symbolic link or junction: {component}")
    resolved = lexical.resolve()
    if not resolved.is_dir():
        raise FileNotFoundError(f"input directory does not exist: {resolved}")
    return resolved


# Compute the SHA256 of a file
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
        # Each entry is visited once; checked parents need not be resolved again.
        for name in folders + names:
            path = directory / name
            if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
                raise ValueError(f"original file cannot contain symbolic links or junctions: {path}")
        files.extend(directory / name for name in names if (directory / name).is_file())
    files.sort(key=lambda path: path.relative_to(input_directory).as_posix())
    if not files:
        raise ValueError(f"input directory contains no original files: {input_directory}")
    records = []
    for path in files:
        relative = path.relative_to(input_directory).as_posix()
        records.append({"path": relative, "size": path.stat().st_size, "sha256": sha256_file(path)})
    return records


# Record current originals and leave generated-file/archive records empty.
def create_manifest(input_directory: Path) -> dict[str, Any]:
    return {
        "sources": inventory_sources(input_directory),
        "derivatives": [],
        "archives": [],
    }


# Locate the input-directory manifest while rejecting linked or escaping components.
def manifest_path(input_directory: Path) -> Path:
    return checked_content_path(input_directory, "generated/manifest.json", "manifest path")


# Serialize stable UTF-8 JSON and atomically replace its destination.
def write_json_atomic(path: Path, manifest_data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Use sorted keys and a final newline so repeated writes produce the same bytes.
    data = (json.dumps(manifest_data, indent=2, sort_keys=True) + "\n").encode("utf-8")
    write_bytes_atomic(path, data)
