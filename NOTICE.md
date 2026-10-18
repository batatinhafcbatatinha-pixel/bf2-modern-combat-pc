# Redistribution and provenance

This is a source and tooling distribution. It contains no original game assets, no game executable (XEX), no generated translation units, no disassembly, no SDK binaries and no compiled game executable. The manifest lists function addresses and sizes that the translator needs; it describes where functions are, not what they contain.

The independently written native integration (renderer, shader translation, audio, hooks, PC settings) and build tooling are offered under the root MIT license. This license grants no rights to Electronic Arts game content.

External dependencies are downloaded by `tools/build_local.py` and are not stored here:

- ReXGlue SDK nightly `0.10.0.24-dev.gbd833a2` (BSD-3-Clause, derived from Xenia). `patches/rexglue-fpscr-mask.patch` documents the one-line change applied locally to its `rex/ppc/context.h`. The SDK's GPU emulation plugin is removed from the build output; this port does not use it.
- FFmpeg public headers from the commit matching the SDK's prebuilt libraries (LGPL-2.1). The libraries themselves come with the SDK.

Some native code follows the semantics of Xenia's BSD-licensed headers shipped with the SDK (Xenos shader microcode, texture swizzles and XMA context layouts); the implementation is our own. License texts are in `licenses/`.

A future binary distribution would have to satisfy every applicable license independently; this source release does not authorize distribution of proprietary game translations.

The publication audit (`tools/audit_publication.py`, run on every push) uses an allowlist and rejects executables, images, audio, archives, game resources, generated code and oversized files. It is an engineering safeguard, not a legal guarantee for every jurisdiction or use. Do not submit game content or private build artifacts in pull requests or issues.
