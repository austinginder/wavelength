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

0. **A factory preset by name.** `wavelength presets <plugin> --search organ` lists what a
   CLAP plugin ships (Altitude alone has 450: basses, leads, pads, plucks, drums, organs,
   sequences); use it as `"preset": "OR Cathedral Organ"`. Prefixes in names tell the role
   (`BA` bass, `LD` lead, `PD` pad, `PL` pluck, `DR` drum one-shot on C4, `OR` organ, `SQ` sequence).
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
and articulations are keyswitches from MIDI 0 (0 = the first articulation, usually Long;
1 = Short; strings: 2 = Pizzicato, 3 = Tremolo). Let Wavelength send them: give the track
`"articulations": {"long": 0, "spiccato": 1, "pizzicato": 2, "tremolo": 3}` and each note an
`"art"`, add `"range": ["G3", "C#7"]` to be warned about unplayable notes, and
`"velocityTo": {"param": "Dynamics"}` so long notes follow your velocities (they ignore
velocity otherwise). Performance controls are parameters: `Expression`, `Dynamics`,
`Vibrato`, `Release`, `Tightness`, `Reverb` and the `Mic: ...` mixes, all automatable.

Plugins with no program parameter and no preset files (factory presets compiled into the
binary) can still be driven by state. JUCE plugins' state is `VC2!` + u32 little-endian
length + an XML document + NUL; write that XML yourself with plain parameter values and load
it as `{"file": ..., "format": "raw"}`. `scripts/extract-embedded-presets.py` reads such factory
presets out of the binary (Relica 2, TAL-NoiseMaker); override values per part with `params`.
Run `wavelength state save <plugin> --out x.vstpreset` first to see the plugin's real layout.

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
- **Real samples:** `builtin:sampler` plays Bitwig's sound content and any WAV folders:
  `{"multisample": "Grand Piano"}`, organs, guitars, basses, and drum machine kits such as
  `{"kit": "Legend 707"}` / `"Legend 808"` / `"Legend 909"`. Find them with
  `wavelength samples --search <text>`; check a kit's key map with `--kit <name>`.
- **Space:** create buses with a `reverb` (and a `delay`, e.g. `"time": 0.75` beats) at
  `"mix": 1`, and send tracks to them (`"sends": {"Hall": -8}`). Shared reverb glues parts.
- **Clean low end:** `eq` high-pass everything that isn't bass (pads ~140 Hz, leads ~150,
  arps ~250) and low-pass the sub.
- **Pump:** `duck` bass, pads and arps from the kick (`"trigger": "Drums", "keys": [36]`).
  Roughly: bass 10 dB, pads 6, arps 4–5, leads ~1.
- **Movement:** automate filters (`"automate": {"cutoff": ...}`) through builds and intros.
  LFOs work on any automatable value (`"lfo": {"cutoff": {"rate": "1/8", "depth": 1}}`), and
  `tremolo`, `gate` (trance gate / gated reverb), `rotary` (Leslie organ), `autowah`, `vibrato`,
  `bitcrush` and `tapestop` are built in. Use `"step"` points for hard switches, not two
  close points (a stray ramp can quietly lower a whole section).
- **Groups:** `"output": "Drums"` sends tracks to a group bus (shared glue, one filter sweep
  for the whole band); buses can feed other buses; `master.automation.gain` fades the song.
- **Feel and expression:** `groove` (swing, lay-back, humanize) instead of hand-shifted notes;
  `roll` for strummed chords; `automation.pitchbend` / `cc` for plugins, per-note `bend` and
  `mono` + `glide` for samples (808 slides, guitar bends); `transpose` for presets that sound
  an octave off; a tempo point with `"ramp": true` for ritardando.
- **Master:** a gentle glue `compressor` (ratio ~1.6–2) then a `limiter` at −1.5 dB.
- **Balance, then dynamics:** first set each track's `gain` from its stem loudness
  (`gain = target − tracks[].lufs`; e.g. leads −18, brass −17, drums −15, pads −23,
  arps −23 LUFS), then add `markers` and ride faders with `automation.gain` until the
  per-section loudness in the report follows the music (quiet sections really quiet).

