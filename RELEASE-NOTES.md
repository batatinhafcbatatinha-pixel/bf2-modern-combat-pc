Source kit only: no game, no XEX, no generated code and no executable. Bring your own Xbox 360 USA disc.

- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Modern controller layout (aim on LT) and keyboard/mouse bindings.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the supported revision by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates your XEX locally and builds the port.
- Tested on Windows 11 x64 through the first single-player mission (~40 fps). Known issues: particle streaks, audio not yet verified by ear.
