Source kit only: no game, no XEX, no generated code and no executable. Bring your own Xbox 360 USA disc.

- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Modern controller layout after Battlefield 6's defaults (aim on LT, B crouch/prone, X reload/use) and keyboard/mouse bindings (Shift sprint, Ctrl crouch, Z prone).
- Direct mouse look on the camera (no stick curve or acceleration); aim follows the right mouse button via the weapon's zoom state.
- In-game PC options (F1) and an F12 renderer capture for bug reports.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the supported revision by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates your XEX locally and builds the port.
- Tested on Windows 11 x64 through the first single-player mission (~40 fps). Known issues: texture shimmer while the camera moves, particle streaks, audio not yet verified by ear.
