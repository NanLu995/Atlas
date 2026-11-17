"""Validate a Release build and package matching server, SDK, source and license materials."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

from build import load_msvc_env, resolve_cmake
from client_sdk import digest, verify
from common.paths import REPO_ROOT
from common.cluster import export_entity_definitions
from common.provenance import require_generated_output, source_state


def engineering_source(path):
    return path.suffix.lower() in (
        '.cc', '.cpp', '.c', '.h', '.hpp', '.cs', '.csproj', '.cmake', '.py', '.sh', '.bat',
        '.json', '.xml', '.def', '.props', '.targets', '.in', '.txt', '.md', '.yml', '.yaml',
        '.sln', '.toml', '.cfg', '.ps1', '.config', '.ini', '.inl', '.ipp', '.asm', '.s',
        '.rc', '.cmakein', '.included', '.map', '.lds', '.exports', '.natvis') \
        or path.name.upper().startswith(('LICENSE', 'NOTICE', 'COPYING')) \
        or path.name in ('.clang-format', '.editorconfig', '.gitignore', '.gitattributes')


def cache(build):
    return dict(line.split('=', 1) for line in (build / 'CMakeCache.txt').read_text(
        encoding='utf-8').splitlines() if '=' in line and not line.startswith(('#', '//')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--verification-build', type=Path, required=True,
                        help='Release build with tests enabled, from the same current source')
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--base-assembly', type=Path, required=True)
    parser.add_argument('--cell-assembly', type=Path)
    parser.add_argument('--output', type=Path, required=True, help='New zip file')
    args = parser.parse_args()
    if os.environ.get('ATLAS_TEST_MYSQL') != '1':
        parser.error('Run through tools/bin/test_mysql to validate against isolated MySQL')
    require_generated_output(REPO_ROOT, args.output)
    manifest = verify(args.sdk)
    if args.output.exists():
        parser.error('--output exists; choose a new delivery filename')
    for build in (args.build_dir, args.verification_build):
        values = cache(build)
        if values.get('CMAKE_BUILD_TYPE:UNINITIALIZED', values.get('CMAKE_BUILD_TYPE:STRING')) != 'Release':
            parser.error('Both build directories must select Release')
    if cache(args.verification_build).get('ATLAS_BUILD_TESTS:BOOL') != 'ON':
        parser.error('Verification build must enable ATLAS_BUILD_TESTS=ON; old test outputs are insufficient')
    if manifest['config'] != 'Release' or manifest['build'] != args.build_dir.name:
        parser.error('SDK must come from the selected Release build')
    if any(manifest.get(key) != value for key, value in source_state(REPO_ROOT).items()):
        parser.error('Source changed since SDK export; build and export again')
    env = load_msvc_env()[0] if sys.platform == 'win32' else dict(os.environ)
    env.setdefault('CMAKE_BUILD_PARALLEL_LEVEL', '2')
    cmake = resolve_cmake(env)
    ctest = str(Path(cmake).with_name('ctest.exe' if sys.platform == 'win32' else 'ctest'))
    subprocess.run([cmake, '--build', str(args.build_dir), '--config', 'Release'],
                   env=env, check=True)
    subprocess.run([cmake, '--build', str(args.verification_build), '--config', 'Release'],
                   env=env, check=True)
    subprocess.run([ctest, '--test-dir', str(args.verification_build), '-C', 'Release',
                    '-L', 'unit|integration|managed', '--output-on-failure', '--no-tests=error'],
                   env=env, check=True)
    if any(manifest.get(key) != value for key, value in source_state(REPO_ROOT).items()):
        parser.error('Source changed during validation; rebuild and export before delivery')
    binary = REPO_ROOT / 'bin' / args.build_dir.name
    with tempfile.TemporaryDirectory(prefix='atlas-delivery-defs-') as directory:
        definition = export_entity_definitions(REPO_ROOT, args.build_dir.name,
                                              args.base_assembly.resolve(),
                                              Path(directory) / 'entity_defs.bin')
    if definition != manifest['entity_definition_sha256']:
        parser.error('Server entity definitions differ from the SDK; export the matching business SDK')
    entries = {}
    for name in ('machined', 'atlas_loginapp', 'atlas_baseappmgr', 'atlas_baseapp',
                 'atlas_dbapp', 'atlas_cellappmgr', 'atlas_cellapp', 'atlas_tool'):
        path = binary / (name + ('.exe' if sys.platform == 'win32' else ''))
        if not path.is_file():
            raise RuntimeError(f'Missing delivery executable: {path}')
        entries['server/' + path.name] = path
    for pattern in ('*.dll', '*.so*'):
        for path in binary.glob(pattern):
            if not path.name.startswith(('atlas_engine.', 'atlas_db_mysql_plugin.',
                                          'atlas_db_sqlite_plugin.', 'mimalloc', 'TracyClient.',
                                          'libmysql', 'libssl', 'libcrypto', 'nethost.',
                                          'libatlas_engine.', 'libatlas_db_', 'libmimalloc',
                                          'libTracyClient', 'libnethost.')):
                continue
            entries['server/' + path.name] = path
    for name in ('Atlas.Shared.dll', 'Atlas.ClrHost.dll', 'Atlas.Runtime.dll'):
        entries['server/' + name] = binary / name
    for path in (args.base_assembly, args.cell_assembly):
        if path:
            if path.resolve().parent != binary.resolve():
                parser.error('Business assemblies must be deployed in the selected build output')
            entries['server/' + path.name] = path
    for subdir in ('licenses', 'private'):
        for path in (binary / subdir).rglob('*'):
            if path.is_file():
                entries['server/' + path.relative_to(binary).as_posix()] = path
    if any(name in entries for name in ('server/nethost.dll', 'server/libnethost.so')):
        executable = cache(args.build_dir).get('DOTNET_EXECUTABLE:FILEPATH')
        if not executable:
            raise RuntimeError('Missing .NET source location for nethost distribution notices')
        for name in ('LICENSE.txt', 'ThirdPartyNotices.txt'):
            path = Path(executable).resolve().parent / name
            if not path.is_file():
                raise RuntimeError(f'.NET distribution notice missing: {path}; install the official SDK')
            entries['server/licenses/DotNet/' + name] = path
    for path in args.sdk.rglob('*'):
        if path.is_file():
            entries['client/' + path.relative_to(args.sdk).as_posix()] = path
            if path.name.endswith('.LICENSE'):
                entries['server/licenses/ThirdParty/' + path.name] = path
    entries['server/runtime/atlas_server.runtimeconfig.json'] = (
        args.build_dir / 'runtime/atlas_server.runtimeconfig.json')
    entries['server/entity_defs.bin'] = args.sdk / 'entity_defs.bin'
    values = cache(args.build_dir)
    for dependency in ('googletest', 'pugixml', 'rapidjson', 'zlib', 'sqlite3', 'mimalloc',
                       'jolt', 'recastnavigation', 'tracy'):
        source = Path(values.get('FETCHCONTENT_SOURCE_DIR_' + dependency.upper() + ':PATH', '')
                      or args.build_dir / '_deps' / (dependency + '-src'))
        if not source.is_dir():
            if dependency == 'tracy' and values.get('ATLAS_ENABLE_PROFILER:BOOL') == 'OFF':
                continue
            raise RuntimeError(f'Corresponding dependency source missing: {source}; prepare this build again')
        for path in source.rglob('*'):
            relative = path.relative_to(source)
            if dependency == 'rapidjson' and len(relative.parts) > 1 and relative.parts[0] != 'include':
                continue
            if dependency == 'tracy' and len(relative.parts) > 1 and relative.parts[0] != 'public':
                continue
            if dependency == 'jolt' and len(relative.parts) > 1 and relative.parts[0] not in ('Jolt', 'Build'):
                continue
            if dependency == 'recastnavigation' and relative.parts[0] in ('RecastDemo', 'Tests', 'Docs'):
                continue
            if path.is_file() and not path.is_symlink() and engineering_source(path):
                entries['source/third_party/' + dependency + '/' + relative.as_posix()] = path
    for name, expected in manifest['sha256'].items():
        path = binary / name
        if path.is_file() and digest(path) != expected:
            raise RuntimeError(f'Server/SDK files differ: {name}; rebuild and export together')
    sources = subprocess.check_output(['git', 'ls-files', '--cached', '--others',
                                      '--exclude-standard', '-z'], cwd=REPO_ROOT).split(b'\0')
    for raw in sources:
        if raw:
            name = raw.decode('utf-8')
            path = REPO_ROOT / name
            if name.startswith(('samples/mvp/UnityClient/', 'samples/mvp/UEClient/')):
                continue
            if not engineering_source(path):
                continue
            if path.is_file() and not path.is_symlink():
                entries['source/' + name] = path
    args.output.parent.mkdir(parents=True, exist_ok=True)
    partial = args.output.with_name(args.output.name + '.partial')
    with zipfile.ZipFile(partial, 'x', compression=zipfile.ZIP_DEFLATED) as package:
        for name, path in entries.items():
            package.write(path, name)
        package.writestr('delivery.json', json.dumps({
            'sdk': manifest, 'sha256': {name: digest(path) for name, path in entries.items()},
            'fixture_only': manifest.get('fixture', False),
            'validation': 'Release CTest unit/integration/managed passed'}, indent=2))
    partial.rename(args.output)
    print(f'Delivery created: {args.output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Delivery failed: {error}', file=sys.stderr)
        raise SystemExit(1)
