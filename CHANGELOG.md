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
- Automation: `"step"` points and a `curve` setting; LFOs (tempo-synced note values or Hz, six shapes, depth curves) on any built-in effect setting, plugin parameter, fader, pan or send; track `pan` automation; automated sends; bus and master gain automation (fades).
- Routing: `"output"` on tracks and buses for group buses and bus chains, processed in dependency order.
- Feel: job/track `groove` (swing, lay-back, humanize with a seed), `roll` (strummed chords), track `transpose`, tempo ramps (`"ramp": true` tempo points).
- MIDI controllers: `automation.cc`, `automation.pitchbend` (semitones via `bendRange`) and `automation.pressure`; CLAP plugins get MIDI or note expressions, VST3 plugins the parameters they map through IMidiMapping.
- New built-in effects: `tremolo` (and autopan), `pan`, `gate` (step pattern or note-keyed), `rotary` (Leslie with rotor inertia), `autowah`, `bitcrush`, `vibrato`, `tapestop`. `duck` depth is automatable.
- Limiter true-peak detection (4x oversampled, on by default); the report gives `mix.truePeakDb`, per-track `renderSeconds` and per-track `sectionLufs`.
- Sampler: `mono` + `glide` (legato slides), per-note `bend`, kit map entries with `gain`/`pan`/`tune`, `retrigger`, `variants` (takes as round robins), `bpm` (tempo-synced loops), `slices`, `start`, `reverse`; toms map by number, loops no longer land on drum keys, `samples` lists loop folders, ambiguous kit names are an error that lists the choices.
- State: `fxp` format (VST2 `.fxp`/`.fxb` chunks: Surge XT and OB-Xf factory patches load directly); VST3 factory program lists in `presets` and `"preset"` (Dexed); a warning when a state or preset changes no parameters, and when an effect plugin only changes the level.
- Stems: `"stems": "float" | "24" | "16" | "none"` (and `render --stems`); a free-space check before rendering.
- Preset formats: Serum 2 `.SerumPreset` (zstd + CBOR, split into processor and controller state), JUCE ValueTree binaries such as Odin2 `.odin`, and u-he `.h2p` (Zebra2, Zebralette, TripleCheese) load as `state` and are auto-detected.
- Presets by name from preset folders: `wavelength presets` and `"preset"` also search the plugin's preset folders (`/Library/Audio/Presets/<Vendor>/<Plugin>`, Serum 2, Odin2, u-he, Surge XT, OB-Xf), about 5,800 presets across these plugins; placeholder VST3 program lists ("Prog 1") are hidden.
- NKS preset index: `.nksf` files in the usual NKS folders are indexed (cached in `~/Library/Caches/wavelength/nks.json`, rebuilt with `presets --rescan` or when a name isn't found) and matched to their plugin, so `"preset"` reaches DUNE 3's 1,071 presets and BBC Symphony Orchestra's instruments by name; NKS chunks of wrapped VST2 plugins get the length prefix their VST3 state needs.
- DX7 cartridges: every voice of Dexed's `.syx` cartridges is a preset by name (1,056 voices installed), or `"state": "<cart>.syx#<voice>"`; the voice is written into Dexed's own state (edit buffer and cartridge).
- Synplant: `.synplant` patches (355 unique, 12 categories) load by name into program slot 0 of Synplant's own state, with their tuning, release and volume.
- Preset listings drop copies of the same preset filed in several folders (All / By Category / By Creator).
- `getState` on plugins and state formats that patch the plugin's current state (templates), used by `dx7`.
- `wavelength params` hides JUCE's MIDI CC placeholder parameters (thousands per plugin) unless `--all`.
- `scripts/stage-gains.py <song>`: sets each track's fader from the last render's stem LUFS and the song's `targets.json`.

### Changed
- Automation points at the same beat keep the order they are written in (`[16, 0], [16, 1]` jumps from 0 to 1); they used to be sorted by value.
- The limiter detects true (inter-sample) peaks by default; mixes limited at the same ceiling come out a few tenths of a dB quieter.
- Kit auto-mapping groups takes of one sound and maps toms by number, so some keys of auto-mapped kits play different files than before (explicit `map` entries are unaffected).

### Fixed
- A failed render no longer leaves the previous render's `mix.wav`, `report.json` and stems behind; it writes `{"ok": false}` to `report.json`.
- `params` set on top of a `state` stick on plugins that apply state late (Surge XT); `params` inspection processes a few blocks first so such plugins show real values.
- Clamp warnings no longer fire for in-range values (float precision) and show the range.
- Limiter warning reads as a positive gain-reduction figure with the time it happened.
- The CLI exits without running plugin static destructors, which crashed some plugins at shutdown after the work was already written.
