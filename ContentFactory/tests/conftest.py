"""Import production helpers once, retaining shared monkeypatch targets across tests."""

import importlib
import importlib.util
import hashlib
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
# Regular imports must resolve to the same tool modules used by production.
sys.path.insert(0, str(TOOLS))


# Import underscore-named helpers normally; only the dashed factory CLI needs a file spec.
def load_tool_module(name: str, filename: str | Path):
    path = TOOLS / filename
    if path.stem.isidentifier():
        return importlib.import_module(path.stem)
    assert path.name == "run-content-factory.py"
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification and specification.loader
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


# Create a command wrapper that runs a checked-in Python codec stand-in.
def make_python_tool(directory: Path, filename: str, name: str) -> Path:
    directory = directory / "generated" / "test-tools"
    directory.mkdir(parents=True, exist_ok=True)
    script = Path(__file__).parent / filename
    wrapper = directory / (name + (".cmd" if sys.platform == "win32" else ""))
    command = f'"{sys.executable}" "{script}"'
    if sys.platform == "win32":
        wrapper.write_text(f'@echo off\n{command} %*\n', encoding="utf-8")
    else:
        wrapper.write_text(f'#!/bin/sh\nexec {command} "$@"\n', encoding="utf-8")
        wrapper.chmod(0o755)
    return wrapper


# Populate originals only; standalone generation must not need an initial report or output directory.
def create_content_set(directory: Path, files: dict[str, bytes]):
    directory.mkdir(parents=True, exist_ok=True)
    for name, contents in files.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    return files


# Independently compare current generated files and originals with the report's recorded bytes and hashes.
def assert_report_files(input_directory: Path, report: dict) -> None:
    assert set(report) == {"sources", "derivatives", "archives", "tools"}
    for group in ("sources", "derivatives", "archives"):
        for record in report[group]:
            path = input_directory / record["path"]
            assert path.is_file(), record["path"]
            data = path.read_bytes()
            assert record["size"] == len(data), record["path"]
            assert record["sha256"] == hashlib.sha256(data).hexdigest(), record["path"]
