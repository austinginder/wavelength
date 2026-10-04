---
name: wavelength
description: Compose, arrange, mix and master original music with Wavelength, the headless music engine for AI agents. It plays notes through the CLAP, VST3 and VST2 instruments and sample libraries installed on this computer (macOS, Linux or Windows), or through its built-in synth, and returns a mastered mix plus measurements. Installs or builds the engine when it is missing. Use when the user asks to make a song, track, theme, score, soundtrack, jingle or beat, or to render music with their plugins.
---

# Wavelength

Wavelength renders music offline through the CLAP, VST3 and VST2 instruments and sample libraries installed on this machine (Vital, Serum 2, Surge XT, Dexed, BBC Symphony Orchestra, ...), or through its built-in synth when there are none. You write a JSON job (notes, sounds, effects, mix); it returns stems, a mix and a report with loudness per track and section. You cannot hear the result, so you measure it, and you ask the human to listen.

## 1. Get the engine

Run the installer that ships with this skill. It uses an installed `wavelength` if there is one, otherwise downloads the release build for this system (macOS, Linux x86_64/arm64, or Windows x86_64 from Git Bash), otherwise clones https://github.com/austinginder/wavelength and compiles it (needs CMake and the Xcode command line tools on macOS, or g++/clang on Linux; it says so if they are missing).

```sh
bash <this skill's folder>/scripts/install.sh      # add --update to refresh, --source to force a build
```

