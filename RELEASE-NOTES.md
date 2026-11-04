Source kit only: no game, no XEX, no generated code, no game executable; `Setup.exe` is our installer and launcher. Bring your own Xbox 360 USA disc.

**v0.2.5**

- **`Setup.exe`**: download it alone and run it. It fetches this repository's source, shows on its *Dependencies* tab what your PC has and what is missing (Python, C++ Build Tools, LLVM/CMake/Ninja, your game files, the built game) with the space each part needs, installs each part or everything, and builds the game. Afterwards it is the launcher: *Play*, and *PC settings* with the same options as the F1 menu, saved before the game starts. Dark and light themes (the default follows Windows). It is built by GitHub from `tools/launcher`; nothing from the game is in it. Windows may warn because it is not signed.
- `Setup.cmd` still works as before; `tools/setup.ps1` gained `-Only python|buildtools|tools|game`.

`Setup.exe` was tested with the FIFA Street 3 kit (stand-in install steps, Play, settings file, real kit download up to the build tools); for this game only its window and settings were checked, not a build through it.

**v0.2.4**

- **One-click `Setup.cmd`:** asks before installing anything, installs Python and Microsoft's C++ Build Tools through winget if missing, downloads pinned, SHA-256-checked LLVM/Clang, CMake and Ninja into `.local/tools` only, fetches missing parts, opens the game-file installer and builds the port; `Play-<game>.cmd` starts it. Run it again after a failure: finished steps are skipped.
- **README:** a *How to play* guide for people new to this, and the same section layout as the other ports.
- The source-kit ZIP now includes the game-file installer (`tools/port-installer`); `Install-Files.cmd` did not work from the ZIP before.

`Setup.cmd` has not been tried from a fresh download with this game yet (only with FIFA Street 3). Please report problems with `setup-log.txt`.

**v0.2.3**

- **Controller vibration** on/off in the F1 PC settings.
- The game-data installer launcher is now `Install-Files.cmd` (English name).
- The shared game-data installer (`tools/port-installer`) ships in the repository.

Vibration has been compiled but not felt on a real controller yet. Nobody has played this exact build through.

**v0.2.2**

- **Untested code in this release:** the native effect parameter commit (`sub_824F4770`) is compiled and on by default, but it has never been run. If a mission breaks or effects look wrong, set `BF2_NO_NATIVE_COMMIT=1`.
- The shadow map no longer uses MSAA, and whole surfaces use the hardware resolve. At 4K, 16x anisotropic filtering, no frame limit, in the light stretch of the first mission (Ryzen 5 3600, RTX 3050 Laptop), without MSAA / 2x / 4x / 8x: 79/75/68/56 fps before, 79/76/71/63 fps now. Measured before the untested code above went in. The heavy stretch stays at about 50 fps.
- Float constants travel as changed blocks; native constant store, parameter lookup, particle vertex writer and vec3 normalize.
- 7e3 render target limits applied to pixel shader output.
- PC settings menu reads every SDL pad; controller mappings sit next to the executable.
- The game keeps running when the audio device stops consuming.
- Experimental macOS build files (not compiled or run yet) and a native Linux build path (not built for this release).

Nobody has played this exact build through.

**Earlier**

Renderer update — 2026-10-23:

- Bound vertex fetch results per 32-bit word to the descriptor's buffer range, returning zero outside it. Mini fetches inherit the full fetch's bounds. This fixes the tested first-mission translucent foliage wall without disabling its shader or lowering graphics settings; the user confirmed the visual result.
- Captured-input replay keeps 7,008 valid positions bit-identical while the 3,500 excess vertices become degenerate. Add 84 game-free WARP bounds cases, including partial elements, mini fetches and legacy negative controls, to the existing shader regression suite. Release build, IEEE arithmetic and 48 position permutations pass.
- Short gameplay profile: five samples, 35–52 FPS, median 47, with VSync and the limiter off. Camera/scene changes and the short interval prevent a controlled performance claim. Full-game image quality and long-term stability remain unverified.

Renderer update — 2026-10-22:

- Make equivalent position-export dot products accumulate in a stable order and mark position output precise. Captured terrain replay is bit-identical across depth/color variants; the tested first-mission snow pattern is fixed. A separate colored wall above the ground remains unresolved. Add a game-free WARP regression covering 48 translated permutations and a failing legacy cancellation case.
- Start the deferred index ring with a DISCARD map and retry with DISCARD after an upload failure; D3D11 debug validation confirms the startup map error is gone.

- Cache identical shader constants and pipeline states within a command list, with invalidation after utility passes and command-list completion.
- Keep native triangle/line strips instead of expanding every primitive on the CPU; check full texture memory using persistent workers; accelerate linear texture decoding with exact SSE2 endian copies.
- Add optional CPU phase profiling and repair per-draw captures so undefined GPU row padding no longer hides real pixel changes. Capture cubemap faces for reflection diagnostics.
- Preserve translated NaN/Inf checks with IEEE-strict shader compilation and include the flags in the disk-cache key. Game-free Windows WARP arithmetic and shader-reflection regressions pass; this is not a verified streak fix.
- Fix scaled pixel-coordinate rounding and remove C++ float/integer aliasing from the system-constant upload path.
- Expose experimental monitor-rate and uncapped modes in F1, alongside VSync off. High-rate simulation behavior still requires validation.
- Validation: local Windows release build succeeded; 131,584 endian/alignment/width comparisons plus state-cache and worker synchronization checks passed. Gameplay profiling reached the first mission. Scene changes prevent treating the observed FPS difference as a controlled speedup. Colored streaks and sustained 60 fps remain unresolved; visual verification of the last shading changes is incomplete. A later profiling session terminated with an invalid guest-function dispatch; the cause remains under investigation, and stability is not verified.

- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Modern controller layout after Battlefield 6's defaults (aim on LT, B crouch/prone, X reload/use) and keyboard/mouse bindings (Shift sprint, Ctrl crouch, Z prone).
- Direct mouse look on the camera (no stick curve or acceleration); aim follows the right mouse button via the weapon's zoom state.
- In-game PC options (F1) and an F12 renderer capture for bug reports.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the supported revision by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates your XEX locally and builds the port.
- Tested on Windows 11 x64 through the first single-player mission (~40 fps). Known issues: texture shimmer while the camera moves, particle streaks, audio not yet verified by ear.
