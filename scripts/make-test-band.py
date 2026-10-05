#!/usr/bin/env python3
"""Write a tiny GarageBand project for scripts/check.sh, made up here (no Apple data):
  <dir>/Test Song.band/Alternatives/000/MetaData.plist   a binary property list: 100 BPM, 4/4, D minor, 48 kHz
  <dir>/Test Song.band/Alternatives/000/ProjectData      the song (chunks, below)
  <dir>/Test Song.band/Resources/ProjectInformation.plist  an XML property list naming what saved it
The song: one software instrument track "Synth" (fader 80 = -2.05 dB, pan 80 = 0.25 right) whose channel strip plays a
Retro Synth-like record (Analog mode, one saw, filter off, transposed +12: a C4 sounds C5) and sends to a bus "Echo"
(level 45 = -12.04 dB) that carries an Echo (1/8 note, 40 % repeat, wet only). Two MIDI regions: at bar 1, C4 for 2 beats,
E4 and G4 (a high-resolution velocity of 16385 = 0.5) for a beat each; at bar 3, trimmed to one bar, A4 for a beat, a CC1
and a C5 past the trim (cut); at bar 5, a one-beat region holding E4 (half a beat) looped over three beats; at bar 7, a
region with its first half beat trimmed off (an A3 there is hidden), Transpose +2 and Time Quantize 1/16, holding a B3
730 ticks in (stored as 600 + a 130-tick offset), which plays C#4 a quarter beat in. An audio track "Loop": at bar 9, a
region of Media/Audio Files/Test Loop.wav (a 1 s, 48 kHz sine) from 0.1 s for 0.5 s, looped over two and a half
passes. Cycle bars 1-2.
Usage: make-test-band.py <dir>"""
import math, os, plistlib, struct, sys

out = os.path.join(sys.argv[1], 'Test Song.band')
alt = os.path.join(out, 'Alternatives', '000')
os.makedirs(alt, exist_ok=True)
os.makedirs(os.path.join(out, 'Resources'), exist_ok=True)
with open(os.path.join(alt, 'MetaData.plist'), 'wb') as f:
    plistlib.dump({'BeatsPerMinute': 100.0, 'SongSignatureNumerator': 4, 'SongSignatureDenominator': 4, 'SongKey': 'D',
                   'SongGenderKey': 'minor', 'SampleRate': 48000, 'NumberOfTracks': 1, 'AudioFiles': [], 'SamplerInstrumentsFiles': []},
                  f, fmt=plistlib.FMT_BINARY)
with open(os.path.join(out, 'Resources', 'ProjectInformation.plist'), 'wb') as f:
    plistlib.dump({'LastSavedFrom': 'make-test-band.py'}, f, fmt=plistlib.FMT_XML)

BAR1 = 38400        # 960 ticks a beat; bar 1 is ten 4/4 bars after tick 0, and a region stored at P shows from P + 3840
END = 0x3fffffff


def chunk(tag, cls, oid, payload, ref=0, sub=0):
    # 36-byte header: tag stored reversed, u16 version, u16 class, u16 0, u32 object id, u32 reference, u32 sub-index,
    # 6 bytes, u32 payload size, u32 0
    return (tag[::-1].encode() + struct.pack('<HHH', 1, cls, 0) + struct.pack('<III', oid, ref, sub) + bytes([2, 0, 0, 0, 2, 0]) +
            struct.pack('<II', len(payload), 0) + payload)


def event(kind, pos, b8=b'\0' * 8, ext=()):
    # a 16-byte record (type, 3 bytes, u32 position, 8 bytes) and its extension records (byte 7 >= 0x80)
    return bytes([kind, 0x40 if kind & 0xf0 == 0x90 else 0, 0, 0]) + struct.pack('<I', pos) + b8 + b''.join(ext)


