"""Translate your own verified game executables with the ReXGlue SDK and build the native PC port.

Everything produced (SDK download, generated C++, build) stays in --local-dir (default .local/),
which is excluded from version control. Never upload it.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
KIT = json.loads((ROOT / 'kit.json').read_text(encoding='utf-8'))
sys.path.insert(0, str(ROOT / 'tools'))
import validate_game  # noqa: E402

SDK = KIT['sdk']
FFMPEG = KIT['ffmpeg']


def run(args, cwd, env):
    print('Running:', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def download(url, destination, sha256):
    if not destination.is_file():
        print('Downloading', url, flush=True)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url) as response:
            payload = response.read()
        destination.write_bytes(payload)
    if sha256 and hashlib.sha256(destination.read_bytes()).hexdigest() != sha256:
        destination.unlink()
        raise ValueError(f'{destination.name}: SHA-256 mismatch; download removed, try again')
    return destination


def prepare_sdk(local, sdk_dir):
    """Return the SDK prefix (the folder with bin/rexglue.exe), downloading the pinned build if needed."""
    if sdk_dir:
        prefix = sdk_dir.resolve()
    else:
        prefix = local / 'sdk' / SDK['folder']
        if not (prefix / 'bin').is_dir():
            archive = download(SDK['url'], local / 'downloads' / Path(SDK['url']).name, SDK['sha256'])
            with zipfile.ZipFile(archive) as bundle:
                bundle.extractall(local / 'sdk')
    if not (prefix / 'lib/cmake/rexglue').is_dir():
        raise ValueError(f'{prefix} is not a ReXGlue SDK install (lib/cmake/rexglue missing)')
    # Keep host floating-point exceptions masked when the guest writes FPSCR (see patches/).
    for patch in SDK.get('patches', []):
        header = prefix / patch['file']
        text = header.read_text(encoding='utf-8')
        if patch['new'] not in text:
            if patch['old'] not in text:
                raise ValueError(f'Unexpected SDK version: cannot apply {patch["name"]} to {header}')
            header.write_text(text.replace(patch['old'], patch['new'], 1), encoding='utf-8')
            print('Applied SDK patch', patch['name'])
    return prefix


def prepare_ffmpeg_headers(local):
    """Public libavcodec/libavutil headers of the FFmpeg fork the SDK's prebuilt libraries come from (LGPL-2.1)."""
    target = local / 'ffmpeg-headers'
    if (target / 'libavcodec/avcodec.h').is_file():
        return target
    archive = download(FFMPEG['url'], local / 'downloads' / ('ffmpeg-' + FFMPEG['commit'] + '.tar.gz'), FFMPEG.get('sha256'))
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            parts = Path(member.name).parts
            if len(parts) < 3 or parts[1] not in ('libavcodec', 'libavutil', 'COPYING.LGPLv2.1', 'LICENSE.md'):
                continue
            if not member.isfile() or not member.name.endswith(('.h', 'LGPLv2.1', 'LICENSE.md')):
                continue
            destination = target.joinpath(*parts[1:])
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(bundle.extractfile(member).read())
    return target


def prepare_project(local, game_files, ffmpeg):
    source = local / 'source'
    if source.exists():
        shutil.rmtree(source / KIT['project_dir'] / 'src', ignore_errors=True)
    for name in KIT['copy']:
        origin = ROOT / name
        if origin.is_dir():
            shutil.copytree(origin, source / name, dirs_exist_ok=True)
        else:
            (source / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(origin, source / name)
    project = source / KIT['project_dir']
    manifest = project / KIT['manifest']
    manifest.write_text(manifest.read_text(encoding='utf-8').replace('@GAME_FILES@', game_files.as_posix()), encoding='utf-8')
    headers = source / KIT['ffmpeg_dest']
    shutil.copytree(ffmpeg, headers, dirs_exist_ok=True)
    return project


def developer_environment():
    """MSVC headers/libraries for clang on Windows (from the Visual Studio vcvars64 script)."""
    if os.name != 'nt' or os.environ.get('VCToolsInstallDir'):
        return {key.upper() if os.name == 'nt' else key: value for key, value in os.environ.items()}
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise ValueError('Visual Studio 2022 (Build Tools) with the C++ workload is required')
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                       'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                       '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not vcvars.is_file():
        raise ValueError('vcvars64.bat not found; install the "Desktop development with C++" workload')
    output = subprocess.check_output(f'"{vcvars}" >nul && set', shell=True, text=True)
    # Windows variable names are case-insensitive ("Path"); keep one spelling.
    return {key.upper(): value for key, value in (line.split('=', 1) for line in output.splitlines() if '=' in line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-files', type=Path, required=True, help='Extracted disc folder (see README)')
    parser.add_argument('--local-dir', type=Path, default=ROOT / '.local')
    parser.add_argument('--sdk-dir', type=Path, help=f'Existing ReXGlue SDK {SDK["version"]} install (default: download the pinned build)')
    parser.add_argument('--llvm-dir', type=Path, help='LLVM install folder containing bin/clang++ (default: PATH)')
    parser.add_argument('--reuse-generated', action='store_true', help='Skip the translation step after a completed one')
    parser.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    args = parser.parse_args()

    game_files = args.game_files.resolve()
    problems = validate_game.validate(game_files)
    if problems:
        raise ValueError('Unsupported game files: ' + '; '.join(problems))
    local = args.local_dir.resolve()
    local.mkdir(parents=True, exist_ok=True)
    env = developer_environment()
    if args.llvm_dir:
        env['PATH'] = str(args.llvm_dir.resolve() / 'bin') + os.pathsep + env.get('PATH', '')
    exe = '.exe' if os.name == 'nt' else ''
    clang = shutil.which('clang' + exe, path=env.get('PATH'))
    clangxx = shutil.which('clang++' + exe, path=env.get('PATH'))
    cmake = shutil.which('cmake', path=env.get('PATH'))
    ninja = shutil.which('ninja', path=env.get('PATH'))
    if not (clang and clangxx and cmake and ninja):
        raise ValueError('clang, clang++, cmake and ninja must be on PATH (or pass --llvm-dir)')

    sdk = prepare_sdk(local, args.sdk_dir)
    project = prepare_project(local, game_files, prepare_ffmpeg_headers(local))
    stamp = local / 'generation-complete.json'
    if not args.reuse_generated or not stamp.is_file():
        stamp.unlink(missing_ok=True)
        run([sdk / 'bin' / ('rexglue' + exe), 'codegen', KIT['manifest']], project, env)
        stamp.write_text(json.dumps({'files': KIT['required_files']}) + '\n', encoding='utf-8')
    build = local / 'build'
    run([cmake, '-S', project, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DCMAKE_C_COMPILER=' + clang, '-DCMAKE_CXX_COMPILER=' + clangxx,
         '-DCMAKE_PREFIX_PATH=' + sdk.as_posix()], local, env)
    run([cmake, '--build', build, '--parallel', str(args.jobs), '--target', KIT['target'], *KIT.get('extra_targets', [])], local, env)
    # The port renders and plays audio natively: never ship the SDK's GPU emulation plugin next to it.
    for plugin in build.glob('rexgpu-*'):
        plugin.unlink()
    print('Built privately in', build)
    print('Play with: python tools/play.py --game-files', game_files)
    print('Do not upload .local/, the generated code or the executable.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('Build failed:', error, file=sys.stderr)
        sys.exit(1)
