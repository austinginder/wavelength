---
name: wavelength
description: Compose, arrange, mix and master original music with Wavelength, the headless music engine for AI agents. It plays notes through the real CLAP and VST3 synths and sample libraries installed on this computer (macOS, Linux or Windows) and returns a mastered mix plus measurements. Installs or builds the engine when it is missing. Use when the user asks to make a song, track, theme, score, soundtrack, jingle or beat, or to render music with their plugins.
---

# Wavelength

Wavelength renders music offline through the synthesizers installed on this machine (Vital,
Serum 2, Surge XT, Dexed, BBC Symphony Orchestra, ...). You write a JSON job (notes, sounds,
effects, mix); it returns stems, a mix and a report with loudness per track and section. You
cannot hear the result, so you measure it, and you ask the human to listen.

## 1. Get the engine

Run the installer that ships with this skill. It uses an installed `wavelength` if there is one,
otherwise downloads the release build for this system (macOS, Linux x86_64/arm64, or Windows
x86_64 from Git Bash), otherwise clones https://github.com/austinginder/wavelength and compiles
it (needs CMake and the Xcode command line tools on macOS, or g++/clang on Linux; it says so if
they are missing).

```sh
bash <this skill's folder>/scripts/install.sh      # add --update to refresh, --source to force a build
```

It prints `WAVELENGTH=` (the binary), `WAVELENGTH_DOCS=` (docs matching that binary's version)
and `WAVELENGTH_SCRIPTS=`. Use those paths below. Check `"$WAVELENGTH" version` works. The
docs describe exactly what this version can do. If a step below needs a command or setting
the binary doesn't know, run the installer with `--update` (newest release) or `--source`
(latest main), or do that step the way the docs you have describe.

**Before writing anything, read `$WAVELENGTH_DOCS/AGENTS.md` in full.** It is the operating
guide: choosing sounds, the mixing playbook, orchestral libraries, reading the report,
mastering. `docs/job-format.md` and `docs/effects.md` are the reference; `examples/` has jobs
that render.

## 2. Plan the piece

Settle, from the request (ask only if it is genuinely unclear):
- **Style and arc:** what the song should sound like at the start, the middle and the end.
- **Key, tempo, form:** sections in bars with a loudness contour (quiet intro, lifts, drops, coda).
- **Material:** a theme (melody over chords) you can vary, and the parts that carry it in each
  section. A strong, simple theme heard in different costumes carries a whole piece.

Write the plan into the song folder's generator as comments; it doubles as documentation.

If the human hands you material, start from it: `"$WAVELENGTH" import score.mxl` (MusicXML from
MuseScore, Sibelius, Dorico; repeats played out, dynamics as velocities), `import part.mid`, or
`import song.dawproject` (a DAW's tracks, plugins and mixer) each write a `job.json` that renders at
once. Without Bitwig's sound content, run `"$WAVELENGTH" samples --install-soundfont` once so
imported parts get General MIDI sounds.

## 3. Choose sounds from what is installed

Never guess plugin or preset names. List them:

```sh
"$WAVELENGTH" plugins --json
"$WAVELENGTH" presets <plugin> --search <text>      # factory presets by name
"$WAVELENGTH" audition <plugin>                     # once per plugin: tags like "octave -1", dark, pluck
"$WAVELENGTH" samples --search <text>               # sample libraries, SFZ, SoundFonts, drum kits (builtin:sampler)
```

Prefer factory presets by name. Transpose presets tagged `octave -1`. **Check every melodic
preset over the range you will write for it** before committing (AGENTS.md explains how):
presets voiced for the mod wheel sound dull and fade on high notes, and aggressive ones hide
distortion and OTT compression. When the choice is a matter of taste, render short candidates
playing the same phrase and let the human pick.

## 4. Write the song

Songs live in their own folder, never inside the engine checkout: `~/wavelength-songs/<slug>/`
unless the user keeps music somewhere else.

- `make-job.py` builds `job.json`: the theme and chords as data, helper functions that voice
  and vary them, one list of notes per track, sounds, effects, sends, buses, markers and rides.
  Generating notes in code keeps variations consistent and edits cheap.
- `targets.json`: target LUFS per track (leads -18, brass -17, drums -15, pads -23, arps -23).
- `gains.json`: faders from measurement (step 5).
- Set `"stems": "none"` (or `"16"`) in the job unless you need stem files: float stems are big.

## 5. Render, measure, adjust

```sh
python3 make-job.py
"$WAVELENGTH" lint job.json --harmony --json            # wrong notes and key clashes, before rendering
"$WAVELENGTH" render job.json --out out --json
python3 "$WAVELENGTH_SCRIPTS/stage-gains.py" .        # faders = target - stem LUFS
python3 make-job.py && "$WAVELENGTH" render job.json --out out --json
"$WAVELENGTH" analyze out --json                      # pitch, brightness, bands, width per stem and section
```

Read `report.json`: no silent tracks, no warnings you can't explain, `sections[].lufs`
following the contour you planned, `tracks[].sectionLufs` to find what dominates a section.
Ride faders with `automation.rides` until the sections breathe. Keep iterating while the numbers
disagree with the plan. To check one passage, `render job.json --from 41 --to 45` renders just
those bars (with the song's buses and master) in seconds.

## 6. Master

Render a pre-master (the master chain reduced to a high-pass), then master it, as described in
AGENTS.md: EQ, a `multiband` with a compressor per band, a limiter, a true-peak `limiter` at
-1 dB, `"loudness": -14`, and `"leadIn": 1` (a second of silence for streaming platforms).

```sh
"$WAVELENGTH" master out/mix.wav --chain mastering.json --lead-in 1 --out mastered --deliver mp3,flac
```

`--deliver` (or `"deliver": ["mp3"]` in the job) writes `mix.mp3`/`mix.flac` next to `mix.wav`,
decodes them again and reports each file's true peak: an MP3 over -1 dBTP warns with how far to
lower the limiter. Once the chain is right, make it the job's own `master` so every render comes
out mastered.

## 7. Hand it over, listen, fix

Give the human the MP3 path and a short description: the form with timestamps, the instruments
per section, the loudness. For a listening session, `"$WAVELENGTH" serve <songs folder>` opens a
local page with the arrangement, stems and quick previews where they pin comments to bars and
tracks; read them from the song's `review.json` and answer there. Ask them to listen and name anything that sounds wrong with a time.
For a named moment, follow "When the human names a moment" in AGENTS.md: find the bar, render
that stretch with stems, find the stem that moves, test the instrument alone, fix, re-render.
Compare versions only at equal loudness.

If the human wants to finish the song in their DAW, `"$WAVELENGTH" export job.json --out
<slug>.dawproject` opens in Bitwig with the same plugins and sounds; tell them what the export
listed as left out.

Finish with a `NOTES.md` in the song folder: the prompt, the description, and a table of every
track with its plugin, preset or patch, and role. It is how the song gets credited and rebuilt.

## Rules

- Only use plugins, presets and sample libraries that the listings show.
- Keep songs out of the engine's repository.
- Check free disk space before long renders; delete trial render folders when done.
- A failed render leaves `{"ok": false, "error": ...}`: fix the named track, parameter or file.
