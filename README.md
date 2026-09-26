# Wavelength

A headless music engine for AI agents. Wavelength plays notes through real, installed
CLAP, VST3 and VST2 instruments (Vital, Serum 2, Surge XT, OB-Xf, Dexed, BBC Symphony Orchestra, Reaktor, …)
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

- **Host plugins:** CLAP, VST3 and VST2 instruments and effects, found by name, id or bundle path.
  VST3 and VST2 bundles are scanned in child processes so a crashing plugin can't take the scan down.
  On Apple silicon, Intel-only plugins (Reaktor 6, Synth1, older VST2s) run in render workers under
  Rosetta: macOS builds are universal for this.
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
  instruments (pianos, organs, guitars, basses, orchestral), SFZ instruments, drum-machine kit folders mapped
  to General MIDI (Legend 707/808/909, …), loops and single samples (WAV, AIFF, FLAC, MP3, Ogg Vorbis). It supports glide, per-note
  pitch bend, slices and reverse.
- **Play expressively:** swing and humanize (`groove`), strummed chords, tempo ramps, MIDI CC,
  pitch bend and pressure.
- **Write for orchestral libraries:** per-note articulations that send their own keyswitches,
  playable-range warnings, and velocity driving a controller such as BBC SO's Dynamics.
- **Mix:** per-track effect chains, sends, group buses, bus and master automation (fades),
  and 20 built-in effects:
  - dynamics: compressor, multiband (a chain per band), true-peak limiter, sidechain duck, gate
  - EQ, filter and saturation
  - space: reverb, delay, chorus, width
  - modulation: tremolo, pan, Leslie rotary, auto-wah, vibrato
  - bitcrush and tape stop
- **Master:** a master chain with a loudness target (`"loudness": -14` finds the gain into the
  limiter), `wavelength master` to master a finished mix without re-rendering, and `leadIn`
  silence before the song for streaming uploads.
- **Measure:** BS.1770 loudness per track, bus, marker section and mix (it matches
  `ffmpeg -af ebur128`), per-track loudness per section, true peak and per-track render time.

## Install

