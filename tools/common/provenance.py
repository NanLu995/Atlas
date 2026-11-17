import hashlib
from pathlib import Path
import subprocess


def require_generated_output(root: Path, path: Path):
    if path.resolve().is_relative_to(root.resolve()):
        relative = path.resolve().relative_to(root.resolve())
        result = subprocess.run(['git', 'check-ignore', '--quiet', '--', str(relative)], cwd=root)
        if result.returncode:
            raise RuntimeError('Generated output inside the repository must use ignored build/, bin/ '
                               'or .tmp/ directories; select a new output path')


def source_state(root: Path):
    names = subprocess.check_output(['git', 'ls-files', '--cached', '--others',
                                     '--exclude-standard', '-z'], cwd=root).split(b'\0')
    digest = hashlib.sha256()
    for raw in sorted(set(names)):
        if raw:
            path = root / raw.decode('utf-8')
            digest.update(raw + b'\0')
            digest.update(path.read_bytes() if path.is_file() else b'<missing>')
    return {'commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root,
                                               text=True).strip(),
            'source_sha256': digest.hexdigest()}
