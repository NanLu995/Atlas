"""Build the pinned OpenSSL release into an isolated Atlas SDK directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

from build import load_msvc_env
from common.json_io import write_json_atomic
from common.paths import REPO_ROOT

VERSION = "3.5.9"
SHA256 = "603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a"
SOURCE_URL = (
    f"https://github.com/openssl/openssl/releases/download/openssl-{VERSION}/"
    f"openssl-{VERSION}.tar.gz"
)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_archive(path: Path) -> None:
    if digest(path) != SHA256:
        raise RuntimeError(f"OpenSSL source checksum mismatch: {path}; use the official {VERSION} archive")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", choices=["Debug", "Release"], default="Debug")
    parser.add_argument("--prefix", type=Path, help="SDK directory under the repository build/ tree")
    parser.add_argument("--archive", type=Path, help="Offline official release archive; SHA-256 is checked")
    parser.add_argument("--perl", type=Path, help="Perl executable; PATH and existing portable Perl are searched")
    parser.add_argument("--nasm", type=Path, help="NASM executable for native Windows assembly support")
    parser.add_argument("--jobs", type=int, default=2, help="Unix make parallelism (default: 2)")
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("--jobs must be positive")
    build_root = (REPO_ROOT / "build").resolve()
    prefix = (args.prefix or build_root / "openssl" / VERSION / args.config).resolve()
    if not prefix.is_relative_to(build_root) or prefix == build_root:
        parser.error("--prefix must be a subdirectory of this repository's build/ tree")
    system = platform.system()
    if system not in ["Windows", "Linux"] or platform.machine().lower() not in ["amd64", "x86_64"]:
        parser.error("this SDK builder supports native Windows x64 and Linux x64")
    artifacts = ["lib/libcrypto.lib", "lib/libssl.lib", "bin/openssl.exe"] if system == "Windows" else [
        "lib/libcrypto.a", "lib/libssl.a", "bin/openssl"]
    artifacts += ["include/openssl/opensslv.h", "share/licenses/OpenSSL/LICENSE.txt",
                  "share/licenses/OpenSSL/AUTHORS.md"]
    receipt_path = prefix / "atlas-source.json"
    if receipt_path.is_file():
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        files = receipt.get("sha256", {})
        if (receipt.get("source_sha256") == SHA256 and receipt.get("config") == args.config
                and receipt.get("platform") == system and set(files) == set(artifacts)):
            if all((prefix / name).is_file() and digest(prefix / name) == value
                   for name, value in files.items()):
                print(f"Verified existing OpenSSL {VERSION} {args.config} SDK: {prefix}")
                return 0
        raise RuntimeError(f"OpenSSL SDK receipt mismatch: {prefix}; choose a fresh --prefix")
    env = load_msvc_env()[0] if system == "Windows" else os.environ.copy()
    portable = build_root / "strawberry_perl_portable"
    perl = args.perl or shutil.which("perl", path=env.get("PATH"))
    if system == "Windows" and args.perl is None and (portable / "perl/bin/perl.exe").is_file():
        perl = portable / "perl/bin/perl.exe"
    if not perl or not Path(perl).is_file():
        raise RuntimeError("Perl is required; pass --perl with a native Perl executable")
    if system == "Windows":
        nasm = args.nasm or shutil.which("nasm", path=env.get("PATH"))
        if nasm is None and (portable / "c/bin/nasm.exe").is_file():
            nasm = portable / "c/bin/nasm.exe"
        if not nasm or not Path(nasm).is_file():
            raise RuntimeError("NASM is required; pass --nasm with an existing executable")
        env["PATH"] = str(Path(nasm).parent) + os.pathsep + env.get("PATH", "")
    make = shutil.which("nmake" if system == "Windows" else "make", path=env.get("PATH"))
    if not make:
        raise RuntimeError("Native make tool missing; use MSVC on Windows or build-essential on Linux")
    cache = build_root / "openssl" / VERSION
    cache.mkdir(parents=True, exist_ok=True)
    archive = args.archive.resolve() if args.archive else cache / f"openssl-{VERSION}.tar.gz"
    if not archive.is_file():
        if args.archive:
            raise RuntimeError(f"Offline archive not found: {archive}")
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as source, archive.open("wb") as output:
            shutil.copyfileobj(source, output)
    verify_archive(archive)
    work = Path(tempfile.mkdtemp(prefix=f"{args.config}-", dir=cache))
    with tarfile.open(archive) as source:
        source.extractall(work, filter="data")
    source_dir = work / f"openssl-{VERSION}"
    build_dir = work / "build"
    build_dir.mkdir()
    target = "VC-WIN64A" if system == "Windows" else "linux-x86_64"
    configure = [str(perl), str(source_dir / "Configure"), target, "no-shared", "no-comp",
                 "no-module", "no-pinshared", "--libdir=lib", f"--prefix={prefix}",
                 f"--openssldir={prefix / 'ssl'}"]
    if args.config == "Debug":
        configure.append("--debug")
    build = [make] if system == "Windows" else [make, f"-j{args.jobs}"]
    for name, command in [("configure", configure), ("build", build),
                          ("test", [make, "test"]), ("install", [make, "install_sw"])]:
        log_path = work / f"{name}.log"
        print(f"OpenSSL {name}: {log_path}", flush=True)
        with log_path.open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=build_dir, env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(f"OpenSSL {name} failed ({result.returncode}); inspect {log_path}")
    notices = prefix / "share/licenses/OpenSSL"
    notices.mkdir(parents=True, exist_ok=True)
    for name in ["LICENSE.txt", "AUTHORS.md"]:
        shutil.copy2(source_dir / name, notices / name)
    write_json_atomic(receipt_path, {"version": VERSION, "source_url": SOURCE_URL,
                                    "source_sha256": SHA256, "config": args.config,
                                    "platform": system,
                                    "configure": configure, "upstream_tests": "passed",
                                    "sha256": {name: digest(prefix / name) for name in artifacts}})
    print(f"OpenSSL SDK ready: {prefix}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, tarfile.TarError) as error:
        raise SystemExit(str(error)) from error
