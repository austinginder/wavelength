# Changelog

All notable changes to Wavelength. Versions follow semantic versioning.

## [Unreleased]

Working towards **v0.1.0**, the first release.

### Added
- Headless CLAP host: loads installed `.clap` bundles, follows CLAP threading (main thread for lifecycle/state, a dedicated audio thread for `process()`), pumps the macOS run loop and `on_main_thread` requests while rendering.
- `wavelength plugins`: scans the CLAP folders with a per-bundle cache; lists built-in instruments too.
- `wavelength params`: parameter names, ranges, values and display text, optionally after loading a preset.
- `wavelength render`: JSON jobs with tempo maps, notes in beats or seconds, per-track presets (`.clap-preset`, Vital `.vital`, raw) and parameter overrides; writes stems, `mix.wav` and `report.json`.
- `wavelength state save`: builds Bitwig-compatible `.clap-preset` files from a starting state plus parameter changes.
- Mixing: per-track effect chains, post-fader sends, buses, a master chain, fader and parameter automation, section markers.
- Built-in effects: gain, eq, filter, delay, reverb (8-line FDN), compressor, limiter, saturate, chorus, width, duck (sidechain keyed from another track's notes). CLAP effect plugins work in any chain.
- Built-in instruments: `builtin:drums` (General MIDI kit) and `builtin:fx` (impact, riser, reverse swell, sub drop).
- Loudness: BS.1770 / EBU R128 integrated LUFS per track, bus, section and mix (matches `ffmpeg -af ebur128`).
- Agent ergonomics: `--json` on every command with actionable errors; plugin stdout is diverted to stderr so reports stay parseable.
- Docs: `AGENTS.md` (operating guide and mixing playbook), `docs/job-format.md`, `docs/effects.md`.
- `scripts/check.sh` regression check; examples `hello.json` and `effects-tour.json`.
