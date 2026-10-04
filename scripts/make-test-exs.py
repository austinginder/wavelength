#!/usr/bin/env python3
"""Write a tiny Logic Sampler instrument and a GarageBand patch that carries it, for scripts/check.sh:
  <dir>/Test Sine.wav               a 440 Hz sine, 2 s, 48 kHz mono 16-bit
  <dir>/Test Instrument.exs         one zone over every key, rooted at A4 (69), playing the WAV, with
                                    instrument parameters: volume -6 dB, release 64 (0.65 s), sustain 127
  <dir>/patches/Test Patch.patch/   a channel strip (#Root.cst) whose Sampler slot stores that instrument
  <dir>/patches/Test Synth.patch/   a channel strip on Retro Synth (an instrument only GarageBand plays)
Usage: make-test-exs.py <dir>"""
import math, os, struct, sys

out = sys.argv[1]
os.makedirs(os.path.join(out, 'patches', 'Test Patch.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Synth.patch'), exist_ok=True)
wav = os.path.abspath(os.path.join(out, 'Test Sine.wav'))
rate, n = 48000, 96000
pcm = struct.pack('<%dh' % n, *[int(16000 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(n)])
with open(wav, 'wb') as f:
    f.write(b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVE')
    f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16))
    f.write(b'data' + struct.pack('<I', len(pcm)) + pcm)

def chunk(kind, index, name, body):
    # 84-byte header: type in bits 24-27 of the signature, data size, index, flags, "TBOS", 64-byte name
    return struct.pack('<IIII', (kind << 24) | 0x0101, len(body), index, 0) + b'TBOS' + name.encode()[:63].ljust(64, b'\0') + body

zone = bytearray(104)
zone[0] = 0             # options: pitch tracks the key, no one-shot
zone[1] = 69            # root key
zone[6], zone[7] = 0, 127
zone[9], zone[10] = 0, 127
struct.pack_into('<II', zone, 12, 0, n)
struct.pack_into('<ii', zone, 88, -1, 0)   # no group, sample 0
sample = bytearray(600)
struct.pack_into('<I', sample, 4, n)
d = os.path.dirname(wav).encode()
sample[80:80 + len(d)] = d
sample[336:336 + len(b'Test Sine.wav')] = b'Test Sine.wav'
params = {0x07: -6, 0x55: 64, 0x51: 127, 0x5a: -60}
ids = bytes(list(params) + [0] * (100 - len(params)))
vals = struct.pack('<100h', *(list(params.values()) + [0] * (100 - len(params))))
exs = (chunk(0, 0, 'Test Instrument', bytes(40)) + chunk(1, 0, 'Zone', bytes(zone)) +
       chunk(3, 0, 'Test Sine.wav', bytes(sample)) + chunk(4, 0, 'Parameters', struct.pack('<I', 100) + ids + vals))
with open(os.path.join(out, 'Test Instrument.exs'), 'wb') as f:
    f.write(exs)

def record(slot, plugin, maker, data):
    # a channel strip slot: 36-byte "UCuA" header (payload size at +0x1c), payload with the plugin's name at
    # +120 and its maker at +132, then the plugin's data
    payload = bytearray(140)
    payload[120:120 + len(plugin)] = plugin.encode()
    payload[132:136] = maker
    payload += data
    head = bytearray(36)
    head[0:4] = b'UCuA'
    struct.pack_into('<HH', head, 4, 2, 14)
    struct.pack_into('<H', head, 0x12, slot)
    struct.pack_into('<I', head, 0x1c, len(payload))
    return bytes(head) + bytes(payload)

def strip(path, records):
    with open(path, 'wb') as f:
        f.write(b'OCuA' + bytes(220) + b''.join(records))

strip(os.path.join(out, 'patches', 'Test Patch.patch', '#Root.cst'),
      [record(0, '', b'\0\0\0\0', bytes(8)), record(3, 'Sampler', b'MELC', bytes(120) + exs), record(4, 'Channel EQ', b'GAME', bytes(64))])
strip(os.path.join(out, 'patches', 'Test Synth.patch', '#Root.cst'), [record(3, 'Retro Synth', b'GAME', bytes(200))])
