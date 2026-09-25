# Changelog

All notable changes to Wavelength. Versions follow semantic versioning.

## [Unreleased]

### Fixed
- `"stems": "none"` no longer creates an empty `stems/` folder.
- `lint` compared every pair at every voice's note starts, so a third voice's rhythm split a pair's
  moves and hid real parallels (four parallel octaves in Fugue for the Night Shift's exposition went
  unreported) or moved where they were reported. Each pair is now compared at its own note starts.
- The skill's installer needed `curl` to download a release; without it (minimal Linux images) it
  quietly built from source and said there was no release build. It now uses `wget` when there is
  no `curl`, and says why it falls back to a source build.
- Linux release archives packed on a Mac carried a macOS extended attribute that made GNU `tar`
  print a warning on extraction.

## [0.2.0] - 2026-09-25

Upgrading from 0.1.0: a song with a master `loudness` target mixes a little differently (the
target's gain now goes in front of the final clip/limiter, see Changed; `"loudnessGain": "start"`
restores the old sound), bus faders staged from `buses[].lufs` need restaging (buses are now
measured before their fader), a render that loses a track exits 1 with `"ok": false`, and every
command rejects options it doesn't know.

### Added
- Bitwig projects: `import` reads the `.bwproject` behind a Bitwig DAWproject export (found by name,
  or `--bitwig`) for what the export leaves out: Drum Machine pads become a track each (plugins with
  their states, Sampler pads as `builtin:sampler`, pad volume, pan and mute) into a drum bus, and
  Chain, EQ+, Compressor, Multiband FX-3, Peak Limiter and Tool become built-in effects with the
  plugins inside them (a master chain comes across whole). A plugin whose VST3 isn't installed falls
  back to its VST2 with the preset converted; Komplete Kontrol uses its VST2 (the VST3 renders
  silence). `import song.bwproject` lists a project's tracks, devices, pads and states. Reads 743 of
  745 test projects (Bitwig 4 to 6); an imported song renders within 0.3 dB of its Bitwig bounce's
  section-by-section loudness contour.
- `wavelength import song.dawproject` (and `render song.dawproject`): a DAWproject export (Bitwig,
  Studio One, Cubase) becomes a job: the arrangement's notes (clips, loops, nested lanes), tracks with
  their CLAP / VST3 / VST2 plugins and saved states, volume, pan, mute, sends to effect returns,
  groups, tempo and tempo map, time signature, markers, volume and pan automation, and the standard
  EQ, compressor and limiter devices. What the file can't carry (a DAW's own devices and their
  samples, launcher clips, audio tracks, device automation) is listed. A Bitwig 6 export of 8 tracks
  and 1808 notes imports in under a second and renders in 26 s.
- VST2 hosting: `.vst` bundles (macOS), `.dll` (Windows) and `.so` (Linux) are scanned in child
  processes and play like CLAP and VST3 plugins (MIDI notes, CC, pitch bend, pressure, transport,
  parameters and automation, `.fxp`/`.fxb` state chunks and parameter lists, factory programs by name,
  `state save` to `.fxb`). Wavelength declares the VST 2 binary interface itself; Steinberg's
  withdrawn VST 2 SDK is not used. 29 native VST2 plugins on the dev Mac, including LoudMax.
- Intel-only plugins on Apple silicon: macOS builds are universal, the catalog records a bundle's
  architecture, and scans and render workers for x86_64-only plugins run under Rosetta
  (`params`, `presets`, `state` and `audition` re-run themselves that way). 34 more plugins load on
  the dev Mac, among them Reaktor 6, Kontakt, Synth1, TyrellN6 and Helm.
- `duration` in the render report: the written file's length, lead-in included.
- `lint --split "Organ=4"` (a chord track as voices, top to bottom), `--from`/`--to` bars and
  `--section NAME`, and both chords' notes with each problem.