It prints `WAVELENGTH=` (the binary), `WAVELENGTH_DOCS=` (docs matching that binary's version) and `WAVELENGTH_SCRIPTS=`. Use those paths below. Check `"$WAVELENGTH" version` works. The docs describe exactly what this version can do. If a step below needs a command or setting the binary doesn't know, run the installer with `--update` (newest release) or `--source` (latest main), or do that step the way the docs you have describe.

**Before writing anything, read `$WAVELENGTH_DOCS/AGENTS.md` in full** (engines from 0.4.0 carry the same docs inside: `"$WAVELENGTH" docs agents`, `docs job-format`, `docs effects`, `docs song-format`, `--section` for one part). It is the operating guide: choosing sounds, the mixing playbook, orchestral libraries, reading the report, mastering. `docs/job-format.md` and `docs/effects.md` are the reference; `examples/` has jobs that render.

## 2. Plan the piece

Settle, from the request (ask only if it is genuinely unclear):
- **Style and arc:** what the song should sound like at the start, the middle and the end.
- **Key, tempo, form:** sections in bars with a loudness contour (quiet intro, lifts, drops, coda).
- **Material:** a theme (melody over chords) you can vary, and the parts that carry it in each section. A strong, simple theme heard in different costumes carries a whole piece.

Write the plan into the song folder's generator as comments; it doubles as documentation.

If the human hands you material, start from it: `"$WAVELENGTH" import score.mxl` (MusicXML from MuseScore, Sibelius, Dorico; repeats played out, dynamics as velocities), `import part.mid`, or `import song.dawproject` (a DAW's tracks, plugins and mixer), or a Bitwig project as it is (`import ~/Documents/Bitwig\ Studio/Projects/<name>/<name>.bwproject`, no export needed) each write a `job.json` that renders at once. Without Bitwig's sound content, run `"$WAVELENGTH" samples --install-soundfont` once so imported parts get General MIDI sounds.

## 3. Choose sounds from what is installed

Never guess plugin or preset names. List them:

```sh
"$WAVELENGTH" plugins --json
"$WAVELENGTH" presets <plugin> --search <text>      # factory presets by name
"$WAVELENGTH" audition <plugin>                     # once per plugin: tags like "octave -1", dark, pluck
"$WAVELENGTH" card <plugin> "<A>" "<B>" + <plugin2> "<C>"   # cards.png: a contact sheet of candidates; read it
"$WAVELENGTH" samples --search <text>               # sample libraries, SFZ, SoundFonts, drum kits (builtin:sampler)
"$WAVELENGTH" loops --search <text>                 # macOS: GarageBand's Apple Loops by key and tempo (builtin:audio clips)
"$WAVELENGTH" presets builtin:synth                 # the built-in synth's patches: always there, no plugin needed
"$WAVELENGTH" presets arp                           # macOS: GarageBand's Arpeggiator presets and patches, for a track's "arp"
```

On a machine with few or no plugins (a cloud container, CI), build the song from `builtin:synth` patches, `builtin:drums` and `builtin:fx`; `samples --install-soundfont` adds General MIDI sounds, and `"$WAVELENGTH" kit install` adds free synthesizers with their factory patches (Surge XT, OB-Xf, Dexed) into Wavelength's own folder. On Linux, install the system libraries it names for any plugin that won't load.

Prefer factory presets by name. Transpose presets tagged `octave -1` (or flagged "C4 sounds C3" on a card). **Check every melodic preset over the range you will write for it** before committing (AGENTS.md explains how): presets voiced for the mod wheel sound dull and fade on high notes, and aggressive ones hide distortion and OTT compression. When the choice is a matter of taste, render short candidates playing the same phrase and let the human pick.

## 4. Write the song

Songs live in their own folder, never inside the engine checkout: `~/wavelength-songs/<slug>/` unless the user keeps music somewhere else.

- `make-job.py` builds `job.json`: the theme and chords as data, helper functions that voice and vary them, one list of notes per track, sounds, effects, sends, buses, markers and rides. Generating notes in code keeps variations consistent and edits cheap.
- `targets.json` (optional): target LUFS for tracks whose role targets don't fit (`wavelength stage` picks a target from each track's name: Kick -12, Bass -15.5, Lead -16, Pad -22, Arp -21 ...).
- `gains.json`: faders from measurement (step 5).
- Set `"stems": "none"` (or `"16"`) in the job unless you need stem files: float stems are big.

## 5. Render, measure, adjust

```sh
python3 make-job.py
"$WAVELENGTH" lint job.json --harmony --json            # wrong notes and key clashes, before rendering
"$WAVELENGTH" stage job.json                          # renders once; faders = target - stem LUFS by role or targets.json -> gains.json
python3 make-job.py && "$WAVELENGTH" render job.json --out out --png --json   # out/song.png: look at it
"$WAVELENGTH" analyze out --json                      # pitch, brightness, bands, width per stem and section
```

Look at `out/song.png` (the report names it): the sections, the loudness contour with each section's level, the spectrum, and every track's notes over its level. It shows at a glance what the numbers only list: a part playing where it should rest, a flat contour, a harsh band. `"$WAVELENGTH" picture job.json` draws the arrangement before any render.

Read the picture of the render **with its master** (the master's compressors flatten contrast the pre-master still had), in this order:
- **Loudness line:** it should draw the form you planned. A flat line from the first drop to the end is a flat song; a breakdown that dips under 4-5 dB doesn't breathe. Ride tracks or buses (`automation.rides`, before the master) rather than the master, which hands most of it back.
- **Drop marks** at the section boundaries: the jump from the last 2 bars before it to the first 4 after it. Green is 3 dB or more, amber passes but under 3, red is a weak drop: empty the build (see AGENTS.md "Arranging"), don't just turn it down.
- **Spectrum:** high-pass sweeps show as rising dark edges in the builds; low end that comes back in the last bar before a drop (risers, reverse swells, a full-range roll) eats the drop. A bright band that never moves is a harsh or resonant sound; an empty top is dull.
- **Lanes:** notes over each track's level after its fader (the label is `tracks[].postFaderLufs`, before any bus). A lane lit where it should rest, or a part far louder or quieter than its role.

Read `report.json`: no silent tracks, no warnings you can't explain, `sections[].lufs` following the contour you planned, `tracks[].sectionLufs` to find what dominates a section. Ride faders with `automation.rides` until the sections breathe. Keep iterating while the numbers disagree with the plan. To check one passage, `render job.json --from 41 --to 45` renders just those bars (with the song's buses and master) in seconds.

## 6. Master

Render a pre-master (the master chain reduced to a high-pass), then master it, as described in AGENTS.md: EQ, a `multiband` with a compressor per band, a limiter, a true-peak `limiter` at -1 dB, `"loudness": -14`, and `"leadIn": 1` (a second of silence for streaming platforms).

```sh
"$WAVELENGTH" master out/mix.wav --chain mastering.json --lead-in 1 --out mastered --deliver mp3,flac
```

`--deliver` (or `"deliver": ["mp3"]` in the job) writes `mix.mp3`/`mix.flac` next to `mix.wav`, decodes them again and reports each file's true peak: an MP3 over -1 dBTP warns with how far to lower the limiter. Once the chain is right, make it the job's own `master` so every render comes out mastered.

## 7. Hand it over, listen, fix

Give the human the MP3 path and a short description: the form with timestamps, the instruments per section, the loudness. For a listening session, `"$WAVELENGTH" serve <songs folder>` opens a local page with the arrangement, stems and quick previews where they pin comments to bars and tracks. `"$WAVELENGTH" comments <song>` lists the open ones with the revision each was made on and whether the music there changed since; answer with `comments <song> --reply <id> --text "..." --done`. Ask them to listen and name anything that sounds wrong with a time. For a named moment, follow "When the human names a moment" in AGENTS.md: find the bar, render that stretch with stems, find the stem that moves, test the instrument alone, fix, re-render. Compare versions only at equal loudness.

If the human wants to finish the song in their DAW, `"$WAVELENGTH" export job.json --out
<slug>.dawproject` opens in Bitwig with the same plugins and sounds; tell them what the export
listed as left out.

Finish with a `NOTES.md` in the song folder: the prompt, the description, and a table of every track with its plugin, preset or patch, and role. It is how the song gets credited and rebuilt.

Keep the song as a song (`"$WAVELENGTH" docs song-format`): `"$WAVELENGTH" save <song> -m "..."` at each milestone (the first save writes `wavelength.json`; fill in its title, summary and licence), `undo`/`restore` when the human prefers an earlier version, `fallbacks <song> --suggest --write` so it plays on machines without your plugins, `render job.json --png --keep` for the render that goes with it, and `pack <song>` for one `.wavelength` file to share.

## Rules

- Only use plugins, presets and sample libraries that the listings show.
- Keep songs out of the engine's repository.
- Check free disk space before long renders; delete trial render folders when done.
- A failed render leaves `{"ok": false, "error": ...}`: fix the named track, parameter or file.
