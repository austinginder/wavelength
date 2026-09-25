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

**Check a melodic preset over the range you write for it.** Some presets are voiced for the mod
wheel or a macro: with the controller at 0 their filters stay nearly closed, so the sound is
dull and loses about 1 dB per semitone as the melody climbs (a lead that fades on its high
notes). Pushing the controller up can expose built-in distortion and an OTT-style compressor
that sound harsh and lift noise between notes. Before committing a lead, render a small job
that holds each note of the part's range and compare their levels; if they fall away, try the
controller (`"Mod Wheel"`, `"Macro 1"` or `automation.cc`), switch the preset's own distortion
or compressor off, or pick another preset. A Vital `.vital` file is JSON: its `modulations`
list shows what `mod_wheel` and the macros drive. When the choice is taste, render the
candidates playing the same phrase and let the human pick.

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
- **Audio on the timeline:** `builtin:audio` places WAV clips in beats: breaks fitted to the song
  tempo with their pitch kept (`"bpm": 133`), vocal chops transposed with `pitch`, and reverse
  swells that end exactly on a downbeat (`"endAt": 64, "reverse": true`).
- **Real samples:** `builtin:sampler` plays Bitwig's sound content and any WAV folders:
  `{"multisample": "Grand Piano"}`, organs, guitars, basses, and drum machine kits such as
  `{"kit": "Legend 707"}` / `"Legend 808"` / `"Legend 909"`. Find them with
  `wavelength samples --search <text>`; check a kit's key map with `--kit <name>`.
- **Space:** create buses with a `reverb` (and a `delay`, e.g. `"time": 0.75` beats) at
  `"mix": 1`, and send tracks to them (`"sends": {"Hall": -8}`). Shared reverb glues parts.
- **Clean low end:** `eq` high-pass everything that isn't bass (pads ~140 Hz, leads ~150,
  arps ~250) and low-pass the sub.
- **Pump:** `duck` bass, pads and arps from the kick (`"trigger": "Drums", "keys": [36]`).
  Roughly: bass 10 dB, pads 6, arps 4–5, leads ~1. For pumping that follows the kick's actual
  sound, use a `compressor` (or a plugin compressor) with `"sidechain": "Kick"`.
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
- **Master:** while mixing, a gentle glue `compressor` (ratio ~1.6–2) then a `limiter` at −1.5 dB;
  for the release, a real mastering chain (see Mastering).
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

**When the human names a moment** ("at 1:29 the melody fades"):
1. Find the bar: `sections[].start` in the report is in seconds of the written file (after any
   `leadIn`); count bars from there with the tempo map.
2. Render just that stretch: a copy of the job keeping only the notes in those bars, `stems`
   `"16"`. Keep every track an effect is keyed from (a `duck` trigger, a `sidechain`), or the
   render fails.
3. Measure each stem's short-term level through the moment (0.25-0.5 s steps) and look for
   the stem that moves; then test that instrument alone (one note per pitch, one controller
   value per render) until you know why.
4. Know the limits: pitch confidence is 0 on chords (not a sign of noise), and high-frequency
   energy can't tell a bright sound from hiss or static. When the numbers can't decide, cut
   short solo clips of the suspect stems for the human and ask which one it is.

## Mastering

The last stage, like a DAW's master channel. Put a chain on `master.fx` (an `eq`, a `multiband`
with a compressor per band, a limiter or a plugin limiter such as MLimiterX, a true-peak `limiter`
at -1 dB) and set `"loudness": -14` to land the song on a streaming target. To try mastering
settings without re-rendering the song, render once with a plain master (a high-pass only, the
"pre-master"), then run `wavelength master out/mix.wav --chain job.json --out mastered`: it plays
the file through the job's `master` (or a chain file) and reports loudness before and after, per
section. Don't master a mix that already went through a limiter. For a release, add `"leadIn": 1`
(a second of silence before the song, for streaming platforms), or `--lead-in 1` on `master`.

- **Set band compressors from measurements.** Solo each band (`"bands": [{}, {"solo": true}, {}]`)
  through `wavelength master` and read its level; aim for 2-3 dB of reduction on the loud
  sections. The loudness target's gain is applied at the start of the chain, so a trim `gain`
  first keeps the compressors' input steady whatever the target.
- **Compare at equal loudness.** Louder sounds better to everyone. Before asking the human to
  choose between two versions, bring both to the same LUFS: `wavelength master old.wav --chain
  '{"fx": [], "loudness": -14}'` writes a level-matched copy.
- **Check plugin limiters** in the report: `truePeakDb` should sit at their ceiling. The built-in
  `limiter` with `truePeak` last in the chain guarantees it.
- **Master the pre-master without a lead-in.** `--chain job.json` reads the job's markers, which
  won't line up with a file that already starts with silence; add the lead-in with `--lead-in`.

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
