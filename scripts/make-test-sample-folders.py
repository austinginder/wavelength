#!/usr/bin/env python3
"""Write sample folders for scripts/check.sh (48 kHz mono 16-bit sines and clicks):
  <dir>/Sci Notes/Sci Notes C3.wav, C4, C5     named as they sound (C4 = 60, as Alchemy's synths)
  <dir>/Logic Notes/Logic Notes C2.wav, C3, C4  named an octave low (C3 = 60, as Logic and Alchemy's vocals): "C3" sounds C4
  <dir>/Machine Kit/MK_BD1.wav, MK_SD1, MK_HH1, MK_HHo, MK_Clap   a drum machine kit by its abbreviations
Usage: make-test-sample-folders.py <dir>"""
import math, os, struct, sys

out, rate = sys.argv[1], 48000

def wav(path, samples):
    data = struct.pack('<%dh' % len(samples), *[int(v * 20000) for v in samples])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b'data' + struct.pack('<I', len(data)) + data)

def sine(key, seconds=1.5):
    hz = 440 * 2 ** ((key - 69) / 12)
    return [math.sin(2 * math.pi * hz * i / rate) * min(1, (len(range(int(rate * seconds))) - i) / 2000) for i in range(int(rate * seconds))]

for name, key in (('C3', 48), ('C4', 60), ('C5', 72)):
    wav(os.path.join(out, 'Sci Notes', 'Sci Notes %s.wav' % name), sine(key))
for name, key in (('C2', 48), ('C3', 60), ('C4', 72)):
    wav(os.path.join(out, 'Logic Notes', 'Logic Notes %s.wav' % name), sine(key))
for name in ('MK_BD1', 'MK_SD1', 'MK_HH1', 'MK_HHo', 'MK_Clap'):
    wav(os.path.join(out, 'Machine Kit', name + '.wav'), [1.0] + [0.0] * 2400)
