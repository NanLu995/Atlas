import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import package_delivery
from common.provenance import source_state


class DeliveryTest(unittest.TestCase):
    def test_archive_excludes_unselected_dlls_and_requires_matching_sdk(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release = root / 'build/release'
            release.mkdir(parents=True)
            (release / 'CMakeCache.txt').write_text(
                'CMAKE_BUILD_TYPE:STRING=Release\nATLAS_BUILD_TESTS:BOOL=ON\n')
            (release / 'runtime').mkdir()
            (release / 'runtime/atlas_server.runtimeconfig.json').write_text('{}')
            for name in ('googletest', 'pugixml', 'rapidjson', 'zlib', 'sqlite3', 'mimalloc',
                         'jolt', 'recastnavigation', 'tracy'):
                source = release / '_deps' / (name + '-src')
                source.mkdir(parents=True)
                (source / 'LICENSE').write_bytes(b'license')
            binary = root / 'bin/release'
            binary.mkdir(parents=True)
            for name in ('machined', 'atlas_loginapp', 'atlas_baseappmgr', 'atlas_baseapp',
                         'atlas_dbapp', 'atlas_cellappmgr', 'atlas_cellapp', 'atlas_tool'):
                (binary / (name + ('.exe' if sys.platform == 'win32' else ''))).touch()
            for name in ('Atlas.Shared.dll', 'Atlas.ClrHost.dll', 'Atlas.Runtime.dll',
                         'Business.dll', 'obsolete.dll'):
                (binary / name).touch()
            bundle = root / 'sdk'
            bundle.mkdir()
            (bundle / 'entity_defs.bin').touch()
            (root / 'source.cc').write_bytes(b'source')
            (root / 'picture.png').write_bytes(b'asset')
            manifest = {'config': 'Release', 'build': 'release', 'sha256': {},
                        'commit': 'commit', 'source_sha256': 'source',
                        'entity_definition_sha256': 'definition'}
            arguments = ['package_delivery', '--build-dir', str(release), '--verification-build',
                         str(release), '--sdk', str(bundle), '--base-assembly',
                         str(binary / 'Business.dll'), '--output', str(root / 'delivery.zip')]
            with patch.object(sys, 'argv', arguments), \
                    patch.dict(os.environ, {'ATLAS_TEST_MYSQL': '1'}), \
                    patch.object(package_delivery, 'REPO_ROOT', root), \
                    patch.object(package_delivery, 'require_generated_output'), \
                    patch.object(package_delivery, 'verify', return_value=manifest), \
                    patch.object(package_delivery, 'export_entity_definitions',
                                 return_value='definition'), \
                    patch.object(package_delivery, 'source_state', return_value={
                        'commit': 'commit', 'source_sha256': 'source'}), \
                    patch.object(package_delivery, 'load_msvc_env', return_value=({}, 17)), \
                    patch.object(package_delivery, 'resolve_cmake', return_value='cmake'), \
                    patch.object(package_delivery.subprocess, 'run') as run, \
                    patch.object(package_delivery.subprocess, 'check_output',
                                 return_value=b'source.cc\0picture.png\0'):
                package_delivery.main()
            self.assertEqual(run.call_count, 3)
            with zipfile.ZipFile(root / 'delivery.zip') as archive:
                self.assertIn('server/Business.dll', archive.namelist())
                self.assertNotIn('server/obsolete.dll', archive.namelist())
                self.assertIn('sha256', json.loads(archive.read('delivery.json')))
                self.assertIn('source/source.cc', archive.namelist())
                self.assertNotIn('source/picture.png', archive.namelist())
                self.assertIn('source/third_party/zlib/LICENSE', archive.namelist())

    def test_missing_isolation_fails_before_reading_or_writing(self):
        arguments = ['package_delivery', '--build-dir', 'unused', '--verification-build', 'unused',
                     '--sdk', 'unused', '--base-assembly', 'unused', '--output', 'unused.zip']
        with patch.object(sys, 'argv', arguments), patch.dict(os.environ, {}, clear=True), \
                contextlib.redirect_stderr(io.StringIO()) as error:
            with self.assertRaises(SystemExit):
                package_delivery.main()
            self.assertIn('isolated MySQL', error.getvalue())

    def test_source_fingerprint_detects_edits_with_same_dirty_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'source.py'
            path.write_bytes(b'first')
            with patch('common.provenance.subprocess.check_output', side_effect=[
                    b'source.py\0', 'commit', b'source.py\0', 'commit']):
                before = source_state(root)
                path.write_bytes(b'second')
                after = source_state(root)
            self.assertEqual(before['commit'], after['commit'])
            self.assertNotEqual(before['source_sha256'], after['source_sha256'])


if __name__ == '__main__':
    unittest.main()
