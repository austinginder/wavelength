#!/usr/bin/env python3
"""Release test for one built wavelength binary, deeper than build-release.sh's smoke render.

Usage: release-test.py WORKDIR LABEL RUNNER...
  e.g. release-test.py /tmp/wt linux-x86_64 ./wavelength-linux-x86_64/wavelength
       release-test.py /tmp/wt windows wine64 wavelength-windows-x86_64/wavelength.exe

Runs in WORKDIR (created, emptied) with relative paths only, so the same commands work under Wine.
Needs the repository's examples/ and scripts/make-song-fixtures.py next to this script. Standard
library only. Checks: a built-in-only render with its picture; the song workflow (save, render,
change, undo, redo, diff, pack, unpack, validate); every fixture judged as the spec says and a
refused unpack writing nothing; a package of plugin and library tracks rendering with its fallbacks
(forced with --fallbacks where the plugins exist); a package whose job writes outside refused; the
built-in docs; kit; and the MCP server over stdin/stdout. Exit status 1 when any check fails.
"""
import json, os, shutil, subprocess, sys, time

if len(sys.argv) < 4:
    sys.exit(__doc__)
WORK, LABEL, RUN = os.path.abspath(sys.argv[1]), sys.argv[2], sys.argv[3:]
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# with the plugins installed (a Mac that builds releases), force the stand-ins; elsewhere they play because the plugins are missing
FORCE = ["--fallbacks"] if LABEL.startswith("macos") else []
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
results = []


