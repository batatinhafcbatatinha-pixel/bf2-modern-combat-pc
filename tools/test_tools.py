"""Game-free tests: the ISO extractor and the file validator, using a synthetic disc image."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import extract_iso  # noqa: E402
import validate_game  # noqa: E402

SECTOR = extract_iso.SECTOR


def entry(left, right, sector, size, attributes, name):
    raw = struct.pack('<HHIIBB', left, right, sector, size, attributes, len(name)) + name
    return raw + b'\xff' * (-len(raw) % 4)


def synthetic_iso(path, base=0, name=b'default.xex', payload=b'not a game'):
    """Root {name, data/} with data/{readme.txt}; files at sectors 40 and 41."""
    sub = entry(0, 0, 41, 5, 0x20, b'readme.txt')
    first_length = len(entry(0, 0, 40, len(payload), 0x20, name))
    data = entry(0, 0, 34, len(sub), 0x10, b'data')
    # The root node's right subtree is the "data" directory entry.
    root = entry(0, first_length // 4, 40, len(payload), 0x20, name) + data
    image = bytearray(base + 42 * SECTOR)
    header = extract_iso.MAGIC + struct.pack('<II', 33, len(root))
    image[base + 32 * SECTOR:base + 32 * SECTOR + len(header)] = header
    image[base + 32 * SECTOR + 0x7EC:base + 32 * SECTOR + 0x800] = extract_iso.MAGIC
    image[base + 33 * SECTOR:base + 33 * SECTOR + len(root)] = root
    image[base + 34 * SECTOR:base + 34 * SECTOR + len(sub)] = sub
    image[base + 40 * SECTOR:base + 40 * SECTOR + len(payload)] = payload
    image[base + 41 * SECTOR:base + 41 * SECTOR + 5] = b'hello'
    path.write_bytes(bytes(image))


class ExtractorTests(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, '-I', str(TOOLS / 'extract_iso.py'), *map(str, args)],
                              capture_output=True, text=True)

    def test_lists_and_extracts_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            iso, out = Path(temp, 'disc.iso'), Path(temp, 'out')
            synthetic_iso(iso)
            listing = self.run_tool(iso)
            self.assertEqual(listing.returncode, 0, listing.stderr)
            self.assertIn('2 files', listing.stdout)
            result = self.run_tool(iso, '--output', out)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((out / 'default.xex').read_bytes(), b'not a game')
            self.assertEqual((out / 'data/readme.txt').read_bytes(), b'hello')

    def test_refuses_non_empty_output(self):
        with tempfile.TemporaryDirectory() as temp:
            iso, out = Path(temp, 'disc.iso'), Path(temp, 'out')
            synthetic_iso(iso)
            out.mkdir()
            (out / 'keep.txt').write_text('mine')
            self.assertNotEqual(self.run_tool(iso, '--output', out).returncode, 0)

    def test_rejects_path_traversal_names(self):
        with tempfile.TemporaryDirectory() as temp:
            iso = Path(temp, 'disc.iso')
            synthetic_iso(iso, name=b'..')
            self.assertNotEqual(self.run_tool(iso, '--output', Path(temp, 'out')).returncode, 0)

    def test_rejects_non_xdvdfs(self):
        with tempfile.TemporaryDirectory() as temp:
            iso = Path(temp, 'random.iso')
            iso.write_bytes(b'\0' * (40 * SECTOR))
            self.assertNotEqual(self.run_tool(iso).returncode, 0)


class ValidatorTests(unittest.TestCase):
    def test_rejects_wrong_or_missing_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            problems = validate_game.validate(root)
            self.assertTrue(problems)
            for name in validate_game.KIT['required_files']:
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(b'not the game')
            self.assertTrue(any('SHA-256' in p for p in validate_game.validate(root)))

    def test_kit_metadata(self):
        kit = json.loads((TOOLS.parent / 'kit.json').read_text(encoding='utf-8'))
        for digest in kit['required_files'].values():
            self.assertRegex(digest, '^[0-9a-f]{64}$')


if __name__ == '__main__':
    unittest.main()
