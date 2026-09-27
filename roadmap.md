# Roadmap

The goal: Wavelength is the tool an AI agent reaches for to make music. Editable notes go in, real instruments play them, and measured results come back that an agent can act on without hearing.

This file tracks what's planned, what's missing and why. `changelog.md` records what shipped; "Known limits" in `AGENTS.md` lists what doesn't work today.

Status: **released** in the version named, **0.5.0** (on main, not released yet), **next**, **later**, or **idea** (not decided).

## 1. The agent sees what it can't hear

An agent has no ears. Everything it learns about a render has to come back as numbers, text or a picture it can read.

| | Status | Notes |
|---|---|---|
| Loudness per track, bus, section and mix; true peak | released 0.1.0 | BS.1770, matches ffmpeg |
| Harmony and voice-leading lint | released 0.3.0 | `lint --harmony`, parallel fifths and octaves |
| The song picture (`render --png`) | released 0.4.0 | sections, loudness, spectrogram, notes per track |
| Mix checks that name a problem and its fix | released 0.4.0 | kick room, wide low end, phase, repetition |
| Compare with a reference track (`analyze --reference`) | next | "make it sound like this" as numbers: balance, loudness over time, width, density |
| Per-note pitch and level tracking in `analyze` | later | |
| Gain reduction readouts for compressors and limiters | later | |
| Parameter sweeps: which ranges of a knob sound musical | later | models choose effects well and set them badly |
| Auditions inside each instrument's range; envelope depth for rhythmic patches | later | |
| A written critique from an audio model | idea | judges arrangement and feel; the report judges fidelity |

## 2. It works on any computer

Cloud agents start in an empty Linux container. A song has to render there, and on a laptop without the plugins it was made with.

| | Status | Notes |
|---|---|---|
| macOS, Linux and Windows builds | released 0.2.0 | |
| VST2 hosting, Intel-only plugins under Rosetta | released 0.3.0 | |
| `builtin:synth`, 27 patches | released 0.4.0 | |
| `wavelength kit`: Surge XT, OB-Xf, Dexed, a General MIDI SoundFont | released 0.4.0 | |
| Track fallbacks, `fallbacks --suggest`, `lib:` library files | released 0.4.0 | |
| A Docker image with the kit installed | next | |
| Installs through package managers (Homebrew, npm or pip) | next | agents reach for these before a tarball |
| Odin2 and free sample sets in the kit (piano, orchestra, drums) | later | |
| A notarized macOS binary | later | needs a Developer ID certificate |
| Audio Unit hosting | 0.5.0 | macOS: instruments, effects and MIDI-controlled effects, factory presets, `.aupreset` state; Intel-only units open out of process |
| Preset capture (`wavelength capture <plugin>`): open a plugin's own window and save each preset loaded there as a named state | idea | for presets that only load through the plugin's window. Kontakt library instruments need a state saved by a host: an `.nki` or a saved `.nkm` multi can't become one without decrypting it, and Komplete Kontrol restores the Kontakt state it embeds, not the preset path it records. Stepping through presets in Komplete Kontrol's window would capture a whole library, saved for Komplete Kontrol and for Kontakt. Needs plugin editor hosting, VST3 on macOS first |
| SoundFont and SFZ LFOs (vibrato, tremolo) and pitch envelopes | later | |

## 3. Agents find it

| | Status | Notes |
|---|---|---|
| The `/wavelength` agent skill | released 0.2.0 | |
| `wavelength mcp`, docs built into the binary, the Claude Code plugin | released 0.4.0 | |
| `llms.txt` on wavelength.run | done | lists the docs of the latest release |
| Listings: the official Claude Code plugin marketplace, MCP Registry, Glama, Smithery, skills.sh | next | after 0.4.0, which has the MCP server |
| GitHub topics; docs indexed by Context7 | next | |

## 4. Songs people keep and share

| | Status | Notes |
|---|---|---|
| The song format: revisions, undo, musical diff, comments pinned to revisions, `.wavelength` packages | released 0.4.0 | spec Draft 2, format 1.0 |
| Downloadable songs on wavelength.run | done | 5 songs, CC BY 4.0 |
| The spec as final 1.0, after feedback | later | |
| A revision content hash, so two copies of a song can be merged | later | from the spec review: revision numbers are local |
| Comment anchors that survive a track getting an explicit id | later | from the spec review |
| ZIP64, for songs over 4 GiB | later | 1.0 packages stay under 4 GiB |
| Exact Unicode NFC and case folding in the engine | later | it covers Latin, Greek and Cyrillic; the Python reader is exact |
| A public song gallery: every song's sources, in its own repository | later | models learn a tool from its examples |
| History in the review page: see revisions, undo from the page | idea | |

## 5. Music a text-to-song model can't make

| | Status | Notes |
|---|---|---|
| DAWproject export (Bitwig, Studio One, Cubase) | released 0.3.0 | a hand-off to a person |
| MIDI, MusicXML and DAWproject import | released 0.3.0 | |
| Bitwig projects without an export | released 0.4.0 | |
| Game loops (`render --loop`) | released 0.4.0 | sample-exact, with smpl loop chunks |
| Scoring to picture: a tempo map from a list of hit times | next | downbeats land on the cuts |
| Adaptive music for games: layers and stingers exported for Godot and Unity | later | |
| Vocals: generated vocal stems brought in as clips | later | the biggest gap against Suno |
| DAWproject import: plugin effect automation, launcher clips, more Bitwig devices; Bitwig project audio clips and Bitwig 6 automation clips | later | |
| Live playback through the speakers | later | the review page renders previews today |

## 6. Measuring progress

| | Status | Notes |
|---|---|---|
| A benchmark for agents making music, judged on the rendered audio | later | tasks, automatic checks from the report, people's votes |
