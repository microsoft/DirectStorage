#!/usr/bin/env python3
"""Inventory or verify one deterministic DirectStorage Content Factory set.

The driver owns the consolidated set manifest. Codec stages append verified
``derivatives`` and ``archives`` records as they are integrated. The current
inventory stage establishes the source contract and format-first directories.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path, PurePosixPath
from typing import Any

from factory_common import write_bytes_atomic
from factory_contracts import CONTENT_TYPES, TRANSFORM_CONTRACT

SCHEMA_VERSION = 1
TOOL_VERSION = "0.1.0"
# Set names become directory names, so exclude unsafe Windows names.
SET_NAME_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
FORMAT_DIRECTORIES = ("originals", "zstd", "gdeflate", "gacl", "dstorage")
WINDOWS_RESERVED_NAMES = {
    "CON", "PRN", "AUX", "NUL",
    *(f"COM{index}" for index in range(1, 10)),
    *(f"LPT{index}" for index in range(1, 10)),
}


def validate_set_name(value: str) -> str:
    device_name = value.split(".", 1)[0].upper()
    if (
        not SET_NAME_PATTERN.fullmatch(value)
        or value in {".", ".."}
        or value.endswith(".")
        or device_name in WINDOWS_RESERVED_NAMES
    ):
        raise ValueError(
            "set name must be a Windows-safe name containing only letters, digits, '.', '_', or '-'"
        )
    return value


def sha256_file(path: Path) -> str:
    # Hash in small reads rather than loading the whole file into memory.
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def relative_path(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def inventory_sources(root: Path, set_name: str) -> list[dict[str, Any]]:
    source_root = root / "originals" / set_name
    if source_root.is_symlink():
        raise ValueError(f"source set cannot be a symbolic link: {source_root}")
    if not source_root.is_dir():
        raise FileNotFoundError(f"source set does not exist: {source_root}")

    # Inspect links first, then sort files so inventories are repeatable.
    entries = list(source_root.rglob("*"))
    for path in entries:
        if path.is_symlink():
            raise ValueError(f"source set cannot contain symbolic links: {path}")
    files = sorted(
        (path for path in entries if path.is_file()),
        key=lambda path: relative_path(path, source_root),
    )
    if not files:
        raise ValueError(f"source set contains no files: {source_root}")

    # Keep every source inside the set and record its exact size and hash.
    resolved_source_root = source_root.resolve()
    records: list[dict[str, Any]] = []
    for path in files:
        try:
            path.resolve().relative_to(resolved_source_root)
        except ValueError as error:
            raise ValueError(f"source file escapes the source set: {path}") from error
        records.append({
            "path": relative_path(path, source_root),
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        })
    return records


# New sets start with originals only; later stages add output records.
def create_manifest(root: Path, set_name: str) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "set_name": set_name,
        "source_root": f"originals/{set_name}",
        "sources": inventory_sources(root, set_name),
        "derivatives": [],
        "archives": [],
        "tools": {"process-set.py": {"version": TOOL_VERSION}},
    }


def validate_relative_manifest_path(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")
    # Manifest paths use forward slashes and must stay relative to the root.
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "." in path.parts or "\\" in value:
        raise ValueError(f"{field} must be a safe normalized relative path")
    return value


def validate_file_record(record: object, field: str) -> dict[str, Any]:
    if not isinstance(record, dict):
        raise ValueError(f"{field} must be an object")
    validate_relative_manifest_path(record.get("path"), f"{field}.path")
    if not isinstance(record.get("size"), int) or record["size"] < 0:
        raise ValueError(f"{field}.size must be a non-negative integer")
    # A file record must contain a complete lowercase SHA-256 digest.
    digest = record.get("sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError(f"{field}.sha256 must be a lowercase SHA-256")
    return record


def validate_record_keys(record: dict[str, Any], expected: set[str], field: str) -> None:
    if set(record) != expected:
        raise ValueError(f"{field} keys must be exactly: {', '.join(sorted(expected))}")


def validate_output_path(path: str, format_name: str, set_name: str, field: str) -> None:
    parts = PurePosixPath(path).parts
    if len(parts) < 3 or parts[:2] != (format_name, set_name):
        raise ValueError(f"{field} must be under {format_name}/{set_name}/")


def validate_manifest(document: object) -> dict[str, Any]:
    if not isinstance(document, dict):
        raise ValueError("manifest must be an object")
    # Validate the overall shape before looking at individual records.
    required = {"schema_version", "set_name", "source_root", "sources", "derivatives", "archives", "tools"}
    if set(document) != required:
        raise ValueError(f"manifest keys must be exactly: {', '.join(sorted(required))}")
    if document["schema_version"] != SCHEMA_VERSION:
        raise ValueError(f"unsupported manifest schema version {document['schema_version']!r}")
    if not isinstance(document["set_name"], str):
        raise ValueError("set_name must be a string")
    set_name = validate_set_name(document["set_name"])
    if document["source_root"] != f"originals/{set_name}":
        raise ValueError("source_root must match originals/<set-name>")

    # Require a stable, unique source list to anchor all generated outputs.
    sources = document["sources"]
    if not isinstance(sources, list) or not sources:
        raise ValueError("sources must be a non-empty array")
    source_paths: list[str] = []
    for index, record in enumerate(sources):
        item = validate_file_record(record, f"sources[{index}]")
        validate_record_keys(item, {"path", "size", "sha256"}, f"sources[{index}]")
        source_paths.append(item["path"])
    if source_paths != sorted(source_paths) or len(source_paths) != len(set(source_paths)):
        raise ValueError("sources must be uniquely sorted by path")

    # Each generated file records its source, codec settings, and verification result.
    derivatives = document["derivatives"]
    if not isinstance(derivatives, list):
        raise ValueError("derivatives must be an array")
    derivative_paths: set[str] = set()
    derivative_order: list[str] = []
    derivative_keys = {"path", "size", "sha256", "source", "format", "parameters", "metadata", "validation"}
    for index, record in enumerate(derivatives):
        item = validate_file_record(record, f"derivatives[{index}]")
        validate_record_keys(item, derivative_keys, f"derivatives[{index}]")
        if item["format"] not in {"zstd", "gdeflate", "gacl"}:
            raise ValueError(f"derivatives[{index}].format is invalid")
        if not isinstance(item["parameters"], dict) or not isinstance(item["metadata"], dict):
            raise ValueError(f"derivatives[{index}] parameters and metadata must be objects")
        # Check the metadata fields specific to each codec.
        if item["format"] == "zstd":
            # Older records omitted the level; keep them readable without inventing a value.
            keys = set(item["parameters"])
            if keys not in ({"block_size_kb", "chunk_size_kb"},
                            {"block_size_kb", "chunk_size_kb", "compression_level"}):
                raise ValueError(f"derivatives[{index}] has invalid Zstd parameters")
            if "compression_level" in item["parameters"]:
                level = item["parameters"]["compression_level"]
                if type(level) is not int or not 1 <= level <= 22:
                    raise ValueError(f"derivatives[{index}] has invalid Zstd compression level")
            block_kb = item["parameters"]["block_size_kb"]
            chunk_kb = item["parameters"]["chunk_size_kb"]
            if (type(block_kb) is not int or type(chunk_kb) is not int or
                    block_kb <= 0 or chunk_kb <= 0 or block_kb > chunk_kb):
                raise ValueError(f"derivatives[{index}] has invalid Zstd sizes")
            if set(item["metadata"]) != {"frame_count", "uncompressed_size"}:
                raise ValueError(f"derivatives[{index}] has invalid Zstd metadata")
            if (type(item["metadata"]["frame_count"]) is not int or item["metadata"]["frame_count"] <= 0 or
                    type(item["metadata"]["uncompressed_size"]) is not int or item["metadata"]["uncompressed_size"] <= 0):
                raise ValueError(f"derivatives[{index}] has invalid Zstd metadata values")
        # GDeflate files include a header as well as the compressed bytes.
        elif item["format"] == "gdeflate":
            if set(item["parameters"]) != {"compression_level"}:
                raise ValueError(f"derivatives[{index}] has invalid GDeflate parameters")
            level = item["parameters"]["compression_level"]
            if type(level) is not int or level < 1 or level > 12:
                raise ValueError(f"derivatives[{index}] has invalid GDeflate level")
            if set(item["metadata"]) != {"header_size", "compressed_size", "uncompressed_size"}:
                raise ValueError(f"derivatives[{index}] has invalid GDeflate metadata")
            metadata = item["metadata"]
            if (metadata["header_size"] != 32 or type(metadata["compressed_size"]) is not int or
                    metadata["compressed_size"] <= 0 or type(metadata["uncompressed_size"]) is not int or
                    metadata["uncompressed_size"] <= 0 or
                    item["size"] != metadata["header_size"] + metadata["compressed_size"]):
                raise ValueError(f"derivatives[{index}] has invalid GDeflate metadata values")
        # GACL must identify the texture format, transform, and selected mip.
        elif item["format"] == "gacl":
            parameter_keys = {
                "texture_format", "zstd_level", "target_block_size", "transform_id",
                "transform_name", "transform_version",
            }
            metadata_keys = {
                "dds_format", "width", "height", "mip_count", "array_size", "array_item",
                "mip", "uncompressed_size", "compressed_size", "container",
            }
            if set(item["parameters"]) != parameter_keys or set(item["metadata"]) != metadata_keys:
                raise ValueError(f"derivatives[{index}] has invalid GACL parameters or metadata")
            parameters = item["parameters"]
            metadata = item["metadata"]
            # The transform ID and name must agree with the recorded texture format.
            texture_format = parameters["texture_format"]
            if texture_format not in TRANSFORM_CONTRACT or (
                    parameters["transform_id"], parameters["transform_name"]) != TRANSFORM_CONTRACT[texture_format]:
                raise ValueError(f"derivatives[{index}] has an unsupported GACL transform")
            if (type(parameters["zstd_level"]) is not int or not 1 <= parameters["zstd_level"] <= 22 or
                    type(parameters["target_block_size"]) is not int or
                    not 1 <= parameters["target_block_size"] <= 1024 * 1024 or
                    parameters["transform_version"] != 1):
                raise ValueError(f"derivatives[{index}] has invalid GACL parameters")
            if (not isinstance(metadata["dds_format"], str) or not metadata["dds_format"] or
                    type(metadata["width"]) is not int or metadata["width"] <= 0 or
                    type(metadata["height"]) is not int or metadata["height"] <= 0 or
                    type(metadata["mip_count"]) is not int or metadata["mip_count"] <= 0 or
                    type(metadata["array_size"]) is not int or metadata["array_size"] <= 0 or
                    metadata["array_item"] != 0 or metadata["mip"] != 0 or
                    type(metadata["uncompressed_size"]) is not int or metadata["uncompressed_size"] <= 0 or
                    type(metadata["compressed_size"]) is not int or metadata["compressed_size"] <= 0 or
                    metadata["compressed_size"] != item["size"] or metadata["container"] != "raw_zstd_stream"):
                raise ValueError(f"derivatives[{index}] has invalid GACL metadata values")
        # Tie the output to a known source and reject repeated output paths.
        validate_output_path(item["path"], item["format"], set_name, f"derivatives[{index}].path")
        validate_relative_manifest_path(item["source"], f"derivatives[{index}].source")
        if item["source"] not in source_paths:
            raise ValueError(f"derivatives[{index}] references an unknown source")
        if item["validation"] != "pass":
            raise ValueError(f"derivatives[{index}] is not validated")
        if item["path"] in derivative_paths:
            raise ValueError(f"derivatives[{index}] duplicates an output path")
        derivative_paths.add(item["path"])
        derivative_order.append(item["path"])
    if derivative_order != sorted(derivative_order):
        raise ValueError("derivatives must be sorted by path")

    # Preserve validation of legacy derivative archives; new tools create only HLK triplets.
    archives = document["archives"]
    if not isinstance(archives, list):
        raise ValueError("archives must be an array")
    archive_paths: list[str] = []
    hlk_groups: dict[str, dict[str, list[tuple[str, str]]]] = {}
    generic_archive_keys = {"path", "size", "sha256", "format", "entries", "validation"}
    hlk_archive_keys = generic_archive_keys | {"archive_group", "payload_codec"}
    for index, record in enumerate(archives):
        item = validate_file_record(record, f"archives[{index}]")
        if set(item) not in {frozenset(generic_archive_keys), frozenset(hlk_archive_keys)}:
            raise ValueError(f"archives[{index}] has an invalid field set")
        is_hlk_set = set(item) == hlk_archive_keys
        if is_hlk_set and (
                not isinstance(item["archive_group"], str) or not item["archive_group"] or
                item["payload_codec"] not in {"uncompressed", "gdeflate", "zstd"}):
            raise ValueError(f"archives[{index}] has invalid HLK archive metadata")
        if item["format"] != "dstorage" or item["validation"] != "pass":
            raise ValueError(f"archives[{index}] has an invalid format or validation state")
        validate_output_path(item["path"], "dstorage", set_name, f"archives[{index}].path")
        archive_paths.append(item["path"])
        # Entry indices and references must match the archive's recorded order.
        entries = item["entries"]
        if not isinstance(entries, list) or not entries:
            raise ValueError(f"archives[{index}].entries must be non-empty")
        for entry_index, entry in enumerate(entries):
            expected_entry_keys = {"index", "source", "content_type"} if is_hlk_set else {
                "index", "derivative", "content_type"
            }
            if not isinstance(entry, dict) or set(entry) != expected_entry_keys:
                raise ValueError(f"archives[{index}].entries[{entry_index}] is invalid")
            if entry["index"] != entry_index:
                raise ValueError(f"archives[{index}] entry indices must be contiguous and ordered")
            if is_hlk_set:
                if entry["source"] not in source_paths:
                    raise ValueError(f"archives[{index}] references an unknown source")
            elif entry["derivative"] not in derivative_paths:
                raise ValueError(f"archives[{index}] references an unknown derivative")
            if entry["content_type"] not in CONTENT_TYPES:
                raise ValueError(f"archives[{index}] has an invalid content type")
        if is_hlk_set:
            group = hlk_groups.setdefault(item["archive_group"], {})
            if item["payload_codec"] in group:
                raise ValueError(f"HLK archive group {item['archive_group']!r} has a duplicate codec")
            group[item["payload_codec"]] = [
                (entry["source"], entry["content_type"]) for entry in entries
            ]
    if archive_paths != sorted(archive_paths) or len(archive_paths) != len(set(archive_paths)):
        raise ValueError("archives must be uniquely sorted by path")
    # All three HLK archives must describe the same sources in the same order.
    for group_name, codecs in hlk_groups.items():
        if set(codecs) != {"uncompressed", "gdeflate", "zstd"}:
            raise ValueError(f"HLK archive group {group_name!r} must contain all three payload codecs")
        memberships = list(codecs.values())
        if any(membership != memberships[0] for membership in memberships[1:]):
            raise ValueError(f"HLK archive group {group_name!r} entries are not lockstep")

    # Keep enough tool information to identify how the set was generated.
    tools = document["tools"]
    if not isinstance(tools, dict):
        raise ValueError("tools must be an object")
    for name, tool in tools.items():
        if not isinstance(name, str) or not name or not isinstance(tool, dict):
            raise ValueError("tool entries must be named objects")
        if set(tool) - {"version", "revision"}:
            raise ValueError(f"tools.{name} contains unknown keys")
        if not isinstance(tool.get("version"), str) or not tool["version"]:
            raise ValueError(f"tools.{name}.version must be non-empty")
        if "revision" in tool and (not isinstance(tool["revision"], str) or not tool["revision"]):
            raise ValueError(f"tools.{name}.revision must be non-empty when present")
    return document


def manifest_path(root: Path, set_name: str) -> Path:
    return root / "manifests" / f"{set_name}.json"


def write_json_atomic(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Use sorted keys and a final newline so repeated writes produce the same bytes.
    data = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")
    write_bytes_atomic(path, data)

def reject_symlink_components(root: Path, relative: str, field: str) -> Path:
    path = root / Path(relative)
    current = root
    # Check each directory component, not just the final file, for symbolic links.
    for part in Path(relative).parts:
        current /= part
        if current.is_symlink():
            raise ValueError(f"{field} cannot contain symbolic links: {relative}")
    try:
        path.resolve().relative_to(root)
    except ValueError as error:
        raise ValueError(f"{field} escapes the Content Factory root: {relative}") from error
    return path


def verify_files(root: Path, set_name: str, document: dict[str, Any]) -> None:
    # Reinventory originals to catch missing, added, or changed source files.
    actual_sources = inventory_sources(root, set_name)
    if actual_sources != document["sources"]:
        raise ValueError("source inventory differs from the manifest")

    # Check generated files against the sizes and hashes already recorded.
    for group in ("derivatives", "archives"):
        for record in document[group]:
            path = reject_symlink_components(root, record["path"], f"{group} path")
            if not path.is_file():
                raise FileNotFoundError(path)
            if path.stat().st_size != record["size"] or sha256_file(path) != record["sha256"]:
                raise ValueError(f"{group} file does not match manifest: {record['path']}")


def ensure_format_directories(root: Path, set_name: str) -> None:
    for name in FORMAT_DIRECTORIES[1:]:
        (root / name / set_name).mkdir(parents=True, exist_ok=True)


def load_manifest(path: Path) -> dict[str, Any]:
    return validate_manifest(json.loads(path.read_text(encoding="utf-8")))


def process(root: Path, set_name: str, verify_only: bool, overwrite: bool) -> Path:
    root = root.resolve()
    set_name = validate_set_name(set_name)
    output = manifest_path(root, set_name)

    # Verification reads the existing set without regenerating its manifest.
    if verify_only:
        document = load_manifest(output)
        if document["set_name"] != set_name:
            raise ValueError("manifest set_name does not match the requested set")
        verify_files(root, set_name, document)
        return output

    # Do not replace an inventory that would lose existing output records.
    if output.exists() and not overwrite:
        raise FileExistsError(f"manifest already exists; use --overwrite: {output}")
    if output.exists():
        existing = load_manifest(output)
        if existing["derivatives"] or existing["archives"]:
            raise ValueError("--overwrite cannot discard recorded derivatives or archives")

    # Create the inventory and empty output folders only after those checks pass.
    document = validate_manifest(create_manifest(root, set_name))
    ensure_format_directories(root, set_name)
    write_json_atomic(output, document)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("set_name")
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="Content Factory root")
    parser.add_argument("--verify", action="store_true", help="verify the manifest and every recorded file")
    parser.add_argument("--overwrite", action="store_true", help="replace an existing manifest deterministically")
    args = parser.parse_args()
    if args.verify and args.overwrite:
        parser.error("--verify and --overwrite are mutually exclusive")

    try:
        output = process(args.root, args.set_name, args.verify, args.overwrite)
        action = "Verified" if args.verify else "Wrote"
        print(f"{action} {output}")
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
