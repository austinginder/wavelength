# Changelog

All notable changes to Wavelength. Versions follow semantic versioning.

## [Unreleased]

### Added
- `skills/wavelength/`: an agent skill that installs the engine (release build, else from source, docs matched to the binary) and runs the whole song workflow.

## [0.1.0] - 2026-09-24

The first release. Wavelength renders JSON music jobs through installed CLAP and VST3 plugins
and sample libraries, headlessly, and reports what an agent can't hear.

Highlights:
- CLAP and VST3 hosting with plugin delay compensation; plugin tracks render in parallel worker
  processes, so a crashing plugin costs its track, not the song.
- About 17,000 presets by name across 33 plugins, from CLAP preset discovery, VST3 program lists,
  NKS, DX7 cartridges and plugins' own preset files (Serum 2, Odin2, u-he, Synplant, Cherry Audio,
  Guitar Rig, Microtonic, Analog Lab, AAS, ...); parameters by value or by display text.
- `wavelength audition` indexes how every preset sounds (octave offset, brightness, envelope, width)
  and `wavelength analyze` measures renders (pitch, spectrum, stereo, onsets, envelope).
- `builtin:sampler` (Bitwig multisamples, drum kits, loops), `builtin:audio` clips with
  pitch-preserving stretch, and built-in drums and FX.
