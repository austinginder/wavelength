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
# serve: the web UI answers, carries its token, refuses changes without it
mkdir -p out/check/serve-songs/demo && cp examples/hello.json out/check/serve-songs/demo/job.json
"./$build/wavelength" serve out/check/serve-songs --port 7499 2>/dev/null &
spid=$!
sleep 1
if curl -s http://127.0.0.1:7499/ | grep -q 'wavelength-token' && curl -s http://127.0.0.1:7499/api/songs | grep -q '"demo"' &&
   [ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -d '{}' 'http://127.0.0.1:7499/api/review?song=demo')" = "403" ]; then
  echo "ok   serve: UI, song list, token check"
else
  echo "FAIL serve"; fail=1
fi
kill $spid 2>/dev/null
exit $fail
