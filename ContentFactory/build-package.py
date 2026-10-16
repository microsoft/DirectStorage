#!/usr/bin/env python3
"""Build the Windows x64 native tools and assemble a portable ContentFactory folder."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import importlib.util
import importlib.metadata
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile


NATIVE_TOOLS = ("zstd.exe", "GDeflateContentTool.exe", "GACLContentTool.exe")
CONFIGURATIONS = ("Release", "RelWithDebInfo", "MinSizeRel", "Debug")


@dataclass(frozen=True)
class BuildPlan:
    """Keep all build inputs explicit so validation and execution can be tested separately."""

    source: Path
    gacl_root: Path
    build_directory: Path
    output_directory: Path
    configuration: str
    cmake: str

    # Keep CMake artifacts separate from PyInstaller's work and specification files.
    @property
    def native_directory(self) -> Path:
        return self.build_directory / "native"

    # Match the configuration-specific runtime output directory set by the CMake project.
    @property
    def native_bin(self) -> Path:
        return self.native_directory / "bin" / self.configuration


# Expose only build inputs; the generator, architecture, and packaging mode are fixed.
def create_parser() -> argparse.ArgumentParser:
    source = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--gacl-root", type=Path, default=os.environ.get("GACL_ROOT"),
                        help="Existing GACL checkout (or set GACL_ROOT); nothing is downloaded.")
    parser.add_argument("--build-directory", type=Path, default=source / "build",
                        help="Reusable build artifacts directory (default: ContentFactory/build).")
    parser.add_argument("--output-directory", type=Path,
                        default=source.parent.parent / "ContentFactory-package",
                        help="New portable package directory; must not already exist.")
    parser.add_argument("--configuration", choices=CONFIGURATIONS, default="Release")
    parser.add_argument("--cmake", default="cmake", help="CMake command or explicit executable path.")
    return parser


# Detect either direction of nesting after callers have resolved symlinks and relative paths.
def overlaps(first: Path, second: Path) -> bool:
    return first == second or first in second.parents or second in first.parents


# Reject unsafe destinations before any subprocess runs or build directory is created.
def validate_paths(plan: BuildPlan) -> None:
    repository = plan.source.parent
    for checkout in (repository, plan.gacl_root):
        if overlaps(plan.output_directory, checkout):
            raise ValueError(f"Output directory must not overlap source checkout: {checkout}")
        if plan.build_directory == checkout or plan.build_directory in checkout.parents:
            raise ValueError(f"Build directory must not contain source checkout: {checkout}")
    if overlaps(plan.build_directory, plan.output_directory):
        raise ValueError("Build and output directories must not overlap.")
    if plan.gacl_root in plan.build_directory.parents:
        raise ValueError("Build directory must not be inside the GACL checkout.")
    if repository in plan.build_directory.parents:
        allowed = plan.source / "build"
        if plan.build_directory != allowed and allowed not in plan.build_directory.parents:
            raise ValueError("An in-repository build directory must be under ContentFactory/build.")
    if plan.output_directory.exists() or plan.output_directory.is_symlink():
        raise ValueError(f"Output directory already exists; it will not be deleted: {plan.output_directory}")
    if plan.build_directory.exists() and not plan.build_directory.is_dir():
        raise ValueError(f"Build directory is not a directory: {plan.build_directory}")


# Resolve user inputs once and ensure the build tools are available without invoking them.
def make_plan(args: argparse.Namespace) -> BuildPlan:
    if sys.platform != "win32" or platform.machine().lower() not in ("amd64", "x86_64") or struct.calcsize("P") != 8:
        raise ValueError("Packaging requires Windows x64 and a 64-bit Python interpreter.")
    if args.gacl_root is None:
        raise ValueError("Supply --gacl-root or set GACL_ROOT to an existing GACL checkout.")
    cmake = shutil.which(args.cmake)
    if cmake is None:
        raise ValueError(f"CMake executable is unavailable: {args.cmake}")
    if importlib.util.find_spec("PyInstaller") is None:
        raise ValueError("PyInstaller is unavailable; install requirements-build.txt in this Python environment before building.")
    output = Path(args.output_directory).absolute()
    if output.is_symlink():
        raise ValueError(f"Output directory is a symlink; refusing to replace it: {output}")
    plan = BuildPlan(Path(__file__).resolve().parent, Path(args.gacl_root).resolve(),
                     Path(args.build_directory).resolve(), output.resolve(), args.configuration, cmake)
    validate_paths(plan)
    return plan


# Preserve the exact checked-in upstream notices under distinct distribution filenames.
def license_sources(plan: BuildPlan) -> dict[str, Path]:
    repository = plan.source.parent
    zstd = plan.gacl_root / "ThirdParty" / "zstd"
    return {
        "DirectStorage-LICENSE.txt": repository / "LICENSE",
        "DirectStorage-NOTICES.txt": repository / "NOTICES.txt",
        "GACL-LICENSE.txt": plan.gacl_root / "LICENSE",
        "GACL-NOTICE.txt": plan.gacl_root / "NOTICE.txt",
        "zstd-LICENSE.txt": zstd / "LICENSE",
        "zstd-COPYING.txt": zstd / "COPYING",
        "GDeflate-LICENSE.txt": repository / "GDeflate" / "GDeflate" / "LICENSE",
        "libdeflate-COPYING.txt": repository / "GDeflate" / "3rdparty" / "libdeflate" / "COPYING",
    }


# Include the interpreter and freezer notices from the actual packaging environment.
def runtime_license_sources() -> dict[str, Path]:
    result = {"Python-LICENSE.txt": Path(sys.base_prefix) / "LICENSE.txt"}
    distribution = importlib.metadata.distribution("PyInstaller")
    for relative in distribution.files or []:
        if relative.name.upper().startswith(("COPYING", "LICENSE")):
            result[f"PyInstaller-{relative.name}"] = Path(distribution.locate_file(relative))
    if len(result) == 1:
        raise ValueError("PyInstaller distribution license files are missing.")
    return result


# Execute an argument list without a shell and stop immediately on a failed build step.
def run_command(command: list[str], cwd: Path) -> None:
    print("Running:", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


# Configure the local dependency checkout and build exactly the three distributed executables.
def build_native(plan: BuildPlan) -> None:
    run_command([plan.cmake, "-S", str(plan.source), "-B", str(plan.native_directory),
                 "-G", "Visual Studio 17 2022", "-A", "x64",
                 f"-DGACL_ROOT:PATH={plan.gacl_root}"], plan.source)
    run_command([plan.cmake, "--build", str(plan.native_directory), "--config", plan.configuration,
                 "--target", "zstd", "GDeflateContentTool", "GACLContentTool", "--parallel"], plan.source)
    missing = [str(plan.native_bin / name) for name in NATIVE_TOOLS if not (plan.native_bin / name).is_file()]
    if missing:
        raise ValueError("Native build did not produce expected executables: " + ", ".join(missing))


# Use this interpreter's PyInstaller so its runtime and modules travel with the launcher.
def freeze_launcher(plan: BuildPlan, destination: Path) -> Path:
    run_command([sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir",
                 "--name", "ContentFactory", "--distpath", str(destination),
                 "--workpath", str(plan.build_directory / "pyinstaller-work"),
                 "--specpath", str(plan.build_directory / "pyinstaller-spec"),
                 "--paths", str(plan.source / "tools"),
                 str(plan.source / "tools" / "run-content-factory.py")], plan.source)
    package = destination / "ContentFactory"
    if not (package / "ContentFactory.exe").is_file():
        raise ValueError("PyInstaller did not produce ContentFactory/ContentFactory.exe.")
    if not (package / "_internal").is_dir():
        raise ValueError("PyInstaller runtime directory is missing; refusing an incomplete package.")
    if not any((package / "_internal").glob("python*.dll")):
        raise ValueError("PyInstaller's bundled Python runtime DLL is missing.")
    return package


# Assemble in a private sibling folder and publish only a complete, previously absent destination.
def build_package(plan: BuildPlan) -> Path:
    validate_paths(plan)
    licenses = {**license_sources(plan), **runtime_license_sources()}
    required_sources = [plan.source / "CMakeLists.txt", plan.source / "tools" / "run-content-factory.py",
                        *licenses.values()]
    required_sources.extend([plan.source / "README.md", plan.source / "examples" / "request.json"])
    missing = [str(path) for path in required_sources if not path.is_file()]
    if missing:
        raise ValueError("Required build inputs or license files are missing: " + ", ".join(missing))
    build_native(plan)
    plan.output_directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".ContentFactory-package-", dir=plan.output_directory.parent) as temporary:
        package = freeze_launcher(plan, Path(temporary) / "dist")
        (package / "bin").mkdir()
        for name in NATIVE_TOOLS:
            shutil.copy2(plan.native_bin / name, package / "bin" / name)
        (package / "licenses").mkdir()
        for name, source in licenses.items():
            shutil.copy2(source, package / "licenses" / name)
        shutil.copy2(plan.source / "README.md", package / "README.md")
        shutil.copytree(plan.source / "examples", package / "examples")
        # Recheck immediately before Windows rename, which refuses an already-existing destination.
        if plan.output_directory.exists() or plan.output_directory.is_symlink():
            raise ValueError(f"Output directory appeared during build; it was not modified: {plan.output_directory}")
        package.rename(plan.output_directory)
    return plan.output_directory


# Present actionable validation and subprocess failures without claiming a partial package succeeded.
def main(argv: list[str] | None = None) -> int:
    parser = create_parser()
    args = parser.parse_args(argv)
    try:
        destination = build_package(make_plan(args))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        return 1
    print(f"Portable package created: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
