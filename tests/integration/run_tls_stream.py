"""Run TLS tests with newly generated, isolated certificate authorities and keys."""

import os
import secrets
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    openssl, executable = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="atlas-tls-") as directory:
        root = Path(directory)
        config = root / "openssl.cnf"
        config.write_text("[req]\ndistinguished_name=dn\n[dn]\n")
        env = os.environ.copy()
        env["OPENSSL_CONF"] = str(config)

        def run(*args):
            result = subprocess.run([openssl, *args], cwd=root, env=env, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError("TLS fixture certificate generation failed: " + result.stderr)

        for name in ["ca", "other"]:
            run("req", "-x509", "-newkey", "rsa:2048", "-noenc", "-keyout", name + ".key",
                "-out", name + ".pem", "-subj", "/CN=Atlas isolated test " + name,
                "-days", "2", "-addext", "basicConstraints=critical,CA:TRUE")
        run("req", "-newkey", "rsa:2048", "-noenc", "-keyout", "server.key", "-out", "server.csr",
            "-subj", "/CN=unused-common-name")
        (root / "key-passphrase").write_text(secrets.token_hex(32))
        run("pkey", "-in", "server.key", "-out", "encrypted.key", "-aes-256-cbc",
            "-passout", "file:key-passphrase")
        (root / "extensions").write_text(
            "subjectAltName=DNS:atlas.test,IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\n"
            "keyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
        for name in ["server", "expired"]:
            validity = ["-days", "2"] if name == "server" else [
                "-not_before", "20200101000000Z", "-not_after", "20200102000000Z"]
            run("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                "-set_serial", "2", "-out", name + ".pem", *validity, "-extfile", "extensions")
        env["ATLAS_TEST_TLS_DIR"] = str(root)
        return subprocess.run([executable], env=env).returncode


if __name__ == "__main__":
    raise SystemExit(main())
