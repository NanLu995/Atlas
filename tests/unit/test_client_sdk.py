import json
import os
from pathlib import Path
import platform
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import client_sdk


class ClientSdkTest(unittest.TestCase):
    def bundle(self, root):
        root.mkdir()
        names = [*client_sdk.ASSEMBLIES,
                 'atlas_net_client.dll' if os.name == 'nt' else 'libatlas_net_client.so']
        for name in names:
            (root / name).write_bytes(name.encode())
        manifest = {'format': 1, 'platform': platform.system(),
                    'architecture': platform.machine(), **client_sdk.contract(),
                    'sha256': {name: client_sdk.digest(root / name) for name in names}}
        (root / client_sdk.MANIFEST).write_text(json.dumps(manifest))

    def test_verify_rejects_tampering_and_extra_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'sdk'
            self.bundle(root)
            client_sdk.verify(root)
            (root / 'Atlas.Client.dll').write_bytes(b'altered')
            with self.assertRaisesRegex(RuntimeError, 'altered'):
                client_sdk.verify(root)

    def test_verify_rejects_mismatched_abi_or_protocol(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'sdk'
            self.bundle(root)
            manifest_path = root / client_sdk.MANIFEST
            original = json.loads(manifest_path.read_text())
            for key, value in (('abi', '0x00000000'), ('protocol', -1)):
                with self.subTest(key=key):
                    manifest = dict(original, **{key: value})
                    manifest_path.write_text(json.dumps(manifest))
                    with self.assertRaisesRegex(RuntimeError, 'ABI/protocol mismatch'):
                        client_sdk.verify(root)

    def test_verify_rejects_extra_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'sdk'
            self.bundle(root)
            (root / 'unknown.dll').touch()
            with self.assertRaisesRegex(RuntimeError, 'unrecorded files'):
                client_sdk.verify(root)

    def test_install_is_repeatable_and_rejects_obsolete_dll(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.bundle(root / 'sdk')
            (root / 'project.godot').touch()
            for _ in range(2):
                client_sdk.install(root / 'sdk', root)
            target = root / 'addons/atlas_client'
            client_sdk.verify(target)
            (target / 'obsolete.dll').write_bytes(b'preserve')
            with self.assertRaisesRegex(RuntimeError, 'removal approval'):
                client_sdk.install(root / 'sdk', root)
            self.assertEqual((target / 'obsolete.dll').read_bytes(), b'preserve')

    def test_install_failure_restores_previous_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.bundle(root / 'sdk')
            (root / 'project.godot').touch()
            client_sdk.install(root / 'sdk', root)
            target = root / 'addons/atlas_client'
            before = {p.name: p.read_bytes() for p in target.iterdir()}
            original = client_sdk.shutil.copy2
            def copy(source, destination):
                if Path(source) == root / 'sdk/Atlas.Client.dll':
                    raise OSError('simulated disk failure')
                return original(source, destination)
            with patch.object(client_sdk.shutil, 'copy2', side_effect=copy):
                with self.assertRaises(OSError):
                    client_sdk.install(root / 'sdk', root)
            self.assertEqual(before, {p.name: p.read_bytes() for p in target.iterdir()})


if __name__ == '__main__':
    unittest.main()
