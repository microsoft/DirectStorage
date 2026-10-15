"""Small shared helpers for Content Factory scripts; no processing on import."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


# Report whether an output directory is absent or contains no entries.
def directory_is_empty(path: Path) -> bool:
    return not path.exists() or (path.is_dir() and not any(path.iterdir()))


# Flush bytes to a sibling temporary file before replacing the destination.
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


# Reject linked path components and paths resolving outside the content root.
def reject_symlink_components(root: Path, relative: str, field: str) -> Path:
    """Reject linked components and resolved escapes before reading or writing content files."""
    root = root.resolve()
    path = root / Path(relative)
    current = root
    for part in Path(relative).parts:
        current /= part
        # Junctions are also redirects on Windows; is_junction is unavailable on older Python.
        junction = getattr(current, "is_junction", lambda: False)()
        if current.is_symlink() or junction:
            raise ValueError(f"{field} cannot contain symbolic links or junctions: {relative}")
    try:
        path.resolve().relative_to(root)
    except ValueError as error:
        raise ValueError(f"{field} escapes the Content Factory root: {relative}") from error
    return path


# Find a packaged encoder relative to the application, never through the caller's PATH.
def bundled_tool(filename: str) -> Path:
    application = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent.parent
    return application / "bin" / filename


# Validate a regular executable path without launching the tool.
def validate_executable(executable: Path) -> Path:
    """Check the supplied file path without executing it."""
    # Reject links and missing files here; do not run the tool during this check.
    if executable.is_symlink():
        raise ValueError(f"tool cannot be a symbolic link: {executable}")
    resolved = executable.resolve()
    if not resolved.is_file():
        raise FileNotFoundError(f"tool executable does not exist: {resolved}")
    return resolved


# Format a bounded subprocess-output excerpt for an error message.
def _diagnostic_output(value: str | bytes | None) -> str:
    # A failed decoder may have emitted a large binary payload. Bound the error message.
    if value is None:
        return "<empty>"
    excerpt = value[:4096]
    text = (excerpt.decode("utf-8", "replace") if isinstance(excerpt, bytes) else excerpt).strip() or "<empty>"
    return text + ("\n<truncated>" if len(value) > 4096 else "")


# Run a tool with captured output and report execution errors, timeouts, or failure exits.
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


# Run the version command and extract its version number from the returned banner.
def tool_version(
    executable: Path, label: str, pattern: str = r"\b(\d+\.\d+\.\d+)\b", *, timeout: int = 30,
) -> str:
    # Read the version once the caller is ready to execute the tool.
    result = run_command([str(executable), "--version"], operation=f"{label} --version", text=True, timeout=timeout)
    # Extract the version number without depending on the rest of the banner.
    banner = result.stdout or result.stderr or ""
    if isinstance(banner, bytes):
        banner = banner.decode("utf-8", "replace")
    match = re.search(pattern, banner.strip())
    if not match:
        raise RuntimeError(f"{label} --version returned no recognizable version")
    return match.group(1)
