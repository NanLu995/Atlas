from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from common.database import add_database_options, database_config, record_database_run, wait_for_database_start
from cluster_control import test_cluster


class DatabaseToolsTest(unittest.TestCase):
    def test_cluster_script_override_is_explicit(self):
        with patch.object(sys, 'argv', ['test_cluster.py']):
            self.assertIsNone(test_cluster.parse_args().base_assembly)
            self.assertIsNone(test_cluster.parse_args().cell_assembly)
        with patch.object(sys, 'argv', ['test_cluster.py', '--base-assembly', 'fixture.dll']):
            self.assertEqual(test_cluster.parse_args().base_assembly, Path('fixture.dll'))
        with patch.object(sys, 'argv', ['test_cluster.py', '--cell-assembly', 'cell.dll']):
            self.assertEqual(test_cluster.parse_args().cell_assembly, Path('cell.dll'))

    def test_missing_explicit_assembly_never_starts_cluster(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'runtime').mkdir()
            (root / 'runtime/atlas_server.runtimeconfig.json').write_text('{}')
            (root / 'bin/debug').mkdir(parents=True)
            (root / 'bin/debug/Atlas.Runtime.dll').write_bytes(b'')
            missing = root / 'missing-fixture.dll'
            args = SimpleNamespace(build_dir='debug', base_assembly=missing, cell_assembly=None)
            with patch.object(test_cluster, 'resolve_repo_root', return_value=root), \
                 patch.object(test_cluster, 'resolve_program', return_value=Path(__file__)), \
                 patch.object(test_cluster, 'start_logged_process') as start, \
                 patch.object(sys, 'stderr', new_callable=io.StringIO) as output:
                self.assertEqual(test_cluster._spawn_cluster(args, 25000), 1)
                start.assert_not_called()
                self.assertIn(str(missing), output.getvalue())
                args.base_assembly = None
                args.cell_assembly = missing
                self.assertEqual(test_cluster._spawn_cluster(args, 25000), 1)
                start.assert_not_called()

    def args(self, *values):
        parser = argparse.ArgumentParser()
        add_database_options(parser)
        return parser.parse_args(values)

    def test_default_requires_mysql_environment(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(self.args().db_backend, 'mysql')
            with self.assertRaisesRegex(RuntimeError, 'MySQL environment blocked'):
                database_config(self.args(), Path('unused.sqlite3'))

    def test_sqlite_is_explicit_and_records_smoke_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = database_config(self.args('--db-backend', 'sqlite'), root / 'smoke.sqlite3')
            self.assertEqual(config['type'], 'sqlite')
            record_database_run(root, config, baseapps=2)
            result = json.loads((root / 'database-run.json').read_text())
            self.assertEqual(result['scope'], 'sqlite_smoke_only')
            self.assertEqual(result['topology']['dbapps'], 1)

    def test_mysql_secret_is_inherited_and_not_serialized(self):
        environment = {'ATLAS_DB_MYSQL_USER': '<test-user>',
                       'ATLAS_DB_MYSQL_PASSWORD': '<test-password>',
                       'ATLAS_DB_MYSQL_DATABASE': 'isolated_test'}
        with patch.dict(os.environ, environment, clear=True):
            config = database_config(self.args(), Path('unused.sqlite3'))
            self.assertNotIn('mysql_password', config)
            self.assertNotIn('sqlite_path', config)
            self.assertEqual(config['mysql_database'], 'isolated_test')
            self.assertEqual(os.environ['ATLAS_DB_MYSQL_PASSWORD'], '<test-password>')
            os.environ['ATLAS_DB_MYSQL_PORT'] = 'invalid'
            with self.assertRaisesRegex(RuntimeError, 'port must be'):
                database_config(self.args(), Path('unused.sqlite3'))

    def test_configuration_must_match_selected_backend(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / 'db.json'
            config.write_text(json.dumps({'database': {'type': 'sqlite'}}))
            with self.assertRaisesRegex(RuntimeError, 'must match'):
                database_config(self.args('--database-config', str(config)), Path('unused.sqlite3'))
            config.write_text('{}')
            with self.assertRaisesRegex(RuntimeError, 'database object'):
                database_config(self.args('--database-config', str(config)), Path('unused.sqlite3'))

    def test_database_registration_does_not_imply_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            logs = Path(directory)
            process = SimpleNamespace(poll=lambda: None)
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                wait_for_database_start(process, logs, timeout=0)
            (logs / 'dbapp.stdout.log').write_text('DBApp: initialised (backend=mysql,')
            wait_for_database_start(process, logs)
            process.poll = lambda: 1
            with self.assertRaisesRegex(RuntimeError, 'no backend fallback'):
                wait_for_database_start(process, logs)


if __name__ == '__main__':
    unittest.main()
