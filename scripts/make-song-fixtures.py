#!/usr/bin/env python3
"""Write test fixtures for the Wavelength song format (docs/song-format.md, Draft 1).

Usage: make-song-fixtures.py DIR

Writes into DIR:
  good/                      a minimal valid song folder (slug "good")
  good.wavelength            the same song as a valid package
  bad-traversal.wavelength   an entry "../evil.txt"
  bad-absolute.wavelength    an entry "/tmp/evil.txt"
  bad-symlink.wavelength     a symbolic-link entry
  bad-git.wavelength         an entry under .git/
  bad-hash.wavelength        a history object whose content doesn't match its name
  bad-case.wavelength        "Job.json" next to "job.json"
  bad-version.wavelength     minReaderVersion "2.0"
  bad-gitdot.wavelength      an entry under ".git." (Windows opens it as .git)
  warn-extra.wavelength      an entry the manifest doesn't list: valid, with a warning

Every bad package is the good one with exactly one problem, and every reader
MUST refuse it. Every warn package MUST be accepted. Standard library only; the output is the same on every run.
"""

import hashlib
import json
import os
import shutil
import struct
import sys
import zipfile
import zlib

MEDIA_TYPE = "application/vnd.wavelength.song+zip"
SONG_ID = "3f0c2a4e-8b1d-4c6e-9f7a-2d5b8e1c0a93"
ZIP_DATE = (2026, 9, 27, 12, 0, 0)
STORED_EXT = (".wav", ".mp3", ".flac", ".png")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def as_json(obj):
    return (json.dumps(obj, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def silent_wav(seconds=0.1, rate=48000, channels=2):
    """A short 16-bit PCM WAV of silence."""
    frames = int(seconds * rate)
    data = b"\x00\x00" * channels * frames
    fmt = struct.pack("<HHIIHH", 1, channels, rate, rate * channels * 2, channels * 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def job(cutoff):
    return {
        "tempo": 120,
        "timeSignature": [4, 4],
        "markers": [{"id": "intro", "beat": 0, "name": "Intro"}],
        "tracks": [{
            "id": "lead",
            "name": "Lead",
            "plugin": "builtin:synth",
            "preset": "BA Pluck",
            "params": {"cutoff": cutoff},
            "notes": [
                {"beat": 0, "dur": 1, "key": "C4", "vel": 0.8},
                {"beat": 1, "dur": 1, "key": "E4", "vel": 0.8},
                {"beat": 2, "dur": 1, "key": "G4", "vel": 0.8},
                {"beat": 3, "dur": 1, "key": "C5", "vel": 0.9},
            ],
        }],
    }


def build_song():
    """The good song: {path: bytes}, in folder layout."""
    job1 = as_json(job(0.4))
    job2 = as_json(job(0.6))
    notes = (b"# Good Fixture\n\nA four-note arpeggio on builtin:synth, used to test readers of the\n"
             b"Wavelength song format.\n\n| Track | Sound |\n|---|---|\n| lead | builtin:synth, BA Pluck |\n")
    source = (b"# How job.json was made. Informational only: readers never run this.\n"
              b"import json\n\njson.dump({\"tracks\": []}, open(\"job.json\", \"w\"))\n")
    mix = silent_wav()
    report = as_json({"song": {"revision": 2, "job": "sha256:" + sha(job2)},
                      "mix": {"lufs": -70.0, "truePeakDb": -70.0}})

    objects = {}

    def track(content):
        digest = sha(content)
        objects[digest] = zlib.compress(content, 9)
        return "sha256:" + digest

    snapshot1 = {"job.json": track(job1), "make-job.py": track(source), "NOTES.md": track(notes)}
    snapshot2 = {"job.json": track(job2), "make-job.py": track(source), "NOTES.md": track(notes)}
    old_report = "sha256:" + sha(b"the report of the first render")
    old_mix = "sha256:" + sha(b"the mix of the first render")
    log = [
        {"rev": 1, "time": "2026-09-27T10:00:00-04:00", "op": "render", "parent": 0,
         "by": {"name": "Fixture Writer", "kind": "ai"}, "message": "First sketch",
         "files": snapshot1,
         "render": {"report": old_report, "mix": old_mix, "lufs": -70.0, "lra": 0.0, "truePeak": -70.0,
                    "seconds": 0.1, "sections": [{"name": "Intro", "lufs": -70.0}]}},
        {"rev": 2, "time": "2026-09-27T10:05:00-04:00", "op": "render", "parent": 1,
         "by": {"name": "Fixture Writer", "kind": "ai"}, "message": "Open the filter",
         "files": snapshot2,
         "render": {"report": "sha256:" + sha(report), "mix": "sha256:" + sha(mix), "lufs": -70.0, "lra": 0.0,
                    "truePeak": -70.0, "seconds": 0.1, "sections": [{"name": "Intro", "lufs": -70.0}]}},
    ]
    log_bytes = "".join(json.dumps(e, separators=(",", ":"), ensure_ascii=False) + "\n" for e in log).encode("utf-8")

    review = {"comments": [
        {"id": "c260927100200a01", "created": "2026-09-27T10:02:00-04:00",
         "author": {"name": "Test Listener", "kind": "human"}, "status": "done",
         "text": "The pluck is too dull.",
         "anchor": {"revision": 1, "render": old_report, "time": [0.0, 2.0], "bars": [1, 1], "beats": [0, 4],
                    "tracks": ["lead"], "notes": [{"track": "lead", "beat": 0, "key": 60}],
                    "ref": "bar 1 · 0:00-0:02 · Intro"},
         "replies": [{"author": {"name": "Fixture Writer", "kind": "ai"}, "time": "2026-09-27T10:05:00-04:00",
                      "revision": 2, "text": "Cutoff 0.4 -> 0.6."}],
         "resolved": {"revision": 2, "time": "2026-09-27T10:05:00-04:00",
                      "by": {"name": "Fixture Writer", "kind": "ai"}}},
        {"id": "c260927100600b02", "created": "2026-09-27T10:06:00-0400", "status": "open",
         "text": "A comment in the shape written before the spec.",
         "ref": "bar 1 · 0:00-0:02 · Lead", "bars": [1, 1], "beats": [0, 4], "time": [0.0, 2.0],
         "tracks": ["Lead"],
         "notes": [{"track": "Lead", "key": "C4", "midi": 60, "bar": "1.1", "beat": 0, "dur": 1, "vel": 0.8}],
         "render": 1790500000, "reply": "Noted; leaving it open."},
    ]}

    files = {
        "job.json": job2,
        "NOTES.md": notes,
        "make-job.py": source,
        "render/mix.wav": mix,
        "render/report.json": report,
    }
    manifest = {
        "format": "wavelength.song",
        "formatVersion": "1.0",
        "minReaderVersion": "1.0",
        "generator": {"name": "make-song-fixtures", "version": "1"},
        "id": SONG_ID,
        "title": "Good Fixture",
        "slug": "good",
        "created": "2026-09-27T10:00:00-04:00",
        "updated": "2026-09-27T10:05:00-04:00",
        "authors": [{"name": "Fixture Writer", "role": "composer", "kind": "ai"}],
        "summary": "Four notes that every Wavelength reader must accept.",
        "license": "CC0-1.0",
        "tags": ["fixture"],
        "job": "job.json",
        "files": [
            {"path": "make-job.py", "role": "source", "mediaType": "text/x-python"},
            {"path": "NOTES.md", "role": "notes", "mediaType": "text/markdown", "size": len(notes),
             "sha256": sha(notes)},
            {"path": "render/mix.wav", "role": "render", "mediaType": "audio/wav", "size": len(mix),
             "sha256": sha(mix)},
            {"path": "render/report.json", "role": "render", "mediaType": "application/json"},
        ],
        "render": {"revision": 2, "job": "sha256:" + sha(job2), "mix": "render/mix.wav",
                   "report": "render/report.json"},
        "extensions": {},
        "extensionsRequired": [],
        "metadata": {"run.wavelength.fixtures": {"purpose": "reader tests"}},
    }
    song = {"wavelength.json": as_json(manifest)}
    song.update(files)
    song["review.json"] = as_json(review)
    song["history/log.jsonl"] = log_bytes
    for digest, blob in sorted(objects.items()):
        song["history/objects/%s/%s" % (digest[:2], digest)] = blob
    return song


def write_folder(root, song):
    if os.path.exists(root):
        shutil.rmtree(root)
    for rel, data in song.items():
        path = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


def entry(name, compress, mode=0o100644):
    info = zipfile.ZipInfo(name, date_time=ZIP_DATE)
    info.compress_type = compress
    info.create_system = 3
    info.external_attr = mode << 16
    info.filename = name   # keep the name exactly as given (no clean-up by zipfile)
    return info


def write_package(path, song, extra=()):
    """mimetype first (stored, no extra field), then wavelength.json, then the rest sorted.

    extra: (name, bytes, mode) entries appended at the end, written as given.
    """
    if os.path.exists(path):
        os.remove(path)
    order = ["wavelength.json"] + sorted(k for k in song if k != "wavelength.json")
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(entry("mimetype", zipfile.ZIP_STORED), MEDIA_TYPE.encode("ascii"))
        for name in order:
            stored = name.lower().endswith(STORED_EXT) or name.startswith("history/objects/")
            z.writestr(entry(name, zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED), song[name])
        for name, data, mode in extra:
            z.writestr(entry(name, zipfile.ZIP_DEFLATED, mode), data)
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
    for name, _, _ in extra:
        assert name in names, "zipfile changed the entry name %r" % name
    return path


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    song = build_song()
    written = []

    write_folder(os.path.join(out, "good"), song)
    written.append(("good/", "valid song folder, %d files" % len(song)))
    write_package(os.path.join(out, "good.wavelength"), song)
    written.append(("good.wavelength", "valid package"))

    evil = b"this file must never be written\n"
    bad = [
        ("bad-traversal.wavelength", song, [("../evil.txt", evil, 0o100644)], "entry ../evil.txt"),
        ("bad-absolute.wavelength", song, [("/tmp/evil.txt", evil, 0o100644)], "entry /tmp/evil.txt"),
        ("bad-symlink.wavelength", song, [("media/link.wav", b"../../../../etc/passwd", 0o120777)],
         "symbolic-link entry media/link.wav"),
        ("bad-git.wavelength", song, [(".git/HEAD", b"ref: refs/heads/main\n", 0o100644)], "entry .git/HEAD"),
        ("bad-case.wavelength", song, [("Job.json", song["job.json"], 0o100644)], "Job.json next to job.json"),
        ("bad-gitdot.wavelength", song, [(".git./config", b"[core]\n", 0o100644)], "entry .git./config"),
        ("warn-extra.wavelength", song, [("scratch/notes.txt", b"not listed\n", 0o100644)], "unlisted entry (warning only)"),
    ]
    for name, contents, extra, what in bad:
        write_package(os.path.join(out, name), contents, extra)
        written.append((name, what))

    tampered = dict(song)
    first = sorted(k for k in song if k.startswith("history/objects/"))[0]
    tampered[first] = zlib.compress(b"not the content this object is named after\n", 9)
    write_package(os.path.join(out, "bad-hash.wavelength"), tampered)
    written.append(("bad-hash.wavelength", "%s content doesn't match its name" % first))

    versioned = dict(song)
    manifest = json.loads(song["wavelength.json"])
    manifest["minReaderVersion"] = "2.0"
    versioned["wavelength.json"] = as_json(manifest)
    write_package(os.path.join(out, "bad-version.wavelength"), versioned)
    written.append(("bad-version.wavelength", "minReaderVersion 2.0"))

    print("wrote to %s:" % out)
    for name, what in written:
        print("  %-26s %s" % (name, what))
    return 0


if __name__ == "__main__":
    sys.exit(main())
