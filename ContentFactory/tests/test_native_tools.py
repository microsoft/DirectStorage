"""Native GDeflate and GACL executable tests; each class uses its own tool setting."""

from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import unittest


# These tests require the real GDeflate executable, not a codec stand-in.
class GDeflateContentToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("GDEFLATE_CONTENT_TOOL")
        if not value:
            raise RuntimeError("Set GDEFLATE_CONTENT_TOOL to the built executable")
        cls.tool = Path(value).resolve()
        if not cls.tool.is_file():
            raise FileNotFoundError(cls.tool)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.tool), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_batch_generation_is_deterministic_and_verifiable(self):
        source = self.root / "source"
        output = self.root / "output"
        (source / "nested").mkdir(parents=True)
        (source / "a.bin").write_bytes(b"abc123" * 10_000)
        (source / "nested" / "b.bin").write_bytes(bytes(range(256)) * 400)

        command = ("--input", source, "--output", output, "--recursive", "--level", 9)
        first = self.run_tool(*command)
        self.assertEqual(first.returncode, 0, first.stderr)

        manifest = json.loads((output / "variants.json").read_text(encoding="utf-8"))
        self.assertEqual({item["source"] for item in manifest["variants"]}, {"a.bin", "nested/b.bin"})
        for item in manifest["variants"]:
            source_path = source / item["source"]
            output_path = output / item["output"]
            self.assertEqual(item["source_sha256"], hashlib.sha256(source_path.read_bytes()).hexdigest())
            self.assertEqual(item["payload_sha256"], hashlib.sha256(output_path.read_bytes()[32:]).hexdigest())

        hashes_before = {
            item["output"]: hashlib.sha256((output / item["output"]).read_bytes()).hexdigest()
            for item in manifest["variants"]
        }
        verify = self.run_tool("--verify", output, "--recursive")
        self.assertEqual(verify.returncode, 0, verify.stderr)

        second = self.run_tool(*command, "--overwrite")
        self.assertEqual(second.returncode, 0, second.stderr)
        hashes_after = {
            item["output"]: hashlib.sha256((output / item["output"]).read_bytes()).hexdigest()
            for item in manifest["variants"]
        }
        self.assertEqual(hashes_before, hashes_after)

    def test_all_supported_levels_round_trip(self):
        source = self.root / "input.bin"
        source.write_bytes((b"directstorage-gdeflate-" * 10_000) + bytes(range(256)))
        for level in range(1, 13):
            output = self.root / f"level-{level}.gdeflate"
            generated = self.run_tool("--input", source, "--output", output, "--level", level)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            verified = self.run_tool("--verify", output)
            self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_truncated_file_is_rejected(self):
        source = self.root / "input.bin"
        output = self.root / "output.gdeflate"
        source.write_bytes(b"content" * 1_000)
        self.assertEqual(self.run_tool("--input", source, "--output", output).returncode, 0)
        output.write_bytes(output.read_bytes()[:-1])
        result = self.run_tool("--verify", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("size does not match the header", result.stderr)

    def test_out_of_range_levels_are_rejected(self):
        source = self.root / "input.bin"
        source.write_bytes(b"content")
        for level in (0, 13):
            result = self.run_tool("--input", source, "--output", self.root / f"bad-{level}.gdeflate", "--level", level)
            self.assertNotEqual(result.returncode, 0)

    def test_empty_input_is_rejected(self):
        source = self.root / "empty.bin"
        source.write_bytes(b"")
        result = self.run_tool("--input", source, "--output", self.root / "empty.gdeflate")
        self.assertNotEqual(result.returncode, 0)


# These tests use the GACL executable; absent GACL configuration retains the original skip.
class GACLContentToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        value = os.environ.get("GACL_CONTENT_TOOL")
        if not value:
            raise unittest.SkipTest("Set GACL_CONTENT_TOOL to the built executable")
        cls.tool = Path(value).resolve()
        if not cls.tool.is_file():
            raise FileNotFoundError(cls.tool)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.tool), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

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
