# Effects, buses and automation

Effects run offline over a whole track, bus or the master, in the order listed.
They are deterministic: the same job renders the same audio every time.

## Where effects go

```json
{
  "tracks": [{
    "name": "Pad", "plugin": "nakst.Apricot", "notes": [ ... ],
    "fx": [ {"type": "eq", "bands": [{"type": "highpass", "freq": 140}]},
            {"type": "duck", "trigger": "Drums", "keys": [36], "depth": 6} ],
    "sends": {"Hall": -6},
    "automation": {"gain": [[0, -12], [16, 0]], "params": {"Filter Cutoff": [[0, 0.2], [16, 0.8]]}}
  }],
  "buses":  [{"name": "Hall", "gain": 0, "fx": [{"type": "reverb", "decay": 3.4, "mix": 1}]}],
  "master": {"gain": 0, "fx": [{"type": "compressor", "threshold": -12, "ratio": 1.6},
                               {"type": "limiter", "ceiling": -1.5}]},
  "markers": [{"beat": 0, "name": "Intro"}, {"beat": 16, "name": "Verse"}]
}
```

Signal flow per track: **instrument → `fx` → stem file → fader (`gain` + `automation.gain`, `pan`) → mix**,
and post-fader **sends** (dB) into buses. Buses run their own `fx` (use `"mix": 1` for reverbs and
delays there) and return to the mix. Then `master.gain`, `master.fx`, and optional `normalize`.

`markers` split the report into sections with their own loudness (LUFS).

Any effect can be skipped with `"bypass": true`.

## Automation

- **Fader:** `automation.gain`, dB added to the track's `gain` over time.
- **Plugin parameters:** `automation.params`, plain values over time, sent sample-block accurately.
- **Built-in effect settings** marked *automatable* below: put the curve under `"automate"` on the
  effect: `{"type": "filter", "cutoff": 800, "automate": {"cutoff": [[0, 300], [16, 8000]]}}`.

Curves are `[[beat, value], ...]`; values are held before the first point and after the last,
and interpolated linearly between points (exponentially for `cutoff`). Use two points close
together (e.g. `[55.5, 0], [56, -3]`) for a quick ramp at a section boundary.

## Built-in effects

| type | settings (defaults) |
|---|---|
| `gain` | `db` 0 *(automatable)* |
| `eq` | `bands`: list of `{type, freq, q (0.707), gain (dB)}`, type = `highpass`, `lowpass`, `bandpass`, `peak`, `lowshelf`, `highshelf` |
| `filter` | `mode` lowpass/highpass/bandpass, `cutoff` 1000 Hz *(automatable, exponential)*, `resonance` 0.707 *(automatable)*, `mix` 1 *(automatable)* |
| `delay` | `time` 0.75 beats (or `ms`), `feedback` 0.35, `pingpong` true, `highpass` 250, `lowpass` 5000 (in the feedback loop), `mix` 0.25 *(automatable)* |
| `reverb` | `decay` 2.5 s (RT60), `size` 0.7 (0–1), `predelay` 15 ms, `damping` 0.5 (0–1), `width` 1, `highpass` 150 Hz, `mix` 0.3 *(automatable)*, an 8-line feedback delay network |
| `compressor` | `threshold` −18 dB, `ratio` 3, `attack` 10 ms, `release` 150 ms, `knee` 6 dB, `makeup` 0 dB, `mix` 1, stereo-linked |
| `limiter` | `ceiling` −1 dB, `release` 80 ms, `lookahead` 5 ms, brickwall on sample peaks; true peaks can be ~0.5 dB higher, so use −1.5 for delivery |
| `saturate` | `drive` 6 dB *(automatable)*, `mix` 1 *(automatable)*, tanh |
| `chorus` | `rate` 0.3 Hz, `depth` 4 ms, `delay` 14 ms, `mix` 0.35 *(automatable)* |
| `width` | `amount` 1 (0 = mono, >1 wider) *(automatable)* |
| `duck` | `trigger` (track name), `keys` (optional list, e.g. `[36]` = kicks only), `depth` 8 dB, `attack` 8 ms, `hold` 20 ms, `release` 180 ms, sidechain-style pumping keyed from another track's notes |

`mix` is a dry/wet crossfade: 0 = dry only, 1 = wet only.

## CLAP effect plugins

`{"plugin": "<clap id or name>", "state": "...", "params": {...}, "automate": {"Param": [[b, v], ...]}, "mix": 1}`
runs an installed CLAP audio effect over the track, bus or master, with the same state, parameter
and automation support as instruments.

## Built-in instruments

- `builtin:drums`, synthesized kit on the General MIDI map: 35/36 kick, 37 rim, 38/40 snare,
  42/44 closed hat, 46 open hat, 49/57 crash, 51 ride, 41/43 low tom, 45/47 mid tom, 48/50 high tom.
- `builtin:fx`, 48 (C3) impact, 50 (D3) riser lasting the note's length, 52 (E3) reverse swell
  ending when the note ends, 53 (F3) sub drop.
