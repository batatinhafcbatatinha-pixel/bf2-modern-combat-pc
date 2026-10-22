Source kit only: no game, no XEX, no generated code and no executable. Bring your own Xbox 360 USA disc.

Renderer update — 2026-10-22:

- Cache identical shader constants and pipeline states within a command list, with invalidation after utility passes and command-list completion.
- Keep native triangle/line strips instead of expanding every primitive on the CPU; check full texture memory using persistent workers; accelerate linear texture decoding with exact SSE2 endian copies.
- Add optional CPU phase profiling and repair per-draw captures so undefined GPU row padding no longer hides real pixel changes. Capture cubemap faces for reflection diagnostics.
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
