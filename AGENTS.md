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
wavelength lint job.json --harmony           # 4. wrong notes, before any render (see Harmony)
wavelength render job.json --out out/x --json  # 5. render, read the report
# 6. adjust gains/params/notes from the report and render again
```

Always pass `--json`. Stdout then carries exactly one JSON document; plugin chatter goes
to stderr. On failure the document is `{"ok": false, "error": "..."}` and the exit status
is non-zero. Errors are written to be actionable (they name the track, parameter or file).

Working on one part? `render job.json --tracks "Lead,Bass" --stems none` renders just those
tracks (plus, muted, whatever keys their `duck`/`gate`/sidechain) through the song's buses and
master: much faster than the whole song, and the report shows those tracks alone. With a
`master.loudness` target the master would push those tracks up to the target on their own; add
`--level-from out/report.json` (the full render's report) to keep the full mix's master gain, so
each part plays and measures at the level it has in the song.

## Starting from a DAW project

`wavelength import song.dawproject --out songs/x --json` writes `songs/x/job.json` and the plugin
states under `songs/x/plugins/`. Read the `notes` in the result: they name what didn't come across.
A DAW's own devices are only names in a DAWproject. For Bitwig exports the importer also reads the
project itself (`<name>.bwproject`, found next to the export or in `~/Documents/Bitwig Studio/Projects/
<name>/`, or given with `--bitwig`): Drum Machine pads become one track each (plugins with their
states, Sampler pads as `builtin:sampler`) into a bus named after the drum track, and Chain, EQ+,
EQ-5, Filter, Reverb, Delay-2, Distortion, Compressor, Multiband FX-3, Peak Limiter and Tool become
built-in effects, with the plugins inside them. Other Bitwig devices (Polymer, Mid-Side Split, ...)
are listed as left out. Without the Bitwig project a Drum Machine becomes `builtin:drums`. Audio
clips become `builtin:audio` tracks (warped clips follow the song tempo); automation of volume, pan
and the instrument plugin's parameters comes across. Automation of plugin effects and of Bitwig's own
devices, and launcher clips, don't. `wavelength import song.bwproject` lists a Bitwig project's tracks,
devices, pads and plugin states. Then edit the job like any other and render it; `render
song.dawproject` does both steps.

## Starting from a MIDI file

`wavelength import part.mid --out songs/x --json` makes a job from a Standard MIDI File (from
MuseScore, a DAW, music21, another agent). Channel 10 becomes `builtin:drums`; every other part gets
a General MIDI-family sound from the sample library (piano, strings, brass, ...) so the job renders
at once: then swap in real plugins and presets track by track (`"plugin"`, `"preset"`), keeping the
notes. `--instrument "Surge XT"` puts one plugin on every melodic part instead (controllers and
pitch bend then reach it as automation). `wavelength export job.json --out song.mid` goes the other
way: hand a part to a person to open in Bitwig, Cubase or a notation program.

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

## VST3, VST2 and sample libraries

VST3 and VST2 plugins work everywhere CLAP plugins do. `wavelength plugins` lists them all
(`format` column); a name that exists in several formats resolves to CLAP, then VST3, then VST2,
unless you write `vst3:Name` or `vst2:Name`. VST2 state comes from `.fxp`/`.fxb` files (a DAW's
saved chunk or a parameter list) as `"state"`, and its factory programs load by name as
`"preset"`. Plugins marked `(x86_64, Rosetta)` in `plugins` are Intel-only: on Apple silicon they
render in a worker process under Rosetta, so use them on tracks (not with `--jobs 0` or as a bus or
master effect). Sample-based
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
velocity otherwise). For a swell or hairpin on a held note give it `"dyn": [0.2, 1.0]` (or
`[[beats, level], ...]`): the Dynamics curve follows it, so the tone opens up as it grows, unlike
a volume ride. Performance controls are parameters: `Expression`, `Dynamics`,
`Vibrato`, `Release`, `Tightness`, `Reverb` and the `Mic: ...` mixes, all automatable.

The percussion patches hold several instruments behind keyswitches with identical articulation
names ("Short Hits Discover"); the instrument is in each articulation's `a_name` inside the
`.nksf` (`strings file.nksf | grep a_name`, in keyswitch order). Discover: **Tuned Percussion**
0 Tubular bells (61-79), 1 Marimba (36-96), 2 Xylophone (65-108), 3 Glockenspiel (77-108);
**Percussion** 0 Timpani (48-74, pitched), 1 Untuned percussion (white keys 48-74 only, black keys
silent; by measurement 48/50/62 are low drums, 53-55/60/71-73 long cymbal-like, 57/64/65/69 very
bright short hits); **Harp and Celeste** 0 Harp, 1 Celeste. Bells are inharmonic: `analyze` pitch
often reads them wrong (a tubular-bell A4 read as F3); trust the key, not the reading.

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

## Arranging: builds, drops and endings

A mix can meet every loudness target and still feel flat, because the arrangement decides where the
energy goes. A 16-track album made by agents produced two defects on almost every song; these
defaults fix them (break them on purpose, not by accident):

- **Empty the build, don't turn it down.** In the last bars before a drop take out the kick and the
  full bass (or go half-time), sweep a high-pass up across the drums and music, and let a snare roll,
  a riser and a filtered lead carry the tension. The master limiter gives back any level you remove
  with faders, so faders alone leave the build as loud as the drop. Aim for the drop's first bars
  3-5 dB over the build's last two.
- **A breath, not a dropout.** Before a drop, at most half a beat of gap, with reverb and delay tails
  ringing (never mute the returns). Fill it: a snare roll cut on the last 16th, a reverse swell that
  ends exactly on the downbeat, a pickup note.
- **Stack the downbeat and add something.** Kick + crash + impact + sub drop on beat 1, every filter
  opening there, and a part the build didn't have (rolling bass, full-width lead, an octave layer).
- **Drive into the ending.** Keep the groove and bass running into the final hit, make the last bar a
  small build, and let the home chord ring 2-4 s under the hit. Stopping the drums two bars early
  sounds like the song died before its last note. (A fade or a hard stop is fine when meant.)
- **Keep supporting parts audible.** A pad 10-15 dB under the lead disappears; background parts
  usually sit 4-8 dB under it and are ducked lightly (3-5 dB), not buried.

The report checks the first two: a **dropout** warning (the song goes 15 dB quiet for 0.75 s or more,
then comes back, with bars and file time), and a **weak drop** warning when a section's first 4 bars
land under +2 dB over the last 2 bars before it (`sections[].transition`, the boundary as heard;
when those bars are an intended near-silence, such as a one-bar power cut, more than 15 dB under
the 4 bars before them or under -40 LUFS, the drop is measured against the last 2 bars of music
before the silence instead, and `transition.skippedSilence` gives the silent bars and their level)
and it either follows a build-like section (Build, Rise, Pre..., Ramp,
Climb, Lead-in, Ignition) or is named like a payoff (Drop, Chorus, Peak, Hook, Final, Climax, Finale)
after a non-payoff; an escalation (Climax, Peak, Finale after another payoff) must rise at least
0.5 dB. `sections[].change` gives every section's step. When a section is meant that way (a trailer's
false-ending silence, a gentle ambient peak), give its marker `"checks": false`: its dropouts are
listed as `"intended": true` and it gets no arrangement warnings. `wavelength analyze out --every 1`
prints the whole contour second by second, labelled with the sections.

## Mixing (this is where the music comes alive)

Raw instrument renders sound flat. Do the work a mix engineer does in a DAW; everything is
in the job (details in `docs/effects.md`):

- **Transients decide how loud a song can get.** A 909/808 kick, a clap, a sampled piano or a
  clavinet can peak 17-22 dB above its own loudness; the master limiter then works 8-15 dB and
  flattens the sections. Tame them per track (`clip` or a `limiter` on the track) or with a
  `clip` before the master limiter, and read `tracks[].postFaderPeakDb`. Sub-heavy sounds (808
  kicks, sub bass) read low in LUFS, so a LUFS target alone overshoots their peaks.
- **Drums and FX:** `builtin:drums` (General MIDI kit) and `builtin:fx` (impact, riser,
  reverse swell, sub drop, Shepard rise and fall) are always available. Big moments need them.
  `builtin:shepard` is an endless riser with its own rate curve (a build that never arrives).
- **Audio on the timeline:** `builtin:audio` places WAV clips in beats: breaks fitted to the song
  tempo with their pitch kept (`"bpm": 133`), vocal chops transposed with `pitch`, and reverse
  swells that end exactly on a downbeat (`"endAt": 64, "reverse": true`). A clip's `file` can be
  the song itself: `{"render": [64, 65], "tail": 3, "fx": [reverb]}` with `"reverse": true, "endAt": 64`
  swells into the drop from the drop's own first beat, in the same render.
- **Real samples:** `builtin:sampler` plays Bitwig's sound content and any WAV folders:
  `{"multisample": "Grand Piano"}`, organs, guitars, basses, and drum machine kits such as
  `{"kit": "Legend 707"}` / `"Legend 808"` / `"Legend 909"`. Find them with
  `wavelength samples --search <text>`; check a kit's key map with `--kit <name>`.
- **Space:** create buses with a `reverb` (and a `delay`, e.g. `"time": 0.75` beats) at
  `"mix": 1`, and send tracks to them (`"sends": {"Hall": -8}`). Shared reverb glues parts.
  Automate the reverb's `freeze` (0/1) to hold a chord's tail under a break. `"loopFx"` on a
  `delay` puts effects inside its feedback loop (frequency shifter spirals, dub filters).
- **Clean low end:** `eq` high-pass everything that isn't bass (pads ~140 Hz, leads ~150,
  arps ~250) and low-pass the sub.
- **Pump:** `duck` bass, pads and arps from the kick (`"trigger": "Drums", "keys": [36]`).
  Roughly: bass 10 dB, pads 6, arps 4–5, leads ~1. For pumping that follows the kick's actual
  sound, use a `compressor` (or a plugin compressor) with `"sidechain": "Kick"`.
- **Movement:** automate filters (`"automate": {"cutoff": ...}`) through builds and intros.
  LFOs work on any automatable value (`"lfo": {"cutoff": {"rate": "1/8", "depth": 1}}`), and
  `tremolo`, `gate` (trance gate / gated reverb), `rotary` (Leslie organ), `autowah`, `vibrato`,
  `bitcrush`, `tapestop` and `repeat` (beat-repeat stutters on a bus or the master) are built
  in. Use `"switch"` (or `"step"`) points for hard switches, not two close points (a stray ramp
  can quietly lower a whole section): `[64, 1, "switch"]` holds the old value to beat 64 and
  ramps in 5 ms, or `{"curve": "switch", "ramp": 12, "points": [...]}`.
- **Plugin effect `mix`:** in `automate`, lowercase `"mix"` is the host dry/wet and `"Mix"` the
  plugin's own parameter (case-sensitive only there). `"intended": true` on an effect keeps meant
  distortion (clip, crushed compression) out of the warnings.
- **Level-neutral effects:** `"match": true` on any effect (distortion with automated drive,
  resonators, comb or flanger freezes, plugins) keeps its output at the input's loudness over
  time, so it changes the tone and not the balance. No trim curves by hand.
- **Automate in the plugin's units.** Curve values can be display text or note names, for plugin
  parameters too: `"automate": {"Frequency (Filter 1)": [[0, "C#4"], [32, "A3", "switch"]]}` or
  `[[0, "0.45 Hz"], [64, "0.96 Hz"]]`; no hand-computed normalized values (Melda frequencies are log
  0..1). `wavelength params <plugin> --set "Name=0.55"` shows what a plain value means.
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
tonality (share of energy in spectral peaks: tones and chords high, noise low), band balance (sub, bass, low-mid, high-mid, presence, air), stereo width and correlation, onsets
and the envelope (attack, decay, sustain). Use it to check a preset sounds in the octave you
wrote, a bass isn't brighter than intended, a section's low end is balanced, a pad is wide, and a
drum pattern has the hits you expect. `--start/--end` narrow it to a window. Limits: pitch is one
median fundamental, so it is only meaningful for a single line (a chord or several lines gives
a wrong low note); attack/decay describe the whole window, so for a melodic part analyze a
window around one note. Audition tags come from a single C4: instruments that can't play C4
(basses, piccolo, tuned percussion) read `silent` there.

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

## Counterpoint: `wavelength lint`

For fugues, chorales and any part-writing, `wavelength lint job.json --tracks "Soprano,Alto,Tenor,Bass"
--low "Bass" --crossings` lists parallel fifths and octaves (similar motion, both voices moving)
and voice crossings, each with both chords' notes and where they sound ("A#4/A#2 (bar 10 beat
1.50) -> D5/D3"). Each track is one voice: its top note, or its lowest for `--low`;
`--split "Organ=4"` reads a chord track as 4 voices, top to bottom, for chorales on one track.
Each pair is compared at its own note starts, so a third voice's rhythm doesn't hide or invent a
parallel. Limit the report with `--from 5 --to 17` (bars) or `--section Exposition`. Without `--tracks` every melodic track is checked and pairs that double each other most of
the time are listed as doublings instead of faults.

## Harmony: `wavelength lint --harmony`

Run it on every song before the first render: `wavelength lint job.json --harmony --chords`. It
works on the notes (exact, instant, no audio) and prints the key of every stretch of bars, a chord
chart (`F>E` = the chord changes halfway through the bar, `!` = outside the key) and the problems a
listener hears as wrong notes:

- **key excursion**: one or two bars of a chord outside the key that go straight back ("Eb (Eb G Bb)
  with Eb is outside D minor for 1 bar"). A human hears it as a one-bar key change, never as colour.
  Build with chords of the key (`bVI - bVII - V` climbs in minor: Bb C A in D minor); put chromatic
  colour in single notes (the leading tone, a passing note, a walk-down), which the check allows.
  A major chord that resolves down a fifth to a chord of the key is a secondary dominant and is
  listed as fine.
- **clash**: two parts a minor 2nd or 9th apart, both held a beat or more, with one note outside the
  key.
- **rub** (not a problem): the same between two notes of the key: a major 7th voiced under its root,
  a suspension, or a part playing one chord early or late. A rub on the same pair of tracks bar after
  bar is usually a part out of step with the chords (a chord cycle counted from the wrong bar).

The key comes from `"keys"` in the job (declare every planned key change there, with a mode:
`"D minor"`, `"C phrygian"`), from `--key "D minor"` for the whole song, or is detected over
8-bar windows (a new key has to hold 4 bars; bars with no third, such as an open-fifth drone, take
the key around them). Detection can put a boundary a bar or two early; declared keys are exact.
Pitched tracks that aren't harmony (sound effects, synth drums played on plugins, a sonar ping)
go in `--ignore "SFX,ChipKick"`, or mark the track `"harmony": false` in the job (lint leaves it out
for good). Left out already, and listed as `skipped` with the reason: drum kits, `builtin:drums`,
`builtin:fx`, and `builtin:sampler` tracks playing one sample with no pitch of its own (noise, or a
drum hit under 0.4 s: a snare roll played up keys 60-67 is a riser, not a Cdim chord). `--from`,
`--to` and `--section` limit the report, `--max-bars 3` widens what counts as a short excursion,
`--json` gives `keys`, `problems`, `rubs`, `info` (and `bars` with `--chords`).

## Mastering

The last stage, like a DAW's master channel. Put a chain on `master.fx` (an `eq`, a `multiband`
with a compressor per band, a limiter or a plugin limiter such as MLimiterX, a true-peak `limiter`
at -1 dB) and set `"loudness": -14` to land the song on a streaming target. To try mastering
settings without re-rendering the song, render once with a plain master (a high-pass only, the
"pre-master"), then run `wavelength master out/mix.wav --chain job.json --out mastered`: it plays
the file through the job's `master` (or a chain file) and reports loudness before and after, per
section. Don't master a mix that already went through a limiter. For a release, add `"leadIn": 1`
(a second of silence before the song, for streaming platforms), or `--lead-in 1` on `master`.
`master` reads a mix's own lead-in from the render's report.json, so `master out/mix.wav --chain
job.json` lines the sections up and keeps the lead-in.

- **MP3 and AAC overshoot.** Lossy encoding adds 0.4-1.0 dB of true peak: a `-1.3` limiter
  decoded to -0.9 dBTP on four album tracks, a `-2.0` one to -1.1 on a dense orchestral mix. For
  a lossy release use a ceiling near `-2.0` (`-2.3` to `-2.7` for dense or breakbeat mixes) and
  check the decoded file (`ffmpeg -i x.mp3 -af ebur128=peak=true -f null -`).
- **Rides are a layer.** Put section rides in `automation.rides` (named curves allowed), not into
  `automation.gain`: they add to the written fader curve instead of replacing it.
- **Rides move contrast, not the final level, when the master limits.** A master limiter or a
  `loudness` target hands most of a ride back: riding a section down 2 dB may move its final LUFS
  by 0.3 dB. `sections[].preMasterLufs` shows each section before the master (what the track and bus
  rides did); `sections[].lufs` is the result after it. Compare the two before riding harder.
- **Start every curve at beat 0.** A curve holds its first value before its first point, so a ride
  written from bar 57 holds that value over bars 1-56. The report warns about it on tracks, buses
  and the master (gain, rides, effect curves) whenever something already plays through them.
- **Master rides run before the chain.** `master.automation.gain` changes the level going into
  the master compressors and limiter, which then pull part of it back: ride tracks or buses to
  shape the section contour, and keep master rides for fades.

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

- `tracks[].sections` and `buses[].sections`, loudness per marker section (`{"name", "lufs"}`)
  after the fader and rides (a long release or reverb tail counts in the next section: a chord that
  rings over a boundary shows up there): find which part dominates a section without writing measuring
  scripts. `sectionLufs` is the same as a bare list in `sections` order.
- `dropouts` and `sections[].change`: near-silence the song comes back from (start/end in the
  file, bars, LUFS), and each section's loudness step over the previous one. `sections[].transition`
  measures the boundary itself: the last 2 bars before it, the first 4 after it and the jump (a
  whole-section average can read +3 dB while the drop itself lands +1.5). Across a near-silence
  it compares with the music before the silence (`skippedSilence`: `bars`, `lufs`, `lastBarsFrom`). See "Arranging".
- `mix.lra`, the loudness range in LU (EBU Tech 3342, equal to ffmpeg's `ebur128` LRA): how far
  quiet and loud passages sit apart. `analyze` and `master` report it too.
- `failedTracks`, tracks whose plugin crashed on every attempt (a crashed worker is started
  again, `"retries"`, default 2) or hung. The mix is written without them, but `ok` is `false`,
  `error` names them and the exit status is 1: levels, ducking and loudness are wrong, so never
  gain-stage from that report. Render again, or swap the plugin or preset. A track warning
  "rendered again (attempt 2)" means a crash was recovered and the track is complete.
- A track reading above +6 LUFS is broken output (runaway feedback, garbage samples), never a
  level: the report warns. Samples that are not numbers or above +30 dBFS are muted after every
  instrument and effect, with a warning saying where.
- `tracks[].renderSeconds`, where the render time goes (plugin load and warmup included).
- `tracks[].latencyCompensatedMs`, processing delay the track's plugins reported; it is already
  removed, so the track stays aligned. A plugin that doesn't report its delay isn't corrected
  (kHs Reverser's wet output lags by one chunk).
- `mix.truePeakDb`, the reconstructed peak (what an MP3 encoder sees).
- `tracks[].postFaderPeakDb`, the track's peak after its fader and pan, as it leaves the track
  (before any bus it feeds: a track into a limited bus can read hotter than what reaches the mix). When the master limiter
  works hard, a warning names the tracks whose peaks feed it; tame those (a `limiter` or
  `saturate` on the track) instead of turning the whole song down.
- `tracks[].lufs` and `buses[].lufs`, integrated loudness after the `fx`, before the fader.
  Use it for gain staging: `gain` = target − lufs. `mix.lufs` is the whole song
  (−14 LUFS is a common streaming level); `sections[].lufs` is per marker section.
  These match `ffmpeg -af ebur128` exactly.
- `tracks[].levels.activeRmsDb`, RMS while the track is sounding.
- `tracks[].levels.peakDb`, stems are unclipped float; peaks above 0 only matter in the mix.
- `tracks[].levels.silent: true` with notes present means the sound didn't play: wrong
  plugin (effect instead of instrument), a state that mutes it, or notes out of range.
- `warnings` lists everything: the mix's own warnings and every track's (prefixed `track 'Name':`).
  Read it after every render. `mix.levels`: prefer a master `limiter` over `"normalize"`.

Stems are written **after** the track's `fx` and **before** its fader, so fader and send
changes don't alter stems. They are 32-bit float by default and big (about 8 MB per track-minute
at 48 kHz); use `"stems": "none"` (or `--stems none`) once you only need the report and the mix.
A failed render leaves `report.json` as `{"ok": false, ...}`, never the previous render's.

## Known limits (v0.1)

- CLAP, VST3 and VST2 (no Audio Units yet; no VST2 shell plugins). DAWproject import brings notes,
  plugins, mixer and automation of volume/pan, not audio clips or a DAW's own devices. Built-in effects cover the
  essentials; installed plugin effects work in any `fx` chain (Intel-only ones on tracks only).
- All tracks render in one process. Some plugin families (the nakst synths) share
  Objective-C class names and print a warning when several load together; if a render
  crashes, split tracks into separate jobs.
- A plugin whose sound depends on its own GUI or licence dialog may render its demo/default.
  An unlicensed plugin that opens a registration window on every load (on macOS the render
  kills that worker within about 2 s and the track fails saying so) should be blocked: `wavelength plugins --block <plugin>
  --reason "..."`; `plugins` marks it BLOCKED and render, params, presets and audition refuse it.
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
