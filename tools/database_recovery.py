from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile

from common.database import add_database_options, database_config
from common.json_io import write_json_atomic
from common.paths import REPO_ROOT


TABLES = ('entities', 'meta', 'atlas_entity_id_counter', 'atlas_write_receipts',
          'atlas_checkout_versions', 'atlas_checkout_processes', 'atlas_account_sessions')


def file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def validate_metadata(metadata: dict, identity: str) -> None:
    expected = {'schema_version': b'4'.hex(), 'blob_format_version': b'2'.hex(),
                'entity_id_counter_initialized': b'1'.hex(),
                'logical_database_id': identity.encode().hex()}
    if any(metadata.get(key) != value for key, value in expected.items()):
        raise RuntimeError('Missing or mismatched database identity/format metadata')
    digest = metadata.get('storage_schema_digest', '')
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
        raise RuntimeError('Missing or malformed storage_schema_digest')


def program(value: str | None, name: str) -> str:
    platform = 'windows/mysql-8.4.13-winx64' if os.name == 'nt' else (
        'linux/mysql-8.4.13-linux-glibc2.28-x86_64-minimal')
    pinned = REPO_ROOT / 'build/oracle-mysql/8.4.13' / platform / 'bin' / name
    resolved = shutil.which(value or str(pinned)) or (None if value else shutil.which(name))
    if not resolved:
        raise RuntimeError(f'Missing Oracle {name} 8.4; run prepare_mysql or pass its path explicitly')
    version = subprocess.run([resolved, '--no-defaults', '--version'], check=True,
                             capture_output=True, timeout=10).stdout.lower()
    if b'mariadb' in version or not re.search(rb'\b8\.4\.\d+\b', version) or b'mysql' not in version:
        raise RuntimeError(f'{name} must be Oracle MySQL 8.4; MariaDB and other versions are unsupported')
    return resolved


