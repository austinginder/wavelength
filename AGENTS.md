# Wavelength, operating guide for AI agents

Wavelength renders music through real, installed CLAP synthesizers without a DAW or a
screen. You describe notes and sounds in a JSON job; Wavelength plays them through the
plugins offline and hands back WAV stems, a mixdown and a JSON report with levels.

You cannot hear the result. Use the measurements in the report (and tools such as
`ffmpeg -af ebur128`) to judge balance and loudness, and ask the human to listen when
the question is taste ("is this pad lush enough?").

## The loop

```
wavelength plugins --json                    # 1. what instruments exist
wavelength params <plugin> --json            # 2. what can be shaped (optionally --state preset)
$EDITOR job.json                             # 3. notes + sounds + gains
wavelength render job.json --out out/x --json  # 4. render, read the report
# 5. adjust gains/params/notes from the report and render again
```

Always pass `--json`. Stdout then carries exactly one JSON document; plugin chatter goes
to stderr. On failure the document is `{"ok": false, "error": "..."}` and the exit status
is non-zero. Errors are written to be actionable (they name the track, parameter or file).

## Choosing sounds

In order of preference:

1. **A preset file as `state`.** Vital `.vital` files (JSON, `~/Music/Vital/**`) load
   directly. Bitwig `.clap-preset` files (inside any exported `.dawproject`, under
   `plugins/`) load for their own plugin. This gives designed, musical sounds.
2. **Parameters** on top of a state or the default patch: `"params": {"Filter Cutoff": 0.4}`.
   Values are the plugin's plain values (most are 0..1; check `min`/`max` and the
   `display` text from `wavelength params`). Names match case-insensitively; use
   `"Module/Name"` or `"#id"` when a name is ambiguous.
3. **Save what you built** so it is reusable and can be wired into a DAW project:
   `wavelength state save <plugin> --state base.vital --set "Filter 1 Cutoff=0.4" --out lead.clap-preset`.

Default patches are usually plain and quiet; don't judge an instrument by its init patch.

## VST3 and sample libraries

VST3 plugins work everywhere CLAP plugins do. `wavelength plugins` lists both (`format`
column); name clashes resolve to CLAP unless you write `vst3:Name`. Sample-based
instruments need a `warmup` of several seconds on their track so samples finish loading.

Many libraries select their instrument through state, not parameters. Look for NKS presets
(`.nksf`, e.g. `~/Spitfire/<library>/NKS/`): each one is a complete instrument and loads as
`state`. **BBC Symphony Orchestra** (Discover): one track per instrument, `warmup` 5+,
notes must be inside the instrument's playable range (out-of-range notes are silent),
and articulations are selected with keyswitch notes starting at MIDI 0 (0 = the first
articulation, usually Long; 1 = Short; strings: 2 = Pizzicato, 3 = Tremolo), played just
before the notes they apply to. Performance controls are parameters: `Expression`,
`Dynamics`, `Vibrato`, `Release`, `Tightness`, `Reverb` and the `Mic: ...` mixes, all
automatable.

## Writing jobs

See `docs/job-format.md` for every field. The essentials:

- Notes are in **beats** (`beat`, `dur`) against the job's `tempo`, which can be a
  single BPM or a tempo map (`[{"beat": 0, "bpm": 76}, {"beat": 8, "bpm": 138}]`).
  Use `time`/`length` (seconds) only when beats don't make sense.
- `key` is a MIDI number or a name with C4 = 60 (`"C4"`, `"F#3"`, `"Bb5"`).
- `vel` is 0..1 (values above 1 are treated as MIDI 0..127).
- One track = one plugin instance. Layer sounds by adding tracks.
- Relative `state` paths resolve from the job file's folder.

## Mixing (this is where the music comes alive)

Raw instrument renders sound flat. Do the work a mix engineer does in a DAW; everything is
in the job (details in `docs/effects.md`):

- **Drums and FX:** `builtin:drums` (General MIDI kit) and `builtin:fx` (impact, riser,
  reverse swell, sub drop) are always available. Big moments need them.
- **Space:** create buses with a `reverb` (and a `delay`, e.g. `"time": 0.75` beats) at
  `"mix": 1`, and send tracks to them (`"sends": {"Hall": -8}`). Shared reverb glues parts.
- **Clean low end:** `eq` high-pass everything that isn't bass (pads ~140 Hz, leads ~150,
  arps ~250) and low-pass the sub.
- **Pump:** `duck` bass, pads and arps from the kick (`"trigger": "Drums", "keys": [36]`).
  Roughly: bass 10 dB, pads 6, arps 4–5, leads ~1.
- **Movement:** automate filters (`"automate": {"cutoff": ...}`) through builds and intros.
- **Master:** a gentle glue `compressor` (ratio ~1.6–2) then a `limiter` at −1.5 dB.
- **Balance, then dynamics:** first set each track's `gain` from its stem loudness
  (`gain = target − tracks[].lufs`; e.g. leads −18, brass −17, drums −15, pads −23,
  arps −23 LUFS), then add `markers` and ride faders with `automation.gain` until the
  per-section loudness in the report follows the music (quiet sections really quiet).

## Reading the report

`out/<dir>/report.json` (also printed with `--json`):

- `tracks[].lufs`, integrated loudness of the stem (after its `fx`, before its fader).
  Use it for gain staging: `gain` = target − lufs. `mix.lufs` is the whole song
  (−14 LUFS is a common streaming level); `sections[].lufs` is per marker section.
  These match `ffmpeg -af ebur128` exactly.
- `tracks[].levels.activeRmsDb`, RMS while the track is sounding.
- `tracks[].levels.peakDb`, stems are unclipped float; peaks above 0 only matter in the mix.
- `tracks[].levels.silent: true` with notes present means the sound didn't play: wrong
  plugin (effect instead of instrument), a state that mutes it, or notes out of range.
- `mix.levels` and `warnings`, prefer a master `limiter` over `"normalize"`.

Stems are written **after** the track's `fx` and **before** its fader, so fader and send
changes don't alter stems.

## Known limits (v0.1)

- CLAP and VST3 (no Audio Units yet). Built-in effects cover the essentials; installed CLAP
  and VST3 effects work in any `fx` chain.
- All tracks render in one process. Some plugin families (the nakst synths) share
  Objective-C class names and print a warning when several load together; if a render
  crashes, split tracks into separate jobs.
- A plugin whose sound depends on its own GUI or licence dialog may render its demo/default.