## Choosing sounds: `wavelength audition`

Preset names don't say how a preset sounds or in which octave. Run `wavelength audition <plugin>`
once per plugin (Altitude's 450 presets take about two minutes); after that `wavelength presets
<plugin>` shows tags measured from a C4 note, and `--search` finds them: `--search "octave -1"`,
`--search dark`, `--search pluck`, `--search sub`, `--search rhythmic`, `--search wide`. Presets
tagged `octave -1` sound an octave below the written note: add `"transpose": 12` or write the
part higher. `self-playing` presets make sound without notes (latched arps, drones); `rhythmic`
ones turn a held note into a pattern. `presets --json` has the numbers (centroid, bands,
envelope, pitch offset in semitones and cents).

## Listening by numbers: `wavelength analyze`

You can't hear the render, so measure it. `wavelength analyze out/<dir> --json` gives, for the
mix, each stem and each section: pitch (note, cents, confidence), spectral centroid (brightness),
band balance (sub, bass, low-mid, high-mid, presence, air), stereo width and correlation, onsets
and the envelope (attack, decay, sustain). Use it to check a preset sounds in the octave you
wrote, a bass isn't brighter than intended, a section's low end is balanced, a pad is wide, and a
drum pattern has the hits you expect. `--start/--end` narrow it to a window.

## Reading the report

`out/<dir>/report.json` (also printed with `--json`):

- `tracks[].sectionLufs`, each track's loudness per marker section after its fader and rides:
  find which part dominates a section without writing measuring scripts.
- `failedTracks`, tracks whose plugin crashed or hung in its worker process: the song still
  rendered without them (each also has a warning). Swap the plugin or preset and render again.
- `tracks[].renderSeconds`, where the render time goes (plugin load and warmup included).
- `tracks[].latencyCompensatedMs`, processing delay the track's plugins reported; it is already
  removed, so the track stays aligned. A plugin that doesn't report its delay isn't corrected.
- `mix.truePeakDb`, the reconstructed peak (what an MP3 encoder sees).
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
changes don't alter stems. They are 32-bit float by default and big (about 8 MB per track-minute
at 48 kHz); use `"stems": "none"` (or `--stems none`) once you only need the report and the mix.
A failed render leaves `report.json` as `{"ok": false, ...}`, never the previous render's.

## Known limits (v0.1)

- CLAP and VST3 (no Audio Units yet). Built-in effects cover the essentials; installed CLAP
  and VST3 effects work in any `fx` chain.
- All tracks render in one process. Some plugin families (the nakst synths) share
  Objective-C class names and print a warning when several load together; if a render
  crashes, split tracks into separate jobs.
- A plugin whose sound depends on its own GUI or licence dialog may render its demo/default.
  An effect that only changes the level gets a warning; a state or preset that changes no
  parameter gets a warning too.
- Presets by name come from CLAP preset discovery, VST3 program lists, and preset files in the
  plugin's preset folders: Serum 2 `.SerumPreset`, Odin2 `.odin`, u-he `.h2p` (Zebra2,
  Zebralette, TripleCheese), Surge XT and OB-Xf `.fxp`, and any `.vstpreset` under
  `/Library/Audio/Presets/<Vendor>/<Plugin>`, every DX7 cartridge voice for Dexed, and NKS
  presets (DUNE 3, BBC Symphony Orchestra, SynthMaster), Synplant, Cherry Audio, Guitar Rig racks
  and Microtonic kits and drums (`"AC BD Back#3"` puts a drum on channel 3). `wavelength presets
  <plugin>` lists them all. Parameters also take the plugin's display text: `"Cutoff": "800 Hz"`.
  Presets compiled into plugin binaries (TAL-NoiseMaker, Relica 2) appear after running
  `scripts/extract-embedded-presets.py` once. Analog Lab V, AAS Player (guitars, electric
  pianos, mallets), MPowerSynth, Soundbox and DecentSampler also load by name.
- Not loadable headlessly: Kontakt (never pass it an unknown state file: it can hang the render),
  Komplete Kontrol, ZENOLOGY (needs a Roland Cloud login), Spitfire LABS (encrypted patches; the
  default patch plays), UVI Workstation.
