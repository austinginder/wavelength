#!/usr/bin/env python3
"""Write a tiny Apple Loop for scripts/check.sh: a CAF (16-bit PCM, so it reads on any system) of 4 beats at
120 BPM, a C4 sine, tagged the way GarageBand tags its loops (beat count 4, key C major, category Keyboards),
with its notes in a 'midi' chunk (C4 on beat 0, E4 on beat 1). A second loop, "Test Drummer.caf", carries the same
notes a whole loop late (beats 4 and 5), as Drummer loops slice a longer performance.
Usage: make-test-apple-loop.py "OUT/Test Loop.caf\""""
import math, os, struct, sys

rate, beats, bpm = 48000, 4, 120
n = int(rate * beats * 60 / bpm)
pcm = struct.pack('>%dh' % n, *[int(12000 * math.sin(2 * math.pi * 261.6256 * i / rate)) for i in range(n)])

def chunk(kind, body):
    return kind + struct.pack('>q', len(body)) + body

# desc: rate (f64), 'lpcm', flags 0 (big-endian integer), 2 bytes per packet, 1 frame per packet, 1 channel, 16 bits
desc = struct.pack('>d4sIIIII', rate, b'lpcm', 0, 2, 1, 1, 16)
tags = [('category', 'Keyboards'), ('subcategory', 'Synthesizer'), ('genre', 'Electronic'), ('key signature', 'C'),
        ('key type', 'major'), ('time signature', '4/4'), ('beat count', str(beats)), ('descriptors', 'Single,Clean')]
uuid = bytes.fromhex('29819273B5BF4AEFB78D62D1EF90BB2C') + struct.pack('>I', len(tags)) + b''.join(
    k.encode() + b'\0' + v.encode() + b'\0' for k, v in tags)

def vlq(v):
    out = [v & 0x7f]
    while v > 0x7f:
        v >>= 7
        out.insert(0, (v & 0x7f) | 0x80)
    return bytes(out)

ppq = 480
def smf(lead):
    events = [(lead, b'\x90\x3c\x64'), (ppq, b'\x80\x3c\x40'), (0, b'\x90\x40\x50'), (ppq, b'\x80\x40\x40'), (0, b'\xff\x2f\x00')]
    trk = b''.join(vlq(d) + e for d, e in events)
    return b'MThd' + struct.pack('>IHHH', 6, 0, 1, ppq) + b'MTrk' + struct.pack('>I', len(trk)) + trk

for path, lead in ((sys.argv[1], 0), (os.path.join(os.path.dirname(sys.argv[1]), 'Test Drummer.caf'), beats * ppq)):
    with open(path, 'wb') as f:
        f.write(b'caff' + struct.pack('>HH', 1, 0))
        f.write(chunk(b'desc', desc))
        f.write(chunk(b'uuid', uuid))
        f.write(chunk(b'midi', smf(lead)))
        f.write(chunk(b'data', struct.pack('>I', 0) + pcm))
