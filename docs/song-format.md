# Wavelength song format

**Draft 2** (2026-09-27), for review. Format version `1.0`. This document is licensed CC BY 4.0; the JSON
Schemas it names are CC0. Anyone may read and write Wavelength songs without asking, paying or
using Wavelength's code.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

All JSON in a song (the manifest, the job, the history log and `review.json`) is I-JSON (RFC 7493):
UTF-8 without a byte order mark, and no object with the same key twice. Readers MUST refuse a
manifest, log line or `review.json` with a duplicate key rather than pick one of the values.

## 1. Overview

A **song** is a folder of open files: the job that renders it, the files the job uses, the scripts
and notes that made it, the latest render, its revision history and the comments on it. A
**`.wavelength` file** is that folder packed into a zip for sharing. Unpacking a package gives a song
folder; packing that folder again gives the same package.

Goals, in order:

1. **Open.** JSON and plain text inside, a published spec and schema, nothing in the format that only
   one program can interpret. The one exception is a plugin's own state file, which only that
   plugin reads; a song can always name a preset instead.
2. **Safe to open.** A song is data. Opening, validating or rendering one never runs code it
   contains, and a package cannot write outside the folder it unpacks into.
3. **Portable.** Paths are relative and stay inside the song. Everything else a song needs (plugins,
   presets, sample libraries) is named, listed in the manifest, and can have a stand-in that plays
   anywhere.
4. **Reviewable.** Every render is a revision; comments point at the revision and bars that were
   heard, so whoever picks a comment up later (a person or an agent) knows exactly what it meant.

## 2. The song folder

```
express-to-nowhere/
  wavelength.json      the manifest (required, section 3)
  job.json             what renders (required, section 4; the manifest may name another file)
  NOTES.md             notes: the story of the song, credits, a table of its tracks
  make-job.py          source: how job.json was made (listed in the manifest)
  media/               files the job uses: samples, audio clips, preset files
  render/              the render that goes with the song: mix, picture, report (section 8)
  review.json          comments (section 7)
  history/             revisions (section 6)
  out/                 scratch renders: never packed, never tracked
```

Only `wavelength.json` and the job are required. The job sits at the top of the folder (its path has
no `/`). A folder MAY hold other files, such as scratch renders in `out/`; the rules below apply to
the song's files (the job, the files the manifest lists, the files the job refers to, `review.json`
and `history/`), and a package carries those and nothing else.

**Names and paths.** Every path in a song is relative to the song folder and uses `/` as the
separator. A path MUST NOT be absolute (start with `/`, a drive letter such as `C:` or `~`), MUST
NOT end with `/`, and MUST NOT contain empty, `.` or `..` segments, so it always resolves inside the
folder. Each segment (a folder or file name):

- is UTF-8 in Unicode NFC, at most 255 bytes, with no control characters (U+0000 to U+001F, U+007F);
- contains none of `\ < > : " | ? *`, and doesn't end with a space or `.`;
- isn't a Windows device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1` to `COM9`, `LPT1` to `LPT9`, in any
  case, with or without an extension);
- isn't `.git` (compared case-insensitively, ignoring trailing spaces and dots) or `git~1`.

Paths MUST be unique when compared after Unicode simple case folding, so a song survives
case-insensitive file systems. Songs MUST NOT contain symbolic links, and writers MUST NOT follow one
into a song's files, history or package.

## 3. The manifest: `wavelength.json`

```json
{
  "format": "wavelength.song",
  "formatVersion": "1.0",
  "minReaderVersion": "1.0",
  "generator": {"name": "wavelength", "version": "0.4.0"},
  "id": "9b2f6c1e-4d7a-4f0e-9a51-6b3c2d8e1f07",
  "title": "Express to Nowhere",
  "slug": "express-to-nowhere",
  "created": "2026-09-25T22:00:00-04:00",
  "updated": "2026-09-27T10:12:44-04:00",
  "authors": [
    {"name": "Austin Ginder", "role": "producer"},
    {"name": "Claude Opus 5.5", "role": "composer", "kind": "ai"}
  ],
  "prompt": "I'd like to make a trance song. It should start off extremely slow...",
  "summary": "A trance train that leaves the station at 32 BPM and pumps up to 140.",
  "license": "CC-BY-4.0",
  "tags": ["trance", "tempo ramp"],
  "job": "job.json",
  "files": [
    {"path": "make-job.py", "role": "source", "mediaType": "text/x-python"},
    {"path": "NOTES.md", "role": "notes", "mediaType": "text/markdown"},
    {"path": "media/white-noise.wav", "role": "media", "mediaType": "audio/wav", "size": 960044, "sha256": "4f1c..."},
    {"path": "render/mix.mp3", "role": "render", "mediaType": "audio/mpeg", "size": 8123904, "sha256": "a0d2..."},
    {"path": "render/song.png", "role": "render", "mediaType": "image/png"},
    {"path": "render/report.json", "role": "render", "mediaType": "application/json"}
  ],
  "render": {"revision": 14, "job": "sha256:7e3a...", "mix": "render/mix.mp3", "picture": "render/song.png", "report": "render/report.json"},
  "requires": [
    {"track": "bass", "plugin": {"name": "Surge XT", "format": "clap", "id": "org.surge-synth-team.surge-xt", "version": "1.3.4"},
     "preset": "Trance Seq Bass", "fallback": true}
  ],
  "extensions": {},
  "extensionsRequired": [],
  "metadata": {"run.wavelength.site": {"sections": []}}
}
```

