"""Run a command against an isolated Oracle MySQL 8.4.13 TLS fixture."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import time

from common.paths import REPO_ROOT


IMAGE = ('container-registry.oracle.com/mysql/community-server:8.4.13@'
         'sha256:68d16b54ba323db45375337dc8289b4e8508f5bef51a634c92b7b76bb393df52')


def run(args: list[str], **kwargs) -> None:
    subprocess.run(args, check=True, timeout=180, **kwargs)


def certificates(openssl: str, directory: Path) -> None:
    def generate(*args):
        run([openssl, *args], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    generate('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
             '-subj', '/CN=Atlas isolated test CA', '-keyout', str(directory / 'ca.key'),
             '-out', str(directory / 'ca.pem'))
    generate('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
             '-subj', '/CN=Untrusted test CA', '-keyout', str(directory / 'untrusted-ca.key'),
             '-out', str(directory / 'untrusted-ca.pem'))
    generate('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
             '-keyout', str(directory / 'server.key'), '-out', str(directory / 'server.csr'))
    (directory / 'server.ext').write_text(
        'subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n')
    generate('x509', '-req', '-in', str(directory / 'server.csr'),
             '-CA', str(directory / 'ca.pem'), '-CAkey', str(directory / 'ca.key'),
             '-set_serial', '2', '-days', '2', '-extfile', str(directory / 'server.ext'),
             '-out', str(directory / 'server.pem'))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path,
                        help='New isolated directory; defaults to a random directory in .tmp/')
    parser.add_argument('--openssl', help='Existing OpenSSL CLI path; never installed automatically')
    parser.add_argument('--docker', action='store_true', help='Use the pinned official Oracle image')
    parser.add_argument('command', nargs=argparse.REMAINDER, help='Command after --; receives test DB environment')
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('Provide a verification command after --')
    directory = (args.directory or REPO_ROOT / '.tmp' / ('mysql-' + secrets.token_hex(8))).resolve()
    if directory.exists():
        parser.error('Fixture directory already exists; choose a new empty location')
    if args.docker and not shutil.which('docker'):
        parser.error('Docker missing; provide a working Docker CLI/daemon or use the native SDK fixture')
    openssl = args.openssl or shutil.which('openssl')
    if not openssl and os.name == 'nt':
        candidate = Path('C:/Program Files/Git/usr/bin/openssl.exe')
        openssl = str(candidate) if candidate.is_file() else None
    if not openssl:
        parser.error('OpenSSL CLI missing; specify an existing program with --openssl')
    platform = ('windows/mysql-8.4.13-winx64' if os.name == 'nt' else
                'linux/mysql-8.4.13-linux-glibc2.28-x86_64-minimal')
    sdk = REPO_ROOT / 'build/oracle-mysql/8.4.13' / platform
    suffix = '.exe' if os.name == 'nt' else ''
    client = str(sdk / 'bin' / ('mysql' + suffix))
    server = str(sdk / 'bin' / ('mysqld' + suffix))
    if not Path(client).is_file() or (not args.docker and not Path(server).is_file()):
        parser.error('Pinned SDK missing; run tools/bin/prepare_mysql first')
    directory.mkdir(parents=True, mode=0o700)
    certificates(openssl, directory)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    scratch = directory / 'scratch'
    scratch.mkdir(mode=0o700)
    password = secrets.token_hex(32)
    env = dict(os.environ, MYSQL_PWD=password, TMP=str(scratch), TEMP=str(scratch), TMPDIR=str(scratch), ATLAS_TEST_MYSQL='1',
               ATLAS_TEST_MYSQL_HOST='127.0.0.1', ATLAS_TEST_MYSQL_PORT=str(port),
               ATLAS_TEST_MYSQL_USER='root', ATLAS_TEST_MYSQL_PASSWORD=password,
               ATLAS_TEST_MYSQL_DATABASE='atlas_test', ATLAS_TEST_MYSQL_SSL_CA=str(directory / 'ca.pem'),
               ATLAS_TEST_MYSQL_UNTRUSTED_CA=str(directory / 'untrusted-ca.pem'))
    for field in ('HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE', 'SSL_CA'):
        env['ATLAS_DB_MYSQL_' + field] = env['ATLAS_TEST_MYSQL_' + field]
    env.update(ATLAS_DB_MYSQL_DATABASE_ID='0123456789abcdef0123456789abcdef',
               ATLAS_DB_MYSQL_INITIALIZE='true', ATLAS_TEST_DB_BACKEND='mysql')
    connection = [client, '--no-defaults', '--protocol=tcp', '--host=127.0.0.1',
                  f'--port={port}', '--user=root', '--ssl-mode=VERIFY_IDENTITY',
                  '--ssl-ca=' + str(directory / 'ca.pem'), '--connect-timeout=2']
    name = 'atlas-mysql-' + secrets.token_hex(8)
    process = None
    try:
        with (directory / 'server.log').open('wb') as log:
            if args.docker:
                for path in directory.iterdir():
                    if path.is_file():
                        path.chmod(0o644)
                directory.chmod(0o755)
                docker_env = dict(env, MYSQL_ROOT_PASSWORD=password)
                run(['docker', 'run', '--detach', '--name', name, '-p', f'127.0.0.1:{port}:3306',
                     '-e', 'MYSQL_ROOT_PASSWORD', '-e', 'MYSQL_ROOT_HOST=%', '-e', 'MYSQL_DATABASE=atlas_test',
                     '-v', f'{directory}:/atlas-tls:ro', IMAGE, '--require-secure-transport=ON',
                     '--ssl-ca=/atlas-tls/ca.pem', '--ssl-cert=/atlas-tls/server.pem',
                     '--ssl-key=/atlas-tls/server.key'], env=docker_env, stdout=log, stderr=log)
            else:
                data = directory / 'data'
                run([server, '--no-defaults', '--initialize-insecure', '--basedir=' + str(sdk),
                     '--datadir=' + str(data)], stdout=log, stderr=log)
                bootstrap = directory / 'bootstrap.sql'
                bootstrap.write_text(f"ALTER USER 'root'@'localhost' IDENTIFIED BY '{password}';\n"
                                     'CREATE DATABASE atlas_test;\n')
                process = subprocess.Popen([server, '--no-defaults', '--basedir=' + str(sdk),
                    '--datadir=' + str(data), '--bind-address=127.0.0.1', f'--port={port}',
                    '--mysqlx=OFF', '--require-secure-transport=ON', '--ssl-ca=' + str(directory / 'ca.pem'),
                    '--ssl-cert=' + str(directory / 'server.pem'), '--ssl-key=' + str(directory / 'server.key'),
                    '--init-file=' + str(bootstrap)], stdout=log, stderr=log)
            deadline = time.monotonic() + 90
            while True:
                ready = subprocess.run([*connection, '-N', '-e', 'SELECT VERSION()'], env=env,
                                       capture_output=True, timeout=5)
                if ready.returncode == 0:
                    break
                if (process and process.poll() is not None) or time.monotonic() >= deadline:
                    raise RuntimeError(f'Oracle fixture failed to start; inspect {directory / "server.log"}')
                time.sleep(0.5)
            if not ready.stdout.strip().startswith(b'8.4.13'):
                raise RuntimeError('Fixture server version differs from pinned Oracle version')
            if not args.docker:
                bootstrap.write_text('')
                bootstrap.unlink()
            (directory / 'run.json').write_text(json.dumps({
                'backend': 'Oracle MySQL', 'version': '8.4.13', 'dbapps': 'selected by command',
                'tls': 'VERIFY_IDENTITY', 'image': IMAGE if args.docker else None}, indent=2) + '\n')
            print(f'Oracle MySQL 8.4.13 fixture ready; TLS VERIFY_IDENTITY; report={directory}', flush=True)
            return subprocess.run(command, env=env).returncode
    finally:
        if args.docker:
            with (directory / 'server.log').open('ab') as log:
                subprocess.run(['docker', 'logs', name], stdout=log, stderr=log)
            subprocess.run(['docker', 'rm', '-fv', name], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        elif process and process.poll() is None:
            subprocess.run([*connection, '-e', 'SHUTDOWN'], env=env, capture_output=True, timeout=10)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'MySQL fixture failed: {error}', file=sys.stderr)
        raise SystemExit(1)
