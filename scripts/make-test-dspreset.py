#!/usr/bin/env python3
"""Write a tiny DecentSampler instrument for scripts/check.sh:
  <dir>/Samples/Sine A3.wav       a 220 Hz sine (A3), 2 s, 48 kHz mono 16-bit
  <dir>/Samples/Sine A5.wav       an 880 Hz sine (A5), the same
  <dir>/Test Instrument.dspreset  one group of two samples split across the keyboard (A3 plays keys 0-68, A5 keys
                                  69-127), a 0.6 s release, a reverb (wetLevel 0.5, roomSize 0.8) and a Volume knob at
                                  0.5 bound to the instrument's volume (so it plays 6 dB under its samples' level); its
                                  group volume is written twice, as some libraries do (the last one, 0 dB, counts)
Usage: make-test-dspreset.py <dir>"""
import math, os, struct, sys

out = sys.argv[1]
os.makedirs(os.path.join(out, 'Samples'), exist_ok=True)
rate, n = 48000, 96000
for name, hz in (('Sine A3.wav', 220.0), ('Sine A5.wav', 880.0)):
    pcm = struct.pack('<%dh' % n, *[int(16000 * math.sin(2 * math.pi * hz * i / rate)) for i in range(n)])
    with open(os.path.join(out, 'Samples', name), 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b'data' + struct.pack('<I', len(pcm)) + pcm)
with open(os.path.join(out, 'Test Instrument.dspreset'), 'w') as f:
    f.write('''<?xml version="1.0" encoding="UTF-8"?>
<DecentSampler minVersion="1.0.0">
  <ui width="812" height="375">
    <tab name="main">
      <labeled-knob x="20" y="20" label="Volume" type="float" minValue="0" maxValue="1" value="0.5">
        <binding type="amp" level="instrument" position="0" parameter="AMP_VOLUME"/>
      </labeled-knob>
    </tab>
  </ui>
  <groups attack="0" release="0.6">
    <group volume="-12dB" volume="0dB">
      <sample path="Samples\\Sine A3.wav" rootNote="57" loNote="0" hiNote="68"/>
      <sample path="Samples/Sine A5.wav" rootNote="81" loNote="69" hiNote="127"/>
    </group>
  </groups>
  <effects>
    <effect type="reverb" wetLevel="0.5" roomSize="0.8" damping="0.3"/>
  </effects>
</DecentSampler>
''')