- A mix engine: effect chains, sends, group buses, sidechain audio, LFOs and step automation,
  tempo ramps, groove, MIDI CC and pitch bend, 20+ built-in effects, true-peak limiting, mastering
  to a loudness target, per-track and per-section loudness in the report.

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
- Cherry Audio: DCO-106, Surrealistic MG-1 Plus, Synthesizer Expander Module and Voltage Modular presets (737) load by name, merged into each plugin's own state (JUCE ValueTree, byte-exact); Voltage Modular gets a 5 s default warmup (it loads patches asynchronously), BBC Symphony Orchestra 6 s.
- SynthMaster One and Player: their NKS presets (about 3,400) load by name; each file is assigned by its payload (the Player's folder holds One presets too).
- Guitar Rig 6: its 1,272 rack presets (`.ngrr`) load by name in an effect chain, built without a template; the listing gives each rack's category and whether the free edition can play it (52 can), and a rack using paid components gets a warning naming them.
- Parameter values as display text: `"params": {"Cutoff": "800 Hz", "Rate": "1/16"}` is parsed by the plugin itself (VST3 getParamValueByString, CLAP text_to_value), in jobs and effect chains.
- Microtonic: 115 kits (`.mtpreset`) and 430 drums (`.mtdrum`) load by name; a kit waits for notes 36-43 (its pattern player and mutes off; set `"PlayStop": "PlayNow"` to let its patterns play), a drum goes onto a channel with `"preset": "AC BD Back#3"`. Values are read by Microtonic's own text parser, so they match the presets exactly.
- `scripts/extract-embedded-presets.py`: extracts factory presets compiled into JUCE plugin binaries (no files on disk) into `~/Library/Application Support/Wavelength/Presets/<Plugin>/`, which `presets` and `"preset"` search: TAL-NoiseMaker (256), Relica 2 (35), Thump One (164), Wavetable (99), Flux Mini 2 (22). Listings show a preset once when a file and a program share its name.
- Analog Lab V (254 Analog Lab presets, 15 s default warmup), AAS Player (673 programs: Strum guitars, Lounge Lizard, Chromaphone, Ultra Analog, String Studio), MPowerSynth (1,578 presets from its bank), Soundbox (45) and DecentSampler (`.dspreset` files, samples resolved next to the preset; 3 s warmup) load by name or as `state`.
- Preset listings drop copies of the same preset filed in several folders (All / By Category / By Creator).
- `getState` on plugins and state formats that patch the plugin's current state (templates), used by `dx7`.
- `wavelength params` hides JUCE's MIDI CC placeholder parameters (thousands per plugin) unless `--all`.
- Sidechain: `"sidechain": "<track>"` on a plugin effect feeds that track's audio (after its effects, before its fader) into the plugin's sidechain input (CLAP second input port, VST3 aux bus), and on the built-in `compressor` keys its detector. Source tracks render first; worker processes get their audio as files; sidechain loops are rejected. Many plugins also need their own sidechain switch turned on (MTurboComp "Side-chain input (Detector)").
- `builtin:audio`: audio clips on the timeline. Each clip is a WAV placed by `beat` or by `endAt` (it ends exactly on that beat: reverse swells), fitted to the song tempo from its own `bpm` with pitch-preserving stretch (Signalsmith Stretch, MIT) or tape-style (`"stretch": false`), transposed with `pitch`, trimmed (`start`, `length` or `beats`), reversed, with gain and fades. Example `examples/clips-tour.json`.
- Plugin tracks render in worker processes, several at once (`render --jobs N` or `"parallel"` in the job; default half the cores, up to 4; 0 = everything in one process). A track whose plugin crashes or hangs is left out of the mix and listed in the report's `failedTracks` instead of failing the song; job mistakes (unknown preset, bad state) still fail the render. Built-in instruments stay in-process; tracks are mixed as they finish, so memory stays flat.
- `audition --retag` recomputes every index's tags from stored measurements; a new audition run retries earlier failures and drops presets that are no longer listed. Serum 1 `.fxp` patches in Serum 2's folders are no longer listed for Serum 2.
- `wavelength audition <plugin> [--jobs N] [--limit N] [--rebuild]`: renders every preset once (C4 for 1 s) in worker processes, each keeping one plugin instance, and indexes how it sounds in `~/Library/Caches/wavelength/audition/`: octave offset and cents, loudness, brightness, band balance, attack, decay, sustain, release, width, onsets, and whether it plays by itself. A preset that crashes or hangs its worker is recorded and skipped. `presets` then shows tags (octave -1, dark, warm, bright, sub, bassy, pluck, slow attack, rhythmic, long release, wide, self-playing, silent, ...) that `--search` finds, and `--json` includes the measurements.
- `wavelength analyze <file.wav | render-dir> [--start] [--end]`: pitch (YIN: note, cents, confidence), spectral centroid, rolloff and band balance (sub to air), stereo width and correlation, onsets, and envelope (attack, decay, sustain, active time). A render folder analyzes its mix, every stem and every marker section. Agents no longer need their own FFT scripts.
- `scripts/stage-gains.py <song>`: sets each track's fader from the last render's stem LUFS and the song's `targets.json`.
- Orchestral writing: per-note articulations (`"art": "spiccato"` with a track `articulations` map of keyswitch keys) insert the keyswitch just before each change; track `range` warns about notes an instrument can't play; `velocityTo` turns note velocities into a controller curve (`{"param": "Dynamics"}` or `{"cc": 1}`) for libraries whose long notes ignore velocity.
- `multiband` effect: 2-4 bands on Linkwitz-Riley crossovers that sum back flat, each band with its own effect chain, gain, solo and mute (multiband compression, band saturation).
- `master.loudness`: a target in LUFS; the gain into the master chain is found in a few passes so the mastered mix lands on it (`mix.loudnessGainDb` in the report).
- `wavelength master <mix.wav> --chain <chain | job>`: masters a finished mix without re-rendering, with loudness before and after per section; the chain can be a file or inline JSON.
- Personal job defaults: `~/Library/Application Support/Wavelength/defaults.json` (or `$WAVELENGTH_DEFAULTS`) holds top-level job settings, e.g. `{"leadIn": 1}`, that every job gets unless it sets them itself; the report lists them in `defaultsApplied`. Jobs Wavelength builds internally (`wavelength master`) ignore them.
- `leadIn`: seconds of silence before the song in the mix and stems (streaming uploads), and `--lead-in` on `wavelength master`.

### Changed
- Automation points at the same beat keep the order they are written in (`[16, 0], [16, 1]` jumps from 0 to 1); they used to be sorted by value.
- The limiter detects true (inter-sample) peaks by default; mixes limited at the same ceiling come out a few tenths of a dB quieter.
- Kit auto-mapping groups takes of one sound and maps toms by number, so some keys of auto-mapped kits play different files than before (explicit `map` entries are unaffected).

### Fixed
- An option given without its value (`analyze out --end --json`) is an error instead of swallowing the next option.
- Plugin delay compensation: instruments and effects that report latency (lookahead limiters, linear-phase EQs, LA-2A, ...) are rendered that many samples longer and shifted back into place, so their tracks no longer land late against the mix. The report gives `tracks[].latencyCompensatedMs`.
- A failed render no longer leaves the previous render's `mix.wav`, `report.json` and stems behind; it writes `{"ok": false}` to `report.json`.
- `params` set on top of a `state` stick on plugins that apply state late (Surge XT); `params` inspection processes a few blocks first so such plugins show real values.
- Clamp warnings no longer fire for in-range values (float precision) and show the range.
- Limiter warning reads as a positive gain-reduction figure with the time it happened.
- The CLI exits without running plugin static destructors, which crashed some plugins at shutdown after the work was already written.
