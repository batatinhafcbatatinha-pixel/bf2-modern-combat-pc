"""Check that a folder holds the exact game files this port supports (SHA-256 of every executable)."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
KIT = json.loads((ROOT / 'kit.json').read_text(encoding='utf-8'))


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def validate(game_files):
    """Return a list of problems; empty means the folder is supported."""
    root = Path(game_files)
    problems = []
    if not root.is_dir():
        return [f'{root} is not a folder']
    for name, expected in KIT['required_files'].items():
        path = root / name
        if not path.is_file():
            problems.append(f'missing {name}')
        elif sha256(path) != expected:
            problems.append(f'{name} has a different SHA-256 (unsupported region/revision or modified file)')
    for name in KIT.get('required_paths', []):
        if not (root / name).exists():
            problems.append(f'missing {name} (extract the whole disc, not only the executable)')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game_files', type=Path, help='Folder with the extracted disc contents')
    args = parser.parse_args()
    problems = validate(args.game_files)
    if problems:
        for problem in problems:
            print('FAIL:', problem)
        print(f'Supported: {KIT["game"]} ({KIT["revision"]}). See README.md.')
        sys.exit(1)
    print(f'PASS: {KIT["game"]} ({KIT["revision"]}): {len(KIT["required_files"])} executable(s) verified')


if __name__ == '__main__':
    main()