| Field | | Meaning |
|---|---|---|
| `format` | required | Always `"wavelength.song"`. |
| `formatVersion` | required | The version of this spec the song follows, `"major.minor"`. |
| `minReaderVersion` | optional | The oldest format version a reader must support to use the song correctly (default: `formatVersion`'s major, `.0`). |
| `generator` | optional | The program that last wrote the song: `{name, version}`. |
| `id` | required | A UUID that stays with the song through edits, renames and copies. A remix gets a new one and names its origin in `derivedFrom`. |
| `derivedFrom` | optional | `{"id", "title", "url"}` of the song this one was made from. |
| `title`, `slug` | required | Display title; `slug` is lowercase `a-z 0-9 -`, the folder's and package's name. |
| `created`, `updated` | optional | RFC 3339 times. |
| `authors` | optional | People and models, each `{name, role, kind?, url?}`; `kind` is `"human"` (default) or `"ai"`. |
| `prompt`, `summary`, `description` | optional | The brief the song was made from; one sentence; longer text in Markdown. |
| `license` | optional | An SPDX license expression (`"CC-BY-4.0"`, `"MIT"`). Missing means all rights reserved. |
| `tags` | optional | Free words. |
| `job` | required | File name of the job, at the top of the folder (section 4). |
| `files` | optional | Every other file that belongs to the song (the job, `review.json` and `history/` are implied), each `{path, role, mediaType?, size?, sha256?}`. No path in it may be `mimetype`, `wavelength.json`, the job or `review.json`, or lie under `history/` or `out/`. |
| `render` | optional | The render that goes with the song (section 8). |
| `requires` | optional | What the song needs from the computer that renders it (section 4.3). Written by `pack`; readers treat it as a summary of the job, never as its source. |
| `extensions`, `extensionsRequired`, `metadata` | optional | Section 10. |

File roles:

| Role | Meaning |
|---|---|
| `source` | How the job was made: generator scripts, data files, style briefs. Informational. Readers MUST NOT run a source file, and a song MUST render from its job alone. |
| `notes` | Human-readable notes (Markdown or text). |
| `media` | Files the job uses (samples, clips, preset files). SHOULD live under `media/`. |
| `render` | Derived output of the job (section 8). |
| `other` | Anything else the author wants to keep with the song. |

## 4. The job

The job is the same JSON document `wavelength render` takes (`docs/job-format.md`), with these rules
when it is part of a song.

### 4.1 Stable IDs

Tracks, buses and markers MAY carry an `"id"`: a string of `a-z 0-9 - _ .`, unique within its list.
When one is missing its ID is its `name`. Comments, diffs and `requires` refer to tracks by ID, so a
generator SHOULD write explicit IDs and keep them when it renames a track. Notes are identified by
their track, start beat and key; they carry no IDs.

### 4.2 References

Every file the job reads is a path inside the song folder that follows section 2, normally under
`media/`. Anything the song uses from outside the folder is referred to **by name**: a plugin, a
preset (`"preset"`), a sample library (`"multisample"`, `"kit"`, `"soundfont"`, `"sfz"` names) or one
file of a library (`"lib:Legend 909/Kick Legend 909 01 accent.wav"`), so the song does not depend on
where another computer keeps it.

Whether a value is a file or a name is decided by its key alone:

| Key (on a track, a fallback or an effect) | Is |
|---|---|
| `state` (or `state.file`) | a file; a `#<n>` suffix (`cart.syx#3`) picks a program inside it |
| sampler `sample`, clip `file`, kit `map` values | a file, unless it starts with `lib:` (a library file) or `*` (a built-in generator); a `map` value with no `/` is a name inside the kit's folder when the track has a `kit` (a library or a folder in the song) |
| sampler `multisample`, `sfz`, `soundfont` | a library name, unless it contains `/` or ends with `.multisample`, `.sfz`, `.sf2` or `.sf3`, then a file |
| sampler `kit` | a library name, unless it contains `/`, then a folder in the song |
| `deliver[].file` | a file the render writes: a path under section 2, relative to the render's output folder |

A renderer MUST check every path the job reads or writes against section 2 before opening it when
it renders a song from a package, and MUST refuse to render a job that would read or write outside
the song and its output folder.

A song SHOULD NOT copy third-party files into `media/` (factory presets, commercial samples) unless
their licence allows redistribution. Name them instead.

### 4.3 What a song needs

A track that plays a plugin or a named library SHOULD have a `fallback` (a stand-in sound that ends,
ideally, with a built-in instrument), so the song renders on computers without the original.
`wavelength pack` lists every such requirement in the manifest's `requires`, one entry per need:
`track` (its ID), `fallback` (whether it has one), and either `plugin` (`{name, format?, id?, version?}`
of the one that rendered it) with an optional `preset`, or `library` (`{kind, name}`: `kind` is
`multisample`, `kit`, `soundfont`, `sfz` or `files` for `lib:` files, `name` the library).

## 5. The package: `.wavelength`

A package is a ZIP file (APPNOTE 6.3.x), named `<slug>.wavelength`, media type
`application/vnd.wavelength.song+zip`.

- The first entry MUST be `mimetype`, stored (not compressed), without an extra field, holding exactly
  `application/vnd.wavelength.song+zip` in ASCII with no line ending. The second SHOULD be
  `wavelength.json`.
- Entries are stored or deflated. Text (JSON, Markdown, scripts) SHOULD be deflated; audio, images
  and other compressed data SHOULD be stored. Entry names are UTF-8, with flag bit 11 set when a
  name isn't ASCII. Entries MAY use data descriptors. Format 1.0 packages don't use ZIP64: a package
  is under 4 GiB with fewer than 65,535 entries, and a writer MUST fail rather than go past either.
- A package contains `wavelength.json`, the job, the files listed in `files`, `review.json` and
  `history/`, each at its path in the folder, and nothing else. `review.json`, `history/` and
  `render` files MAY be left out (`pack --no-review`, `--no-history`, `--no-render`); the manifest
  still lists the render, and readers treat a listed file that is absent as not included.
- A package MAY leave out a history object whose content is the file the package carries at the path a
  revision gives it (a sample that never changed would otherwise travel twice). A reader takes that
  object from the file, after checking its hash, and unpacking stores it in `history/objects/` again,
  so a song folder always has every object. A reader
  SHOULD warn about an entry that is not one of these and MAY ignore it; it MUST NOT refuse the
  package for it.
- Encryption, multi-disk archives and entries outside the rules of section 2 are not allowed.

**Readers** MUST refuse entries whose names break section 2 (absolute paths, `..` segments, names
that collide after case folding, `.git`), symbolic links and encrypted entries. They take each
entry's name and sizes from the central directory, MUST refuse an entry whose local header names a
different file, and MUST check each entry's CRC-32. They MUST limit the total unpacked size (history
objects included) and the compression ratio (zip bombs). They SHOULD accept a package whose
`mimetype` entry is missing, compressed or not first, identifying it by `wavelength.json`. Unpacking
and rendering a package MUST NOT execute anything inside it.