def ext(b0=b'', u12=0, mark=0x89):
    r = bytearray(16)
    r[0:len(b0)] = b0
    r[7] = mark
    struct.pack_into('<I', r, 12, u12)
    return bytes(r)


def events(*evs):
    return b''.join(evs) + bytes([0xf1, 0, 0, 0]) + struct.pack('<I', END) + bytes(8)   # the end record


def seq(cls, oid, name, evs, traks=b''):
    # a sequence: its name (u16 length at +0x10), its tracks, then its events under the same class and id
    return chunk('MSeq', cls, oid, bytes(0x10) + struct.pack('<H', len(name)) + name.encode()) + traks + chunk('EvSq', cls, oid, evs)


def region(cls, oid, name, evs, trim=0, length=0, quantize=0):
    # a region's sequence with its settings after the name (padded to an even length): +4 its left trim in ticks,
    # +0x3c its length (one pass), +0x48 its Time Quantize (-6 = 1/16)
    head = bytes(0x10) + struct.pack('<H', len(name)) + name.encode() + bytes(len(name) & 1)
    settings = bytearray(0x60)
    struct.pack_into('<I', settings, 4, trim)
    struct.pack_into('<I', settings, 0x3c, length)
    struct.pack_into('<h', settings, 0x48, quantize)
    return chunk('MSeq', cls, oid, head + bytes(settings)) + chunk('EvSq', cls, oid, evs)


def note(pos, key, vel, length, hires=0, offset=0):
    b8 = bytearray(8)
    if hires:
        struct.pack_into('<H', b8, 2, hires)   # +10..11: velocity / 32767
    else:
        b8[3] = vel                            # +11: velocity
    b8[4] = key                                # +12: the key
    return event(0x90, pos, bytes(b8), [ext(struct.pack('<IH', 0, offset), u12=length)])   # length; +4 ticks past pos


def envi(oid, name, channel):
    # an environment object: name length at +0x9e, the name at +0xa0, then (at an even offset) its channel's number + 1
    pl = bytearray(0xa0)
    struct.pack_into('<H', pl, 0x9e, len(name))
    pl += name.encode() + bytes(len(name) & 1) + struct.pack('<HH', channel + 1, 0xff)
    return chunk('Envi', 0x14, oid, bytes(pl))


def channel(number, kind, index, name, volume, pan, uuid):
    # a channel strip object: +4 type, +6 index, +0x3c name, the fader (8.24 on 0-127, 90 = 0 dB) exactly at +0x74 and
    # as a coarse copy at +0x52, +0x58 flags (bit 0 solo), +0x59 pan (0-127, 64 = centre), +0x5a state (bit 0 muted),
    # then its UUID after ff at +0xd4
    pl = bytearray(261)
    pl[4] = kind
    struct.pack_into('<H', pl, 6, index)
    pl[0x3c:0x3c + len(name)] = name.encode()
    struct.pack_into('<I', pl, 0x52, volume << 24)
    struct.pack_into('<I', pl, 0x74, volume << 24)
    pl[0x59] = pan
    pl[0xd4] = 0xff
    pl[0xd5:0xd5 + 16] = uuid
    return chunk('AuCO', 0x0e, 4 * number, bytes(pl), ref=number)


def record(number, slot, payload):
    return chunk('AuCU', 0x0e, 4 * number, payload, ref=number, sub=slot)


def plugin(order, name, flags, plugin_id, values, preset='#default.pst'):
    # a plug-in record: +6 insert order, +14 settings name, +120 plug-in name, +132 maker, then a 32-byte prefix (flags
    # at payload +148) and the settings block (u32 size, u16 version, u8 big-endian flag, u8, u32 count, "GAME" "TSPP",
    # u32 plug-in id, count float32 values, the first one reserved)
    pl = bytearray(140)
    struct.pack_into('<H', pl, 6, order)
    pl[14:14 + len(preset)] = preset.encode()
    pl[120:120 + len(name)] = name.encode()
    pl[132:136] = b'GAME'
    vals = [0.0] + values
    block = struct.pack('<IHBBI', 24 + 4 * len(vals), 1, 0, 0, len(vals)) + b'GAMETSPP' + struct.pack('<I', plugin_id) + struct.pack('<%df' % len(vals), *vals)
    return bytes(pl) + struct.pack('<III', plugin_id, 0, flags) + struct.pack('<I', len(block)) + bytes(16) + block


