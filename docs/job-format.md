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
| `import` | none | Written by `import song.band`: where the job came from (`from`, `format`, `savedWith`, the project's `cycle` in beats) and what wasn't converted (`warnings`). Rendering ignores it. |
| `groove` | none | Swing and humanize for every track (a track's own `groove` overrides keys): `{"swing": 0.58, "grid": 0.25, "lay": 0.02, "humanize": {"time": 0.01, "vel": 0.06, "seed": 7}}`. `swing` 0.5 = straight, 0.667 = triplet feel, applied to notes on the off-steps of `grid` (beats; 0.25 = 16ths); `lay` shifts every note (beats, + = behind the beat); `humanize` adds random timing (beats) and velocity (fraction) deviations, deterministic per `seed`. |

## Tracks

| Field | Default | Meaning |
|---|---|---|
| `name` | `trackN` | Used for the stem file name and in the report. |
| `plugin` | required | A plugin id (`nakst.Apricot`, a VST3 class id), a name (`Apricot`, `BBC Symphony Orchestra`), a path to a `.clap`, `.vst3` or `.vst` bundle, or `path#id`. A name in several formats resolves to CLAP, then VST3, then VST2, then Audio Unit (macOS); prefix `clap:`, `vst3:`, `vst2:` or `au:` to pick one (e.g. `vst3:Vital`, `au:DLSMusicDevice`, or an AU by its codes, `au:aumu:dls :appl`). |
| `preset` | none | A preset from the plugin's own library, by name (`"OR Cathedral Organ"`), `"Category/Name"`, or a unique part of the name. List them with `wavelength presets <plugin>`: CLAP preset discovery, a VST3 plugin's factory program list (Dexed cartridges), a VST2 plugin's program list, preset files in the plugin's preset folders (Serum 2, Odin2, u-he, Surge XT, Surge 1.x, OB-Xf, OB-Xd bank programs, `.vstpreset`), Dexed's DX7 cartridge voices, or NKS presets (DUNE 3, BBC Symphony Orchestra, ...). Applied before `state` and `params`. |
| `state` | none | A preset file path, or `{"file": "...", "format": "auto"}`. Formats: `clap-preset` (CLAP, Bitwig / DAWproject container), `vstpreset` (VST3 preset), `nksf` (NKS preset: the plugin's own state), `fxp` (VST2 `.fxp`/`.fxb` program chunks such as Surge XT and OB-Xf factory patches; `"<bank>.fxb#<n>"` is program n of an OB-Xd or Full Bucket bank), `firefly` (Firefly Synth 2 `.ff2preset`), `juce-xml` (a JUCE XML preset such as Vaporizer2 `.vvp`, as the plugin's state), `serum` (Serum 2 `.SerumPreset`), `serumfx` (Serum 2 `.SerumFX` / `.SerumFXRack` into Serum 2 FX), `surgefx` (a Surge `.srgfx` effect preset into Surge XT Effects), `hise` (a HISE user `.preset`: the interface's control values), `reaktor` (a Reaktor 6 ensemble, `.ens` or `.rkplr` from a Native Instruments library: the state points Reaktor at the file and Reaktor opens it; `"<ensemble>.ens#<snapshot>"` then picks one of its first bank's snapshots), `juce-valuetree` (Odin2 `.odin`), `h2p` (u-he presets), `helix` (audjoo Helix `.hxp`), `kilohearts` (kHs snap-in presets, `.ksdl` and friends), `dx7` (a DX7 cartridge voice for Dexed: `"<cart>.syx#<voice 0-31>"`), `synplant` (Synplant `.synplant` patches), `echobode` (Echobode `.echobode` patches), `permut8` (a Permut8 `.p8bank`: `"<bank>.p8bank#<program 0-29>"`, else the bank's current program), `cherry` (Cherry Audio presets), `ngrr` (Guitar Rig racks), `microtonic` (`.mtpreset` kits; `.mtdrum` drums as `"<file>.mtdrum#<channel>"`), `soundbox` (`.sbset`), `decentsampler` (`.dspreset`), `juce-string` (text presets such as Vital `.vital`), `raw`; `auto` detects them. A state or preset that changes none of the plugin's parameters gets a warning (it was ignored, or already loaded). |
| `params` | `{}` | `name → plain value` or the plugin's own display text (`"Cutoff": "800 Hz"`, `"StepRate": "1/16"`, parsed by the plugin), applied after the state. Keys: exact name, `Module/Name`, or `#id`. Out-of-range values are clamped (with a warning). |
| `gain` | 0 | dB applied when summing into the mix. |
| `transpose` | 0 | Semitones added to every note (presets that sound an octave off, key changes). |
| `output` | master | A bus name: the track feeds that bus instead of the master (group buses / sub-mixes). |
| `roll` | 0 | Beats between notes that start together, lowest first (strummed or rolled chords); negative rolls from the top. |
| `arp` | none | Play the held notes as an arpeggio, on any instrument, the way Logic's and GarageBand's Arpeggiator does: `{"rate": "1/16", "order": "up", "octaves": 1, "gate": 0.8}`. Steps fall on the song's grid every `rate` (a note value, `"1/8T"`, `"1/8D"`, or beats); each step plays the next of the notes held there, ordered by `order` (`up`, `down`, `updown`, `downup`, `outsidein`, `played`: the order they were struck, `random`: seeded by `seed`, `chord`: all of them) and `variation` (1-4, Logic's: for `updown` 1 plays the top and bottom notes twice, 2 once; for `up` and `down` 2-4 regroup the notes; for `outsidein` 3-4 go inside out; for `random` 2 plays each note once per cycle, 3-4 favour low or high notes) over `octaves` (1-4; `"inversions": true` climbs through the chord's inversions instead), for `gate` of a step (0.01-1.5), at the held note's velocity. A phrase starts when a note goes down with none held and ends when the last is released, so back-to-back chords restart the pattern and overlapping ones carry on; a note up to a tenth of a step late still starts on its step. `steps` is a rhythm grid repeating from each phrase's start: a velocity (0-1) per step, `"rest"`, `{"vel": 0.8, "len": 2}` (a longer step ties over the next), `{"chord": true, "vel": 0.6}` (every held note at once). `swing` (0.5 straight to 0.99) delays every other step, `lengthRandom` (0-1) varies note lengths, `velocity` `{"base": 0.63, "range": 0.5, "random": 0.2}` squeezes velocities toward `base` and randomizes them, `cycle` (1-32 notes, or `"grid"`: the grid's note steps) restarts the order early. A GarageBand or Logic patch's Arpeggiator, or one of the Arpeggiator's presets, by name: `"arp": "Classic Analog Arp"`, or `{"preset": "Rolling 8ths", "octaves": 2}` / `{"patch": "..."}` with settings on top (`wavelength presets arp` lists 83 presets and the 122 patches that arpeggiate). An Alchemy patch's own arpeggiator counts too (57 of them): its rate, order, octave range and note length play, its step sequencer, shuffle and key velocity don't. A track playing such a patch (`"preset"` on builtin:synth, `"patch"` on the sampler) arpeggiates by itself, with the patch's other MIDI effects (see `midiFx`); `"arp": false` plays the notes as written. `"arp": {...}` is shorthand for `"midiFx": [{"type": "arp", ...}]` (a track takes one or the other). Write the chords as held notes; edits, lint and the picture see the arpeggio. |
| `midiFx` | none | The notes through a chain of MIDI effects, in order, before anything else reads them (any instrument), the way Logic's and GarageBand's MIDI effects play them: `[{"type": "chord", "intervals": [0, 4, 7]}, {"type": "repeat", "time": "1/8", "repeats": 3, "ramp": 0.8}]`. `chord` (Chord Trigger) plays a chord on every note, its `intervals` in semitones from the note, or with `chords` one chord per key (`{"C3": [0, 4, 7], "D3": [0, 3, 7]}`), the keys it doesn't map silent; `range` (`["C2", "B4"]`, keys as names or numbers) limits it to those keys (the others play as written) and `transpose` (-48 to 48) moves every chord. `transpose` (Transposer) moves every note `semitones` (-48 to 48), then onto the nearest note of its `scale` (ties go down): a key (`"A minor"`, `"D dorian"`, `"C minor pentatonic"`) or a list of notes (`["C", "Eb", "G"]`, or 0-11 with C = 0). `repeat` (Note Repeater) plays every note `repeats` more times (0-99, default 3), `time` apart (a note value, `"1/8T"`, `"1/8D"`, or beats; default `"1/8"`; or `ms`), each one `transpose` semitones from the one before (-48 to 48) at `ramp` times its velocity (0.01-2, default 1), as long as the note; `"thru": false` plays only the repeats, and `range` limits it to those keys (low over high: all but those). `arp` is the arpeggiator, with `arp`'s settings. Two notes that come out on one key at one moment sound once (the louder, then the longer), a repeat that strikes a key still sounding ends the earlier note there, and notes past MIDI's 0-127 are dropped. Each type takes a preset of the Apple effect it re-creates, by name or the last part of it when that is unique (`{"type": "chord", "preset": "Single/Triads/Major"}`; `wavelength presets chord`, `presets transpose` and `presets repeat` list them and the patches that use them), or a patch's (`{"type": "repeat", "patch": "Funk Bot"}`), with settings on top. A track playing a GarageBand or Logic patch (`"preset"` on builtin:synth, `"patch"` on the sampler) plays the patch's whole MIDI chain by itself: its Arpeggiator, Chord Trigger, Transposer and Note Repeater in their slot order (Classic Chords turns every note into a chord moved an octave down, Funk Bot arpeggiates and repeats each note 5 semitones lower), with a warning naming them; the track's own `midiFx` or `arp` replaces the chain, and `"midiFx": false` plays the notes as written. A Scripter that only passes notes on and GarageBand's built-in instrument scripts (Erhu, Koto, ...) play the notes as written; other scripts, the Velocity Processor, Randomizer and Modifier are left out with a warning, and a Single mode Chord Trigger left on a drum kit (Blue Ridge) is skipped. It runs on the notes as written, before `transpose`, `roll` and `groove`; edits, lint and the picture see what it plays. |
| `fallback` | none | Stand-in sounds for when this computer lacks the track's plugin (or its `builtin:sampler` library): a sound object or a list, tried in order, e.g. `[{"plugin": "Surge XT", "preset": "Trance Seq Bass"}, {"plugin": "builtin:synth", "preset": "BA Pluck", "gain": -2}]`. The first one available replaces the track's sound: `plugin`, `preset`, `state`, `params`, `synth`, `sampler`, `articulations`, `range`, `velocityTo`, `warmup` and the plugin's own `automation.params`/`cc`/`pressure` curves go, and every key the fallback gives is set (`gain`, `transpose`, `fx`, its own `automation`...). Notes lose their `art` when the fallback has no `articulations`. The report lists each swap first in `warnings` and in `fallbacks`; a track that can play neither its sound nor any fallback fails the render, naming both. `wavelength fallbacks --suggest` proposes built-in stand-ins at matched levels; `render --fallbacks` plays them all to hear the result. |
| `midiProgram`, `midiChannel` | none | Kept by `import song.mid` (the part's General MIDI program and channel) and written back by `export`; rendering ignores them. |
| `bendRange` | 2 | The plugin's pitch-bend range in semitones, so `automation.pitchbend` can be written in semitones. |
| `pan` | 0 | -1 (left) to 1 (right), equal-power. |
| `panLaw` | `"constant-power"` | `"balance"`: the side the track is panned to stays at its level and the other falls as (1 - \|pan\|)², as GarageBand and Logic pan a stereo track (half left takes the right 12 dB down). Imported GarageBand projects use it. |
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

**GarageBand's synth patches** (macOS): a `preset` that isn't one of the built-in patches can name a GarageBand or Logic patch on Retro Synth, ES2, ES1, EFM1, Vintage B3, Vintage Electric Piano, Vintage Clav, Sculpture or Alchemy (559 here, category `GarageBand` in `wavelength presets builtin:synth`), or a `.patch` folder by path (from the job's folder): its saved settings are re-created on this synth, with the patch's own effects after it.
- Retro Synth: Analog and Sync modes with their oscillators, pulse widths, hard sync and sine level; FM mode as a sine carrier and modulator; Table mode playing the wavetable a patch carries in its settings (read from the installed patch, never shipped) as additive oscillators at its Shape positions, held there (the LFO's or envelope's sweep through the table and the formant stretch aren't played), and saws where a patch plays Retro Synth's built-in Digiwaves, which no data file holds; filter, envelopes, LFO and vibrato, glide, unison, its chorus or flanger.
- ES2: three oscillators weighted by its mix triangle (Digiwaves play as sines), FM, hard sync and sine level, the filter its blend favours, the router's envelope, velocity, key, LFO and pitch routes, voices, glide, unison, distortion and its modulation effect. ES1: oscillator, sub, filter and envelopes, LFO, chorus. EFM1: a sine carrier and modulator at its harmonic ratio with the modulation envelope, the sub, unison, vibrato.
- Vintage B3: the upper manual's drawbars as sines (3 dB a step), percussion, key click, scanner vibrato, its EQ, distortion, rotor cabinet (with its speaker's roll-off, fitted on GarageBand's organ loops) and reverb (the lower manual and pedals aren't played).
- Vintage Electric Piano: its model's family as the voice (tine: a sine with a decaying FM bark and the tine bell; reed: a sine and a decaying square under a velocity-opened filter; Electra), decay, release, tune, voices, and its drive, EQ, Bass Boost, phaser, tremolo and chorus. Vintage Clav: its model's family (classic, funk, mellow, harpsichord, wood, sitar, dulcimer, harp) as oscillators, the pickup positions as the pulse width, Brilliance, Shape, string damping and decay, Damper, stiffness and tension modulation, the tone switches as EQ, and its wah, compressor, distortion and modulation effect. Damper and release clicks, stretch tuning and key-position stereo aren't played.
- Sculpture (13 of GarageBand's 15): its exciting object as a class (plucked, struck, bowed, blown, noise), the string's material and losses as its tone and decay (a stiff material as inharmonic FM), the morph pad, Resolution, its filter, amp envelope, vibrato, LFO, envelope and velocity routes to cutoff and the exciter's strength, pickup movement as a chorus, keyboard mode, glide, transpose, tune, Warmth, the waveshaper, Body EQ's Basic EQ and the delay. Patches played by side-chain audio, ones whose morph envelope is the sound, and sustained textures made of jitter and modulation a static voice can't play are refused with the reason.
- Alchemy (313 of GarageBand's 505 Alchemy patches, the ones built from virtual-analog, noise and additive sources): its preset text read with the Perform knobs and XY pads where the patch saved them, wheels and aftertouch at rest. Each source's oscillator (basic waves as themselves, named pulses at their width, other wavetables as the shape their family is named for), noise, tuning and level through the morph / xfade pad, unison, hard sync, the filter covering most of the level with its envelope, key and velocity follow (a second one after the voices; one source's own filter filters only that source and those with a filter of its kind), the amplitude AHDSR, a decaying pitch envelope, LFOs to pitch, cutoff, amp, pulse width and pan, mono, glide, and Alchemy's effects racks (delay, reverbs, Mod FX, distortion, MM Filter, band-pass, compressors, bass enhancer, panner, amp, 3-band EQ, phaser). Tuned filter types (comb, ring; formant ones play as EQ peaks at the vowel's formants), MSEG and sequencer movement, and modulation without a counterpart are left out. Additive sources play the partials they hold at rest: Num Partials and the element's effect units (Harmonic, Pulse/Saw, Saw+Noise, Beating, Stretch, Shift, Noise, Ripples, Spread, Auto Pan, Alchemy 1.x's pitch and pan profiles; Comb, Filter and EQ after the voices) over drawn partials or installed analysis data (.aaz of every version and coding, each partial's level averaged over its loop), as the waves they amount to where they do (a saw or square spectrum, a hard-synced saw, a detuned stack as unison, up to 12 sines) and as an additive oscillator otherwise; moving partials, the motion of partial noise and Strum are left out, and patches with an effect unit that isn't decoded are refused. For additive and granular patches an Amp effect an envelope opens plays at its held level, and of several per-source effects racks the one carrying the most level plays. Alchemy patches that play installed samples play on the sampler (`"sampler": {"patch": ...}`), granular ones too when their grains stand still, additive sources layered with samples as sampler regions beside them, and spectral sources in Noise mode as their analysed spectra plus a noise band; other spectral patches, granular ones whose grains move, and ones whose content isn't installed, are refused with the reason. An effects rack Bandpass whose high cut sits at 0 is open, not a 12 Hz low-pass.
- An approximation: the instruments' cutoff in Hz, envelope and modulation depths, FM indexes and levels were read from patch data, not measured against GarageBand. The render's warnings say it's a re-creation, and `wavelength samples --patch <name>` lists what's approximated.
 `"synth": {"effects": false}` leaves the patch's effects out; any other `synth` key changes the re-created patch as usual.

```json
{"name": "Acid", "plugin": "builtin:synth", "preset": "BA Acid", "params": {"resonance": 0.65},
 "automation": {"params": {"cutoff": [[32, 250], [48, 1600]]}},
 "notes": [{"beat": 32, "dur": 0.2, "key": "A2", "vel": 1}]}
```

`wavelength presets builtin:synth` lists the patches (Init, `BA` bass, `LD` lead, `PD` pad, `PL` pluck, `KY` keys, `BR` brass, `FX`); each plays about -18 LUFS on a typical phrase at velocity 0.8 and sounds at the written pitch. `wavelength params builtin:synth` lists the parameters: `cutoff` (Hz, sweeps exponentially), `resonance`, `drive`, `env`, `keytrack`, `detune`, `spread`, `pw`, `fm`, `lfo`, `sub`, `noise`, `glide`, `level`. Values may be text: `"800 Hz"`, `"2.4 kHz"`, a note name.

The `synth` object:

| Setting | Default | Meaning |
|---|---|---|
| `osc` | `[{"wave": "saw"}]` | Up to 12 oscillators: `wave` (`saw`, `square`/`pulse`, `triangle`, `sine`, `noise`, `additive`: see below), `level` 1, `octave`, `semi`, `cents`, `pw` 0.5 (pulse width), `decay` (seconds of its own fade: a tine or click on top of a sustained sound), on a noise oscillator `lowcut` and `highcut` (Hz, 12 dB/oct each: a band of noise, breath or air), and on a sine `fm`: `{"ratio": 2, "index": 1.5, "decay": 0.4, "sustain": 0}` (a sine modulator at `ratio` times the pitch; `index` sets brightness and falls to `sustain` of itself over `decay` seconds), `"filter": false` (it joins after the filter: a clean sine body under a filtered patch), `"sync": true` (hard sync: it restarts with every cycle of the first oscillator, so tuning it up with `semi` sweeps the classic sync timbre). `phase` (where its cycle starts at each note, 0-1: every unison copy starts there, so they begin in phase and beat apart; by default each copy starts at its own phase, or at 0 without unison). `keytrack` (dB per octave) and `keycenter` (a key, C4 unless set) scale its level across the keyboard: it plays at `level` on the `keycenter` and on the side the sign leaves alone, and fades by `keytrack` dB an octave on the other (`"keytrack": -12, "keycenter": "C3"`: full below C3, 12 dB quieter at C4; a positive value fades the keys below instead). Saw and pulse are band-limited (a synced oscillator's restart isn't). |
| `unison` | 1 | Copies of every oscillator: a count, or `{"voices": 7, "detune": 30, "spread": 0.8}` (cents from lowest to highest copy, stereo width). Copies start at random (seeded) phases, so supersaws sound full. |
| `sub`, `noise` | 0, 0 | A square an octave below the note, and white noise. Above about 0.2 the sub takes over the pitch: the part sounds (and `analyze` reads it) an octave lower. |
| `filter` | lowpass 8000 | `type` (`lowpass`, `highpass`, `bandpass`, `off`), `slope` 12 or 24, `cutoff` (Hz or text), `resonance` 0-1, `keytrack` 0-1 (from C4), `env` (octaves the filter envelope adds), `velocity` (octaves darker at velocity 0), `drive` 0-1 (saturation into the filter). |
| `amp`, `filterEnv` | 3 ms / 0.3 s / 1 / 0.15 s; 0 / 0.3 s / 0 / 0.3 s | ADSR: `attack`, `decay`, `sustain` 0-1, `release` (seconds); `amp` also takes `velocity` 0-1 (how much velocity changes the level, 0.6) and `keytrack` (-2 to 2; at 1 the decay and release halve every octave above C4 and double below, as a piano's low notes ring longer). A sound that decays to a sustain of 0 ends there. |
| `pitchEnv` | none | `{"amount": 36, "decay": 0.06}`: semitones added at the attack, falling away (zaps, kick-like thumps). |
| `lfo` | none | One LFO or a list of up to 4: `rate` (Hz or a note value, `"1/8"`), `depth`, `shape` (as automation LFOs), `to` (`pitch` in semitones, `cutoff` in octaves, `amp` 0-1, `pw`, `pan`), `delay` and `fade` (seconds after each attack: delayed vibrato). |
| `mono`, `legato`, `glide` | false, true, 0 | `mono`: one voice. A note that starts while another is held slides to it without a new attack (`legato`), gliding over `glide` seconds; back-to-back notes attack again. |
| `level` | 0 | dB. |

Per-note `bend` and `vibrato` play on builtin:synth tracks as on the sampler, and `automation.pitchbend` (semitones) bends every voice. MIDI CC and pressure automation do nothing here: automate parameters by name.

**Additive oscillators.** `{"wave": "additive", ...}` plays a list of sine partials, alias-free at any pitch:

| Key | Meaning |
|---|---|
| `partials` | `[[amplitude, ratio], ...]`, or `[amplitude, ratio, pan]` with pan -1..1: each partial at `ratio` times the note's frequency (above 0, at most 1024; whole numbers are harmonics). Up to 1024 partials. |
| `harmonics` | A harmonic series instead of (or under) the list: `{"count": 64, "tilt": -6, "odd": 1, "even": 1}`, or just a count. `tilt` is dB per octave (-6 falls like a saw, 0 is flat), `odd` and `even` scale those harmonics (the fundamental is odd: `"even": 0` gives a square's series). |
| `partialWave` | `sine` (default), or `saw`, `square` (with `pw`) or `triangle`: every partial plays that wave instead, its harmonics joining the list (an organ of saws). |
| `shiftHz` | Moves every partial up (or down) by that many Hz: an inharmonic, metallic spectrum that doesn't follow the key. |

Amplitudes are relative: when the partials together hold more power than one full sine they are scaled down together, so a long list plays about as loud as a single oscillator at the same `level`. Partials above 0.45 times the sample rate are left out (faded from 0.40) at every pitch, glide and vibrato included. Harmonic sets play from precomputed tables and cost about what a saw does; inharmonic or shifted ones play as a bank of sines (at most 512 partials across the unison voices: the loudest ones play), so keep those lists short. `octave`, `semi`, `cents`, `level`, `decay`, `filter`, `sync` (harmonic sets only) and unison work as on any oscillator.

```json
{"name": "Glass", "plugin": "builtin:synth", "synth": {"osc": [{"wave": "additive", "partials": [[1, 1], [0.5, 2.76], [0.3, 5.4, -0.4], [0.2, 8.93, 0.4]]}],
 "filter": {"type": "off"}, "amp": {"attack": 0.002, "decay": 2.5, "sustain": 0, "release": 1}},
 "notes": [{"beat": 0, "dur": 2, "key": "E5", "vel": 0.8}]}
```

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
| `file` | required | An audio file (WAV, AIFF, CAF, FLAC, MP3, Ogg Vorbis or M4A, told apart by content; AAC and Apple Lossless decode on macOS only): relative to the job (in a song: under `media/`), a library file by name (`"lib:Legend 909/Kick Legend 909 01 accent.wav"`, `"lib:Apple Loops/Early Days Piano.caf"`, see [Library files](#library-files-lib)), an absolute path, or relative to a sample root; or `{"render": ...}`, the song's own audio (below). |
| `beat` / `endAt` | one of them | Where the clip starts, or the beat where it ends (reverse swells, pickups). |
| `bpm` | none (an Apple Loop: its own) | The file's own tempo: the clip is sped up or slowed to the song's tempo at its anchor. |
| `speed` | 1 | An explicit speed factor instead of `bpm`. |
| `stretch` | true | Keep the pitch while changing speed (Signalsmith Stretch); `false` = tape-style, pitch follows speed. |
| `pitch` | 0 | Semitones, without changing length. |
| `start`, `length` | 0, whole file | Trim, in seconds of the file; or `beats` (with `bpm`) instead of `length`. |
| `reverse` | false | Play the trimmed audio backwards. |
| `repeat` | 1 | Play the trimmed audio this many times back to back (a loop for 4 bars: `"beats": 16, "repeat": 4`). |
| `key` | none | Apple Loops: move the loop into a key, `"D minor"`, or into the job's own key at the clip, `"song"` (from `keys`). Adds to `pitch`. |
| `gain`, `fadeIn`, `fadeOut` | 0 dB, 2 ms, 5 ms | Level and edge fades (ms). |

**Apple Loops** (macOS: the loops GarageBand and Logic install in `/Library/Audio/Apple Loops`, and your own in `~/Library/Audio/Apple Loops`). `wavelength loops --search "hip hop piano"` lists them with category, key, tempo and length in beats; `"file": "lib:Apple Loops/<name>.caf"` plays one. A loop knows its tempo, so it follows the song's tempo with its pitch kept (`"bpm"` or `"speed"` override that), and its key, so `"key": "song"` moves a tonal loop into the song's key: tonic to tonic, or to the relative key when one is minor and the other major (so its notes stay in the song's scale), never more than 6 semitones. GarageBand itself goes tonic to tonic whatever the modes (a D minor loop in a C major song plays in C minor), and an imported GarageBand project's loops play that way, with each region's own gain and Transpose. Drum and effect loops have no key and play as recorded. Software-instrument loops ("notes" in the list) also carry their notes: `wavelength loops --notes <name> --key "D minor"` prints them in beats for a track's `notes`, and `wavelength import <loop.caf>` makes a job of them.

```json
{"name": "Keys", "plugin": "builtin:audio", "clips": [
  {"file": "lib:Apple Loops/Early Days Piano.caf", "beat": 0, "repeat": 4, "key": "song"}]}
```

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

Plays sample libraries without a plugin: Bitwig `.multisample` instruments (the open zip + XML format of Bitwig's Sampler: pianos, organs, guitars, basses, keys, orchestral), SFZ instruments (the open text format most free and many commercial sample libraries ship in), DecentSampler presets (without the DecentSampler plugin), folders of drum samples, or a single sample. Samples can be WAV, AIFF/AIFC, FLAC, MP3 (encoder delay removed) or Ogg Vorbis. List what is installed with `wavelength samples [--search text]`. Names are searched in `$WAVELENGTH_SAMPLES_PATH` (colon-separated folders), the Bitwig Studio package folders and DecentSampler's library folder; paths work too (relative to the job).

#### Library files: `lib:`

One file of an installed sample library, by name, so a job never depends on where a computer keeps it: `"lib:<library>/<file>"`, where `<library>` is a kit or loop folder as `wavelength samples` lists it (`"lib:Legend 909/Kick Legend 909 01 accent.wav"`, or with its category, `"lib:Classic Drum Machines/Legend 909/..."`), `Apple Loops` for any Apple Loop by its file name (`"lib:Apple Loops/Early Days Piano.caf"`, whatever folder it is in), or `"lib:<path under a sample root>"` (`"lib:Bitwig/Anti-Loops/Genys/Kick from Tony's Beatbox.wav"`). Works for a sampler `sample`, a kit `map` entry and an audio clip `file`. A `lib:` name is never a file of the song: `pack` lists it in the manifest's `requires`, and `wavelength migrate` turns absolute library paths into `lib:` names.

```json
{"name": "Keys", "plugin": "builtin:sampler", "sampler": {"multisample": "Grand Piano", "release": 0.4}, "notes": [...]}
{"name": "Drums", "plugin": "builtin:sampler", "sampler": {"kit": "Legend 707", "map": {"36": "Kick Legend 707 02.wav"}}, "notes": [...]}
```

| Setting | Default | Meaning |
|---|---|---|
| `multisample` | | Name or path of a `.multisample` (or a folder with `multisample.xml`). Key and velocity zones, velocity crossfades, round robins, sustain loops and key tracking come from the file. Keys outside every zone stretch the nearest sample. Or a folder of samples named by their notes (`"Huge Saw C3.wav"`, `"Amanda Aa 1 E3.wav"`): each plays from its note and shares the keys with its neighbours, files on the same note are round robins, and the files' own loop points (WAV `smpl`) hold long notes. Libraries count octaves two ways (C4 or C3 = 60), so a few samples' measured pitch decides. On a Mac, GarageBand's Alchemy sample folders are listed this way (synths, basses, pads, organs, solo and synth vocals). |
| `sfz` | | Name or path of an `.sfz` file (a `multisample` ending in `.sfz` works too). See [SFZ](#sfz) below. |
| `dspreset` | | Name or path of a DecentSampler preset: a `.dspreset`, a `.dsbundle` folder or a `.dslibrary` (`"<bundle or library>#<preset>"` picks one of several). See [DecentSampler](#decentsampler) below. |
| `soundfont` + `program` / `bank` / `preset` | bank 0 | A SoundFont (`.sf2`, or `.sf3` with Ogg Vorbis samples) by name or path, and one of its presets: General MIDI `program` 0-127 in `bank` (128 = drum kits, `program` picks the kit), or `"preset": "Violin"` by name. `wavelength samples --soundfont <name>` lists its presets; `wavelength samples --install-soundfont` downloads MuseScore General (MIT, 40 MB), the General MIDI set imports fall back on. See [SoundFonts](#soundfonts) below. |
| `kit` | | A folder of one-shot samples mapped to General MIDI keys by file name (36 kick, 38 snare, 39 clap, 37 rim, 42 closed hat, 46 open hat, 49 crash, 51 ride, 45/47/50 toms, 54 tambourine, 56 cowbell; drum machine abbreviations too, `BD1`, `SD2`, `HH1`, `HHo`, `CP`, `RS`; unrecognised files take free keys from 60). On a Mac, GarageBand's drum machine kits are listed (`"Boutique 808"`, `"Boutique 78"`, the Drum Machine Designer kits such as `"Trap Door GB"`). `wavelength samples --kit <name>` prints the map. Or an object `{"36": "file.wav", ...}`. |
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

`"sampler": {"exs": "Steinway Grand Piano 2"}` (a name from `wavelength samples --search`, or a path to an `.exs`) plays a Logic / GarageBand Sampler instrument (the EXS24 format). Zones keep their key and velocity ranges, root, tuning, level, pan, loops, one-shot, reverse and pitch-tracking settings; groups add their velocity layers, key ranges and level. Groups that Logic enables by a controller play when it rests at 0 (a piano's sustain-pedal resonance groups stay off), groups enabled by a MIDI channel (a guitar's six strings, a shaker's tempos) play from the lowest channel that covers each key, and groups with different articulation IDs become keyswitches from MIDI 0 in ID order (`Tuba Solo+`: 0 legato, 1 staccato), so `"articulations": {"legato": 0, "staccato": 1}` and a note's `"art"` pick them. Samples are found where the instrument says, else by name under Logic's and GarageBand's sample folders; a consolidated CAF (one file for the whole instrument) is read only in the ranges its zones play. Zones whose samples aren't installed (Logic's additional content) are left out with a warning. Logic names octaves with C3 = 60, so a sample named `A1` is MIDI 45: basses sound an octave below the key, as in Logic. The instrument's own parameters play too: its volume, tuning and transposition, velocity sensitivity and amplitude envelope (delay, attack, hold, decay, sustain, release); a track's `attack` or `release` replaces the instrument's. Its filter, LFOs and modulation matrix are not played.

**GarageBand and Logic patches.** `"sampler": {"patch": "Steinway Grand Piano"}` plays a patch from GarageBand's or Logic's library (or your own in `~/Music/Audio Music Apps/Patches`) when its channels play Logic's samplers: Sampler (whose patches carry the whole instrument), EXS24 or Drum Kit Designer. Every such channel's instrument plays, merged into one track (a multi-channel drum kit plays its kit). The patch's own effects play too, in its insert order, where Wavelength has a counterpart, decoded from the patch's saved settings: Channel EQ and Single Band EQ (`eq` bands; low and high cuts of 6-48 dB/oct as cascaded biquads), Compressor (`compressor` with its auto gain, input and output gain, output distortion as a `clip` and its limiter), Tape Delay, Stereo Delay and Delay Designer (`delay`, one tap), Space Designer (`convolve` with its room, or a `reverb` for a synthesized one), SilverVerb, PlatinumVerb and ChromaVerb (`reverb`), Overdrive and Clip Distortion (`saturate`), Bitcrusher, Chorus, Ensemble and Flanger (`chorus`), Noise Gate (`gate` with a threshold), Phaser, Tremolo, Limiter, Gain, Enveloper's level, and the guitar and bass amps: Amp Designer, Bass Amp Designer and Pedalboard's stompboxes (gain stages as `saturate`, tone stacks, cabinets and microphones as `eq`, their tremolo, vibrato, spring reverb, wahs, modulation and delays). Effects the patch has switched off (often behind a Smart Control knob) and ones saved silent (a Wet or Mix of 0) stay out, and a room that isn't installed is left out with a warning. Gain-Q coupling, compressor circuit types, delay grooves, wow and flutter, flanger feedback and reverb room types are approximated, and so are the amps' gain laws and cabinet curves; MIDI effects play as the track's `midiFx`; sends don't play. `"effects": false` plays the dry instrument. `wavelength samples --search <text>` lists the patches that play here, and `wavelength samples --patch <name>` shows one: its channels and instruments, how many of their samples are installed, its effects, and its sends with the Space Designer room of each, ready for a `convolve` bus (`{"name": "Hall", "fx": [{"type": "convolve", "ir": "06.6s Botta Church-OST", "mix": 1}]}`, `"sends": {"Hall": -13.8}`) so the patch sits in the space GarageBand gives it. Ultrabeat drum machine patches (`"Trap Door GB"`, `"Boutique 808 GB"`) play their kit's samples, which GarageBand installs as kit folders, without Ultrabeat's own synthesis and effects (a warning says so). Patches on Retro Synth, ES2, ES1, EFM1, Vintage B3, Vintage Electric Piano, Vintage Clav and Sculpture, and Alchemy's virtual-analog patches, are synths: they play on builtin:synth by their names (see [builtin:synth](#builtinsynth)), and the sampler's error says so. Alchemy patches built on its sampler (22 here, such as `"Celestial Voices"`) play here: their samples as SFZ regions (each source's tuning, level, key and velocity ranges, loops, start position), the amplitude envelope, a static filter where its envelopes rest, VA layers as SFZ generators, mono and glide, and Alchemy's effects racks before the channel strip's. So do 6 on granular sources whose grains stand still (such as `"Neon Synth Bass"`): at Speed 100% the sample itself, frozen (Speed 0) a short crossfaded loop at Position, a slow scan the sample at its own rate (the grain texture and Alchemy's time-kept transposition are lost). Alchemy patches built on spectral synthesis or moving grains, ones layering additive sources with samples, and ones whose content isn't installed, are refused with the reason. GarageBand installs the samples of most patches only when asked (GarageBand > Sound Library > Download All Available Sounds); a patch whose samples are missing says so.

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

#### DecentSampler

`"sampler": {"dspreset": "Malevolence Keys 5"}` (a name from `wavelength samples --search`, or a path) plays a DecentSampler instrument without the plugin: a `.dspreset` (XML), a `.dsbundle` folder holding one, or a `.dslibrary` (a zip of a bundle, its samples read from inside it). Presets in DecentSampler's library folder (the one its settings name) are listed with the folder or library they belong to. The preset becomes SFZ regions and plays as described under [SFZ](#sfz), with DecentSampler's own rules and levels (measured against DecentSampler 1.11):

- Samples: key and velocity ranges, root, tuning (`tuning`, also spelled `tunning`, plus `groupTuning` and `globalTuning`, in semitones), volume in dB (`"-3dB"`) or linear, multiplied over `<groups>`, `<group>` (with `modVolume`) and `<sample>`, pan on a constant-power law (inherited, plus `groupPan` and `globalPan`), `start`/`end`, `pitchKeyTrack`, release triggers (`trigger="release"`, `releaseTriggerDecay`), samples for a controller range at rest (`loCCN`/`hiCCN`, as SFZ), and `sine`, `saw`, `square`, `triangle` and `noise` oscillators. Attributes set on `<groups>` or `<group>` reach every sample under them unless the sample sets its own; a repeated attribute counts once, the last one.
- Loops: `loopEnabled`, `loopStart`/`loopEnd`, `loopCrossfade` (frames); a sample with no loop settings loops on its file's own loop points, as in DecentSampler.
- Envelope: `attack`, `decay`, `sustain`, `release` with DecentSampler's curve shapes (`attackCurve`, `decayCurve`, `releaseCurve`; by default a fast-rising attack and exponential decay and release; 0.5 s release when none is set); `ampEnvEnabled="false"` plays one-shots. A track's `attack` or `release` replaces the preset's.
- Velocity: `ampVelTrack` (gain 1 - t + t x velocity / 127; 1 by default).
- Round robins: `seqMode` with `seqPosition`: every sample at the chosen position plays (stereo or mic layers sound together); `random` modes cycle in order. Samples with positions but no `seqMode` all play at once, as in DecentSampler.
- Tags: a `<tag>`'s volume and `enabled` reach every sample carrying it (its own tags and its group's); `silencedByTags` makes choke groups (an open hi-hat cut by a closed one).
- Controls: every knob, slider, menu and button fires its bindings at its saved position (a knob's `value`, a menu's selected option, a button's state), through the binding's `factor` and `translation` (`linear` over the control's range onto `translationOutputMin`/`Max`, `table`, `fixed_value`), the way DecentSampler sets an instrument up when it loads: instrument, group and tag volumes, tuning, pan, the envelope, `ampVelTrack`, groups and tags switched on or off (a menu choosing one articulation of several), and effect parameters. Velocity bindings play for the volume (a velocity curve) and for a filter's frequency (the filter opens with velocity, per voice, from its frequency to 22 kHz).
- Effects, after the instrument: `lowpass` (and `lowpass_4pl`, `lowpass_1pl`), `highpass` and `bandpass` as `filter`; `peak`, `notch`, `low_shelf` and `high_shelf` as `eq`; `gain`; `reverb` (room size as decay time, damping) and `delay` (time, feedback, `feedbackCutoff`), each adding its wet signal to the full dry one at DecentSampler's level (a `reverb` or `delay` with its mix and a `gain` after it); `chorus` and `phaser` with their depth and rate; `convolution` as `convolve`. A filter inside a group plays on that group's samples. `"effects": false` plays the dry instrument.
- Levels: as DecentSampler plays them, a sample in the centre 5.3 dB under its own level, and through DecentSampler's output stage after the effects (also with `"effects": false`): a compressor on what passes -6 dBFS (4:1) and a limiter at 0 dBFS, so presets that drive it hard come out as loud as they do there.
- A preset DecentSampler saved (with `samplePath` naming the library on the computer that saved it) plays from the samples beside it.

Left out, each named in the render's warning (`wavelength samples --dspreset <name>` lists them all and shows the effects as played): MIDI controller and note bindings (controllers rest), modulators (LFOs, envelopes), glide, `legato`/`first` triggers, tag polyphony, buses and auxiliary outputs, the delay's stereo offset, tempo-synced delays, and effects without a counterpart (pitch shifter, wave folder and shaper).

#### SoundFonts

`"sampler": {"soundfont": "MuseScore_General", "program": 40}` plays a preset of a SoundFont the way FluidSynth does: its zones (key and velocity ranges, stereo pairs), tuning, loops, the volume envelope (decay and release falling in dB, key-scaled hold and decay), the low-pass filter with its resonance and the modulation envelope that opens it, exclusive classes (a closed hat cuts the open one) and the modulators that follow velocity and key (level, filter, envelope times, pan, tuning). Controller modulators are read at their resting positions (volume 100, expression 127, pedals up). Checked against FluidSynth 2.6 on MuseScore General: levels within 0.5 dB, brightness within 2%. Not played: LFOs (vibrato, tremolo), reverb and chorus sends. SoundFonts are found in the sample roots and in Wavelength's settings folder under `soundfonts/` (where `--install-soundfont` puts them); `$WAVELENGTH_SOUNDFONT` names the one MIDI and MusicXML imports use.

## Buses, master, markers

| Field | Meaning |
|---|---|
| `buses` | `[{"name": "Hall", "gain": 0, "fx": [...], "output": "Glue", "stem": true, "automation": {"gain": curve, "rides": curve}}]`. Tracks reach them through `sends` or `output`; a bus returns to the master or to another bus (`output`), and runs after every bus that feeds it. `"stem": true` writes `stems/bus-<name>.wav`: the bus after its effects and before its fader and rides, the same signal as its `lufs` in the report (so `gain` = target - `lufs`, as for a track), in the job's `stems` format. |
| `master` | `{"gain": 0, "fx": [...], "automation": {"gain": curve, "rides": curve}, "loudness": -14}`, applied to the full mix before `normalize`. Gain automation fades the whole mix. `loudness` is a target in integrated LUFS after the chain: Wavelength finds the gain that lands on it, like a limiter's input gain. `"loudnessGain"` says where it goes: `"peak"` (default) in front of the trailing run of `clip`/`limiter` effects, so EQ and glue compressors before it see the mix as mixed and keep the section contrast while the clip still shaves the loud signal; `"limiter"` in front of the last built-in limiter only (after any clip before it); `"start"` before the whole chain (the behaviour before 0.2). A chain without a built-in limiter always gets it at the start; the report gives `mix.loudnessGainDb`, and a warning when the chain caps it. |
| `markers` | `[{"beat": 0, "name": "Intro"}, ...]`, the report gives loudness per section (before and after the master). `"checks": false` on a marker says the section is meant as it is (a false-ending silence, a soft peak): no dropout or weak-drop warnings for it. |
| `keys` | `[{"bar": 1, "key": "D minor"}, {"bar": 69, "key": "E minor"}]`: the key from each bar on (or `"beat"`), for `wavelength lint --harmony` and for Apple Loop clips with `"key": "song"`; the render ignores it otherwise. Keys: a note and a mode, `"D minor"`, `"F# major"`, `"Bbm"`, `"C phrygian"` (major, minor, dorian, phrygian, lydian, mixolydian). Declare every planned key change here so the lint reads it as a new key, not as wrong notes; `"checks": false` on an entry makes a stretch chromatic on purpose (no harmony problems reported in it). |
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

### Note edits: `edits.json`

A job is often written by a script (`make-job.py`) that makes its notes from patterns, so there is no list of notes to change by hand, and a hand edit to `job.json` is gone the next time the script runs. `edits.json` beside the job holds changes to its notes that every reader applies on top of whatever wrote the job: `render` (and its workers, previews, windows), `lint`, `serve` and its live notes. The `serve` editor writes it (move, delete and add notes, then Save to song and Render).

```json
{"format": "wavelength.edits", "formatVersion": "1.0", "edits": [
  {"track": "Lead", "at": {"beat": 204, "key": 88}, "to": {"key": 90}},
  {"track": "Lead", "at": {"beat": 272, "key": 86}, "to": {"beat": 272.5, "dur": 0.5, "vel": 0.7}},
  {"track": "Lead", "at": {"beat": 144, "key": 86}, "delete": true},
  {"track": "Lead", "add": {"beat": 146, "dur": 1, "key": 81, "vel": 0.8}}
]}
```

- A note is found by its track (`name` or `id`), its `beat` and its **sounding** key (after the track's `transpose`, as the timeline shows it). Keys in `to` and `add` sound too. Notes placed with `time` (seconds) can't be edited this way.
- Edits apply in order, so a later edit can change a note an earlier one moved.
- An edit whose note is no longer there (the script changed) is skipped with a warning on its track: `edits.json: an edit of E6 at beat 204.00 no longer matches a note ...`.
- `serve` adds `batch`, `time` and `by` to each edit it saves (Undo last save removes a batch). Readers ignore keys they don't know.
- To make the edits part of the script instead, change the script, render, and remove the edits it now covers (a moved note that the script already writes where the edit put it would no longer match, and warns).

## Output

```
<out>/stems/01-lead.wav   32-bit float stereo, one per track, after its fx, before its fader
<out>/mix.wav             32-bit float stereo sum (normalized if requested)
<out>/report.json         LUFS and levels per track, bus, section and mix; warnings; timings
```
