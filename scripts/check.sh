#!/usr/bin/env bash
# Build, then render every example and fail on errors, silent tracks, or a clipping mix.
# Usage: scripts/check.sh            (from the repo root)
#        BUILD=build-dev scripts/check.sh   (a second build tree, while agents render with build/)
set -euo pipefail
cd "$(dirname "$0")/.."
build="${BUILD:-build}"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" -j "${JOBS:-8}" 2>&1 | grep -E "error:|warning:" && { echo "build has errors or warnings"; exit 1; } || true
fail=0
for job in examples/*.json; do
  name=$(basename "$job" .json)
  out="out/check/$name"
  if ! report=$("./$build/wavelength" render "$job" --out "$out/$build" --json 2>/dev/null); then
    echo "FAIL $name: $(echo "$report" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("error"))' 2>/dev/null)"; fail=1; continue
  fi
  echo "$report" | python3 scripts/check_report.py "$name" || fail=1
  # MIDI round trip: export, import, export again; the two files must match byte for byte
  if python3 -c 'import json,sys; sys.exit(0 if any(t.get("notes") for t in json.load(open(sys.argv[1]))["tracks"]) else 1)' "$job"; then
    mid="$out/$build/midi"
    mkdir -p "$mid"
    if ! { "./$build/wavelength" export "$job" --out "$mid/a.mid" > /dev/null &&
           "./$build/wavelength" import "$mid/a.mid" --out "$mid/in" --instrument "Surge XT" > /dev/null &&
           "./$build/wavelength" export "$mid/in/job.json" --out "$mid/in/a.mid" > /dev/null &&
           cmp -s "$mid/a.mid" "$mid/in/a.mid"; }; then
      echo "FAIL $name: MIDI export/import round trip differs"; fail=1
    fi
  fi
done
# harmony lint: the example's one-bar Eb (bar 10) must be flagged; its declared key change (bar 13) and
# secondary dominant (bar 19) must not be; the snare roll (unpitched one-shot) and the "harmony": false ping are skipped
if ! "./$build/wavelength" lint examples/harmony-tour.json --harmony --json | python3 -c '
import json, sys
j = json.load(sys.stdin)
p = [(x["kind"], x["bars"]) for x in j["problems"]]
i = [(x["kind"], x["bars"]) for x in j["info"]]
s = sorted(x["track"] for x in j["skipped"])
sys.exit(0 if p == [("key excursion", [10, 10])] and i == [("secondary dominant", [19, 19])] and s == ["Snare Roll", "Tritone Ping"] else 1)'; then
  echo "FAIL harmony-tour: lint --harmony"; fail=1
fi
# lint: a long pitched kick sample (0.8 s of decaying 55 Hz) is a drum by its name, not a run of C notes
mkdir -p out/check/lint-kick
python3 - <<'PY'
import json, math, struct, wave
sr = 48000
with wave.open("out/check/lint-kick/kick-long.wav", "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b"".join(struct.pack("<h", int(30000 * math.exp(-5 * i / sr) * math.sin(2 * math.pi * 55 * i / sr))) for i in range(int(0.8 * sr))))
pad = [{"beat": b * 4, "dur": 3.9, "key": k, "vel": 0.7} for b in range(4) for k in (64, 68, 71)]
json.dump({"tempo": 120, "tracks": [{"name": "Kick", "plugin": "builtin:sampler", "sampler": {"sample": "kick-long.wav", "root": 36, "oneShot": True},
                                     "notes": [{"beat": q, "dur": 0.5, "key": 36} for q in range(16)]},
                                    {"name": "Pad", "plugin": "builtin:synth", "preset": "PD Warm", "notes": pad}]},
          open("out/check/lint-kick/job.json", "w"))
PY
if "./$build/wavelength" lint out/check/lint-kick/job.json --harmony --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
sys.exit(0 if any(x.get("track") == "Kick" and x.get("reason") == "drum sample" for x in d.get("skipped", [])) else 1)'; then
  echo "ok   lint: a long kick sample is a drum, not notes"
else
  echo "FAIL lint: the long kick sample was read as pitched notes"; fail=1
fi
# late curves on buses and the master warn like on tracks; a bus fed only after its curve starts does not
mkdir -p out/check/late-curves
cat > out/check/late-curves/job.json <<'JOB'
{"tempo": 120, "stems": "none",
 "buses": [{"name": "Music", "automation": {"gain": [[16, -4], [24, 0]]}},
           {"name": "Late", "automation": {"rides": {"lift": [[8, -3], [12, 0]]}}}],
 "master": {"automation": {"rides": [[8, -2], [12, 0]]}},
 "tracks": [{"name": "Kit", "plugin": "builtin:drums", "output": "Music", "notes": [{"beat": 0, "dur": 0.5, "key": 36, "vel": 0.9}, {"beat": 20, "dur": 0.5, "key": 36, "vel": 0.9}]},
            {"name": "Snare", "plugin": "builtin:drums", "output": "Late", "notes": [{"beat": 14, "dur": 0.5, "key": 38, "vel": 0.9}]}]}
JOB
if ! "./$build/wavelength" render out/check/late-curves/job.json --out "out/check/late-curves/$build" --json 2>/dev/null | python3 -c '
import json, sys
w = [x for x in json.load(sys.stdin)["warnings"] if "automation starts at beat" in x]
sys.exit(0 if len(w) == 2 and w[0].startswith("bus '"'"'Music'"'"'") and w[1].startswith("master:") else 1)'; then
  echo "FAIL late-curves: bus/master late automation warnings"; fail=1
fi
# thin bass: a bass whose energy sits in its mids (an FM bass high-passed at 200 Hz) warns; the same bass with a sine sub
# layered on its notes doesn't; tracks[].lowShare reports each part's share below 120 Hz
mkdir -p out/check/thin-bass
python3 - <<'PY'
import json
kick = [{"beat": b, "dur": 0.5, "key": 36, "vel": 0.9} for b in range(32)]
line = [{"beat": b + 0.5, "dur": 0.4, "key": 40 if (b // 4) % 2 == 0 else 36, "vel": 0.9} for b in range(32)]
bass = {"name": "Bass", "plugin": "builtin:synth", "preset": "BA FM", "gain": 6, "notes": line,
        "fx": [{"type": "eq", "bands": [{"type": "highpass", "freq": 200}]}]}
base = {"tempo": 120, "stems": "none", "tracks": [{"name": "Kick", "plugin": "builtin:drums", "notes": kick}, bass]}
json.dump(base, open("out/check/thin-bass/thin.json", "w"))
base["tracks"].append({"name": "Sub", "plugin": "builtin:synth", "preset": "BA Sub", "notes": line})
json.dump(base, open("out/check/thin-bass/full.json", "w"))
PY
thin_why=""
for v in thin full; do
  "./$build/wavelength" render out/check/thin-bass/$v.json --out "out/check/thin-bass/$build-$v" --json 2>/dev/null > "out/check/thin-bass/$v.out" || thin_why="$v did not render"
done
[ -z "$thin_why" ] && thin_why=$(python3 -c '
import json
t = json.load(open("out/check/thin-bass/thin.out")); f = json.load(open("out/check/thin-bass/full.out"))
tw = [w for w in t["warnings"] if w.startswith("the bass is thin")]; fw = [w for w in f["warnings"] if w.startswith("the bass is thin")]
share = {x["name"]: x["lowShare"] for x in t["tracks"]}
if len(tw) != 1 or "'"'"'Bass'"'"'" not in tw[0]: print("no thin-bass warning:", t["warnings"])
elif fw: print("warned with a sub:", fw)
elif not share["Bass"] < 0.15: print("Bass lowShare", share["Bass"])')
if [ -z "$thin_why" ]; then
  echo "ok   mix check: thin bass warns, a sub layer passes"
else
  echo "FAIL thin bass: $thin_why"; fail=1
fi
# stage: faders from stem loudness by role (Kick -12, Bass -15.5, Sub -19), written to gains.json; --apply puts them
# in the job, and the next render (from the track cache) has each track at its target
mkdir -p out/check/stage && cp out/check/thin-bass/full.json out/check/stage/job.json && rm -f out/check/stage/gains.json
stage_why=""
"./$build/wavelength" stage out/check/stage/job.json --apply --json > out/check/stage/stage.json 2>/dev/null || stage_why="stage failed: $(head -c 300 out/check/stage/stage.json)"
[ -z "$stage_why" ] && { "./$build/wavelength" render out/check/stage/job.json --out "out/check/stage/$build" --cache --json > out/check/stage/after.json 2>/dev/null || stage_why="the staged job did not render"; }
[ -z "$stage_why" ] && stage_why=$(python3 -c '
import json
s = json.load(open("out/check/stage/stage.json")); g = json.load(open("out/check/stage/gains.json")); r = json.load(open("out/check/stage/after.json"))
want = {"Kick": ("Kick", -12), "Bass": ("Bass", -15.5), "Sub": ("Sub", -19)}
roles = {t["name"]: t["role"] for t in s["tracks"]}
post = {t["name"]: t["postFaderLufs"] for t in r["tracks"]}
bad = [n for n, (role, lufs) in want.items() if roles.get(n) != role or n not in g or abs(post[n] - lufs) > 0.3]
print(("roles " + str(roles) + " post " + str(post)) if bad else "")')
if [ -z "$stage_why" ]; then
  echo "ok   stage: faders by role into gains.json and the job, tracks land on their targets"
else
  echo "FAIL stage: $stage_why"; fail=1
fi
# render --from/--to: a window of the clips tour (a clip starts before it) renders, and its file is the window's length
if ! "./$build/wavelength" render examples/clips-tour.json --from 3 --to 5 --stems none --out out/check/window/$build --json 2>/dev/null | python3 -c '
import json, sys, wave
r = json.load(sys.stdin)
ok = r.get("ok") and r.get("window") and abs(r["window"]["fromBar"] - 3) < 1e-6 and r["window"]["seconds"] > 0
sys.exit(0 if ok else 1)'; then
  echo "FAIL window render (render --from/--to)"; fail=1
else
  echo "ok   window render: clips-tour bars 3-4"
fi
# analyze finds a render's stems when the render ran elsewhere (the report names them relative to where it ran)
(cd examples && "../$build/wavelength" render clips-tour.json --from 3 --to 5 --stems 16 --out "../out/check/window-rel/$build" --json >/dev/null 2>&1)
if "./$build/wavelength" analyze "out/check/window-rel/$build" --json 2>/dev/null | python3 -c '
import json, sys
sys.exit(0 if len(json.load(sys.stdin)["stems"]) == 3 else 1)'; then
  echo "ok   analyze: a render folder's stems found from another directory"
else
  echo "FAIL analyze: stems of a render made from another directory"; fail=1
fi
# MusicXML: repeats, endings and D.S. al Fine play in order; transposition, dynamics, ties, chords and voices
if ! { "./$build/wavelength" import examples/scores/repeats.musicxml --out out/check/mx-repeats --json > /dev/null &&
       "./$build/wavelength" import examples/scores/dynamics.musicxml --out out/check/mx-dynamics --json > /dev/null &&
       "./$build/wavelength" import examples/scores/pedal-and-graces.musicxml --out out/check/mx-pedal --json > /dev/null; } ||
   ! python3 -c '
import json, sys
r = json.load(open("out/check/mx-repeats/job.json"))["tracks"][0]["notes"]
order = [((n["key"] // 12 - 1) - 4) * 7 + [0, 2, 4, 5, 7, 9, 11].index(n["key"] % 12) + 1 for n in r]
d = [(n["beat"], n["dur"], n["key"], n["vel"]) for n in json.load(open("out/check/mx-dynamics/job.json"))["tracks"][0]["notes"]]
want = [(0.0, 1.0, 70, 0.4), (1.0, 1.0, 72, 0.4), (2.0, 1.0, 74, 0.503), (3.0, 1.0, 75, 0.607), (4.0, 0.5, 77, 0.71), (4.0, 4.0, 58, 0.71),
        (5.0, 1.0, 77, 0.83), (6.0, 4.0, 70, 0.71), (6.0, 2.0, 74, 0.71), (10.0, 2.0, 67, 0.71)]
p = [(n["beat"], n["dur"], n["key"]) for n in json.load(open("out/check/mx-pedal/job.json"))["tracks"][0]["notes"]]
pw = [(0.0, 2.0, 60), (1.0, 1.0, 64), (2.0, 2.0, 67), (3.0, 1.0, 72), (3.875, 0.125, 64), (4.0, 4.0, 62)]
sys.exit(0 if order == [1, 2, 1, 2, 3, 4, 3, 5, 6, 7, 8, 9, 10, 6, 7, 8] and d == want and p == pw else 1)'; then
  echo "FAIL musicxml import"; fail=1
else
  echo "ok   musicxml: repeat order, transposition, dynamics, ties, pedal, grace notes"
fi
# SoundFont: a generated one-sample SF2 plays in tune, loops past its 45 ms of audio, tracks the key, and
# decays to its sustain (-20 dB) faster at low velocity (a velocity modulator on the decay time)
mkdir -p out/check/sf2
python3 scripts/make-test-sf2.py out/check/sf2/test.sf2
cat > out/check/sf2/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 1, "stems": "float",
 "tracks": [{"name": "Hi", "plugin": "builtin:sampler", "sampler": {"soundfont": "test.sf2", "program": 0}, "notes": [{"beat": 0, "dur": 3, "key": 69, "vel": 1.0}]},
            {"name": "Lo", "plugin": "builtin:sampler", "sampler": {"soundfont": "test.sf2", "program": 0}, "notes": [{"beat": 0, "dur": 3, "key": 81, "vel": 0.3}]}]}
JOB
if ! "./$build/wavelength" render out/check/sf2/job.json --out "out/check/sf2/$build" --json > /dev/null 2>&1 || ! python3 -c '
import json, subprocess, sys
w = sys.argv[1]
def an(f, s, e): return json.loads(subprocess.run([w, "analyze", f, "--start", str(s), "--end", str(e), "--json"], capture_output=True, text=True).stdout)
d = sys.argv[2]
hi0, hi2 = an(d + "/stems/01-hi.wav", 0.1, 0.3), an(d + "/stems/01-hi.wav", 2.0, 2.5)
lo2 = an(d + "/stems/02-lo.wav", 2.0, 2.5)
ok = hi0["pitch"]["note"] == "A4" and hi2["pitch"]["note"] == "A4" and lo2["pitch"]["note"] == "A5" and hi2["rmsDb"] > -60 and hi0["rmsDb"] - hi2["rmsDb"] > 5
sys.exit(0 if ok else 1)' "./$build/wavelength" "out/check/sf2/$build"; then
  echo "FAIL soundfont: pitch, loop, key tracking or envelope"; fail=1
else
  echo "ok   soundfont: generated SF2 in tune, looped, key-tracked, decaying"
fi
# DAWproject export: hello.json (four CLAP synths) exports with every plugin state, imports back with the same
# plugins, notes and faders, and each track renders as it did
dp="out/check/dawproject/$build"
mkdir -p "$dp"
if ! "./$build/wavelength" export examples/hello.json --out "$dp/hello.dawproject" --json > /dev/null 2>&1 ||
   ! "./$build/wavelength" import "$dp/hello.dawproject" --out "$dp/in" --bitwig none --json > /dev/null 2>&1 ||
   ! python3 -c '
import json, sys
a, b = json.load(open("examples/hello.json")), json.load(open(sys.argv[1] + "/in/job.json"))
ok = [t["name"] for t in a["tracks"]] == [t["name"] for t in b["tracks"]]
for x, y in zip(a["tracks"], b["tracks"]):
    ok = ok and len(x["notes"]) == len(y["notes"]) and abs(x.get("gain", 0) - y.get("gain", 0)) < 0.01 and y.get("state", "").endswith(".clap-preset")
sys.exit(0 if ok else 1)' "$dp"; then
  echo "FAIL dawproject: export/import round trip"; fail=1
else
  echo "ok   dawproject: hello exports with its plugin states and imports back"
fi
# built-in instruments print: the SFZ tour's two sampler tracks arrive as audio files in the project
if ! "./$build/wavelength" export examples/sfz-tour.json --out "$dp/sfz.dawproject" --json > /dev/null 2>&1 ||
   ! python3 -c '
import sys, zipfile
names = zipfile.ZipFile(sys.argv[1]).namelist()
sys.exit(0 if "audio/synth-printed.wav" in names and "audio/drums-printed.wav" in names else 1)' "$dp/sfz.dawproject"; then
  echo "FAIL dawproject: built-in tracks not printed"; fail=1
else
  echo "ok   dawproject: built-in instrument tracks printed to audio"
fi
# deliveries: FLAC (24 and 16 bit) and a 24-bit WAV decode back to mix.wav's loudness and true peak
dl="out/check/deliver/$build"
if ! "./$build/wavelength" render examples/mastering.json --deliver flac,flac:16,wav:24 --out "$dl" --json 2>/dev/null | python3 -c '
import json, sys
r = json.load(sys.stdin)
m, d = r["mix"], r["mix"].get("deliveries", [])
ok = len(d) == 3 and all(abs(x["lufs"] - m["lufs"]) <= 0.1 and abs(x["truePeakDb"] - m["truePeakDb"]) <= 0.1 for x in d)
sys.exit(0 if ok else 1)' || { command -v flac > /dev/null && ! flac -s -t "$dl/mix.flac" "$dl/mix-16bit.flac"; }; then
  echo "FAIL deliver: flac/wav deliveries"; fail=1
else
  echo "ok   deliver: flac 24/16 and wav 24 decode back to the mix"
fi
# Audio Units (macOS): Apple's GM synth plays a GM program by name through a factory reverb preset and AUDelay
if [ "$(uname)" = Darwin ]; then
  mkdir -p out/check/au
  cat > out/check/au/job.json <<'JOB'
{"sampleRate": 48000, "tempo": 120, "tail": 1, "stems": "none",
 "tracks": [{"name": "GM", "plugin": "au:DLSMusicDevice", "preset": "Violin", "notes": [{"beat": 0, "dur": 1, "key": "A4", "vel": 0.8}],
             "fx": [{"plugin": "au:AUMatrixReverb", "preset": "Large Hall"}, {"plugin": "au:AUDelay", "params": {"Delay Time": 0.25}}]}]}
JOB
  if "./$build/wavelength" render out/check/au/job.json --out out/check/au/out --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
sys.exit(0 if d.get("ok") and d["tracks"][0].get("lufs", -120) > -60 else 1)'; then
    echo "ok   au: DLSMusicDevice GM program, AUMatrixReverb preset, AUDelay"
  else
    echo "FAIL au: Apple's Audio Units did not render"; fail=1
  fi
fi
# Apple Loops: a tagged CAF (scripts/make-test-apple-loop.py: 4 beats at 120 BPM in C major, notes inside) found
# by name follows the song's tempo (3 repeats at 90 BPM = 8 s) and moves into the song's key (C -> D: the sine
# reads D4); its notes come out moved into a key and import as a job; on macOS an AAC copy decodes to the same
# length and loudness
rm -rf out/check/loops && mkdir -p out/check/loops/lib
python3 scripts/make-test-apple-loop.py "out/check/loops/lib/Test Loop.caf"
cat > out/check/loops/job.json <<'JOB'
{"tempo": 90, "tail": 0, "leadIn": 0, "stems": "16", "keys": [{"bar": 1, "key": "D major"}],
 "tracks": [{"name": "Loop", "plugin": "builtin:audio", "clips": [{"file": "lib:Apple Loops/Test Loop.caf", "beat": 0, "repeat": 3, "key": "song"}]}]}
JOB
export WAVELENGTH_APPLE_LOOPS="$PWD/out/check/loops/lib"
loops_why=""
if ! "./$build/wavelength" render out/check/loops/job.json --out out/check/loops/out --json > out/check/loops/report.json 2>/dev/null; then
  loops_why="the loop job did not render"
elif ! "./$build/wavelength" analyze out/check/loops/out/stems/01-loop.wav --json 2>/dev/null | python3 -c '
import json, sys
a = json.load(sys.stdin); r = json.load(open("out/check/loops/report.json"))
sys.exit(0 if a["pitch"]["note"] == "D4" and abs(r["duration"] - 8.0) < 0.02 else 1)'; then
  loops_why="the loop did not follow the tempo (8 s) and key (D4)"
elif ! "./$build/wavelength" loops --notes "Test Loop" --key "E major" --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
sys.exit(0 if [n["key"] for n in d["notes"]] == [64, 68] and d["loop"]["bpm"] == 120 else 1)'; then
  loops_why="loops --notes did not give the notes moved into E"
elif ! "./$build/wavelength" loops --notes "Test Drummer" --json 2>/dev/null | python3 -c '
import json, sys
sys.exit(0 if [n["beat"] for n in json.load(sys.stdin)["notes"]] == [0, 1] else 1)'; then
  loops_why="loops --notes did not move a Drummer slice back to beat 0"
elif ! "./$build/wavelength" import "out/check/loops/lib/Test Loop.caf" --out out/check/loops/imp > /dev/null 2>&1 ||
     ! python3 -c 'import json; j = json.load(open("out/check/loops/imp/job.json")); assert len(j["tracks"][0]["notes"]) == 2'; then
  loops_why="import of the loop's notes failed"
elif [ "$(uname)" = Darwin ]; then
  afconvert -f caff -d aac out/check/loops/out/mix.wav out/check/loops/aac.caf
  cat > out/check/loops/aac.json <<'JOB'
{"tempo": 90, "tail": 0, "leadIn": 0, "stems": "none", "tracks": [{"name": "AAC", "plugin": "builtin:audio", "clips": [{"file": "aac.caf", "beat": 0}]}]}
JOB
  "./$build/wavelength" render out/check/loops/aac.json --out out/check/loops/aac --json > out/check/loops/aac-report.json 2>/dev/null &&
    python3 -c '
import json, sys
a = json.load(open("out/check/loops/aac-report.json")); r = json.load(open("out/check/loops/report.json"))
sys.exit(0 if abs(a["mix"]["lufs"] - r["mix"]["lufs"]) < 0.3 and abs(a["duration"] - r["duration"]) < 0.05 else 1)' ||
    loops_why="an AAC CAF did not decode to the same length and loudness"
fi
unset WAVELENGTH_APPLE_LOOPS
if [ -n "$loops_why" ]; then echo "FAIL apple loops: $loops_why"; fail=1; else echo "ok   apple loops: tempo, key, repeat, notes (Drummer slices from 0), import, AAC"; fi
# Logic and GarageBand instruments: a generated EXS instrument (scripts/make-test-exs.py) plays A4 from its one zone
# with its own envelope (release 64 = 0.65 s past a 1 s note) unless the track sets "release"; a GarageBand patch whose
# Sampler slot stores that instrument plays it the same; a patch on Alchemy that stores no preset text is refused, naming it
rm -rf out/check/exs && mkdir -p out/check/exs
python3 scripts/make-test-exs.py out/check/exs
cat > out/check/exs/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 2, "stems": "16",
 "tracks": [{"name": "Exs", "plugin": "builtin:sampler", "sampler": {"exs": "Test Instrument.exs"}, "notes": [{"beat": 0, "dur": 1, "key": 69, "vel": 1}]},
            {"name": "Patch", "plugin": "builtin:sampler", "sampler": {"patch": "Test Patch", "effects": false}, "notes": [{"beat": 0, "dur": 1, "key": 69, "vel": 1}]},
            {"name": "Short", "plugin": "builtin:sampler", "sampler": {"exs": "Test Instrument.exs", "release": 0.05}, "notes": [{"beat": 0, "dur": 1, "key": 69, "vel": 1}]},
            {"name": "PatchFx", "plugin": "builtin:sampler", "sampler": {"patch": "Test Patch"}, "notes": [{"beat": 0, "dur": 1, "key": 69, "vel": 1}]}]}
JOB
cat > out/check/exs/synth.json <<'JOB'
{"tempo": 60, "tracks": [{"name": "Synth", "plugin": "builtin:sampler", "sampler": {"patch": "Test Synth"}, "notes": [{"beat": 0, "dur": 1, "key": 60}]}]}
JOB
export WAVELENGTH_LOGIC_PATCHES="$PWD/out/check/exs/patches"
exs_why=""
if ! "./$build/wavelength" render out/check/exs/job.json --out out/check/exs/out --json > out/check/exs/report.json 2>/dev/null; then
  exs_why="the instrument job did not render"
else
  for t in 01-exs 02-patch 03-short; do "./$build/wavelength" analyze "out/check/exs/out/stems/$t.wav" --json > "out/check/exs/$t.json" 2>/dev/null; done
  python3 -c '
import json, sys
a = {t: json.load(open("out/check/exs/%s.json" % t)) for t in ("01-exs", "02-patch", "03-short")}
ok = all(x["pitch"]["note"] == "A4" for x in a.values())
ok &= abs(a["01-exs"]["lufs"] - a["02-patch"]["lufs"]) < 0.1
ok &= 1.4 < a["01-exs"]["envelope"]["lastSound"] < 1.8 and a["03-short"]["envelope"]["lastSound"] < 1.15
r = {t["name"]: t["lufs"] for t in json.load(open("out/check/exs/report.json"))["tracks"]}
ok &= -4.2 < r["PatchFx"] - r["Patch"] < -3.0   # its Channel EQ: -3 dB master, a 200 Hz low cut under the 440 Hz sine
sys.exit(0 if ok else 1)' || exs_why="the instrument or the patch played wrong (pitch, level or release): $(python3 -c 'import json; print([(t, json.load(open("out/check/exs/%s.json" % t))["envelope"]["lastSound"]) for t in ("01-exs", "02-patch", "03-short")])')"
fi
synth_out=$("./$build/wavelength" render out/check/exs/synth.json --out out/check/exs/synth --json 2>/dev/null || true)
if [ -z "$exs_why" ] && ! echo "$synth_out" | grep "Alchemy" > /dev/null; then
  exs_why="an Alchemy patch was not refused by name"
fi
# samples --patch: the stored instrument's sample is installed and the patch plays; the synth patch says why it doesn't
if [ -z "$exs_why" ]; then
  p1=$("./$build/wavelength" samples --patch "Test Patch" --json 2>/dev/null || true)
  p2=$("./$build/wavelength" samples --patch "Test Synth" --json 2>/dev/null || true)
  python3 -c '
import json, sys
a, b = json.loads(sys.argv[1])["patch"], json.loads(sys.argv[2])["patch"]
c = a["channels"][0]
sys.exit(0 if a["plays"] and c["instrument"] == "Sampler" and c["samples"] == {"installed": 1, "total": 1} and "Channel EQ" in c["effects"]
         and a["effects"] == [{"type": "eq", "bands": [{"type": "highpass", "freq": 200, "q": 0.71}]}, {"type": "gain", "db": -3}]
         and not b["plays"] and "GarageBand" in b["why"] else 1)' "$p1" "$p2" || exs_why="samples --patch described the test patches wrong"
fi
# a Retro Synth patch plays on builtin:synth by name (its saw a C4 note sounds C5: the patch is transposed +12), and so
# does a Vintage B3 patch
if [ -z "$exs_why" ]; then
  cat > out/check/exs/retro.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 0, "stems": "16",
 "tracks": [{"name": "Retro", "plugin": "builtin:synth", "preset": "Test Retro", "notes": [{"beat": 0, "dur": 1, "key": 60, "vel": 0.8}]}]}
JOB
  if ! "./$build/wavelength" render out/check/exs/retro.json --out out/check/exs/retro --json > /dev/null 2>&1 ||
     [ "$("./$build/wavelength" analyze out/check/exs/retro/stems/01-retro.wav --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["pitch"]["note"])')" != "C5" ]; then
    exs_why="the Retro Synth patch did not play C5 on builtin:synth"
  fi
  # and a Vintage B3 patch whose only drawbar is the upper 8' plays the note itself
  sed -e 's/"Retro"/"Organ"/' -e 's/Test Retro/Test Organ/' out/check/exs/retro.json > out/check/exs/organ.json
  if [ -z "$exs_why" ] && { ! "./$build/wavelength" render out/check/exs/organ.json --out out/check/exs/organ --json > /dev/null 2>&1 ||
     [ "$("./$build/wavelength" analyze out/check/exs/organ/stems/01-organ.wav --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["pitch"]["note"])')" != "C4" ]; }; then
    exs_why="the Vintage B3 patch did not play C4 on builtin:synth"
  fi
  # an Alchemy patch with its preset text (Test Alchemy: source A a saw tuned +12, an AHDSR on the amp, a low-pass) plays
  # on builtin:synth, C4 sounding C5; with Alchemy's arpeggiator on (Test Alchemy Arp) the notes play through it
  sed -e 's/"Retro"/"Alchemy"/' -e 's/Test Retro/Test Alchemy/' out/check/exs/retro.json > out/check/exs/alchemy.json
  if [ -z "$exs_why" ] && { ! "./$build/wavelength" render out/check/exs/alchemy.json --out out/check/exs/alchemy --json > /dev/null 2>&1 ||
     [ "$("./$build/wavelength" analyze out/check/exs/alchemy/stems/01-alchemy.wav --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["pitch"]["note"])')" != "C5" ]; }; then
    exs_why="the Alchemy patch did not play C5 on builtin:synth"
  fi
  sed -e 's/Test Alchemy/Test Alchemy Arp/' out/check/exs/alchemy.json > out/check/exs/alchemy-arp.json
  if [ -z "$exs_why" ] && ! "./$build/wavelength" render out/check/exs/alchemy-arp.json --out out/check/exs/alchemy-arp --json 2>/dev/null | python3 -c '
import json, sys
t = json.load(sys.stdin)["tracks"][0]
sys.exit(0 if any("through its Arpeggiator (1/16 up over 2 octaves)" in w for w in t["warnings"]) else 1)'; then
    exs_why="Alchemy's arpeggiator did not play the Test Alchemy Arp patch's notes"
  fi
  # Vintage Electric Piano, Vintage Clav and Sculpture patches play on builtin:synth by name: Test EP and Test Clav
  # sound C4, Test Sculpture (transposed +12) C5; Test Sculpture Side (object 2 External) is refused with the reason,
  # there and in samples --patch
  if [ -z "$exs_why" ]; then
    cat > out/check/exs/keys.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 0, "stems": "16",
 "tracks": [{"name": "EP", "plugin": "builtin:synth", "preset": "Test EP", "notes": [{"beat": 0, "dur": 1, "key": 60, "vel": 0.8}]},
            {"name": "Clav", "plugin": "builtin:synth", "preset": "Test Clav", "notes": [{"beat": 0, "dur": 1, "key": 60, "vel": 0.8}]},
            {"name": "Sculpture", "plugin": "builtin:synth", "preset": "Test Sculpture", "notes": [{"beat": 0, "dur": 1, "key": 60, "vel": 0.8}]}]}
JOB
    if ! "./$build/wavelength" render out/check/exs/keys.json --out out/check/exs/keys --json > /dev/null 2>&1; then
      exs_why="the Vintage Electric Piano, Vintage Clav and Sculpture patches did not render on builtin:synth"
    else
      for t in 01-ep:C4 02-clav:C4 03-sculpture:C5; do
        got=$("./$build/wavelength" analyze "out/check/exs/keys/stems/${t%%:*}.wav" --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["pitch"]["note"])')
        [ "$got" = "${t##*:}" ] || exs_why="${exs_why:+$exs_why; }the ${t%%:*} patch played $got, not ${t##*:}"
      done
    fi
    sed -e 's/Test Retro/Test Sculpture Side/' out/check/exs/retro.json > out/check/exs/side.json
    side_out=$("./$build/wavelength" render out/check/exs/side.json --out out/check/exs/side --json 2>/dev/null || true)
    side_patch=$("./$build/wavelength" samples --patch "Test Sculpture Side" --json 2>/dev/null || true)
    if [ -z "$exs_why" ] && ! python3 -c '
import json, sys
r, p = json.loads(sys.argv[1]), json.loads(sys.argv[2])["patch"]
sys.exit(0 if not r["ok"] and "External" in r["error"] and not p["plays"] and "External" in p["why"] else 1)' "$side_out" "$side_patch"; then
      exs_why="a Sculpture patch played by side-chain audio was not refused with the reason"
    fi
  fi
  # an Alchemy patch on additive synthesis (Test Alchemy Additive: source A 25 partials at a saw's levels, untuned)
  # plays on builtin:synth through one additive oscillator, C4 sounding C4
  sed -e 's/"Retro"/"Additive"/' -e 's/Test Retro/Test Alchemy Additive/' out/check/exs/retro.json > out/check/exs/additive.json
  if [ -z "$exs_why" ] && { ! "./$build/wavelength" render out/check/exs/additive.json --out out/check/exs/additive --json > /dev/null 2>&1 ||
     ! "./$build/wavelength" analyze out/check/exs/additive/stems/01-additive.wav --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
sys.exit(0 if d["pitch"]["note"] == "C4" and not d["silent"] and d["lufs"] > -40 else 1)' ||
     ! "./$build/wavelength" samples --patch "Test Alchemy Additive" --json 2>/dev/null | python3 -c '
import json, sys
o = json.load(sys.stdin)["patch"]["synth"]["patch"]["osc"]
sys.exit(0 if len(o) == 1 and o[0]["wave"] == "additive" and len(o[0]["partials"]) == 25 else 1)'; }; then
    exs_why="the Test Alchemy Additive patch did not play C4 through an additive oscillator"
  fi
fi
unset WAVELENGTH_LOGIC_PATCHES
if [ -n "$exs_why" ]; then echo "FAIL exs: $exs_why"; fail=1; else echo "ok   exs: instrument envelope and level, a patch's stored instrument and its Channel EQ, synth patches refused or re-created (Alchemy too, with its arpeggiator and on additive synthesis; Vintage Electric Piano, Vintage Clav and Sculpture), --patch"; fi
# convolve: a click through a generated stereo IR (scripts/make-test-ir.py) comes out as that IR, each channel at unit
# energy and nothing before the click; with predelay 100 ms it comes 100 ms later
rm -rf out/check/ir && mkdir -p out/check/ir
python3 scripts/make-test-ir.py out/check/ir
cat > out/check/ir/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 1, "stems": "float",
 "tracks": [{"name": "Wet", "plugin": "builtin:audio", "clips": [{"file": "click.wav", "beat": 0, "fadeIn": 0, "fadeOut": 0}],
             "fx": [{"type": "convolve", "ir": "test-room.wav", "mix": 1, "highpass": 0}]},
            {"name": "Late", "plugin": "builtin:audio", "clips": [{"file": "click.wav", "beat": 0, "fadeIn": 0, "fadeOut": 0}],
             "fx": [{"type": "convolve", "ir": "test-room.wav", "mix": 1, "highpass": 0, "predelay": 100}]}]}
JOB
if "./$build/wavelength" render out/check/ir/job.json --out out/check/ir/out --json > /dev/null 2>&1 && python3 - <<'PY'
import math, struct, sys
def channels(p):
    b = open(p, 'rb').read(); i = 12; fmt = data = None
    while i < len(b):
        cid, n = b[i:i + 4], struct.unpack('<I', b[i + 4:i + 8])[0]
        if cid == b'fmt ': fmt = struct.unpack('<HHIIHH', b[i + 8:i + 24])
        if cid == b'data': data = b[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)
    v = struct.unpack('<%df' % (len(data) // 4), data) if fmt[0] == 3 else [x / 32767 for x in struct.unpack('<%dh' % (len(data) // 2), data)]
    return [v[c::fmt[1]] for c in range(fmt[1])]
ir = channels('out/check/ir/test-room.wav')
ok = True
for name, at in (('01-wet', 480), ('02-late', 480 + 4800)):
    w = channels('out/check/ir/out/stems/%s.wav' % name)
    for c in range(2):
        h = ir[c]; e = math.sqrt(sum(x * x for x in h)); seg = w[c][at:at + len(h)]
        na = math.sqrt(sum(x * x for x in seg))
        corr = sum(a * b / e for a, b in zip(seg, h)) / na
        ok &= corr > 0.9999 and abs(na * na - 1) < 0.01 and max(abs(x) for x in w[c][:at]) < 1e-4
sys.exit(0 if ok else 1)
PY
then echo "ok   convolve: a click comes out as the IR, unit energy, predelay"
else echo "FAIL convolve: the convolution did not reproduce the impulse response"; fail=1; fi
# sample folders: note-named samples play as a multisample whichever way their names count octaves (a sample's
# pitch decides: "Logic Notes C3" sounds C4), and a drum machine kit maps by its abbreviations (BD1, SD1, HH1, HHo)
rm -rf out/check/folders && mkdir -p out/check/folders
python3 scripts/make-test-sample-folders.py out/check/folders/lib
cat > out/check/folders/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 0, "stems": "16",
 "tracks": [{"name": "Sci", "plugin": "builtin:sampler", "sampler": {"multisample": "Sci Notes"}, "notes": [{"beat": 0, "dur": 1, "key": 57}]},
            {"name": "Logic", "plugin": "builtin:sampler", "sampler": {"multisample": "Logic Notes"}, "notes": [{"beat": 0, "dur": 1, "key": 57}]}]}
JOB
folders_why=""
if ! WAVELENGTH_SAMPLES_PATH="$PWD/out/check/folders/lib" "./$build/wavelength" render out/check/folders/job.json --out out/check/folders/out --json > /dev/null 2>&1; then
  folders_why="the note-named folders did not render"
else
  for t in 01-sci 02-logic; do
    note=$("./$build/wavelength" analyze "out/check/folders/out/stems/$t.wav" --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["pitch"]["note"])')
    [ "$note" = "A3" ] || folders_why="$t played $note for A3"
  done
fi
map=$(WAVELENGTH_SAMPLES_PATH="$PWD/out/check/folders/lib" "./$build/wavelength" samples --kit "Machine Kit" --json 2>/dev/null | python3 -c '
import json, sys
print(" ".join("%d=%s" % (m["key"], m["file"]) for m in json.load(sys.stdin)["map"]))')
for want in 36=MK_BD1.wav 38=MK_SD1.wav 42=MK_HH1.wav 46=MK_HHo.wav 39=MK_Clap.wav; do
  case " $map " in *" $want "*) ;; *) folders_why="kit map lacks $want ($map)";; esac
done
# an Ultrabeat patch plays its kit folder (named by its settings, "Machine Kit.pst")
python3 scripts/make-test-exs.py out/check/folders/exs
cat > out/check/folders/beat.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 0, "stems": "none",
 "tracks": [{"name": "Beat", "plugin": "builtin:sampler", "sampler": {"patch": "Test Beat GB"}, "notes": [{"beat": 0, "dur": 0.5, "key": 36}]}]}
JOB
beat=$(WAVELENGTH_SAMPLES_PATH="$PWD/out/check/folders/lib" WAVELENGTH_LOGIC_PATCHES="$PWD/out/check/folders/exs/patches" \
  "./$build/wavelength" render out/check/folders/beat.json --out out/check/folders/beat --json 2>/dev/null || true)
echo "$beat" | python3 -c '
import json, sys
r = json.load(sys.stdin)
sys.exit(0 if r.get("ok") and r["tracks"][0]["lufs"] > -60 and "Machine Kit" in " ".join(r["tracks"][0].get("warnings", [])) else 1)' ||
  folders_why="the Ultrabeat patch did not play its kit"
if [ -n "$folders_why" ]; then echo "FAIL sample folders: $folders_why"; fail=1; else echo "ok   sample folders: note-named multisamples (both octave namings), drum machine abbreviations, Ultrabeat patch kits"; fi
# DecentSampler: a generated preset (scripts/make-test-dspreset.py) plays each key from the sample its range maps (C4
# from the A3 sample, E5 from the A5 one) with its 0.6 s release; its Volume knob at 0.5 sets the instrument 6 dB down
# (and DecentSampler plays 5.3 dB under a sample's level), so it measures 11.3 dB under the raw sample; its reverb rings on
# after the dry note's release
rm -rf out/check/dspreset && mkdir -p out/check/dspreset
python3 scripts/make-test-dspreset.py out/check/dspreset
cat > out/check/dspreset/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 2, "stems": "16",
 "tracks": [{"name": "Low", "plugin": "builtin:sampler", "sampler": {"dspreset": "Test Instrument.dspreset", "effects": false}, "notes": [{"beat": 0, "dur": 1, "key": 60, "vel": 1}]},
            {"name": "High", "plugin": "builtin:sampler", "sampler": {"dspreset": "Test Instrument.dspreset", "effects": false}, "notes": [{"beat": 0, "dur": 1, "key": 76, "vel": 1}]},
            {"name": "Raw", "plugin": "builtin:sampler", "sampler": {"sample": "Samples/Sine A3.wav", "root": 57}, "notes": [{"beat": 0, "dur": 1, "key": 60, "vel": 1}]},
            {"name": "Wet", "plugin": "builtin:sampler", "sampler": {"dspreset": "Test Instrument.dspreset"}, "notes": [{"beat": 0, "dur": 1, "key": 60, "vel": 1}]}]}
JOB
ds_why=""
if ! "./$build/wavelength" render out/check/dspreset/job.json --out out/check/dspreset/out --json > out/check/dspreset/report.json 2>/dev/null; then
  ds_why="the preset did not render: $(python3 -c 'import json; print(json.load(open("out/check/dspreset/report.json")).get("error"))' 2>/dev/null)"
else
  for t in 01-low 02-high 04-wet; do "./$build/wavelength" analyze "out/check/dspreset/out/stems/$t.wav" --json > "out/check/dspreset/$t.json" 2>/dev/null; done
  python3 - <<'PY' || ds_why="it played wrong (pitch, release, knob volume or reverb): $(cat out/check/dspreset/why.txt 2>/dev/null)"
import json, math, struct, sys
a = {t: json.load(open("out/check/dspreset/%s.json" % t)) for t in ("01-low", "02-high", "04-wet")}
def rms(name, t0, t1):   # dB over [t0, t1) s of a 16-bit stem
    b = open("out/check/dspreset/out/stems/%s.wav" % name, "rb").read(); i = 12; ch = 2; data = b""
    while i < len(b):
        cid, n = b[i:i + 4], struct.unpack("<I", b[i + 4:i + 8])[0]
        if cid == b"fmt ": ch = struct.unpack("<H", b[i + 10:i + 12])[0]
        if cid == b"data": data = b[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)
    v = struct.unpack("<%dh" % (len(data) // 2), data)[int(t0 * 48000) * ch:int(t1 * 48000) * ch]
    return 10 * math.log10(sum(x * x for x in v) / len(v) / 32768 ** 2 + 1e-12)
knob = rms("01-low", 0.2, 0.8) - rms("03-raw", 0.2, 0.8)
checks = {"Low plays C4": a["01-low"]["pitch"]["note"] == "C4", "High plays E5": a["02-high"]["pitch"]["note"] == "E5",
          "0.6 s release": 1.3 < a["01-low"]["envelope"]["lastSound"] < 1.75, "knob volume -11.3 dB (%.2f)" % knob: abs(knob + 11.3) < 0.3,
          "reverb tail": rms("04-wet", 1.7, 2.2) > rms("01-low", 1.7, 2.2) + 20}
open("out/check/dspreset/why.txt", "w").write(", ".join(k for k, v in checks.items() if not v))
sys.exit(0 if all(checks.values()) else 1)
PY
fi
# samples --dspreset describes it: both samples play, the reverb and its make-up gain, no notes for a preset that plays whole
if [ -z "$ds_why" ] && ! "./$build/wavelength" samples --dspreset out/check/dspreset/"Test Instrument.dspreset" --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)["dspreset"]
sys.exit(0 if d["regions"] == 2 and d["keys"] == [0, 127] and [e["type"] for e in d["effects"]] == ["reverb", "gain"] and not d["notes"] else 1)'; then
  ds_why="samples --dspreset described the test preset wrong"
fi
if [ -n "$ds_why" ]; then echo "FAIL dspreset: $ds_why"; fail=1; else echo "ok   dspreset: key split, release, a knob binding at its saved value, reverb, --dspreset"; fi
# gate as a noise gate: a 220 Hz tone at -6 dB for a second, then at -50 dB: with a -30 dB threshold the quiet second goes
# silent and the loud one stays; phaser: a saw through it keeps its pitch, sounds, and turns stereo
rm -rf out/check/gate && mkdir -p out/check/gate
python3 - <<'PY'
import math, struct
rate = 48000
x = [(0.5 if i < rate else 0.003) * math.sin(2 * math.pi * 220 * i / rate) for i in range(2 * rate)]
d = struct.pack('<%dh' % len(x), *[int(v * 32767) for v in x])
open('out/check/gate/tone.wav', 'wb').write(b'RIFF' + struct.pack('<I', 36 + len(d)) + b'WAVE' + b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16) + b'data' + struct.pack('<I', len(d)) + d)
PY
cat > out/check/gate/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 0, "stems": "16",
 "tracks": [{"name": "Gated", "plugin": "builtin:audio", "clips": [{"file": "tone.wav", "beat": 0, "fadeIn": 0, "fadeOut": 0}],
             "fx": [{"type": "gate", "threshold": -30, "hold": 20, "release": 30}]},
            {"name": "Phased", "plugin": "builtin:synth", "preset": "Init", "notes": [{"beat": 0, "dur": 2, "key": 48}],
             "fx": [{"type": "phaser", "rate": 1, "stages": 6, "feedback": 0.5}]}]}
JOB
gate_why=""
if ! "./$build/wavelength" render out/check/gate/job.json --out out/check/gate/out --json > /dev/null 2>&1; then gate_why="the job did not render"
else
  loud=$("./$build/wavelength" analyze out/check/gate/out/stems/01-gated.wav --start 0.2 --end 0.9 --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["lufs"])')
  quiet=$("./$build/wavelength" analyze out/check/gate/out/stems/01-gated.wav --start 1.3 --end 1.9 --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["lufs"])')
  python3 -c "import sys; sys.exit(0 if $loud > -10 and $quiet < -100 else 1)" || gate_why="the noise gate passed $quiet LUFS of the quiet second (loud $loud)"
  "./$build/wavelength" analyze out/check/gate/out/stems/02-phased.wav --start 0.2 --end 1.8 --json 2>/dev/null | python3 -c '
import json, sys
a = json.load(sys.stdin)
sys.exit(0 if a["pitch"]["note"] == "C3" and a["lufs"] > -30 and a["stereo"]["width"] > 0.1 else 1)' || gate_why="${gate_why:+$gate_why; }the phaser lost the pitch, the level or its width"
fi
if [ -n "$gate_why" ]; then echo "FAIL gate/phaser: $gate_why"; fail=1; else echo "ok   gate/phaser: a noise gate closes on a quiet tone, a phaser keeps pitch and widens"; fi
# additive oscillator: a 200-partial saw at C7 sounds C7 and nothing aliases below its fundamental (every component
# under 1.9 kHz at least 60 dB under it: the partials above 0.45 x the sample rate are left out); an inharmonic,
# shifted and panned set plays through a unison as a bank of sines
mkdir -p out/check/additive
cat > out/check/additive/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 0.2, "stems": "16",
 "tracks": [{"name": "Saw", "plugin": "builtin:synth", "synth": {"osc": [{"wave": "additive", "harmonics": {"count": 200, "tilt": -6}}], "filter": {"type": "off"}},
             "notes": [{"beat": 0, "dur": 1, "key": "C7", "vel": 0.8}]},
            {"name": "Bell", "plugin": "builtin:synth", "synth": {"osc": [{"wave": "additive", "partials": [[1, 1], [0.5, 2.76, -0.5], [0.3, 5.4, 0.5]], "shiftHz": 3}],
             "filter": {"type": "off"}, "unison": 3}, "notes": [{"beat": 0, "dur": 1, "key": "A4", "vel": 0.8}]}]}
JOB
additive_why=""
if ! "./$build/wavelength" render out/check/additive/job.json --out "out/check/additive/$build" --json > /dev/null 2>&1; then
  additive_why="the job did not render"
elif [ "$("./$build/wavelength" analyze "out/check/additive/$build/stems/01-saw.wav" --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["pitch"]["note"])')" != "C7" ]; then
  additive_why="the 200-partial saw did not sound C7"
elif ! python3 - "out/check/additive/$build/stems/01-saw.wav" <<'PY'
import math, sys, wave
w = wave.open(sys.argv[1])
sr, ch, raw = w.getframerate(), w.getnchannels(), w.readframes(w.getnframes())
x = [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2 * ch)][int(0.3 * sr):int(0.3 * sr) + 8192]
x = [s * (0.5 - 0.5 * math.cos(2 * math.pi * i / (len(x) - 1))) for i, s in enumerate(x)]
def level(hz):   # Goertzel: the windowed segment's magnitude at hz
    c, a, b = 2 * math.cos(2 * math.pi * hz / sr), 0.0, 0.0
    for v in x: a, b = v + c * a - b, a
    return math.sqrt(max(1e-30, a * a + b * b - c * a * b))
sys.exit(0 if max(level(hz) for hz in range(40, 1900, 15)) < level(2093.0) * 1e-3 else 1)
PY
then
  additive_why="the 200-partial saw at C7 aliases below its fundamental"
elif ! "./$build/wavelength" analyze "out/check/additive/$build/stems/02-bell.wav" --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
sys.exit(0 if not d["silent"] and d["stereo"]["width"] > 0 else 1)'; then
  additive_why="the inharmonic bell was silent or mono"
fi
if [ -n "$additive_why" ]; then echo "FAIL additive: $additive_why"; fail=1; else echo "ok   additive: a 200-partial saw at C7 in tune with nothing aliased below it, an inharmonic panned bank of sines"; fi
# arp: a C major chord held two beats at 1/16 up-and-down (top and bottom once) over two octaves plays C4 E4 G4 C5
# E5 G5 E5 C5 (read back from a MIDI export); a patch's Arpeggiator (make-test-exs.py's Test Arp: 1/16 up over two
# octaves, note length 50 %, swing 60, the grid note 127, rest, chord 64, note 100 tied over 2, note 64) plays by
# itself, "arp": false turns it off, and its preset plays on another track with one octave
mkdir -p out/check/arp
cat > out/check/arp/job.json <<'JOB'
{"tempo": 120, "stems": "none", "tracks": [{"name": "Arp", "plugin": "builtin:synth", "preset": "PL Pluck",
  "arp": {"rate": "1/16", "order": "updown", "variation": 2, "octaves": 2},
  "notes": [{"beat": 0, "dur": 2, "key": "C4"}, {"beat": 0, "dur": 2, "key": "E4"}, {"beat": 0, "dur": 2, "key": "G4"}]}]}
JOB
if "./$build/wavelength" export out/check/arp/job.json --out out/check/arp/arp.mid > /dev/null 2>&1 &&
   "./$build/wavelength" import out/check/arp/arp.mid --out out/check/arp/in > /dev/null 2>&1 &&
   python3 -c '
import json, sys
n = json.load(open("out/check/arp/in/job.json"))["tracks"][0]["notes"]
sys.exit(0 if [(x["beat"], x["key"]) for x in n] == [(i * 0.25, k) for i, k in enumerate([60, 64, 67, 72, 76, 79, 76, 72])] else 1)'; then
  arp_why=""
else arp_why="up-and-down over two octaves played the wrong notes"; fi
python3 scripts/make-test-exs.py out/check/arp/fx
cat > out/check/arp/patch.json <<'JOB'
{"tempo": 120, "stems": "none", "tracks": [
 {"name": "Auto", "plugin": "builtin:synth", "preset": "Test Arp", "notes": [{"beat": 0, "dur": 2, "key": "C4"}, {"beat": 0, "dur": 2, "key": "E4"}, {"beat": 0, "dur": 2, "key": "G4"}]},
 {"name": "Off", "plugin": "builtin:synth", "preset": "Test Arp", "arp": false, "notes": [{"beat": 0, "dur": 2, "key": "C4"}, {"beat": 0, "dur": 2, "key": "E4"}, {"beat": 0, "dur": 2, "key": "G4"}]},
 {"name": "Preset", "plugin": "builtin:synth", "preset": "PL Pluck", "arp": {"preset": "Test Arp", "octaves": 1}, "notes": [{"beat": 0, "dur": 2, "key": "C4"}, {"beat": 0, "dur": 2, "key": "E4"}, {"beat": 0, "dur": 2, "key": "G4"}]}]}
JOB
if [ -z "$arp_why" ] && ! { WAVELENGTH_LOGIC_PATCHES="$PWD/out/check/arp/fx/patches" WAVELENGTH_PLUGIN_SETTINGS="$PWD/out/check/arp/fx/settings" \
     "./$build/wavelength" export out/check/arp/patch.json --out out/check/arp/patch.mid --no-print > /dev/null 2>&1 &&
   "./$build/wavelength" import out/check/arp/patch.mid --out out/check/arp/patch-in > /dev/null 2>&1 &&
   python3 -c '
import json, sys
t = {x["name"]: [(x2["beat"], x2["key"], round(x2.get("vel", 0) * 127), x2["dur"]) for x2 in x["notes"]] for x in json.load(open("out/check/arp/patch-in/job.json"))["tracks"]}
grid = [(0.0, 60, 127, 0.125), (0.5, 60, 64, 0.125), (0.5, 64, 64, 0.125), (0.5, 67, 64, 0.125), (0.8, 64, 100, 0.25), (1.3, 67, 64, 0.125)]
ok = t["Auto"] == grid + [(1.5, 72, 127, 0.125)] and t["Preset"] == grid + [(1.5, 60, 127, 0.125)]
ok &= [(b, k) for b, k, v, d in t["Off"]] == [(0.0, 60), (0.0, 64), (0.0, 67)]
sys.exit(0 if ok else 1)'; }; then arp_why="the Test Arp patch or preset played the wrong arpeggio"; fi
if [ -n "$arp_why" ]; then echo "FAIL arp: $arp_why"; fail=1
else echo "ok   arp: up-and-down over two octaves on the grid; a patch's Arpeggiator grid, swing and chord step, its preset"; fi

# synth noise band: a noise oscillator with lowcut 2000 and highcut 4000 Hz centres between them; open noise sits far higher
mkdir -p out/check/noiseband
cat > out/check/noiseband/job.json <<'JOB'
{"tempo": 120, "leadIn": 0, "tail": 0, "stems": "24",
 "tracks": [{"name": "Band", "plugin": "builtin:synth", "synth": {"osc": [{"wave": "noise", "lowcut": 2000, "highcut": 4000}], "filter": {"type": "off"}}, "notes": [{"beat": 0, "dur": 2, "key": 60}]},
            {"name": "Open", "plugin": "builtin:synth", "synth": {"osc": [{"wave": "noise"}], "filter": {"type": "off"}}, "notes": [{"beat": 0, "dur": 2, "key": 60}]}]}
JOB
if "./$build/wavelength" render out/check/noiseband/job.json --out out/check/noiseband/out > /dev/null 2>&1 &&
   b=$("./$build/wavelength" analyze out/check/noiseband/out/stems/01-band.wav --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["spectrum"]["centroidHz"])') &&
   o=$("./$build/wavelength" analyze out/check/noiseband/out/stems/02-open.wav --json 2>/dev/null | python3 -c 'import json, sys; print(json.load(sys.stdin)["spectrum"]["centroidHz"])') &&
   python3 -c "import sys; sys.exit(0 if 2000 < $b < 4500 and $o > 6000 else 1)"; then
  echo "ok   noise band: a band-limited noise oscillator centres in its band"
else echo "FAIL noise band: the noise oscillator's band didn't shape it (centroid ${b:-?} Hz, open ${o:-?} Hz)"; fail=1; fi

# serve: the web UI answers, carries its token, refuses changes without it
# render --png and picture: real PNGs of the size the report names, a lane per track (taller with more tracks)
if ! "./$build/wavelength" render examples/synth-tour.json --out "out/check/picture/$build" --stems none --png --json 2>/dev/null | python3 -c '
import json, struct, sys
p = json.load(sys.stdin)["picture"]
d = open(p["file"], "rb").read(24)
sys.exit(0 if d[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", d[16:24]) == (p["width"], p["height"]) == (1400, p["height"]) and p["height"] > 900 else 1)' ||
   ! "./$build/wavelength" picture examples/hello.json --out "out/check/picture/$build/hello.png" --width 1000 --json | python3 -c '
import json, struct, sys
p = json.load(sys.stdin)
d = open(p["file"], "rb").read(24)
sys.exit(0 if struct.unpack(">II", d[16:24]) == (1000, p["height"]) and p["height"] > 200 else 1)'; then
  echo "FAIL picture: render --png / picture"; fail=1
else
  echo "ok   picture: render --png and the arrangement picture"
fi
# card: a contact sheet of built-in synth patches (a bad name fails alone), the pitch and a percussive patch read right
if "./$build/wavelength" card builtin:synth "LD Chip" "BA Pluck" "PD Nope" --out "out/check/picture/$build/cards.png" --json | python3 -c '
import json, struct, sys
d = json.load(sys.stdin)
p = {x.get("preset"): x for x in d["patches"]}
png = open(d["file"], "rb").read(24)
ok = (struct.unpack(">II", png[16:24]) == (d["width"], d["height"]) and p["LD Chip"]["ok"] and p["LD Chip"]["sounds"] == "C4"
      and p["LD Chip"]["transpose"] == 0 and any(f.startswith("percussive") for f in p["BA Pluck"]["flags"]) and not p["PD Nope"]["ok"])
sys.exit(0 if ok else 1)'; then
  echo "ok   card: patch contact sheet, sounding pitch, percussive flag, a bad preset fails alone"
else
  echo "FAIL card"; fail=1
fi
# fallback: a track whose plugin isn't installed plays its first available fallback, and the report says so first
mkdir -p out/check/fallback
cat > out/check/fallback/job.json <<'JOB'
{"tempo": 120, "stems": "none", "tail": 1,
 "tracks": [{"name": "Lead", "plugin": "No Such Plugin 9000", "preset": "X", "params": {"Cutoff": 0.3}, "articulations": {"long": 0},
             "fallback": [{"plugin": "Another Missing One"}, {"plugin": "builtin:synth", "preset": "LD Saw", "gain": -3}],
             "notes": [{"beat": 0, "dur": 2, "key": "C5", "vel": 0.8, "art": "long"}]}]}
JOB
if "./$build/wavelength" render out/check/fallback/job.json --out "out/check/fallback/$build" --json 2>/dev/null | python3 -c '
import json, sys
r = json.load(sys.stdin)
t = r["tracks"][0]
sys.exit(0 if r["ok"] and t["plugin"] == "builtin:synth" and not t["levels"]["silent"] and len(r["fallbacks"]) == 1 and r["warnings"][0] == r["fallbacks"][0] else 1)'; then
  echo "ok   fallback: a missing plugin plays its fallback"
else
  echo "FAIL fallback"; fail=1
fi
# note edits: edits.json beside the job moves, deletes and adds notes (keys as they sound, after transpose),
# and an edit whose note is gone is a warning on its track
mkdir -p out/check/edits
cat > out/check/edits/job.json <<'JOB'
{"tempo": 60, "leadIn": 0, "tail": 1, "stems": "float",
 "tracks": [{"name": "Keys", "plugin": "builtin:synth", "preset": "Init", "transpose": 12,
             "notes": [{"beat": 0, "dur": 1, "key": "A3", "vel": 0.8}, {"beat": 1, "dur": 1, "key": "C4", "vel": 0.8}]}]}
JOB
cat > out/check/edits/edits.json <<'JOB'
{"format": "wavelength.edits", "formatVersion": "1.0", "edits": [
  {"track": "Keys", "at": {"beat": 0, "key": 69}, "to": {"key": 81}},
  {"track": "Keys", "at": {"beat": 1, "key": 72}, "delete": true},
  {"track": "Keys", "add": {"beat": 2, "dur": 1, "key": 69, "vel": 0.8}},
  {"track": "Keys", "at": {"beat": 9, "key": 60}, "delete": true}]}
JOB
if "./$build/wavelength" render out/check/edits/job.json --out "out/check/edits/$build" --json 2>/dev/null | python3 -c '
import json, subprocess, sys
r = json.load(sys.stdin)
t = r["tracks"][0]
w, d = sys.argv[1], sys.argv[2]
def an(s, e): return json.loads(subprocess.run([w, "analyze", d + "/stems/01-keys.wav", "--start", str(s), "--end", str(e), "--json"], capture_output=True, text=True).stdout)
moved, gone, added = an(0.2, 0.7), an(1.2, 1.7), an(2.2, 2.7)
ok = r["ok"] and t["notes"] == 2 and sum("edits.json" in x for x in t.get("warnings", [])) == 1 and moved["pitch"]["note"] == "A5" and added["pitch"]["note"] == "A4" and gone["rmsDb"] < moved["rmsDb"] - 20
sys.exit(0 if ok else 1)' "./$build/wavelength" "out/check/edits/$build"; then
  echo "ok   note edits: edits.json moves, deletes and adds notes; a stale edit warns"
else
  echo "FAIL note edits: edits.json"; fail=1
fi
# render --loop: every file exactly the loop's length (8 bars at 124 BPM) with a smpl loop over all of it
if "./$build/wavelength" render examples/synth-tour.json --from 9 --to 17 --loop --stems 16 --out "out/check/loop/$build" --json > /dev/null 2>&1 &&
   python3 - "out/check/loop/$build" <<'PY'
import glob, struct, sys
want = round(32 * 60 / 124 * 48000)
ok = True
for f in [sys.argv[1] + "/mix.wav"] + glob.glob(sys.argv[1] + "/stems/*.wav"):
    d = open(f, "rb").read()
    p, frames, loop = 12, None, None
    while p + 8 <= len(d):
        cid, n = d[p:p + 4], struct.unpack("<I", d[p + 4:p + 8])[0]
        if cid == b"fmt ": bps = struct.unpack("<H", d[p + 22:p + 24])[0] // 8
        if cid == b"data": frames = n // (2 * bps)
        if cid == b"smpl": loop = struct.unpack("<II", d[p + 52:p + 60])
        p += 8 + n + (n & 1)
    ok &= frames == want and loop == (0, want - 1)
sys.exit(0 if ok else 1)
PY
then
  echo "ok   loop: render --loop files are the loop's length with a smpl loop"
else
  echo "FAIL loop"; fail=1
fi
# mcp: initialize, the tool list, the built-in guide, and a render that answers with its summary and picture
if python3 - "./$build/wavelength" "out/check/mcp/$build" <<'PY'
import base64, json, subprocess, sys
p = subprocess.Popen([sys.argv[1], "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
def call(i, method, params):
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": i, "method": method, "params": params}) + "\n"); p.stdin.flush()
    while True:
        m = json.loads(p.stdout.readline())
        if m.get("id") == i: return m
init = call(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "check", "version": "1"}})["result"]
names = [t["name"] for t in call(2, "tools/list", {})["result"]["tools"]]
guide = call(3, "tools/call", {"name": "guide", "arguments": {"section": "The loop"}})["result"]
r = call(4, "tools/call", {"name": "render", "arguments": {"job": "examples/sfz-tour.json", "out": sys.argv[2]}})["result"]
png = base64.b64decode(r["content"][1]["data"])[:8]
p.stdin.close(); p.wait(timeout=10)
ok = (init["protocolVersion"] == "2025-06-18" and {"guide", "render", "list_presets", "stage"} <= set(names) and "## The loop" in guide["content"][0]["text"]
      and not r["isError"] and r["content"][0]["text"].startswith("ok") and png == b"\x89PNG\r\n\x1a\n")
sys.exit(0 if ok else 1)
PY
then
  echo "ok   mcp: initialize, tools, guide, render with picture"
else
  echo "FAIL mcp"; fail=1
fi
# song format: save, render, change, undo/redo restore the job byte for byte; the package validates in both
# readers and unpacks to the same job; git export makes one commit per revision; every fixture is judged the
# same way by the engine and by the independent Python reader
w="$PWD/$build/wavelength"
rm -rf out/check/song out/check/song-unpacked out/check/song.wavelength out/check/song-git out/check/fixtures
mkdir -p out/check/song && cp examples/synth-tour.json out/check/song/job.json
if (cd out/check/song && "$w" save -m first >/dev/null && "$w" render job.json --out out >/dev/null 2>&1 &&
    python3 -c 'import json;j=json.load(open("job.json"));j["title"]="changed";json.dump(j,open("job.json","w"))' &&
    "$w" save -m second >/dev/null && "$w" undo >/dev/null && cmp -s job.json ../../../examples/synth-tour.json &&
    "$w" redo >/dev/null && grep -q '"changed"' job.json && "$w" diff r1 --json | grep -q '"ok": true' &&
    grep -q '"revision": 2' out/report.json && "$w" render job.json --png --keep >/dev/null 2>&1 && [ -s render/mix.mp3 ] && [ -s render/song.png ] &&
    python3 -c 'import json,sys;m=json.load(open("wavelength.json"));sys.exit(0 if m["render"]["revision"]==6 and m["render"]["job"].startswith("sha256:") else 1)' &&
    "$w" pack --out ../song.wavelength >/dev/null) &&
   python3 scripts/wavelength_song.py validate out/check/song.wavelength >/dev/null &&
   "$w" validate out/check/song.wavelength >/dev/null &&
   "$w" unpack out/check/song.wavelength --out out/check/song-unpacked >/dev/null && cmp -s out/check/song/job.json out/check/song-unpacked/job.json &&
   "$w" history out/check/song --to-git out/check/song-git >/dev/null &&
   [ "$(git -C out/check/song-git rev-list --count HEAD)" = "$(grep -c . out/check/song/history/log.jsonl)" ]; then
  echo "ok   song: save, render revision, undo/redo, diff, render --keep, pack, unpack, git export"
else
  echo "FAIL song format"; fail=1
fi
# render --cache: a second render reuses every track (the same mix), a mixer change renders none, a note change
# renders only that track
rm -rf out/check/cache && mkdir -p out/check/cache && cp examples/synth-tour.json out/check/cache/job.json
if (cd out/check/cache && python3 -c 'import json;j=json.load(open("job.json"));j["cacheCheck"]=__import__("time").time();json.dump(j,open("job.json","w"))' &&
    "$w" render job.json --cache --stems none --out a --json > a.json 2>/dev/null && "$w" render job.json --cache --stems none --out b --json > b.json 2>/dev/null &&
    python3 -c 'import json;j=json.load(open("job.json"));t=j["tracks"][0];t["gain"]=t.get("gain",0)-3;t["pan"]=0.4;json.dump(j,open("mix.json","w"))' &&
    "$w" render mix.json --cache --stems none --out c --json > c.json 2>/dev/null &&
    python3 -c 'import json;j=json.load(open("job.json"));n=j["tracks"][1]["notes"][0];n["key"]=(n["key"] if isinstance(n["key"],int) else 60)+1;json.dump(j,open("note.json","w"))' &&
    "$w" render note.json --cache --stems none --out d --json > d.json 2>/dev/null &&
    python3 - <<'PY'
import json, struct, sys
def mix(p):
    f = open(p + "/mix.wav", "rb").read(); i = 12
    while i < len(f):
        n = struct.unpack("<I", f[i + 4:i + 8])[0]
        if f[i:i + 4] == b"data": return struct.unpack("<%df" % (n // 4), f[i + 8:i + 8 + n])
        i += 8 + n + (n & 1)
cached = lambda p: [t.get("cached", False) for t in json.load(open(p))["tracks"]]
a, b, c, d = cached("a.json"), cached("b.json"), cached("c.json"), cached("d.json")
same = max(abs(x - y) for x, y in zip(mix("a"), mix("b"))) < 1e-5
ok = not any(a) and all(b) and all(c) and same and d[1] is False and all(x for k, x in enumerate(d) if k != 1)
sys.exit(0 if ok else 1)
PY
); then
  echo "ok   render --cache: reused tracks give the same mix, a mixer change renders none, a note change renders one"
else
  echo "FAIL render --cache"; fail=1
fi
# purge: a render's mix.wav, stems and preview cache go, an MP3 is made first, the song's other files stay,
# and a second run finds nothing
chmod -R u+w out/check/purge 2>/dev/null || true; rm -rf out/check/purge && mkdir -p out/check/purge/media && cp examples/synth-tour.json out/check/purge/job.json
if (cd out/check/purge && "$w" render job.json --out out --stems 16 >/dev/null 2>&1 && cp out/mix.wav media/keep.wav &&
    mkdir -p out/preview/x && cp out/mix.wav out/report.json out/preview/x/ &&
    touch -t 202601010000 out/mix.wav out/report.json out/stems/*.wav out/preview/x/* &&
    mkdir kept && cp -Rp out/mix.wav out/report.json kept/ && chmod -R a-w kept &&
    "$w" purge . --json | python3 -c 'import json,sys; r=json.load(sys.stdin); sys.exit(0 if r["mp3sMade"] == 1 and r["renders"][0]["previews"] == 1 else 1)' &&
    [ ! -e out/mix.wav ] && [ ! -e out/stems ] && [ ! -e out/preview ] && [ -s out/mix.mp3 ] && [ -s out/report.json ] && [ -s media/keep.wav ] &&
    [ -s kept/mix.wav ] && grep -q '"purged"' out/report.json && "$w" purge . | grep -q "nothing to purge"); then
  echo "ok   purge: WAVs, stems and previews go, an MP3 first, other files and read-only folders stay, a second run finds nothing"
else
  echo "FAIL purge"; fail=1
fi
python3 scripts/make-song-fixtures.py out/check/fixtures >/dev/null
fixtures_ok=1
for f in out/check/fixtures/good out/check/fixtures/*.wavelength; do
  want=0; case "$(basename "$f")" in bad-*) want=1;; esac
  c=0; "$w" validate "$f" >/dev/null 2>&1 || c=1
  p=0; python3 scripts/wavelength_song.py validate "$f" >/dev/null 2>&1 || p=1
  [ $c = $want ] && [ $p = $want ] || { echo "     $(basename "$f"): engine $c, python $p, want $want"; fixtures_ok=0; }
done
if [ $fixtures_ok = 1 ]; then echo "ok   song fixtures: engine and Python reader agree"; else echo "FAIL song fixtures"; fail=1; fi
# a package is untrusted: rendering one whose job writes outside its output folder is refused; undo takes
# back a file that was added (a restore makes the tracked files exactly the target's)
rm -rf out/check/undo-add && cp -R out/check/song out/check/undo-add && rm -rf out/check/undo-add/history out/check/undo-add/wavelength.json
if ! "$w" render out/check/fixtures/bad-deliver.wavelength --out out/check/escape >/dev/null 2>&1 &&
   (cd out/check/undo-add && "$w" save -m base >/dev/null && mkdir -p media && echo x > media/added.txt &&
    python3 -c 'import json;m=json.load(open("wavelength.json"));m["files"].append({"path":"media/added.txt","role":"media"});json.dump(m,open("wavelength.json","w"))' &&
    "$w" save -m added >/dev/null && "$w" undo >/dev/null && [ ! -e media/added.txt ] && "$w" redo >/dev/null && [ -e media/added.txt ]); then
  echo "ok   package render stays inside its song; undo removes an added file"
else
  echo "FAIL package safety or undo of an added file"; fail=1
fi
# migrate: a pre-format folder (absolute paths inside it, a sample in scratch out/) gets a manifest,
# relative paths (the scratch sample copied into media/), a revision, and still renders; both readers accept it
up="$PWD/out/check/migrate"
rm -rf "$up" && mkdir -p "$up/sounds" "$up/out"
cp "out/check/synth-tour/$build/mix.wav" "$up/sounds/hit.wav" && cp "out/check/synth-tour/$build/mix.wav" "$up/out/tail.wav"
python3 - "$up" <<'PY'
import json, sys
d = sys.argv[1]
note = lambda b: {"beat": b, "key": 60, "dur": 1, "vel": 0.8}
json.dump({"tempo": 120, "tracks": [
    {"name": "Hit", "plugin": "builtin:sampler", "sampler": {"sample": d + "/sounds/hit.wav", "oneShot": True}, "notes": [note(0)]},
    {"name": "Tail", "plugin": "builtin:sampler", "sampler": {"sample": d + "/out/tail.wav", "oneShot": True}, "notes": [note(2)]}]},
    open(d + "/job.json", "w"))
PY
if "$w" migrate "$up" --license CC0-1.0 --author Tester >/dev/null &&
   python3 -c 'import json,sys;j=json.load(open(sys.argv[1]+"/job.json"));m=json.load(open(sys.argv[1]+"/wavelength.json"));s=[t["sampler"]["sample"] for t in j["tracks"]];sys.exit(0 if s==["sounds/hit.wav","media/tail.wav"] and m["license"]=="CC0-1.0" and m["authors"][0]["name"]=="Tester" else 1)' "$up" &&
   [ -s "$up/media/tail.wav" ] && [ "$("$w" history "$up" --json | python3 -c 'import json,sys;print(len(json.load(sys.stdin)["revisions"]))')" = 1 ] &&
   "$w" validate "$up" >/dev/null && python3 scripts/wavelength_song.py validate "$up" >/dev/null &&
   "$w" render "$up/job.json" --out "$up/out" >/dev/null 2>&1; then
  echo "ok   migrate: manifest, relative paths, scratch file into media/, revision, renders"
else
  echo "FAIL migrate"; fail=1
fi
# fallbacks: every CLAP track of hello gets a built-in stand-in, and render --fallbacks plays them all
rm -rf out/check/fallbacks && mkdir -p out/check/fallbacks && cp examples/hello.json out/check/fallbacks/job.json
if "$w" fallbacks out/check/fallbacks/job.json --suggest --write --no-measure >/dev/null &&
   "$w" render out/check/fallbacks/job.json --fallbacks --out out/check/fallbacks/out --json 2>/dev/null |
   python3 -c 'import json,sys;r=json.load(sys.stdin);n=len(json.load(open("examples/hello.json"))["tracks"]);sys.exit(0 if len(r.get("fallbacks",[]))==n and all(t["lufs"]>-40 for t in r["tracks"]) else 1)'; then
  echo "ok   fallbacks: suggested for every plugin track, render --fallbacks plays them"
else
  echo "FAIL fallbacks"; fail=1
fi
# help: the grouped list, one command's details by `help <command>` and by `--help`, plain in a pipe
# (grep reads the whole output: `grep -q` stops at the first match, and pipefail counts the writer's SIGPIPE)
if "./$build/wavelength" help | grep "^Make music" >/dev/null && "./$build/wavelength" help render | grep -- "--loop" >/dev/null &&
   "./$build/wavelength" render --help | sed -n 1p | grep "^wavelength render" >/dev/null && "./$build/wavelength" help all | grep "^Usage:" >/dev/null &&
   ! "./$build/wavelength" help | grep -q $'\x1b'; then
  echo "ok   help: grouped list, per command, plain in a pipe"
else
  echo "FAIL help"; fail=1
fi
# a free port each run: two checkouts (or agents) running the check at once must not answer for each other
sport=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
mkdir -p out/check/serve-songs/demo out/check/serve-songs/kitmap && cp examples/hello.json out/check/serve-songs/demo/job.json
# a sampler kit given as a key -> file map (not a library name) once made /api/song throw: an empty 500
cat > out/check/serve-songs/kitmap/job.json <<'JOB'
{"tempo": 120, "tracks": [{"name": "Kit", "plugin": "builtin:sampler", "sampler": {"kit": {"36": "kick.wav", "38": "snare.wav"}},
  "notes": [{"beat": 0, "dur": 0.5, "key": 36}]}]}
JOB
"./$build/wavelength" serve out/check/serve-songs --port $sport 2>/dev/null &
spid=$!
for _ in $(seq 50); do curl -s -o /dev/null http://127.0.0.1:$sport/ && break; sleep 0.1; done   # up to 5 s on a busy machine
serve_why=""
page=$(curl -s -w '\nHTTP %{http_code}' http://127.0.0.1:$sport/)
echo "$page" | grep -q 'wavelength-token' || serve_why="the page has no token ($(echo "$page" | tail -1); $(echo "$page" | head -c 160 | tr '\n' ' '))"
[ -z "$serve_why" ] && { curl -s http://127.0.0.1:$sport/api/songs | grep -q '"demo"' || serve_why="the song list lacks demo: $(curl -s http://127.0.0.1:$sport/api/songs | head -c 200)"; }
[ -z "$serve_why" ] && { code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{}' "http://127.0.0.1:$sport/api/review?song=demo"); [ "$code" = "403" ] || serve_why="a POST without the token got $code, not 403"; }
[ -z "$serve_why" ] && { curl -s "http://127.0.0.1:$sport/api/song?song=kitmap" | grep -q '"custom kit"' || serve_why="a song with a kit map: $(curl -s -w ' HTTP %{http_code}' "http://127.0.0.1:$sport/api/song?song=kitmap" | head -c 200)"; }
if [ -z "$serve_why" ]; then
  echo "ok   serve: UI, song list, token check, a song with a kit map"
else
  echo "FAIL serve: $serve_why"; fail=1
fi
kill $spid 2>/dev/null
exit $fail
