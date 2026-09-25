# Bitwig project files (.bwproject)

Bitwig's own project format is private and undocumented. Wavelength reads it (`src/bitwig.cpp`) only
for what a DAWproject export leaves out: the settings of Bitwig's own devices, the plugins inside
them (Drum Machine pads, Chain, Multiband FX-3 bands) and those plugins' saved states. Notes and
clips still come from the DAWproject. This page records the layout as observed in 745 projects
saved by Bitwig 4 to 6, so the reader can be extended.

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
