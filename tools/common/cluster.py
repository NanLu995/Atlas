from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from common.paths import resolve_repo_root


def _log(message: str, *, stream: object = sys.stdout) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", file=stream, flush=True)


def config_to_snake(config: str) -> str:
    return re.sub(r"([a-z])([A-Z])", r"\1_\2", config).lower()


def resolve_program(repo_root: Path, bin_name: str, subdirs: Iterable[str], stem: str) -> Path:
    bin_base = repo_root / "bin" / bin_name
    suffixes = [".exe", ""] if os.name == "nt" else ["", ".exe"]
    for subdir in (*subdirs, ""):
        for suffix in suffixes:
            candidate = bin_base / subdir / f"{stem}{suffix}"
            if candidate.exists():
                return candidate
    return bin_base / f"{stem}{'.exe' if os.name == 'nt' else ''}"


def export_entity_definitions(repo_root: Path, build_dir: str, assembly: Path, output: Path) -> str:
    tool = repo_root / "build" / Path(build_dir).name / "csharp" / "Atlas.Tools.DefDump.dll"
    if not tool.is_file():
        raise RuntimeError(f"missing {tool}; build atlas_tools_defdump in the selected build directory")
    if not assembly.is_file():
        raise RuntimeError(f"missing entity assembly {assembly}; build or select --base-assembly")
    try:
        result = subprocess.run(
            ["dotnet", str(tool), "--assembly", str(assembly), "--out", str(output)],
            cwd=repo_root, capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"cannot export definitions from {assembly}: {error}") from error
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"DefDump failed for {assembly}: {detail}")
    try:
        with output.open("rb") as source:
            header = source.read(40)
    except OSError as error:
        raise RuntimeError(f"DefDump did not create {output}: {error}") from error
    if (len(header) != 40 or header[:4] != b"ATDF" or int.from_bytes(header[4:6], "little") != 4
            or header[6:8] != b"\0\0" or header[8:] == bytes(32)):
        raise RuntimeError(f"invalid definition header or digest in {output}; rebuild the entity assembly")
    return header[8:].hex()


@dataclass
class LoggedProcess:
    name: str
    start_order: int
    process: subprocess.Popen[str]
    stdout_handle: object
    stderr_handle: object


def start_logged_process(
    *,
    name: str,
    file_path: Path,
    arguments: Iterable[str],
    working_directory: Path,
    log_directory: Path,
) -> LoggedProcess:
    stdout_path = log_directory / f"{name}.stdout.log"
    stderr_path = log_directory / f"{name}.stderr.log"

    _log(f"Starting {name}")
    stdout_handle = stdout_path.open("w", encoding="utf-8", newline="")
    stderr_handle = stderr_path.open("w", encoding="utf-8", newline="")

    creationflags = 0
    popen_kwargs: dict[str, object] = {}
    if os.name == "nt":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP  # type: ignore[attr-defined]
    else:
        popen_kwargs["start_new_session"] = True

    process = subprocess.Popen(
        [str(file_path), *list(arguments)],
        cwd=working_directory,
        stdout=stdout_handle,
        stderr=stderr_handle,
        text=True,
        creationflags=creationflags,
        **popen_kwargs,
    )

    # Parent's handles closed; child kept its own inherited copies.
    stdout_handle.close()
    stderr_handle.close()

    return LoggedProcess(
        name=name,
        start_order=0,
        process=process,
        stdout_handle=None,
        stderr_handle=None,
    )


def stop_logged_processes(processes: list[LoggedProcess]) -> None:
    failures = []
    for order in sorted({entry.start_order for entry in processes}, reverse=True):
        group = [entry for entry in processes if entry.start_order == order]
        stopping = []
        for entry in group:
            proc = entry.process
            if proc.poll() is None:
                stopping.append(entry)
                _log(f"Stopping {entry.name} (pid={proc.pid})")
                try:
                    if os.name == "nt":
                        # CTRL_BREAK_EVENT graceful shutdown via CREATE_NEW_PROCESS_GROUP.
                        os.kill(proc.pid, signal.CTRL_BREAK_EVENT)
                    else:
                        os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass

        deadline = time.monotonic() + 35
        for entry in stopping:
            proc = entry.process
            try:
                proc.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                failures.append(f"{entry.name}: shutdown exceeded 35 seconds; forced termination")
                try:
                    if os.name == "nt":
                        proc.kill()
                    else:
                        os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    failures.append(f"{entry.name}: process did not exit after forced termination")
            else:
                if proc.returncode != 0:
                    failures.append(f"{entry.name}: shutdown exit code {proc.returncode}")

        for entry in group:
            if entry.stdout_handle:
                entry.stdout_handle.close()
            if entry.stderr_handle:
                entry.stderr_handle.close()

    if failures:
        raise RuntimeError("Cluster shutdown failed: " + "; ".join(failures))


def wait_for_registration(
    *,
    atlas_tool: Path,
    machined_address: str,
    proc_type: str,
    name: str,
    timeout_sec: int = 15,
) -> bool:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        result = subprocess.run(
            [str(atlas_tool), "--machined", machined_address, "list", proc_type],
            capture_output=True,
            text=True,
            cwd=resolve_repo_root(),
            env=os.environ.copy(),
        )
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                fields = line.split()
                if len(fields) >= 2 and fields[0] == proc_type and fields[1] == name:
                    return True
        time.sleep(0.5)
    return False
