# Battlefield 2: Modern Combat — Native PC Port | No Emulated GPU or Audio Hardware

> [!WARNING]
> This repository does **not** include the game, an ISO/XEX, EA assets, generated game code or a precompiled executable.
> You need your own Xbox 360 USA disc. See [the full notice](#notice) at the end.

An experimental port of the Xbox 360 version of Battlefield 2: Modern Combat, built through static recompilation with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), with its own Direct3D 11 renderer, its own XMA audio pipeline and a modern control layout. **This public repository is a source and local-build kit, not a ready-to-play game download.** The game's PowerPC code is translated locally from your own verified disc; no game executable or generated game code is distributed here.

## Status

- Tested on Windows 11 x64 through the first single-player mission ("In and Out"), including combat. Performance varies substantially with the scene; 60 fps gameplay is not guaranteed. The latest work reduces CPU costs in the renderer without lowering resolution, filtering or effects.
- Graphics: the game's Direct3D calls are intercepted and drawn by a native Direct3D 11 renderer; Xenos shader microcode is translated to HLSL. No GPU command processor or emulation plugin.
- Audio: the XMA and XAudio exports are reimplemented on the host (FFmpeg's XMA decoder, own render driver, SDL3 output). The mix carries real signal, but audio has not yet been checked by ear.
- The Xbox 360 kernel and XAM services come from the ReXGlue runtime as an operating-system compatibility layer.
- Mouse look turns the camera directly (it does not go through the emulated right stick, so there is no stick curve, acceleration or turn-rate cap); aiming follows the right mouse button reliably.
- In-game PC options (F1): display mode, internal resolution, vsync, frame limit, FXAA, MSAA (off, 2x, 4x or 8x; applies at once), anisotropic filtering, aspect, volume.
- Known issues: ground and scenery textures shimmer while the camera moves (under investigation; likely the missing MSAA); translucent streaks from some particle effects (when the player is hit or near explosions); the first mission load can take 1–2 minutes while shaders compile; the game's own aim-sensitivity option does not affect the mouse yet; the stance controls (crouch/prone toggles) are new and not fully tested; multiplayer is not supported.
- Windows x64 only for now.

## Why a native port instead of an emulator

An emulator such as [Xenia](https://xenia.jp) recreates the Xbox 360: it JIT-translates the PowerPC code at run time, emulates the Xenos GPU by processing the console's command buffers and models the audio hardware. That is the right approach for the whole library. This port takes a different route for one game:

- **Ahead-of-time code.** The game's PowerPC code becomes C++ compiled by a modern optimizing compiler before you play.
- **No GPU emulation in the hot path.** Draw calls go from the game's D3D library straight to Direct3D 11, instead of being turned into command packets and register writes that are emulated again.
- **Native audio** decoding and mixing on the PC.
- **PC-side controls in the port itself:** a modern shooter layout (aim on LT) and keyboard/mouse bindings, written as plain C++ hooks you can read and change.

The trade-off: it covers only this game and revision, and it is experimental. An emulator is the more general and better-tested option.

## Your game files

Only the **Xbox 360 USA** revision is accepted. Extract your own disc image into a new folder and validate it:

```sh
python tools/extract_iso.py "Battlefield 2 - Modern Combat (USA).iso" --output game_files
python tools/validate_game.py game_files
```

```text
default.xex  a2e791b578ef15420f3737b48999ca4285fdf85c85cb6732aac0e465ed9d5484
```

Do not upload the ISO, extracted files, generated code or the built executable.

## Build on Windows

Requirements: Python 3.10+, Visual Studio 2022 Build Tools (C++ workload, Windows SDK), LLVM/Clang 20 or newer (a complete install that includes the clang resource headers), CMake 3.25+ and Ninja.

```powershell
python tools/build_local.py --game-files game_files --llvm-dir "C:\Program Files\LLVM"
python tools/play.py --game-files game_files
```

`build_local.py` validates your files, downloads the pinned ReXGlue SDK nightly (`0.10.0.24-dev.gbd833a2`, SHA-256 checked) and applies the FPSCR patch in `patches/`, downloads the FFmpeg headers matching the SDK's `libavcodec`, translates your `default.xex` into private C++ under `.local/`, compiles the port, and removes any GPU emulation plugin from the output. `--reuse-generated` skips the translation on later builds.

## Controls

Modern layout after Battlefield 6's defaults (during gameplay only; menus keep the original A = select, B = back). Pass `--bf2_modern_controls=false` after `play.py` for the original console layout.

| Action | Controller | Keyboard / mouse |
| --- | --- | --- |
| Look | Right stick | Mouse |
| Aim (hold) | LT | Right mouse button |
| Fire | RT | Left mouse button |
| Move | Left stick | W A S D |
| Sprint | L3 | Shift |
| Jump (stands up when crouched / prone) | A | Space |
| Crouch / stand (toggle) | B (tap) | C |
| Crouch while held | — | Ctrl |
| Prone | B (hold) | Z |
| Reload | X (tap) | R |
| Use / enter vehicle | X (hold) or Y | E |
| Hotswap | LB | Q |
| Switch weapon | hold RB + right stick | hold F + mouse |
| Switch seat / call squad | R3 | V |
| Radio | D-pad | Arrow keys |
| Pause | Start | Enter / Esc |
| Back | Back | Tab (Backspace in menus) |
| PC options | Back + Start | F1 |
| Renderer debug capture | — | F12 |

Mouse options: `--bf2_mouse_sensitivity=1.0`, `--bf2_mouse_invert=true`, `--bf2_mouse_look=false --mnk_mouse=true` (the SDK's mouse-as-right-stick instead). F12 writes the next frame (draw trace, render targets and every sampled texture with its mip levels) to `captures/<time>/` next to the executable, for bug reports.

## Settings

**F1** (or Back + Start on a controller), or the **PC SETTINGS** line added to the game's own OPTIONS submenu (main menu, pause menus and campaign screen), opens the PC settings over the game. On the game's menus a small "PC settings" button is also shown (`botao_menu=0` in `bf2mc-pc.ini` hides it), and the main menu's "XBOX LIVE GAME" line reads "ONLINE GAME" (`menu_no_jogo=0` keeps the original text and the OPTIONS line out). The settings screen uses the game's menu look, decoded at run time from your own disc archive (no assets are shipped; `PCS_THEME=0` uses the plain theme). Options: display mode, monitor, refresh rate, VSync, frame limit, internal resolution, FXAA, MSAA (off, 2x, 4x or 8x; applies at once), anisotropic filtering, graphics API, graphics card, FPS counter and volume. Settings are saved in `bf2mc-pc.ini` next to the executable. **Graphics API**: DirectX 11, or Vulkan through DXVK (restart); if something looks wrong or stutters, try the other one. **Frame rate**: 30, 60, the monitor's rate, or experimental **no limit**. Select **VSync off** and **no limit** in F1 to unlock presentation. Physics, animation and audio timing above 60 fps have not been fully validated; select 60 if they behave incorrectly. Uncapping does not guarantee that the machine can render faster. Disabling VSync can introduce tearing.

## Renderer profiling and regressions

For automated menu tests using the default keyboard bindings, `BF2_TEST_MENU_TAPS=1` retains short Enter/Space/Backspace/arrow events for 120 ms so guest polling can observe them. It is off by default and does not change gameplay input. Set `BF2_PERF=1` and `PCS_FPS_LOG=1` before launching to log FPS and CPU costs of drawing, constant uploads, vertex synchronization and texture hashing/decoding/uploads. The CPU phase timings are nested and are not GPU timestamps. Avoid periodic image captures during throughput measurements, because GPU readback causes stalls.

The renderer keeps native triangle/line strips, skips identical constant uploads and pipeline-state changes, hashes textures with persistent workers, and copies linear texture rows with exact SSE2 endian conversion. Full texture data is still checked; no frames, textures or effects are deliberately dropped. F12 captures also include cubemap faces. An optional `step-region.txt` next to the executable, containing `x y width height` (up to 64×64), selects a per-draw diagnostic region on the next F12 capture.

Game-free tests for endian conversion, state invalidation and worker synchronization:

```sh
c++ -std=c++23 -O2 -pthread tests/native_regression.cpp -o native-regression
./native-regression
```

A gameplay profiling session also terminated with an invalid guest-function dispatch; its cause remains under investigation. Stability is not verified. FXC uses IEEE strictness to retain translated NaN/Inf checks; the disk shader cache includes the compilation flags. Windows developers can run `tests\run-shader-regression.cmd` (Visual Studio C++ and Windows SDK) for game-free D3D11 WARP arithmetic, pixel-input NaN compilation and position-invariance checks.

Position-export DP4 instructions now accumulate permuted terms in constant-component order, with precise position output. The terrain depth and color variants previously produced different positions; replaying 3,574 captured vertices now gives bit-identical positions, and the first mission's snow no longer shows the ground pattern in the tested view. The WARP regression exercises 48 translated permutations and confirms that the former order fails a cancellation case. No effect or depth prepass is intentionally removed. The initial deferred index-buffer map also now uses DISCARD, fixing a startup error confirmed by the D3D11 debug layer. There is no controlled same-camera FPS comparison or full-game image-quality validation.

Vertex fetches now zero each word outside the buffer's declared range before unpacking, including mini fetches and partially valid elements. A first-mission foliage draw requested 10,508 vertices from buffers holding 7,008; the excess read adjacent memory and produced giant translucent colored geometry. Captured-input replay preserves all 7,008 valid positions bit-for-bit and makes the 3,500 excess vertices degenerate. The user confirmed the wall is gone in the tested scene, with the foliage shader still enabled. The shader regression script also runs 84 synthetic WARP bounds cases with legacy negative controls. A short, changing-camera profiling interval after shader compilation recorded five FPS samples of 35–52 (median 47), VSync off and no frame limit. This is not a controlled speedup measurement or a stability guarantee.

## Credits and licenses

- Port integration, native renderer, audio and controls: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): Tom Clay and contributors, BSD-3-Clause; derived from [Xenia](https://github.com/xenia-project/xenia) (Ben Vanik and contributors). External dependency, downloaded by the build script; license in `licenses/`. Shader-microcode semantics, texture swizzles and XMA field layouts follow the SDK's Xenia-derived headers.
- [FFmpeg](https://ffmpeg.org) (Xenia fork by wmarti), LGPL-2.1: the SDK's prebuilt `libavcodec`/`libavutil` and their public headers, downloaded at build time.
- Microsoft Visual C++ runtime, Windows SDK and Direct3D: Microsoft; obtained separately.
- Battlefield 2: Modern Combat, its game code, trademarks and assets belong to Electronic Arts / DICE and the respective rights holders. This project is not affiliated with or endorsed by EA, DICE or Microsoft.

See [NOTICE.md](NOTICE.md) for the publication boundary and [LICENSE](LICENSE) for the license of the port's own code.

---

<a id="notice"></a>

**⚠️ THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XEX, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED XBOX 360 USA GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS. ⚠️**
