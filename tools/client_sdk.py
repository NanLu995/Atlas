"""Export, verify and install a matching desktop SDK; no Godot patch version requirement."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

from common.cluster import export_entity_definitions
from common.json_io import write_json_atomic
from common.paths import REPO_ROOT
from common.provenance import require_generated_output, source_state
from build import load_msvc_env, resolve_cmake, validate_build_cache


MANIFEST = 'source-manifest.json'
ASSEMBLIES = ('Atlas.Shared.dll', 'Atlas.Client.dll', 'Atlas.ClrHost.dll',
              'Atlas.Client.Desktop.dll')


def contract():
    native = (REPO_ROOT / 'src/lib/net_client/client_api.h').read_text(encoding='utf-8')
    protocol = (REPO_ROOT / 'src/lib/network/protocol_version.h').read_text(encoding='utf-8')
    abi = re.search(r'ATLAS_NET_ABI_VERSION\s+(0x[0-9a-fA-F]+)', native)
    version = re.search(r'kConnectionProtocolVersion\s*=\s*(\d+)', protocol)
    if not abi or not version:
        raise RuntimeError('Cannot read existing ABI/protocol constants; check the client headers')
    return {'abi': abi[1].lower(), 'protocol': int(version[1])}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(directory):
    manifest = json.loads((directory / MANIFEST).read_text(encoding='utf-8'))
    if not isinstance(manifest, dict):
        raise RuntimeError('SDK manifest must be a JSON object')
    expected_contract = contract()
    if (manifest.get('format') != 1 or manifest.get('platform') != platform.system()
            or not isinstance(manifest.get('architecture'), str)
            or manifest.get('architecture', '').lower() != platform.machine().lower()
            or any(manifest.get(key) != value for key, value in expected_contract.items())):
        raise RuntimeError('SDK format/platform/ABI/protocol mismatch; export from this repository again')
    files = manifest['sha256']
    if not isinstance(files, dict) or not files:
        raise RuntimeError('SDK manifest requires a nonempty file/hash mapping')
    for name, expected in files.items():
        if (not isinstance(name, str) or not isinstance(expected, str) or len(expected) != 64
                or any(character not in '0123456789abcdef' for character in expected)):
            raise RuntimeError('SDK manifest contains an invalid filename or SHA-256')
        path = directory / name
        if (Path(name).is_absolute() or not path.resolve().is_relative_to(directory.resolve())
                or path.is_symlink() or not path.is_file() or digest(path) != expected):
            raise RuntimeError(f'SDK file missing, unsafe or altered: {name}; export again')
    native = 'atlas_net_client.dll' if os.name == 'nt' else 'libatlas_net_client.so'
    for name in (*ASSEMBLIES, native):
        if name not in files:
            raise RuntimeError(f'SDK missing assembly: {name}')
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file()}
    if actual != set(files) | {MANIFEST}:
        raise RuntimeError('SDK contains unrecorded files; use a fresh export directory')
    return manifest


def export(args):
    require_generated_output(REPO_ROOT, args.output)
    if platform.system() not in ('Windows', 'Linux') or platform.machine().lower() not in (
            'amd64', 'x86_64'):
        raise RuntimeError('Desktop SDK export currently supports Windows/Linux x86-64')
    build = args.build_dir.resolve()
    if build.parent != (REPO_ROOT / 'build').resolve():
        raise RuntimeError('--build-dir must be directly under the repository build/')
    if args.output.exists():
        raise RuntimeError('Export directory exists; select a new --output directory')
    cache_path = build / 'CMakeCache.txt'
    if not cache_path.is_file():
        raise RuntimeError('Build not configured; run tools/bin/build with --build-dir first')
    cache = dict(line.split('=', 1) for line in cache_path.read_text(encoding='utf-8').splitlines()
                 if '=' in line and not line.startswith(('#', '//')))
    config = cache.get('CMAKE_BUILD_TYPE:UNINITIALIZED', cache.get('CMAKE_BUILD_TYPE:STRING'))
    if config not in ('Debug', 'Release'):
        raise RuntimeError('Desktop SDK export requires a Debug or Release build directory')
    env = load_msvc_env()[0] if os.name == 'nt' else dict(os.environ)
    env.setdefault('CMAKE_BUILD_PARALLEL_LEVEL', '2')
    validate_build_cache(build, env)
    cmake = resolve_cmake(env)
    subprocess.run([cmake, '-S', str(REPO_ROOT), '-B', str(build),
                    '-DATLAS_BUILD_NET_CLIENT=ON'], env=env, check=True)
    targets = ['atlas_net_client', 'atlas_shared_dll_deploy', 'atlas_client_dll_deploy',
               'atlas_clrhost_dll_deploy', 'atlas_client_desktop_dll_deploy', 'atlas_tools_defdump']
    if args.fixture:
        targets += ['atlas_rpc_fixture_base_dll_deploy', 'atlas_rpc_fixture_cell_dll_deploy',
                    'atlas_rpc_fixture_client_dll_deploy']
    subprocess.run([cmake, '--build', str(build), '--config', config, '--target', *targets],
                   env=env, check=True)
    binary = REPO_ROOT / 'bin' / build.name
    args.output.mkdir(parents=True)
    names = list(ASSEMBLIES)
    names.append('atlas_net_client.dll' if os.name == 'nt' else 'libatlas_net_client.so')
    names += [p.name for pattern in ('*mimalloc*.dll', '*mimalloc*.so*', 'TracyClient.dll',
                                     'libTracyClient.so*') for p in binary.glob(pattern)]
    if args.client_assembly:
        shutil.copy2(args.client_assembly, args.output / args.client_assembly.name)
    if args.fixture:
        names.append('Atlas.RpcFixture.Client.dll')
    for name in names:
        shutil.copy2(binary / name, args.output / name)
    definition = export_entity_definitions(REPO_ROOT, build.name, args.base_assembly.resolve(),
                                          args.output / 'entity_defs.bin')
    clients = [args.client_assembly] if args.client_assembly else []
    if args.fixture:
        clients.append(binary / 'Atlas.RpcFixture.Client.dll')
    for client in clients:
        result = subprocess.run(['dotnet', str(build / 'csharp/Atlas.Tools.DefDump.dll'),
                                 '--digest-only', str(client.resolve())], env=env,
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f'Client definition digest missing: {client}; rebuild with the current '
                               '.def generator')
        if result.stdout.strip() != f'DefDump: digest={definition}':
            raise RuntimeError(f'Client/server entity definitions differ: {client}; build them together')
    shutil.copy2(REPO_ROOT / 'LICENSE', args.output / 'Atlas.LICENSE')
    for dependency, filename in (('mimalloc', 'LICENSE'), ('tracy', 'LICENSE'), ('zlib', 'LICENSE'),
                                  ('jolt', 'LICENSE'), ('recastnavigation', 'License.txt'),
                                  ('pugixml', 'LICENSE.md'), ('rapidjson', 'license.txt')):
        source = Path(cache.get('FETCHCONTENT_SOURCE_DIR_' + dependency.upper() + ':PATH', '')
                      or build / '_deps' / (dependency + '-src'))
        if source.is_dir():
            shutil.copy2(source / filename, args.output / (dependency + '.LICENSE'))
    for source, name in [('LICENSE.txt', 'OpenSSL.LICENSE'), ('AUTHORS.md', 'OpenSSL.AUTHORS.md'),
                          ('atlas-source.json', 'OpenSSL.source.json')]:
        shutil.copy2(binary / 'licenses/OpenSSL' / source, args.output / name)
    manifest = {'format': 1, 'platform': platform.system(), 'architecture': platform.machine(),
                'build': build.name, 'config': config, 'entity_definition_sha256': definition,
                'sha256': {}, 'source': str(REPO_ROOT), 'fixture': args.fixture}
    manifest.update(contract())
    for command, field in [(['git', 'rev-parse', 'HEAD'], 'commit'),
                           (['git', 'status', '--porcelain'], 'working_tree')]:
        manifest[field] = subprocess.check_output(command, cwd=REPO_ROOT, text=True).strip()
    manifest['toolchain'] = {key: value for key, value in cache.items()
                             if key.startswith(('CMAKE_CXX_COMPILER:', 'CMAKE_GENERATOR:',
                                                'CMAKE_GENERATOR_INSTANCE:', 'DOTNET_'))}
    manifest.update(source_state(REPO_ROOT))
    manifest['sha256'] = {p.name: digest(p) for p in args.output.iterdir() if p.is_file()}
    write_json_atomic(args.output / MANIFEST, manifest)
    verify(args.output)


def install(bundle, project):
    manifest = verify(bundle)
    if not (project / 'project.godot').is_file():
        raise RuntimeError('Godot project.godot missing; select --project <project directory>')
    target = project / 'addons/atlas_client'
    if target.is_symlink() or not target.resolve().is_relative_to(project.resolve()):
        raise RuntimeError('SDK target must remain inside the project')
    target.mkdir(parents=True, exist_ok=True)
    stale = [p for p in target.iterdir()
             if (p.suffix in ('.dll', '.so', '.dylib') or '.so.' in p.name)
             and p.name not in manifest['sha256']]
    if stale:
        raise RuntimeError('Obsolete SDK files require explicit removal approval: '
                           + ', '.join(str(p) for p in stale))
    backup = Path(tempfile.mkdtemp(prefix='atlas-sdk-backup-'))
    print(f'Previous SDK backup: {backup}')
    copied = []
    try:
        for name in [*manifest['sha256'], MANIFEST]:
            path = target / name
            if path.is_symlink() or not path.resolve().is_relative_to(target.resolve()):
                raise RuntimeError(f'Unsafe install destination: {name}')
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.exists():
                (backup / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, backup / name)
            copied.append(name)
            shutil.copy2(bundle / name, path)
    except (OSError, RuntimeError):
        for name in reversed(copied):
            if (backup / name).exists():
                shutil.copy2(backup / name, target / name)
            else:
                (target / name).unlink(missing_ok=True)
        raise
    print(f'Installed matching SDK in {target}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    exporter = commands.add_parser('export', help='Build and export to a new directory')
    exporter.add_argument('--build-dir', type=Path, default=REPO_ROOT / 'build/debug')
    exporter.add_argument('--output', type=Path, required=True)
    exporter.add_argument('--base-assembly', type=Path, required=True)
    exporter.add_argument('--client-assembly', type=Path)
    exporter.add_argument('--fixture', action='store_true', help='Include existing RPC verification fixture')
    for command in ('verify', 'install'):
        sub = commands.add_parser(command)
        sub.add_argument('--bundle', type=Path, required=True)
        if command == 'install':
            sub.add_argument('--project', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'export':
        export(args)
    elif args.command == 'verify':
        verify(args.bundle)
        print('SDK hashes and platform verified')
    else:
        install(args.bundle, args.project)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'SDK failed: {error}', file=sys.stderr)
        raise SystemExit(1)
