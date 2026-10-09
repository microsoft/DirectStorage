import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import jsonschema


FACTORY_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = FACTORY_ROOT.parent
DRIVER = FACTORY_ROOT / "tools" / "process-set.py"
SCHEMA = FACTORY_ROOT / "tools" / "content-set-manifest.schema.json"


def load_driver():
    spec = importlib.util.spec_from_file_location("process_set", DRIVER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


class ProcessSetTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.sources = self.root / "originals" / "sample_set"
        (self.sources / "nested").mkdir(parents=True)
        (self.sources / "z.bin").write_bytes(b"z" * 17)
        (self.sources / "nested" / "a.bin").write_bytes(bytes(range(64)))

    def tearDown(self):
        self.temp.cleanup()

    def run_driver(self, *args: object) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(DRIVER), "sample_set", "--root", str(self.root), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_creates_schema_valid_deterministic_manifest_and_layout(self):
        first = self.run_driver()
        self.assertEqual(first.returncode, 0, first.stderr)
        manifest_path = self.root / "manifests" / "sample_set.json"
        first_bytes = manifest_path.read_bytes()
        document = json.loads(first_bytes)
        schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
        jsonschema.validate(document, schema)

        self.assertEqual([item["path"] for item in document["sources"]], ["nested/a.bin", "z.bin"])
        for item in document["sources"]:
            path = self.sources / item["path"]
            self.assertEqual(item["size"], path.stat().st_size)
            self.assertEqual(item["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        for name in ("zstd", "gdeflate", "gacl", "dstorage"):
            self.assertTrue((self.root / name / "sample_set").is_dir())

        second = self.run_driver("--overwrite")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first_bytes, manifest_path.read_bytes())
        self.assertEqual(self.run_driver("--verify").returncode, 0)

    def test_refuses_existing_manifest_without_overwrite(self):
        self.assertEqual(self.run_driver().returncode, 0)
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("use --overwrite", result.stderr)

    def test_verify_detects_source_change_and_extra_source(self):
        self.assertEqual(self.run_driver().returncode, 0)
        (self.sources / "z.bin").write_bytes(b"changed")
        result = self.run_driver("--verify")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source inventory differs", result.stderr)

        (self.sources / "z.bin").write_bytes(b"z" * 17)
        (self.sources / "new.bin").write_bytes(b"new")
        result = self.run_driver("--verify")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source inventory differs", result.stderr)

    def test_rejects_invalid_set_names_missing_and_empty_sets(self):
        for name in ("../escape", ".", "bad/name", "bad name", "CON", "nul.txt", "trailing."):
            result = subprocess.run(
                [sys.executable, str(DRIVER), name, "--root", str(self.root)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0, name)

        missing = subprocess.run(
            [sys.executable, str(DRIVER), "missing", "--root", str(self.root)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(missing.returncode, 0)

        empty = self.root / "originals" / "empty"
        empty.mkdir()
        result = subprocess.run(
            [sys.executable, str(DRIVER), "empty", "--root", str(self.root)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)

    @unittest.skipUnless(hasattr(Path, "symlink_to"), "symbolic links are unavailable")
    def test_rejects_source_symlinks(self):
        external = self.root / "external.bin"
        external.write_bytes(b"outside")
        link = self.sources / "link.bin"
        try:
            link.symlink_to(external)
        except OSError as error:
            self.skipTest(f"symbolic links require additional privileges: {error}")
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symbolic links", result.stderr)
        self.assertFalse((self.root / "manifests").exists())
        self.assertFalse((self.root / "zstd").exists())

    def test_overwrite_refuses_to_discard_derivative_records(self):
        self.assertEqual(self.run_driver().returncode, 0)
        manifest = self.root / "manifests" / "sample_set.json"
        document = json.loads(manifest.read_text(encoding="utf-8"))
        document["derivatives"] = [{
            "path": "zstd/sample_set/output.zst", "size": 1, "sha256": "0" * 64,
            "source": document["sources"][0]["path"], "format": "zstd",
            "parameters": {"block_size_kb": 4, "chunk_size_kb": 64},
            "metadata": {"frame_count": 1, "uncompressed_size": document["sources"][0]["size"]},
            "validation": "pass",
        }]
        manifest.write_text(json.dumps(document), encoding="utf-8")
        result = self.run_driver("--overwrite")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot discard", result.stderr)

    def test_schema_and_internal_validator_reject_extra_record_fields(self):
        driver = load_driver()
        document = driver.create_manifest(self.root, "sample_set")
        document["sources"][0]["extra"] = True
        with self.assertRaises(ValueError):
            driver.validate_manifest(document)
        schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(document, schema)

    def test_rejects_derivative_outside_its_format_set_directory(self):
        driver = load_driver()
        document = driver.create_manifest(self.root, "sample_set")
        document["derivatives"] = [{
            "path": "gdeflate/other_set/output.bin",
            "size": 1,
            "sha256": "0" * 64,
            "source": document["sources"][0]["path"],
            "format": "gdeflate",
            "parameters": {},
            "metadata": {},
            "validation": "pass",
        }]
        with self.assertRaises(ValueError):
            driver.validate_manifest(document)

    def test_internal_validation_rejects_bad_references_and_paths(self):
        driver = load_driver()
        document = driver.create_manifest(self.root, "sample_set")
        source = document["sources"][0]
        document["derivatives"] = [{
            "path": "../escape.bin",
            "size": 1,
            "sha256": "0" * 64,
            "source": source["path"],
            "format": "zstd",
            "parameters": {"block_size_kb": 4, "chunk_size_kb": 64},
            "metadata": {"frame_count": 1, "uncompressed_size": source["size"]},
            "validation": "pass",
        }]
        with self.assertRaises(ValueError):
            driver.validate_manifest(document)

        document["derivatives"][0]["path"] = "zstd/sample_set/output.zst"
        document["derivatives"][0]["source"] = "unknown.bin"
        with self.assertRaises(ValueError):
            driver.validate_manifest(document)

    def test_verify_checks_recorded_derivative_bytes(self):
        driver = load_driver()
        document = driver.create_manifest(self.root, "sample_set")
        output = self.root / "zstd" / "sample_set" / "output.zst"
        output.parent.mkdir(parents=True)
        output.write_bytes(b"payload")
        source = document["sources"][0]
        document["derivatives"] = [{
            "path": "zstd/sample_set/output.zst",
            "size": output.stat().st_size,
            "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "source": source["path"],
            "format": "zstd",
            "parameters": {"chunk_size_kb": 64, "block_size_kb": 4},
            "metadata": {"frame_count": 1, "uncompressed_size": source["size"]},
            "validation": "pass",
        }]
        driver.validate_manifest(document)
        driver.write_json_atomic(driver.manifest_path(self.root, "sample_set"), document)
        self.assertEqual(self.run_driver("--verify").returncode, 0)

        output.write_bytes(b"corrupt")
        result = self.run_driver("--verify")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match manifest", result.stderr)

    def test_schema_and_runtime_accept_dotted_set_name(self):
        driver = load_driver()
        source = self.root / "originals" / "content.v2" / "asset.bin"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"asset")
        document = driver.create_manifest(self.root, "content.v2")
        driver.validate_manifest(document)
        schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
        jsonschema.validate(document, schema)
        path = driver.manifest_path(self.root, "content.v2")
        driver.write_json_atomic(path, document)
        driver.verify_files(self.root, "content.v2", driver.load_manifest(path))


if __name__ == "__main__":
    unittest.main()
