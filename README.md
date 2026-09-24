# Wavelength

A headless music engine for AI agents. Wavelength plays notes through real, installed
CLAP synthesizers (Vital, Surge's OB-Xf, the nakst synths, …) offline, with no DAW and
no screen, mixes them with built-in effects, buses and automation, and returns WAV
stems, a mixdown and a machine-readable report with loudness per track and section.

It is built for agents: jobs are JSON, every command has `--json` output with actionable
errors, and plugin output never pollutes stdout. See **[AGENTS.md](AGENTS.md)** for the
operating guide, **[docs/job-format.md](docs/job-format.md)** for the job schema and
**[docs/effects.md](docs/effects.md)** for effects, buses, automation and the built-in instruments.

https://wavelength.run

## Build

Requires CMake 3.20+ and a C++17 compiler (Xcode command line tools on macOS). The CLAP
headers and nlohmann/json are fetched at configure time.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/wavelength help
```

## Try it

```sh
./build/wavelength plugins                      # installed CLAP plugins
./build/wavelength params nakst.Apricot         # parameters and current values
./build/wavelength render examples/hello.json --out out/hello
```

`examples/hello.json` is four bars of C minor through Apricot, ExtraBold, OB-Xf and Vital.

## Commands

| Command | Does |
|---|---|
| `plugins [--rescan] [--json]` | Lists CLAP plugins in `~/Library/Audio/Plug-Ins/CLAP`, `/Library/Audio/Plug-Ins/CLAP` and `$WAVELENGTH_CLAP_PATH`. Results are cached per bundle. |
| `params <plugin> [--state F] [--all] [--json]` | Parameters with ranges, current values and display text, optionally after loading a preset. |
| `render <job.json> [--out DIR] [--json] [--verbose]` | Renders stems, mix and `report.json`. |
| `state save <plugin> --out F.clap-preset [--state F] [--set "Name=v"]…` | Builds a preset from a starting state plus parameter changes. The file uses Bitwig's `.clap-preset` layout, so it can be dropped into a `.dawproject`. |

## How it works

`src/bundle.*` loads `.clap` bundles; `src/instance.*` is the host side of one plugin
instance (thread-check, log, params, state, audio/note ports) and follows CLAP's threading
rules: lifecycle and state on the main thread, `process()` on a dedicated audio thread
while the main thread keeps pumping the run loop and `on_main_thread` requests.
`src/engine.*` runs a plugin over the timeline (sample-accurate notes, tempo-mapped
transport, parameter automation, audio input for effects). `src/effects.*` holds the
built-in effects and the CLAP effect wrapper, `src/builtins.*` the drum and FX instruments,
`src/loudness.*` a BS.1770 meter, and `src/render.*` the mix graph (tracks → sends → buses →
master). `src/state_file.*` reads and writes plugin state containers.

## Roadmap

1. Per-track process isolation (crash containment, plugin families that clash in one process).
2. `render project.dawproject`: render DAWproject files directly, including their plugin states.
3. A local service (Go) with a web UI and live playback through the speakers.
4. VST3 and Audio Unit hosting.
5. True-peak (oversampled) limiting and metering.
