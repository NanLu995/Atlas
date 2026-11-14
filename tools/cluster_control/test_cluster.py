#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import random
import socket
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from common.database import add_database_options, database_config, record_database_run, wait_for_database_start
from common.cluster import (
    LoggedProcess,
    resolve_program,
    start_logged_process,
    stop_logged_processes,
    wait_for_registration,
)
from common.paths import resolve_repo_root


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Spawn a minimal Atlas cluster (machined + dbapp + baseappmgr + "
        "loginapp + baseapp) on 127.0.0.1 for integration testing. Outputs a "
        "READY line to stdout once every process has registered, then blocks "
        "until stdin is closed before tearing the cluster down. Stdin accepts "
        "kill-baseapp or restart-baseapp <internal-port> for this launcher's own BaseApps.",
    )
    parser.add_argument("--build-dir", default="debug",
                        help="Subdirectory of bin/ where binaries live (default: debug).")
    parser.add_argument("--base-assembly", type=Path,
                        help="BaseApp script assembly (default: bin/<build-dir>/Atlas.Runtime.dll).")
    parser.add_argument("--cell-assembly", type=Path,
                        help="Also start CellAppMgr and one CellApp with this script assembly.")
    parser.add_argument("--baseapps", type=int, choices=(1, 2), default=1,
                        help="Number of BaseApps; use 2 for client handoff verification (default: 1).")
    parser.add_argument("--port-base", type=int, default=0,
                        help="Lowest port to use; 0 = auto-pick a free range.")
    # Allow database initialization and cold .NET JIT startup.
    parser.add_argument("--registration-timeout-sec", type=int, default=60)
    add_database_options(parser)
    return parser.parse_args()


# Reserve one 16-slot block for machined, its UDP ports, core apps, and tools.
_PORT_BLOCK_SIZE = 16


def _pick_free_port_base() -> int:
    # Avoids the OS dynamic range (Windows 49152-65535) where Hyper-V / WSL
    # / TIME_WAIT can WSAEACCES(10013) seconds after bind(0) succeeds.
    rng = random.Random()
    for _ in range(64):
        candidate = rng.randrange(20000, 40000)
        candidate &= ~(_PORT_BLOCK_SIZE - 1)
        if all(_port_bindable(candidate + offset)
               for offset in range(_PORT_BLOCK_SIZE)):
            return candidate
    raise RuntimeError("could not find a usable port block after 64 attempts")


def _port_bindable(port: int) -> bool:
    for sock_type in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        s = socket.socket(socket.AF_INET, sock_type)
        try:
            s.bind(("127.0.0.1", port))
        except OSError:
            return False
        finally:
            s.close()
    return True


def main() -> int:
    args = parse_args()
    if args.db_backend == 'mysql':
        database_config(args, Path())
    # Retry on bind failure — even with the probe filter, a port can be
    # transiently reserved between probe and child bind. Fixed port_base skips.
    if args.port_base > 0:
        return _spawn_cluster(args, args.port_base)
    for attempt in range(_MAX_SPAWN_RETRIES):
        rc = _spawn_cluster(args, _pick_free_port_base())
        if rc != _RC_RETRY:
            return rc
        print(f"cluster spawn attempt {attempt + 1} hit port bind failure, retrying",
              file=sys.stderr, flush=True)
    print(f"giving up after {_MAX_SPAWN_RETRIES} cluster spawn attempts",
          file=sys.stderr, flush=True)
    return 3


_MAX_SPAWN_RETRIES = 4
_RC_RETRY = -42
_EARLY_EXIT_GRACE_SEC = 2.0


def _kill_baseapp(processes: list[LoggedProcess], ports: list[tuple[int, int]], port: int) -> None:
    index = next((i for i, pair in enumerate(ports) if pair[0] == port), None)
    if index is None:
        raise RuntimeError(f"BaseApp port {port} is not in this test cluster")
    name = 'baseapp' if index == 0 else 'baseapp-2'
    process = next(entry.process for entry in processes if entry.name == name)
    if process.poll() is not None:
        raise RuntimeError(f"{name} already exited; inspect its test cluster log")
    process.kill()
    process.wait(timeout=5)


def _restart_baseapp(processes: list[LoggedProcess], ports: list[tuple[int, int]], port: int,
                     repo_root: Path, log_dir: Path) -> None:
    index = next((i for i, pair in enumerate(ports) if pair[0] == port), None)
    if index is None:
        raise RuntimeError(f"BaseApp port {port} is not in this test cluster")
    name = 'baseapp' if index == 0 else 'baseapp-2'
    entry_index = next(i for i, entry in enumerate(processes) if entry.name == name)
    previous = processes[entry_index]
    if previous.process.poll() is None:
        raise RuntimeError(f"{name} is still running; kill it before restart")
    arguments = previous.process.args
    restart_logs = log_dir / 'restarts' / f'{name}-{time.monotonic_ns()}'
    restart_logs.mkdir(parents=True)
    replacement = start_logged_process(name=name, file_path=Path(arguments[0]),
        arguments=arguments[1:], working_directory=repo_root, log_directory=restart_logs)
    replacement.start_order = previous.start_order
    processes[entry_index] = replacement


