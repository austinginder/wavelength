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
| (defaults) | | Any top-level setting here can come from `defaults.json` in Wavelength's settings folder (see the README's Folders table) or `$WAVELENGTH_DEFAULTS`, e.g. `{"leadIn": 1}`; the job's own value wins, and the report's `defaultsApplied` lists what was filled in. |
| `leadIn` | 0 | Seconds of digital silence written before the audio in `mix.wav` and every stem (streaming uploads such as Spotify and SoundCloud like about 1 s). Notes, automation and loudness are unaffected; report `sections` start/end times refer to the written file. |
| `normalize` | none | If set, scale the mix so its peak is this many dBFS (e.g. `-1`). Stems are never normalized. |
| `parallel` | auto | Plugin tracks rendered at once, each in its own worker process (default: half the cores, up to 4, and fewer when the machine is already busy, e.g. with other renders). `0` renders everything in one process. `render --jobs N` overrides it. |
| `retries` | 2 | Times a worker whose plugin crashed is started again (0 to 10). A track that crashes on every attempt, or hangs, is left out of the mix and listed in `failedTracks`; the render then reports `"ok": false` and exits 1. |
| `stems` | `"float"` | Stem files: `"float"` (32-bit, keeps overs), `"24"`, `"16"`, or `"none"` (the report still has every track's loudness). `render --stems` overrides it. A render checks free disk space first. |
| `groove` | none | Swing and humanize for every track (a track's own `groove` overrides keys): `{"swing": 0.58, "grid": 0.25, "lay": 0.02, "humanize": {"time": 0.01, "vel": 0.06, "seed": 7}}`. `swing` 0.5 = straight, 0.667 = triplet feel, applied to notes on the off-steps of `grid` (beats; 0.25 = 16ths); `lay` shifts every note (beats, + = behind the beat); `humanize` adds random timing (beats) and velocity (fraction) deviations, deterministic per `seed`. |

## Tracks

| Field | Default | Meaning |
|---|---|---|
| `name` | `trackN` | Used for the stem file name and in the report. |
| `plugin` | required | A plugin id (`nakst.Apricot`, a VST3 class id), a name (`Apricot`, `BBC Symphony Orchestra`), a path to a `.clap`/`.vst3` bundle, or `path#id`. Prefix `vst3:` or `clap:` when a name exists in both formats (e.g. `vst3:Vital`). |
| `preset` | none | A preset from the plugin's own library, by name (`"OR Cathedral Organ"`), `"Category/Name"`, or a unique part of the name. List them with `wavelength presets <plugin>`: CLAP preset discovery, a VST3 plugin's factory program list (Dexed cartridges), preset files in the plugin's preset folders (Serum 2, Odin2, u-he, Surge XT, OB-Xf, `.vstpreset`), Dexed's DX7 cartridge voices, or NKS presets (DUNE 3, BBC Symphony Orchestra, ...). Applied before `state` and `params`. |
| `state` | none | A preset file path, or `{"file": "...", "format": "auto"}`. Formats: `clap-preset` (CLAP, Bitwig / DAWproject container), `vstpreset` (VST3 preset), `nksf` (NKS preset: the plugin's own state), `fxp` (VST2 `.fxp`/`.fxb` program chunks such as Surge XT and OB-Xf factory patches), `serum` (Serum 2 `.SerumPreset`), `juce-valuetree` (Odin2 `.odin`), `h2p` (u-he presets), `dx7` (a DX7 cartridge voice for Dexed: `"<cart>.syx#<voice 0-31>"`), `synplant` (Synplant `.synplant` patches), `cherry` (Cherry Audio presets), `ngrr` (Guitar Rig racks), `microtonic` (`.mtpreset` kits; `.mtdrum` drums as `"<file>.mtdrum#<channel>"`), `soundbox` (`.sbset`), `decentsampler` (`.dspreset`), `juce-string` (text presets such as Vital `.vital`), `raw`; `auto` detects them. A state or preset that changes none of the plugin's parameters gets a warning (it was ignored, or already loaded). |
| `params` | `{}` | `name → plain value` or the plugin's own display text (`"Cutoff": "800 Hz"`, `"StepRate": "1/16"`, parsed by the plugin), applied after the state. Keys: exact name, `Module/Name`, or `#id`. Out-of-range values are clamped (with a warning). |
| `gain` | 0 | dB applied when summing into the mix. |
| `transpose` | 0 | Semitones added to every note (presets that sound an octave off, key changes). |
| `output` | master | A bus name: the track feeds that bus instead of the master (group buses / sub-mixes). |
| `roll` | 0 | Beats between notes that start together, lowest first (strummed or rolled chords); negative rolls from the top. |
| `bendRange` | 2 | The plugin's pitch-bend range in semitones, so `automation.pitchbend` can be written in semitones. |
| `pan` | 0 | −1 (left) … 1 (right), equal-power. |
| `stem` | `true` = write this track's stem file (when the job writes stems); `false` skips it. |
| `mute` | false | Render the stem but leave it out of the mix. |
| `warmup` | job `warmup` | Seconds this plugin gets after activation, e.g. 5 for orchestral libraries that stream samples. |
| `notes` | `[]` | See below. |
| `fx` | `[]` | Effect chain (built-in or CLAP plugins), see `effects.md`. |
| `sends` | `{}` | Bus name → send level in dB (post-fader), or an automation curve of dB (`[[beat, dB], ...]`) for throws. |
| `articulations` | none | Articulation name → keyswitch key (`{"long": 0, "spiccato": 1, "tremolo": 3}`). Notes pick one with `"art"`; the keyswitch note is sent 30 ms before the first note of every change. |
| `range` | none | `[lowest, highest]` playable keys (`["G3", "C#7"]`): notes outside it get a warning in the report (sample libraries are silent there). |
| `velocityTo` | none | Drive a controller from note velocities, one point per onset, ramping between them: `{"param": "Dynamics", "min": 0.1, "max": 1}` or `{"cc": 1, "min": 10, "max": 127}`. For libraries whose long notes take loudness from a controller instead of velocity. Explicit automation of the same target wins. |
| `automation` | none | `gain` (dB), `rides` (dB added on top of `gain`: one curve, or named curves `{"sections": curve, "fills": curve}` that all add up, so section rides never overwrite the written fader curve), `pan` (-1..1), `params` (`{"Name": curve}`, plain values), `cc` (`{"1": curve, "64": curve}`, MIDI CC values 0-127), `pitchbend` (semitones, see `bendRange`), `pressure` (0-127). CC, pitch bend and pressure reach CLAP plugins as MIDI (or note expressions) and VST3 plugins through the parameters they map those controllers to (a warning names any they don't map). Curves are described in `effects.md` (points, steps, LFOs). |

`plugin` may also be `builtin:drums` or `builtin:fx` (see `effects.md`), `builtin:sampler`, or `builtin:audio`.

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
| `file` | required | A WAV: absolute, relative to the job, or relative to a sample root (Bitwig content, `$WAVELENGTH_SAMPLES_PATH`). |
| `beat` / `endAt` | one of them | Where the clip starts, or the beat where it ends (reverse swells, pickups). |
| `bpm` | none | The file's own tempo: the clip is sped up or slowed to the song's tempo at its anchor. |
| `speed` | 1 | An explicit speed factor instead of `bpm`. |
| `stretch` | true | Keep the pitch while changing speed (Signalsmith Stretch); `false` = tape-style, pitch follows speed. |
| `pitch` | 0 | Semitones, without changing length. |
| `start`, `length` | 0, whole file | Trim, in seconds of the file; or `beats` (with `bpm`) instead of `length`. |
| `reverse` | false | Play the trimmed audio backwards. |
| `gain`, `fadeIn`, `fadeOut` | 0 dB, 2 ms, 5 ms | Level and edge fades (ms). |

### builtin:sampler

Plays sample libraries without a plugin: Bitwig `.multisample` instruments (the open zip + XML
format of Bitwig's Sampler: pianos, organs, guitars, basses, keys, orchestral), folders of
drum WAVs, or a single WAV. List what is installed with `wavelength samples [--search text]`.
Names are searched in `$WAVELENGTH_SAMPLES_PATH` (colon-separated folders) and the Bitwig
Studio package folders; paths work too (relative to the job).

```json
{"name": "Keys", "plugin": "builtin:sampler", "sampler": {"multisample": "Grand Piano", "release": 0.4}, "notes": [...]}
{"name": "Drums", "plugin": "builtin:sampler", "sampler": {"kit": "Legend 707", "map": {"36": "Kick Legend 707 02.wav"}}, "notes": [...]}
```

| Setting | Default | Meaning |
|---|---|---|
| `multisample` | | Name or path of a `.multisample` (or a folder with `multisample.xml`). Key and velocity zones, velocity crossfades, round robins, sustain loops and key tracking come from the file. Keys outside every zone stretch the nearest sample. |
| `kit` | | A folder of one-shot WAVs mapped to General MIDI keys by file name (36 kick, 38 snare, 39 clap, 37 rim, 42 closed hat, 46 open hat, 49 crash, 51 ride, 45/47/50 toms, 54 tambourine, 56 cowbell; unrecognised files take free keys from 60). `wavelength samples --kit <name>` prints the map. Or an object `{"36": "file.wav", ...}`. |
| `map` | `{}` | Key → file overrides on top of a kit (file names inside the kit folder, or paths), or `{"file": ..., "gain": dB, "pan": -1..1, "tune": semitones}`, or just the settings for the kit's own sample on that key. |
| `sample` + `root` | 60 | One WAV played chromatically, `root` = the key it sounds at its own pitch. |
| `attack`, `release` | 0.002 / 0.25 s (kits 0 / 0.05) | Amplitude envelope. |
| `oneShot` | kits true | Play samples to their end, ignoring note length. |
| `choke` | kits `[[42, 44, 46]]` | Key groups that cut each other (a closed hat stops the open hat). A one-key group chokes itself. |
| `retrigger` | `"overlap"` | `"cut"`: a new note on a key stops the previous one on that key. |
| `variants` | `"keys"` | Kits: takes of one sound (`Snare 01`, `Snare 02`) go to the drum's alternate General MIDI keys, then free keys from 60; `"roundrobin"` stacks them on one key and cycles through them; the General MIDI alternate keys (35, 40, 41/43, 48, 52, 55, 57, 59) then play their drum's takes, so a GM part still sounds. |
| `mono`, `glide` | false, 0 s | One voice at a time; overlapping notes play legato and glide (seconds) from the previous pitch: 808 slides, portamento leads. |
| `bpm` | none | The sample's own tempo: resampled (pitch and speed) to the song tempo at each note. |
| `slices` | none | With `sample`: cut the file into this many equal slices on keys `root`, `root+1`, ... (chop a break). |
| `start` | 0 | Seconds to skip into every sample. |
| `reverse` | false | Play samples backwards (reverse cymbals and swells). |
| `select` | 0 | Value (0-127) matched against multisample `select` ranges (alternate articulations). |
| `transpose` | 0 | Semitones. |
| `velocity` | 1 | Velocity sensitivity 0-1 (1 = about 7 dB quieter at half velocity). |
| `gain` | 0 | dB. |

## Buses, master, markers

| Field | Meaning |
|---|---|
| `buses` | `[{"name": "Hall", "gain": 0, "fx": [...], "output": "Glue", "automation": {"gain": curve, "rides": curve}}]`. Tracks reach them through `sends` or `output`; a bus returns to the master or to another bus (`output`), and runs after every bus that feeds it. |
| `master` | `{"gain": 0, "fx": [...], "automation": {"gain": curve, "rides": curve}, "loudness": -14}`, applied to the full mix before `normalize`. Gain automation fades the whole mix. `loudness` is a target in integrated LUFS after the chain: Wavelength finds the gain that lands on it, like a limiter's input gain. `"loudnessGain"` says where it goes: `"peak"` (default) in front of the trailing run of `clip`/`limiter` effects, so EQ and glue compressors before it see the mix as mixed and keep the section contrast while the clip still shaves the loud signal; `"limiter"` in front of the last built-in limiter only (after any clip before it); `"start"` before the whole chain (the behaviour before 0.2). A chain without a built-in limiter always gets it at the start; the report gives `mix.loudnessGainDb`, and a warning when the chain caps it. |
| `markers` | `[{"beat": 0, "name": "Intro"}, ...]`, the report gives loudness per section. |

## Notes

| Field | Meaning |
|---|---|
| `beat`, `dur` | Start and length in beats (quarter notes), through the tempo map. |
| `time`, `length` | Start and length in seconds (use instead of `beat`/`dur`). |
| `key` | MIDI note number, or a name with C4 = 60: `"C4"`, `"F#3"`, `"Bb5"`. |
| `vel` | 0..1; values above 1 are read as MIDI velocity 0..127. Default 0.8. |
| `channel` | MIDI channel 0..15 (default 0). |
| `art` | Articulation: a name from the track's `articulations`, or a keyswitch key number/name. |
| `bend` | Pitch over the note, `[[beats after the note start, semitones], ...]` (sampler tracks: scoops, bends, slides). |
| `vibrato` | Sampler tracks: depth in semitones (`0.3`), or `{"depth": 0.3, "rate": 5.5, "delay": 0.25, "rise": 0.25}` (rate in Hz, delay and fade-in in beats). Adds to `bend`, so a bent note can land and then shake. Plugin tracks warn: use `automation.pitchbend` there. |

Notes are delivered as CLAP note events, or as MIDI if the plugin only accepts MIDI.

## Output

```
<out>/stems/01-lead.wav   32-bit float stereo, one per track, after its fx, before its fader
<out>/mix.wav             32-bit float stereo sum (normalized if requested)
<out>/report.json         LUFS and levels per track, bus, section and mix; warnings; timings
```
