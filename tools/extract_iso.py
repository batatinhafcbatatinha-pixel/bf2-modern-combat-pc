"""List or extract the files of your own Xbox / Xbox 360 disc image (XDVDFS) into a new folder."""
import argparse
from pathlib import Path
import struct
import sys

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
# Game partition offsets: plain XISO, XGD2 (most Xbox 360 discs), XGD3, XGD1 (original Xbox redump).
PARTITIONS = (0, 0xFD90000, 0x2080000, 0x18300000)
FORBIDDEN = set('<>:"/\\|?*')


def find_partition(iso):
    for base in PARTITIONS:
        iso.seek(base + 32 * SECTOR)
        header = iso.read(SECTOR)
        if header[:20] == MAGIC and header[0x7EC:0x800] == MAGIC:
            root_sector, root_size = struct.unpack_from('<II', header, 20)
            return base, root_sector, root_size
    raise ValueError('No XDVDFS volume found: is this an Xbox or Xbox 360 game disc image?')


def walk(iso, base, sector, size, prefix=''):
    """Yield (relative path, start sector, byte count, is_directory) for every entry."""
    if size == 0:
        return
    iso.seek(base + sector * SECTOR)
    table = iso.read(size)
    pending, seen = [0], set()
    while pending:
        offset = pending.pop()
        if offset in seen or offset + 14 > len(table):
            continue
        seen.add(offset)
        left, right, start, length, attributes, name_length = struct.unpack_from('<HHIIBB', table, offset)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        raw = table[offset + 14:offset + 14 + name_length]
        name = raw.decode('ascii', 'strict')
        if not name or name in ('.', '..') or any(c in FORBIDDEN or ord(c) < 32 for c in name):
            raise ValueError(f'Unsafe file name in image: {raw!r}')
        path = prefix + name
        directory = bool(attributes & 0x10)
        yield path, start, length, directory
        if directory:
            yield from walk(iso, base, start, length, path + '/')
        for child in (left, right):
            if child and child != 0xFFFF:
                pending.append(child * 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', type=Path)
    parser.add_argument('--output', type=Path, help='New, empty folder (omit to only list the files)')
    args = parser.parse_args()
    with args.iso.open('rb') as iso:
        base, root_sector, root_size = find_partition(iso)
        entries = list(walk(iso, base, root_sector, root_size))
        files = [e for e in entries if not e[3]]
        print(f'XDVDFS at 0x{base:X}: {len(files)} files, {sum(e[2] for e in files):,} bytes')
        if not args.output:
            for path, _, length, directory in entries:
                print(('<dir> ' if directory else f'{length:>12,} ') + path)
            return
        out = args.output.resolve()
        if out.exists() and any(out.iterdir()):
            parser.error('the output folder must be new or empty')
        out.mkdir(parents=True, exist_ok=True)
        for path, start, length, directory in entries:
            target = (out / path).resolve()
            if out not in target.parents:
                raise ValueError(f'Path escapes the output folder: {path}')
            if directory:
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            iso.seek(base + start * SECTOR)
            left = length
            with target.open('wb') as stream:
                while left:
                    chunk = iso.read(min(left, 16 << 20))
                    if not chunk:
                        raise ValueError(f'Image is truncated inside {path}')
                    stream.write(chunk)
                    left -= len(chunk)
        print('Extracted to', out)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, UnicodeDecodeError) as error:
        print('Extraction failed:', error, file=sys.stderr)
        sys.exit(1)
