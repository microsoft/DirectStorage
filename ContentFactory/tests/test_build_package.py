"""Exercise package planning and assembly using fake outputs; never run CMake or native tools."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parent / "build-package.py"
if not SCRIPT.is_file():
    SCRIPT = Path(__file__).resolve().parents[1] / "build-package.py"
SPEC = importlib.util.spec_from_file_location("content_factory_package_builder", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
builder = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = builder
SPEC.loader.exec_module(builder)


class PackageTests(unittest.TestCase):
    """Use isolated local files and mocked subprocess calls for every build/package scenario."""

    # Keep fixtures local to the owned test directory, not in an unrelated system temporary tree.
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        runtime_licenses = mock.patch.object(builder, "runtime_license_sources", return_value={})
        runtime_licenses.start()
        self.addCleanup(runtime_licenses.stop)
        self.root = Path(self.temporary.name).resolve()
        self.repository = self.root / "repository"
        self.source = self.repository / "ContentFactory"
        self.source.mkdir(parents=True)
        self.gacl = self.root / "gacl"
        self.gacl.mkdir()
        self.plan = builder.BuildPlan(self.source, self.gacl, self.source / "build",
                                      self.root / "package", "Release", "cmake.exe")

    # Fake only the known native output filenames; no executable is launched.
    def create_native_outputs(self) -> None:
        self.plan.native_bin.mkdir(parents=True, exist_ok=True)
        for name in builder.NATIVE_TOOLS:
            (self.plan.native_bin / name).write_bytes(b"fake native executable")

    # Supply dummy source/license contents rather than relying on a dependency installation.
    def create_inputs(self) -> dict[str, Path]:
        (self.source / "tools").mkdir()
        (self.source / "tools" / "run-content-factory.py").write_text("# mock launcher\n")
        (self.source / "CMakeLists.txt").write_text("# mock CMake input\n")
        (self.source / "README.md").write_text("mock usage\n")
        (self.source / "examples").mkdir()
        (self.source / "examples" / "request.json").write_text('{"input_directory":"../assets"}')
        result = {}
        for name in ("DirectStorage-LICENSE.txt", "GACL-LICENSE.txt", "zstd-LICENSE.txt", "GDeflate-LICENSE.txt"):
            path = self.root / name
            path.write_text(f"mock license {name}\n")
            result[name] = path
        return result

    # Materialize the minimum onedir contract when the mocked Python packaging step is called.
    def fake_pyinstaller(self, command: list[str], cwd: Path, check: bool = True) -> None:
        self.assertEqual(cwd, self.source)
        self.assertEqual(command[:3], [sys.executable, "-m", "PyInstaller"])
        dist = Path(command[command.index("--distpath") + 1])
        package = dist / "ContentFactory"
        (package / "_internal").mkdir(parents=True)
        (package / "ContentFactory.exe").write_bytes(b"fake launcher")
        (package / "_internal" / "python313.dll").write_bytes(b"fake Python runtime")

    # Verify arguments while replacing the entire subprocess boundary with a mock.
    def test_native_commands_are_fixed_local_x64_targets(self) -> None:
        self.create_native_outputs()
        with mock.patch.object(builder.subprocess, "run") as run:
            builder.build_native(self.plan)
        commands = [call.args[0] for call in run.call_args_list]
        self.assertEqual(len(commands), 2)
        self.assertIn("Visual Studio 17 2022", commands[0])
        self.assertEqual(commands[0][commands[0].index("-A") + 1], "x64")
        self.assertIn(f"-DGACL_ROOT:PATH={self.gacl}", commands[0])
        self.assertEqual(commands[1][commands[1].index("--target") + 1:-1],
                         ["zstd", "GDeflateContentTool", "GACLContentTool"])
        self.assertTrue(all(call.kwargs["check"] for call in run.call_args_list))

    # A successful subprocess status alone must not be mistaken for usable build outputs.
    def test_missing_native_outputs_fail_before_packaging(self) -> None:
        with mock.patch.object(builder.subprocess, "run"), self.assertRaisesRegex(ValueError, "expected executables"):
            builder.build_native(self.plan)

    # Retain the fake Python runtime and collect exactly the three adjacent native tools.
    def test_complete_onedir_package_is_published(self) -> None:
        licenses = self.create_inputs()
        self.create_native_outputs()
        with mock.patch.object(builder, "license_sources", return_value=licenses), \
                mock.patch.object(builder, "build_native"), \
                mock.patch.object(builder.subprocess, "run", side_effect=self.fake_pyinstaller) as run:
            result = builder.build_package(self.plan)
        self.assertEqual(result, self.plan.output_directory)
        self.assertTrue((result / "ContentFactory.exe").is_file())
        self.assertTrue((result / "_internal" / "python313.dll").is_file())
        self.assertEqual({path.name for path in (result / "bin").iterdir()}, set(builder.NATIVE_TOOLS))
        self.assertEqual({path.name for path in (result / "licenses").iterdir()}, set(licenses))
        command = run.call_args.args[0]
        self.assertIn("--onedir", command)
        self.assertEqual(command[command.index("--name") + 1], "ContentFactory")
        self.assertEqual(command[-1], str(self.source / "tools" / "run-content-factory.py"))
        self.assertFalse(list(self.root.glob(".ContentFactory-package-*")))

    # Preserve user-owned contents and abort before any build boundary is entered.
    def test_existing_output_is_never_modified(self) -> None:
        self.plan.output_directory.mkdir()
        marker = self.plan.output_directory / "keep.txt"
        marker.write_text("user data")
        with mock.patch.object(builder, "build_native") as native, self.assertRaisesRegex(ValueError, "already exists"):
            builder.build_package(self.plan)
        native.assert_not_called()
        self.assertEqual(marker.read_text(), "user data")

    # Cover either nesting direction so a package cannot become an accidental source inventory.
    def test_output_source_and_build_overlaps_are_rejected(self) -> None:
        invalid = (self.source / "package", self.repository, self.root,
                   self.gacl / "package", self.plan.build_directory / "package")
        for output in invalid:
            with self.subTest(output=output), self.assertRaises(ValueError):
                builder.validate_paths(builder.BuildPlan(self.source, self.gacl, self.plan.build_directory,
                                                         output, "Release", "cmake.exe"))

    # Only the dedicated ContentFactory/build subtree is allowed within the public checkout.
    def test_build_cannot_contain_or_overwrite_sources(self) -> None:
        invalid = (self.root, self.repository, self.source, self.source / "tools", self.gacl / "build")
        for build in invalid:
            with self.subTest(build=build), self.assertRaises(ValueError):
                builder.validate_paths(builder.BuildPlan(self.source, self.gacl, build,
                                                         self.plan.output_directory, "Release", "cmake.exe"))

    # A failed freezer must leave the public output absent and remove its temporary stage.
    def test_packaging_failure_cleans_only_private_stage(self) -> None:
        licenses = self.create_inputs()
        self.create_native_outputs()
        with mock.patch.object(builder, "license_sources", return_value=licenses), \
                mock.patch.object(builder, "build_native"), \
                mock.patch.object(builder.subprocess, "run", side_effect=subprocess.CalledProcessError(2, ["mock"])):
            with self.assertRaises(subprocess.CalledProcessError):
                builder.build_package(self.plan)
        self.assertFalse(self.plan.output_directory.exists())
        self.assertFalse(list(self.root.glob(".ContentFactory-package-*")))

    # Refuse publication if another actor claims the output path while PyInstaller runs.
    def test_output_created_during_build_is_preserved(self) -> None:
        licenses = self.create_inputs()
        self.create_native_outputs()
        # Simulate a destination race without launching an external program.
        def freezer(command: list[str], cwd: Path, check: bool = True) -> None:
            self.fake_pyinstaller(command, cwd)
            self.plan.output_directory.mkdir()
            (self.plan.output_directory / "keep.txt").write_text("other actor")
        with mock.patch.object(builder, "license_sources", return_value=licenses), \
                mock.patch.object(builder, "build_native"), \
                mock.patch.object(builder.subprocess, "run", side_effect=freezer):
            with self.assertRaisesRegex(ValueError, "appeared during build"):
                builder.build_package(self.plan)
        self.assertEqual((self.plan.output_directory / "keep.txt").read_text(), "other actor")
        self.assertFalse(list(self.root.glob(".ContentFactory-package-*")))

    # Verify that a lone launcher executable cannot masquerade as a self-contained package.
    def test_pyinstaller_runtime_is_required(self) -> None:
        destination = self.root / "dist"
        package = destination / "ContentFactory"
        package.mkdir(parents=True)
        (package / "ContentFactory.exe").write_bytes(b"launcher")
        with mock.patch.object(builder.subprocess, "run"), self.assertRaisesRegex(ValueError, "runtime directory"):
            builder.freeze_launcher(self.plan, destination)
        (package / "_internal").mkdir()
        with mock.patch.object(builder.subprocess, "run"), self.assertRaisesRegex(ValueError, "runtime DLL"):
            builder.freeze_launcher(self.plan, destination)

    # Distribution must not silently omit required upstream notices.
    def test_missing_license_fails_before_native_build(self) -> None:
        licenses = self.create_inputs()
        next(iter(licenses.values())).unlink()
        with mock.patch.object(builder, "license_sources", return_value=licenses), \
                mock.patch.object(builder, "build_native") as native, self.assertRaisesRegex(ValueError, "license files"):
            builder.build_package(self.plan)
        native.assert_not_called()

    # Mock platform and tool discovery, not CMake configuration or native execution.
    def test_tool_availability_is_checked_without_execution(self) -> None:
        args = builder.create_parser().parse_args(["--gacl-root", str(self.gacl),
                                                  "--build-directory", str(self.plan.build_directory),
                                                  "--output-directory", str(self.plan.output_directory)])
        with mock.patch.object(builder.sys, "platform", "win32"), \
                mock.patch.object(builder.platform, "machine", return_value="AMD64"), \
                mock.patch.object(builder.shutil, "which", return_value=None), \
                mock.patch.object(builder.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "CMake executable"):
                builder.make_plan(args)
        run.assert_not_called()
        with mock.patch.object(builder.sys, "platform", "win32"), \
                mock.patch.object(builder.platform, "machine", return_value="AMD64"), \
                mock.patch.object(builder.shutil, "which", return_value="cmake.exe"), \
                mock.patch.object(builder.importlib.util, "find_spec", return_value=None):
            with self.assertRaisesRegex(ValueError, "PyInstaller is unavailable"):
                builder.make_plan(args)

    # Keep a Linux/macOS caller from configuring an unsupported native build accidentally.
    def test_non_windows_is_rejected_without_execution(self) -> None:
        args = builder.create_parser().parse_args(["--gacl-root", str(self.gacl)])
        with mock.patch.object(builder.sys, "platform", "linux"), \
                mock.patch.object(builder.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "Windows x64"):
                builder.make_plan(args)
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