## 6. History

Revisions live in `history/` as immutable, content-addressed files and an append-only log. A
revision is a snapshot of the song's tracked files: the job and the files whose role is `source`,
`notes` or `media`. The manifest, renders, comments and `out/` are not tracked, so undoing a change
to the music never changes the song's title, licence or current render.

```
history/
  log.jsonl                          one revision per line, append-only
  objects/7e/7e3a0c...  (64 hex)     a file's content, zlib-compressed, named by the SHA-256 of the uncompressed content
```

**Objects.** `objects/<first two hex digits>/<64 hex digits>` holds the file's bytes compressed with
zlib (RFC 1950) as a single stream with nothing after it. The name is the SHA-256 of the uncompressed
bytes; readers MUST check it and MUST cap how much they decompress. Hashes are written
`"sha256:<64 lowercase hex digits>"` everywhere in a song; a reader that meets another prefix treats
the value as unsupported, not as corrupt.

**The log file.** `log.jsonl` is UTF-8 with one JSON object per line, lines ending in LF (a CR
before it is tolerated). Readers skip empty lines, and MAY ignore a last line with no LF (a write that
was interrupted) with a warning. One program writes the log at a time: a writer creates
`history/lock` exclusively before it appends or writes objects and removes it after; readers ignore
that file and packages never carry it.

