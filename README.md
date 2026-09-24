# Wavelength

A headless music engine for AI agents. Wavelength plays notes through real, installed
CLAP and VST3 instruments (Vital, Serum 2, Surge XT, OB-Xf, Dexed, BBC Symphony Orchestra, …)
and sample libraries, offline, with no DAW and no screen. It mixes them with built-in
effects, buses and automation, and returns WAV stems, a mixdown and a machine-readable
report with loudness per track and section.

It is built for agents: jobs are JSON, every command has `--json` output with actionable
errors, and plugin output never pollutes stdout. See **[AGENTS.md](AGENTS.md)** for the
operating guide and mixing playbook, **[docs/job-format.md](docs/job-format.md)** for the job
schema and **[docs/effects.md](docs/effects.md)** for effects, buses, automation and the
built-in instruments.

https://wavelength.run

## What it can do

- **Host plugins:** CLAP and VST3 instruments and effects, found by name, id or bundle path.
  VST3 bundles are scanned in child processes so a crashing plugin can't take the scan down.
- **Load sounds by name:** about 17,000 presets across 33 installed plugins. Sources include:
  - CLAP preset discovery and VST3 program lists
  - preset files in each plugin's preset folders
  - NKS presets
  - DX7 cartridges
  - bank files
  - presets compiled into plugin binaries
- **Read plugin preset formats:**
  - Serum 2 `.SerumPreset`, Odin2 `.odin`, u-he `.h2p`, `.fxp`/`.fxb` (Surge XT, OB-Xf)
  - NKS `.nksf`, DX7 `.syx`
  - Synplant, Cherry Audio (DCO-106, MG-1, SEM, Voltage Modular), Guitar Rig racks
  - Microtonic kits and drums, Analog Lab V, AAS Player, MPowerSynth, Soundbox, DecentSampler
  - Vital `.vital`, `.vstpreset`, Bitwig `.clap-preset`
- **Set parameters** by value or by the plugin's own display text (`"Cutoff": "800 Hz"`), and
  automate them with breakpoints, steps and LFOs.
- **Play samples without a plugin:** `builtin:sampler` plays Bitwig `.multisample`
  instruments (pianos, organs, guitars, basses, orchestral), drum-machine kit folders mapped
  to General MIDI (Legend 707/808/909, …), loops and single WAVs. It supports glide, per-note
  pitch bend, slices and reverse.
- **Play expressively:** swing and humanize (`groove`), strummed chords, tempo ramps, MIDI CC,
  pitch bend and pressure.
- **Mix:** per-track effect chains, sends, group buses, bus and master automation (fades),
  and 19 built-in effects:
  - dynamics: compressor, true-peak limiter, sidechain duck, gate
  - EQ, filter and saturation
  - space: reverb, delay, chorus, width
  - modulation: tremolo, pan, Leslie rotary, auto-wah, vibrato
  - bitcrush and tape stop
- **Measure:** BS.1770 loudness per track, bus, marker section and mix (it matches
  `ffmpeg -af ebur128`), per-track loudness per section, true peak and per-track render time.

## Build

Requires CMake 3.20+ and a C++17 compiler (Xcode command line tools on macOS). CMake fetches
these at configure time:
- the CLAP headers
- the VST3 SDK (hosting sources only)
- nlohmann/json
- zstd

