#!/usr/bin/env python3
"""Broken and hostile files for `wavelength __parse`: each one targets a bounds check a reader once got
wrong (a size that wraps around, a length past the buffer, a nesting or sharing pattern that blows up).
A reader must refuse every one of them with an error: no crash, no read outside the file, no hang.

Usage: scripts/make-hostile-files.py <dir>   (writes <format>--<what>.<ext> files; the format is the
__parse format to read them with)
"""
import os
import struct
import sys
import zlib


def bplist(objects, top=0):
    """A binary plist from already-encoded objects (bytes each), 1-byte refs and 2-byte offsets."""
    out = bytearray(b"bplist00")
    offsets = []
    for o in objects:
        offsets.append(len(out))
        out += o
    table = len(out)
    for off in offsets:
        out += struct.pack(">H", off)
    out += bytes(6) + bytes([2, 1]) + struct.pack(">QQQ", len(objects), top, table)
    return bytes(out)


def big_count(marker, count):
    """An object marker whose count follows as an 8-byte integer object."""
    return bytes([marker | 0x0F, 0x13]) + struct.pack(">Q", count)


def zip_with_entry(name, data, method=8, comp=None, size=None, name_len=None):
    """A one-entry zip; comp/size/name_len override what the central directory claims."""
    raw = zlib.compress(data)[2:-4] if method == 8 else data
    crc = zlib.crc32(data)
    local = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, method, 0, 0, crc, len(raw), len(data), len(name), 0) + name + raw
    cd = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, method, 0, 0, crc,
                     len(raw) if comp is None else comp, len(data) if size is None else size,
                     len(name) if name_len is None else name_len, 0, 0, 0, 0, 0, 0)
    cd += name if name_len is None else b""
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 1, 1, len(cd), len(local), 0)
    return local + cd + eocd


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    files = {}

    # A UTF-16 string of 2^63 characters: 2 * count wraps to 0, so `at + 2 * count <= n` once passed
    files["bplist--utf16-count-wraps.plist"] = bplist([big_count(0x60, 1 << 63)])
    # data and ASCII strings whose count is 2^64 - 1: `at + count` wraps below the end
    files["bplist--data-count-wraps.plist"] = bplist([big_count(0x40, (1 << 64) - 1)])
    files["bplist--ascii-count-wraps.plist"] = bplist([big_count(0x50, (1 << 64) - 1)])
    # an array of 2^63 refs: count * refSize wraps
    files["bplist--array-count-wraps.plist"] = bplist([big_count(0xA0, 1 << 63)])
    # 60 levels of arrays that each hold the next one twice: 2^60 visits without a budget
    files["bplist--shared-refs-explode.plist"] = bplist(
        [bytes([0xA2, i + 1, i + 1]) for i in range(60)] + [bytes([0x08])])
    # a keyed archive whose $objects fan out the same way through UIDs: $objects[k] = [UID k+1, UID k+1]
    objs = [bytes([0xD2, 1, 2, 3, 4]), b"\x54$top", b"\x58$objects", bytes([0xD1, 5, 6]),
            bytes([0xAF, 0x10, 62]) + bytes(range(7, 69)), b"\x54root", bytes([0x80, 1]), b"\x55$null"]
    objs += [bytes([0xA2, 68 + k, 68 + k]) for k in range(1, 61)]   # objects 8..67: $objects[1..60]
    objs += [b"\x51x"]                                              # object 68: $objects[61]
    objs += [bytes([0x80, j]) for j in range(2, 62)]                 # objects 69..128: UID 2..61
    files["bplist--archive-uids-explode.plist"] = bplist(objs)

    # a central directory entry whose name runs 65,535 bytes past the directory
    files["zip--name-past-directory.zip"] = zip_with_entry(b"a.txt", b"hello", name_len=0xFFFF)
    # an entry that claims 4 GB from a few bytes of deflate
    files["zip--size-claims-4gb.zip"] = zip_with_entry(b"a.txt", b"hello", size=0xFFFFFFF0)
    # an entry whose compressed size runs past the file
    files["zip--data-past-file.zip"] = zip_with_entry(b"a.txt", b"hello", comp=0x7FFFFFF0)
    # a directory that claims to be bigger than the file
    z = bytearray(zip_with_entry(b"a.txt", b"hello"))
    z[-10:-6] = struct.pack("<I", 0xFFFFFF00)
    files["zip--directory-past-file.zip"] = bytes(z)

    # a Serum 2 header whose length is 2^64 - 10: 17 + n + 8 wraps to 15
    files["serum--header-length-wraps.SerumPreset"] = b"XferJson\0" + struct.pack("<Q", (1 << 64) - 10) + b"{}" + bytes(16)
    # a payload that claims 4 GB
    files["serum--payload-claims-4gb.SerumPreset"] = (b"XferJson\0" + struct.pack("<Q", 2) + b"{}" +
                                                       struct.pack("<II", 0xFFFFFFF0, 2) + bytes(8))

    # a JUCE ValueTree property: a double (8 bytes) in a value 2 bytes long, at the end of the file
    files["valuetree--double-past-end.bin"] = b"a\0" + bytes([1, 1]) + b"k\0" + bytes([1, 2, 4, 0])

    # a SoundFont preset-data chunk shorter than its own 4-byte type: n - 4 wraps to 4 GB
    sf = b"LIST" + struct.pack("<I", 2) + b"pdta"
    files["sf2--pdta-size-wraps.sf2"] = b"RIFF" + struct.pack("<I", 4 + len(sf)) + b"sfbk" + sf
    # a sample chunk that claims more than the file holds
    sd = b"LIST" + struct.pack("<I", 4 + 8) + b"sdta" + b"smpl" + struct.pack("<I", 0x7FFFFFF0)
    files["sf2--samples-past-file.sf2"] = b"RIFF" + struct.pack("<I", 4 + len(sd)) + b"sfbk" + sd

    # XML nested 100,000 elements deep: one recursion each, past any thread's stack
    files["xml--nested-100k.xml"] = b"<a>" * 100000
    files["musicxml--nested-100k.musicxml"] = b"<score-partwise>" + b"<a>" * 100000

    # a channel strip record that claims 152 bytes (so its flags word at +148 is read) where the file holds 140: the
    # strip reads, cut to the bytes that are there
    payload = bytearray(140)
    payload[132:136] = b"MELC"
    files["patch--cst-record-past-end-readable.patch/#Root.cst"] = b"UCuA" + bytes(24) + struct.pack("<I", 152) + bytes(4) + bytes(payload)

    for name, data in files.items():
        os.makedirs(os.path.dirname(os.path.join(out, name)), exist_ok=True)
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
    print(len(files), "files in", out)


if __name__ == "__main__":
    main()