**The log.** Each line is one JSON object:

```json
{"rev": 12, "time": "2026-09-27T10:12:44-04:00", "op": "render", "parent": 11,
 "by": {"name": "Claude Opus 5.5", "kind": "ai"}, "message": "Build holds the Train filter open",
 "files": {"job.json": "sha256:7e3a...", "make-job.py": "sha256:91b0...", "NOTES.md": "sha256:0c4d..."},
 "render": {"report": "sha256:55e1...", "mix": "sha256:a0d2...", "lufs": -12.3, "lra": 8.0, "truePeak": -1.0,
            "seconds": 204.5, "sections": [{"name": "Build", "lufs": -11.9}]}}
```

| Field | Meaning |
|---|---|
| `rev` | The revision number: 1 for the first, and greater than the line before (writers add one; pruning leaves gaps). |
| `time`, `by`, `message` | When, who (`{name, kind}`) and why. |
| `op` | `save` (a save point someone made; `named`: true when it has a message), `render` (a render: its files as they were rendered), `undo`, `redo`, `restore`. Readers treat an op they don't know like `save`. |
| `parent` | The revision before this one in the log (0 for the first), so the log is one line of history. |
| `files` | The full snapshot: every tracked path and its content hash. Reading a revision needs only its line and objects. |
| `render` | For `render` entries: the hashes of the report and of the mix file the render wrote (not necessarily the file later kept in `render/`), and their loudness summary. |
| `target` | For `undo`, `redo` and `restore`: the revision whose files were restored (the one that was current is `parent`). |

**Operations.** Undo, redo and restore never lose a revision: each makes the folder's tracked files
exactly the target revision's (writing its files and deleting tracked files the target doesn't have)
and appends an entry, so it can itself be undone. Unsaved edits in the folder are saved as a revision
before any of them runs. They follow the song's changes the way an editor does. Reading the log in
order with two stacks, `undo` and `redo`:

- an `undo` entry moves the top of `undo` to `redo`;
- a `redo` entry moves the top of `redo` back to `undo`;
- a `restore`, or any other entry whose files differ from the entry before it, is a change: its
  `rev` goes on `undo` and `redo` is cleared (the first entry is always a change);
- anything else (a `save` or `render` that changed nothing) leaves both stacks alone.

Undo restores the revision below the top of `undo`; redo restores the top of `redo`.
Every render appends a `render` entry, even when the files are unchanged (it costs one line: the
objects are already there), so each render a person listens to has a revision of its own. Content
that appears in many revisions is stored once.

**Pruning.** Tools MAY drop unnamed `render` revisions that no comment and no manifest `render`
points at, rewriting the log with their `rev` numbers kept (gaps are allowed) and deleting objects
no remaining revision uses.

**Git** (informative). A song never contains a git repository. `wavelength history --to-git` (a
repository) and `--bundle` (a git bundle) map each revision to one git commit: its tree is the
revision's files, its parent the commit of `parent` (none when that revision was pruned), author and
committer `by.name` with an empty e-mail, both dates `time`, and its message `message` (or the
operation, when there is none) followed by a blank line and `Wavelength-Revision: <rev>`. The same log
gives the same commits on every computer.

## 7. Comments: `review.json`

```json
{
  "comments": [
    {
      "id": "c260926212340b41",
      "created": "2026-09-26T21:23:40-04:00",
      "author": {"name": "Austin Ginder", "kind": "human"},
      "status": "open",
      "text": "The filtering on the Arp drowns out the build.",
      "anchor": {
        "revision": 11,
        "render": "sha256:55e1...",
        "time": [137.82, 144.67],
        "bars": [53, 56],
        "beats": [208, 224],
        "tracks": ["arp"],
        "notes": [{"track": "arp", "beat": 210.5, "key": 69}],
        "ref": "bars 53-56 · 2:17-2:24 · Build"
      },
      "replies": [
        {"author": {"name": "Claude Opus 5.5", "kind": "ai"}, "time": "2026-09-26T21:40:02-04:00", "revision": 12,
         "text": "Bars 53-56 now hold the Train filter open and the duck at 0."}
      ],
      "resolved": {"revision": 12, "time": "2026-09-26T21:40:02-04:00", "by": {"name": "Claude Opus 5.5", "kind": "ai"}}
    }
  ]
}
```

