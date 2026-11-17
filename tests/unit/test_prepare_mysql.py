import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
import json
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import prepare_mysql
import test_mysql as fixture


class MysqlSdkTest(unittest.TestCase):
    def test_digest_handles_multiple_chunks_and_empty_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'archive'
            for content in (b'', b'archive' * 200000):
                path.write_bytes(content)
                self.assertEqual(prepare_mysql.digest(path), hashlib.sha256(content).hexdigest())

    def test_repeated_preparation_refreshes_the_corresponding_source_location(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / prepare_mysql.ARTIFACTS['windows'][0]
            sdk_name = binary.stem
            with zipfile.ZipFile(binary, 'w') as package:
                for name in ('LICENSE', 'include/mysql.h', 'include/mysql_version.h'):
                    package.writestr(sdk_name + '/' + name, b'test SDK')
            for index in range(2):
                source = root / f'source-{index}.tar.gz'
                source.write_bytes(b'test source')
                with patch.object(prepare_mysql, 'REPO_ROOT', root), \
                     patch.object(prepare_mysql.platform, 'system', return_value='Windows'), \
                     patch.object(prepare_mysql.platform, 'machine', return_value='AMD64'), \
                     patch.object(prepare_mysql, 'archive', side_effect=[binary, source]), \
                     patch.object(sys, 'stdout', new_callable=io.StringIO):
                    self.assertEqual(prepare_mysql.main([]), 0)
                manifest = root / 'build/oracle-mysql/8.4.13/windows' / sdk_name / '.atlas-verified.json'
                self.assertEqual(json.loads(manifest.read_text())['source_archive'], str(source.resolve()))

    def test_missing_docker_is_reported_before_creating_a_fixture(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(fixture.shutil, 'which', return_value=None), \
             patch.object(sys, 'stderr', new_callable=io.StringIO) as error:
            path = Path(directory) / 'new'
            with self.assertRaises(SystemExit):
                fixture.main(['--directory', str(path), '--docker', '--', 'unused-command'])
            self.assertIn('Docker missing', error.getvalue())
            self.assertFalse(path.exists())

    def test_fixture_never_reuses_or_modifies_an_existing_directory(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(fixture.subprocess, 'run') as run:
            path = Path(directory)
            (path / 'preserve').write_bytes(b'existing data')
            with self.assertRaises(SystemExit) as error:
                fixture.main(['--directory', str(path), '--', 'unused-command'])
            self.assertEqual(error.exception.code, 2)
            self.assertEqual((path / 'preserve').read_bytes(), b'existing data')
            run.assert_not_called()

    def test_fixture_requires_a_command_before_preparing_any_files(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(fixture.subprocess, 'run') as run:
            path = Path(directory) / 'new'
            with self.assertRaises(SystemExit):
                fixture.main(['--directory', str(path)])
            self.assertFalse(path.exists())
            run.assert_not_called()

    def test_corrupt_cached_archive_is_rejected_without_extracting(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            name, _ = prepare_mysql.ARTIFACTS['windows']
            (cache / name).write_bytes(b'corrupt')
            with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
                prepare_mysql.archive('windows', cache)


if __name__ == '__main__':
    unittest.main()
