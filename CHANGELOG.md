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
- VST3 hosting (Steinberg VST3 SDK 3.8, MIT): instruments and effects render through the same jobs, effects chains, automation and reports as CLAP plugins. Plugins resolve by name, id or bundle path; `vst3:` / `clap:` prefixes pick a format when a plugin ships as both.
- VST3 scanning runs each unknown bundle in a child process with a timeout, so a plugin that crashes or hangs while loading can't take the scan down; results (including failures such as Intel-only bundles) are cached.
- State formats `vstpreset` (VST3 preset files, as found inside Bitwig DAWprojects) and `nksf` (Native Instruments NKS presets, which carry the plugin's own state; this is how BBC Symphony Orchestra instruments load headlessly).
- Per-track and per-effect `warmup` for plugins that stream samples after activation (orchestral libraries).
- `state save` writes `.vstpreset` files for VST3 plugins.
- CLAP preset discovery: `wavelength presets <plugin> [--search text]` lists a plugin's factory banks and preset files, and `"preset": "<name>"` on a track or effect (or `--preset` on `params`/`state save`) loads one by name through the plugin's preset-load extension. This opens the full factory libraries of plugins like Altitude (450 presets), Apricot, Regency, ExtraBold and Fluctus.
- `builtin:sampler`: plays Bitwig `.multisample` instruments (key/velocity zones, velocity crossfades, round robins, select ranges, sustain loops with crossfade, key tracking, reverse), WAV drum kit folders auto-mapped to General MIDI from file names (with overrides and hat choke), and single WAVs. `wavelength samples [--search] [--kit]` lists libraries from `$WAVELENGTH_SAMPLES_PATH` and Bitwig Studio's installed packages (Legend 707/808/909, Grand Piano, Rare Organs, electric guitar, basses, ...).
- `scripts/stage-gains.py <song>`: sets each track's fader from the last render's stem LUFS and the song's `targets.json`.

### Fixed
- The CLI exits without running plugin static destructors, which crashed some plugins at shutdown after the work was already written.
