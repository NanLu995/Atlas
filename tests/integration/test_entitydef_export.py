import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest


class EntityDefExportTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="atlas_entitydef_export_")
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / "entity_defs.bin"

    def run_tool(self, *arguments):
        return subprocess.run(
            [str(ARGS.dotnet), str(ARGS.defdump), *map(str, arguments)],
            capture_output=True, text=True, timeout=30,
        )

    def test_export_works_without_a_native_provider_and_matches_the_metadata_digest(self):
        result = self.run_tool("--assembly", ARGS.assembly, "--out", self.output)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = self.output.read_bytes()
        magic, version, flags, digest = struct.unpack_from("<IHH32s", data)
        self.assertEqual((magic, version, flags), (0x46445441, 4, 0))
        self.assertEqual(hashlib.sha256(data[:-32]).digest(), data[-32:])
        self.assertNotEqual(digest, bytes(32))
        self.assertRegex(result.stdout, r"types=[1-9]\d*")
        metadata = self.run_tool("--digest-only", ARGS.assembly)
        self.assertEqual(metadata.returncode, 0, metadata.stdout + metadata.stderr)
        match = re.search(r"digest=([0-9a-f]{64})", metadata.stdout)
        self.assertIsNotNone(match, metadata.stdout)
        self.assertEqual(digest.hex(), match.group(1))

    def test_assembly_without_the_export_contract_is_rejected_before_loading(self):
        result = self.run_tool("--assembly", ARGS.runtime_assembly, "--out", self.output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rebuild", result.stderr)
        self.assertFalse(self.output.exists())

    def test_invalid_assembly_is_diagnosed_without_an_output(self):
        invalid = Path(self.directory.name) / "invalid.dll"
        invalid.write_bytes(b"invalid assembly")
        result = self.run_tool("--assembly", invalid, "--out", self.output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot inspect", result.stderr)
        self.assertIn("invalid.dll", result.stderr)
        self.assertFalse(self.output.exists())

    def test_write_failure_is_reported(self):
        blocked = Path(self.directory.name) / "file"
        blocked.write_bytes(b"occupied")
        result = self.run_tool("--assembly", ARGS.assembly, "--out", blocked / "entity_defs.bin")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("write failed", result.stderr)

    def run_dbapp(self, *arguments):
        return subprocess.run(
            [str(ARGS.dbapp), "--type", "dbapp", "--name", "dbapp", "--internal-port", "0",
             *map(str, arguments)], capture_output=True, text=True, timeout=15,
        )

    def test_dbapp_requires_definitions_before_opening_the_database(self):
        result = self.run_dbapp()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--entitydef-bin-path", result.stdout + result.stderr)
        self.assertNotIn("database startup failed", result.stdout + result.stderr)

    def test_dbapp_rejects_empty_definitions_before_opening_the_database(self):
        data = struct.pack("<IHH32s", 0x46445441, 4, 0, bytes(range(32))) + bytes(3)
        self.output.write_bytes(data + hashlib.sha256(data).digest())
        result = self.run_dbapp("--entitydef-bin-path", self.output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("entity types", result.stdout + result.stderr)
        self.assertNotIn("database startup failed", result.stdout + result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify isolated offline entity-definition export.")
    parser.add_argument("--dotnet", type=Path, required=True)
    parser.add_argument("--defdump", type=Path, required=True)
    parser.add_argument("--assembly", type=Path, required=True)
    parser.add_argument("--runtime-assembly", type=Path, required=True)
    parser.add_argument("--dbapp", type=Path, required=True)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
