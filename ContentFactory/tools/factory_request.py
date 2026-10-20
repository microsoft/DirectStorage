"""Parse JSON processing requests without loading saved output reports."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from factory_common import bundled_encoder_path
from factory_contracts import ZSTD_COMPRESSION_LEVEL

# JSON groups map directly to the internal stage settings.
GROUPS = {
    "zstd": {"level": "zstd_level", "shader_matrix": "zstd_shader_matrix", "block_sizes_kb": "block_sizes_kb", "chunk_sizes_kb": "chunk_sizes_kb"},
    "gdeflate": {"levels": "gdeflate_levels", "all_levels": "gdeflate_all_levels"},
    "gacl": {"zstd_level": "gacl_zstd_level", "target_block_size": "gacl_target_block_size"},
    "hlk": {"archive_prefix": "hlk_archive_prefix", "alignment": "hlk_alignment", "zstd_level": "hlk_zstd_level"},
}
BOOL_FIELDS = {"overwrite", "zstd_shader_matrix", "gdeflate_all_levels"}
INTEGER_FIELDS = {"zstd_level", "hlk_zstd_level", "gacl_zstd_level", "gacl_target_block_size", "hlk_alignment"}
INTEGER_LIST_FIELDS = {"block_sizes_kb", "chunk_sizes_kb", "gdeflate_levels"}


# Reject duplicate JSON keys so mistyped or conflicting requests cannot be silently overridden.
def unique_fields(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate request field: {key}")
        result[key] = value
    return result


# Flatten documented request groups and validate JSON value types before any file generation.
def load_config(path: Path) -> dict:
    request = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique_fields)
    if not isinstance(request, dict):
        raise ValueError("request must be a JSON object")
    unknown = set(request) - {"input_directory", "stages", "overwrite", *GROUPS}
    if unknown:
        raise ValueError(f"unknown request fields: {', '.join(sorted(unknown))}")
    values = {key: value for key, value in request.items() if key not in GROUPS}
    for group, mapping in GROUPS.items():
        if group not in request:
            continue
        options = request[group]
        if not isinstance(options, dict):
            raise ValueError(f"{group} must be a JSON object")
        unknown = set(options) - set(mapping)
        if unknown:
            raise ValueError(f"unknown {group} fields: {', '.join(sorted(unknown))}")
        values.update({mapping[key]: value for key, value in options.items()})
    for key, value in values.items():
        if key in BOOL_FIELDS and type(value) is not bool:
            raise ValueError(f"{key} must be a boolean")
        if key in INTEGER_FIELDS and type(value) is not int:
            raise ValueError(f"{key} must be an integer")
        if key in INTEGER_LIST_FIELDS and (not isinstance(value, list) or not value or any(type(item) is not int for item in value)):
            raise ValueError(f"{key} must be a nonempty list of integers")
        if key == "stages" and (not isinstance(value, list) or not value or any(not isinstance(item, str) for item in value)):
            raise ValueError("stages must be a nonempty list of stage names")
        if key in {"input_directory", "hlk_archive_prefix"} and (not isinstance(value, str) or not value.strip()):
            raise ValueError(f"{key} must be a nonempty string")
    if "input_directory" in values:
        directory = Path(values["input_directory"])
        config_directory = path.parent if path.is_absolute() else Path.cwd() / path.parent
        values["input_directory"] = directory if directory.is_absolute() else config_directory / directory
    return values


# Expose one processing interface: a required JSON request path.
def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--config", required=True, type=Path, help="JSON request; relative input paths use this file's directory")
    return parser


# Apply documented defaults and the JSON request; encoders always come from the package.
def parse_request(parser: argparse.ArgumentParser, argv: list[str] | None = None) -> argparse.Namespace:
    config = parser.parse_args(argv).config
    values = {
        "stages": ["all"], "overwrite": False,
        "zstd_level": ZSTD_COMPRESSION_LEVEL, "zstd_shader_matrix": False, "block_sizes_kb": None, "chunk_sizes_kb": None,
        "gdeflate_levels": None, "gdeflate_all_levels": False,
        "gacl_zstd_level": ZSTD_COMPRESSION_LEVEL, "gacl_target_block_size": 64 * 1024,
        "hlk_archive_prefix": "dstoragetest", "hlk_alignment": 1, "hlk_zstd_level": ZSTD_COMPRESSION_LEVEL,
        "zstd_exe": bundled_encoder_path("zstd.exe"), "gdeflate_exe": bundled_encoder_path("GDeflateContentTool.exe"),
        "gacl_exe": bundled_encoder_path("GACLContentTool.exe"),
    }
    values.update(load_config(config))
    if "input_directory" not in values:
        raise ValueError("input_directory is required in the JSON request")
    return argparse.Namespace(**values)
