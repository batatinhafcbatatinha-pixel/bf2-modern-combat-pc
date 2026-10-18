"""Audit every tracked file against the publication allowlist and optionally build the source-kit ZIP.

Rejects game executables, disc images, assets, generated translations, binaries and oversized
files. It is an engineering safeguard, not a legal guarantee.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
KIT = json.loads((ROOT / 'kit.json').read_text(encoding='utf-8'))
ALLOWED = {'.c', '.h', '.cpp', '.hpp', '.inc', '.hlsl', '.cmake', '.txt', '.md', '.py', '.json',
           '.toml', '.cmd', '.ps1', '.sh', '.yml', '.yaml', '.patch'}
ROOT_FILES = {'.gitignore', '.gitattributes', 'LICENSE', 'NOTICE'}
FORBIDDEN_PARTS = {'.local', 'game_files', 'generated', 'gen', 'out', 'output', 'build', 'dist',
                   'runs', 'analysis', '__pycache__', 'assets'}
# Executables/archives/images/audio: PE, XBE, XEX2, ELF, ZIP, PNG, RIFF, DDS, EA BIG, 7z, gzip.
MAGIC = (b'MZ', b'XBEH', b'XEX2', b'\x7fELF', b'PK\x03\x04', b'\x89PNG', b'RIFF', b'DDS ', b'BIGF',
         b'BIG4', b"7z\xbc\xaf", b'\x1f\x8b')
# Output of the xboxrecomp / ReXGlue translators (hand-written hooks use other macros).
# (Split so this file does not match itself.)
GENERATED_MARKERS = (b'#define RECOMP_' + b'GENERATED_CODE', b'DEFINE_REX_' + b'FUNC(', b'REX_FUNC_' + b'PROLOGUE()')
MAX_BYTES = 512 * 1024


def audited_files():
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode('utf-8').split('\0')
    files = []
    for name in filter(None, names):
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or any(part in FORBIDDEN_PARTS for part in path.parts):
            raise ValueError('Forbidden publication path: ' + name)
        source = ROOT / name
        if source.is_symlink() or not source.is_file():
            raise ValueError('Only regular files may be published: ' + name)
        if path.suffix.lower() not in ALLOWED and path.name not in ROOT_FILES and not path.name.startswith(('LICENSE', 'COPYING')):
            raise ValueError('Not an allowed source file: ' + name)
        payload = source.read_bytes()
        if b'\0' in payload or payload.startswith(MAGIC):
            raise ValueError('Binary content rejected: ' + name)
        if len(payload) > MAX_BYTES:
            raise ValueError('File too large for hand-written source (possible data dump): ' + name)
        if any(marker in payload for marker in GENERATED_MARKERS) and name not in KIT.get('audit_exempt', []):
            raise ValueError('Generated game code rejected: ' + name)
        files.append((name, payload))
    if not files:
        raise ValueError('No tracked files to audit')
    return sorted(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zip', type=Path)
    args = parser.parse_args()
    files = audited_files()
    if args.zip:
        destination = args.zip.resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        prefix = KIT['repo'] + '/'
        with zipfile.ZipFile(destination, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            manifest = []
            for name, payload in files:
                info = zipfile.ZipInfo(prefix + name, (2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, payload)
                manifest.append({'path': name, 'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()})
            archive.writestr(prefix + 'source-manifest.json', json.dumps(manifest, indent=2) + '\n')
        with zipfile.ZipFile(destination) as archive:
            if archive.testzip():
                raise ValueError('ZIP integrity failed')
        print('Source ZIP:', destination, destination.stat().st_size, 'bytes')
    print('PASS:', len(files), 'tracked source/license/documentation files; no game binaries or generated game code')


if __name__ == '__main__':
    main()
