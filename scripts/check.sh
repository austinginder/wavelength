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
ok = (init["protocolVersion"] == "2025-06-18" and {"guide", "render", "list_presets"} <= set(names) and "## The loop" in guide["content"][0]["text"]
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
chmod -R u+w out/check/purge 2>/dev/null; rm -rf out/check/purge && mkdir -p out/check/purge/media && cp examples/synth-tour.json out/check/purge/job.json
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