def send(index, code, level, target):
    # a send: +6 index, +0x14 destination code, +0x18 level (8.24 on the fader's 0-127 scale), +0x3c the destination's UUID
    pl = bytearray(76)
    struct.pack_into('<H', pl, 6, index)
    pl[0x14], pl[0x16] = code, 2
    struct.pack_into('<I', pl, 0x18, level << 24)
    pl[0x3c:0x4c] = target
    return bytes(pl)


INST, BUS, AUD = 0x29, 0x4d, 0x31                 # channel numbers
LOOP, FILE = 0x10c, 0x200                         # the audio track's object, its file's id
TRACK, ECHO, MASTER, REGION1, REGION2, REGION3, REGION4 = 0x100, 0x104, 0x108, 8, 12, 16, 20
bus_uuid = bytes([0xd5]) + bytes(range(1, 16))
retro = [1e30] * 902                               # parameter #n; unused ones hold 1e30
for n, v in {1: 8, 2: 0, 3: 12, 4: 0, 5: -6, 201: 0, 209: 0, 301: 1, 303: 1, 401: 0, 802: 1, 803: 100, 804: 1, 805: 100, 806: 0}.items():
    retro[n] = v
echo = [0.0] * 21
echo[16:21] = [7, 40, 0, 0, 100]                  # Time 1/8, Repeat 40 %, Color 0, Dry 0 %, Wet 100 %

body = b''
body += seq(3, 0, 'Tempo', events(event(0x60, BAR1, ext=[ext(struct.pack('<I', 100 * 10000), mark=0x88)])))
body += seq(1, 0, 'Signature', events(event(0x30, 0, bytes([0, 0, 0, 2, 4, 0, 0, 0]), [ext(mark=0x88)])))   # +11 log2 4, +12 4
body += seq(0x16, 0, 'Locators', events(event(0x10, BAR1 + 2 * 3840 - 1, ext=[ext(u12=BAR1)])))             # cycle bars 1-2
body += envi(TRACK, 'Synth', INST) + envi(ECHO, 'Echo', BUS) + envi(MASTER, 'Master', -1)
trak = lambda row, kind, obj: chunk('Trak', 0x17, 4, struct.pack('<HHHHI', kind, 0, 0, 0, obj) + bytes(46), sub=row)
# a region's placement: main +13 bit 0x10 looped; extension 1: the track object and the length (a looped one's whole
# span), 2: its sequence, 0x8a: its parameters (+5 Transpose)
placement = lambda pos, length, region, looped=False, transpose=0: event(
    0x20, pos, bytes([0, 0, 0, 0, 0, 0x16 if looped else 0x04, 0, 0]),
    ext=[ext(struct.pack('<I', TRACK), length), ext(struct.pack('<I', region), mark=0x88), ext(bytes([0, 6, 0, 0, 0, transpose & 0xff]), mark=0x8a)])
# an audio region's placement (0x24): looped; extension 1: the track and the whole span (2000 ticks = 2.5 passes of the
# region's 0.5 s at 100 BPM), 0xbc: +8 the region's index, +12 its file
audio_placement = lambda pos, length, track, file: event(
    0x24, pos, bytes([0, 0, 0, 0, 0, 0x16, 0, 0]),
    ext=[ext(struct.pack('<I', track), length), ext(struct.pack('<III', 0, 0, 0), u12=file, mark=0xbc)])
