"""Native GDeflate and GACL executable tests; each class uses its own tool setting."""

from pathlib import Path
import hashlib
import struct
import os
import subprocess
import tempfile
import unittest


# These tests require the real GDeflate executable, not a codec stand-in.
class GDeflateContentToolTests(unittest.TestCase):
    # Require and locate the real GDeflate executable configured for native tests.
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("GDEFLATE_CONTENT_TOOL")
        if not value:
            raise RuntimeError("Set GDEFLATE_CONTENT_TOOL to the built executable")
        cls.tool = Path(value).resolve()
        if not cls.tool.is_file():
            raise FileNotFoundError(cls.tool)

    # Create a temporary workspace for native GDeflate inputs and outputs.
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    # Run the configured GDeflate executable and capture its output and exit status.
    def run_tool(self, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.tool), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

    # Check single-file generation, default header values, overwrite policy, and no sidecars.
    def test_single_file_is_deterministic_without_sidecars(self):
        source = self.root / "input.bin"
        output = self.root / "output.gdeflate"
        data = b"abc123" * 1_000
        source.write_bytes(data)
        command = ("--input", source, "--output", output)
        first = self.run_tool(*command)
        self.assertEqual(first.returncode, 0, first.stderr)
        stored = output.read_bytes()
        self.assertEqual(
            struct.unpack("<IHHIIQQ", stored[:32]),
            (0x31464447, 1, 32, 9, 0, len(data), len(stored) - 32),
        )
        self.assertGreater(len(stored), 32)
        refused = self.run_tool(*command)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(output.read_bytes(), stored)
        second = self.run_tool(*command, "--overwrite")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(output.read_bytes(), stored)
        self.assertEqual(source.read_bytes(), data)
        self.assertEqual({path.name for path in self.root.iterdir()}, {source.name, output.name})

    # Check the stored header at every level; successful generation includes both internal round trips.
    def test_all_supported_levels_round_trip(self):
        source = self.root / "input.bin"
        data = (b"directstorage-gdeflate-" * 1_000) + bytes(range(256))
        source.write_bytes(data)
        for level in range(1, 13):
            output = self.root / f"level-{level}.gdeflate"
            generated = self.run_tool("--input", source, "--output", output, "--level", level)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            stored = output.read_bytes()
            self.assertEqual(
                struct.unpack("<IHHIIQQ", stored[:32]),
                (0x31464447, 1, 32, level, 0, len(data), len(stored) - 32),
            )

    # Check rejected levels without creating any output file.
    def test_invalid_levels_are_rejected_before_output(self):
        source = self.root / "input.bin"
        source.write_bytes(b"small")
        output = self.root / "output.gdeflate"
        for level in ("0", "13", "", "9abc", "1.5", "-1", "+9", " 9", "9 ", "4294967297", "9" * 100):
            result = self.run_tool("--input", source, "--output", output, "--level", level)
            self.assertNotEqual(result.returncode, 0, level)
            self.assertFalse(output.exists(), level)

    # Check missing values, unknown options, and unsupported directory/verification CLI forms.
    def test_malformed_cli_is_rejected(self):
        source = self.root / "input.bin"
        source.write_bytes(b"small")
        output = self.root / "output.gdeflate"
        cases = (
            (), ("--input",), ("--output",), ("--level",),
            ("--input", source), ("--output", output),
            ("--input", source, "--output", output, "--level"),
            ("--input", source, "--output", output, "--unknown"),
            ("--input", source, "--output", output, "--recursive"),
            ("--verify", source),
        )
        for args in cases:
            result = self.run_tool(*args)
            self.assertNotEqual(result.returncode, 0, args)
            self.assertFalse(output.exists(), args)

    # Check directory paths cannot be used as a worker input or output.
    def test_directories_are_rejected(self):
        source = self.root / "input.bin"
        source.write_bytes(b"small")
        directory = self.root / "directory"
        directory.mkdir()
        output = self.root / "output.gdeflate"
        input_directory = self.run_tool("--input", directory, "--output", output)
        self.assertNotEqual(input_directory.returncode, 0)
        self.assertFalse(output.exists())
        output_directory = self.run_tool("--input", source, "--output", directory, "--overwrite")
        self.assertNotEqual(output_directory.returncode, 0)
        self.assertEqual(list(directory.iterdir()), [])

    # Check neither the original path nor a hardlink can be overwritten, even when requested.
    def test_input_and_hardlink_outputs_are_rejected(self):
        source = self.root / "input.bin"
        data = b"original content" * 100
        source.write_bytes(data)
        alias = self.root / "hardlink.bin"
        os.link(source, alias)
        for output in (source, alias):
            result = self.run_tool("--input", source, "--output", output, "--overwrite")
            self.assertNotEqual(result.returncode, 0, output)
            self.assertEqual(source.read_bytes(), data)
            self.assertEqual(alias.read_bytes(), data)

    # Check a pre-existing staging hardlink is not truncated while writing a new output.
    def test_existing_temporary_file_does_not_overwrite_original(self):
        source = self.root / "input.bin"
        data = b"original content" * 100
        source.write_bytes(data)
        output = self.root / "output.gdeflate"
        temporary = self.root / "output.gdeflate.syncing"
        os.link(source, temporary)
        result = self.run_tool("--input", source, "--output", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertEqual(source.read_bytes(), data)
        self.assertEqual(temporary.read_bytes(), data)

    # Check a single-file call leaves unrelated metadata untouched.
    def test_single_file_keeps_existing_sidecar(self):
        sidecar = self.root / "variants.json"
        sidecar.write_bytes(b"existing metadata")
        source = self.root / "input.bin"
        source.write_bytes(b"small" * 100)
        output = self.root / "output.gdeflate"
        result = self.run_tool("--input", source, "--output", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sidecar.read_bytes(), b"existing metadata")

    # Check the worker version remains the stable codec identity.
    def test_version(self):
        result = self.run_tool("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "GDeflateContentTool 1.0.0")

    # Check that native GDeflate compression rejects an empty input file.
    def test_empty_input_is_rejected(self):
        source = self.root / "empty.bin"
        source.write_bytes(b"")
        result = self.run_tool("--input", source, "--output", self.root / "empty.gdeflate")
        self.assertNotEqual(result.returncode, 0)


# These tests use the GACL executable; absent GACL configuration retains the original skip.
class GACLContentToolTests(unittest.TestCase):
    # Locate the real GACL executable or skip the class when none is configured.
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("GACL_CONTENT_TOOL")
        if not value:
            raise unittest.SkipTest("Set GACL_CONTENT_TOOL to the built executable")
        cls.tool = Path(value).resolve()
        if not cls.tool.is_file():
            raise FileNotFoundError(cls.tool)

    # Create a temporary workspace for native GACL inputs and outputs.
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    # Run the configured GACL executable and capture its output and exit status.
    def run_tool(self, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.tool), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

    # Check transform IDs, deterministic output, and verification for all five tested BC formats.
    def test_all_phase2_formats_are_deterministic_and_verifiable(self):
        cases = {"BC1": (8, 1), "BC3": (16, 2), "BC4": (8, 3), "BC5": (16, 4), "BC7": (16, 7)}
        for format_name, (bytes_per_block, transform_id) in cases.items():
            source = self.root / f"{format_name}.bin"
            first = self.root / f"{format_name}-first.gacl"
            second = self.root / f"{format_name}-second.gacl"
            source.write_bytes(bytes((index * 37 + 11) & 0xFF for index in range(bytes_per_block * 17)))
            arguments = (
                "--input", source, "--format", format_name,
                "--zstd-level", 19, "--target-block-size", 65536,
            )
            generated = self.run_tool(*arguments, "--output", first)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            self.assertIn(f"transform_id={transform_id}", generated.stdout)
            regenerated = self.run_tool(*arguments, "--output", second)
            self.assertEqual(regenerated.returncode, 0, regenerated.stderr)
            self.assertEqual(hashlib.sha256(first.read_bytes()).digest(), hashlib.sha256(second.read_bytes()).digest())
            verified = self.run_tool("--verify", "--input", first, "--original", source, "--format", format_name)
            self.assertEqual(verified.returncode, 0, verified.stderr)

    # Check that native BC7 verification rejects a corrupted GACL stream.
    def test_corruption_is_rejected_for_bc7(self):
        source = self.root / "BC7.bin"
        output = self.root / "BC7.gacl"
        source.write_bytes(bytes(range(256)))
        generated = self.run_tool(
            "--input", source, "--output", output, "--format", "BC7",
            "--zstd-level", 19, "--target-block-size", 65536,
        )
        self.assertEqual(generated.returncode, 0, generated.stderr)
        damaged = bytearray(output.read_bytes())
        damaged[-1] ^= 0xFF
        output.write_bytes(damaged)
        verified = self.run_tool("--verify", "--input", output, "--original", source, "--format", "BC7")
        self.assertNotEqual(verified.returncode, 0)


if __name__ == "__main__":
    unittest.main()
