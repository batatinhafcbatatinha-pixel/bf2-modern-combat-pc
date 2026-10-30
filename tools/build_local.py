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
if os.name != 'nt' and 'sdk_linux' in KIT:
    SDK = {**SDK, **KIT['sdk_linux']}  # same SDK build, Linux package; the header patches still apply
FFMPEG = KIT['ffmpeg']
DXVK = KIT.get('dxvk')


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


def prepare_dxvk(local):
    """DXVK (zlib license): the settings menu's Vulkan option loads its d3d11.dll/dxgi.dll from the vulkan folder."""
    target = local / 'dxvk'
    if (target / 'x64/d3d11.dll').is_file():
        return target
    archive = download(DXVK['url'], local / 'downloads' / Path(DXVK['url']).name, DXVK['sha256'])
    with tarfile.open(archive) as bundle:
        for member in bundle.getmembers():
            parts = Path(member.name).parts
            if member.isfile() and parts[1:] in (('x64', 'd3d11.dll'), ('x64', 'dxgi.dll'), ('LICENSE',)):
                destination = target.joinpath(*parts[1:])
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(bundle.extractfile(member).read())
    license_text = ROOT / 'licenses/DXVK-LICENSE.txt'
    if license_text.is_file():
        shutil.copy2(license_text, target / 'LICENSE')
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


def linux_runtime_files(build, sdk, args):
    """Next to the executable (its RPATH is $ORIGIN): the SDK runtime, DXVK native and vkd3d-shader. DXVK's SDL3
    WSI opens libSDL3.so.0; the SDK runtime carries SDL3 itself, so that name points at the runtime and both share
    one SDL (and one window)."""
    names = {'librexruntime', 'librexruntimerd', 'librexruntimed', 'libTracyClient', 'libTracyClientrd', 'libTracyClientd'}
    needed = subprocess.run(['ldd', str(build / KIT['target'])], capture_output=True, text=True).stdout
    for lib in sorted((sdk / 'lib').glob('*.so')):
        if lib.stem in names and lib.name in needed:
            shutil.copy2(lib, build / lib.name)
    runtime = next((build / n for n in ('librexruntimerd.so', 'librexruntime.so', 'librexruntimed.so') if (build / n).is_file()), None)
    dxvk = args.dxvk_native_dir.resolve()
    for name in ('libdxvk_d3d11.so.0', 'libdxvk_dxgi.so.0'):
        source = next((p for p in (dxvk / 'lib' / name, dxvk / 'lib64' / name) if p.exists()), None)
        if source:
            shutil.copy2(source, build / name)
    if args.vkd3d_dir:
        for d in ('lib', 'lib64', 'lib/x86_64-linux-gnu'):
            source = args.vkd3d_dir.resolve() / d / 'libvkd3d-shader.so.1'
            if source.exists():
                shutil.copy2(source, build / source.name)
                break
    if runtime:
        link = build / 'libSDL3.so.0'
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(runtime.name)


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
    parser.add_argument('--dxvk-native-dir', type=Path, help='Linux: DXVK native prefix (include/dxvk, lib/libdxvk_d3d11.so)')
    parser.add_argument('--vkd3d-dir', type=Path, help='Linux: vkd3d prefix with libvkd3d-shader 1.19+ (lib/pkgconfig)')
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
    if os.name != 'nt':
        # The SDK's headers need Clang 20; distributions often install it as clang-20.
        clang = shutil.which('clang-20', path=env.get('PATH')) or clang
        clangxx = shutil.which('clang++-20', path=env.get('PATH')) or clangxx
        if not args.dxvk_native_dir:
            raise ValueError('Linux: pass --dxvk-native-dir (DXVK native 3.1.1, D3D11 on Vulkan) and --vkd3d-dir')
        if args.vkd3d_dir:
            pkg = [str(args.vkd3d_dir.resolve() / d / 'pkgconfig') for d in ('lib', 'lib64', 'lib/x86_64-linux-gnu')]
            env['PKG_CONFIG_PATH'] = os.pathsep.join(pkg + [env.get('PKG_CONFIG_PATH', '')])
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
         '-DCMAKE_PREFIX_PATH=' + sdk.as_posix(),
         *(['-DDXVK_DIR=' + prepare_dxvk(local).as_posix()] if DXVK and os.name == 'nt' else []),
         *(['-DDXVK_NATIVE_DIR=' + args.dxvk_native_dir.resolve().as_posix()] if os.name != 'nt' else [])], local, env)
    run([cmake, '--build', build, '--parallel', str(args.jobs), '--target', KIT['target'], *KIT.get('extra_targets', [])], local, env)
    # The port renders and plays audio natively: never ship the SDK's GPU emulation plugin next to it.
    for plugin in [*build.glob('rexgpu-*'), *build.glob('librexgpu-*')]:
        plugin.unlink()
    if os.name != 'nt':
        linux_runtime_files(build, sdk, args)
    print('Built privately in', build)
    print('Play with: python tools/play.py --game-files', game_files)
    print('Do not upload .local/, the generated code or the executable.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('Build failed:', error, file=sys.stderr)
        sys.exit(1)
