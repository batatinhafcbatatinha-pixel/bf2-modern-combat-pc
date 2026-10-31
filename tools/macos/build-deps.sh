#!/usr/bin/env bash
# macOS dependencies of the port, built once for x86-64 (Intel Macs, or Rosetta 2 on Apple Silicon):
# DXMT (Direct3D 11 on Metal) with the LLVM 15 it needs, and vkd3d-shader (HLSL to DXBC).
#   tools/macos/build-deps.sh [prefix]     default prefix: ~/.local/share/native-ports-macos
# Result: <prefix>/dxmt and <prefix>/vkd3d. Needs Xcode 16+ with the Metal toolchain and an x86-64 Homebrew in
# /usr/local with: cmake ninja meson pkg-config autoconf automake libtool bison flex spirv-headers mingw-w64
set -euo pipefail
if [ "$(uname -m)" != x86_64 ]; then
  exec arch -x86_64 /bin/bash "$0" "$@"
fi
DXMT_COMMIT=3b252590cb
VKD3D_TAG=vkd3d-1.19
prefix="${1:-$HOME/.local/share/native-ports-macos}"
src="$prefix/src"
export PATH="/usr/local/bin:/usr/local/opt/bison/bin:/usr/local/opt/flex/bin:$PATH"
mkdir -p "$src"

if [ ! -f "$src/llvm/lib/libLLVMCore.a" ]; then
  [ -d "$src/llvm-project" ] || git clone --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git "$src/llvm-project"
  cmake -B "$src/llvm-build" -S "$src/llvm-project/llvm" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$src/llvm" -DCMAKE_OSX_ARCHITECTURES=x86_64 -DLLVM_HOST_TRIPLE=x86_64-apple-darwin \
    -DLLVM_ENABLE_ASSERTIONS=On -DLLVM_ENABLE_ZSTD=Off -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=Off \
    -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off
  cmake --build "$src/llvm-build"
  cmake --install "$src/llvm-build"
fi

if [ ! -f "$prefix/dxmt/lib/d3d11.dylib" ]; then
  if [ ! -d "$src/dxmt" ]; then
    git clone https://github.com/3Shain/dxmt.git "$src/dxmt"
    git -C "$src/dxmt" checkout "$DXMT_COMMIT"
    git -C "$src/dxmt" submodule update --init --recursive
  fi
  meson setup "$src/dxmt-build" "$src/dxmt" -Dnative_llvm_path="$src/llvm" --buildtype release
  meson compile -C "$src/dxmt-build"
  mkdir -p "$prefix/dxmt/lib" "$prefix/dxmt/include"
  find "$src/dxmt-build/src" -name '*.dylib' -exec cp {} "$prefix/dxmt/lib/" \;
  cp -R "$src/dxmt/include/native/windows" "$src/dxmt/include/native/directx" "$prefix/dxmt/include/"
fi

if [ ! -f "$prefix/vkd3d/lib/libvkd3d-shader.dylib" ]; then
  [ -d "$src/vkd3d" ] || git clone --depth 1 -b "$VKD3D_TAG" https://gitlab.winehq.org/wine/vkd3d.git "$src/vkd3d"
  mkdir -p "$src/tools"
  command -v widl >/dev/null || ln -sf "$(command -v x86_64-w64-mingw32-widl)" "$src/tools/widl"
  (
    cd "$src/vkd3d"
    export PATH="$src/tools:$PATH"
    [ -f configure ] || ./autogen.sh
    ./configure --prefix="$prefix/vkd3d" --disable-tests --disable-doxygen-doc --without-ncurses
    make -j"$(sysctl -n hw.ncpu)"
    make install
  )
fi
echo "DXMT:  $prefix/dxmt"
echo "vkd3d: $prefix/vkd3d"
