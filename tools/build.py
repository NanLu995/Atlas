#!/usr/bin/env python3
"""One-shot cmake configure + build helper; loads MSVC env, provisions Ninja."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common.paths import REPO_ROOT

PRESET_CONFIGS = {
    "debug": "Debug",
    "profile": "RelWithDebInfo",
    "release": "Release",
    "vs": "Debug",
}

VS_GENERATOR = {
    17: "Visual Studio 17 2022",
}

NINJA_VERSION = "1.12.1"
NINJA_URLS = {
    "Windows": f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-win.zip",
    "Linux":   f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-linux.zip",
    "Darwin":  f"https://github.com/ninja-build/ninja/releases/download/v{NINJA_VERSION}/ninja-mac.zip",
}

NINJA_SHA256 = {
    'Windows': 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a',
    'Linux': '6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255',
    'Darwin': '89a287444b5b3e98f88a945afa50ce937b8ffd1dcc59c555ad9b1baf855298c9',
}


def log(msg: str) -> None:
    print(f"[build] {msg}", flush=True)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Atlas one-shot configure + build.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Presets:\n"
            "  debug    Ninja Multi-Config, Debug    (fast dev iteration, /Z7 + PCH)\n"
            "  profile  RelWithDebInfo + Tracy + viewer (no tests)\n"
            "  release  Release (no tests)\n"
            "  vs       Visual Studio solution, Debug default (Windows only, with tests)"
        ),
    )
    p.add_argument(
        "preset",
        choices=sorted(PRESET_CONFIGS),
        help="CMake preset to use.",
    )
    p.add_argument(
        "--clean",
        action="store_true",
        help="Wipe build/<preset> before configuring.",
    )
    p.add_argument("--build-dir", type=Path,
                   help="Isolated build directory directly under build/ (relative to repo root).")
    p.add_argument('--with-tests', action='store_true',
                   help='Enable existing tests, including for Release acceptance.')
    p.add_argument('--cmake-arg', action='append', default=[],
                   help='Forward a configure argument, for example --cmake-arg=-DATLAS_DB_MYSQL=OFF.')
    g = p.add_mutually_exclusive_group()
    g.add_argument("--config-only", action="store_true",
                   help="Run cmake configure, skip build.")
    g.add_argument("--build-only",  action="store_true",
                   help="Skip configure, just build.")
    return p.parse_args()


def detect_vs_install() -> tuple[Path, int]:
    """Select a complete VS 2022 17.14 release installation."""
    pf86 = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    vswhere = Path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError(f"vswhere.exe not found at {vswhere}")

    raw = subprocess.run(
        [str(vswhere), "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-format", "json"],
        capture_output=True, check=True,
    ).stdout.decode(errors="replace")
    installs = json.loads(raw) if raw.strip() else []
    installs = [item for item in installs if not item.get("isPrerelease", False)
                and item.get("isComplete", False)
                and item.get("installationVersion", "").startswith("17.14.")]
    if not installs:
        raise RuntimeError("Install Visual Studio 2022 17.14 stable with Desktop C++ tools")

    def _ver(s: str) -> tuple[int, ...]:
        return tuple(int(p) for p in s.split(".") if p.isdigit())

    best = max(installs, key=lambda i: _ver(i.get("installationVersion", "0")))
    return Path(best["installationPath"]), _ver(best["installationVersion"])[0]


def load_msvc_env() -> tuple[dict[str, str], int | None]:
    """Load the selected release toolchain, independent of the caller's PATH."""
    vs_path, vs_major = detect_vs_install()
    vcvars = vs_path / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not vcvars.is_file():
        raise RuntimeError(f"vcvars64.bat not found at {vcvars}")

    log(f"Loading MSVC env from VS {vs_major} at {vs_path}")
    res = subprocess.run(
        f'"{vcvars}" -vcvars_ver=14.44 >nul && set',
        capture_output=True, shell=True, check=True,
    )
    env = os.environ.copy()
    for line in res.stdout.decode(errors="replace").splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            env[key] = value
    compiler = shutil.which("cl.exe", path=env.get("PATH"))
    if not compiler or not Path(compiler).resolve().is_relative_to(vs_path.resolve()):
        raise RuntimeError("Selected VS did not provide cl.exe; repair the Desktop C++ workload")
    if not env.get("VCToolsVersion", "").startswith("14.44."):
        raise RuntimeError("Install the MSVC 14.44 toolset in Visual Studio Installer")
    return env, vs_major


