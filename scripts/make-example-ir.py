#!/usr/bin/env python3
"""Write examples/ir/small-room.wav: a synthetic small room for examples/convolve-tour.json (24 kHz stereo 16-bit,
0.9 s): a few early reflections in the first 40 ms, then decorrelated noise decaying to -60 dB in 0.8 s and
growing darker as it fades (a one-pole low-pass closing from 9 kHz to 1.5 kHz). Deterministic; CC0.
Usage: make-example-ir.py [out.wav]"""
import math, struct, sys

out = sys.argv[1] if len(sys.argv) > 1 else 'examples/ir/small-room.wav'
rate, seconds, rt60 = 24000, 0.9, 0.8
n = int(rate * seconds)
seed = 2026
def rnd():
    global seed
    seed = (seed * 1103515245 + 12345) & 0x7fffffff
    return seed / 0x3fffffff - 1.0

chans = []
for c in range(2):
    x = [0.0] * n
    x[0] = 0.9
    for ms, g in ((7.1, 0.55), (11.3, -0.42), (17.9, 0.36), (23.4, -0.3), (31.0, 0.24), (38.6, -0.2)):
        x[int((ms + 1.7 * c) * rate / 1000)] += g
    z = 0.0
    for i in range(int(0.004 * rate), n):
        t = i / rate
        cutoff = 9000 * (1500 / 9000) ** min(1.0, t / seconds)
        a = 1 - math.exp(-2 * math.pi * cutoff / rate)
        z += a * (rnd() - z)
        onset = min(1.0, t / 0.02)
        x[i] += 0.5 * onset * z * 10 ** (-3 * t / rt60)
    fade = int(0.05 * rate)
    for i in range(fade):
        x[n - 1 - i] *= i / fade
    chans.append(x)
peak = max(abs(v) for ch in chans for v in ch)
data = b''.join(struct.pack('<hh', int(chans[0][i] / peak * 30000), int(chans[1][i] / peak * 30000)) for i in range(n))
with open(out, 'wb') as f:
    f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
    f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate, rate * 4, 4, 16))
    f.write(b'data' + struct.pack('<I', len(data)) + data)
