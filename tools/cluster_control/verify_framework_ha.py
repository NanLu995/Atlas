from concurrent.futures import ThreadPoolExecutor
import argparse
from pathlib import Path
import os
import json
import queue
import re
import subprocess
import sys
import threading
import time
import uuid

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root/'tools'))
from common.cluster import start_logged_process, stop_logged_processes, wait_for_registration
from cluster_control.test_cluster import _pick_free_port_base
from common.database import add_database_options, database_config

def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=
        'Verify isolated MySQL framework recovery using the existing cluster launcher and HA verifiers. '
        'Requires the RPC Base/Cell fixture assemblies; never creates or deletes a database.')
    parser.add_argument('--build', default='debug', help='Directory under bin/ containing this build.')
    parser.add_argument('--scenario', choices=('all', 'managers', 'workers'), default='all',
                        help='Three Manager restart/overlap cycles, owned Worker/machined cycles, or both.')
    add_database_options(parser)
    args = parser.parse_args(argv)
    if args.db_backend != 'mysql':
        parser.error('Framework recovery acceptance requires MySQL; SQLite is only for single-DBApp smoke')
    if Path(args.build).name != args.build or args.build in ('.', '..'):
        parser.error('--build must be a single directory name under bin/')
    return args


def registered_names(output, role):
    return {fields[1] for fields in (line.split() for line in output.splitlines())
            if len(fields) >= 2 and fields[0] == role}


def collect_diagnostics(control):
    snapshots = {}
    for role, names in (('baseapp', ('baseapp', 'baseapp-2')),
                       ('cellapp', ('cellapp', 'cellapp-2')), ('loginapp', ('loginapp',)),
                       ('dbapp', ('dbapp',)), ('baseappmgr', ('baseappmgr',)),
                       ('cellappmgr', ('cellappmgr',)), ('dbappmgr', ('dbappmgr',))):
        for name in names:
            target = role + ':' + name
            paths = ['app/ready', 'app/process_instance', 'machined/registration_generation',
                     'app/shutdown/state', 'app/shutdown/pending_operations', 'network/pending_sends']
            if role in ('baseapp', 'cellapp', 'loginapp'):
                paths += ['rpc/pending', 'rpc/pending_limit', 'rpc/replies', 'rpc/reply_limit',
                          'rpc/oldest_pending_ms']
            if role == 'dbapp':
                paths += ['dbapp/pending_operations', 'dbapp/operation_limit']
            values = {}
            for path in paths:
                parts = control('watch', target, path).strip().split(maxsplit=1)
                if len(parts) != 2 or parts[0] != name:
                    raise RuntimeError('Invalid diagnostic response: ' + target + ' ' + path)
                values[path] = parts[1]
            if values['app/ready'] != 'true' or values['app/shutdown/state'] != 'running':
                raise RuntimeError('Process not ready: ' + target)
            for path in paths:
                if path in ('app/ready', 'app/shutdown/state'):
                    continue
                if int(values[path]) < 0:
                    raise RuntimeError('Negative diagnostic count: ' + target + ' ' + path)
            for path in ('app/process_instance', 'machined/registration_generation'):
                if int(values[path]) == 0:
                    raise RuntimeError('Unregistered diagnostic identity: ' + target)
            for count, limit in (('rpc/pending', 'rpc/pending_limit'),
                                 ('rpc/replies', 'rpc/reply_limit'),
                                 ('dbapp/pending_operations', 'dbapp/operation_limit')):
                if count in values and int(values[count]) > int(values[limit]):
                    raise RuntimeError('Diagnostic capacity exceeded: ' + target + ' ' + count)
            snapshots[target] = values
    return snapshots


