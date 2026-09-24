#!/usr/bin/env python3
"""Reads a `wavelength render --json` report on stdin; fails on silent tracks or a clipping mix."""
import json
import sys

d = json.load(sys.stdin)
name = sys.argv[1]
bad = [f"silent track {t['name']}" for t in d["tracks"] if t["levels"]["silent"]]
if d["mix"]["levels"]["peakDb"] > 0:
    bad.append("mix clips")
line = f"{name}: {d['seconds']} s in {d['renderSeconds']} s, mix {d['mix']['lufs']} LUFS"
print(("FAIL " if bad else "ok   ") + line + ("  " + "; ".join(bad) if bad else ""))
sys.exit(1 if bad else 0)