- Marker `"checks": false` for sections that are meant as they are: their dropouts are reported as
  `"intended": true` and they get no arrangement warnings (a trailer's false-ending silence, a soft
  ambient peak).
- `sections[].preMasterLufs`: each section's loudness before the master gain, rides, chain and
  loudness target, so a ride the master limiter hands back shows where it went.
- Per-note `dyn` curves: `[start, end]` or `[[beats, level], ...]` shape the `velocityTo` controller
  (BBC SO's Dynamics) across a note, so a held brass chord crescendos with its timbre changing. The
  album's orchestral builds had to fake crescendos with volume rides.
- `automation.rides` on tracks, buses and the master: dB curves (one, or named ones) added on top of
  `automation.gain`. Album agents replaced their fader curves when they added section rides (a pad
  came out 9 dB too loud in the drops); rides are now a separate layer.
- Arrangement checks in the render report: `dropouts` (the song goes 15 dB quiet for 0.75 s or more
  and then comes back, with bars and file time) and a warning when a Drop/Chorus/Peak/Hook/Final
  section lands under +2 dB over the section before it; `sections[].change` gives every step.
  Album listening found both on most tracks (a two-bar hole before Afterburner's final hit, drops
  0-2 dB over their builds).
- `analyze --every S`: loudness of every S-second window, labelled with the render's sections.
- AGENTS.md "Arranging: builds, drops and endings": the defaults that fixed the album's drops and
  endings.
- `clip` effect: a soft clipper (ceiling, knee, drive) to shave transients before the master
  limiter; album agents built one out of gain, saturate and gain to stop a 909 kick pumping the
  limiter. `"kneeDb"` sets where the curve starts in dB under the ceiling (`knee`, a fraction of
  the ceiling's amplitude, confused three agents: 0.5 starts shaping 6 dB down and distorts).
- Track `"stem": false` skips that track's stem file; `render --tracks` sets it on the muted helper
  tracks, which wrote 2 GB of silent stems on a nearly full disk.
- `wavelength lint <job.json>`: voice leading between melodic tracks (parallel fifths and octaves in
  similar motion, voice crossings with `--crossings`), with bar and beat. Pairs that move in
  octaves or fifths most of the time are reported as doublings, not faults. Best with `--tracks`
  naming the contrapuntal voices (a fugue's subject, answer and bass).
- `analyze --grid BPM [--div 4]`: each onset's song-time beat and its offset in ms from the nearest
  grid step, plus the mean offset, to check a break or a groove against the grid.
- `tracks[].postFaderPeakDb`, and when the master limiter works hard a warning naming the three
  tracks whose peaks feed it most (with how far their peaks stand above their loudness): album
  agents needed extra renders to find that a spiky kick or clap was limiting the whole song.
- A warning when an automation curve starts late and its first value differs from what the song
  would otherwise start at (the effect's or parameter's static value, 0 dB for gain rides): a
  curve holds its first value before its first point, which muffled a whole Theme behind a filter
  curve that began at bar 65.
- `saturate` `"match": true`: level-matched saturation (output RMS equal to the input's), so
  changing the drive doesn't change the track's loudness.
- `render --tracks "Lead,Bass"`: render only the named tracks, through the song's buses and
  master; tracks that key their `duck`, `gate` or sidechain render muted. The report lists them in
  `onlyTracks`.
- Per-note `vibrato` on sampler tracks (depth, rate, delay, rise), layered on top of `bend`; guitar
  leads needed dozens of hand-written bend points per note. Per-note `bend` or `vibrato` on a
  plugin track now warns that it only plays on the sampler.
- `analyze --song-time`: `--start`/`--end` in song time; the render's lead-in is read from the
  report next to the file. Without it, a window on a render with a lead-in prints a note that the
  times are file times (agents measured the wrong notes after the 1 s lead-in default).
- A warning when a master loudness target adds more than 8 dB before the master chain, which
  makes every master compressor and the limiter work that much harder than their settings say.
- The loudness range (LRA, EBU Tech 3342, matches ffmpeg's `ebur128`) in the render report
  (`mix.lra`), `master` (input and output) and `analyze`.
- Labelled per-section loudness: `tracks[].sections` and `buses[].sections` are
  `[{"name", "lufs"}]` lists; buses get per-section loudness for the first time.
- `skills/wavelength/`: an agent skill that installs the engine (release build, else from source, docs matched to the binary) and runs the whole song workflow.
- Linux and Windows support. Wavelength builds and runs on macOS (universal), Linux (x86_64 and
  arm64) and Windows (x86_64), with the platforms' standard CLAP and VST3 folders, worker
  processes, and caches and settings in each platform's usual place.
- Release binaries for all five targets, built on one Mac with `scripts/build-release.sh`
  (Docker for Linux, llvm-mingw for Windows), each smoke-tested before packaging. The Windows
  archive carries the licences of the runtime libraries linked into it (`licenses/`).
- The `/wavelength` skill's installer downloads the release build on Linux and Windows (Git Bash)
  too, and builds from source on Linux when there is none.
- `examples/arrangement-tour.json`, which `scripts/check.sh` renders and checks: rides, `clip`,
  level-matched `saturate`, vibrato, a skipped stem, a loudness target and a build that earns its
  drop.
- `wavelength plugins --block <plugin> [--reason TEXT]` and `--unblock`: a blocked plugin (an
  unlicensed one that opens a registration window on every load, one that keeps crashing) is
  marked BLOCKED in `plugins`, and render, params, presets and audition refuse it by name.
- `"retries"` job setting (default 2): a track whose plugin crashes in its worker process is
  rendered again, and the track says so in its warnings. Altitude crashed about 1 render in 5
  under load (a race in its own threads); the song now comes out complete.

### Fixed
- `analyze` could read a note a twelfth or an octave low when a waveform repeats exactly only every
  few cycles: a chip square lead's B5 read as E4 (an oscillator whose edges fall on the sample grid
  in a 3-cycle pattern). A reading is now moved up to k times the frequency when nearly all the
  energy sits on every k-th harmonic and the signal is nearly as periodic at the shorter period,
  which leaves real low notes alone (a cello's C2 is mostly its 3rd harmonic and still reads C2).
  Checked on 256 synthetic tones, 85 sampled and BBC SO notes, and the stems of five songs.
- An unknown option at the end of a command (`analyze mix.wav --bogus`) was reported as missing
  its value; it is now named as unknown, with the options the command takes.
- `analyze --grid` printed the mean distance from the grid as "mean offset (+ = late)". It now
  prints both: the signed mean offset (the groove's push or lay) and the mean distance
  (`grid.meanOffsetMs` and `meanAbsOffsetMs` in JSON).
- `analyze --every` ignored `--song-time`; with it, windows now start on the first beat and print
  song time.
- The `clip` distortion warning fired when a tenth of the samples were shaped (it counted both
  channels against a fifth), and always advised `"kneeDb": 2`, even when the knee already was.
- `analyze` took a lead-in from any report.json next to (or above) a file, so a decoded MP3 in an
  unrelated folder got a lead-in note. It now uses a report only when it wrote that file (a
  render's mix or stem, or a `master` output).
- `analyze` gave no pitch for windows shorter than about 85 ms; a 60 ms window (one short note) now
  reads its pitch (fundamentals down to about 50 Hz).
- `analyze` onset times were about 25 ms early (the spectral frame's start, not the attack); they
  are now within one hop (about 5 ms), measured on a click track. A window that cuts into a sound
  no longer reports an onset at its first frame.
- `wavelength master` on a mix rendered with a lead-in put every section 1 s off and added a
  second lead-in. It now reads the lead-in from the render's report.json (or `--input-lead-in`),
  lines the markers up, and keeps the lead-in on the output unless `--lead-in` says otherwise.
- Loudness of a window shorter than 0.4 s (an `analyze` window, a one-hit section) read -120 LUFS;
  it is now the window's plain K-weighted loudness.
- A plugin that opens a window while rendering (a licence or registration dialog) no longer holds
  the render until the hang timeout: on macOS the worker is killed within about 2 s and the track
  fails with a message that suggests `plugins --block`.
- Kits with `"variants": "roundrobin"` left the General MIDI alternate keys (35 kick 2, 40 snare 2,
  43 floor tom, 48 tom, 57 crash 2...) empty, so a GM drum part went partly silent. They now play
  their drum's round-robin takes.
- `wavelength master --chain` read a `{"master": {...}, "markers": [...]}` file (a job without
  tracks) as an empty chain and passed the mix through unchanged without a word. That shape now
  works, and a chain with no effects and no loudness target is an error naming the keys it found.
- The chorus, delay, reverb and rotary effects could read one sample past their delay buffer
  when a modulated delay time landed a hair below a whole sample, returning whatever memory
  came next (the likely cause of a Dexed lead through chorus and delay reading +460 LUFS once).
- A render with failed tracks reports `"ok": false` with an `error` naming them and exits 1. It
  used to say `"ok": true` with only `failedTracks` showing that the mix was missing a part.
- Garbage samples (not a number, or above +30 dBFS; DUNE 3 once wrote a single +79 dBFS sample
  into a brass chord) no longer click, excite every reverb downstream and wreck the track's
  loudness reading: they are muted after every plugin, effect and built-in instrument, the track
  warns with the time, and a track reading above +6 LUFS warns that its output is broken.

### Changed
- Every track warning is also listed in the top-level `warnings`, prefixed with the track's name.
  Furnace Liturgy's agent read only the top-level list and missed a late gain curve that held the
  lead organ 9 dB down through the first drop.
- The weak-drop check measures the boundary as it is heard: the section's first 4 bars against the
  last 2 bars before it (`sections[].transition` in the report, for every boundary). Whole-section
  averages read Hammerklang's Final as +3.3 dB while its drop landed +1.5.
- The weak-drop check also covers any section after a build-like one (Build, Rise, Pre..., Ramp,
  Climb, Lead-in, Ignition), whatever it is called, and asks an escalation (Climax, Peak, Finale after
  another payoff) to rise at least 0.5 dB. Build -> Discovery and Final Act -> Climax went unchecked.
- A curve that starts late only warns when the track sounds before it (its first note or clip): a
  melody whose filter curve begins with its first note at bar 21 no longer warns.
- Every command rejects options it doesn't know, naming the ones it takes. They used to be
  ignored: an agent with newer docs than its binary ran `render --tracks` and silently got the
  whole song.
- The limiter's warning adds its gain reduction per marker section (average and maximum), so the
  drops' limiting shows next to the quiet sections'.
- A master `loudness` target adds its gain in front of the chain's trailing run of `clip` and
  `limiter` effects (`"loudnessGain": "peak"`, the default) instead of before the whole chain, so
  glue compressors and EQ ahead of it keep their settings and the section contrast (three album
  tracks lost 1-2 dB of contrast to the old placement) while a `clip` before the limiter still sees
  the loud signal. `"limiter"` puts it in front of the last limiter only; `"start"` restores 0.1.
- The limiter's gain-reduction warning says how long it worked (share of the time above 3 dB and
  6 dB), so one hot transient reads differently from sustained crushing, and it also warns when
  more than 10 % of the song sits above 6 dB of reduction.
- The automatic `parallel` setting takes the system load into account: with other renders
  already filling the cores, a render starts fewer workers instead of oversubscribing the CPU
  (ten agents rendering at once drove the load average past 70 on 8 cores).
- `buses[].lufs` and `buses[].levels` are measured before the bus fader, like a track's, so
  `gain` = target − lufs works for buses too. They used to be measured after it.
- A plugin library that fails to load names the library and the reason (on Linux the VST3 SDK
  only said "dlopen failed").

## [0.1.0] - 2026-09-24

The first release. Wavelength renders JSON music jobs through installed CLAP and VST3 plugins
and sample libraries, headlessly, and reports what an agent can't hear.

Highlights:
- CLAP and VST3 hosting with plugin delay compensation; plugin tracks render in parallel worker
  processes, so a crashing plugin costs its track, not the song.
- About 17,000 presets by name across 33 plugins, from CLAP preset discovery, VST3 program lists,
  NKS, DX7 cartridges and plugins' own preset files (Serum 2, Odin2, u-he, Synplant, Cherry Audio,
  Guitar Rig, Microtonic, Analog Lab, AAS, ...); parameters by value or by display text.
- `wavelength audition` indexes how every preset sounds (octave offset, brightness, envelope, width)
  and `wavelength analyze` measures renders (pitch, spectrum, stereo, onsets, envelope).
- `builtin:sampler` (Bitwig multisamples, drum kits, loops), `builtin:audio` clips with
  pitch-preserving stretch, and built-in drums and FX.
- A mix engine: effect chains, sends, group buses, sidechain audio, LFOs and step automation,
  tempo ramps, groove, MIDI CC and pitch bend, 20+ built-in effects, true-peak limiting, mastering
  to a loudness target, per-track and per-section loudness in the report.

### Added
- Headless CLAP host: loads installed `.clap` bundles, follows CLAP threading (main thread for lifecycle/state, a dedicated audio thread for `process()`), pumps the macOS run loop and `on_main_thread` requests while rendering.
- `wavelength plugins`: scans the CLAP folders with a per-bundle cache; lists built-in instruments too.
- `wavelength params`: parameter names, ranges, values and display text, optionally after loading a preset.
- `wavelength render`: JSON jobs with tempo maps, notes in beats or seconds, per-track presets (`.clap-preset`, Vital `.vital`, raw) and parameter overrides; writes stems, `mix.wav` and `report.json`.
- `wavelength state save`: builds Bitwig-compatible `.clap-preset` files from a starting state plus parameter changes.
- Mixing: per-track effect chains, post-fader sends, buses, a master chain, fader and parameter automation, section markers.
- Built-in effects: gain, eq, filter, delay, reverb (8-line FDN), compressor, limiter, saturate, chorus, width, duck (sidechain keyed from another track's notes). CLAP effect plugins work in any chain.
- Built-in instruments: `builtin:drums` (General MIDI kit) and `builtin:fx` (impact, riser, reverse swell, sub drop).
- Loudness: BS.1770 / EBU R128 integrated LUFS per track, bus, section and mix (matches `ffmpeg -af ebur128`).
- Agent ergonomics: `--json` on every command with actionable errors; plugin stdout is diverted to stderr so reports stay parseable.
- Docs: `AGENTS.md` (operating guide and mixing playbook), `docs/job-format.md`, `docs/effects.md`.
- `scripts/check.sh` regression check; examples `hello.json` and `effects-tour.json`.
- VST3 hosting (Steinberg VST3 SDK 3.8, MIT): instruments and effects render through the same jobs, effects chains, automation and reports as CLAP plugins. Plugins resolve by name, id or bundle path; `vst3:` / `clap:` prefixes pick a format when a plugin ships as both.
- VST3 scanning runs each unknown bundle in a child process with a timeout, so a plugin that crashes or hangs while loading can't take the scan down; results (including failures such as Intel-only bundles) are cached.
- State formats `vstpreset` (VST3 preset files, as found inside Bitwig DAWprojects) and `nksf` (Native Instruments NKS presets, which carry the plugin's own state; this is how BBC Symphony Orchestra instruments load headlessly).
- Per-track and per-effect `warmup` for plugins that stream samples after activation (orchestral libraries).
- `state save` writes `.vstpreset` files for VST3 plugins.
- CLAP preset discovery: `wavelength presets <plugin> [--search text]` lists a plugin's factory banks and preset files, and `"preset": "<name>"` on a track or effect (or `--preset` on `params`/`state save`) loads one by name through the plugin's preset-load extension. This opens the full factory libraries of plugins like Altitude (450 presets), Apricot, Regency, ExtraBold and Fluctus.
- `builtin:sampler`: plays Bitwig `.multisample` instruments (key/velocity zones, velocity crossfades, round robins, select ranges, sustain loops with crossfade, key tracking, reverse), WAV drum kit folders auto-mapped to General MIDI from file names (with overrides and hat choke), and single WAVs. `wavelength samples [--search] [--kit]` lists libraries from `$WAVELENGTH_SAMPLES_PATH` and Bitwig Studio's installed packages (Legend 707/808/909, Grand Piano, Rare Organs, electric guitar, basses, ...).
- Automation: `"step"` points and a `curve` setting; LFOs (tempo-synced note values or Hz, six shapes, depth curves) on any built-in effect setting, plugin parameter, fader, pan or send; track `pan` automation; automated sends; bus and master gain automation (fades).
- Routing: `"output"` on tracks and buses for group buses and bus chains, processed in dependency order.
- Feel: job/track `groove` (swing, lay-back, humanize with a seed), `roll` (strummed chords), track `transpose`, tempo ramps (`"ramp": true` tempo points).
- MIDI controllers: `automation.cc`, `automation.pitchbend` (semitones via `bendRange`) and `automation.pressure`; CLAP plugins get MIDI or note expressions, VST3 plugins the parameters they map through IMidiMapping.
- New built-in effects: `tremolo` (and autopan), `pan`, `gate` (step pattern or note-keyed), `rotary` (Leslie with rotor inertia), `autowah`, `bitcrush`, `vibrato`, `tapestop`. `duck` depth is automatable.
- Limiter true-peak detection (4x oversampled, on by default); the report gives `mix.truePeakDb`, per-track `renderSeconds` and per-track `sectionLufs`.
- Sampler: `mono` + `glide` (legato slides), per-note `bend`, kit map entries with `gain`/`pan`/`tune`, `retrigger`, `variants` (takes as round robins), `bpm` (tempo-synced loops), `slices`, `start`, `reverse`; toms map by number, loops no longer land on drum keys, `samples` lists loop folders, ambiguous kit names are an error that lists the choices.
- State: `fxp` format (VST2 `.fxp`/`.fxb` chunks: Surge XT and OB-Xf factory patches load directly); VST3 factory program lists in `presets` and `"preset"` (Dexed); a warning when a state or preset changes no parameters, and when an effect plugin only changes the level.
- Stems: `"stems": "float" | "24" | "16" | "none"` (and `render --stems`); a free-space check before rendering.
- Preset formats: Serum 2 `.SerumPreset` (zstd + CBOR, split into processor and controller state), JUCE ValueTree binaries such as Odin2 `.odin`, and u-he `.h2p` (Zebra2, Zebralette, TripleCheese) load as `state` and are auto-detected.
- Presets by name from preset folders: `wavelength presets` and `"preset"` also search the plugin's preset folders (`/Library/Audio/Presets/<Vendor>/<Plugin>`, Serum 2, Odin2, u-he, Surge XT, OB-Xf), about 5,800 presets across these plugins; placeholder VST3 program lists ("Prog 1") are hidden.
- NKS preset index: `.nksf` files in the usual NKS folders are indexed (cached in `~/Library/Caches/wavelength/nks.json`, rebuilt with `presets --rescan` or when a name isn't found) and matched to their plugin, so `"preset"` reaches DUNE 3's 1,071 presets and BBC Symphony Orchestra's instruments by name; NKS chunks of wrapped VST2 plugins get the length prefix their VST3 state needs.
- DX7 cartridges: every voice of Dexed's `.syx` cartridges is a preset by name (1,056 voices installed), or `"state": "<cart>.syx#<voice>"`; the voice is written into Dexed's own state (edit buffer and cartridge).
- Synplant: `.synplant` patches (355 unique, 12 categories) load by name into program slot 0 of Synplant's own state, with their tuning, release and volume.
- Cherry Audio: DCO-106, Surrealistic MG-1 Plus, Synthesizer Expander Module and Voltage Modular presets (737) load by name, merged into each plugin's own state (JUCE ValueTree, byte-exact); Voltage Modular gets a 5 s default warmup (it loads patches asynchronously), BBC Symphony Orchestra 6 s.
- SynthMaster One and Player: their NKS presets (about 3,400) load by name; each file is assigned by its payload (the Player's folder holds One presets too).
- Guitar Rig 6: its 1,272 rack presets (`.ngrr`) load by name in an effect chain, built without a template; the listing gives each rack's category and whether the free edition can play it (52 can), and a rack using paid components gets a warning naming them.
- Parameter values as display text: `"params": {"Cutoff": "800 Hz", "Rate": "1/16"}` is parsed by the plugin itself (VST3 getParamValueByString, CLAP text_to_value), in jobs and effect chains.
- Microtonic: 115 kits (`.mtpreset`) and 430 drums (`.mtdrum`) load by name; a kit waits for notes 36-43 (its pattern player and mutes off; set `"PlayStop": "PlayNow"` to let its patterns play), a drum goes onto a channel with `"preset": "AC BD Back#3"`. Values are read by Microtonic's own text parser, so they match the presets exactly.
- `scripts/extract-embedded-presets.py`: extracts factory presets compiled into JUCE plugin binaries (no files on disk) into `~/Library/Application Support/Wavelength/Presets/<Plugin>/`, which `presets` and `"preset"` search: TAL-NoiseMaker (256), Relica 2 (35), Thump One (164), Wavetable (99), Flux Mini 2 (22). Listings show a preset once when a file and a program share its name.
- Analog Lab V (254 Analog Lab presets, 15 s default warmup), AAS Player (673 programs: Strum guitars, Lounge Lizard, Chromaphone, Ultra Analog, String Studio), MPowerSynth (1,578 presets from its bank), Soundbox (45) and DecentSampler (`.dspreset` files, samples resolved next to the preset; 3 s warmup) load by name or as `state`.
- Preset listings drop copies of the same preset filed in several folders (All / By Category / By Creator).
- `getState` on plugins and state formats that patch the plugin's current state (templates), used by `dx7`.
- `wavelength params` hides JUCE's MIDI CC placeholder parameters (thousands per plugin) unless `--all`.
- Sidechain: `"sidechain": "<track>"` on a plugin effect feeds that track's audio (after its effects, before its fader) into the plugin's sidechain input (CLAP second input port, VST3 aux bus), and on the built-in `compressor` keys its detector. Source tracks render first; worker processes get their audio as files; sidechain loops are rejected. Many plugins also need their own sidechain switch turned on (MTurboComp "Side-chain input (Detector)").
- `builtin:audio`: audio clips on the timeline. Each clip is a WAV placed by `beat` or by `endAt` (it ends exactly on that beat: reverse swells), fitted to the song tempo from its own `bpm` with pitch-preserving stretch (Signalsmith Stretch, MIT) or tape-style (`"stretch": false`), transposed with `pitch`, trimmed (`start`, `length` or `beats`), reversed, with gain and fades. Example `examples/clips-tour.json`.
- Plugin tracks render in worker processes, several at once (`render --jobs N` or `"parallel"` in the job; default half the cores, up to 4; 0 = everything in one process). A track whose plugin crashes or hangs is left out of the mix and listed in the report's `failedTracks` instead of failing the song; job mistakes (unknown preset, bad state) still fail the render. Built-in instruments stay in-process; tracks are mixed as they finish, so memory stays flat.
- `audition --retag` recomputes every index's tags from stored measurements; a new audition run retries earlier failures and drops presets that are no longer listed. Serum 1 `.fxp` patches in Serum 2's folders are no longer listed for Serum 2.
- `wavelength audition <plugin> [--jobs N] [--limit N] [--rebuild]`: renders every preset once (C4 for 1 s) in worker processes, each keeping one plugin instance, and indexes how it sounds in `~/Library/Caches/wavelength/audition/`: octave offset and cents, loudness, brightness, band balance, attack, decay, sustain, release, width, onsets, and whether it plays by itself. A preset that crashes or hangs its worker is recorded and skipped. `presets` then shows tags (octave -1, dark, warm, bright, sub, bassy, pluck, slow attack, rhythmic, long release, wide, self-playing, silent, ...) that `--search` finds, and `--json` includes the measurements.
- `wavelength analyze <file.wav | render-dir> [--start] [--end]`: pitch (YIN: note, cents, confidence), spectral centroid, rolloff and band balance (sub to air), stereo width and correlation, onsets, and envelope (attack, decay, sustain, active time). A render folder analyzes its mix, every stem and every marker section. Agents no longer need their own FFT scripts.
- `scripts/stage-gains.py <song>`: sets each track's fader from the last render's stem LUFS and the song's `targets.json`.
- Orchestral writing: per-note articulations (`"art": "spiccato"` with a track `articulations` map of keyswitch keys) insert the keyswitch just before each change; track `range` warns about notes an instrument can't play; `velocityTo` turns note velocities into a controller curve (`{"param": "Dynamics"}` or `{"cc": 1}`) for libraries whose long notes ignore velocity.
- `multiband` effect: 2-4 bands on Linkwitz-Riley crossovers that sum back flat, each band with its own effect chain, gain, solo and mute (multiband compression, band saturation).
- `master.loudness`: a target in LUFS; the gain into the master chain is found in a few passes so the mastered mix lands on it (`mix.loudnessGainDb` in the report).
- `wavelength master <mix.wav> --chain <chain | job>`: masters a finished mix without re-rendering, with loudness before and after per section; the chain can be a file or inline JSON.
- Personal job defaults: `~/Library/Application Support/Wavelength/defaults.json` (or `$WAVELENGTH_DEFAULTS`) holds top-level job settings, e.g. `{"leadIn": 1}`, that every job gets unless it sets them itself; the report lists them in `defaultsApplied`. Jobs Wavelength builds internally (`wavelength master`) ignore them.
- `leadIn`: seconds of silence before the song in the mix and stems (streaming uploads), and `--lead-in` on `wavelength master`.

### Changed
- Automation points at the same beat keep the order they are written in (`[16, 0], [16, 1]` jumps from 0 to 1); they used to be sorted by value.
- The limiter detects true (inter-sample) peaks by default; mixes limited at the same ceiling come out a few tenths of a dB quieter.
- Kit auto-mapping groups takes of one sound and maps toms by number, so some keys of auto-mapped kits play different files than before (explicit `map` entries are unaffected).

### Fixed
- An option given without its value (`analyze out --end --json`) is an error instead of swallowing the next option.
- Plugin delay compensation: instruments and effects that report latency (lookahead limiters, linear-phase EQs, LA-2A, ...) are rendered that many samples longer and shifted back into place, so their tracks no longer land late against the mix. The report gives `tracks[].latencyCompensatedMs`.
- A failed render no longer leaves the previous render's `mix.wav`, `report.json` and stems behind; it writes `{"ok": false}` to `report.json`.
- `params` set on top of a `state` stick on plugins that apply state late (Surge XT); `params` inspection processes a few blocks first so such plugins show real values.
- Clamp warnings no longer fire for in-range values (float precision) and show the range.
- Limiter warning reads as a positive gain-reduction figure with the time it happened.
- The CLI exits without running plugin static destructors, which crashed some plugins at shutdown after the work was already written.
