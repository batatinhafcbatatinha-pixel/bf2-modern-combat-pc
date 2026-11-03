"""Portable build tools and missing project parts for Setup.cmd, pinned and SHA-256 checked.

Reads setup.json next to the repository root:
- "tools": any of llvm, cmake, ninja, ghidra (Ghidra + the PS2 extension + a Java 21 JDK), downloaded into
  .local/tools; nothing is installed system-wide. A complete Clang 20+ already installed is used instead.
- "submodules": {path: {"url", "commit"}}; a ZIP download has these folders empty. With Git they are checked
  out; without it the pinned commit's archive is downloaded.
Prints KEY=VALUE lines for setup.ps1: PATH (folders to put first), LLVM, GHIDRA, JAVA_HOME.
`--check-pins` compares the submodule pins with the repository's gitlinks (run it before publishing).
"""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
GH = 'https://github.com/'
TOOLS = {
    'llvm': {
        'version': '22.1.2', 'sha256': '6550bcc308bf972f7f956001b73f6478da3d22f1ebd4b01653c978a6c7ff3698',
        'url': GH + 'llvm/llvm-project/releases/download/llvmorg-22.1.2/clang+llvm-22.1.2-x86_64-pc-windows-msvc.tar.xz',
        'check': 'bin/clang++.exe',
    },
    'cmake': {
        'version': '3.31.12', 'sha256': '0c4baa40f28b3f8225eb3fdf6946c987b4fe901403b4eaf2fbbd9378100aaa0c',
        'url': GH + 'Kitware/CMake/releases/download/v3.31.12/cmake-3.31.12-windows-x86_64.zip',
        'check': 'bin/cmake.exe',
    },
    'ninja': {
        'version': '1.13.2', 'sha256': '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65',
        'url': GH + 'ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip',
        'check': 'ninja.exe',
    },
    'jdk': {
        'version': '21.0.13', 'sha256': 'a4dbaa05fca4bd89d84584e58c459bd732efebb1722af80ab13c6516832004f9',
        'url': GH + 'adoptium/temurin21-binaries/releases/download/jdk-21.0.13%2B9/OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip',
        'check': 'bin/java.exe',
    },
    'ghidra': {
        'version': '12.1.4', 'sha256': 'ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db',
        'url': GH + 'NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip',
        'check': 'support/analyzeHeadless.bat',
    },
    'ghidra-ee': {  # ghidra-emotionengine-reloaded, the PlayStation 2 (R5900) extension for that Ghidra
        'version': '2.1.39', 'sha256': '088369c4d7de43cc95dbf254936dfed45eae5eddfcaf8b928189344bcc6a4dac',
        'url': GH + 'chaoticgd/ghidra-emotionengine-reloaded/releases/download/v2.1.39/'
                    'ghidra_12.1.4_PUBLIC_20261029_ghidra-emotionengine-reloaded.zip',
        'check': 'extension.properties',
    },
}
LLVM_NEEDED = ('clang.exe', 'clang++.exe', 'clang-cl.exe', 'lld-link.exe', 'llvm-ar.exe', 'llvm-rc.exe')


def log(text):
    print(text, file=sys.stderr, flush=True)


def clang_major(clang):
    try:
        out = subprocess.run([str(clang), '--version'], capture_output=True, text=True, timeout=30).stdout
    except OSError:
        return 0
    match = re.search(r'clang version (\d+)', out)
    return int(match.group(1)) if match else 0


def existing_llvm():
    candidates = [shutil.which('clang++'), Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'LLVM/bin/clang++.exe']
    for clang in filter(None, candidates):
        clang = Path(clang)
        # Partial installs exist (seen: clang without lld-link); the build needs the linker and tools too.
        complete = all((clang.parent / name).is_file() for name in LLVM_NEEDED)
        if clang.is_file() and complete and clang_major(clang) >= 20:
            log(f'Using Clang {clang_major(clang)} from {clang.parent}')
            return clang.parent.parent
        if clang.is_file():
            log(f'Ignoring the incomplete or old LLVM in {clang.parent}')
    return None


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, target, sha256=None):
    if target.is_file() and (sha256 is None or file_sha256(target) == sha256):
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + '.part')
    # Windows' own curl.exe uses the system certificate store fully (Python's ssl may miss intermediates).
    curl = shutil.which('curl.exe') if os.name == 'nt' else None
    if not (curl and subprocess.run([curl, '-L', '--fail', '--retry', '3', '--progress-bar', '-o', str(partial), url]).returncode == 0):
        request = urllib.request.Request(url, headers={'User-Agent': 'port-setup'})
        with urllib.request.urlopen(request, timeout=60) as response, partial.open('wb') as out:
            total = int(response.headers.get('Content-Length') or 0)
            done, shown = 0, 0.0
            while chunk := response.read(1 << 20):
                out.write(chunk)
                done += len(chunk)
                if time.monotonic() - shown > 2:
                    shown = time.monotonic()
                    log(f'  {done >> 20} / {total >> 20} MB' if total else f'  {done >> 20} MB')
    if sha256 and file_sha256(partial) != sha256:
        partial.unlink()
        raise ValueError(f'{url}: SHA-256 mismatch (download damaged or changed); run Setup again')
    partial.replace(target)


