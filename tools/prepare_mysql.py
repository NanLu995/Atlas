"""Prepare the pinned Oracle MySQL SDK and matching source archive in build/."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import sys
import tarfile
import urllib.request
import zipfile

from common.paths import REPO_ROOT


VERSION = '8.4.13'
ARTIFACTS = {
    'windows': ('mysql-8.4.13-winx64.zip',
                'e9cd98b1d23f122987b1afca26ad942efec5891481103d48ae3cde237fa3ea54'),
    'linux': ('mysql-8.4.13-linux-glibc2.28-x86_64-minimal.tar.xz',
              '5dfbb764f731df40ef9e4f5d69aed32bdac8ba1af20bd96b7d40a942d83d4ed2'),
    'source': ('mysql-8.4.13.tar.gz',
               'f6ddc7f7fb6462bab8b533b13c06af3fff57fa9ca12259b81ab13cd9930b9d58'),
}


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            checksum.update(chunk)
    return checksum.hexdigest()


def archive(kind: str, cache: Path) -> Path:
    name, expected = ARTIFACTS[kind]
    path = cache / name
    if not path.exists():
        partial = cache / (name + '.partial')
        with urllib.request.urlopen('https://cdn.mysql.com/Downloads/MySQL-8.4/' + name,
                                    timeout=60) as source, partial.open('wb') as output:
            while chunk := source.read(1024 * 1024):
                output.write(chunk)
        if digest(partial) != expected:
            raise RuntimeError(f'Oracle MySQL checksum mismatch: {partial}; do not use this archive')
        partial.replace(path)
    if digest(path) != expected:
        raise RuntimeError(f'Oracle MySQL checksum mismatch: {path}; obtain the pinned official archive')
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, help='Existing pinned archive cache; defaults inside build/')
    args = parser.parse_args(argv)
    system = platform.system().lower()
    if system not in ('windows', 'linux') or platform.machine().lower() not in ('amd64', 'x86_64'):
        parser.error('The pinned Oracle SDK supports Windows/Linux x86-64 only')
    root = REPO_ROOT / 'build' / 'oracle-mysql' / VERSION
    cache = args.cache or root / 'downloads'
    cache.mkdir(parents=True, exist_ok=True)
    binary = archive(system, cache)
    source = archive('source', cache)
    prefix = root / system
    prefix.mkdir(parents=True, exist_ok=True)
    directory = binary.name.removesuffix('.zip').removesuffix('.tar.xz')
    sdk = prefix / directory
    if not (sdk / '.atlas-verified.json').exists():
        if system == 'windows':
            with zipfile.ZipFile(binary) as package:
                for member in package.infolist():
                    target = (prefix / member.filename).resolve()
                    if not target.is_relative_to(prefix.resolve()):
                        raise RuntimeError('Invalid Oracle SDK archive member path')
                package.extractall(prefix)
        else:
            with tarfile.open(binary) as package:
                package.extractall(prefix, filter='data')
    for name in ('LICENSE', 'include/mysql.h', 'include/mysql_version.h'):
        if not (sdk / name).is_file():
            raise RuntimeError(f'Missing Oracle SDK file {sdk / name}; inspect the isolated SDK')
    (sdk / '.atlas-verified.json').write_text(json.dumps({
        'version': VERSION, 'sha256': ARTIFACTS[system][1],
        'source_url': 'https://cdn.mysql.com/Downloads/MySQL-8.4/' + source.name,
        'source_sha256': ARTIFACTS['source'][1], 'source_archive': str(source.resolve()),
        'license_route': 'GPLv2 with Universal FOSS Exception; complete-source FOSS delivery only',
    }, indent=2) + '\n', encoding='utf-8')
    print(f'Oracle MySQL {VERSION} SDK: {sdk}')
    print(f'Corresponding MySQL source archive: {source}; include source and licenses with FOSS delivery')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f'Oracle MySQL preparation failed: {error}', file=sys.stderr)
        raise SystemExit(1)
