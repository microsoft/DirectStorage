"""Small shared helpers for Content Factory scripts; no processing on import."""

from __future__ import annotations

import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator


def load_module(name: str, filename: str):
    # Load a sibling script even if its filename contains a dash.
    path = Path(__file__).with_name(filename)
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    # Register the module so classes defined inside it can find their module name.
    previous = sys.modules.get(name)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    # Restore the previous registration if loading fails.
    except Exception:
        if previous is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = previous
        raise
    return module


def directory_is_empty(path: Path) -> bool:
    return not path.exists() or (path.is_dir() and not any(path.iterdir()))


def write_bytes_atomic(path: Path, data: bytes) -> None:
    # Write beside the destination so the final rename stays on the same filesystem.
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # Replace the old file only after the new bytes have been flushed.
        os.replace(temporary, path)
    finally:
        # Remove an unfinished temporary file if a write or rename fails.
        temporary.unlink(missing_ok=True)


@contextmanager
def set_lock(root: Path, set_name: str, stage: str, label: str) -> Iterator[None]:
    # Keep the existing stage-specific names. This is not a cross-stage mutex.
    lock = root / "manifests" / f".{set_name}.{stage}.lock"
    try:
        # Create the lock exclusively; an existing lock means this stage is already active.
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError as error:
        raise RuntimeError(f"another {label} operation is active for set '{set_name}'") from error
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(str(os.getpid()))
        # Keep the lock until the caller finishes, including when it raises an error.
        yield
    finally:
        lock.unlink(missing_ok=True)


def validate_executable(executable: Path) -> Path:
    """Check the supplied file path without executing it."""
    # Reject links and missing files here; do not run the tool during this check.
    if executable.is_symlink():
        raise ValueError(f"tool cannot be a symbolic link: {executable}")
    resolved = executable.resolve()
    if not resolved.is_file():
        raise FileNotFoundError(f"tool executable does not exist: {resolved}")
    return resolved


def _output_text(value: str | bytes | None) -> str:
    return (value.decode("utf-8", "replace") if isinstance(value, bytes) else value or "").strip()


def _diagnostic_output(value: str | bytes | None) -> str:
    # A failed decoder may have emitted a large binary payload. Bound the error message.
    if value is None:
        return "<empty>"
    text = _output_text(value[:4096]) or "<empty>"
    return text + ("\n<truncated>" if len(value) > 4096 else "")


def run_command(
    arguments: list[str],
    *,
    operation: str,
    input_data: bytes | None = None,
    text: bool = False,
    timeout: int = 300,
) -> subprocess.CompletedProcess:
    """Run a tool and report failures, including silent native crashes."""
    # Catch invalid calls before starting a process.
    if not arguments:
        raise ValueError("tool command cannot be empty")
    if text and input_data is not None:
        raise ValueError("binary input requires binary output mode")
    try:
        # Keep compressed output as bytes unless this caller explicitly requests text.
        result = subprocess.run(
            arguments, input=input_data, capture_output=True, text=text,
            timeout=timeout, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"{operation} timed out after {timeout} seconds; executable: {arguments[0]}"
        ) from error
    except OSError as error:
        raise RuntimeError(f"could not execute {arguments[0]} for {operation}: {error}") from error
    # A crashed native tool may return no text, so always include its exit code.
    if result.returncode != 0:
        stderr = _diagnostic_output(result.stderr)
        stdout = _diagnostic_output(result.stdout)
        raise RuntimeError(
            f"{operation} failed\nExecutable: {arguments[0]}\n"
            f"Exit code: {result.returncode} (0x{result.returncode & 0xFFFFFFFF:08X})\n"
            f"stderr: {stderr}\nstdout: {stdout}"
        )
    return result


def tool_version(
    executable: Path, label: str, pattern: str = r"\b(\d+\.\d+\.\d+)\b", *, timeout: int = 30,
) -> str:
    # Read the version once the caller is ready to execute the tool.
    result = run_command([str(executable), "--version"], operation=f"{label} --version", text=True, timeout=timeout)
    # Extract the version number without depending on the rest of the banner.
    match = re.search(pattern, _output_text(result.stdout or result.stderr))
    if not match:
        raise RuntimeError(f"{label} --version returned no recognizable version")
    return match.group(1)
