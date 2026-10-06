#!/usr/bin/env python3
"""Write a tiny GarageBand project for scripts/check.sh, made up here (no Apple data):
  <dir>/Test Song.band/Alternatives/000/MetaData.plist   a binary property list: 100 BPM (90 from bar 13), 4/4, D minor, 48 kHz
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
passes; at bar 11, an Apple Loop tagged G major (Media/Audio Files/Test Apple Loop.caf) with Transpose +2, gain -6 dB
and Reverse Playback (in D minor it moves G -> D, -5, so it plays -3). Its automation: volume 0 dB at bar 1 to -12.04 dB
at bar 3 (a step GarageBand stores between them is a decoy), pan from the middle at bar 1 to hard left at bar 5. The
master fades out from bar 1 to silence at bar 3 (its output channel's volume automation).
Cycle bars 1-2.
The audio track also sends to the Echo bus, its send automated from silence at bar 1 to 0 dB at bar 3. The Synth
track's channel maps Smart Control knob 3 ("Filter") onto Retro Synth's Cutoff by Env (#407, steps 100-150 of 0-200),
automated from 0 at bar 1 to full at bar 3 (envelope depth 0 to half: 4.98 octaves).
Usage: make-test-band.py <dir> [3]   (3: the same song in 3/4; GarageBand still puts bar 1 at tick 38400)"""
import math, os, plistlib, struct, sys
from plistlib import UID

