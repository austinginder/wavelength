#!/usr/bin/env python3
"""Write a tiny Logic Sampler instrument and a GarageBand patch that carries it, for scripts/check.sh:
  <dir>/Test Sine.wav               a 440 Hz sine, 2 s, 48 kHz mono 16-bit
  <dir>/Test Instrument.exs         one zone over every key, rooted at A4 (69), playing the WAV, with
                                    instrument parameters: volume -6 dB, release 64 (0.65 s), sustain 127
  <dir>/patches/Test Patch.patch/   a channel strip (#Root.cst) whose Sampler slot stores that instrument, then a
                                    Channel EQ (low cut 200 Hz 12 dB/oct, master -3 dB)
  <dir>/patches/Test Synth.patch/   a channel strip on Alchemy that stores no preset text (only GarageBand can play it)
  <dir>/patches/Test Alchemy.patch/ a channel strip on Alchemy with a small preset written here: source A a saw tuned
                                    +12 semitones, an AHDSR on the amp, main filter 1 a low-pass
  <dir>/patches/Test Alchemy Arp.patch/  the same with Alchemy's arpeggiator on: 1/16 up over 2 octaves, note length 0.5
  <dir>/patches/Test Beat GB.patch/ a channel strip on Ultrabeat with the settings "Machine Kit.pst" (its kit folder
                                    comes from make-test-sample-folders.py)
  <dir>/patches/Test Retro.patch/   a channel strip on Retro Synth: Analog mode, one saw, filter off, transposed +12
  <dir>/patches/Test Organ.patch/   a channel strip on Vintage B3: only the upper 8' drawbar out (a sine at the note),
                                    its 234 preset-key ints before the values as Vintage B3 stores them
  <dir>/patches/Test Arp.patch/     Test Retro's Retro Synth after an Arpeggiator: 1/16 up over 2 octaves, note length
                                    50 %, swing 60, a 6-step grid (note 127, rest, chord 64, note 100 tied over 2, note 64)
  <dir>/settings/Arpeggiator/Test Arp.pst   the same Arpeggiator settings as a preset
Usage: make-test-exs.py <dir>"""
import math, os, plistlib, struct, sys