def wl(*args, timeout=900):
    t = time.time()
    p = subprocess.run(RUN + list(args), cwd=WORK, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr, time.time() - t


def js(text):
    try:
        return json.loads(text)
    except Exception:
        return None


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("ok   " if ok else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)


def read(rel):
    return open(os.path.join(WORK, rel), "rb").read()


def write_json(rel, data):
    os.makedirs(os.path.dirname(os.path.join(WORK, rel)) or WORK, exist_ok=True)
    with open(os.path.join(WORK, rel), "w") as f:
        json.dump(data, f)


try:
    shutil.copytree(os.path.join(REPO, "examples"), os.path.join(WORK, "examples"))
    subprocess.run([sys.executable, os.path.join(REPO, "scripts", "make-song-fixtures.py"), os.path.join(WORK, "fixtures")],
                   check=True, capture_output=True)

    rc, out, err, _ = wl("version", "--json")
    check("version", rc == 0 and (js(out) or {}).get("songFormat") == "1.0", out.strip().replace("\n", " ")[:80])

    rc, out, err, dt = wl("render", "examples/synth-tour.json", "--out", "r1", "--png", "--stems", "none", "--json")
    r = js(out) or {}
    png = read("r1/song.png")[:8] if os.path.exists(os.path.join(WORK, "r1", "song.png")) else b""
    check("render built-in instruments with the picture", rc == 0 and r.get("ok") and png == b"\x89PNG\r\n\x1a\n",
          "mix %.1f LUFS in %.0fs" % (r.get("mix", {}).get("lufs", -99), dt))

    # the song workflow
    shutil.copy(os.path.join(WORK, "examples", "synth-tour.json"), os.path.join(WORK, "job.json"))
    os.makedirs(os.path.join(WORK, "song"))
    shutil.move(os.path.join(WORK, "job.json"), os.path.join(WORK, "song", "job.json"))
    original = read("song/job.json")
    ok = [wl("save", "song", "-m", "first", "--json")[0] == 0,
          wl("render", "song/job.json", "--out", "song/out", "--stems", "none", "--json")[0] == 0]
    job = json.loads(original)
    job["title"] = "changed"
    write_json("song/job.json", job)
    ok += [wl("save", "song", "-m", "second", "--json")[0] == 0, wl("undo", "song", "--json")[0] == 0]
    undone = read("song/job.json") == original
    ok.append(wl("redo", "song", "--json")[0] == 0)
    rc, out, _, _ = wl("diff", "song", "r1", "--json")
    diff_ok = rc == 0 and (js(out) or {}).get("ok")
    revs = len((js(wl("history", "song", "--json")[1]) or {}).get("revisions", []))
    packed = wl("pack", "song", "--out", "song.wavelength", "--json")[0] == 0
    unpacked = wl("unpack", "song.wavelength", "--out", "song2", "--json")[0] == 0
    valid = wl("validate", "song.wavelength")[0] == 0 and wl("validate", "song2")[0] == 0
    same = unpacked and read("song2/job.json") == read("song/job.json")
    check("song: save, render, undo, redo, diff, pack, unpack, validate",
          all(ok) and undone and diff_ok and revs == 5 and packed and unpacked and valid and same,
          "steps=%s undone=%s diff=%s revs=%d packed=%s unpacked=%s valid=%s same=%s" % (ok, undone, diff_ok, revs, packed, unpacked, valid, same))

    # fixtures, and a refused unpack that writes nothing
    wrong = []
    for f in sorted(os.listdir(os.path.join(WORK, "fixtures"))):
        if not (f.endswith(".wavelength") or f == "good"):
            continue
        refused = wl("validate", "fixtures/" + f)[0] != 0
        if refused != f.startswith("bad-"):
            wrong.append(f)
    rc = wl("unpack", "fixtures/bad-traversal.wavelength", "--out", "fx-unpacked")[0]
    escaped = os.path.exists(os.path.join(WORK, "evil.txt")) or os.path.exists(os.path.dirname(WORK) + os.sep + "evil.txt")
    check("fixtures judged as the spec says", not wrong and rc != 0 and not escaped, "wrong=%s escaped=%s" % (wrong, escaped))

    # a package whose job writes outside its output folder is refused
    rc, out, _, _ = wl("render", "fixtures/bad-deliver.wavelength", "--out", "escape", "--json")
    check("a package that writes outside is refused", rc != 0 and "outside" in out)

    # plugin and library tracks: a package that plays everywhere through its fallbacks
    hello = json.loads(read("examples/hello.json"))
    note = lambda b: {"beat": b, "key": 60, "dur": 1, "vel": 0.9}
    hello["tracks"].append({"name": "Snare", "plugin": "builtin:sampler", "notes": [note(0), note(2)],
                            "sampler": {"sample": "lib:Legend 909/Snare Legend 909 01 accent.wav", "root": 60, "oneShot": True}})
    os.makedirs(os.path.join(WORK, "portable"))
    write_json("portable/job.json", hello)
    steps = [wl("fallbacks", "portable", "--suggest", "--write", "--no-measure", "--json")[0] == 0,
             wl("save", "portable", "-m", "with fallbacks", "--json")[0] == 0,
             wl("pack", "portable", "--out", "portable.wavelength", "--json")[0] == 0]
    rc, out, err, dt = wl("render", "portable.wavelength", "--out", "portable-out", "--stems", "none", "--json", *FORCE)
    r = js(out) or {}
    silent = [t["name"] for t in r.get("tracks", []) if t.get("lufs", -120) < -60]
    check("plugin and library tracks play their fallbacks", all(steps) and rc == 0 and r.get("ok") and not silent
          and len(r.get("fallbacks", [])) == len(hello["tracks"]),
          "steps=%s fallbacks=%d/%d silent=%s in %.0fs %s" % (steps, len(r.get("fallbacks", [])), len(hello["tracks"]), silent, dt,
                                                           "" if rc == 0 else (out + err)[-200:]))

    rc, out, _, _ = wl("docs", "song-format", "--section", "The package")
    check("docs built in", rc == 0 and "## 5. The package" in out)
    rc, out, _, _ = wl("kit", "--json")
    check("kit lists its instruments", rc == 0 and len((js(out) or {}).get("kit", [])) > 0)

    # MCP over stdin/stdout: initialize, the tools, the guide, a tool that runs the engine again
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "release-test", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "guide", "arguments": {"section": "The loop"}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "history", "arguments": {"song": "song"}}},
    ]
    p = subprocess.Popen(RUN + ["mcp"], cwd=WORK, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    got = {}
    for m in msgs:
        p.stdin.write((json.dumps(m) + "\n").encode())
        p.stdin.flush()
        deadline = time.time() + 120
        while "id" in m and m["id"] not in got and time.time() < deadline:
            line = p.stdout.readline()
            if not line:
                break
            d = js(line.decode("utf-8", "replace"))
            if isinstance(d, dict) and "id" in d:
                got[d["id"]] = d
    p.stdin.close()
    try:
        p.wait(timeout=30)
    except Exception:
        p.kill()
    tools = [t["name"] for t in got.get(2, {}).get("result", {}).get("tools", [])]
    guide, hist = got.get(3, {}).get("result", {}), got.get(4, {}).get("result", {})
    check("mcp over stdin/stdout", got.get(1, {}).get("result", {}).get("protocolVersion") == "2025-06-18"
          and {"render", "save", "history", "pack"} <= set(tools) and not guide.get("isError") and "The loop" in json.dumps(guide)
          and not hist.get("isError") and "revisions" in json.dumps(hist), "%d tools" % len(tools))
except Exception as e:
    check("the test itself", False, repr(e))
finally:
    failed = [n for n, ok in results if not ok]
    print("%s: %d/%d passed%s" % (LABEL, len(results) - len(failed), len(results), "" if not failed else ", FAILED: " + ", ".join(failed)), flush=True)
    sys.exit(1 if failed else 0)