METER = int(sys.argv[2]) if len(sys.argv) > 2 else 4
out = os.path.join(sys.argv[1], 'Test Song.band')
alt = os.path.join(out, 'Alternatives', '000')
os.makedirs(alt, exist_ok=True)
os.makedirs(os.path.join(out, 'Resources'), exist_ok=True)
with open(os.path.join(alt, 'MetaData.plist'), 'wb') as f:
    plistlib.dump({'BeatsPerMinute': 100.0, 'SongSignatureNumerator': METER, 'SongSignatureDenominator': 4, 'SongKey': 'D',
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


def region(cls, oid, name, evs, trim=0, length=0, quantize=0, strength=100):
    # a region's sequence with its settings after the name (padded to an even length): +4 its left trim in ticks,
    # +0x3c its length (one pass), +0x48 its Time Quantize (-6 = 1/16), +0x56 its Strength (int8 offset from 100)
    head = bytes(0x10) + struct.pack('<H', len(name)) + name.encode() + bytes(len(name) & 1)
    settings = bytearray(0x60)
    struct.pack_into('<I', settings, 4, trim)
    struct.pack_into('<I', settings, 0x3c, length)
    struct.pack_into('<h', settings, 0x48, quantize)
    struct.pack_into('<b', settings, 0x56, strength - 100)
    return chunk('MSeq', cls, oid, head + bytes(settings)) + chunk('EvSq', cls, oid, evs)


def note(pos, key, vel, length, hires=0, offset=0):
    b8 = bytearray(8)
    if hires:
        struct.pack_into('<H', b8, 2, hires)   # +10..11: velocity / 32767
    else:
        b8[3] = vel                            # +11: velocity
    b8[4] = key                                # +12: the key
    return event(0x90, pos, bytes(b8), [ext(struct.pack('<Ih', 0, offset), u12=length)])   # length; +4 ticks past pos (signed)


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


def plugin(order, name, flags, plugin_id, values, preset='#default.pst', steps=None):
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
    table = b''.join(struct.pack('<iii', d, h, d) for d, h in (steps or []))   # (default, highest, default) steps per parameter
    return bytes(pl) + struct.pack('<III', plugin_id, 0, flags) + struct.pack('<I', len(block)) + bytes(16) + table + block


def archive(top, objects):
    # an NSKeyedArchiver binary plist: $objects[0] is $null, references are UIDs
    return plistlib.dumps({'$version': 100000, '$archiver': 'NSKeyedArchiver', '$top': top, '$objects': ['$null'] + objects}, fmt=plistlib.FMT_BINARY)


def archived_record(index, bp):
    # a channel's record of kind 7: u16 7, u16 its index, then the archive's length at +16 and the archive at +20
    return struct.pack('<HH', 7, index) + bytes(12) + struct.pack('<I', len(bp)) + bp


def send(index, code, level, target):
    # a send: +6 index, +0x14 destination code, +0x18 level (8.24 on the fader's 0-127 scale), +0x3c the destination's UUID
    pl = bytearray(76)
    struct.pack_into('<H', pl, 6, index)
    pl[0x14], pl[0x16] = code, 2
    struct.pack_into('<I', pl, 0x18, level << 24)
    pl[0x3c:0x4c] = target
    return bytes(pl)


INST, BUS, AUD, OUT, AUI = 0x29, 0x4d, 0x31, 0x51, 0x39   # channel numbers (AUI: a third-party Audio Unit instrument)
AUTRACK, REGION7 = 0x110, 32
LOOP, FILE, FILE2 = 0x10c, 0x200, 0x204           # the audio track's object, its files' ids
AUTOROOT, AUTOLOOP, AUTOMASTER, AUTOSYNTH = 0x300, 0x304, 0x308, 0x30c   # the automation root folder, the Loop track's, the master's, the Synth's
TRACK, ECHO, MASTER, REGION1, REGION2, REGION3, REGION4, REGION5, REGION6 = 0x100, 0x104, 0x108, 8, 12, 16, 20, 24, 28
bus_uuid = bytes([0xd5]) + bytes(range(1, 16))
retro = [1e30] * 902                               # parameter #n; unused ones hold 1e30
for n, v in {1: 8, 2: 0, 3: 12, 4: 0, 5: -6, 201: 0, 209: 0, 301: 1, 303: 1, 401: 0, 802: 1, 803: 100, 804: 1, 805: 100, 806: 0}.items():
    retro[n] = v
echo = [0.0] * 21
echo[16:21] = [7, 40, 0, 0, 100]                  # Time 1/8, Repeat 40 %, Color 0, Dry 0 %, Wet 100 %

body = b''
body += seq(3, 0, 'Tempo', events(event(0x60, BAR1, ext=[ext(struct.pack('<I', 100 * 10000), mark=0x88)]),
                                  event(0x60, BAR1 + 12 * 3840, ext=[ext(struct.pack('<I', 90 * 10000), mark=0x88)])))   # 90 BPM from bar 13
body += seq(1, 0, 'Signature', events(event(0x30, 0, bytes([0, 0, 0, 2, METER, 0, 0, 0]), [ext(mark=0x88)])))   # +11 log2 4, +12 the beats
body += seq(0x16, 0, 'Locators', events(event(0x10, BAR1 + 2 * 3840 - 1, ext=[ext(u12=BAR1)])))             # cycle bars 1-2
# arrangement markers: TxSq objects hold the names (between the offsets at +0x10 and +0x14); the class 5 sequence has
# an event 0x12 at each start, extension +0 the name's object, +12 the length
txsq = lambda oid, name: chunk('TxSq', 0, oid, bytes(0x10) + struct.pack('<II', 0x18, 0x18 + len(name) + 1) + name.encode() + b'\0')
body += txsq(4, 'Intro') + txsq(8, 'Verse')
# the transposition track (class 0x19): event 0x70 at each point, extension +5 the semitones, +6 the root they give;
# +2 from beat 40 moves the last note and the Apple Loop at beat 40
body += seq(0x19, 0, 'Untitled', events(event(0x70, BAR1, ext=[ext(bytes([0xff] * 5 + [0, 60]), mark=0xb2)]),
                                         event(0x70, BAR1 + 40 * 960, ext=[ext(bytes([0xff] * 5 + [2, 62]), mark=0xb2)])))
body += seq(5, 0, 'Untitled', events(event(0x12, BAR1, ext=[ext(struct.pack('<I', 4), u12=8 * 960, mark=0x88)]),
                                     event(0x12, BAR1 + 8 * 960, ext=[ext(struct.pack('<I', 8), u12=8 * 960, mark=0x88)])))
body += envi(TRACK, 'Synth', INST) + envi(ECHO, 'Echo', BUS) + envi(MASTER, 'Master', OUT) + envi(AUTRACK, 'AU Synth', AUI)
trak = lambda row, kind, obj: chunk('Trak', 0x17, 4, struct.pack('<HHHHI', kind, 0, 0, 0, obj) + bytes(46), sub=row)
# a region's placement: main +13 bit 0x10 looped; extension 1: the track object and the length (a looped one's whole
# span), 2: its sequence, 0x8a: its parameters (+5 Transpose)
placement = lambda pos, length, region, looped=False, transpose=0: event(
    0x20, pos, bytes([0, 0, 0, 0, 0, 0x16 if looped else 0x04, 0, 0]),
    ext=[ext(struct.pack('<I', TRACK), length), ext(struct.pack('<I', region), mark=0x88), ext(bytes([0, 6, 0, 0, 0, transpose & 0xff]), mark=0x8a)])
# an audio region's placement (0x24): looped; extension 1: the track and the whole span (2000 ticks = 2.5 passes of the
# region's 0.5 s at 100 BPM), 0xbc: +8 the region's index, +12 its file, 0x8a: its parameters (+0 bit 0x20 Reverse
# Playback, +4 gain dB, +5 Transpose)
audio_placement = lambda pos, length, track, file, looped=True, params=None: event(
    0x24, pos, bytes([0, 0, 0, 0, 0, 0x16 if looped else 0x04, 0, 0]),
    ext=[ext(struct.pack('<I', track), length), ext(struct.pack('<III', 0, 0, 0), u12=file, mark=0xbc)] + ([ext(params, mark=0x8a)] if params else []))
# an automation point: a controller record, +8 the value in 8.24, +12 the controller (7 volume, 10 pan), +15 0x40 on a
# step GarageBand stores between points
auto_point = lambda pos, cc, value, step=False: event(0xb0, pos, struct.pack('<I', int(value * (1 << 24))) + bytes([cc, 0, 0, 0x40 if step else 1]))
body += seq(0x17, 4, 'Test Song', events(placement(BAR1 - 3840, END, REGION1), placement(BAR1 + 3840, 3840, REGION2),
                                         placement(BAR1 + 3 * 3840, 3 * 960, REGION3, looped=True),
                                         placement(BAR1 + 5 * 3840, END, REGION4, transpose=2),
                                         placement(BAR1 + 9 * 3840, END, REGION5),
                                         placement(BAR1 + 11 * 3840, END, REGION6),
                                         audio_placement(BAR1 + 7 * 3840, 2000, LOOP, FILE),
                                         audio_placement(BAR1 + 9 * 3840, END, LOOP, FILE2, looped=False, params=bytes([0x20, 6, 0, 0, (-6) & 0xff, 2])),
                                         event(0x20, BAR1 - 3840, bytes([0, 0, 0, 0, 0, 0x04, 0, 0]),
                                               ext=[ext(struct.pack('<I', AUTRACK), END), ext(struct.pack('<I', REGION7), mark=0x88)])),
            trak(0, 1, TRACK) + trak(1, 1, LOOP) + trak(2, 3, MASTER) + trak(3, 1, AUTRACK))
body += seq(0x17, REGION7, 'AU Synth', events(note(BAR1, 62, 100, 960)))
body += seq(0x17, REGION1, 'Synth', events(note(BAR1, 60, 100, 1920), note(BAR1 + 1920, 64, 80, 960), note(BAR1 + 2880, 67, 0, 960, hires=16385)))
body += seq(0x17, REGION2, 'Synth 2', events(note(BAR1, 69, 90, 960), event(0xb0, BAR1 + 480, bytes([0, 0, 0, 64, 1, 0, 0, 0])),
                                             note(BAR1 + 3840, 72, 90, 960)))
body += region(0x17, REGION3, 'Synth 3', events(note(BAR1, 64, 90, 480)), length=960)
body += region(0x17, REGION4, 'Synth 4', events(note(BAR1, 57, 90, 480), note(BAR1 + 600, 59, 90, 480, offset=130)), trim=480, length=1920,
               quantize=-6)
body += region(0x17, REGION5, 'Synth 5', events(note(BAR1 + 480, 62, 90, 240)), length=1920, quantize=-26)
# 1/4 at Strength 50 %: played at 0.75 beat (stored at 0.875, 120 ticks back), its grid line 1.0, so it plays at 0.875
body += region(0x17, REGION6, 'Synth 6', events(note(BAR1 + 840, 60, 90, 240, offset=-120)), length=1920, quantize=-10, strength=50)   # 1/8 Swing F: the off-beat 8th goes to 17/24 of the pair
body += channel(INST, 0x43, 0, ' Inst 1', 80, 80, bytes([0xd5]) + bytes(15))
body += record(INST, 0, send(0, 0, 45, bus_uuid))
steps = [(0, 100)] * 410
steps[407] = (100, 200)                            # Cutoff by Env: 0-200, 100 = none
body += record(INST, 1, plugin(0, 'Retro Synth', 0x08000000, 279, retro, steps=steps))
# Smart Controls: knob 2 -> Retro Synth (slot 0) #407 over steps 100-150; its label "Filter" in a second archive
cls_dict = {'$classname': 'NSDictionary', '$classes': ['NSDictionary', 'NSObject']}
cls_arr = {'$classname': 'NSArray', '$classes': ['NSArray', 'NSObject']}
mapping = archive({'dictionary': UID(1)}, [
    {'NS.keys': [UID(2)], 'NS.objects': [UID(3)], '$class': UID(10)}, '2', {'NS.objects': [UID(4)], '$class': UID(11)},
    {'NS.keys': [UID(5), UID(6), UID(7), UID(8), UID(9)], 'NS.objects': [407, 0, 100, 150, False], '$class': UID(10)},
    'parameterIndex_1', 'slot', 'rangeLow', 'rangeHigh', 'rangeIsFlipped', cls_dict, cls_arr])
labels = archive({'dictionary': UID(1)}, [
    {'NS.keys': [UID(2)], 'NS.objects': [UID(3)], '$class': UID(9)}, 'kLgControlLabelLookupKey',
    {'NS.keys': [UID(4)], 'NS.objects': [UID(6)], '$class': UID(9)}, {'uuidString': UID(5), 'autogenerated': False}, 'Smart Knob 3',
    {'NS.keys': [UID(7)], 'NS.objects': [UID(8)], '$class': UID(9)}, 'customLabel', 'Filter', cls_dict])
body += record(INST, 2, archived_record(1, mapping)) + record(INST, 3, archived_record(2, labels))
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
# the Apple Loop: a CAF of 4 beats at 120 BPM (a 2 s, 48 kHz sine), tagged G major the way GarageBand tags its loops
n2 = 2 * rate
pcm2 = struct.pack('>%dh' % n2, *[int(12000 * math.sin(2 * math.pi * 392 * i / rate)) for i in range(n2)])
cafchunk = lambda kind, body: kind + struct.pack('>q', len(body)) + body
tags = [('category', 'Keyboards'), ('key signature', 'G'), ('key type', 'major'), ('time signature', '4/4'), ('beat count', '4')]
uuid = bytes.fromhex('29819273B5BF4AEFB78D62D1EF90BB2C') + struct.pack('>I', len(tags)) + b''.join(k.encode() + b'\0' + v.encode() + b'\0' for k, v in tags)
fname2 = 'Test Apple Loop.caf'
with open(os.path.join(media, fname2), 'wb') as f:
    f.write(b'caff' + struct.pack('>HH', 1, 0) + cafchunk(b'desc', struct.pack('>d4sIIIII', rate, b'lpcm', 0, 2, 1, 1, 16)) +
            cafchunk(b'uuid', uuid) + cafchunk(b'data', struct.pack('>I', 0) + pcm2))
fl2 = bytearray(10 + 2 * len(fname2) + 0x1f0)
struct.pack_into('<H', fl2, 8, len(fname2))
fl2[10:10 + 2 * len(fname2)] = fname2.encode('utf-16le')
b2 = 10 + 2 * len(fname2)
fl2[b2 + 0x8a:b2 + 0x8a + 11] = b'Audio Files'
struct.pack_into('<I', fl2, b2 + 0x1d4, n2)
struct.pack_into('<H', fl2, b2 + 0x1dc, rate)
fl2[b2 + 0x1e0], fl2[b2 + 0x1e2] = 1, 16
rg2 = bytearray(0x4c + 10 + 16)
struct.pack_into('<I', rg2, 0x16, n2)
struct.pack_into('<H', rg2, 0x4a, 10)
rg2[0x4c:0x56] = b'Apple Loop'
body += chunk('AuFl', 0x05, FILE2, bytes(fl2)) + chunk('AuRg', 0x05, FILE2, bytes(rg2), ref=0)
# automation: the root folder places one "*Automation" sequence per track (extension 1: the track object, 2: the sequence)
body += seq(0x17, AUTOROOT, 'Track Automation Root Folder', events(event(0x20, BAR1 - 3840, ext=[ext(struct.pack('<I', LOOP), END), ext(struct.pack('<I', AUTOLOOP), mark=0x88)]),
                                                                  event(0x20, BAR1 - 3840, ext=[ext(struct.pack('<I', MASTER), END), ext(struct.pack('<I', AUTOMASTER), mark=0x88)]),
                                                                  event(0x20, BAR1 - 3840, ext=[ext(struct.pack('<I', TRACK), END), ext(struct.pack('<I', AUTOSYNTH), mark=0x88)])))
body += seq(0x17, AUTOMASTER, '*Automation', events(auto_point(BAR1, 7, 90), auto_point(BAR1 + 2 * 3840, 7, 0)))
knob_point = lambda pos, knob, value: event(0x50, pos, struct.pack('<I', int(value * (1 << 24))) + bytes([knob, 1, 0, 1]))     # +13 1: a Smart Control
body += seq(0x17, AUTOSYNTH, '*Automation', events(knob_point(BAR1, 2, 0), knob_point(BAR1 + 2 * 3840, 2, 127)))
body += channel(OUT, 0x4c, 0, 'Output 1-2', 90, 64, bytes([0xd5, 0x51]) + bytes(14))
send_point = lambda pos, slot, value: event(0x50, pos, struct.pack('<I', int(value * (1 << 24))) + bytes([28 + slot, 0, 0, 1]))   # 0x50: parameter 28 + slot
body += seq(0x17, AUTOLOOP, '*Automation', events(auto_point(BAR1, 7, 90), auto_point(BAR1 + 3840, 7, 70, step=True), auto_point(BAR1 + 2 * 3840, 7, 45),
                                                   auto_point(BAR1, 10, 64), auto_point(BAR1 + 4 * 3840, 10, 0),
                                                   send_point(BAR1, 0, 0), send_point(BAR1 + 2 * 3840, 0, 90)))
body += channel(AUD, 0x40, 0, ' Audio 1', 90, 64, bytes([0xd5, 9]) + bytes(14))
body += record(AUD, 0, send(0, 0, 0, bus_uuid))   # slot 0 to the Echo bus, its level automated
# a third-party Audio Unit instrument: its name at +120, manufacturer, type and subtype reversed at +132, +136, +140,
# then its state as its ClassInfo property list (what an .aupreset holds)
class_info = (b'<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0">\n<dict>\n\t<key>name</key>\n\t<string>Test State</string>\n'
              b'\t<key>type</key>\n\t<integer>1635085685</integer>\n</dict>\n</plist>\n')
au = bytearray(160)
au[120:130] = b'Test Synth'
au[132:136], au[136:140], au[140:144] = b'Test'[::-1], b'aumu'[::-1], b'Synt'[::-1]
body += channel(AUI, 0x43, 1, ' Inst 2', 90, 64, bytes([0xd5, 0x39]) + bytes(14))
body += record(AUI, 1, bytes(au) + struct.pack('<I', len(class_info)) + class_info)
fx = bytearray(160)                                # and an Audio Unit effect after it
fx[120:131] = b'Test Effect'
fx[132:136], fx[136:140], fx[140:144] = b'Test'[::-1], b'aufx'[::-1], b'Efct'[::-1]
body += record(AUI, 2, bytes(fx) + struct.pack('<I', len(class_info)) + class_info)
with open(os.path.join(alt, 'ProjectData'), 'wb') as f:
    f.write(b'#G\xc0\xab' + struct.pack('<HHIII', 0x09d0, 3, 4, 0x00080001, len(body)) + struct.pack('<I', 0) + body)