out = sys.argv[1]
os.makedirs(os.path.join(out, 'patches', 'Test Patch.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Synth.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Alchemy.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Alchemy Arp.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Beat GB.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Retro.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Organ.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'patches', 'Test Arp.patch'), exist_ok=True)
os.makedirs(os.path.join(out, 'settings', 'Arpeggiator'), exist_ok=True)
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

def record(slot, plugin, maker, data, preset=''):
    # a channel strip slot: 36-byte "UCuA" header (payload size at +0x1c), payload with the plugin's name at
    # +120 and its maker at +132, then the plugin's data
    payload = bytearray(140)
    payload[14:14 + len(preset)] = preset.encode()
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

def settings(plugin_id, values):
    # a plug-in's saved settings after the 32-byte prefix: the TSPP block (u32 size, u16 version, u8 big-endian flag,
    # u8, u32 count, "GAME" "TSPP", u32 plug-in id, count float32 values, the first one reserved)
    vals = [0.0] + values
    block = struct.pack('<IHBBI', 24 + 4 * len(vals), 1, 0, 0, len(vals)) + b'GAMETSPP' + struct.pack('<I', plugin_id) + struct.pack('<%df' % len(vals), *vals)
    return struct.pack('<I', plugin_id) + bytes(8) + struct.pack('<I', len(block)) + bytes(16) + block

# Channel EQ: a 12 dB/oct low cut at 200 Hz (#0 on, #1 Hz, #2 slope 2 = 12 dB/oct, #3 Q) and -3 dB master gain (#32)
ceq = [0.0] * 51
ceq[0:4] = [1, 200, 2, 0.71]
ceq[32] = -3
strip(os.path.join(out, 'patches', 'Test Patch.patch', '#Root.cst'),
      [record(0, '', b'\0\0\0\0', bytes(8)), record(3, 'Sampler', b'MELC', bytes(120) + exs), record(4, 'Channel EQ', b'GAME', settings(236, ceq))])
strip(os.path.join(out, 'patches', 'Test Synth.patch', '#Root.cst'), [record(3, 'Alchemy', b'GAME', bytes(200))])
strip(os.path.join(out, 'patches', 'Test Beat GB.patch', '#Root.cst'), [record(3, 'Ultrabeat', b'GAME', bytes(200), 'Machine Kit.pst')])

def alchemy_text(arp):
    # Alchemy's preset text: "<section>" lines and "Key = value" lines, CRLF. A modulatable value is "v smooth n" and n
    # slots follow (Type, Id, ModMap, Depth; Type 2 = an AHDSR, Depth 1.0 = +100%). Values are 0..1: coarse tune
    # 0.5 + st / 96, cutoff 1.0 = 20 kHz over 128/12 octaves, AHDSR times 20 s x v^4, volumes linear.
    v = lambda x: '%.4f 0.0000 0' % x
    lines = ['<alchemypreset>', 'Version = 166', 'Name = Test Alchemy', '', '<perform>', 'PerVol = 0.7940', '',
             '<filters>', 'F1On = 1', 'F1Type = 0', 'F1Cut = ' + v(0.8), 'F1Res = ' + v(0.1), 'F1Drive = ' + v(0), 'F1FxMix = ' + v(0),
             'F1EfxRot = 0', 'F2On = 0', 'F2FxMix = ' + v(0), 'FSerMix = ' + v(0), '',
             '<master>', 'Volume = ' + v(0.63), 'Amp = 1.0000 0.0000 1', '  Type = 2', '  Id = 0', '  ModMap = 0', '  Depth = 1.0000 0.0000 0',
             'TuneCrs = ' + v(0.5), 'TuneFine = ' + v(0.5), 'NmVoices = 8', 'PlayMode = 0', 'Porto = ' + v(0), '']
    if arp:   # five blocks (all sources, then A-D): Mode 1/7 = up, Rate 0.6837 synced = 1/16, Octaves 1/3 = two, Sustain 0.5
        lines += ['<arp>']
        for b in range(5):
            lines += ['ArpRate = 0.6837', 'ArpSustn = ' + v(0.5), 'ArpMode = ' + (v(1 / 7) if b == 0 else v(0)), 'ArpOct = ' + v(1 / 3), 'ArpSync = 1']
        lines += ['']
    lines += ['<morph>', 'MorAll = 2', 'MorAllX = ' + v(0), 'MorAllY = ' + v(1), '']
    for k, letter in enumerate('ABCD'):   # source A plays a saw a octave up; B-D are off
        lines += ['<source>', 'SOn = %d' % (k == 0), 'SAmp = ' + v(1), 'STunCrs = ' + v(0.5 + 12 / 96), 'STunFin = ' + v(0.5),
                  'SPan = ' + v(0.5), 'SKeyTrk = 2', 'SStereo = 0', 'SVAOn = 1', 'SVAShpe = Alchemy/Libraries/WaveOsc/Basic/Saw.raw',
                  'SVAVol = ' + v(1), 'SVASym = ' + v(0.5), 'SVASync = ' + v(0), 'SVANOsc = ' + v(0), 'SVAUnis = ' + v(0), 'SVAPhas = ' + v(0),
                  'SNsOn = 0', 'SAdOn = 0', 'SSpOn = 0', 'SGrOn = 0', 'SFile = ', 'SF1On = 0', 'SF2On = 0', 'SF3On = 0', 'SFiPar = 0',
                  'SFilMix = ' + v(0), '']
    lines += ['<ahdsr>', 'AhdAttck = ' + v(0.2), 'AhdHold = ' + v(0), 'AhdDecay = ' + v(0.3), 'AhdSustn = ' + v(0.8), 'AhdRelse = ' + v(0.3), '',
              '<extensions>', 'A-VAWideUnison = 0', '']
    # the plug-in data: a header (u32 313 here), then the text and a byte that isn't text
    return struct.pack('<I', 313) + bytes(30) + '\r\n'.join(lines).encode() + b'\r\n\0\0\0\0'

for name, arp in (('Test Alchemy', False), ('Test Alchemy Arp', True)):
    strip(os.path.join(out, 'patches', name + '.patch', '#Root.cst'), [record(3, 'Alchemy', b'GAME', alchemy_text(arp), name + '.acp')])
# Retro Synth (plug-in id 279): parameter #n; unused ones hold 1e30
retro = [1e30] * 902
for n, v in {1: 8, 2: 0, 3: 12, 4: 0, 5: -6, 201: 0, 209: 0, 301: 1, 303: 1, 401: 0, 802: 1, 803: 100, 804: 1, 805: 100, 806: 0}.items():
    retro[n] = v
strip(os.path.join(out, 'patches', 'Test Retro.patch', '#Root.cst'), [record(3, 'Retro Synth', b'GAME', settings(279, retro))])
# Vintage B3 (plug-in id 216): 234 int32 preset-key registrations come first, then its 169 values (parameter #n = value n+1)
b3 = [0.0] * 169
b3[1 + 13] = 8        # upper 8' drawbar full; the other drawbars, percussion, vibrato, click, rotor and effects stay off
b3[1 + 104] = 1       # expression
b3[1 + 105] = -6      # volume dB
block = struct.pack('<IHBBI', 24 + 936 + 4 * len(b3), 2, 0, 0, len(b3)) + b'GAMETSPP' + struct.pack('<I', 216) + bytes(936) + struct.pack('<%df' % len(b3), *b3)
organ = struct.pack('<I', 216) + bytes(8) + struct.pack('<I', len(block)) + bytes(16) + block
strip(os.path.join(out, 'patches', 'Test Organ.patch', '#Root.cst'), [record(3, 'Vintage B3', b'GAME', organ)])
# Arpeggiator (plug-in id 300, a MIDI effect: flags 0x02000000 in the prefix): parameter #n = value n+1, then chunks
# (tag stored byte-reversed, size counting the 8-byte header): "L13a" empty, "UGCD" the grid as a binary property list
arp = [1e30] * 64
for n, v in {0: 1, 2: 0, 4: 1.016, 5: 1, 6: 0, 7: 1, 9: 2, 10: 0, 11: 1, 13: 6, 14: 0, 15: 50, 16: 0, 18: 60, 19: 80, 20: 100,
             21: 0, 23: 0, 27: 3, 37: 0}.items():
    arp[n] = v
grid = plistlib.dumps({'Version': 1, 'UUID': 'TEST-GRID', 'ActiveSteps': 6, 'Steps': [
    {'Type': 'Note', 'Velocity': 127, 'Length': 1.0}, {'Type': 'Rest', 'Velocity': 80, 'Length': 1.0},
    {'Type': 'Chord', 'Velocity': 64, 'Length': 1.0}, {'Type': 'Note', 'Velocity': 100, 'Length': 2.0},
    {'Type': 'Note', 'Velocity': 64, 'Length': 1.0}]}, fmt=plistlib.FMT_BINARY)
grid += bytes(-len(grid) % 4)
chunks = b'a31L' + struct.pack('<I', 8) + b'DCGU' + struct.pack('<I', 8 + len(grid)) + grid
vals = [0.0] + arp
block = struct.pack('<IHBBI', 24 + 4 * len(vals) + len(chunks), 1, 0, 0, len(vals)) + b'GAMETSPP' + struct.pack('<I', 300) + struct.pack('<%df' % len(vals), *vals) + chunks
with open(os.path.join(out, 'settings', 'Arpeggiator', 'Test Arp.pst'), 'wb') as f:
    f.write(block)
arpeggiator = struct.pack('<III', 300, 0, 0x02000000) + struct.pack('<I', len(block)) + bytes(16) + block
strip(os.path.join(out, 'patches', 'Test Arp.patch', '#Root.cst'),
      [record(0, 'Arpeggiator', b'GAME', arpeggiator), record(3, 'Retro Synth', b'GAME', settings(279, retro))])