def main():
    args = parse_args()
    database_config(args, Path())
    run = root/'.tmp'/('framework-ha-' + uuid.uuid4().hex)
    run.mkdir(parents=True)
    binary = root/'bin'/args.build
    suffix = '.exe' if os.name == 'nt' else ''
    tool = binary/('atlas_tool' + suffix)
    for name in ('machined', 'atlas_tool', 'atlas_dbapp', 'atlas_baseapp', 'atlas_cellapp',
                 'atlas_baseappmgr', 'atlas_cellappmgr', 'atlas_dbappmgr', 'atlas_reviver'):
        if not (binary/(name + suffix)).is_file():
            raise RuntimeError('Missing binary ' + str(binary/(name + suffix)) + '; build this target first')
    for name in ('Atlas.RpcFixture.Base.dll', 'Atlas.RpcFixture.Cell.dll'):
        if not (binary/name).is_file():
            raise RuntimeError('Missing fixture ' + str(binary/name) + '; build atlas_rpc_fixture_base_dll_deploy and atlas_rpc_fixture_cell_dll_deploy')
    port = _pick_free_port_base()
    host = f'127.0.0.1:{port}'
    events = queue.Queue()


    def consume(pipe, destination, notify=False):
        with destination.open('w', encoding='utf-8') as log:
            for line in pipe:
                log.write(line)
                log.flush()
                if notify:
                    events.put(line)


    def control(*arguments):
        result = subprocess.run([str(tool), '--machined', host, *arguments], cwd=root,
                                capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise RuntimeError(f'atlas_tool {arguments[0]} failed: ' + result.stderr[-800:])
        return result.stdout


    command = [sys.executable, str(root/'tools/cluster_control/test_cluster.py'),
               '--build-dir', args.build, '--port-base', str(port),
               '--base-assembly', str(binary/'Atlas.RpcFixture.Base.dll'),
               '--cell-assembly', str(binary/'Atlas.RpcFixture.Cell.dll'),
               '--baseapps', '2', '--cellapps', '2', '--log-level', 'debug']
    if args.database_config:
        command += ['--database-config', str(args.database_config.resolve())]
    cluster_env = os.environ.copy()
    cluster_env['ATLAS_RPC_FIXTURE_DIR'] = str(run/'fixture')
    cluster = subprocess.Popen(command, cwd=root, env=cluster_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, encoding='utf-8', errors='replace')
    threads = [threading.Thread(target=consume, args=(cluster.stdout, run/'cluster.stdout.log', True)),
               threading.Thread(target=consume, args=(cluster.stderr, run/'cluster.stderr.log'))]
    for thread in threads:
        thread.start()
    extras = []
    try:
        deadline = time.monotonic() + 100
        while True:
            if time.monotonic() >= deadline or cluster.poll() is not None:
                raise RuntimeError('test cluster startup failed; inspect isolated logs')
            try:
                line = events.get(timeout=1)
            except queue.Empty:
                continue
            if line.startswith('READY '):
                print(line.strip(), flush=True)
                break

        control('set-watch', 'cellappmgr:cellappmgr',
                'cellappmgr/lb/auto_merge/sustain_balance_ticks', '4294967295')
        recovery_config = run/'recovery-config.json'
        recovery_config.write_text(json.dumps({'cellappmgr_lb_auto_merge_sustain_ticks': 4294967295}),
                                   encoding='utf-8')
        manager_types = ('baseappmgr', 'cellappmgr', 'dbappmgr')
        manager_ports = (port + 4, port + 8, port + 12)
        dbmgr = start_logged_process(name='dbappmgr', file_path=binary/('atlas_dbappmgr' + suffix),
            working_directory=root, log_directory=run,
            arguments=['--type', 'dbappmgr', '--name', 'dbappmgr', '--machined', host,
                       '--internal-port', str(port + 12), '--log-level', 'debug'])
        extras.append(dbmgr)
        if not wait_for_registration(atlas_tool=tool, machined_address=host,
                                     proc_type='dbappmgr', name='dbappmgr', timeout_sec=30):
            raise RuntimeError('DBAppMgr registration timeout')
        reviver_arguments = ['--type', 'reviver', '--name', 'reviver', '--machined', host,
                             '--internal-port', str(port + 13), '--log-level', 'debug',
                             '--revive-restart-delay-ms', '500', '--config', str(recovery_config)]
        for role, internal_port in zip(manager_types, manager_ports):
            reviver_arguments += [f'--revive-{role}-exe', str(binary/('atlas_' + role + suffix)),
                                  f'--revive-{role}-name', role,
                                  f'--revive-{role}-port', str(internal_port),
                                  f'--revive-{role}-on-start', 'true',
                                  f'--revive-{role}-output-path', str(run/(role + '-revived.log'))]
        reviver = start_logged_process(name='reviver', file_path=binary/('atlas_reviver' + suffix),
            working_directory=root, log_directory=run, arguments=reviver_arguments)
        extras.append(reviver)
        if not wait_for_registration(atlas_tool=tool, machined_address=host,
                                     proc_type='reviver', name='reviver', timeout_sec=30):
            raise RuntimeError('Reviver registration timeout')
        time.sleep(5)

        def check_topology(label):
            topology = control('watch', 'cellappmgr:cellappmgr', 'cellappmgr/lb/spaces')
            if not re.search(r'\bspace=1\b', topology) or not re.search(r'\bleaves=2\b', topology):
                raise RuntimeError('expected one real space with two leaves: ' + topology)
            counts = []
            for name in ('cellapp', 'cellapp-2'):
                spaces = control('watch', 'cellapp:' + name, 'cellapp/space_count').split()[-1]
                if spaces != '1':
                    raise RuntimeError(name + ' has no recovered space')
                count = control('watch', 'cellapp:' + name, 'cellapp/real_entity_count').split()[-1]
                counts.append(int(count))
            if sum(counts) != 2:
                raise RuntimeError('expected exactly two Real probes: ' + str(counts))
            (run/(label + '-topology.txt')).write_text(topology + chr(10) + str(counts), encoding='utf-8')

        deadline = time.monotonic() + 60
        while True:
            try:
                check_topology('initial')
                break
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.2)

        def verify(role, mode, label=None):
            prefix = role + '-' + (label or mode)
            output = run/(prefix + '.log')
            arguments = [sys.executable, str(root/f'tools/cluster_control/verify_{role}_ha.py'),
                         '--build', args.build, '--machined', host, '--timeout-sec', '60',
                         '--poll-sec', '0.2', '--stability-sec', '2', '--check-active-reviver',
                         '--summary-json', str(run/(prefix + '.json'))]
            if role == 'cellappmgr':
                arguments += ['--min-cellapps', '2']
            if role == 'baseappmgr':
                arguments += ['--min-baseapps', '2']
            if mode == 'baseline':
                arguments.append('--no-inject')
            with output.open('w', encoding='utf-8') as log:
                result = subprocess.run(arguments, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=180)
            print(role, mode, 'exit', result.returncode, flush=True)
            if result.returncode:
                print(output.read_text(encoding='utf-8')[-4000:], flush=True)
                raise RuntimeError(f'{role} {mode} failed')

        def owned_command(verb, name):
            cluster.stdin.write(verb + '-process ' + name + chr(10))
            cluster.stdin.flush()
            expected = ('KILLED' if verb == 'kill' else 'RESTARTED') + ' process=' + name
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                try:
                    line = events.get(timeout=1)
                except queue.Empty:
                    if cluster.poll() is not None:
                        raise RuntimeError('test cluster exited during process control')
                    continue
                if line.strip() == expected:
                    return
            raise RuntimeError('process control acknowledgement timeout: ' + name)

        (run/'initial-diagnostics.json').write_text(
            json.dumps(collect_diagnostics(control), indent=2), encoding='utf-8')
        if args.scenario in ('all', 'managers'):
            for manager in manager_types:
                verify(manager, 'baseline')
            for manager in manager_types:
                verify(manager, 'restart')
            with ThreadPoolExecutor(max_workers=3) as pool:
                list(pool.map(lambda manager: verify(manager, 'overlap'), manager_types))
        if args.scenario in ('all', 'workers'):
            for name, role in (('dbapp', 'dbapp'), ('cellapp', 'cellapp'),
                               ('cellapp-2', 'cellapp'), ('machined', 'machined')):
                owned_command('kill', name)
                if role != 'machined':
                    deadline = time.monotonic() + 30
                    while name in registered_names(control('list', role), role):
                        if time.monotonic() >= deadline:
                            raise RuntimeError('dead process was not removed: ' + name)
                        time.sleep(0.2)
                else:
                    time.sleep(3)
                owned_command('restart', name)
                deadline = time.monotonic() + 60
                while True:
                    try:
                        for process_type, names in (('baseapp', ('baseapp', 'baseapp-2')),
                                ('cellapp', ('cellapp', 'cellapp-2')), ('dbapp', ('dbapp',)),
                                ('baseappmgr', ('baseappmgr',)), ('cellappmgr', ('cellappmgr',)),
                                ('dbappmgr', ('dbappmgr',))):
                            registered = control('list', process_type)
                            found = registered_names(registered, process_type)
                            if not set(names).issubset(found):
                                raise RuntimeError('registration still recovering: ' + process_type)
                        check_topology(name + '-recovered')
                        break
                    except RuntimeError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.2)
                for manager in manager_types:
                    verify(manager, 'baseline', name)
                print('PASS owned process crash/restart', name, flush=True)
        check_topology('final')
        (run/'final-diagnostics.json').write_text(
            json.dumps(collect_diagnostics(control), indent=2), encoding='utf-8')
        print('PASS framework recovery', args.scenario, 'backend=mysql', run, flush=True)

    finally:
        try:
            if len(extras) > 1:
                stop_logged_processes([extras.pop()])
            if cluster.poll() is None:
                for role in ('baseappmgr', 'cellappmgr', 'dbappmgr'):
                    if role in control('list', role):
                        control('shutdown', f'{role}:{role}', '0')
                deadline = time.monotonic() + 15
                while any(role in control('list', role)
                          for role in ('baseappmgr', 'cellappmgr', 'dbappmgr')):
                    if time.monotonic() >= deadline:
                        raise RuntimeError('manager cleanup timeout')
                    time.sleep(0.2)
        finally:
            cluster.stdin.close()
            code = cluster.wait(timeout=100)
            for thread in threads:
                thread.join(timeout=5)
            stop_logged_processes(extras)
            print('cluster shutdown exit', code, 'logs', run, flush=True)
            if code:
                raise RuntimeError('test cluster shutdown failed')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        print('Framework recovery failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