zlib comes from the system.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/wavelength help
scripts/check.sh              # build + render the examples; fails on errors, silence or clipping
```

## Try it

```sh
./build/wavelength plugins                          # installed CLAP/VST3 plugins + built-ins
./build/wavelength presets "Serum 2" --search bass  # presets you can use by name
./build/wavelength samples --search piano           # sample libraries for builtin:sampler
./build/wavelength params "vst3:Dexed" --preset "E.-PIANO"
./build/wavelength render examples/hello.json --out out/hello
```

The examples:
- `examples/hello.json`: four bars of C minor through Apricot, ExtraBold, OB-Xf and Vital.
- `examples/effects-tour.json`: the built-in effects.
- `examples/sampler-tour.json`: Bitwig's Grand Piano, a Farfisa organ and the Legend 707 kit.

A minimal job:

```json
{
  "tempo": 120,
  "tracks": [
    {"name": "Keys", "plugin": "vst3:Dexed", "preset": "E.-PIANO",
     "notes": [{"beat": 0, "dur": 4, "key": "C4", "vel": 0.8}]},
    {"name": "Drums", "plugin": "builtin:sampler", "sampler": {"kit": "Legend 707"},
     "notes": [{"beat": 0, "dur": 0.25, "key": 36}, {"beat": 1, "dur": 0.25, "key": 38}]}
  ],
  "master": {"fx": [{"type": "limiter", "ceiling": -1.5}]}
}
```

## Commands

| Command | Does |
|---|---|
| `plugins [--rescan] [--json]` | Lists CLAP and VST3 plugins and the built-in instruments. It searches the standard plug-in folders, `$WAVELENGTH_CLAP_PATH` and `$WAVELENGTH_VST3_PATH`, and caches results per bundle. |
| `presets <plugin> [--search T] [--rescan] [--json]` | Lists a plugin's presets to use by name as `"preset"`, with category and notes (e.g. Guitar Rig racks marked free edition or Pro). |
| `samples [--search T] [--kit NAME] [--json]` | Lists sample libraries for `builtin:sampler` (Bitwig content and `$WAVELENGTH_SAMPLES_PATH`); `--kit` prints a kit's key map. |
| `analyze <file.wav \| render-dir> [--start S] [--end S] [--json]` | Measures pitch, brightness, band balance, stereo width, onsets and envelope of a WAV, or of a render's mix, stems and sections. |
| `params <plugin> [--preset N] [--state F] [--all] [--json]` | Shows parameters with ranges, current values and display text, optionally after loading a preset or state. |
| `render <job.json> [--out DIR] [--stems float\|24\|16\|none] [--json]` | Renders stems, `mix.wav` and `report.json`. A failed render leaves `{"ok": false}` in `report.json`, never a stale report. |
| `state save <plugin> --out FILE [--state F] [--set "Name=v"]…` | Builds a preset from a starting state plus parameter changes (`.clap-preset` for CLAP, `.vstpreset` for VST3). |

`scripts/extract-embedded-presets.py` extracts factory presets compiled into JUCE plugin
binaries, for example TAL-NoiseMaker and Relica 2, so `presets` can list them.
`scripts/stage-gains.py <song>` sets a song's faders from its target loudness.

## How it works

**Plugin hosting**
- `src/bundle.*` and `src/instance.*` host CLAP plugins, following CLAP's threading rules:
  lifecycle and state on the main thread, `process()` on a dedicated audio thread.
- `src/vst3_plugin.*` hosts VST3 plugins with the SDK's hosting helpers, including
  IMidiMapping for MIDI controllers.
- `src/engine.*` opens a plugin (preset, state, parameters, automation) and renders it over
  the timeline: sample-accurate notes and controllers, a tempo-mapped transport, and audio
  input for effects.

**Presets and state**
- `src/state_file.*` reads state containers.
- `src/preset_formats.*` and `src/microtonic.*` convert plugins' own preset files into the
  state bytes each plugin loads.
- `src/preset_files.*` finds preset files and NKS presets per plugin; `src/presets.*` runs
  CLAP preset discovery.

**Sound and mixing**
- `src/sampler.*` is the built-in sampler; `src/builtins.*` holds the drum and FX instruments.
- `src/effects.*` holds the built-in effects and the plugin effect wrapper.
- `src/loudness.*` is the BS.1770 meter.
- `src/render.*` is the mix graph: tracks → group buses and sends → buses → master.

## Roadmap

1. Per-track process isolation and parallel rendering. Loading and warming up plugins one
   after another is most of the render time.
2. A preset audition index: render one note of every preset and store its pitch offset,
   loudness, brightness and envelope, so agents can choose sounds by description.
3. `render project.dawproject`: render DAWproject files directly, including their plugin states.
4. A local service with a web UI and live playback through the speakers.
5. Audio Unit hosting.