class Database:
    def __init__(self, settings: dict, client: str):
        self.settings = settings
        self.client = client
        self.env = dict(os.environ, MYSQL_PWD=os.environ['ATLAS_DB_MYSQL_PASSWORD'])

    def arguments(self) -> list[str]:
        config = self.settings
        return ['--protocol=tcp', '--host=' + config['mysql_host'],
                '--port=' + str(config['mysql_port']), '--user=' + config['mysql_user'],
                '--default-character-set=utf8mb4', '--ssl-mode=VERIFY_IDENTITY',
                '--ssl-ca=' + config['mysql_ssl_ca'], '--tls-version=TLSv1.2,TLSv1.3']

    def run(self, command: list[str], **kwargs) -> None:
        result = subprocess.run(command, env=self.env, stderr=subprocess.PIPE,
                                timeout=600, **kwargs)
        if result.returncode:
            raise RuntimeError(f'{Path(command[0]).name} failed (exit {result.returncode}); '
                               'check tool arguments with --help, connectivity, credentials and permissions; '
                               'no fallback or cleanup of partial restored data was attempted')

    def query(self, sql: str) -> list[list[str]]:
        with tempfile.TemporaryFile() as output:
            self.run([self.client, '--no-defaults', *self.arguments(), '--batch', '--raw', '--skip-column-names',
                      '--database=' + self.settings['mysql_database']],
                     input=sql.encode(), stdout=output)
            output.seek(0)
            return [line.decode('ascii').rstrip('\r\n').split('\t') for line in output]

    def tables(self) -> set[str]:
        return {bytes.fromhex(row[0]).decode() for row in self.query(
            'SELECT HEX(TABLE_NAME) FROM information_schema.TABLES '
            'WHERE TABLE_SCHEMA=DATABASE()')}

    def has_stored_programs(self) -> bool:
        counts = self.query(
            'SELECT COUNT(*) FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA=DATABASE(); '
            'SELECT COUNT(*) FROM information_schema.EVENTS WHERE EVENT_SCHEMA=DATABASE()')
        return counts != [['0'], ['0']]

    def inspect(self) -> dict:
        if self.tables() != set(TABLES) or self.has_stored_programs():
            raise RuntimeError('Database must contain exactly the seven Atlas tables and no stored programs')
        metadata = {bytes.fromhex(key).decode(): value.lower() for key, value in self.query(
            'SELECT HEX(`key`), HEX(`value`) FROM meta')}
        validate_metadata(metadata, self.settings['mysql_database_id'])
        invalid = self.query(
            'SELECT COUNT(*) FROM atlas_entity_id_counter WHERE id=1 AND next_id '
            'BETWEEN 1 AND 4294967296; '
            'SELECT COUNT(*) FROM entities e LEFT JOIN atlas_checkout_versions v '
            'ON e.dbid=v.dbid AND e.type_id=v.type_id WHERE e.checked_out=1 '
            'AND (v.version IS NULL OR v.version<e.checkout_version); '
            'SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() '
            "AND (TABLE_TYPE<>'BASE TABLE' OR ENGINE<>'InnoDB'); "
            'SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE TRIGGER_SCHEMA=DATABASE()')
        if invalid != [['1'], ['0'], ['0'], ['0']]:
            raise RuntimeError('Invalid ID counter, checkout versions, table engine or triggers')
        return metadata

    def fingerprint(self) -> dict:
        statements = ['START TRANSACTION WITH CONSISTENT SNAPSHOT']
        for table in TABLES:
            columns = self.query(
                'SELECT HEX(COLUMN_NAME) FROM information_schema.COLUMNS '
                f"WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='{table}' ORDER BY ORDINAL_POSITION")
            names = ['`' + bytes.fromhex(row[0]).decode().replace('`', '``') + '`'
                     for row in columns]
            encoded = ','.join(f"IFNULL(HEX({name}),'NULL')" for name in names)
            statements.append(f"SELECT '{table}',{encoded} FROM `{table}` ORDER BY " +
                              ','.join(str(index) for index in range(2, len(names) + 2)))
        statements.append('COMMIT')
        results = {name: {'rows': 0, 'sha256': hashlib.sha256()} for name in TABLES}
        with tempfile.TemporaryFile() as output:
            self.run([self.client, '--no-defaults', *self.arguments(), '--batch', '--raw', '--skip-column-names',
                      '--database=' + self.settings['mysql_database']],
                     input=(';'.join(statements) + ';').encode(), stdout=output)
            output.seek(0)
            for line in output:
                table, payload = line.rstrip(b'\r\n').split(b'\t', 1)
                state = results[table.decode()]
                state['rows'] += 1
                state['sha256'].update(payload + b'\n')
        auto_increment = {bytes.fromhex(name).decode(): int(value) for name, value in self.query(
            'SELECT HEX(TABLE_NAME), COALESCE(AUTO_INCREMENT,0) FROM information_schema.TABLES '
            'WHERE TABLE_SCHEMA=DATABASE()')}
        return {name: dict(rows=state['rows'], sha256=state['sha256'].hexdigest(),
                           auto_increment=auto_increment[name])
                for name, state in results.items()}


def load_backup(directory: Path, identity: str) -> dict:
    try:
        manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
        if not isinstance(manifest, dict) or not isinstance(manifest.get('tables'), dict):
            raise ValueError('manifest')
        if manifest['format'] != 1 or manifest['database_id'] != identity:
            raise ValueError('identity/format')
        if set(manifest['tables']) != set(TABLES):
            raise ValueError('tables')
        if not isinstance(manifest['metadata'], dict):
            raise ValueError('metadata')
        validate_metadata(manifest['metadata'], identity)
        for table in manifest['tables'].values():
            if type(table['rows']) is not int or table['rows'] < 0:
                raise ValueError('row count')
            if len(table['sha256']) != 64 or any(c not in '0123456789abcdef' for c in table['sha256']):
                raise ValueError('table checksum')
            if type(table['auto_increment']) is not int or table['auto_increment'] < 0:
                raise ValueError('auto_increment')
        if file_digest(directory / 'database.sql') != manifest['dump_sha256']:
            raise ValueError('checksum')
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise RuntimeError('Invalid, incomplete or mismatched backup; restore was not started') from error
    return manifest


