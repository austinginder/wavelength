# Job format (v1)

A job is one JSON object. Unknown fields are ignored.

```json
{
  "sampleRate": 48000,
  "blockSize": 512,
  "tempo": 120,
  "timeSignature": [4, 4],
  "tail": 3,
  "warmup": 0.4,
  "length": 0,
  "normalize": -1,
  "tracks": [
    {
      "name": "Lead",
      "plugin": "audio.vital.synth",
      "state": "presets/lead.vital",
      "params": { "Filter 1 Cutoff": 0.45 },
      "gain": -4,
      "pan": 0.2,
      "mute": false,
      "notes": [
        { "beat": 0, "dur": 1, "key": "C4", "vel": 0.8 },
        { "time": 2.5, "length": 0.5, "key": 67, "vel": 100, "channel": 0 }
      ]
    }
  ]
}
```

## Top level

| Field | Default | Meaning |
|---|---|---|
| `sampleRate` | 48000 | Output sample rate (Hz). |
| `blockSize` | 512 | Frames per `process()` call. |
| `tempo` | 120 | A BPM number, or a tempo map: `[{"beat": 0, "bpm": 76}, {"beat": 8, "bpm": 138}]`. Points step by default; `"ramp": true` on a point reaches its bpm by a smooth ramp from the previous point (accelerando / ritardando). Also sent to plugins as transport, so tempo-synced LFOs and delays follow it. |
| `timeSignature` | `[4, 4]` | Sent to plugins as transport. |
| `tail` | 3 | Seconds rendered after the last note-off, for releases and reverb. |
| `warmup` | 0.4 | Wall-clock seconds each plugin gets after activation to finish loading samples or restoring state. |
| `length` | 0 | Fixed render length in seconds (0 = last note + `tail`). |
| (defaults) | | Any top-level setting here can come from `defaults.json` in Wavelength's settings folder (`~/Library/Application Support/Wavelength` on macOS, `~/.config/wavelength` or `$XDG_CONFIG_HOME/wavelength` on Linux, `%APPDATA%\Wavelength` on Windows) or the file `$WAVELENGTH_DEFAULTS` names, e.g. `{"leadIn": 1}`; the job's own value wins, and the report's `defaultsApplied` lists what was filled in. |
| `leadIn` | 0 | Seconds of digital silence written before the audio in `mix.wav` and every stem (streaming uploads such as Spotify and SoundCloud like about 1 s). Notes, automation and loudness are unaffected; report `sections` start/end times refer to the written file. |
| `normalize` | none | If set, scale the mix so its peak is this many dBFS (e.g. `-1`). Stems are never normalized. |
| `parallel` | auto | Plugin tracks rendered at once, each in its own worker process (default: half the cores, up to 4, and fewer when the machine is already busy, e.g. with other renders). `0` renders everything in one process. `render --jobs N` overrides it. |
| `retries` | 2 | Times a worker whose plugin crashed is started again (0 to 10). A track that crashes on every attempt, or hangs, is left out of the mix and listed in `failedTracks`; the render then reports `"ok": false` and exits 1. |
| `deliver` | none | Files to write next to `mix.wav`, each decoded again and measured: `"mp3"` (320 kbps CBR, `"mp3:256"`), `"flac"` (24-bit, `"flac:16"` dithered), `"wav:16"` / `"wav:24"`, or objects `{"format": "mp3", "bitrate": 256, "file": "~/Downloads/song.mp3"}` (relative paths are inside the output folder; `~/` is your home folder). A string or a list. They carry the lead-in like `mix.wav`. MP3 uses LAME (libmp3lame, found in the usual places or `$WAVELENGTH_LAME`) or, failing that, `ffmpeg` on the PATH; MP3s carry the LAME tag, so decoders skip the encoder delay. The report's `mix.deliveries` gives each file's decoded `lufs`, `truePeakDb` and `overshootDb` (over `mix.wav`), and an MP3 above -1 dBTP warns. `render --deliver mp3,flac` overrides it; `--tracks` and `--from` renders skip it. |
| `picture` | false | `true` (or `{"width": 1800}`, 800-3200 px) writes `song.png` next to `mix.wav`: sections and bars, the mix's loudness over time with each section's level, a spectrogram of the mix, and a lane per track with its notes over its post-fader level. The report names it in `picture`. `render --png` turns it on. |
| `stems` | `"float"` | Stem files: `"float"` (32-bit, keeps overs), `"24"`, `"16"`, or `"none"` (the report still has every track's loudness). `render --stems` overrides it. A render checks free disk space first. |
| `groove` | none | Swing and humanize for every track (a track's own `groove` overrides keys): `{"swing": 0.58, "grid": 0.25, "lay": 0.02, "humanize": {"time": 0.01, "vel": 0.06, "seed": 7}}`. `swing` 0.5 = straight, 0.667 = triplet feel, applied to notes on the off-steps of `grid` (beats; 0.25 = 16ths); `lay` shifts every note (beats, + = behind the beat); `humanize` adds random timing (beats) and velocity (fraction) deviations, deterministic per `seed`. |

## Tracks

| Field | Default | Meaning |
|---|---|---|
| `name` | `trackN` | Used for the stem file name and in the report. |
| `plugin` | required | A plugin id (`nakst.Apricot`, a VST3 class id), a name (`Apricot`, `BBC Symphony Orchestra`), a path to a `.clap`, `.vst3` or `.vst` bundle, or `path#id`. A name in several formats resolves to CLAP, then VST3, then VST2; prefix `clap:`, `vst3:` or `vst2:` to pick one (e.g. `vst3:Vital`). |
| `preset` | none | A preset from the plugin's own library, by name (`"OR Cathedral Organ"`), `"Category/Name"`, or a unique part of the name. List them with `wavelength presets <plugin>`: CLAP preset discovery, a VST3 plugin's factory program list (Dexed cartridges), a VST2 plugin's program list, preset files in the plugin's preset folders (Serum 2, Odin2, u-he, Surge XT, Surge 1.x, OB-Xf, OB-Xd bank programs, `.vstpreset`), Dexed's DX7 cartridge voices, or NKS presets (DUNE 3, BBC Symphony Orchestra, ...). Applied before `state` and `params`. |
| `state` | none | A preset file path, or `{"file": "...", "format": "auto"}`. Formats: `clap-preset` (CLAP, Bitwig / DAWproject container), `vstpreset` (VST3 preset), `nksf` (NKS preset: the plugin's own state), `fxp` (VST2 `.fxp`/`.fxb` program chunks such as Surge XT and OB-Xf factory patches; `"<bank>.fxb#<n>"` is program n of an OB-Xd bank), `serum` (Serum 2 `.SerumPreset`), `serumfx` (Serum 2 `.SerumFX` / `.SerumFXRack` into Serum 2 FX), `surgefx` (a Surge `.srgfx` effect preset into Surge XT Effects), `hise` (a HISE user `.preset`: the interface's control values), `juce-valuetree` (Odin2 `.odin`), `h2p` (u-he presets), `helix` (audjoo Helix `.hxp`), `kilohearts` (kHs snap-in presets, `.ksdl` and friends), `dx7` (a DX7 cartridge voice for Dexed: `"<cart>.syx#<voice 0-31>"`), `synplant` (Synplant `.synplant` patches), `echobode` (Echobode `.echobode` patches), `permut8` (a Permut8 `.p8bank`: `"<bank>.p8bank#<program 0-29>"`, else the bank's current program), `cherry` (Cherry Audio presets), `ngrr` (Guitar Rig racks), `microtonic` (`.mtpreset` kits; `.mtdrum` drums as `"<file>.mtdrum#<channel>"`), `soundbox` (`.sbset`), `decentsampler` (`.dspreset`), `juce-string` (text presets such as Vital `.vital`), `raw`; `auto` detects them. A state or preset that changes none of the plugin's parameters gets a warning (it was ignored, or already loaded). |
| `params` | `{}` | `name → plain value` or the plugin's own display text (`"Cutoff": "800 Hz"`, `"StepRate": "1/16"`, parsed by the plugin), applied after the state. Keys: exact name, `Module/Name`, or `#id`. Out-of-range values are clamped (with a warning). |
| `gain` | 0 | dB applied when summing into the mix. |
| `transpose` | 0 | Semitones added to every note (presets that sound an octave off, key changes). |
| `output` | master | A bus name: the track feeds that bus instead of the master (group buses / sub-mixes). |
| `roll` | 0 | Beats between notes that start together, lowest first (strummed or rolled chords); negative rolls from the top. |
| `fallback` | none | Stand-in sounds for when this computer lacks the track's plugin (or its `builtin:sampler` library): a sound object or a list, tried in order, e.g. `[{"plugin": "Surge XT", "preset": "Trance Seq Bass"}, {"plugin": "builtin:synth", "preset": "BA Pluck", "gain": -2}]`. The first one available replaces the track's sound: `plugin`, `preset`, `state`, `params`, `synth`, `sampler`, `articulations`, `range`, `velocityTo`, `warmup` and the plugin's own `automation.params`/`cc`/`pressure` curves go, and every key the fallback gives is set (`gain`, `transpose`, `fx`, its own `automation`...). Notes lose their `art` when the fallback has no `articulations`. The report lists each swap first in `warnings` and in `fallbacks`; a track that can play neither its sound nor any fallback fails the render, naming both. `wavelength fallbacks --suggest` proposes built-in stand-ins at matched levels; `render --fallbacks` plays them all to hear the result. |
| `midiProgram`, `midiChannel` | none | Kept by `import song.mid` (the part's General MIDI program and channel) and written back by `export`; rendering ignores them. |
| `bendRange` | 2 | The plugin's pitch-bend range in semitones, so `automation.pitchbend` can be written in semitones. |
| `pan` | 0 | -1 (left) to 1 (right), equal-power. |
| `harmony` | true | `false` = unpitched material (a pitched snare roll, a noise sweep, synth drums on a plugin): `wavelength lint` leaves the track out of chords, clashes and voice leading. Kits, `builtin:drums`/`builtin:fx` and single-sample `builtin:sampler` tracks whose sample has no pitch (noise, a short drum hit) are left out without it. |
| `stem` | true | Write this track's stem file (when the job writes stems); `false` skips it. |
| `mute` | false | Render the stem but leave it out of the mix. |
| `warmup` | job `warmup` | Seconds this plugin gets after activation, e.g. 5 for orchestral libraries that stream samples. |
| `notes` | `[]` | See below. |
| `fx` | `[]` | Effect chain (built-in, CLAP, VST3 or VST2), see `effects.md`. Every effect also takes `bypass`, `match` (level match), `matchMs` and `intended` (no distortion warnings). |
| `sends` | `{}` | Bus name → send level in dB (post-fader), an automation curve of dB (`[[beat, dB], ...]` or a curve object), or throws: `{"base": -40, "throws": [[beat, length in beats, dB], ...], "ramp": 5}` sits at `base` and opens to each throw's level for its length (switch curve, 5 ms ramps; overlapping throws take the louder). |
| `articulations` | none | Articulation name → keyswitch key (`{"long": 0, "spiccato": 1, "tremolo": 3}`). Notes pick one with `"art"`; the keyswitch note is sent 30 ms before the first note of every change. |
| `range` | none | `[lowest, highest]` playable keys (`["G3", "C#7"]`): notes outside it get a warning in the report (sample libraries are silent there). |
| `velocityTo` | none | Drive a controller from note velocities, one point per onset, ramping between them: `{"param": "Dynamics", "min": 0.1, "max": 1}` or `{"cc": 1, "min": 10, "max": 127}`. For libraries whose long notes take loudness from a controller instead of velocity. Explicit automation of the same target wins. |
| `automation` | none | `gain` (dB), `rides` (dB added on top of `gain`: one curve, or named curves `{"sections": curve, "fills": curve}` that all add up, so section rides never overwrite the written fader curve), `pan` (-1..1), `params` (`{"Name": curve}` or `{"#id": curve}`, plain values; a curve object with `"scale": "normalized"` gives 0..1 of the parameter's range, as DAWs store automation; point values may be the plugin's display text, `[[0, "800 Hz"], [16, "2.4 kHz"]]`, or note names, `"C#4"` = its frequency, each read by the plugin once before the render; `"scale": "display"` reads plain numbers as display values too), `cc` (`{"1": curve, "64": curve}`, MIDI CC values 0-127), `pitchbend` (semitones, see `bendRange`), `pressure` (0-127). CC, pitch bend and pressure reach CLAP plugins as MIDI (or note expressions) and VST3 plugins through the parameters they map those controllers to (a warning names any they don't map). Curves are described in `effects.md` (points, steps, LFOs). |

`plugin` may also be `builtin:synth`, `builtin:drums` or `builtin:fx` (see `effects.md`), `builtin:sampler`, `builtin:audio`, or `builtin:shepard`.

### builtin:synth

A virtual-analog polysynth inside Wavelength: melodic parts render with no plugin installed (a CI runner, a fresh laptop, an agent's cloud container). Pick a patch with `preset`, change any part of it with a `synth` object (merged over the preset: objects merge key by key, `osc` and `lfo` lists replace), and set or automate its parameters by name with `params` and `automation.params`. Renders are deterministic: the same job gives the same samples.

```json
{"name": "Acid", "plugin": "builtin:synth", "preset": "BA Acid", "params": {"resonance": 0.65},
 "automation": {"params": {"cutoff": [[32, 250], [48, 1600]]}},
 "notes": [{"beat": 32, "dur": 0.2, "key": "A2", "vel": 1}]}
```

`wavelength presets builtin:synth` lists the patches (Init, `BA` bass, `LD` lead, `PD` pad, `PL` pluck, `KY` keys, `BR` brass, `FX`); each plays about -18 LUFS on a typical phrase at velocity 0.8 and sounds at the written pitch. `wavelength params builtin:synth` lists the parameters: `cutoff` (Hz, sweeps exponentially), `resonance`, `drive`, `env`, `keytrack`, `detune`, `spread`, `pw`, `fm`, `lfo`, `sub`, `noise`, `glide`, `level`. Values may be text: `"800 Hz"`, `"2.4 kHz"`, a note name.

The `synth` object:

| Setting | Default | Meaning |
|---|---|---|
| `osc` | `[{"wave": "saw"}]` | Up to 6 oscillators: `wave` (`saw`, `square`/`pulse`, `triangle`, `sine`, `noise`), `level` 1, `octave`, `semi`, `cents`, `pw` 0.5 (pulse width), `decay` (seconds of its own fade: a tine or click on top of a sustained sound), and on a sine `fm`: `{"ratio": 2, "index": 1.5, "decay": 0.4, "sustain": 0}` (a sine modulator at `ratio` times the pitch; `index` sets brightness and falls to `sustain` of itself over `decay` seconds). Saw and pulse are band-limited. |
| `unison` | 1 | Copies of every oscillator: a count, or `{"voices": 7, "detune": 30, "spread": 0.8}` (cents from lowest to highest copy, stereo width). Copies start at random (seeded) phases, so supersaws sound full. |
| `sub`, `noise` | 0, 0 | A square an octave below the note, and white noise. Above about 0.2 the sub takes over the pitch: the part sounds (and `analyze` reads it) an octave lower. |
| `filter` | lowpass 8000 | `type` (`lowpass`, `highpass`, `bandpass`, `off`), `slope` 12 or 24, `cutoff` (Hz or text), `resonance` 0-1, `keytrack` 0-1 (from C4), `env` (octaves the filter envelope adds), `velocity` (octaves darker at velocity 0), `drive` 0-1 (saturation into the filter). |
| `amp`, `filterEnv` | 3 ms / 0.3 s / 1 / 0.15 s; 0 / 0.3 s / 0 / 0.3 s | ADSR: `attack`, `decay`, `sustain` 0-1, `release` (seconds); `amp` also takes `velocity` 0-1 (how much velocity changes the level, 0.6). A sound that decays to a sustain of 0 ends there. |
| `pitchEnv` | none | `{"amount": 36, "decay": 0.06}`: semitones added at the attack, falling away (zaps, kick-like thumps). |
| `lfo` | none | One LFO or a list of up to 4: `rate` (Hz or a note value, `"1/8"`), `depth`, `shape` (as automation LFOs), `to` (`pitch` in semitones, `cutoff` in octaves, `amp` 0-1, `pw`, `pan`), `delay` and `fade` (seconds after each attack: delayed vibrato). |
| `mono`, `legato`, `glide` | false, true, 0 | `mono`: one voice. A note that starts while another is held slides to it without a new attack (`legato`), gliding over `glide` seconds; back-to-back notes attack again. |
| `level` | 0 | dB. |

Per-note `bend` and `vibrato` play on builtin:synth tracks as on the sampler, and `automation.pitchbend` (semitones) bends every voice. MIDI CC and pressure automation do nothing here: automate parameters by name.

### builtin:shepard

An endless riser (or faller): octave-spaced partials glide together under a bell curve over log-frequency, so each fades in at one end and out at the other and the sum climbs without arriving. Each note plays it for its length (the key doesn't matter; velocity sets the level). The stair's position runs from the start of the song, so back-to-back notes carry on where it is.

```json
{"name": "Stair", "plugin": "builtin:shepard", "shepard": {"rate": [[0, 0.1], [32, 0.6]], "direction": "up", "centre": "A5"},
 "notes": [{"beat": 0, "dur": 32, "key": 60, "vel": 0.8}]}
```

| Setting | Default | Meaning |
|---|---|---|
| `rate` | 0.1 | Octaves per second, a number or a curve `[[beat, rate], ...]` (an accelerating build: 0.1 -> 0.6). |
| `direction` | `"up"` | `"up"` or `"down"`. |
| `centre` | 880 | Where the bell peaks: Hz or a note name (`"A5"`). Lower is darker. |
| `width` | 1.35 | The bell's width (sigma, octaves): wider = more partials heard at once. |
| `partials` | 8 | Octaves spanned (3-12). |
| `harmonic` | 0.18 | Level of each partial's octave harmonic, for body. |
| `attack`, `release` | 0.05, 0.05 | Seconds of fade at the note's start and after its end. |

### builtin:audio

Audio files on the timeline, in beats. The track has `"clips"` instead of notes:

```json
{"name": "Break", "plugin": "builtin:audio", "clips": [
  {"file": "breaks/amen-136.wav", "beat": 0, "bpm": 136, "beats": 16},
  {"file": "fx/crash.wav", "endAt": 64, "reverse": true, "fadeIn": 200},
  {"file": "vox/hook.wav", "beat": 32, "pitch": -2, "start": 1.5, "length": 2}]}
```

| Setting | Default | Meaning |
|---|---|---|
| `file` | required | An audio file (WAV, AIFF, FLAC, MP3 or Ogg Vorbis, told apart by content): relative to the job (in a song: under `media/`), a library file by name (`"lib:Legend 909/Kick Legend 909 01 accent.wav"`, see [Library files](#library-files-lib)), an absolute path, or relative to a sample root; or `{"render": ...}`, the song's own audio (below). |
| `beat` / `endAt` | one of them | Where the clip starts, or the beat where it ends (reverse swells, pickups). |
| `bpm` | none | The file's own tempo: the clip is sped up or slowed to the song's tempo at its anchor. |
| `speed` | 1 | An explicit speed factor instead of `bpm`. |
| `stretch` | true | Keep the pitch while changing speed (Signalsmith Stretch); `false` = tape-style, pitch follows speed. |
| `pitch` | 0 | Semitones, without changing length. |
| `start`, `length` | 0, whole file | Trim, in seconds of the file; or `beats` (with `bpm`) instead of `length`. |
| `reverse` | false | Play the trimmed audio backwards. |
| `gain`, `fadeIn`, `fadeOut` | 0 dB, 2 ms, 5 ms | Level and edge fades (ms). |

**The song's own audio as a clip.** `file` can be `{"render": [fromBeat, toBeat], "tracks": [...], "tail": 3, "fx": [...]}`: those beats of the song, rendered inside the same render, become the clip's audio. A reverse swell of the drop with no pre-render:

```json
{"name": "Drop Swell", "plugin": "builtin:audio", "clips": [
  {"file": {"render": [256, 257], "tracks": ["Kick", "Bass", "Chords"], "tail": 3.5,
            "fx": [{"type": "reverb", "decay": 3.2, "size": 0.9, "mix": 0.55}]},
   "reverse": true, "endAt": 256, "fadeIn": 400}]}
```

- What is captured: each listed track (default: every track except ones that play rendered clips themselves) after its effects, fader, automation and pan, as it enters the mix; not buses, sends or the master. Muted tracks add nothing. The range is cut with 5 ms fades.
- Then `tail` seconds of silence (0-60, default 0) are added and the clip's own `fx` run over it (a reverb there rings into the tail), and the result is the "file": `start`, `length`, `reverse`, `pitch`, `endAt` and the rest apply to it as to a WAV.
- Ordering: the listed tracks render first (the audio track waits for them, as for a sidechain source). A track that plays rendered clips can't be a source for another one, so it can't recurse. The clip may play before its range (a swell ending on the drop it was made from).

### builtin:sampler

Plays sample libraries without a plugin: Bitwig `.multisample` instruments (the open zip + XML format of Bitwig's Sampler: pianos, organs, guitars, basses, keys, orchestral), SFZ instruments (the open text format most free and many commercial sample libraries ship in), folders of drum samples, or a single sample. Samples can be WAV, AIFF/AIFC, FLAC, MP3 (encoder delay removed) or Ogg Vorbis. List what is installed with `wavelength samples [--search text]`. Names are searched in `$WAVELENGTH_SAMPLES_PATH` (colon-separated folders) and the Bitwig Studio package folders; paths work too (relative to the job).

#### Library files: `lib:`

One file of an installed sample library, by name, so a job never depends on where a computer keeps it: `"lib:<library>/<file>"`, where `<library>` is a kit or loop folder as `wavelength samples` lists it (`"lib:Legend 909/Kick Legend 909 01 accent.wav"`, or with its category, `"lib:Classic Drum Machines/Legend 909/..."`), or `"lib:<path under a sample root>"` (`"lib:Bitwig/Anti-Loops/Genys/Kick from Tony's Beatbox.wav"`). Works for a sampler `sample`, a kit `map` entry and an audio clip `file`. A `lib:` name is never a file of the song: `pack` lists it in the manifest's `requires`, and `wavelength migrate` turns absolute library paths into `lib:` names.

```json
{"name": "Keys", "plugin": "builtin:sampler", "sampler": {"multisample": "Grand Piano", "release": 0.4}, "notes": [...]}
{"name": "Drums", "plugin": "builtin:sampler", "sampler": {"kit": "Legend 707", "map": {"36": "Kick Legend 707 02.wav"}}, "notes": [...]}
```

| Setting | Default | Meaning |
|---|---|---|
| `multisample` | | Name or path of a `.multisample` (or a folder with `multisample.xml`). Key and velocity zones, velocity crossfades, round robins, sustain loops and key tracking come from the file. Keys outside every zone stretch the nearest sample. |
| `sfz` | | Name or path of an `.sfz` file (a `multisample` ending in `.sfz` works too). See [SFZ](#sfz) below. |
| `soundfont` + `program` / `bank` / `preset` | bank 0 | A SoundFont (`.sf2`, or `.sf3` with Ogg Vorbis samples) by name or path, and one of its presets: General MIDI `program` 0-127 in `bank` (128 = drum kits, `program` picks the kit), or `"preset": "Violin"` by name. `wavelength samples --soundfont <name>` lists its presets; `wavelength samples --install-soundfont` downloads MuseScore General (MIT, 40 MB), the General MIDI set imports fall back on. See [SoundFonts](#soundfonts) below. |
| `kit` | | A folder of one-shot samples mapped to General MIDI keys by file name (36 kick, 38 snare, 39 clap, 37 rim, 42 closed hat, 46 open hat, 49 crash, 51 ride, 45/47/50 toms, 54 tambourine, 56 cowbell; unrecognised files take free keys from 60). `wavelength samples --kit <name>` prints the map. Or an object `{"36": "file.wav", ...}`. |
| `map` | `{}` | Key → file overrides on top of a kit (file names inside the kit folder, or paths), or `{"file": ..., "gain": dB, "pan": -1..1, "tune": semitones}`, or just the settings for the kit's own sample on that key. |
| `sample` + `root` | 60 | One sample played chromatically, `root` = the key it sounds at its own pitch. A path relative to the job, or an installed library's file by name: `"lib:Legend 909/Snare Legend 909 01 accent.wav"` (see below). |
| `attack`, `release` | 0.002 / 0.25 s (kits 0 / 0.05) | Amplitude envelope. |
| `oneShot` | kits true | Play samples to their end, ignoring note length. |
| `choke` | kits `[[42, 44, 46]]` | Key groups that cut each other (a closed hat stops the open hat). A one-key group chokes itself. |
| `retrigger` | `"overlap"` | `"cut"`: a new note on a key stops the previous one on that key. |
| `variants` | `"keys"` | Kits: takes of one sound (`Snare 01`, `Snare 02`) go to the drum's alternate General MIDI keys, then free keys from 60; `"roundrobin"` stacks them on one key and cycles through them; the General MIDI alternate keys (35, 40, 41/43, 48, 52, 55, 57, 59) then play their drum's takes, so a GM part still sounds. |
| `mono`, `glide` | false, 0 s | One voice at a time; overlapping notes play legato and glide (seconds) from the previous pitch: 808 slides, portamento leads. |
| `bpm` | none | The sample's own tempo: resampled (pitch and speed) to the song tempo at each note. |
| `slices` | none | With `sample`: cut the file into this many equal slices on keys `root`, `root+1`, ... (chop a break). |
| `start`, `length` | 0, whole sample | Trim every sample, in seconds of the file: skip `start`, then play at most `length` (like `builtin:audio`). |
| `reverse` | false | Play samples backwards (reverse cymbals and swells). The trimmed region is reversed as a whole: with `"length": 1.5` a reversed crash swells for 1.5 s and ends on its attack, so start the note 1.5 s before the hit. |
| `select` | 0 | Value (0-127) matched against multisample `select` ranges (alternate articulations). |
| `transpose` | 0 | Semitones. |
| `velocity` | 1 | Velocity sensitivity 0-1 (1 = about 7 dB quieter at half velocity). |
| `gain` | 0 | dB. |

#### Logic and GarageBand instruments (EXS)

`"sampler": {"exs": "Steinway Grand Piano 2"}` (a name from `wavelength samples --search`, or a path to an `.exs`) plays a Logic / GarageBand Sampler instrument (the EXS24 format). Zones keep their key and velocity ranges, root, tuning, level, pan, loops, one-shot, reverse and pitch-tracking settings; groups add their velocity layers, key ranges and level. Groups that Logic enables by a controller play when it rests at 0 (a piano's sustain-pedal resonance groups stay off), groups enabled by a MIDI channel (a guitar's six strings, a shaker's tempos) play from the lowest channel that covers each key, and groups with different articulation IDs become keyswitches from MIDI 0 in ID order (`Tuba Solo+`: 0 legato, 1 staccato), so `"articulations": {"legato": 0, "staccato": 1}` and a note's `"art"` pick them. Samples are found where the instrument says, else by name under Logic's and GarageBand's sample folders; a consolidated CAF (one file for the whole instrument) is read only in the ranges its zones play. Zones whose samples aren't installed (Logic's additional content) are left out with a warning. Logic names octaves with C3 = 60, so a sample named `A1` is MIDI 45: basses sound an octave below the key, as in Logic.

#### SFZ

`"sampler": {"sfz": "Splendid Grand Piano"}` (a name from `wavelength samples --search`, or a path) reads the file's `<control>`, `<global>`, `<master>`, `<group>` and `<region>` headers, `#define` and `#include`. What plays:

- Mapping: `sample` (relative to the file and `default_path`), `lokey`/`hikey`/`key` (numbers or note names, `c4` = 60), `pitch_keycenter`, `pitch_keytrack`, `lovel`/`hivel`, `note_offset`, `octave_offset`. Keys outside every region stay silent (unlike a multisample).
- Level and pitch: `volume`, `group_volume`/`master_volume`/`global_volume`, `amplitude`, `pan`, `tune` (cents), `transpose`, `amp_veltrack`, `amp_velcurve_N` (a velocity curve through the given points, from 0 at velocity 0 to 1 at 127 unless set; replaces `amp_veltrack`), `amp_keytrack`/`amp_keycenter` (dB per key), velocity crossfades `xfin_lovel`/`xfin_hivel`/`xfout_lovel`/`xfout_hivel`.
- Envelope: `ampeg_attack`, `ampeg_hold`, `ampeg_decay`, `ampeg_sustain`, `ampeg_release` (these replace the sampler's `attack`/`release` for that region).
- Playback: `offset`, `end`, `direction=reverse`, `loop_mode` (`no_loop`, `one_shot`, `loop_continuous`, `loop_sustain`), `loop_start`/`loop_end`, `loop_crossfade` (seconds), or the WAV's own loop (`smpl` chunk) when the region gives no points. A file whose regions are all `one_shot` plays like a kit.
- Round robins (`seq_length`/`seq_position`, `lorand`/`hirand` cycled in order), keyswitches (`sw_lokey`/`sw_hikey`/`sw_last`/`sw_default`: a note in the switch range picks the articulation and makes no sound), choke groups (`group`/`off_by`), `note_polyphony=1` (a repeated key cuts the previous note; set `"retrigger"` to override).
- Controllers stay at their `set_ccN` values (0 when unset): regions gated by `locc`/`hicc` play only if that holds (a piano's pedal-down resonance regions are left out), and `*cc*` modulation is ignored.
- Generators `*sine`, `*saw`, `*square`, `*triangle`, `*noise`, `*silence` (one cycle at `pitch_keycenter`, looped).
- Filters: `cutoff`, `resonance`, `fil_type` (low-, high- and band-pass), `fil_veltrack`, `fil_keytrack`, `fil_keycenter`. Release triggers (`trigger=release`: key-up sounds when the note ends, `rt_decay` dB quieter per second held), `delay` / `ampeg_delay`.

Not played: filter and pitch envelopes, LFOs, `<curve>` and `<effect>`. The render warns once, naming the opcodes it skipped. `examples/sfz-tour.json` plays `examples/sfz/tour.sfz`, built from generators only.

#### SoundFonts

`"sampler": {"soundfont": "MuseScore_General", "program": 40}` plays a preset of a SoundFont the way FluidSynth does: its zones (key and velocity ranges, stereo pairs), tuning, loops, the volume envelope (decay and release falling in dB, key-scaled hold and decay), the low-pass filter with its resonance and the modulation envelope that opens it, exclusive classes (a closed hat cuts the open one) and the modulators that follow velocity and key (level, filter, envelope times, pan, tuning). Controller modulators are read at their resting positions (volume 100, expression 127, pedals up). Checked against FluidSynth 2.6 on MuseScore General: levels within 0.5 dB, brightness within 2%. Not played: LFOs (vibrato, tremolo), reverb and chorus sends. SoundFonts are found in the sample roots and in Wavelength's settings folder under `soundfonts/` (where `--install-soundfont` puts them); `$WAVELENGTH_SOUNDFONT` names the one MIDI and MusicXML imports use.

## Buses, master, markers

| Field | Meaning |
|---|---|
| `buses` | `[{"name": "Hall", "gain": 0, "fx": [...], "output": "Glue", "stem": true, "automation": {"gain": curve, "rides": curve}}]`. Tracks reach them through `sends` or `output`; a bus returns to the master or to another bus (`output`), and runs after every bus that feeds it. `"stem": true` writes `stems/bus-<name>.wav`: the bus after its effects and before its fader and rides, the same signal as its `lufs` in the report (so `gain` = target - `lufs`, as for a track), in the job's `stems` format. |
| `master` | `{"gain": 0, "fx": [...], "automation": {"gain": curve, "rides": curve}, "loudness": -14}`, applied to the full mix before `normalize`. Gain automation fades the whole mix. `loudness` is a target in integrated LUFS after the chain: Wavelength finds the gain that lands on it, like a limiter's input gain. `"loudnessGain"` says where it goes: `"peak"` (default) in front of the trailing run of `clip`/`limiter` effects, so EQ and glue compressors before it see the mix as mixed and keep the section contrast while the clip still shaves the loud signal; `"limiter"` in front of the last built-in limiter only (after any clip before it); `"start"` before the whole chain (the behaviour before 0.2). A chain without a built-in limiter always gets it at the start; the report gives `mix.loudnessGainDb`, and a warning when the chain caps it. |
| `markers` | `[{"beat": 0, "name": "Intro"}, ...]`, the report gives loudness per section (before and after the master). `"checks": false` on a marker says the section is meant as it is (a false-ending silence, a soft peak): no dropout or weak-drop warnings for it. |
| `keys` | `[{"bar": 1, "key": "D minor"}, {"bar": 69, "key": "E minor"}]`: the key from each bar on (or `"beat"`), for `wavelength lint --harmony`; the render ignores it. Keys: a note and a mode, `"D minor"`, `"F# major"`, `"Bbm"`, `"C phrygian"` (major, minor, dorian, phrygian, lydian, mixolydian). Declare every planned key change here so the lint reads it as a new key, not as wrong notes; `"checks": false` on an entry makes a stretch chromatic on purpose (no harmony problems reported in it). |
| `chords` | `[[0, "C#m"], [80, "A"], [88, "B"]]` (beat, chord symbol) or `[{"bar": 21, "chord": "A"}, ...]`: the chord timeline that `"follow"` curves retune to (see `effects.md`). Symbols: a root (`C#`, `Bb`) and a quality: `m`, `dim`, `aug`, `sus2`, `sus4`, `5`, with `7`, `maj7`, `m7`, `m7b5`, `dim7`; a slash bass is ignored; `"-"` holds the previous chord. Without it, a `follow` curve uses the chords `wavelength lint --harmony --chords` reads from the notes (per bar, or per half bar when the halves differ). |

## Notes

| Field | Meaning |
|---|---|
| `beat`, `dur` | Start and length in beats (quarter notes), through the tempo map. |
| `time`, `length` | Start and length in seconds (use instead of `beat`/`dur`). |
| `key` | MIDI note number, or a name with C4 = 60: `"C4"`, `"F#3"`, `"Bb5"`. |
| `vel` | 0..1; values above 1 are read as MIDI velocity 0..127. Default 0.8. |
| `channel` | MIDI channel 0..15 (default 0). |
| `art` | Articulation: a name from the track's `articulations`, or a keyswitch key number/name. |
| `bend` | Pitch over the note, `[[beats after the note start, semitones], ...]` (`builtin:synth` and `builtin:sampler` tracks: scoops, bends, slides). Plugin tracks warn: use `automation.pitchbend` there. |
| `dyn` | Loudness over the note for tracks with `velocityTo` (BBC SO's Dynamics, CC1 libraries): `[0.2, 1.0]` swells from start to end, `[[beats after the start, 0-1], ...]` draws any shape (sforzando-piano, hairpins). It replaces the note's single velocity point in the `velocityTo` curve, so a held chord can crescendo with its tone changing, which a gain ride can't do. Without `velocityTo` it warns. |
| `marks` | Score marks kept by a MusicXML import (`["staccato", "accent", "fermata"]`); the render ignores them. |
| `vibrato` | `builtin:synth` and `builtin:sampler` tracks: depth in semitones (`0.3`), or `{"depth": 0.3, "rate": 5.5, "delay": 0.25, "rise": 0.25}` (rate in Hz, delay and fade-in in beats). Adds to `bend`, so a bent note can land and then shake. Plugin tracks warn: use `automation.pitchbend` there. |

Notes reach CLAP plugins as note events (or as MIDI if the plugin only accepts MIDI), VST3 plugins as note events and VST2 plugins as MIDI.

## Output

```
<out>/stems/01-lead.wav   32-bit float stereo, one per track, after its fx, before its fader
<out>/mix.wav             32-bit float stereo sum (normalized if requested)
<out>/report.json         LUFS and levels per track, bus, section and mix; warnings; timings
```
