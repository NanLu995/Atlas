from __future__ import annotations

import argparse
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from common.database import add_database_options, database_config, record_database_run, wait_for_database_start
from cluster_control import test_cluster
from common.cluster import wait_for_registration


class DatabaseToolsTest(unittest.TestCase):
    def test_restart_only_reuses_a_stopped_owned_baseapp_and_keeps_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process = Mock()
            process.poll.return_value = None
            process.args = ['owned-baseapp', '--name', 'baseapp']
            previous = SimpleNamespace(name='baseapp', process=process, start_order=4)
            processes = [previous]
            with patch.object(test_cluster, 'start_logged_process') as start:
                with self.assertRaisesRegex(RuntimeError, 'not in this test cluster'):
                    test_cluster._restart_baseapp(processes, [(25005, 25006)], 26000, root, root)
                with self.assertRaisesRegex(RuntimeError, 'still running'):
                    test_cluster._restart_baseapp(processes, [(25005, 25006)], 25005, root, root)
                start.assert_not_called()
                process.poll.return_value = 1
                test_cluster._restart_baseapp(processes, [(25005, 25006)], 25005, root, root)
                self.assertEqual(processes, [start.return_value])
                self.assertEqual(processes[0].start_order, 4)
                self.assertEqual(start.call_args.kwargs['arguments'], ['--name', 'baseapp'])
                self.assertTrue(start.call_args.kwargs['log_directory'].is_relative_to(root / 'restarts'))

    def test_cluster_kill_is_limited_to_its_own_live_baseapp(self):
        first, second = Mock(), Mock()
        first.poll.return_value = second.poll.return_value = None
        processes = [SimpleNamespace(name='baseapp', process=first),
                     SimpleNamespace(name='baseapp-2', process=second)]
        ports = [(25005, 25006), (25010, 25011)]
        with self.assertRaisesRegex(RuntimeError, 'not in this test cluster'):
            test_cluster._kill_baseapp(processes, ports, 26000)
        first.kill.assert_not_called()
        second.kill.assert_not_called()
        test_cluster._kill_baseapp(processes, ports, 25010)
        first.kill.assert_not_called()
        second.kill.assert_called_once_with()
        second.wait.assert_called_once_with(timeout=5)
        first.poll.return_value = 1
        with self.assertRaisesRegex(RuntimeError, 'already exited'):
            test_cluster._kill_baseapp(processes, ports, 25005)
        first.kill.assert_not_called()

    def test_handoff_cluster_count_is_explicit_and_bounded(self):
        with patch.object(sys, 'argv', ['test_cluster.py']):
            self.assertEqual(test_cluster.parse_args().baseapps, 1)
        with patch.object(sys, 'argv', ['test_cluster.py', '--baseapps', '2']):
            self.assertEqual(test_cluster.parse_args().baseapps, 2)
        with patch.object(sys, 'argv', ['test_cluster.py', '--baseapps', '3']), \
             patch.object(sys, 'stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                test_cluster.parse_args()

    def test_registration_matches_the_whole_process_name(self):
        outputs = [SimpleNamespace(returncode=0, stdout='baseapp baseapp-2 127.0.0.1:25001'),
                   SimpleNamespace(returncode=0, stdout='baseapp baseapp 127.0.0.1:25002')]
        with patch('common.cluster.subprocess.run', side_effect=outputs) as run, \
             patch('common.cluster.time.sleep'):
            self.assertTrue(wait_for_registration(atlas_tool=Path('tool'),
                machined_address='127.0.0.1:25000', proc_type='baseapp', name='baseapp'))
            self.assertEqual(run.call_count, 2)

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
