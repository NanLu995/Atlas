from __future__ import annotations

import argparse
import json
import os
import time
from pathlib import Path

from common.json_io import write_json_atomic


def add_database_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument('--db-backend', choices=('mysql', 'sqlite'), default='mysql',
                        help='Oracle MySQL 8.4 entity persistence; sqlite is rejected')
    parser.add_argument('--database-config', type=Path,
                        help='Existing JSON configuration containing a database object')


def database_config(args: argparse.Namespace) -> dict[str, object]:
    if args.db_backend == 'sqlite':
        raise RuntimeError('SQLite entity persistence is no longer supported; use Oracle MySQL 8.4')
    settings: dict[str, object] = {}
    if args.database_config:
        try:
            settings = json.loads(args.database_config.read_text(encoding='utf-8'))['database']
        except (OSError, ValueError, KeyError, TypeError):
            raise RuntimeError('Cannot read database configuration; provide JSON with a database object') from None
        if isinstance(settings, dict) and settings.get('type') == 'sqlite':
            raise RuntimeError('SQLite entity persistence is no longer supported; use Oracle MySQL 8.4')
        if not isinstance(settings, dict) or settings.get('type', args.db_backend) != args.db_backend:
            raise RuntimeError('--database-config backend must match --db-backend')
        settings = dict(settings)
    if any(key.startswith('sqlite_') for key in settings):
        raise RuntimeError('Removed SQLite entity persistence settings are not supported')
    settings['type'] = args.db_backend
    for field in ('host', 'port', 'user', 'password', 'database', 'database_id', 'pool_size', 'ssl_ca'):
        key = 'mysql_' + field
        value = os.environ.get('ATLAS_DB_MYSQL_' + field.upper())
        if value is not None and key not in settings:
            settings[key] = value
    settings.setdefault('mysql_host', '127.0.0.1')
    settings.setdefault('mysql_port', 3306)
    settings.setdefault('mysql_pool_size', 4)
    missing = [key for key in ('mysql_host', 'mysql_user', 'mysql_password', 'mysql_database', 'mysql_database_id', 'mysql_ssl_ca')
               if not isinstance(settings.get(key), str) or not settings[key].strip()]
    if missing:
        raise RuntimeError('MySQL environment blocked: missing ' + ', '.join(missing) +
                           '; set ATLAS_DB_MYSQL_* or --database-config; no SQLite fallback')
    try:
        port = int(settings['mysql_port'])
        pool = int(settings['mysql_pool_size'])
        if not 1 <= port <= 65535 or pool <= 0:
            raise ValueError
    except (TypeError, ValueError):
        raise RuntimeError('MySQL port must be 1..65535 and pool size must be positive') from None
    identity = settings['mysql_database_id']
    if len(identity) != 32 or any(c not in '0123456789abcdef' for c in identity):
        raise RuntimeError('mysql_database_id requires 32 lowercase hexadecimal characters')
    if 'mysql_initialize' not in settings:
        initialize = os.environ.get('ATLAS_DB_MYSQL_INITIALIZE', 'false')
        if initialize not in ('true', 'false'):
            raise RuntimeError('ATLAS_DB_MYSQL_INITIALIZE must be true or false')
        settings['mysql_initialize'] = initialize == 'true'
    if type(settings['mysql_initialize']) is not bool:
        raise RuntimeError('mysql_initialize must be a JSON boolean')
    settings['mysql_port'], settings['mysql_pool_size'] = port, pool
    os.environ['ATLAS_DB_MYSQL_PASSWORD'] = str(settings.pop('mysql_password'))
    return settings


def record_database_run(run_root: Path, settings: dict[str, object], *,
                        baseapps: int = 1, cellapps: int = 0) -> None:
    backend = settings['type']
    topology = {'dbapps': 1, 'baseapps': baseapps, 'cellapps': cellapps}
    write_json_atomic(run_root / 'database-run.json', {
        'backend': backend, 'topology': topology,
        'scope': 'mysql_validation',
    })
    print(f'Database backend={backend} topology={topology}; manifest={run_root / "database-run.json"}',
          flush=True)


def wait_for_database_start(process, log_dir: Path, timeout: float = 60) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'Database startup failed; no backend fallback; check {log_dir}')
        path = log_dir / 'dbapp.stdout.log'
        if path.exists() and 'DBApp: initialised (backend=' in path.read_text(
                encoding='utf-8', errors='replace'):
            return
        time.sleep(0.1)
    raise RuntimeError(f'Database startup timed out; no backend fallback; check {log_dir}')
