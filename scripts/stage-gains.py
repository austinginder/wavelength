#!/usr/bin/env python3
"""Gain staging for a song folder: set each track's fader so it hits a target loudness.

usage: scripts/stage-gains.py <song folder> [render folder, default <song>/out]

Reads <song>/targets.json (track name -> target LUFS after the fader; "bus:<Name>" for a bus)
and the last render's <song>/out/report.json (stem LUFS, measured before the fader; a bus's
LUFS after its effects, before its fader), and writes <song>/gains.json with gain = target -
LUFS, bus gains under "bus:<Name>". Rebuild the job and render again.
"""
import json
import os
import sys

song = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
targets = json.load(open(os.path.join(song, 'targets.json')))
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(song, 'out')
report = json.load(open(os.path.join(out, 'report.json')))
path = os.path.join(song, 'gains.json')
gains = json.load(open(path)) if os.path.exists(path) else {}
for t in report['tracks']:
    if t['name'] in targets and t['lufs'] > -90:
        gains[t['name']] = round(targets[t['name']] - t['lufs'], 1)
        print(f"{t['name']:18} stem {t['lufs']:6.1f} LUFS -> gain {gains[t['name']]:+6.1f} dB (target {targets[t['name']]})")
for b in report.get('buses', []):
    key = 'bus:' + b['name']
    if key in targets and b['lufs'] > -90:
        gains[key] = round(targets[key] - b['lufs'], 1)
        print(f"{key:18} bus  {b['lufs']:6.1f} LUFS -> gain {gains[key]:+6.1f} dB (target {targets[key]})")
json.dump(gains, open(path, 'w'), indent=1)
