from __future__ import annotations

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from cluster_control import verify_framework_ha as verify


class FrameworkHaTest(unittest.TestCase):
    def test_diagnostics_require_registered_ready_processes_and_bounded_counts(self):
        def control(verb, target, path):
            value = '0'
            if path == 'app/ready':
                value = 'true'
            elif path == 'app/shutdown/state':
                value = 'running'
            elif path in ('app/process_instance', 'machined/registration_generation'):
                value = '1'
            elif path.endswith('_limit'):
                value = '4096'
            return target.split(':')[1] + ' ' + value

        self.assertEqual(len(verify.collect_diagnostics(control)), 9)
        for path, value in (('app/ready', 'false'), ('app/process_instance', '0'),
                            ('rpc/pending', '4097'), ('rpc/replies', '-1'),
                            ('app/shutdown/state', 'timed_out')):
            def invalid(verb, target, query):
                return target.split(':')[1] + ' ' + value if query == path else control(verb, target, query)
            with self.subTest(path=path), self.assertRaises(RuntimeError):
                verify.collect_diagnostics(invalid)
        with self.assertRaisesRegex(RuntimeError, 'Invalid diagnostic response'):
            verify.collect_diagnostics(lambda *args: 'missing')

    def test_arguments_require_mysql_and_a_build_directory_name(self):
        args = verify.parse_args(['--build', 'framework-verify', '--scenario', 'workers'])
        self.assertEqual(args.db_backend, 'mysql')
        self.assertEqual(args.scenario, 'workers')
        for values in (['--db-backend', 'sqlite'], ['--build', '../bad'],
                       ['--build', '..'], ['--scenario', 'unknown']):
            with self.subTest(values=values), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    verify.parse_args(values)

    def test_registry_names_match_both_columns_exactly(self):
        output = 'TYPE NAME ADDR PID LOAD\ncellapp cellapp-2 addr 1 0%\nbaseapp cellapp addr 2 0%'
        self.assertEqual(verify.registered_names(output, 'cellapp'), {'cellapp-2'})
        self.assertNotIn('cellapp', verify.registered_names(output, 'cellapp'))
        self.assertEqual(verify.registered_names('(no processes registered)', 'cellapp'), set())

    def test_missing_database_is_an_environment_block_before_process_creation(self):
        with patch.object(sys, 'argv', ['verify_framework_ha']), patch.dict('os.environ', {}, clear=True):
            with patch.object(verify.subprocess, 'Popen') as start:
                with self.assertRaisesRegex(RuntimeError, 'MySQL environment blocked'):
                    verify.main()
                start.assert_not_called()

    def test_missing_binary_names_the_build_target_without_starting_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(verify, 'root', Path(directory)), patch.object(verify, 'database_config'):
                with patch.object(sys, 'argv', ['verify_framework_ha']):
                    with patch.object(verify.subprocess, 'Popen') as start:
                        with self.assertRaisesRegex(RuntimeError, 'Missing binary.*machined'):
                            verify.main()
                        start.assert_not_called()


if __name__ == '__main__':
    unittest.main()