def backup(database: Database, directory: Path, dump: str) -> None:
    if directory.exists():
        raise RuntimeError('Backup output already exists; choose a new directory')
    metadata = database.inspect()
    before = database.fingerprint()
    directory.mkdir(parents=True, mode=0o700)
    command = [dump, '--no-defaults', *database.arguments(), '--single-transaction', '--quick', '--hex-blob',
               '--skip-comments', '--skip-lock-tables', '--skip-add-locks',
               '--skip-add-drop-table', '--skip-triggers']
    command += ['--set-gtid-purged=OFF', '--no-tablespaces']
    command += [database.settings['mysql_database'], *TABLES]
    with (directory / 'database.sql').open('xb') as output:
        database.run(command, stdout=output)
    if database.fingerprint() != before or database.inspect() != metadata:
        raise RuntimeError('Database changed during backup; stop all writers and use a new directory')
    write_json_atomic(directory / 'manifest.json', {
        'format': 1, 'database_id': database.settings['mysql_database_id'],
        'metadata': metadata, 'tables': before,
        'dump_sha256': file_digest(directory / 'database.sql')})


def restore(database: Database, directory: Path) -> None:
    manifest = load_backup(directory, database.settings['mysql_database_id'])
    if database.tables() or database.has_stored_programs():
        raise RuntimeError('Restore requires an existing empty target database; nothing was imported')
    with (directory / 'database.sql').open('rb') as source:
        database.run([database.client, '--no-defaults', *database.arguments(), '--binary-mode', '--skip-reconnect',
                      '--database=' + database.settings['mysql_database']],
                     stdin=source, stdout=subprocess.DEVNULL)
    if database.inspect() != manifest['metadata'] or database.fingerprint() != manifest['tables']:
        raise RuntimeError('Restored state does not match backup; keep target isolated and inspect it')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='Atlas development database backup and recovery')
    parser.add_argument('action', choices=('new-id', 'backup', 'restore', 'verify'))
    add_database_options(parser)
    parser.add_argument('--client', help='Path to the matching Oracle mysql 8.4 client')
    parser.add_argument('--dump-tool', help='Path to the matching Oracle mysqldump 8.4')
    parser.add_argument('--directory', type=Path, help='New backup directory, or trusted backup to read')
    parser.add_argument('--quiesced', action='store_true',
                        help='Assert all DBApps and other writers are stopped; required for backup/restore')
    args = parser.parse_args(argv)
    try:
        if args.action == 'new-id':
            print(secrets.token_hex(16))
            return 0
        if args.db_backend != 'mysql':
            raise RuntimeError('Recovery tool requires Oracle MySQL 8.4; SQLite is single-DBApp smoke only')
        if args.action != 'verify' and (not args.quiesced or args.directory is None):
            raise RuntimeError('backup/restore require --directory and --quiesced; stop all writers first')
        settings = database_config(args, Path('unused.sqlite3'))
        database = Database(settings, program(args.client, 'mysql'))
        if args.action == 'backup':
            backup(database, args.directory, program(args.dump_tool, 'mysqldump'))
        elif args.action == 'restore':
            restore(database, args.directory)
        else:
            metadata, tables = database.inspect(), database.fingerprint()
            if args.directory:
                manifest = load_backup(args.directory, settings['mysql_database_id'])
                if metadata != manifest['metadata'] or tables != manifest['tables']:
                    raise RuntimeError('Database differs from the backup; target is not verified')
            print(json.dumps({'database_id': settings['mysql_database_id'], 'tables': tables}))
        print(f'{args.action}: confirmed; backend=mysql')
        return 0
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'Database recovery failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
