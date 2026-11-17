from __future__ import annotations

import argparse
import io
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from common.database import add_database_options, database_config, record_database_run, wait_for_database_start
from cluster_control import test_cluster
from common.cluster import export_entity_definitions, stop_logged_processes, wait_for_registration
import database_recovery as recovery


class DatabaseToolsTest(unittest.TestCase):
    def test_definition_export_requires_the_selected_build_tool(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch('common.cluster.subprocess.run') as run:
            root = Path(directory)
            with self.assertRaisesRegex(RuntimeError, 'atlas_tools_defdump'):
                export_entity_definitions(root, 'framework-verify', root / 'Base.dll',
                                          root / 'defs.bin')
            run.assert_not_called()

    def test_definition_export_uses_the_selected_assembly_and_validates_its_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tool = root / 'build/verify/csharp/Atlas.Tools.DefDump.dll'
            tool.parent.mkdir(parents=True)
            tool.write_bytes(b'')
            assembly = root / 'Base.dll'
            assembly.write_bytes(b'')
            output = root / 'defs.bin'
            digest = bytes(range(32))
            output.write_bytes(b'ATDF\x04\0\0\0' + digest)
            with patch('common.cluster.subprocess.run',
                       return_value=SimpleNamespace(returncode=0)) as run:
                self.assertEqual(export_entity_definitions(root, 'build/verify', assembly, output),
                                 digest.hex())
                command = run.call_args.args[0]
                self.assertEqual(command, ['dotnet', str(tool), '--assembly', str(assembly),
                                           '--out', str(output)])
                output.write_bytes(b'truncated')
                with self.assertRaisesRegex(RuntimeError, 'invalid definition header'):
                    export_entity_definitions(root, 'verify', assembly, output)

    def test_cluster_requires_an_explicit_entity_assembly(self):
        args = SimpleNamespace(build_dir='debug', base_assembly=None, cell_assembly=None)
        with patch.object(test_cluster, 'start_logged_process') as start, \
             patch.object(sys, 'stderr', new_callable=io.StringIO) as output:
            self.assertEqual(test_cluster._spawn_cluster(args, 25000), 1)
            self.assertIn('--base-assembly', output.getvalue())
            start.assert_not_called()

    def test_cluster_log_level_is_explicit_and_validated(self):
        with patch.object(sys, 'argv', ['test_cluster.py']):
            self.assertEqual(test_cluster.parse_args().log_level, 'info')
        with patch.object(sys, 'argv', ['test_cluster.py', '--dtls-certificate', 'missing.pem']), \
                patch.object(sys, 'stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                test_cluster.parse_args()
        with patch.object(sys, 'argv', ['test_cluster.py', '--log-level', 'debug']):
            self.assertEqual(test_cluster.parse_args().log_level, 'debug')
        with patch.object(sys, 'argv', ['test_cluster.py', '--log-level', 'invalid']), \
             patch.object(sys, 'stderr', new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                test_cluster.parse_args()

    def test_shutdown_only_requests_an_owned_live_baseapp_and_checks_exit(self):
        process = Mock(returncode=0)
        process.poll.return_value = None
        process.wait.return_value = 0
        entries = [SimpleNamespace(name='baseapp', process=process)]
        ports = [(25005, 25006)]
        with patch.object(test_cluster.subprocess, 'run') as run:
            run.return_value.returncode = 0
            with self.assertRaisesRegex(RuntimeError, 'not in this test cluster'):
                test_cluster._shutdown_baseapp(entries, ports, 26000, Path('atlas_tool'), '127.0.0.1:1')
            run.assert_not_called()
            test_cluster._shutdown_baseapp(entries, ports, 25005, Path('atlas_tool'), '127.0.0.1:1')
            self.assertEqual(run.call_args.args[0][-2:], ['shutdown', 'baseapp:baseapp'])
            process.wait.assert_called_once_with(timeout=35)
            process.wait.return_value = 1
            with self.assertRaisesRegex(RuntimeError, 'shutdown exit code 1'):
                test_cluster._shutdown_baseapp(entries, ports, 25005, Path('atlas_tool'), '127.0.0.1:1')

    def test_cluster_shutdown_propagates_failed_exit_and_forced_termination(self):
        process = Mock(returncode=1, pid=123)
        process.poll.return_value = None
        process.wait.return_value = 1
        entry = SimpleNamespace(name='owned', process=process, start_order=1,
                                stdout_handle=None, stderr_handle=None)
        signal_function = 'os.kill' if os.name == 'nt' else 'os.killpg'
        with patch('common.cluster.' + signal_function):
            with self.assertRaisesRegex(RuntimeError, 'shutdown exit code 1'):
                stop_logged_processes([entry])
            process.wait.side_effect = [subprocess.TimeoutExpired('owned', 35), 0]
            with self.assertRaisesRegex(RuntimeError, 'forced termination'):
                stop_logged_processes([entry])

    def test_restart_only_reuses_a_stopped_owned_baseapp_and_keeps_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            process = Mock()
            process.poll.return_value = None
            process.args = ['owned-baseapp', '--name', 'baseapp']
            previous = SimpleNamespace(name='baseapp', process=process, start_order=4,
                                       stdout_handle=Mock(), stderr_handle=Mock())
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
                previous.stdout_handle.close.assert_called_once_with()
                previous.stderr_handle.close.assert_called_once_with()

    def test_named_process_control_is_limited_to_owned_processes(self):
        for name in ('cellapp', 'dbapp', 'machined'):
            process = Mock()
            process.poll.return_value = None
            entry = SimpleNamespace(name=name, process=process)
            with self.assertRaisesRegex(RuntimeError, 'not in this test cluster'):
                test_cluster._process_index([entry], 'unowned')
            self.assertEqual(test_cluster._process_index([entry], name), 0)
            test_cluster._kill_process(entry)
            process.kill.assert_called_once_with()
            process.wait.assert_called_once_with(timeout=5)
            process.poll.return_value = 1
            with self.assertRaisesRegex(RuntimeError, 'already exited'):
                test_cluster._kill_process(entry)

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

    def test_recovery_cluster_cell_count_is_explicit_and_requires_an_assembly(self):
        with patch.object(sys, 'argv', ['test_cluster.py']):
            self.assertEqual(test_cluster.parse_args().cellapps, 1)
        with patch.object(sys, 'argv', ['test_cluster.py', '--cellapps', '2',
                                      '--cell-assembly', 'Cell.dll']):
            self.assertEqual(test_cluster.parse_args().cellapps, 2)
        for arguments in (['--cellapps', '2'], ['--cellapps', '3']):
            with self.subTest(arguments=arguments), \
                 patch.object(sys, 'argv', ['test_cluster.py', *arguments]), \
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
                       'ATLAS_DB_MYSQL_DATABASE': 'isolated_test',
                       'ATLAS_DB_MYSQL_SSL_CA': '<test-ca.pem>',
                       'ATLAS_DB_MYSQL_DATABASE_ID': '0123456789abcdef0123456789abcdef'}
        with patch.dict(os.environ, environment, clear=True):
            config = database_config(self.args(), Path('unused.sqlite3'))
            self.assertNotIn('mysql_password', config)
            self.assertNotIn('sqlite_path', config)
            self.assertEqual(config['mysql_database'], 'isolated_test')
            self.assertEqual(config['mysql_database_id'], environment['ATLAS_DB_MYSQL_DATABASE_ID'])
            self.assertFalse(config['mysql_initialize'])
            self.assertEqual(os.environ['ATLAS_DB_MYSQL_PASSWORD'], '<test-password>')
            os.environ['ATLAS_DB_MYSQL_PORT'] = 'invalid'
            with self.assertRaisesRegex(RuntimeError, 'port must be'):
                database_config(self.args(), Path('unused.sqlite3'))

    def test_mysql_identity_is_required_and_never_inferred_from_database_name(self):
        environment = {'ATLAS_DB_MYSQL_USER': '<test-user>',
                       'ATLAS_DB_MYSQL_PASSWORD': '<test-password>',
                       'ATLAS_DB_MYSQL_DATABASE': 'isolated_test',
                       'ATLAS_DB_MYSQL_SSL_CA': '<test-ca.pem>'}
        with patch.dict(os.environ, environment, clear=True):
            with self.assertRaisesRegex(RuntimeError, 'mysql_database_id'):
                database_config(self.args(), Path('unused.sqlite3'))
            for identity in ('a' * 31, 'a' * 33, 'A' * 32, 'g' * 32):
                os.environ['ATLAS_DB_MYSQL_DATABASE_ID'] = identity
                with self.assertRaisesRegex(RuntimeError, '32 lowercase'):
                    database_config(self.args(), Path('unused.sqlite3'))

    def test_mysql_initialization_requires_explicit_valid_configuration(self):
        environment = {'ATLAS_DB_MYSQL_USER': '<test-user>',
                       'ATLAS_DB_MYSQL_PASSWORD': '<test-password>',
                       'ATLAS_DB_MYSQL_DATABASE': 'isolated_test',
                       'ATLAS_DB_MYSQL_SSL_CA': '<test-ca.pem>',
                       'ATLAS_DB_MYSQL_DATABASE_ID': '0123456789abcdef0123456789abcdef'}
        with patch.dict(os.environ, environment, clear=True):
            self.assertFalse(database_config(self.args(), Path('unused'))['mysql_initialize'])
            os.environ['ATLAS_DB_MYSQL_INITIALIZE'] = 'true'
            self.assertTrue(database_config(self.args(), Path('unused'))['mysql_initialize'])
            os.environ['ATLAS_DB_MYSQL_INITIALIZE'] = 'yes'
            with self.assertRaisesRegex(RuntimeError, 'must be true or false'):
                database_config(self.args(), Path('unused'))
            os.environ['ATLAS_DB_MYSQL_INITIALIZE'] = 'false'
            with tempfile.TemporaryDirectory() as directory:
                config_path = Path(directory) / 'config.json'
                config_path.write_text(json.dumps({'database': {'mysql_initialize': 'false'}}))
                with self.assertRaisesRegex(RuntimeError, 'JSON boolean'):
                    database_config(self.args('--database-config', str(config_path)), Path('unused'))

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


class DatabaseRecoveryToolsTest(unittest.TestCase):
    identity = '0123456789abcdef0123456789abcdef'

    def test_connection_arguments_are_shared_by_client_and_dump_without_credentials(self):
        settings = {'mysql_host': '127.0.0.1', 'mysql_port': 3306, 'mysql_user': '<test-user>',
                    'mysql_ssl_ca': '<test-ca.pem>'}
        with patch.dict(os.environ, {'ATLAS_DB_MYSQL_PASSWORD': '<test-password>'}, clear=True):
            database = recovery.Database(settings, 'mysql')
            arguments = database.arguments()
            self.assertEqual(arguments, ['--protocol=tcp', '--host=127.0.0.1', '--port=3306',
                                         '--user=<test-user>', '--default-character-set=utf8mb4',
                                         '--ssl-mode=VERIFY_IDENTITY', '--ssl-ca=<test-ca.pem>',
                                         '--tls-version=TLSv1.2,TLSv1.3'])
            self.assertNotIn('<test-password>', ' '.join(arguments))
            self.assertEqual(database.env['MYSQL_PWD'], '<test-password>')

    def test_recovery_rejects_wrong_vendor_and_client_series(self):
        for version in (b'mysql Ver 15.1 Distrib 11.8.6-MariaDB', b'mysql Ver 8.0.45 MySQL'):
            with patch.object(recovery.shutil, 'which', return_value='candidate'), \
                 patch.object(recovery.subprocess, 'run', return_value=SimpleNamespace(stdout=version)):
                with self.assertRaisesRegex(RuntimeError, 'Oracle MySQL 8.4'):
                    recovery.program('candidate', 'mysql')
        with patch.object(recovery.shutil, 'which', return_value='candidate'), \
             patch.object(recovery.subprocess, 'run', return_value=SimpleNamespace(
                 stdout=b'mysql Ver 8.4.13 MySQL Community Server - GPL')):
            self.assertEqual(recovery.program('candidate', 'mysql'), 'candidate')

    def manifest(self, directory):
        (directory / 'database.sql').write_bytes(b'CREATE TABLE isolated (id INT);')
        value = {'format': 1, 'database_id': self.identity,
                 'dump_sha256': recovery.file_digest(directory / 'database.sql'),
                 'metadata': {'schema_version': b'4'.hex(), 'blob_format_version': b'2'.hex(),
                              'entity_id_counter_initialized': b'1'.hex(),
                              'logical_database_id': self.identity.encode().hex(),
                              'storage_schema_digest': '0' * 64},
                 'tables': {name: {'rows': 0, 'sha256': '0' * 64, 'auto_increment': 0}
                            for name in recovery.TABLES}}
        (directory / 'manifest.json').write_text(json.dumps(value))
        return value

    def test_backup_manifest_rejects_damage_wrong_identity_and_missing_tables(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            expected = self.manifest(root)
            self.assertEqual(recovery.load_backup(root, self.identity), expected)
            with self.assertRaisesRegex(RuntimeError, 'Invalid'):
                recovery.load_backup(root, 'a' * 32)
            (root / 'database.sql').write_bytes(b'corrupted')
            with self.assertRaisesRegex(RuntimeError, 'Invalid'):
                recovery.load_backup(root, self.identity)
            value = self.manifest(root)
            del value['tables']['atlas_checkout_versions']
            (root / 'manifest.json').write_text(json.dumps(value))
            with self.assertRaisesRegex(RuntimeError, 'Invalid'):
                recovery.load_backup(root, self.identity)

    def test_restore_refuses_nonempty_target_before_importing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.manifest(root)
            database = Mock(settings={'mysql_database_id': self.identity})
            database.tables.return_value = {'existing_data'}
            with self.assertRaisesRegex(RuntimeError, 'empty target'):
                recovery.restore(database, root)
            database.run.assert_not_called()

    def test_restore_rejects_checksum_before_contacting_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.manifest(root)
            (root / 'database.sql').write_bytes(b'truncated')
            database = Mock(settings={'mysql_database_id': self.identity})
            with self.assertRaisesRegex(RuntimeError, 'Invalid'):
                recovery.restore(database, root)
            database.tables.assert_not_called()
            database.run.assert_not_called()

    def test_backup_never_overwrites_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            database = Mock()
            with self.assertRaisesRegex(RuntimeError, 'already exists'):
                recovery.backup(database, Path(directory), 'unused-dump')
            database.inspect.assert_not_called()

    def test_backup_rejects_changed_source_and_does_not_publish_manifest(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(recovery.subprocess, 'run', return_value=SimpleNamespace(stdout=b'mysqldump  Ver 8.4.13 MySQL Community Server - GPL')):
            root = Path(directory) / 'backup'
            database = Mock(settings={'mysql_database': 'isolated_test'})
            database.inspect.return_value = {}
            database.arguments.return_value = []
            database.fingerprint.side_effect = [{'old': {}}, {'new': {}}]
            with self.assertRaisesRegex(RuntimeError, 'changed during backup'):
                recovery.backup(database, root, 'mysqldump')
            self.assertFalse((root / 'manifest.json').exists())

    def test_recovery_requires_mysql_and_explicit_quiescence(self):
        with patch.object(recovery, 'database_config') as configure, \
             patch.object(sys, 'stderr', new_callable=io.StringIO):
            self.assertEqual(recovery.main(['backup', '--directory', 'unused']), 1)
            self.assertEqual(recovery.main(['verify', '--db-backend', 'sqlite']), 1)
            configure.assert_not_called()


if __name__ == '__main__':
    unittest.main()
