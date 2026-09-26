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

Signal flow per track: **instrument → `fx` → stem file → fader (`gain` + `automation.gain`, `pan`) → mix**
(or the bus named in the track's `output`), and post-fader **sends** (dB) into buses. Buses run their own
`fx` (use `"mix": 1` for reverbs and delays there) and return to the mix or to their `output` bus, after
every bus that feeds them. Then `master.gain` (+ `master.automation.gain`), `master.fx`, and optional
`normalize`.

`markers` split the report into sections with their own loudness (LUFS).

Any effect can be skipped with `"bypass": true`.

## Automation

- **Fader:** `automation.gain`, dB added to the track's `gain` over time.
- **Plugin parameters:** `automation.params`, plain values over time, sent sample-block accurately.
- **Built-in effect settings** marked *automatable* below: put the curve under `"automate"` on the
  effect: `{"type": "filter", "cutoff": 800, "automate": {"cutoff": [[0, 300], [16, 8000]]}}`.

Curves are `[[beat, value], ...]`; values are held before the first point and after the last,
so a curve that starts late holds its first value from the top of the song: the report warns when that
value differs from the static one and something already sounds through it (track, bus and master
`gain` and `rides`, track `pan` and plugin parameters, and effect `automate` curves on tracks, buses and
the master; a bus hears its earliest feeding track, by output, send or another bus). Values are
interpolated linearly between points (exponentially for `cutoff`). A third element
`"step"` (`[56, -3, "step"]`) holds the previous value until that beat and then jumps. The object
form `{"points": [...], "curve": "linear" | "exp" | "step", "lfo": {...}}` sets the curve for all
points, and `{"value": 0.5, "lfo": {...}}` is a steady value with an LFO on it.

**LFOs** add a wave on top of any automatable value: on a built-in effect with
`"lfo": {"cutoff": {"rate": "1/8", "depth": 1.5, "shape": "sine"}}` (next to `automate`), or inside
any curve's object form (plugin parameters, fader, pan, sends). `rate` is Hz or a tempo-synced
note value (`"1/4"`, `"1/8T"` triplet, `"1/16D"` dotted, `"2/1"` two bars); `shape` sine, triangle,
square, saw (falling), ramp (rising), random (sample and hold); `phase` 0-1; `depth` in the value's
units (octaves for cutoff), or a curve `[[beat, depth], ...]` to fade the LFO in and out.

Also automatable: bus and master `automation.gain` (whole-mix fades, bus throws), send levels
(`"sends": {"Echo": [[0, -40], [31.5, -40, "step"], [31.5, -6], [32, -40, "step"]]}`), track `pan`,
and MIDI `cc` / `pitchbend` / `pressure` for plugins (`job-format.md`).

## Built-in effects

| type | settings (defaults) |
|---|---|
| `gain` | `db` 0 *(automatable)* |
| `eq` | `bands`: list of `{type, freq, q (0.707), gain (dB)}`, type = `highpass`, `lowpass`, `bandpass`, `peak`, `lowshelf`, `highshelf` |
| `filter` | `mode` lowpass/highpass/bandpass, `cutoff` 1000 Hz *(automatable, exponential)*, `resonance` 0.707 *(automatable)*, `mix` 1 *(automatable)* |
| `delay` | `time` 0.75 beats (or `ms`), `feedback` 0.35, `pingpong` true, `highpass` 250, `lowpass` 5000 (in the feedback loop), `mix` 0.25 *(automatable)* |
| `reverb` | `decay` 2.5 s (RT60), `size` 0.7 (0–1), `predelay` 15 ms, `damping` 0.5 (0–1), `width` 1, `highpass` 150 Hz, `mix` 0.3 *(automatable)*, an 8-line feedback delay network |
| `compressor` | `threshold` −18 dB, `ratio` 3, `attack` 10 ms, `release` 150 ms, `knee` 6 dB, `makeup` 0 dB, `mix` 1, stereo-linked; `sidechain` (track name) detects on that track's audio instead: real kick-keyed pumping |
| `limiter` | `ceiling` −1 dB, `release` 80 ms, `lookahead` 5 ms, `truePeak` true (4x oversampled detection, so the ceiling holds for inter-sample peaks too; the report gives the mix's `truePeakDb`) |
| `clip` | `ceiling` -1 dB, `kneeDb` (where shaping starts, in dB under the ceiling: 2-3 is a gentle transient shaver) or `knee` 0.5 (the same as a fraction of the ceiling's amplitude: 0.5 starts 6 dB under it, which already distorts a loud mix; 0.2-0.3 is typical), `drive` 0 dB *(automatable)*. A soft clipper that never exceeds the ceiling: put it before the master `limiter` (or on a kick or clap) to shave the first milliseconds of transients instead of limiting the whole mix. Warns when it shapes so much that it's distortion. |
| `saturate` | `drive` 6 dB *(automatable)*, `mix` 1 *(automatable)*, tanh. Quiet material comes out up to `drive` louder: add `"match": true` (see Level match) so an automated `drive` changes the tone and not the balance. |
| `chorus` | `rate` 0.3 Hz, `depth` 4 ms, `delay` 14 ms, `mix` 0.35 *(automatable)* |
| `width` | `amount` 1 (0 = mono, >1 wider) *(automatable)* |
| `duck` | `trigger` (track name), `keys` (optional list, e.g. `[36]` = kicks only), `depth` 8 dB *(automatable)*, `attack` 8 ms, `hold` 20 ms, `release` 180 ms, sidechain-style pumping keyed from another track's notes |
| `tremolo` | `rate` "1/8" (Hz or note value), `shape` sine, `phase`, `depth` 0.5 (0-1) *(automatable)*, `spread` 0 (right channel phase offset; 0.5 = autopan) |
| `pan` | `position` 0 (-1..1) *(automatable, LFO-able)*: balance a stereo signal |
| `gate` | `pattern` step string (`"x-x-xx--"`, digits 0-9 for levels) with `step` "1/16" (trance gate), or `trigger` (track name) + `keys` + `hold` 120 ms (opens on each note: gated reverb); `attack` 1 ms, `release` 15 ms, `depth` 80 dB, `mix` 1 *(automatable)* |
| `rotary` | Leslie speaker: `speed` 0 (chorale) to 1 (tremolo) *(automatable; the rotors speed up and slow down with inertia)*, `crossover` 800 Hz, `hornSlow`/`hornFast` 0.8/6.7 Hz, `drumSlow`/`drumFast` 0.7/5.8 Hz, `mix` 1 *(automatable)* |
| `autowah` | envelope follower sweeping a resonant filter: `min` 350 Hz, `max` 2800 Hz, `resonance` 3, `sensitivity` 0 dB, `attack` 6 ms, `release` 120 ms, `mode` bandpass/lowpass, `mix` 1 *(automatable)* |
| `bitcrush` | `bits` 8 *(automatable)*, `downsample` 1 (sample-and-hold factor) *(automatable)*, `mix` 1 *(automatable)* |
| `vibrato` | pitch wobble (tape wow, flutter, vibrato): `rate` 5.5 Hz (or note value), `depth` 20 cents (max 100) *(automatable)* |
| `tapestop` | `speed` 1 (1 = normal, 0 = stopped, up to 4) *(automatable)*: varispeed of the incoming audio, pitch and time together; whenever speed is back at 1 the output is in sync again |
| `multiband` | `crossovers` [250, 2500] Hz (1-3, rising: 2-4 bands, Linkwitz-Riley 4th order, sums back flat), `bands`: one object per band with `fx` (any effect chain: compressor, saturate, eq, plugins), `gain` dB, `solo`, `mute`; `mix` 1. A multiband compressor: `{"type": "multiband", "crossovers": [257, 2840], "bands": [{"fx": [{"type": "compressor", "threshold": -20, "ratio": 2}]}, {}, {}]}` |

`mix` is a dry/wet crossfade: 0 = dry only, 1 = wet only.

### Level match (`"match"`, any effect)

`"match": true` on any effect, built-in or plugin, holds its output at the input's loudness over time:
K-weighted input and output power, smoothed over `matchMs` (300 ms, zero phase), set a gain after the
effect (limited to +12 / -40 dB). Driven `saturate`, `clip` or `bitcrush`, a resonator that adds 12-16 dB
(kHs Resonator), a comb or flanger freeze whose level depends on where it froze: each changes the tone and
not the balance, without pumping on transients. Where the input is near silence (45 dB under its loud
parts) the gain holds, so a ring or tail the effect adds is not pulled down; raise `matchMs` (1000-3000)
for effects whose tails outlast the notes. `"match": "static"` uses one gain for the whole timeline
(the overall loudness), so heavily driven bars still come out louder. Match suits inline effects; a
reverb or delay at `"mix": 1` on a bus is a return, set its level with the bus `gain` instead.
`{"plugin": "kHs Resonator", "params": {"Intensity": 0.8}, "match": true}`

## Plugin effects (CLAP, VST3 and VST2)

`{"plugin": "<id or name>", "state": "...", "preset": "...", "params": {...}, "automate": {"Param": [[b, v], ...]}, "mix": 1}`
runs an installed CLAP, VST3 or VST2 audio effect over the track, bus or master, with the same state, preset,
parameter and automation support as instruments (`mix` is automatable too). `"sidechain": "Kick"` feeds
that track's audio (after its effects, before its fader) into the plugin's sidechain input; many plugins
also need their own sidechain switch set in `params` (e.g. MTurboComp `"Side-chain input (Detector)": 1`).
A warning says when the plugin has no sidechain input. An effect whose output is
only a level-scaled copy of its input gets a warning (unlicensed or demo mode, bypass, an ignored preset).

## Built-in instruments

- `builtin:drums`, synthesized kit on the General MIDI map: 35/36 kick, 37 rim, 38/40 snare,
  42/44 closed hat, 46 open hat, 49/57 crash, 51 ride, 41/43 low tom, 45/47 mid tom, 48/50 high tom.
- `builtin:fx`, 48 (C3) impact, 50 (D3) riser lasting the note's length, 52 (E3) reverse swell
  ending when the note ends, 53 (F3) sub drop.