Download the archive for your platform from the
[latest release](https://github.com/austinginder/wavelength/releases/latest). Each has the
`wavelength` binary plus the docs and examples (the names carry no version, so the links below always
fetch the newest release):

| Platform | Archive |
|---|---|
| macOS 12+ (Apple silicon and Intel) | `wavelength-macos-universal.tar.gz` |
| Linux x86_64 / arm64 (glibc 2.35+: Ubuntu 22.04, Debian 12, Fedora 36 or newer) | `wavelength-linux-x86_64.tar.gz`, `wavelength-linux-arm64.tar.gz` |
| Windows 10+ x64 | `wavelength-windows-x86_64.zip` |

```sh
curl -L https://github.com/austinginder/wavelength/releases/latest/download/wavelength-macos-universal.tar.gz | tar xz
./wavelength-macos-universal/wavelength plugins
```

The macOS binary isn't notarized. `curl` downloads run as-is; if you download it in a browser,
clear the quarantine flag first: `xattr -d com.apple.quarantine wavelength`. `SHA256SUMS.txt` on
each release has the checksums.

Wavelength finds plugins in each platform's standard folders, plus `$WAVELENGTH_CLAP_PATH` and
`$WAVELENGTH_VST3_PATH` and `$WAVELENGTH_VST2_PATH` (lists separated by `:`, or `;` on Windows):

| | CLAP | VST3 | VST2 |
|---|---|---|---|
| macOS | `~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP` | `~/Library/Audio/Plug-Ins/VST3`, `/Library/Audio/Plug-Ins/VST3` | `~/Library/Audio/Plug-Ins/VST`, `/Library/Audio/Plug-Ins/VST` |
| Linux | `~/.clap`, `/usr/local/lib/clap`, `/usr/lib/clap` | `~/.vst3`, `/usr/local/lib/vst3`, `/usr/lib/vst3` | `~/.vst`, `/usr/local/lib/vst`, `/usr/lib/vst` |
| Windows | `%LOCALAPPDATA%\Programs\Common\CLAP`, `%COMMONPROGRAMFILES%\CLAP` | `%LOCALAPPDATA%\Programs\Common\VST3`, `%COMMONPROGRAMFILES%\VST3` | `%PROGRAMFILES%\VSTPlugins`, `%PROGRAMFILES%\Steinberg\VSTPlugins`, `%COMMONPROGRAMFILES%\VST2` |

### Folders

| | Settings (`defaults.json`, `blocked.json`, extracted presets) | Caches (plugin scan, NKS index, auditions) |
|---|---|---|
| macOS | `~/Library/Application Support/Wavelength` | `~/Library/Caches/wavelength` |
| Linux | `$XDG_CONFIG_HOME/wavelength` (`~/.config/wavelength`) | `$XDG_CACHE_HOME/wavelength` (`~/.cache/wavelength`) |
| Windows | `%APPDATA%\Wavelength` | `%LOCALAPPDATA%\wavelength\cache` |

## Build

Requires CMake 3.20+ and a C++17 compiler: the Xcode command line tools on macOS, g++ or clang
on Linux, llvm-mingw for Windows. CMake fetches these at configure time:
- the CLAP headers
- the VST3 SDK (hosting sources only)
- nlohmann/json
- zstd
- Signalsmith Stretch

zlib comes from the system, or is fetched where there is none (Windows).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/wavelength help
scripts/check.sh              # build + render the examples; fails on errors, silence or clipping
```

`scripts/build-release.sh <tag>` builds the release archives for all five targets from a git tag
on one Mac: macOS natively, Linux in Docker, Windows cross-compiled with llvm-mingw and
smoke-tested under Wine. `--upload` attaches them to the GitHub release. The whole release checklist is in
[docs/releasing.md](docs/releasing.md).

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
- `examples/clips-tour.json`: audio clips on the timeline (`builtin:audio`).
- `examples/mastering.json`: EQ, a three-band compressor, a true-peak limiter and a -14 LUFS target.

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
| `plugins [--rescan] [--json]` | Lists CLAP, VST3 and VST2 plugins and the built-in instruments (Intel-only ones marked for Rosetta). It searches the standard plug-in folders (see Install), `$WAVELENGTH_CLAP_PATH`, `$WAVELENGTH_VST3_PATH` and `$WAVELENGTH_VST2_PATH`, and caches results per bundle. |
| `presets <plugin> [--search T] [--rescan] [--json]` | Lists a plugin's presets to use by name as `"preset"`, with category and notes (e.g. Guitar Rig racks marked free edition or Pro). |
| `samples [--search T] [--kit NAME] [--json]` | Lists sample libraries for `builtin:sampler` (Bitwig content and `$WAVELENGTH_SAMPLES_PATH`); `--kit` prints a kit's key map. |
| `audition <plugin> [--jobs N] [--limit N] [--rebuild]` | Renders every preset once in worker processes and indexes how it sounds (octave offset, brightness, band balance, envelope, width), so `presets` can show and search sound tags. |
| `analyze <file.wav \| render-dir> [--start S] [--end S] [--peaks] [--json]` | Measures pitch, brightness, band balance, stereo width, onsets and envelope of a WAV, or of a render's mix, stems (bus stems too) and sections. `--peaks` lists the strongest spectral peaks as Hz, note and level, and the spacing they share (a comb's tuning). |
| `params <plugin> [--preset N] [--state F] [--all] [--set "Name=v"]… [--map "Name"] [--json]` | Shows parameters with ranges, current values and display text, optionally after loading a preset or state. `--set "Rate=0.5"` prints the display text of a plain value (or the value display text or a note name reads as), `--map "Rate"` a value -> display table across the range. |
| `render <job.json> [--out DIR] [--stems float\|24\|16\|none] [--deliver mp3,flac] [--jobs N] [--json]` | Renders stems, `mix.wav` and `report.json`, plus MP3/FLAC/16-bit WAV deliveries measured after decoding (`--deliver` or the job's `deliver`; MP3 through LAME or ffmpeg). Plugin tracks render in worker processes, several at once; a crashing plugin costs its track, not the song. A failed render leaves `{"ok": false}` in `report.json`, never a stale report. |
| `import <song.dawproject> [--out DIR] [--bitwig FILE \| none] [--json]` | Turns a DAWproject export (Bitwig, Studio One, Cubase) into a job: notes, tracks with their plugins and saved states, mixer, sends, groups, tempo map, markers, volume and pan automation. Audio clips (warped ones follow the song tempo) and automation of the instrument plugin's parameters come across too. For Bitwig it also reads the `.bwproject` behind the export for Bitwig's own devices: Drum Machine pads (plugins and samples), Chain, EQ+, EQ-5, Filter, Reverb, Delay-2, Distortion, Compressor, Multiband FX-3, Peak Limiter, Tool. Lists what didn't come across. `import song.bwproject` lists a Bitwig project's devices; `render song.dawproject` imports and renders in one step. |
| `import <song.mid> [--out DIR] [--instrument PLUGIN] [--json]` | Turns a Standard MIDI File into a job: tempo map, time signature, markers, and a track per MIDI track and channel with its notes (sustain pedal folded into note lengths), volume, pan and expression, other controllers, pitch bend (with the file's bend range) and pressure. Channel 10 plays `builtin:drums`; other channels a General MIDI-family sound from the installed sample library, or `--instrument` for all of them. `render song.mid` imports and renders. |
| `import <score.musicxml \| score.mxl> [--out DIR] [--instrument PLUGIN] [--json]` | Turns a MusicXML score (MuseScore, Sibelius, Finale, Dorico, music21) into a job: a track per part with repeats, first/second endings, D.C./D.S. played out, ties, chords and voices, transposing instruments at concert pitch, dynamics and hairpins as velocities, staccato and accents, tempo marks, key signatures as `keys` (for `lint --harmony`) and rehearsal marks as markers. Each note keeps its score marks in `marks`. Parts get General MIDI sounds as for a MIDI import; `render score.mxl` imports and renders. |
| `export <job.json> [--out song.mid] [--json]` | Writes the job's parts as a type 1 MIDI file to open in a DAW or notation program: tempo map (ramps stepped every 1/16 beat), time signature, markers, and a track per job track with its notes, fader, pan, rides and CC, pitch bend and pressure automation. Export, import and export again gives the same file byte for byte. |
| `timeline <job.json> [--every BARS] [--json]` | Song time of every marker and every BARS bars from the tempo map (ramps included), in song seconds and file time (after the lead-in), with the tempo there and the song's length, before rendering. |
| `master <mix.wav> --chain <chain \| job> [--loudness L] [--lead-in S] [--out DIR] [--deliver mp3,flac] [--json]` | Masters a finished mix: plays it through a master chain (an effect list, a master object, or a song's job with its markers; a file or inline JSON) and reports loudness and true peak before and after, per section. |
| `state save <plugin> --out FILE [--state F] [--set "Name=v"]…` | Builds a preset from a starting state plus parameter changes (`.clap-preset` for CLAP, `.vstpreset` for VST3). |

`skills/wavelength/` is a skill for Claude Code and other agents (`/wavelength`): it installs
the engine (release build, else from source) and walks the agent through writing, mixing and
mastering a song. Copy the folder into `~/.claude/skills/`.

`scripts/extract-embedded-presets.py` extracts factory presets compiled into JUCE plugin
binaries, for example TAL-NoiseMaker and Relica 2, so `presets` can list them.
`scripts/stage-gains.py <song> [render dir]` sets a song's faders from its target loudness and a
render's stem loudness (`<song>/out` by default).

## How it works

**Plugin hosting**
- `src/platform.*` holds everything that differs between macOS, Linux and Windows: folders,
  worker processes, loading plugin libraries, the main-thread event loop.
- `src/bundle.*` and `src/instance.*` host CLAP plugins, following CLAP's threading rules:
  lifecycle and state on the main thread, `process()` on a dedicated audio thread.
- `src/vst2_plugin.*` hosts VST2 plugins on Wavelength's own declaration of the VST 2 binary
  interface (`src/vst2_abi.hpp`; Steinberg's VST 2 SDK is not used): MIDI events, transport,
  `.fxp`/`.fxb` chunks and parameter lists, programs.
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

1. Per-note pitch and level tracking in `analyze`; gain-reduction readouts for compressors and limiters.
2. Auditions inside each instrument's range, and an envelope-depth measure for rhythmic patches.
3. DAWproject import: automation of plugin effects and of Bitwig's own devices, launcher clips, more
   Bitwig devices (Mid-Side Split, Polymer, Phase-4).
4. A General MIDI SoundFont fallback; SFZ filters and release triggers.
5. A local service with a web UI and live playback through the speakers; Audio Unit hosting.

## License

MIT, see [LICENSE](LICENSE). Third-party libraries compiled into the binary are listed with their
licences in [THIRD_PARTY.md](THIRD_PARTY.md).