- A comment's `anchor` records what was heard: the revision and render (report hash) that were
  playing, the song time, bars and beats selected, and the tracks and notes picked. The anchor is
  never rewritten. `time` is `[start, end]` in seconds of the rendered mix (its lead-in included),
  `beats` `[start, end]` counted from the job's beat 0, `bars` the first and last bar, from 1.
  `tracks` and notes' `track` are track IDs (section 4.1). A reply's `time`, like `created`, is
  when it was written.
- Whether a comment is **outdated** (the anchored tracks or bars changed after `anchor.revision`) is
  computed by comparing that revision with the current one; it is not stored.
- `status` is `open` or `done` (readers treat a status they don't know as `open`); `resolved` says
  which revision addressed it. Replies may come from people or agents.
- Comments are not part of history: writing one never makes a revision.
- `id`, `status` and `text` are required, and ids are unique within the file. Tools SHOULD write `created`, `author` and
  `anchor.revision` on a comment and `author` on a reply.
- Readers MUST accept comments written before this spec: their anchor fields sit on the comment itself,
  their single `reply` is a string, and they have no author or revision.

## 8. The render

`render/` holds the render that goes with the song, so it can be heard without rendering:

| File | |
|---|---|
| `render/mix.mp3` (or `.flac`, `.wav`) | The mix. |
| `render/song.png` | The song picture (`render --png`). |
| `render/report.json` | The render report, which names the revision and job it came from: `"song": {"revision": 14, "job": "sha256:..."}`. |

The manifest's `render` names these files (`mix` is required, `picture` and `report` optional), and
SHOULD name the `revision` and the job hash (`"sha256:..."`) it was rendered from. A reader can tell
whether the render is current by comparing that hash with the job's.

## 9. Security

A song is untrusted input. Implementations:

- MUST NOT run `source` files, or any other file in a song, to open, validate, render or unpack it.
- MUST apply the path rules of section 2 to every path in the manifest, the job, comments and history,
  and the entry rules of section 5 to packages.
- MUST NOT create or read a git repository inside a song folder; history export writes git data only
  where the user asks.
- SHOULD check `sha256` values where a manifest or log gives them.
- Load only plugins already installed on the computer; a package never contains plugin binaries.

## 10. Versions and extensions

- Versions are `"major.minor"`, compared as two numbers (`1.10` is newer than `1.9`).
- A minor version only adds optional fields and new values of open lists. Readers MUST ignore object
  keys they don't know and SHOULD keep them when they rewrite a file. These lists are open, and a
  reader treats a value it doesn't know as shown: file `role` (`other`), history `op` (`save`),
  comment `status` (`open`), author `kind` (`human`), `requires` entries of a kind it doesn't know
  (ignored).
- A major version may change meaning. A reader refuses a song whose `minReaderVersion` (by default
  `formatVersion`'s major with `.0`) is newer than the newest version it supports, rather than guess.
- Tools put their own data in `metadata` under reverse-domain keys (`"run.wavelength.site"`,
  `"com.example.daw"`). An extension that changes how a song must be read goes in `extensions` under
  the same kind of key, and in `extensionsRequired` when a reader that doesn't know it MUST refuse
  the song.

## 11. Conformance

- `wavelength validate <song | package>` checks a song against this spec: paths, the manifest, the
  job's paths, the log's order, object hashes, comments, and the package's entry rules.
- JSON Schemas: `schemas/wavelength.schema.json` (manifest), `schemas/review.schema.json`,
  `schemas/history-entry.schema.json`.
- Test songs from `scripts/make-song-fixtures.py`: a valid folder and package, packages every reader
  must refuse (path traversal, absolute paths, symbolic links, `.git`, a bad hash, names that collide
  by case, newer formats, an unknown required extension, a duplicate key, a job that writes outside
  its folder) and ones it must accept (with objects left out as files, with a warning).
- A second, independent reader (Python, in `scripts/wavelength_song.py`) validates and unpacks
  packages without the engine, so the spec never depends on one implementation.

## Changes

- Draft 2 (2026-09-27): after an independent review. Stricter, portable names (no Windows-reserved
  names, case folding, no trailing `/`); files versus names decided by key; `deliver` paths and
  render-time path checks; I-JSON; ZIP details (no ZIP64 in 1.0, central-directory names, CRC-32);
  log file rules and a lock; undo and redo written as an algorithm, and restoring deletes files the
  target doesn't have; open value lists; numeric versions; comment anchor units; git mapping
  informative.
- Draft 1 (2026-09-27): first draft for review.
