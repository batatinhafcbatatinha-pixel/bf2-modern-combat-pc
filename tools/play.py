"""Launch the privately built port with your verified game files."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KIT = json.loads((ROOT / 'kit.json').read_text(encoding='utf-8'))
sys.path.insert(0, str(ROOT / 'tools'))
import validate_game  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-files', type=Path, required=True)
    parser.add_argument('--local-dir', type=Path, default=ROOT / '.local')
    parser.add_argument('extra', nargs=argparse.REMAINDER, help='Extra runtime options, e.g. --fullscreen=true')
    args = parser.parse_args()
    game_files = args.game_files.resolve()
    problems = validate_game.validate(game_files)
    if problems:
        parser.error('; '.join(problems))
    executable = args.local_dir.resolve() / 'build' / (KIT['target'] + ('.exe' if os.name == 'nt' else ''))
    if not executable.is_file():
        parser.error('Build first: python tools/build_local.py --game-files ' + str(game_files))
    command = [str(executable), '--game_data_root=' + str(game_files)] + KIT.get('play_args', []) + args.extra
    raise SystemExit(subprocess.call(command, cwd=executable.parent))


if __name__ == '__main__':
    main()
