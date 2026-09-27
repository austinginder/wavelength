# Bitwig project files (.bwproject)

Bitwig's own project format is private and undocumented. Wavelength reads it (`src/bitwig.cpp`) for
the settings of Bitwig's own devices, the plugins inside them (Drum Machine pads, Chain, Multiband FX-3
bands) and those plugins' saved states, which a DAWproject export leaves out, and for the arrangement
itself (tempo, mixer, note clips, automation), so `wavelength import song.bwproject` needs no export.
This page records the layout as observed in 745 projects saved by Bitwig 4 to 6, so the reader can be
extended. The arrangement fields were checked against a DAWproject export of the same song (the two
imports give the same job, note for note).

## File

| Bytes | Content |
|---|---|
| 0-3 | `BtWg` |
| 4-15 | hex digits: `0003` `0002` + format revision (`00b0`... `00ba` = Bitwig 4-5, `00c0`/`00c1` = Bitwig 6) |
| 16-23 | hex offset of the body |
| 24-39 | hex offset of the plugin-state zip at the end of the file |
| after the header | a metadata block (key/value: creator, application version, ...) |
| body offset | one object: the project |
| end | a zip: `plugin-states/<uuid>.vstpreset`, `.clap-preset`, `.fxb`, `.fxp` |

## Body

Big-endian throughout. An object is a u32 class id followed by fields, each a u32 field id, a u8
type and a value, until a u32 `0`. Field ids are stable across versions.

| Type | Value |
|---|---|
| 0x00, 0x0a | none (null) |
| 0x01, 0x05 | u8 (0x05 = bool) |
| 0x02, 0x03, 0x04 | i16, i32, i64 |
| 0x06, 0x07 | float, double |
| 0x08 | string: u32 length (high bit set: UTF-16BE code units) + bytes |
| 0x09 | object |
| 0x0b | reference: u32 object number |
| 0x0f | u32 count + that many i32s |
| 0x0d | blob: u32 length + bytes (a nested `BtWg` stream for device internals) |
| 0x12 | list of objects, ended by the u32 `3` |
| 0x14 | u8 count + that many strings + u32 (user names); takes an object number |
| 0x15 | 16-byte UUID |
| 0x16 | colour: 4 floats |
| 0x17, 0x19 | u32 count + that many floats / u32s |
| 0x1a | object + a string key (the key of a shared object) |

Class id `1` in place of an object is a reference: u32 object number. Objects are numbered from 1
in the order they start (each type-0x14 value also takes a number), and an object shared by several
owners is written in full at its first appearance, which is often somewhere unexpected (a track's
device can first appear inside a remote-control mapping). Always resolve references.