def extract(archive, folder):
    """Unpack into folder; an archive with a single top folder is unwrapped."""
    temp = folder.with_name(folder.name + '.extracting')
    shutil.rmtree(temp, ignore_errors=True)
    temp.mkdir(parents=True)
    if archive.name.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            z.extractall(temp)
    else:
        with lzma.open(archive) as xz, tarfile.open(fileobj=xz, mode='r|') as tar:
            try:
                tar.extractall(temp, filter='data')
            except TypeError:  # Python without extraction filters
                tar.extractall(temp)
    entries = list(temp.iterdir())
    inner = entries[0] if len(entries) == 1 and entries[0].is_dir() else temp
    folder.parent.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(folder, ignore_errors=True)
    inner.replace(folder)
    shutil.rmtree(temp, ignore_errors=True)


def ensure(name, folder):
    tool = TOOLS[name]
    if (folder / tool['check']).is_file():
        return folder
    archive = folder.parent / Path(tool['url'].replace('%2B', '+')).name
    log(f'Downloading {name} {tool["version"]} ...')
    download(tool['url'], archive, tool['sha256'])
    log(f'Unpacking {name} (this can take a few minutes) ...')
    extract(archive, folder)
    if not (folder / tool['check']).is_file():
        raise ValueError(f'{name}: {tool["check"]} missing after unpacking')
    archive.unlink()
    return folder


def ghidra(base):
    jdk = ensure('jdk', base / 'jdk-21')
    home = ensure('ghidra', base / f'ghidra-{TOOLS["ghidra"]["version"]}')
    # Extensions under Ghidra/Extensions of the install are loaded for every user, headless included.
    ensure('ghidra-ee', home / 'Ghidra/Extensions/ghidra-emotionengine-reloaded')
    # Point Ghidra at this JDK so it never asks for one.
    props = home / 'support/launch.properties'
    text = props.read_text(encoding='utf-8')
    line = 'JAVA_HOME_OVERRIDE=' + jdk.as_posix()
    if line not in text:
        props.write_text(re.sub(r'^JAVA_HOME_OVERRIDE=.*$', line, text, flags=re.M), encoding='utf-8')
    return home, jdk


def submodules(config):
    for path, pin in config.get('submodules', {}).items():
        folder = ROOT / path
        if folder.is_dir() and any(folder.iterdir()):
            continue
        if (ROOT / '.git').exists() and shutil.which('git'):
            log(f'Checking out {path} ...')
            subprocess.run(['git', 'submodule', 'update', '--init', '--recursive', '--', path], cwd=ROOT, check=True)
            continue
        owner_repo = pin['url'].removeprefix(GH).removesuffix('.git')
        log(f'Downloading {path} ({owner_repo} {pin["commit"][:10]}) ...')
        archive = ROOT / '.local/downloads' / f'{owner_repo.replace("/", "-")}-{pin["commit"]}.zip'
        download(f'https://codeload.github.com/{owner_repo}/zip/{pin["commit"]}', archive)
        extract(archive, folder)


def check_pins(config):
    """The pins in setup.json must equal the gitlinks, or a ZIP download would get other submodule versions."""
    out = subprocess.check_output(['git', 'ls-files', '-s'], cwd=ROOT, text=True)
    links = {line.split('\t')[1]: line.split()[1] for line in out.splitlines() if line.startswith('160000')}
    pins = {path: pin['commit'] for path, pin in config.get('submodules', {}).items()}
    if links != pins:
        raise ValueError(f'setup.json submodule pins {pins} differ from the gitlinks {links}')
    print('Submodule pins match the gitlinks')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dir', type=Path, default=ROOT / '.local/tools')
    parser.add_argument('--check-pins', action='store_true')
    args = parser.parse_args()
    config = json.loads((ROOT / 'setup.json').read_text(encoding='utf-8'))
    if args.check_pins:
        return check_pins(config)
    base = args.dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    submodules(config)
    wanted = config.get('tools', [])
    path, result = [], {}
    if 'llvm' in wanted:
        llvm = existing_llvm() or ensure('llvm', base / f'llvm-{TOOLS["llvm"]["version"]}')
        path.append(llvm / 'bin')
        result['LLVM'] = llvm
    for name in ('cmake', 'ninja'):
        if name in wanted:
            folder = ensure(name, base / f'{name}-{TOOLS[name]["version"]}')
            path.append(folder / ('bin' if name == 'cmake' else ''))
    if 'ghidra' in wanted:
        result['GHIDRA'], result['JAVA_HOME'] = ghidra(base)
    print('PATH=' + ';'.join(str(p) for p in path))
    for key, value in result.items():
        print(f'{key}={value}')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        log(f'Setup tools failed: {error}')
        sys.exit(1)
