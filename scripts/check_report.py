#!/usr/bin/env python3
"""Reads a `wavelength render --json` report on stdin; fails on silent tracks or a clipping mix,
and on the expectations listed below for examples that exercise a feature."""
import json
import sys


def arrangement_tour(d):
    """examples/arrangement-tour.json: rides, clip, level-matched saturate, vibrato, a skipped
    stem, a loudness target and the arrangement checks on a build that does its job."""
    bad = []
    mix = d["mix"]
    if abs(mix["lufs"] - (-14)) > 0.3:
        bad.append(f"loudness target -14 missed: {mix['lufs']}")
    if "lra" not in mix:
        bad.append("no mix.lra")
    if d.get("warnings"):
        bad.append("warnings: " + " | ".join(w[:60] for w in d["warnings"]))
    if d.get("dropouts"):
        bad.append(f"dropouts: {d['dropouts']}")
    sections = {s["name"]: s for s in d["sections"]}
    if any("preMasterLufs" not in s for s in d["sections"]):
        bad.append("sections without preMasterLufs")
    if sections.get("Drop", {}).get("change", 0) < 3:
        bad.append(f"drop only {sections.get('Drop', {}).get('change')} dB over the build")
    tracks = {t["name"]: t for t in d["tracks"]}
    if tracks["Bass"].get("file"):
        bad.append('"stem": false still wrote a stem')
    for t in d["tracks"]:
        if t.get("warnings"):
            bad.append(f"{t['name']}: " + " | ".join(w[:60] for w in t["warnings"]))
    return bad


EXPECT = {"arrangement-tour": arrangement_tour}

d = json.load(sys.stdin)
name = sys.argv[1]
bad = [f"silent track {t['name']}" for t in d["tracks"] if t["levels"]["silent"]]
if d["mix"]["levels"]["peakDb"] > 0:
    bad.append("mix clips")
if name in EXPECT:
    bad += EXPECT[name](d)
line = f"{name}: {d['seconds']} s in {d['renderSeconds']} s, mix {d['mix']['lufs']} LUFS"
print(("FAIL " if bad else "ok   ") + line + ("  " + "; ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