Some classes write a second group of fields after the first `0` (their base class): the first `0`
is then followed by one `0x00` byte, the second group, and another `0`. A few classes have the extra
byte without a second group. Nothing in the stream marks these classes, so `bitwig.cpp` keeps a table
per format (`kTable5`, `kTable6`: Bitwig 6 gives the project itself and three more classes a second
group and takes the clip class's away), learned by parsing the corpus and checking every change
against files that already read. One project needs the project-info class (477) as two groups; the
reader retries with that when the first pass fails.

A read only counts when the body ends where the plugin-state zip begins (within a few KB of the
offset in the header): a wrong table rule can otherwise close the project object early and "succeed"
with no tracks. All 745 projects pass that check.

## Where things are

| Path | Meaning |
|---|---|
| project `0x4de` | tracks (list), `0x4df` effect tracks, `0x4e0` master track (reference) |
| track `0x15b` | name (often empty: Bitwig shows the instrument's), `0x164` -> `0x144` device slots |
| slot `0x197` | the device |
| device `0x9a` | name; `0xa3` enabled; `0xbe5` state file in `plugin-states/` |
| device `0xce4` | VST3 class id (string) or VST2 id (i32); `0x2ec9` CLAP id; `0x99` Bitwig device UUID |
| native device `0xa4` -> `0x20c` | parameters: `0x2b9` id, value in `0x136` (double), `0x273` (enum) or `0x12f` (bool); a parameter with `0x8e1` is a sub-chain (`0x349` -> `0x87` devices) |
| Drum Machine `DRUM_PADS` `0x8e0` | pads: `0x8e5` key, `0x349` chain, `0x825` mixer (`0x821` volume, `0x822` pan, `0x823` mute) |
| Sampler `SAMPLE` | `0x74c` zone -> `0x748` -> `0x129e` file -> `0xcd4` package path (`Vendor/Package:ver/samples/...`); zone `0x75c` root key; a multisample has `0xfb3` name and `0x76d` zones |

Units: frequencies are MIDI pitch (69 = 440 Hz); faders and pad volumes store amplitude^(1/3)
(1.26 = +6 dB); compressor attack and release are log10 seconds, ratio is 1 - 1/ratio; EQ+ gains are
dB and Q is log10. EQ+ band types seen: 3 and 5 bell, 1 and 10 low cut, 0 and 14 high cut, 6 and 15
high shelf, 16 and 17 low shelf, 13 off (the cut and shelf numbers were inferred from their
frequencies and gains).

## The arrangement

| Path | Meaning |
|---|---|
| project `0xbd9` -> `0x211` | tempo parameter, value `0x2c8` (bpm). Bitwig 6: `0x211` on the project itself |
| project `0xbd9` -> `0x212` | time signature, `0x1cbe` = `0x1000_00<log2 denominator><numerator>` (`0x10000024` = 4/4; only 4/4 appears in the corpus, so the nibble order is inferred). Bitwig 6: `0x212` on the project |
| track `0x165` | mixer: `0x1a4` volume, `0x1a5` pan, `0x1a6` mute; each a parameter with its value in `0x2c8` (volume = amplitude^(1/3), pan -1..1) or `0xd2` (mute); `0xa5` sends |
| send | `0xab3` amount (a parameter, amplitude^(1/3)), `0xbd0` the effect track it feeds |
| track `0x15d` -> `0x238` -> `0x21f` | the arranger's clips. Bitwig 6: track `0x2bf8` -> `0x2d8e` -> `0x21f` |
| clip (class 71) | `0x2af` position, `0x26` length (beats), `0x10f8` muted; `0x288` the content (Bitwig 6: `0x2ae9` -> `0x10e9`) |
| content (class 191 = notes) | `0x98f` playback (Bitwig 6: on the clip, `0x2adb`): `0x98c` -> `0x2af` play start, `0x991` loop on, `0x992` loop region (`0x2af` start, `0x26` length). Audio clips have another content class |
| content `0x49c` -> `0x18cb` | note rows (class 66): `0xee` key, `0x21f` notes (class 102): `0x2af` start, `0x26` length, `0xef` velocity, `0xf0` release velocity, `0x10f8` muted (content time) |
| track `0x15d` -> `0x239` | automation lanes (class 290): `0x4cc` target, `0x48e` -> `0x21f` points (class 264: `0x2af` beat, `0x28f` value, `0xba6` curve tension) |
| Bitwig 6 automation | track `0x2dc9` -> `0x4ba` lanes (class 573): `0x2a3` target, `0x2d8c` body -> `0x35ac` curve -> `0x21f` points; the body's `0x21f` holds automation clips (not read yet) |
| target (class 101) | `0x133b` device name, `0x133a` parameter name, `0xed` path: `.../PID<hex id>` a CLAP or VST3 parameter, `.../PARAM<index>` a VST2 one, a path with `:` a device inside a chain, empty with device `Mixer` the track's `Volume` or `Pan` |

Values follow the clip semantics of DAWproject: content time `playStart` sounds at the clip's position,
a looping clip then repeats the loop region until the clip ends, and notes past the clip's end are cut.
Automation values are stored like the parameters they move: volume as amplitude^(1/3), pan -1..1,
plugin parameters normalized. A track's name is often empty; Bitwig then shows its instrument's.
