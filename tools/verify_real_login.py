"""Require one real MySQL login test to execute and pass."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from common.paths import REPO_ROOT, bin_dir


TEST_NAME = ('Atlas.Client.IntegrationTests.AtlasClientRealTests.'
             'ConnectAsync_AgainstRealCluster_ReachesConnected')


def verify_report(path: Path) -> None:
    results = ET.parse(path).getroot().findall('.//{*}UnitTestResult')
    if (len(results) != 1 or results[0].get('testName') != TEST_NAME
            or results[0].get('outcome') != 'Passed'):
        raise RuntimeError(f'Real login did not execute and pass exactly once; inspect {path}')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', default=Path(os.environ.get('ATLAS_BIN_DIR', 'debug')).name,
                        help='Build directory name under bin/; defaults to ATLAS_BIN_DIR or debug')
    args = parser.parse_args(argv)
    if not args.build or args.build in ('.', '..') or Path(args.build).name != args.build:
        parser.error('--build must be a single directory name under bin/')
    if os.environ.get('ATLAS_TEST_MYSQL') != '1':
        parser.error('MySQL environment blocked; run through tools/bin/test_mysql')
    binary = bin_dir(args.build)
    if not binary.is_dir():
        parser.error(f'Missing {binary}; build the selected configuration first')
    assembly = Path(os.environ.get('ATLAS_TEST_BASE_ASSEMBLY',
                                   str(binary / 'Atlas.RpcFixture.Base.dll')))
    if not assembly.is_file():
        parser.error(f'Missing real-login entity assembly {assembly}; build the selected '
                     'configuration or set ATLAS_TEST_BASE_ASSEMBLY')
    scratch = REPO_ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    report_dir = Path(tempfile.mkdtemp(prefix='real-login-', dir=scratch))
    print(f'Real MySQL login report: {report_dir}', flush=True)
    result = subprocess.run([
        'dotnet', 'test', 'tests/csharp/Atlas.Client.IntegrationTests', '--nologo',
        '--filter', 'FullyQualifiedName=' + TEST_NAME, '--results-directory', str(report_dir),
        '--logger', 'trx;LogFileName=real-login.trx',
    ], cwd=REPO_ROOT, env=dict(os.environ, ATLAS_BIN_DIR=str(binary),
                             ATLAS_TEST_BASE_ASSEMBLY=str(assembly.resolve())))
    if result.returncode:
        return result.returncode
    verify_report(report_dir / 'real-login.trx')
    print('PASS real MySQL login: one executed, one passed, zero skipped', flush=True)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ET.ParseError) as error:
        print(f'Real login verification failed: {error}', file=sys.stderr)
        sys.exit(1)
