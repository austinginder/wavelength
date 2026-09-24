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
| `tempo` | 120 | A BPM number, or a tempo map: `[{"beat": 0, "bpm": 76}, {"beat": 8, "bpm": 138}]` (steps, no ramps). Also sent to plugins as transport, so tempo-synced LFOs and delays follow it. |
| `timeSignature` | `[4, 4]` | Sent to plugins as transport. |
| `tail` | 3 | Seconds rendered after the last note-off, for releases and reverb. |
| `warmup` | 0.4 | Wall-clock seconds each plugin gets after activation to finish loading samples or restoring state. |
| `length` | 0 | Fixed render length in seconds (0 = last note + `tail`). |
| `normalize` | none | If set, scale the mix so its peak is this many dBFS (e.g. `-1`). Stems are never normalized. |

## Tracks

| Field | Default | Meaning |
|---|---|---|
| `name` | `trackN` | Used for the stem file name and in the report. |
| `plugin` | required | A plugin id (`nakst.Apricot`, a VST3 class id), a name (`Apricot`, `BBC Symphony Orchestra`), a path to a `.clap`/`.vst3` bundle, or `path#id`. Prefix `vst3:` or `clap:` when a name exists in both formats (e.g. `vst3:Vital`). |
| `preset` | none | A preset from the plugin's own library, by name (`"OR Cathedral Organ"`), `"Category/Name"`, or a unique part of the name. List them with `wavelength presets <plugin>`. CLAP plugins with preset discovery. Applied before `state` and `params`. |
| `state` | none | A preset file path, or `{"file": "...", "format": "auto"}`. Formats: `clap-preset` (CLAP, Bitwig / DAWproject container), `vstpreset` (VST3 preset), `nksf` (NKS preset: the plugin's own state), `juce-string` (text presets such as Vital `.vital`), `raw`; `auto` detects them. |
| `params` | `{}` | `name → plain value`, applied after the state. Keys: exact name, `Module/Name`, or `#id`. Out-of-range values are clamped (with a warning). |
| `gain` | 0 | dB applied when summing into the mix. |
| `pan` | 0 | −1 (left) … 1 (right), equal-power. |
| `mute` | false | Render the stem but leave it out of the mix. |
| `warmup` | job `warmup` | Seconds this plugin gets after activation, e.g. 5 for orchestral libraries that stream samples. |
| `notes` | `[]` | See below. |
| `fx` | `[]` | Effect chain (built-in or CLAP plugins), see `effects.md`. |
| `sends` | `{}` | Bus name → send level in dB (post-fader). |
| `automation` | none | `{"gain": [[beat, dB], ...], "params": {"Name": [[beat, value], ...]}}`. |

`plugin` may also be `builtin:drums` or `builtin:fx` (see `effects.md`), or `builtin:sampler`.

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
| `map` | `{}` | Key → file overrides on top of a kit (file names inside the kit folder, or paths). |
| `sample` + `root` | 60 | One WAV played chromatically, `root` = the key it sounds at its own pitch. |
| `attack`, `release` | 0.002 / 0.25 s (kits 0 / 0.05) | Amplitude envelope. |
| `oneShot` | kits true | Play samples to their end, ignoring note length. |
| `choke` | kits `[[42, 44, 46]]` | Key groups that cut each other (a closed hat stops the open hat). |
| `select` | 0 | Value (0-127) matched against multisample `select` ranges (alternate articulations). |
| `transpose` | 0 | Semitones. |
| `velocity` | 1 | Velocity sensitivity 0-1 (1 = about 7 dB quieter at half velocity). |
| `gain` | 0 | dB. |

## Buses, master, markers

| Field | Meaning |
|---|---|
| `buses` | `[{"name": "Hall", "gain": 0, "fx": [...]}]`, tracks reach them through `sends`. |
| `master` | `{"gain": 0, "fx": [...]}`, applied to the full mix before `normalize`. |
| `markers` | `[{"beat": 0, "name": "Intro"}, ...]`, the report gives loudness per section. |

## Notes

| Field | Meaning |
|---|---|
| `beat`, `dur` | Start and length in beats (quarter notes), through the tempo map. |
| `time`, `length` | Start and length in seconds (use instead of `beat`/`dur`). |
| `key` | MIDI note number, or a name with C4 = 60: `"C4"`, `"F#3"`, `"Bb5"`. |
| `vel` | 0..1; values above 1 are read as MIDI velocity 0..127. Default 0.8. |
| `channel` | MIDI channel 0..15 (default 0). |

Notes are delivered as CLAP note events, or as MIDI if the plugin only accepts MIDI.

## Output

```
<out>/stems/01-lead.wav   32-bit float stereo, one per track, after its fx, before its fader
<out>/mix.wav             32-bit float stereo sum (normalized if requested)
<out>/report.json         LUFS and levels per track, bus, section and mix; warnings; timings
```
