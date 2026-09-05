# Cuelume-derived cues

These seven MP3 files are deterministic offline approximations of the Cuelume v0.2.2 browser recipes. They are not bit-identical renders of the Web Audio implementation: the preview renderer uses a seeded noise source, a simple moving-average filter, and a bounded delay tail.

Upstream: https://github.com/Danilaa1/cuelume
Upstream commit checked: `b879b72c01f3b3fa74c45c9b20bbd064baffb282`
License: MIT; see `LICENSE.cuelume`.

Mapping: `select` from `navigation_move`/`tick`, `tap` from `button_activate`/`press`, `complete` from `success`, `interrupt` from `error`, `online` from `ready`, `speaking` from `loading`, and `startup` from `arrival`. The short `tick` recipe keeps repeated rocker navigation below 120 ms; `press` remains a distinct muted button cue.

The source renderer is `scripts/generate_cuelume_sounds.py`. It writes matching 16 kHz mono WAV previews to `emulator/web/audio/` and 32 kbps mono 16 kHz MP3 files here. The existing audio runtime and cue ownership are unchanged.
