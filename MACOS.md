# macOS (native build)

**Experimental: nothing here has been compiled or run yet.** This port has never been built for macOS, and it has not
run on a Mac. The build files below are a first version, so expect compile and link errors.

The plan is an x86-64 native build with no Wine: it runs directly on Intel Macs and through Rosetta 2 on Apple Silicon.
Part of the code is x86-64 only for now, so there is no arm64 build. [DXMT's build notes](https://github.com/3Shain/dxmt/blob/main/docs/DEVELOPMENT.md)
say Intel Macs and Rosetta 2 are being deprecated, so this route may not last.

- **Graphics:** the same Direct3D 11 renderer, on [DXMT](https://github.com/3Shain/dxmt) (D3D11 on Metal) built as native libraries.
- **Shaders:** vkd3d-shader compiles the HLSL to DXBC, and DXMT converts that for Metal.
- **Window, input and audio:** SDL3 from the ReXGlue SDK's macOS package.

## Status (2026-10-31)

| Check | Result |
| --- | --- |
| Port compiled for macOS | **Not tested** |
| DXMT and vkd3d-shader built by `tools/macos/build-deps.sh` | **Not tested** |
| `native_metal_bridge.m` presenting into the SDL window | **Not tested** |
| Run on a Mac, including graphics, sound and controllers | **Not tested** |

Known risks:

- DXMT's main use is under Wine, so its native window path is little used. It looks for Wine's Mac driver, and
  `native_metal_bridge.m` stands in for that driver. Its structures must match the DXMT commit pinned in `build-deps.sh`.
- DXMT's own `windows.h` replaces the DXVK headers used on Linux, so some types may need adjusting.
- DXMT's native window functions return a fixed 1024x1024 size if the swap chain is created without a size.
- Performance on a Mac is unknown.

## Requirements

- macOS 14 or later, with Xcode 16+ and its Metal toolchain.
- On Apple Silicon, Rosetta 2: `softwareupdate --install-rosetta`
- An x86-64 Homebrew in `/usr/local`, also on Apple Silicon:
  `arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"`
- Packages: `arch -x86_64 /usr/local/bin/brew install cmake ninja meson pkg-config autoconf automake libtool bison flex spirv-headers mingw-w64 python llvm@20 fontconfig freetype`
- Your own game files, as for the other platforms.

## Build and run

```sh
tools/macos/build-deps.sh     # once: LLVM 15, DXMT and vkd3d-shader; this takes a long time
D=~/.local/share/native-ports-macos
arch -x86_64 /usr/local/bin/python3 tools/build_local.py --game-files game_files \
  --dxmt-dir $D/dxmt --vkd3d-dir $D/vkd3d
arch -x86_64 /usr/local/bin/python3 tools/play.py --game-files game_files
```

The libraries go next to the executable. Do not share the build: it contains code generated from your disc.
