# Wavelength song format

**Draft 1** (2026-09-27), for review. Format version `1.0`. This document is licensed CC BY 4.0; the JSON
Schemas it names are CC0. Anyone may read and write Wavelength songs without asking, paying or
using Wavelength's code.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

## 1. Overview

A **song** is a folder of open files: the job that renders it, the files the job uses, the scripts
and notes that made it, the latest render, its revision history and the comments on it. A
**`.wavelength` file** is that folder packed into a zip for sharing. Unpacking a package gives a song
folder; packing that folder again gives the same package.

Goals, in order:

1. **Open.** JSON and plain text inside, a published spec and schema, no opaque blobs required to
   read or render a song, nothing in the format that only one program can interpret.
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
separator. Paths MUST NOT be absolute, MUST NOT contain empty, `.` or `..` segments and MUST
resolve inside the folder. Names are UTF-8 in Unicode NFC, at most 255 bytes per segment, and unique when compared
case-insensitively (the folder has to survive case-insensitive file systems). Entries MUST NOT be
symbolic links, and nothing in a song may be named `.git`.

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
| `files` | optional | Every other file that belongs to the song (the job, `review.json` and `history/` are implied), each `{path, role, mediaType?, size?, sha256?}`. |
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

Every file reference in the job (a `state` file, a sampler `sample`, `sfz` or `soundfont` path, a
clip `file`) MUST be a relative path inside the song folder, normally under `media/`. Anything the
song uses from outside the folder is referred to **by name**: a plugin, a preset (`"preset"`), a
sample library (`"multisample"`, `"kit"`, `"soundfont"` names) or one file of a library
(`"lib:Legend 909/Kick Legend 909 01 accent.wav"`: a `lib:` name is never a file of the song), so
the song does not depend on where another computer keeps it.

A song SHOULD NOT copy third-party files into `media/` (factory presets, commercial samples) unless
their licence allows redistribution. Name them instead.

### 4.3 What a song needs

A track that plays a plugin or a named library SHOULD have a `fallback` (a stand-in sound that ends,
ideally, with a built-in instrument), so the song renders on computers without the original.
`wavelength pack` lists every such requirement in the manifest's `requires`: the track, the plugin's
identity (`name`, `format`, `id`, `version` of the one that rendered it), the preset or library, and
whether a fallback exists.

## 5. The package: `.wavelength`

A package is a ZIP file (APPNOTE 6.3.x), named `<slug>.wavelength`, media type
`application/vnd.wavelength.song+zip`.

- The first entry MUST be `mimetype`, stored (not compressed), without an extra field, holding exactly
  `application/vnd.wavelength.song+zip` in ASCII with no line ending. The second SHOULD be
  `wavelength.json`.
- Entries are stored or deflated. Text (JSON, Markdown, scripts) SHOULD be deflated; audio, images
  and other compressed data SHOULD be stored. ZIP64 MAY be used. Entry names are UTF-8 (flag bit 11).
- A package contains `wavelength.json`, the job, the files listed in `files`, `review.json` and
  `history/`, each at its path in the folder, and nothing else. `review.json`, `history/` and
  `render` files MAY be left out (`pack --no-review`, `--no-history`, `--no-render`); the manifest
  still lists the render, and readers treat a listed file that is absent as not included. A reader
  SHOULD warn about an entry that is not one of these and MAY ignore it; it MUST NOT refuse the
  package for it.
- Encryption, multi-disk archives and entries outside the rules of section 2 are not allowed.

**Readers** MUST refuse entries with absolute paths, `..` segments, symbolic links, names that
collide case-insensitively, or any path segment named `.git`. They SHOULD limit the total unpacked
size and the compression ratio (zip bombs). They SHOULD accept a package whose `mimetype` entry is
missing or not first, identifying it by `wavelength.json`. Unpacking and rendering a package MUST NOT
execute anything inside it.

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
zlib (RFC 1950). The name is the SHA-256 of the uncompressed bytes; readers MUST check it.

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
| `rev` | 1, 2, 3...: the revision number, one more than the line before. |
| `time`, `by`, `message` | When, who (`{name, kind}`) and why. |
| `op` | `save` (a save point someone made; `named`: true when it has a message), `render` (a render: its files as they were rendered), `undo`, `redo`, `restore`. |
| `parent` | The revision before this one in the log (0 for the first), so the log is one line of history. |
| `files` | The full snapshot: every tracked path and its content hash. Reading a revision needs only its line and objects. |
| `render` | For `render` entries: hashes of the report and mix that were made, and their loudness summary. |
| `target` | For `undo`, `redo` and `restore`: the revision whose files were restored (the one that was current is `parent`). |

**Operations.** Undo, redo and restore never remove anything: each writes the target revision's files
into the folder and appends an entry, so it can itself be undone. They follow the song's changes the
way an editor does: a `save` or `render` whose files differ from the previous state is a change (one
that changed nothing is not), and a `restore` is one. Undo returns to the state before the latest
change; redo reapplies what undo took back, until the next change. Unsaved edits in the folder are
saved as a revision before any of them runs.
Every render appends a `render` entry, even when the files are unchanged (it costs one line: the
objects are already there), so each render a person listens to has a revision of its own. Content
that appears in many revisions is stored once.

**Pruning.** Tools MAY drop unnamed `render` revisions that no comment and no manifest `render`
points at, rewriting the log with their `rev` numbers kept (gaps are allowed) and deleting objects
no remaining revision uses.

**Git.** A song never contains a git repository. Each revision maps to one git commit: its tree is
the revision's files, its parent the commit of `parent`, author and committer `by.name` with an empty
e-mail, both dates `time`, and its message `message` followed by a blank line and
`Wavelength-Revision: <rev>`. The mapping is deterministic, so `wavelength history --to-git` (a
repository) and `--bundle` (a git bundle) give the same commits on every computer.

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
  never rewritten.
- Whether a comment is **outdated** (the anchored tracks or bars changed after `anchor.revision`) is
  computed by comparing that revision with the current one; it is not stored.
- `status` is `open` or `done`; `resolved` says which revision addressed it. Replies may come from
  people or agents.
- Comments are not part of history: writing one never makes a revision.
- `id`, `status` and `text` are required. Tools SHOULD write `created`, `author` and
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

- A minor version only adds optional fields and file roles. Readers MUST ignore object keys they
  don't know and SHOULD keep them when they rewrite a file.
- A major version may change meaning. A reader that doesn't support `minReaderVersion` MUST refuse
  the song rather than guess.
- Tools put their own data in `metadata` under reverse-domain keys (`"run.wavelength.site"`,
  `"com.example.daw"`). An extension that changes how a song must be read goes in `extensions` under
  the same kind of key, and in `extensionsRequired` when a reader that doesn't know it MUST refuse
  the song.

## 11. Conformance

- `wavelength validate <song | package>` checks a song against this spec: paths, the manifest's JSON
  Schema, the job, object hashes, and the package's entry rules.
- JSON Schemas: `schemas/wavelength.schema.json` (manifest), `schemas/review.schema.json`,
  `schemas/history-entry.schema.json`.
- Test songs from `scripts/make-song-fixtures.py`: a valid folder and package, packages every reader
  must refuse (path traversal, absolute paths, symbolic links, `.git`, a bad hash, names that collide
  by case, a newer format) and one it must accept with a warning.
- A second, independent reader (Python, in `scripts/wavelength_song.py`) validates and unpacks
  packages without the engine, so the spec never depends on one implementation.

## Changes

- Draft 1 (2026-09-27): first draft for review.
