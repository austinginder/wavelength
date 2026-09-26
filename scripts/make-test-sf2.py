#!/usr/bin/env python3
"""Write a tiny SoundFont for scripts/check.sh: one preset (bank 0, program 0) playing a looped
440 Hz sine rooted at A4 (key 69), with a velocity modulator on its decay. Usage: make-test-sf2.py OUT.sf2"""
import math, struct, sys

def chunk(cid, data):
    return cid + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')

def lst(kind, *chunks):
    return chunk(b'LIST', kind + b''.join(chunks))

def name20(s):
    return s.encode()[:20].ljust(20, b'\0')

rate, n = 44000, 2000                      # 100 samples per 440 Hz cycle: the loop closes exactly
pcm = [int(20000 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(n)] + [0] * 46
smpl = struct.pack('<%dh' % len(pcm), *pcm)

phdr = name20('Test Sine') + struct.pack('<HHHIII', 0, 0, 0, 0, 0, 0) + name20('EOP') + struct.pack('<HHHIII', 0, 0, 1, 0, 0, 0)
pbag = struct.pack('<HH', 0, 0) + struct.pack('<HH', 1, 0)
pmod = b'\0' * 10
pgen = struct.pack('<Hh', 41, 0) + struct.pack('<Hh', 0, 0)                      # instrument 0
inst = name20('Sine') + struct.pack('<H', 0) + name20('EOI') + struct.pack('<H', 1)
ibag = struct.pack('<HH', 0, 0) + struct.pack('<HH', 4, 1)
imod = struct.pack('<HHhHH', 0x0002, 36, 1200, 0, 0) + b'\0' * 10               # velocity -> decay time
igen = (struct.pack('<Hh', 54, 1) +                                              # sampleModes: loop
        struct.pack('<Hh', 36, 0) +                                              # decayVolEnv 1 s
        struct.pack('<Hh', 37, 200) +                                            # sustain 20 dB down
        struct.pack('<Hh', 53, 0) +                                              # sampleID 0
        struct.pack('<Hh', 0, 0))
shdr = (name20('Sine A4') + struct.pack('<IIIIIBbHH', 0, n, 1000, 1900, rate, 69, 0, 0, 1) +
        name20('EOS') + struct.pack('<IIIIIBbHH', 0, 0, 0, 0, 0, 0, 0, 0, 0))
body = b'sfbk' + lst(b'INFO', chunk(b'ifil', struct.pack('<HH', 2, 1)), chunk(b'isng', b'EMU8000\0'), chunk(b'INAM', b'Wavelength test\0')) \
    + lst(b'sdta', chunk(b'smpl', smpl)) \
    + lst(b'pdta', chunk(b'phdr', phdr), chunk(b'pbag', pbag), chunk(b'pmod', pmod), chunk(b'pgen', pgen), chunk(b'inst', inst),
          chunk(b'ibag', ibag), chunk(b'imod', imod), chunk(b'igen', igen), chunk(b'shdr', shdr))
open(sys.argv[1], 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)