def check_version(program: str, minimum: tuple[int, ...], env: dict[str, str],
                  *, exact: bool = False) -> str:
    program = shutil.which(program, path=env.get('PATH')) or program
    required = '.'.join(map(str, minimum)) + (' exactly' if exact else ' or newer')
    try:
        result = subprocess.run([program, "--version"], env=env, cwd=REPO_ROOT,
                                capture_output=True, text=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        raise RuntimeError(f'{program} missing or cannot select its version; install {required} '
                           'and select the matching executable on PATH') from None
    match = re.search(r"\d+\.\d+(?:\.\d+)?", result.stdout)
    version = tuple(map(int, match[0].split("."))) if match else ()
    if (version != minimum if exact else version < minimum):
        required = ".".join(map(str, minimum))
        raise RuntimeError(f"{program}: found {match[0] if match else 'unknown'}; install {required}"
                           + (" exactly" if exact else " or newer"))
    log(f"{program}: {match[0]}")
    return match[0]


def check_environment(env: dict[str, str], cmake: str) -> None:
    if sys.version_info < (3, 11, 8):
        raise RuntimeError("Install Python 3.11.8 or newer")
    check_version(cmake, (3, 28, 0), env)
    sdk = json.loads((REPO_ROOT / "global.json").read_text())["sdk"]["version"]
    check_version("dotnet", tuple(map(int, sdk.split("."))), env, exact=True)
    if platform.system() == "Linux":
        try:
            release = platform.freedesktop_os_release()
        except OSError:
            release = {}
        if release.get('ID') != 'ubuntu' or release.get('VERSION_ID') != '24.04':
            log('Linux distribution unverified: main acceptance requires Ubuntu 24.04 LTS x86-64')
        for key in ("CC", "CXX"):
            env[key] = env.get(key) or ("gcc-13" if key == "CC" else "g++-13")
            version = check_version(env[key], (13, 0), env)
            if version.split(".")[0] != "13":
                raise RuntimeError(f"{key}: select GCC 13 with {key}=<compiler>")


def validate_build_cache(directory: Path, env: dict[str, str]) -> None:
    path = directory / 'CMakeCache.txt'
    if not path.is_file():
        return
    values = {line.split(':', 1)[0]: line.split('=', 1)[1]
              for line in path.read_text(encoding='utf-8').splitlines()
              if '=' in line and ':' in line and not line.startswith(('#', '//'))}
    windows = platform.system() == 'Windows'
    generators = ('Ninja Multi-Config', 'Visual Studio 17 2022' if windows else 'Unix Makefiles')
    if values.get('CMAKE_GENERATOR') not in generators:
        raise RuntimeError('Cached generator is outside the baseline; select a fresh --build-dir')
    expected = shutil.which('cl.exe' if windows else env.get('CXX', 'g++-13'), path=env.get('PATH'))
    compiler = values.get('CMAKE_CXX_COMPILER')
    if compiler and expected and Path(compiler).resolve() != Path(expected).resolve():
        raise RuntimeError('Cached compiler differs from the selected toolchain; use a fresh --build-dir')
    instance = values.get('CMAKE_GENERATOR_INSTANCE')
    if windows and instance and env.get('VSINSTALLDIR') and (
            Path(instance).resolve() != Path(env['VSINSTALLDIR']).resolve()):
        raise RuntimeError('Cached VS instance differs from the selected release; use a fresh --build-dir')


def ensure_ninja(env: dict[str, str]) -> None:
    """Make sure 'ninja' resolves on env['PATH']; download to .tmp/ninja/ if not."""
    if shutil.which("ninja", path=env.get("PATH")):
        check_version("ninja", (1, 11, 0), env)
        return

    system = platform.system()
    url = NINJA_URLS.get(system)
    if url is None:
        raise RuntimeError(f"No Ninja download URL for platform {system}")

    ninja_dir = REPO_ROOT / ".tmp" / "ninja"
    ninja_exe_name = "ninja.exe" if system == "Windows" else "ninja"
    ninja_exe = ninja_dir / ninja_exe_name

    if not ninja_exe.is_file():
        ninja_dir.mkdir(parents=True, exist_ok=True)
        zip_path = ninja_dir / "ninja.zip"
        log(f"Downloading Ninja v{NINJA_VERSION} ({system}) -> {ninja_dir}")
        urllib.request.urlretrieve(url, zip_path)
        if hashlib.sha256(zip_path.read_bytes()).hexdigest() != NINJA_SHA256[system]:
            raise RuntimeError(f"Ninja checksum mismatch: {zip_path}; obtain the official archive")
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(ninja_dir)
        zip_path.unlink()
        if system != "Windows":
            ninja_exe.chmod(ninja_exe.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    env["PATH"] = f"{ninja_dir}{os.pathsep}{env.get('PATH', '')}"
    log(f"Ninja: {ninja_exe}")


def run_cmake(args: list[str], env: dict[str, str]) -> None:
    log(" ".join(args))
    subprocess.run(args, env=env, cwd=REPO_ROOT, check=True)


def resolve_cmake(env: dict[str, str]) -> str:
    cmake = shutil.which("cmake", path=env.get("PATH", ""))
    if cmake:
        return cmake
    if platform.system() == "Windows":
        vs_dir = env.get("VSINSTALLDIR")
        vs_path = Path(vs_dir) if vs_dir else detect_vs_install()[0]
        bundled = vs_path / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
        if bundled.is_file():
            return str(bundled)
    raise RuntimeError("CMake not found; install CMake 3.28+ and add its bin directory to PATH")


def main() -> int:
    args = parse_args()
    if sys.version_info < (3, 11, 8):
        raise RuntimeError('Install Python 3.11.8 or newer')
    config = PRESET_CONFIGS[args.preset]
    build_dir = (REPO_ROOT / (args.build_dir or Path("build") / args.preset)).resolve()
    if (build_dir.parent != (REPO_ROOT / "build").resolve() or
            not build_dir.is_relative_to(REPO_ROOT.resolve())):
        raise RuntimeError("--build-dir must be a directory directly under the repository build/")

    if platform.system() == "Windows":
        env, vs_major = load_msvc_env()
        # Use the generator matching the selected compiler installation.
        if vs_major is not None and vs_major in VS_GENERATOR:
            env["CMAKE_GENERATOR"] = VS_GENERATOR[vs_major]
            log(f"CMAKE_GENERATOR={env['CMAKE_GENERATOR']}")
    else:
        env = os.environ.copy()

    env.setdefault("CMAKE_BUILD_PARALLEL_LEVEL", "2")
    cmake = resolve_cmake(env)
    check_environment(env, cmake)
    validate_build_cache(build_dir, env)
    if platform.system() == "Linux":
        env["CMAKE_GENERATOR"] = "Unix Makefiles"
    if args.preset == "debug":
        ensure_ninja(env)

    if args.clean and build_dir.exists():
        log(f"Wiping {build_dir}")
        shutil.rmtree(build_dir)

    t_start = time.monotonic()

    if not args.build_only:
        configure = [cmake, "--preset", args.preset, "-B", str(build_dir)]
        if args.with_tests:
            configure.append('-DATLAS_BUILD_TESTS=ON')
        configure.extend(args.cmake_arg)
        run_cmake(configure, env)

    if not args.config_only:
        run_cmake(
            [cmake, "--build", str(build_dir), "--config", config],
            env,
        )

    log(f"Done in {time.monotonic() - t_start:.1f}s")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        log(f"command failed (exit {e.returncode}): {' '.join(map(str, e.cmd))}")
        sys.exit(e.returncode)
    except Exception as e:
        log(f"FAILED: {e}")
        sys.exit(1)
