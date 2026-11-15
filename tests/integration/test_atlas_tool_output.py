from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from cluster_control.test_cluster import _pick_free_port_base
from common.cluster import start_logged_process, stop_logged_processes, wait_for_registration


class AtlasToolOutputTest(unittest.TestCase):
    bin_dir: Path

    @classmethod
    def setUpClass(cls):
        suffix = '.exe' if os.name == 'nt' else ''
        cls.tool = cls.bin_dir / ('atlas_tool' + suffix)
        programs = [cls.bin_dir / (name + suffix) for name in ('machined', 'atlas_baseappmgr')]
        for path in [cls.tool, *programs]:
            if not path.is_file():
                raise RuntimeError(f'Missing {path}; build atlas_tool, machined and atlas_baseappmgr')
        temporary = tempfile.TemporaryDirectory(prefix='atlas-tool-output-')
        cls.addClassCleanup(temporary.cleanup)
        cls.scratch = Path(temporary.name)
        cls.processes = []
        cls.addClassCleanup(stop_logged_processes, cls.processes)
        port = _pick_free_port_base()
        cls.address = f'127.0.0.1:{port}'
        for index, (path, name, extra) in enumerate([
            (programs[0], 'machined', ['--internal-port', str(port)]),
            (programs[1], 'baseappmgr', ['--internal-port', str(port + 3),
                                        '--machined', cls.address]),
        ], start=1):
            process = start_logged_process(name=name, file_path=path, working_directory=ROOT,
                log_directory=cls.scratch, arguments=['--type', name, '--name', name, *extra])
            process.start_order = index
            cls.processes.append(process)
        if not wait_for_registration(atlas_tool=cls.tool, machined_address=cls.address,
                proc_type='baseappmgr', name='baseappmgr'):
            errors = '\n'.join(path.read_text(errors='replace')
                               for path in cls.scratch.glob('*.stderr.log'))
            raise RuntimeError(f'BaseAppMgr did not register within 15 seconds: {errors}')

    def watch(self, path, stdout=subprocess.PIPE):
        return subprocess.run([str(self.tool), '--machined', self.address, 'watch',
            'baseappmgr:baseappmgr', path], cwd=ROOT, stdout=stdout, stderr=subprocess.PIPE,
            text=True, timeout=10)

    def test_success_returns_complete_response(self):
        result = self.watch('baseappmgr/baseapp_count')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), ['baseappmgr', '0'])

    def test_unwritable_stdout_fails_without_changing_output_file(self):
        path = self.scratch / 'read-only-stdout.bin'
        path.write_bytes(b'unchanged')
        with path.open('rb') as output:
            result = self.watch('baseappmgr/baseapp_count', stdout=output)
        self.assertNotEqual(result.returncode, 0, 'stdout failure falsely reported success')
        self.assertIn('watch: stdout write failed', result.stderr)
        self.assertEqual(path.read_bytes(), b'unchanged')

    def test_missing_value_remains_an_error(self):
        result = self.watch('missing/watcher')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('watch: no value', result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Verify Watcher output and stdout failure handling.')
    parser.add_argument('--bin-dir', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    AtlasToolOutputTest.bin_dir = args.bin_dir.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
