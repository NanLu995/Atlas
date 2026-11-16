import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import platform
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("atlas_build_openssl", ROOT / "tools/build_openssl.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)
from build import resolve_cmake


class OpenSslBuilderTest(unittest.TestCase):
    def test_cmake_rejects_missing_or_wrong_sdk_without_system_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "sdk"
            module = ROOT / "cmake/AtlasOpenSSL.cmake"
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.28)\nproject(Probe NONE)\n'
                'add_library(Threads::Threads INTERFACE IMPORTED)\n'
                f'set(ATLAS_BIN_ROOT "{(root / "output").as_posix()}")\n'
                f'include("{module.as_posix()}")\n', encoding="utf-8")
            command = [resolve_cmake(dict(os.environ)), "-S", str(root), "-B", str(root / "build"),
                       f"-DOPENSSL_ROOT_DIR={sdk}"]
            missing = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("Pinned OpenSSL SDK missing", missing.stdout)
            names = ["lib/libcrypto.lib", "lib/libssl.lib"] if platform.system() == "Windows" else [
                "lib/libcrypto.a", "lib/libssl.a"]
            names += ["atlas-source.json", "share/licenses/OpenSSL/LICENSE.txt",
                      "share/licenses/OpenSSL/AUTHORS.md", "include/openssl/opensslv.h"]
            for name in names:
                path = sdk / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("test fixture", encoding="utf-8")
            header = sdk / "include/openssl/opensslv.h"
            header.write_text('#define OPENSSL_VERSION_STR "3.5.5"\n', encoding="utf-8")
            wrong = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertIn("requires the pinned OpenSSL 3.5.9 SDK", wrong.stdout)
            header.write_text('#define OPENSSL_VERSION_STR "3.5.9"\n', encoding="utf-8")
            correct = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(correct.returncode, 0, correct.stdout)
            self.assertEqual((root / "output/licenses/OpenSSL/LICENSE.txt").read_text(), "test fixture")

    def test_wrong_source_checksum_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "archive.tar.gz"
            archive.write_bytes(b"not the official OpenSSL release")
            with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
                builder.verify_archive(archive)

    def test_prefix_outside_build_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(builder, "REPO_ROOT", root), contextlib.redirect_stderr(io.StringIO()):
                with patch.object(sys, "argv", ["build_openssl", "--prefix", str(root / "outside")]):
                    with self.assertRaises(SystemExit) as result:
                        builder.main()
            self.assertEqual(result.exception.code, 2)
            self.assertFalse((root / "outside").exists())

    def test_verified_sdk_is_reused_and_changed_artifacts_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "build/openssl/3.5.9/Debug"
            names = ["lib/libcrypto.lib", "lib/libssl.lib", "bin/openssl.exe"] if platform.system() == "Windows" else [
                "lib/libcrypto.a", "lib/libssl.a", "bin/openssl"]
            names += ["include/openssl/opensslv.h", "share/licenses/OpenSSL/LICENSE.txt",
                      "share/licenses/OpenSSL/AUTHORS.md"]
            for name in names:
                path = prefix / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"isolated test artifact")
            receipt = {"source_sha256": builder.SHA256, "config": "Debug", "platform": platform.system(),
                       "sha256": {name: hashlib.sha256((prefix / name).read_bytes()).hexdigest()
                                  for name in names}}
            (prefix / "atlas-source.json").write_text(json.dumps(receipt), encoding="utf-8")
            with patch.object(builder, "REPO_ROOT", root), patch.object(sys, "argv", ["build_openssl"]):
                with patch.object(builder, "load_msvc_env", side_effect=AssertionError("must not rebuild")):
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(builder.main(), 0)
                    (prefix / names[0]).write_bytes(b"changed artifact")
                    with self.assertRaisesRegex(RuntimeError, "receipt mismatch"):
                        builder.main()


if __name__ == "__main__":
    unittest.main()