body += seq(0x17, 4, 'Test Song', events(placement(BAR1 - 3840, END, REGION1), placement(BAR1 + 3840, 3840, REGION2),
                                         placement(BAR1 + 3 * 3840, 3 * 960, REGION3, looped=True),
                                         placement(BAR1 + 5 * 3840, END, REGION4, transpose=2),
                                         audio_placement(BAR1 + 7 * 3840, 2000, LOOP, FILE)),
            trak(0, 1, TRACK) + trak(1, 1, LOOP) + trak(2, 3, MASTER))
body += seq(0x17, REGION1, 'Synth', events(note(BAR1, 60, 100, 1920), note(BAR1 + 1920, 64, 80, 960), note(BAR1 + 2880, 67, 0, 960, hires=16385)))
body += seq(0x17, REGION2, 'Synth 2', events(note(BAR1, 69, 90, 960), event(0xb0, BAR1 + 480, bytes([0, 0, 0, 64, 1, 0, 0, 0])),
                                             note(BAR1 + 3840, 72, 90, 960)))
body += region(0x17, REGION3, 'Synth 3', events(note(BAR1, 64, 90, 480)), length=960)
body += region(0x17, REGION4, 'Synth 4', events(note(BAR1, 57, 90, 480), note(BAR1 + 600, 59, 90, 480, offset=130)), trim=480, length=1920,
               quantize=-6)
body += channel(INST, 0x43, 0, ' Inst 1', 80, 80, bytes([0xd5]) + bytes(15))
body += record(INST, 0, send(0, 0, 45, bus_uuid))
body += record(INST, 1, plugin(0, 'Retro Synth', 0x08000000, 279, retro))
body += channel(BUS, 0x45, 0, ' Bus 1', 90, 64, bus_uuid)
body += record(BUS, 0, plugin(1, 'Echo', 0, 0, echo))
# the audio track: its file in Media/Audio Files, the file's record (AuFl: UTF-16 name, then at fixed distances from
# its end the folder, frames, rate, channels, bits) and the region's (AuRg: +6 start and +0x16 length in frames, its name)
media = os.path.join(out, 'Media', 'Audio Files')
os.makedirs(media, exist_ok=True)
rate, frames = 48000, 48000
pcm = struct.pack('<%dh' % frames, *[int(12000 * math.sin(2 * math.pi * 220 * i / rate)) for i in range(frames)])
with open(os.path.join(media, 'Test Loop.wav'), 'wb') as f:
    f.write(b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVE' + b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16) +
            b'data' + struct.pack('<I', len(pcm)) + pcm)
fname = 'Test Loop.wav'
fl = bytearray(10 + 2 * len(fname) + 0x1f0)
struct.pack_into('<H', fl, 8, len(fname))
fl[10:10 + 2 * len(fname)] = fname.encode('utf-16le')
b = 10 + 2 * len(fname)
fl[b + 0x8a:b + 0x8a + 11] = b'Audio Files'
struct.pack_into('<I', fl, b + 0x1d4, frames)
struct.pack_into('<H', fl, b + 0x1dc, rate)
fl[b + 0x1e0], fl[b + 0x1e2] = 1, 16
rg = bytearray(0x4c + 10 + 16)
struct.pack_into('<I', rg, 6, 4800)
struct.pack_into('<I', rg, 0x16, 24000)
struct.pack_into('<H', rg, 0x4a, 10)
rg[0x4c:0x56] = b'Loop Clip1'
body += envi(LOOP, 'Loop', AUD)
body += chunk('AuFl', 0x05, FILE, bytes(fl)) + chunk('AuRg', 0x05, FILE, bytes(rg), ref=0)
body += channel(AUD, 0x40, 0, ' Audio 1', 90, 64, bytes([0xd5, 9]) + bytes(14))
with open(os.path.join(alt, 'ProjectData'), 'wb') as f:
    f.write(b'#G\xc0\xab' + struct.pack('<HHIII', 0x09d0, 3, 4, 0x00080001, len(body)) + struct.pack('<I', 0) + body)