def _spawn_cluster(args: argparse.Namespace, port_base: int) -> int:
    # machined_app.cc binds UDP heartbeat to internal_port+1 and mesh to
    # internal_port+2, so peers start at port_base+3.
    machined_port = port_base
    dbapp_port = port_base + 3
    baseappmgr_port = port_base + 4
    baseapp_internal_port = port_base + 5
    baseapp_external_port = port_base + 6
    loginapp_port = port_base + 7

    repo_root = resolve_repo_root()
    bin_name = args.build_dir
    runtime_config = repo_root / "runtime" / "atlas_server.runtimeconfig.json"
    runtime_assembly = repo_root / "bin" / bin_name / "Atlas.Runtime.dll"
    base_assembly = args.base_assembly or runtime_assembly

    machined = resolve_program(repo_root, bin_name, [], "machined")
    loginapp = resolve_program(repo_root, bin_name, [], "atlas_loginapp")
    baseapp = resolve_program(repo_root, bin_name, [], "atlas_baseapp")
    baseappmgr = resolve_program(repo_root, bin_name, [], "atlas_baseappmgr")
    dbapp = resolve_program(repo_root, bin_name, [], "atlas_dbapp")
    atlas_tool = resolve_program(repo_root, bin_name, [], "atlas_tool")

    required = [machined, loginapp, baseapp, baseappmgr, dbapp, atlas_tool,
                runtime_config, runtime_assembly, base_assembly]
    if args.cell_assembly:
        cellappmgr = resolve_program(repo_root, bin_name, [], "atlas_cellappmgr")
        cellapp = resolve_program(repo_root, bin_name, [], "atlas_cellapp")
        required.extend([cellappmgr, cellapp, args.cell_assembly])
    for prog in required:
        if not prog.exists():
            print(f"missing: {prog}", file=sys.stderr, flush=True)
            return 1

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    run_root = repo_root / ".tmp" / "test_cluster" / timestamp
    log_dir = run_root / "logs"
    db_dir = run_root / "db"
    db_config_path = run_root / "dbapp.json"

    log_dir.mkdir(parents=True, exist_ok=True)
    db_dir.mkdir(parents=True, exist_ok=True)

    database = database_config(args, db_dir / 'test_cluster.sqlite3')
    record_database_run(run_root, database, baseapps=args.baseapps,
                        cellapps=1 if args.cell_assembly else 0)

    machined_address = f"127.0.0.1:{machined_port}"
    db_config_path.write_text(
        json.dumps({
            "machined_address": machined_address,
            "auto_create_accounts": True,
            "account_type_id": 1,
            "database": database,
        }, indent=2) + "\n",
        encoding="utf-8",
    )

    processes: list[LoggedProcess] = []

    try:
        processes.append(start_logged_process(
            name="machined", file_path=machined,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "machined", "--name", "machined",
                       "--internal-port", str(machined_port), "--log-level", "info"]))
        processes[-1].start_order = 1
        time.sleep(1)

        processes.append(start_logged_process(
            name="dbapp", file_path=dbapp,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "dbapp", "--name", "dbapp",
                       "--machined", machined_address,
                       "--internal-port", str(dbapp_port),
                       "--config", str(db_config_path),
                       "--update-hertz", "50", "--log-level", "info"]))
        processes[-1].start_order = 2
        wait_for_database_start(processes[-1].process, log_dir)

        processes.append(start_logged_process(
            name="baseappmgr", file_path=baseappmgr,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "baseappmgr", "--name", "baseappmgr",
                       "--machined", machined_address,
                       "--internal-port", str(baseappmgr_port),
                       "--update-hertz", "50", "--log-level", "info"]))
        processes[-1].start_order = 3

        if args.cell_assembly:
            processes.append(start_logged_process(
                name="cellappmgr", file_path=cellappmgr,
                working_directory=repo_root, log_directory=log_dir,
                arguments=["--type", "cellappmgr", "--name", "cellappmgr",
                           "--machined", machined_address,
                           "--internal-port", str(port_base + 8),
                           "--update-hertz", "50", "--log-level", "info"]))
            processes[-1].start_order = 3
            processes.append(start_logged_process(
                name="cellapp", file_path=cellapp,
                working_directory=repo_root, log_directory=log_dir,
                arguments=["--type", "cellapp", "--name", "cellapp",
                           "--machined", machined_address,
                           "--internal-port", str(port_base + 9),
                           "--assembly", str(args.cell_assembly),
                           "--runtime-config", str(runtime_config),
                           "--update-hertz", "50", "--log-level", "info"]))
            processes[-1].start_order = 3

        baseapp_ports = [(baseapp_internal_port, baseapp_external_port)]
        if args.baseapps == 2:
            baseapp_ports.append((port_base + 10, port_base + 11))
        for index, (internal_port, external_port) in enumerate(baseapp_ports):
            name = "baseapp" if index == 0 else "baseapp-2"
            processes.append(start_logged_process(
                name=name, file_path=baseapp,
                working_directory=repo_root, log_directory=log_dir,
                arguments=["--type", "baseapp", "--name", name,
                           "--machined", machined_address,
                           "--internal-port", str(internal_port),
                           "--external-port", str(external_port),
                           "--assembly", str(base_assembly),
                           "--runtime-config", str(runtime_config),
                           "--update-hertz", "50", "--log-level", "info"]))
            processes[-1].start_order = 4

        processes.append(start_logged_process(
            name="loginapp", file_path=loginapp,
            working_directory=repo_root, log_directory=log_dir,
            arguments=["--type", "loginapp", "--name", "loginapp",
                       "--machined", machined_address,
                       "--external-port", str(loginapp_port),
                       "--auto-create-accounts", "true",
                       # Effectively-off; the prod 5/min would rate-limit
                       # sequential RealCluster tests sharing 127.0.0.1.
                       "--login-rate-limit-per-ip", "10000",
                       "--login-rate-limit-global", "100000",
                       "--update-hertz", "50", "--log-level", "info"]))
        processes[-1].start_order = 5

        # Grace window for bind failures (WSAEACCES → immediate exit).
        # Any early-exit signals the outer loop to pick a fresh port_base.
        time.sleep(_EARLY_EXIT_GRACE_SEC)
        for p in processes:
            if p.process.poll() is not None:
                if p.name == "dbapp":
                    detail = (log_dir / "dbapp.stderr.log").read_text(encoding="utf-8", errors="replace")
                    if "database startup failed" in detail or "CreateDatabase:" in detail:
                        print("Database startup failed; no backend fallback. " + detail,
                              file=sys.stderr, flush=True)
                        return 1
                print(f"{p.name} exited early (rc={p.process.returncode}); will retry",
                      file=sys.stderr, flush=True)
                return _RC_RETRY

        ready_targets = [
            ("dbapp", "dbapp"),
            ("baseappmgr", "baseappmgr"),
            ("baseapp", "baseapp"),
            ("loginapp", "loginapp"),
        ]
        if args.cell_assembly:
            ready_targets.extend([("cellappmgr", "cellappmgr"), ("cellapp", "cellapp")])
        if args.baseapps == 2:
            ready_targets.append(("baseapp", "baseapp-2"))
        for proc_type, name in ready_targets:
            if not wait_for_registration(
                atlas_tool=atlas_tool, machined_address=machined_address,
                proc_type=proc_type, name=name,
                timeout_sec=args.registration_timeout_sec,
            ):
                print(f"timeout waiting for {name}", file=sys.stderr, flush=True)
                return 2

        # Single-line READY marker; consumers parse `key=value` pairs.
        print(
            f"READY loginapp_port={loginapp_port} baseapp_external_port={baseapp_external_port} "
            f"machined_port={machined_port} log_dir={log_dir} db_backend={args.db_backend} "
            f"dbapps=1 baseapps={args.baseapps} cellapps={1 if args.cell_assembly else 0} "
            f"baseapp_internal_ports={','.join(str(p[0]) for p in baseapp_ports)} "
            f"baseapp_external_ports={','.join(str(p[1]) for p in baseapp_ports)}",
            flush=True,
        )

        for line in iter(sys.stdin.readline, ""):
            command = line.split()
            if (len(command) != 2 or command[0] not in ('kill-baseapp', 'restart-baseapp')
                    or not command[1].isdigit()):
                raise RuntimeError("expected kill-baseapp or restart-baseapp <internal-port>")
            port = int(command[1])
            if command[0] == 'kill-baseapp':
                _kill_baseapp(processes, baseapp_ports, port)
                print(f"KILLED baseapp_internal_port={port}", flush=True)
            else:
                _restart_baseapp(processes, baseapp_ports, port, repo_root, log_dir)
                print(f"RESTARTED baseapp_internal_port={port}", flush=True)
        return 0
    except KeyboardInterrupt:
        return 0
    finally:
        stop_logged_processes(processes)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
