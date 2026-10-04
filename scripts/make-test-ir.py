#!/usr/bin/env python3
"""Write a stereo impulse response and a click for scripts/check.sh (48 kHz, 16-bit):
  <dir>/test-room.wav   0.25 s of decaying noise, different in each channel (deterministic)
  <dir>/click.wav       10 ms of silence, one full-scale sample, 10 ms of silence (mono)
Usage: make-test-ir.py <dir>"""
import math, os, struct, sys

out = sys.argv[1]
rate = 48000

def wav(path, channels, frames):
    data = b''.join(struct.pack('<%dh' % channels, *[max(-32767, min(32767, int(v * 32767))) for v in f]) for f in frames)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * 2 * channels, 2 * channels, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)

seed = 12345
def rnd():
    global seed
    seed = (seed * 1103515245 + 12345) & 0x7fffffff
    return seed / 0x3fffffff - 1.0

n = int(0.25 * rate)
wav(os.path.join(out, 'test-room.wav'), 2, [(0.8 * rnd() * math.exp(-i / (0.05 * rate)), 0.8 * rnd() * math.exp(-i / (0.03 * rate))) for i in range(n)])
wav(os.path.join(out, 'click.wav'), 1, [(0.0,)] * 480 + [(1.0,)] + [(0.0,)] * 480)
