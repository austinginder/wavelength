#!/usr/bin/env python3
"""Read, validate and unpack Wavelength songs without the engine.

A second, independent implementation of the Wavelength song format
(docs/song-format.md, Draft 2, format version 1.0). Python 3.9+, standard
library only. It never runs, imports or executes anything a song contains.

Commands:
  validate <song-folder | file.wavelength> [--json]
  unpack   <file.wavelength> [--out DIR] [--force]
  info     <song | package> [--json]
  history  <song | package> [--json]
  cat      <song | package> <rev> <path>

Every problem has a severity (error or warning), the path it is about, a
message and a short stable code (for example path-traversal, symlink,
case-collision, object-hash, reader-version), so other implementations can be
compared with this one. validate exits 1 when there are errors.

In a song folder the rules of section 2 apply to the song's files (the manifest,
the job, listed and referenced files, review.json, history/); other files such as
scratch renders in out/ are not part of the song and are not checked.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import shutil
import struct
import sys
import tempfile
import unicodedata
import zipfile
import zlib

READER_VERSION = (1, 0)
FORMAT = "wavelength.song"
PACKAGE_MEDIA_TYPE = "application/vnd.wavelength.song+zip"
MANIFEST = "wavelength.json"
REVIEW = "review.json"
LOG = "history/log.jsonl"
OBJECTS = "history/objects/"

LOCK = "history/lock"

# Open lists (section 10): a value this reader doesn't know is read as the default, with a warning.
ROLES = ("source", "notes", "media", "render", "other")
TRACKED_ROLES = ("source", "notes", "media")
OPS = ("save", "render", "undo", "redo", "restore")
KINDS = ("human", "ai")
STATUSES = ("open", "done")
LIBRARY_KINDS = ("multisample", "kit", "soundfont", "sfz", "files")
LEGACY_FIELDS = ("ref", "bars", "beats", "time", "tracks", "notes", "render", "reply")
NEW_FIELDS = ("anchor", "replies", "resolved")

DEFAULT_MAX_TOTAL = 2 * 1024 ** 3   # total unpacked bytes of a package
DEFAULT_MAX_RATIO = 100             # per-entry compression ratio ...
RATIO_FLOOR = 1024 ** 2             # ... checked for entries larger than this
MAX_ENTRIES = 100_000
MAX_JSON = 64 * 1024 ** 2           # wavelength.json, the job, review.json, report.json
MAX_LOG = 256 * 1024 ** 2

LIB_PREFIX = "lib:"
GENERATOR_PREFIX = "*"
LIBRARY_FILE_EXT = (".multisample", ".sfz", ".sf2", ".sf3", ".exs", ".dspreset", ".dsbundle", ".dslibrary")   # a sampler library value ending so is a file
IR_FILE_EXT = (".wav", ".aif", ".aiff", ".caf", ".flac", ".mp3", ".ogg", ".sdir")   # an effect "ir" ending so is a file
BOM = b"\xef\xbb\xbf"

# Patterns are matched with re.fullmatch (a bare "$" would accept a trailing newline).
VERSION_RE = r"([0-9]+)\.([0-9]+)"
SLUG_RE = r"[a-z0-9]+(?:-[a-z0-9]+)*"
UUID_RE = r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
SHA256_RE = r"[0-9a-f]{64}"
HASHREF_RE = r"sha256:[0-9a-f]{64}"
OTHER_HASH_RE = r"(?!sha256:)[A-Za-z0-9][A-Za-z0-9+._-]*:.+"   # another algorithm: unsupported, not corrupt
DEVICE_RE = r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]"
WINDOWS_CHARS = '<>:"|?*'
RFC3339_RE = (r"([0-9]{4})-([0-9]{2})-([0-9]{2})[Tt]([0-9]{2}):([0-9]{2}):([0-9]{2})(\.[0-9]+)?"
              r"(?:[Zz]|[+-]([0-9]{2}):([0-9]{2}))")
REVERSE_DOMAIN_RE = r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)+"
ID_RE = r"[a-z0-9._-]+"
MEDIA_TYPE_RE = r"[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]*/[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]*(?:\s*;.*)?"
OBJECT_RE = r"history/objects/([0-9a-f]{2})/([0-9a-f]{64})"
SPDX_ID_RE = (r"(?:DocumentRef-[A-Za-z0-9.-]+:)?LicenseRef-[A-Za-z0-9.-]+"
              r"|[A-Za-z0-9][A-Za-z0-9.-]*\+?")


# ---------------------------------------------------------------------------
# Problems


class Problems:
    """Collects problems: dicts with severity, path, message and code."""

    def __init__(self):
        self.items = []

    def add(self, severity, path, message, code):
        self.items.append({"severity": severity, "path": path, "message": message, "code": code})

    def error(self, path, message, code):
        self.add("error", path, message, code)

    def warn(self, path, message, code):
        self.add("warning", path, message, code)

    @property
    def errors(self):
        return [p for p in self.items if p["severity"] == "error"]

    @property
    def warnings(self):
        return [p for p in self.items if p["severity"] == "warning"]


class Refused(Exception):
    """The song must be refused (unsupported version or required extension)."""


class ReadProblem(Exception):
    """A file in the song could not be read."""


def printable(text):
    """Escape control characters so names from a package can't drive the terminal."""
    return "".join(c if (c.isprintable() or c == " ") else "\\x%02x" % ord(c) if ord(c) < 256
                   else "\\u%04x" % ord(c) for c in str(text))


# ---------------------------------------------------------------------------
# Small helpers


def is_int(v):
    """An integer as JSON Schema sees it (14 and 14.0, never true/false)."""
    if isinstance(v, bool):
        return False
    return isinstance(v, int) or (isinstance(v, float) and v.is_integer())


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def matches(pattern, v):
    return isinstance(v, str) and re.fullmatch(pattern, v) is not None


def is_rfc3339(v):
    """True for an RFC 3339 date-time with sane field ranges."""
    m = re.fullmatch(RFC3339_RE, v) if isinstance(v, str) else None
    if not m:
        return False
    month, day, hour, minute, second = (int(m.group(i)) for i in range(2, 7))
    oh = int(m.group(8)) if m.group(8) else 0
    om = int(m.group(9)) if m.group(9) else 0
    return (1 <= month <= 12 and 1 <= day <= 31 and hour <= 23 and minute <= 59 and second <= 60
            and oh <= 23 and om <= 59)


def parse_version(v):
    m = re.fullmatch(VERSION_RE, v) if isinstance(v, str) else None
    return (int(m.group(1)), int(m.group(2))) if m else None


def is_spdx(expr):
    """Syntax check of an SPDX license expression (ids are not checked against the list)."""
    tokens = re.findall(r"\(|\)|[^\s()]+", expr)
    if not tokens or "".join(tokens) != re.sub(r"\s+", "", expr):
        return False
    pos = [0]

    def peek():
        return tokens[pos[0]] if pos[0] < len(tokens) else None

    def take():
        pos[0] += 1
        return tokens[pos[0] - 1]

    def term():
        t = peek()
        if t == "(":
            take()
            if not expression() or peek() != ")":
                return False
            take()
            return True
        if t is None or t in ("AND", "OR", "WITH", ")") or not re.fullmatch(SPDX_ID_RE, t):
            return False
        take()
        if peek() == "WITH":
            take()
            e = peek()
            if e is None or e in ("AND", "OR", "WITH", "(", ")") or not re.fullmatch(SPDX_ID_RE, e):
                return False
            take()
        return True

    def expression():
        if not term():
            return False
        while peek() in ("AND", "OR"):
            take()
            if not term():
                return False
        return True

    return expression() and pos[0] == len(tokens)


def fold(path):
    """Key for case-insensitive (and normalization-insensitive) name comparison."""
    return unicodedata.normalize("NFC", unicodedata.normalize("NFC", path).casefold())


def path_problems(p):
    """Check one relative path against the rules of section 2.

    Returns a list of (code, message) pairs; empty when the path is fine.
    """
    if not isinstance(p, str) or not p:
        return [("path-invalid", "is not a non-empty string")]
    try:
        p.encode("utf-8")
    except UnicodeEncodeError:
        return [("path-invalid", "is not valid Unicode")]
    found = []

    def add(code, message):
        if all(c != code for c, _ in found):
            found.append((code, message))

    if "\\" in p:
        add("path-backslash", "contains a backslash; the separator is '/'")
    if p.startswith(("/", "~")) or re.match(r"[A-Za-z]:", p):
        add("path-absolute", "is absolute; paths are relative to the song folder")
    segments = p.split("/")
    if p.startswith("/"):
        segments = segments[1:]
    if len(p) > 1 and p.endswith("/"):
        add("path-trailing-slash", "ends with '/'")
        segments = segments[:-1]
    for seg in segments:
        if seg == "..":
            add("path-traversal", "has a '..' segment")
        elif seg in ("", "."):
            add("path-segment", "has an empty or '.' segment")
        else:
            if any(c in WINDOWS_CHARS for c in seg):
                add("path-char", 'has a name with one of < > : " | ? *')
            if seg.endswith((" ", ".")):
                add("path-trailing", "has a name that ends with a space or '.'")
            if re.fullmatch(DEVICE_RE, seg.split(".")[0].rstrip(" "), re.I | re.A):
                add("path-device", "has a Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9)")
        if seg.rstrip(" .").casefold() == ".git" or seg.casefold() == "git~1":
            add("path-git", "has a segment named .git")
        if len(seg.encode("utf-8")) > 255:
            add("path-length", "has a segment longer than 255 bytes")
        if any(ord(c) < 0x20 or ord(c) == 0x7F for c in seg):
            add("path-control", "contains a control character")
    if unicodedata.normalize("NFC", p) != p:
        add("path-nfc", "is not in Unicode NFC")
    return found


def hash_ref(v):
    """"ok" for sha256:<64 lowercase hex>, "unsupported" for another algorithm's prefix, else "invalid"."""
    if matches(HASHREF_RE, v):
        return "ok"
    return "unsupported" if matches(OTHER_HASH_RE, v) else "invalid"


# ---------------------------------------------------------------------------
# JSON


class JsonProblem(ValueError):
    def __init__(self, message, code=None):
        super().__init__(message)
        self.code = code


def _pairs(pairs):
    obj = {}
    for k, v in pairs:
        if k in obj:
            raise JsonProblem("has the key %r twice (I-JSON)" % k, "json-duplicate-key")
        obj[k] = v
    return obj


def _constant(name):
    raise JsonProblem("uses %s, which is not JSON" % name)


def parse_json(data):
    """I-JSON (RFC 7493): UTF-8 without a BOM, no NaN/Infinity, no duplicate keys."""
    if isinstance(data, bytes):
        if data.startswith(BOM):
            raise JsonProblem("starts with a byte order mark (I-JSON)", "json-bom")
        try:
            data = data.decode("utf-8")
        except UnicodeDecodeError as e:
            raise JsonProblem("is not UTF-8 (%s at byte %d)" % (e.reason, e.start))
    try:
        return json.loads(data, object_pairs_hook=_pairs, parse_constant=_constant)
    except json.JSONDecodeError as e:
        raise JsonProblem("is not valid JSON: %s at line %d column %d" % (e.msg, e.lineno, e.colno))
    except RecursionError:
        raise JsonProblem("is nested too deeply")


# ---------------------------------------------------------------------------
# Song trees: a folder on disk or a package in memory, behind one interface


class FolderTree:
    """A song folder. Only the regular files the scan found are visible."""

    kind = "folder"

    def __init__(self, root, files, issues=()):
        self.root = root
        self.issues = list(issues)      # section 2 problems, reported for the song's files only
        self.digests = {}
        self.names = set(files)
        self.by_fold = {fold(f): f for f in files}
        self.dirs = {posixpath.dirname(f) for f in files}
        for d in list(self.dirs):
            while d:
                d = posixpath.dirname(d)
                self.dirs.add(d)

    def exists(self, rel):
        return rel in self.names

    def is_dir(self, rel):
        return rel in self.dirs

    def case_variant(self, rel):
        v = self.by_fold.get(fold(rel)) if isinstance(rel, str) else None
        return v if v and v != rel else None

    def list(self):
        return sorted(self.names)

    def _abs(self, rel):
        return os.path.join(self.root, *rel.split("/"))

    def size(self, rel):
        return os.lstat(self._abs(rel)).st_size

    def open(self, rel):
        try:
            fd = os.open(self._abs(rel), os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            return os.fdopen(fd, "rb")
        except OSError as e:
            raise ReadProblem("can't be read: %s" % e.strerror)

    def close(self):
        pass


class ZipTree:
    """The file entries of a package that passed the entry rules."""

    kind = "package"

    def __init__(self, zf, entries, dir_entries, digests=None):
        self.zf = zf
        self.entries = entries          # name -> ZipInfo, regular files only
        self.dir_entries = dir_entries  # explicit directory entries (names without the slash)
        self.digests = digests or {}    # name -> (sha256 hex, size), from the CRC-32 pass
        self.issues = []
        self.by_fold = {fold(n): n for n in entries}
        self.dirs = set(dir_entries)
        for n in entries:
            d = posixpath.dirname(n)
            while d:
                self.dirs.add(d)
                d = posixpath.dirname(d)

    def exists(self, rel):
        return rel in self.entries

    def is_dir(self, rel):
        return rel in self.dirs

    def case_variant(self, rel):
        v = self.by_fold.get(fold(rel)) if isinstance(rel, str) else None
        return v if v and v != rel else None

    def list(self):
        return sorted(self.entries)

    def size(self, rel):
        return self.entries[rel].file_size

    def open(self, rel):
        try:
            return self.zf.open(self.entries[rel])
        except (zipfile.BadZipFile, OSError, RuntimeError, NotImplementedError) as e:
            raise ReadProblem("can't be read: %s" % e)

    def close(self):
        self.zf.close()


def read_bytes(tree, rel, limit):
    """Read a whole file, refusing ones larger than limit."""
    try:
        with tree.open(rel) as f:
            data = f.read(limit + 1)
    except (zipfile.BadZipFile, OSError, zlib.error, EOFError) as e:
        raise ReadProblem("can't be read: %s" % e)
    if len(data) > limit:
        raise ReadProblem("is larger than %d bytes" % limit)
    return data


def hash_file(tree, rel):
    """SHA-256 and size of a file, read in chunks."""
    if rel in tree.digests:
        return tree.digests[rel]
    h = hashlib.sha256()
    n = 0
    try:
        with tree.open(rel) as f:
            while True:
                chunk = f.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
                n += len(chunk)
    except (zipfile.BadZipFile, OSError, zlib.error, EOFError) as e:
        raise ReadProblem("can't be read: %s" % e)
    tree.digests[rel] = (h.hexdigest(), n)
    return tree.digests[rel]


def load_json_file(tree, rel, problems, code, limit=MAX_JSON):
    """Read and parse a JSON file, reporting problems under code. None on failure."""
    try:
        return parse_json(read_bytes(tree, rel, limit))
    except JsonProblem as e:
        problems.error(rel, str(e), e.code or code)
    except ReadProblem as e:
        problems.error(rel, str(e), code)
    return None


# ---------------------------------------------------------------------------
# Opening songs: the folder scan and the package entry rules


def scan_folder(root, problems):
    """Walk a song folder without following links, noting where entries break section 2.

    Returns (files, issues): the relative paths of the regular files (symbolic
    links and .git are left out), and (path, code, message, other path) for each
    problem. Issues are reported by report_folder_issues, for the song's files only.
    """
    files = []
    issues = []
    seen = {}
    collided = set()

    def walk(rel_dir):
        abs_dir = os.path.join(root, *rel_dir.split("/")) if rel_dir else root
        try:
            with os.scandir(abs_dir) as it:
                entries = sorted(it, key=lambda e: e.name)
        except OSError as e:
            problems.error(rel_dir or ".", "can't be listed: %s" % e.strerror, "read-failed")
            return
        for e in entries:
            rel = rel_dir + "/" + e.name if rel_dir else e.name
            for code, message in path_problems(rel):
                issues.append((rel, code, message, None))
            key = fold(rel)
            if key in seen:
                if fold(rel_dir) not in collided:
                    issues.append((rel, "case-collision", "collides with %s when case is ignored" % seen[key], seen[key]))
                collided.add(key)
            else:
                seen[key] = rel
            if e.is_symlink():
                issues.append((rel, "symlink", "is a symbolic link", None))
                continue
            if e.name.rstrip(" .").casefold() == ".git":
                continue
            if e.is_dir(follow_symlinks=False):
                walk(rel)
            elif e.is_file(follow_symlinks=False):
                files.append(rel)
            else:
                issues.append((rel, "special-file", "is not a regular file or folder", None))

    walk("")
    return files, issues


def report_folder_issues(tree, song_paths, problems):
    """Report a folder's section 2 problems for the song's files and the folders that hold them.

    song_paths None reports everything (the manifest couldn't be read). A symbolic
    link or .git elsewhere in the folder is a warning; other files are not the song's.
    """
    if not tree.issues:
        return
    keys = None
    if song_paths is not None:
        keys = set()
        for p in song_paths:
            k = fold(p).rstrip("/")
            while k:
                keys.add(k)
                k = posixpath.dirname(k)

    def mine(rel):
        k = fold(rel)
        return keys is None or k in keys or k == "history" or k.startswith("history/")

    for rel, code, message, other in tree.issues:
        if mine(rel) or (other is not None and mine(other)):
            problems.error(rel, message, code)
        elif code in ("symlink", "path-git"):
            problems.warn(rel, message + " (not one of the song's files)", code)
    tree.issues = []


def local_header(fp, offset):
    """Parse the ZIP local file header at offset (the central directory can differ)."""
    fp.seek(offset)
    raw = fp.read(30)
    if len(raw) < 30 or raw[:4] != b"PK\x03\x04":
        return None
    _, _, flags, method, _, _, _, _, _, name_len, extra_len = struct.unpack("<4sHHHHHIIIHH", raw)
    name = fp.read(name_len)
    return {"flags": flags, "method": method, "name": name, "extra": fp.read(extra_len)}


def extra_ids(extra):
    """The header IDs in a ZIP extra field."""
    ids, i = set(), 0
    while i + 4 <= len(extra):
        hid, size = struct.unpack("<HH", extra[i:i + 4])
        ids.add(hid)
        i += 4 + size
    return ids


def has_zip64_end(fp):
    """True when the archive ends with a ZIP64 end of central directory locator."""
    fp.seek(0, 2)
    size = fp.tell()
    n = min(size, 0xFFFF + 22 + 20)
    fp.seek(size - n)
    tail = fp.read(n)
    at = tail.rfind(b"PK\x05\x06")
    return at >= 20 and tail[at - 20:at - 16] == b"PK\x06\x07"


def local_name_problem(info, lh):
    """Why the local header doesn't name the central directory's file, or None."""
    if lh is None:
        return "has no local file header where the central directory says"
    try:
        local = lh["name"].decode("utf-8" if info.flag_bits & 0x800 else "cp437")
    except UnicodeDecodeError:
        local = None
    if local != info.orig_filename:
        return "is named %r in its local header" % (local if local is not None else lh["name"])
    return None


def scan_package(path, problems, max_total=DEFAULT_MAX_TOTAL, max_ratio=DEFAULT_MAX_RATIO):
    """Apply the entry rules of section 5 to a package.

    Names and sizes come from the central directory. Every file entry that
    passes is read once to check its CRC-32 (its SHA-256 is kept for later).
    Returns a ZipTree of the entries that passed, or None when the package
    can't be read safely at all (not a ZIP, encrypted, too large, a zip bomb).
    """
    label = os.path.basename(path)
    try:
        zf = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, zipfile.LargeZipFile, OSError, UnicodeDecodeError, ValueError,
            NotImplementedError, EOFError) as e:
        problems.error(label, "is not a readable ZIP file: %s" % e, "zip-invalid")
        return None
    infos = zf.infolist()
    fatal = False
    if len(infos) > MAX_ENTRIES:
        problems.error(label, "has %d entries (limit %d)" % (len(infos), MAX_ENTRIES), "zip-too-large")
        zf.close()
        return None
    headers = {id(i): local_header(zf.fp, i.header_offset) for i in infos}
    zip64 = has_zip64_end(zf.fp) or any(0x0001 in extra_ids(i.extra) or
                                        (headers[id(i)] and 0x0001 in extra_ids(headers[id(i)]["extra"]))
                                        for i in infos)
    if zip64:
        problems.warn(label, "uses ZIP64; format 1.0 packages don't", "zip64")

    # The mimetype entry: readers identify a package by wavelength.json, so problems here are warnings.
    mimetype = [i for i in infos if i.orig_filename == "mimetype"]
    if not mimetype:
        problems.warn("mimetype", "is missing; the package is identified by wavelength.json", "mimetype-missing")
    else:
        m = mimetype[0]
        if m is not infos[0] or m.header_offset != 0:
            problems.warn("mimetype", "is not the first entry", "mimetype-not-first")
        lh = headers[id(m)]
        if m.compress_type != zipfile.ZIP_STORED or (lh and lh["method"] != 0):
            problems.warn("mimetype", "is compressed; it should be stored", "mimetype-compressed")
        if m.extra or (lh and lh["extra"]):
            problems.warn("mimetype", "has an extra field", "mimetype-extra")
        if m.file_size > 256:
            problems.error("mimetype", "is not %s" % PACKAGE_MEDIA_TYPE, "mimetype-content")
        elif not m.flag_bits & 0x41 and not local_name_problem(m, lh):
            try:
                content = zf.read(m)   # read whole, so its CRC-32 is checked
            except (zipfile.BadZipFile, OSError, zlib.error, NotImplementedError, EOFError) as e:
                content = None
                problems.error("mimetype", "can't be read: %s" % e, "zip-crc" if "CRC" in str(e) else "read-failed")
            if content is not None and content != PACKAGE_MEDIA_TYPE.encode("ascii"):
                problems.error("mimetype", "holds %r, not %s" % (content[:80], PACKAGE_MEDIA_TYPE),
                               "mimetype-content")
        if m is infos[0] and len(infos) > 1 and infos[1].orig_filename != MANIFEST:
            problems.warn(infos[1].orig_filename, "is the second entry; wavelength.json should be",
                          "manifest-not-second")

    entries = {}
    dir_entries = set()
    seen_exact = set()
    seen_fold = {}
    total = 0
    for info in infos:
        name = info.orig_filename   # the central directory's name, before zipfile's clean-up
        total += info.file_size
        if info.flag_bits & 0x2041:
            problems.error(name, "is encrypted", "encrypted")
            fatal = True
            continue
        bad = False
        why = local_name_problem(info, headers[id(info)])
        if why:
            problems.error(name, why, "zip-local-name")
            bad = True
        if info.filename != name:
            problems.error(name, "has a second name, %r (a Unicode path extra field or a NUL byte)" % info.filename,
                           "zip-name")
            bad = True
        if name in seen_exact:
            problems.error(name, "appears twice", "duplicate-entry")
            bad = True
        seen_exact.add(name)
        if name == "mimetype":
            continue
        is_dir = name.endswith("/")
        rel = name[:-1] if is_dir else name
        if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            problems.error(name, "uses compression method %d; only stored and deflated are allowed"
                           % info.compress_type, "compression")
            bad = True
        if not name.isascii() and not info.flag_bits & 0x800:
            problems.error(name, "has a non-ASCII name without the UTF-8 flag (bit 11)", "name-encoding")
            bad = True
        for code, message in path_problems(rel):
            problems.error(name, message, code)
            bad = True
        mode = (info.external_attr >> 16) & 0o170000
        if mode == 0o120000:
            problems.error(name, "is a symbolic link", "symlink")
            bad = True
        elif mode not in (0, 0o040000 if is_dir else 0o100000):
            problems.error(name, "is not a regular file or folder", "special-file")
            bad = True
        if not bad:
            key = fold(rel)
            if key in seen_fold and seen_fold[key] != rel:
                problems.error(name, "collides with %s when case is ignored" % seen_fold[key], "case-collision")
                bad = True
            elif key not in seen_fold:
                seen_fold[key] = rel
        if info.file_size > RATIO_FLOOR and (info.compress_size == 0
                                             or info.file_size / info.compress_size > max_ratio):
            problems.error(name, "expands %d bytes to %d (more than %d:1)"
                           % (info.compress_size, info.file_size, max_ratio), "zip-ratio")
            fatal = True
        if bad:
            continue
        if is_dir:
            dir_entries.add(rel)
        else:
            entries[rel] = info

    # A name that is a file in one entry and a folder in another.
    folders = set(fold(d) for d in dir_entries)
    for n in entries:
        d = posixpath.dirname(n)
        while d:
            folders.add(fold(d))
            d = posixpath.dirname(d)
    for n in list(entries):
        if fold(n) in folders:
            problems.error(n, "is both a file and a folder", "case-collision")
            del entries[n]

    if total > max_total:
        problems.error(label, "unpacks to %d bytes (limit %d)" % (total, max_total), "zip-too-large")
        fatal = True
    if fatal:
        zf.close()
        return None

    # Read every file entry to its end: zipfile checks the CRC-32 there.
    digests = {}
    for rel, info in sorted(entries.items()):
        h = hashlib.sha256()
        n = 0
        try:
            with zf.open(info) as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
                    n += len(chunk)
        except (zipfile.BadZipFile, OSError, zlib.error, EOFError, RuntimeError, NotImplementedError) as e:
            problems.error(rel, "can't be read: %s" % e, "zip-crc" if "CRC" in str(e) else "read-failed")
            del entries[rel]
            continue
        digests[rel] = (h.hexdigest(), n)
    return ZipTree(zf, entries, dir_entries, digests)


def open_song(path, problems, max_total=DEFAULT_MAX_TOTAL, max_ratio=DEFAULT_MAX_RATIO):
    """A tree for a song folder or a package, after the folder or entry rules."""
    if os.path.isdir(path):
        files, issues = scan_folder(path, problems)
        return FolderTree(path, files, issues)
    if os.path.isfile(path):
        return scan_package(path, problems, max_total, max_ratio)
    problems.error(path, "is neither a song folder nor a package", "not-found")
    return None


# ---------------------------------------------------------------------------
# Typed field checks for one JSON file


class Fields:
    """Report schema problems against one file, locating each by a JSON path."""

    def __init__(self, problems, file, code="schema"):
        self.problems = problems
        self.file = file
        self.code = code

    def err(self, where, message, code=None):
        self.problems.error(self.file, "%s %s" % (where, message) if where else message, code or self.code)

    def warn(self, where, message, code=None):
        self.problems.warn(self.file, "%s %s" % (where, message) if where else message, code or self.code)

    def _get(self, obj, key, where, required):
        if key not in obj:
            if required:
                self.err(self.at(where, key), "is required")
            return False
        return True

    @staticmethod
    def at(where, key):
        """Join a location and a key: "files[2]" + "role", "line 3:" + "rev"."""
        if not where:
            return key
        return "%s %s" % (where, key) if where.endswith(":") else "%s.%s" % (where, key)

    def string(self, obj, key, where="", required=False, nonempty=False, pattern=None, what=None):
        if not self._get(obj, key, where, required):
            return None
        v = obj[key]
        if not isinstance(v, str):
            self.err(self.at(where, key), "must be a string")
            return None
        if nonempty and not v:
            self.err(self.at(where, key), "must not be empty")
            return None
        if pattern and not re.fullmatch(pattern, v):
            self.err(self.at(where, key), "%r is not %s" % (v, what or "valid"))
            return None
        return v

    def integer(self, obj, key, where="", required=False, minimum=None):
        if not self._get(obj, key, where, required):
            return None
        v = obj[key]
        if not is_int(v) or (minimum is not None and v < minimum):
            self.err(self.at(where, key), "must be an integer" + (" >= %d" % minimum if minimum is not None else ""))
            return None
        return int(v)

    def number(self, obj, key, where="", required=False, minimum=None):
        if not self._get(obj, key, where, required):
            return None
        v = obj[key]
        if not is_num(v) or (minimum is not None and v < minimum):
            self.err(self.at(where, key), "must be a number" + (" >= %s" % minimum if minimum is not None else ""))
            return None
        return v

    def boolean(self, obj, key, where="", required=False):
        if not self._get(obj, key, where, required):
            return None
        if not isinstance(obj[key], bool):
            self.err(self.at(where, key), "must be true or false")
            return None
        return obj[key]

    def known(self, obj, key, values, default, where="", required=False, code=None):
        """A value from an open list (section 10): one this reader doesn't know is read as default."""
        if not self._get(obj, key, where, required):
            return None
        v = obj[key]
        if not isinstance(v, str) or not v:
            self.err(self.at(where, key), "must be a non-empty string", code)
            return None
        if v not in values:
            self.warn(self.at(where, key), "%r is not one of %s; read as %s" % (v, ", ".join(values), default), code)
            return default
        return v

    def hashref(self, obj, key, where="", required=False):
        """A "sha256:<64 hex>" value; another algorithm's prefix is unsupported (a warning), not corrupt."""
        v = self.string(obj, key, where, required)
        if v is None:
            return None
        kind = hash_ref(v)
        if kind == "unsupported":
            self.warn(self.at(where, key), "%r uses a hash this reader doesn't support" % v, "hash-unsupported")
        elif kind == "invalid":
            self.err(self.at(where, key), "%r is not \"sha256:\" and 64 lowercase hex digits" % v)
        return v if kind == "ok" else None

    def obj(self, obj, key, where="", required=False):
        if not self._get(obj, key, where, required):
            return None
        if not isinstance(obj[key], dict):
            self.err(self.at(where, key), "must be an object")
            return None
        return obj[key]

    def array(self, obj, key, where="", required=False):
        if not self._get(obj, key, where, required):
            return None
        if not isinstance(obj[key], list):
            self.err(self.at(where, key), "must be an array")
            return None
        return obj[key]

    def time(self, obj, key, where="", required=False, strict=True):
        v = self.string(obj, key, where, required)
        if v is not None and not is_rfc3339(v):
            (self.err if strict else self.warn)(self.at(where, key), "%r is not an RFC 3339 time" % v, "time-format")
        return v

    def path(self, obj, key, where="", required=False):
        """A song path (section 2); returns it only when it obeys every rule."""
        v = self.string(obj, key, where, required)
        if v is None:
            return None
        bad = path_problems(v)
        for code, message in bad:
            self.err(self.at(where, key), "%r %s" % (v, message), code)
        return None if bad else v

    def person(self, obj, key, where="", required=False, kind_required=False, role=False):
        p = self.obj(obj, key, where, required)
        if p is not None:
            w = self.at(where, key)
            self.string(p, "name", w, required=True, nonempty=True)
            if role:
                self.string(p, "role", w, required=True, nonempty=True)
            self.known(p, "kind", KINDS, "human", w, required=kind_required, code="author-kind")
            self.string(p, "url", w)
        return p

    def pair(self, obj, key, where="", integer=False, minimum=0):
        """A [start, end] range with start <= end."""
        if key not in obj:
            return None
        v = obj[key]
        ok = isinstance(v, list) and len(v) == 2 and all(
            (is_int(x) if integer else is_num(x)) and x >= minimum for x in v)
        if not ok:
            self.err(self.at(where, key), "must be [start, end] %s >= %d" % ("integers" if integer else "numbers", minimum))
            return None
        if v[0] > v[1]:
            self.err(self.at(where, key), "starts after it ends")
        return v


# ---------------------------------------------------------------------------
# The manifest (section 3) and versions (section 10)


def check_version(manifest, problems):
    """Refuse songs this 1.0 reader must not guess at (sections 3 and 10).

    Versions are compared as two numbers. The song is refused when its
    minReaderVersion (default: formatVersion's major, .0) is newer than 1.0, or
    when it requires an extension (this reader knows none).
    """
    f = Fields(problems, MANIFEST)
    fmt = manifest.get("format")
    if fmt != FORMAT:
        f.err("format", "is %r, not %r" % (fmt, FORMAT) if "format" in manifest else "is required", "format")
        raise Refused()
    fv = parse_version(manifest.get("formatVersion"))
    if fv is None:
        f.err("formatVersion", "must be \"major.minor\"" if "formatVersion" in manifest else "is required",
              "format-version")
        raise Refused()
    if "minReaderVersion" in manifest:
        mr = parse_version(manifest["minReaderVersion"])
        if mr is None:
            f.err("minReaderVersion", "must be \"major.minor\"", "reader-version")
            raise Refused()
    else:
        mr = (fv[0], 0)
    if mr > READER_VERSION:
        where = "minReaderVersion" if "minReaderVersion" in manifest else "formatVersion"
        f.err(where, "needs a %d.%d reader; this reader supports %d.%d, so it refuses the song"
              % (mr + READER_VERSION), "reader-version")
        raise Refused()
    required = manifest.get("extensionsRequired")
    if isinstance(required, list) and required:
        for ext in required:
            f.err("extensionsRequired", "names %r, which this reader does not support" % (ext,), "extension-required")
        raise Refused()
    return fv


def known_requirement(r):
    """False for a requires entry of a kind this reader doesn't know, which it ignores (section 10)."""
    if not isinstance(r, dict) or ("plugin" not in r and "library" not in r):
        return False
    lib = r.get("library")
    return "plugin" in r or not (isinstance(lib, dict) and isinstance(lib.get("kind"), str)
                                 and lib["kind"] not in LIBRARY_KINDS)


def check_manifest(m, problems):
    """The schema rules of wavelength.schema.json, in code."""
    f = Fields(problems, MANIFEST)
    f.string(m, "id", required=True, pattern=UUID_RE, what="a lowercase UUID")
    f.string(m, "title", required=True, nonempty=True)
    slug = f.string(m, "slug", required=True, pattern=SLUG_RE, what="a slug (lowercase a-z 0-9 and single hyphens)")
    if slug and len(slug) > 244:
        f.err("slug", "is longer than 244 characters")
    gen = f.obj(m, "generator")
    if gen is not None:
        f.string(gen, "name", "generator", required=True, nonempty=True)
        f.string(gen, "version", "generator", required=True, nonempty=True)
    derived = f.obj(m, "derivedFrom")
    if derived is not None:
        f.string(derived, "id", "derivedFrom", required=True, pattern=UUID_RE, what="a lowercase UUID")
        f.string(derived, "title", "derivedFrom")
        f.string(derived, "url", "derivedFrom")
    f.time(m, "created")
    f.time(m, "updated")
    authors = f.array(m, "authors")
    for i, a in enumerate(authors or []):
        if not isinstance(a, dict):
            f.err("authors[%d]" % i, "must be an object")
            continue
        w = "authors[%d]" % i
        f.string(a, "name", w, required=True, nonempty=True)
        f.string(a, "role", w, required=True, nonempty=True)
        f.known(a, "kind", KINDS, "human", w, code="author-kind")
        f.string(a, "url", w)
    for key in ("prompt", "summary", "description"):
        f.string(m, key)
    lic = f.string(m, "license")
    if lic is not None and not is_spdx(lic):
        f.err("license", "%r is not an SPDX license expression" % lic, "license")
    tags = f.array(m, "tags")
    for i, t in enumerate(tags or []):
        if not isinstance(t, str):
            f.err("tags[%d]" % i, "must be a string")
    f.path(m, "job", required=True)
    if isinstance(m.get("job"), str) and "/" in m["job"]:
        f.err("job", "%r is not at the top of the song folder" % m["job"], "job-location")

    files = f.array(m, "files")
    seen = {}
    for i, entry in enumerate(files or []):
        w = "files[%d]" % i
        if not isinstance(entry, dict):
            f.err(w, "must be an object")
            continue
        p = f.path(entry, "path", w, required=True)
        role = f.known(entry, "role", ROLES, "other", w, required=True, code="file-role")
        f.string(entry, "mediaType", w, pattern=MEDIA_TYPE_RE, what="a media type")
        f.integer(entry, "size", w, minimum=0)
        f.string(entry, "sha256", w, pattern=SHA256_RE, what="64 lowercase hex digits")
        if p is None:
            continue
        key = fold(p)
        if key in seen:
            f.err(w + ".path", "%r is listed twice (as %r)" % (p, seen[key]), "file-duplicate")
        seen[key] = p
        job = m.get("job")
        if key in (fold(MANIFEST), "mimetype", fold(REVIEW)) or (isinstance(job, str) and key == fold(job)):
            f.err(w + ".path", "%r is the manifest, the job, review.json or mimetype, which files may not list" % p,
                  "file-reserved")
        elif key.startswith("history/"):
            f.err(w + ".path", "%r is inside history/, which is reserved for revisions" % p, "file-reserved")
        elif key.startswith("out/"):
            f.err(w + ".path", "%r is inside out/, which is scratch and never part of the song" % p, "file-reserved")
        elif role == "media" and not p.startswith("media/"):
            f.warn(w + ".path", "%r has role media but is not under media/" % p, "file-media-folder")

    render = f.obj(m, "render")
    if render is not None:
        f.integer(render, "revision", "render", minimum=1)
        f.hashref(render, "job", "render")
        f.path(render, "mix", "render", required=True)
        f.path(render, "picture", "render")
        f.path(render, "report", "render")

    requires = f.array(m, "requires")
    for i, r in enumerate(requires or []):
        w = "requires[%d]" % i
        if not isinstance(r, dict):
            f.err(w, "must be an object")
            continue
        if not known_requirement(r):
            continue
        f.string(r, "track", w, required=True, nonempty=True)
        plugin = f.obj(r, "plugin", w)
        if plugin is not None:
            f.string(plugin, "name", w + ".plugin", required=True, nonempty=True)
            for k in ("format", "id", "version"):
                f.string(plugin, k, w + ".plugin")
        library = f.obj(r, "library", w)
        if library is not None:
            f.string(library, "kind", w + ".library", required=True, nonempty=True)
            f.string(library, "name", w + ".library", required=True, nonempty=True)
        if plugin is not None and library is not None:
            f.warn(w, "has both a plugin and a library; an entry names one need")
        f.string(r, "preset", w)
        f.boolean(r, "fallback", w, required=True)

    for key in ("extensions", "metadata"):
        for k in f.obj(m, key) or {}:
            if not matches(REVERSE_DOMAIN_RE, k):
                f.err(key, "key %r is not a reverse-domain name such as run.wavelength.site" % k, "reverse-domain")
    required = f.array(m, "extensionsRequired")
    for i, k in enumerate(required or []):
        if not matches(REVERSE_DOMAIN_RE, k):
            f.err("extensionsRequired[%d]" % i, "%r is not a reverse-domain name" % (k,), "reverse-domain")


def listed_files(manifest):
    """path -> files[] entry, for entries with a usable path."""
    out = {}
    for entry in manifest.get("files") or []:
        if isinstance(entry, dict) and isinstance(entry.get("path"), str) and not path_problems(entry["path"]):
            out.setdefault(entry["path"], entry)
    return out


def check_manifest_files(tree, manifest, problems):
    """Listed files exist and match their size and sha256; the render names real files."""
    for p, entry in listed_files(manifest).items():
        if not tree.exists(p):
            variant = tree.case_variant(p)
            if variant:
                problems.error(p, "is listed but the song has %s (names differ only in case)" % variant, "file-case")
            else:
                problems.warn(p, "is listed in the manifest but not included", "file-missing")
            continue
        want_size = entry.get("size")
        want_hash = entry.get("sha256")
        if is_int(want_size) or matches(SHA256_RE, want_hash):
            try:
                digest, size = hash_file(tree, p)
            except ReadProblem as e:
                problems.error(p, str(e), "read-failed")
                continue
            if is_int(want_size) and size != want_size:
                problems.error(p, "is %d bytes; the manifest says %d" % (size, want_size), "file-size")
            if matches(SHA256_RE, want_hash) and digest != want_hash:
                problems.error(p, "hashes to %s; the manifest says %s" % (digest, want_hash), "file-hash")

    render = manifest.get("render")
    if not isinstance(render, dict):
        return
    listed = listed_files(manifest)
    for key in ("mix", "picture", "report"):
        p = render.get(key)
        if not isinstance(p, str) or path_problems(p):
            continue
        if p not in listed:
            problems.warn(MANIFEST, "render.%s %r is not listed in files" % (key, p), "render-unlisted")
        if not tree.exists(p) and p not in listed:
            problems.warn(p, "is the render's %s but is not included" % key, "file-missing")
    report = render.get("report")
    if isinstance(report, str) and not path_problems(report) and tree.exists(report):
        data = load_json_file(tree, report, problems, "render-report")
        song = data.get("song") if isinstance(data, dict) else None
        if isinstance(song, dict):
            for k in ("revision", "job"):
                if k in song and k in render and song[k] != render[k]:
                    problems.warn(report, "song.%s is %r; the manifest's render.%s is %r"
                                  % (k, song[k], k, render[k]), "render-report")


# ---------------------------------------------------------------------------
# The job (section 4)


def job_references(job):
    """Every reference in a job, a file or a name decided by its key alone (section 4.2).

    Returns (where, value, kind, path): kind is "file" (path is the song file it
    reads), "output" (a deliver file), "lib" (one file of a library, by name) or
    "name" (a library, or a file inside a named kit); path is None for the last two.
    """
    refs = []

    def file_or_lib(where, v):
        """sampler sample, clip file, kit map values: files unless lib: or a * generator."""
        if v.startswith(LIB_PREFIX):
            refs.append((where, v, "lib", None))
        elif not v.startswith(GENERATOR_PREFIX):
            refs.append((where, v, "file", v))

    def state(o, base):
        st = o.get("state")
        if isinstance(st, dict):
            st, base = st.get("file"), base + ".state.file"
        else:
            base += ".state"
        if isinstance(st, str):
            refs.append((base, st, "file", re.sub(r"#[0-9]+$", "", st)))   # "cart.syx#3": a program of the file

    def patch(v, where):
        """A GarageBand or Logic patch: a library name, or a folder in the song when it ends with .patch."""
        if isinstance(v, str):
            is_file = v.lower().endswith(".patch")
            refs.append((where, v, "file" if is_file else "name", v if is_file else None))

    def fx(chain, base, depth=0):
        if not isinstance(chain, list) or depth > 8:
            return
        for i, e in enumerate(chain):
            if not isinstance(e, dict):
                continue
            eb = "%s[%d]" % (base, i)
            state(e, eb)
            if e.get("type") == "patch":
                patch(e.get("patch"), eb + ".patch")
            ir = e.get("ir")
            if isinstance(ir, str) and not ir.startswith(LIB_PREFIX):   # an installed room by name, else a file
                is_file = "/" in ir or "\\" in ir or ir.lower().endswith(IR_FILE_EXT)
                refs.append((eb + ".ir", ir, "file" if is_file else "name", ir if is_file else None))
            fx(e.get("loopFx"), eb + ".loopFx", depth + 1)   # inside a delay's feedback loop
            bands = e.get("bands") if isinstance(e.get("bands"), list) else []
            for bi, band in enumerate(bands):   # a multiband's chain per band
                if isinstance(band, dict):
                    fx(band.get("fx"), "%s.bands[%d].fx" % (eb, bi), depth + 1)

    def sound(s, base):
        state(s, base)
        fx(s.get("fx"), base + ".fx")
        if isinstance(s.get("preset"), str) and s["preset"].lower().endswith(".patch"):
            patch(s["preset"], base + ".preset")
        sampler = s.get("sampler")
        if not isinstance(sampler, dict):
            return
        sb = base + ".sampler"
        patch(sampler.get("patch"), sb + ".patch")
        if isinstance(sampler.get("sample"), str):
            file_or_lib(sb + ".sample", sampler["sample"])
        for key in ("multisample", "sfz", "soundfont", "exs", "dspreset", "kit"):
            v = sampler.get(key)
            if isinstance(v, str):
                path = v.split("#")[0] if key == "dspreset" else v   # "<bundle>#<preset>": one preset of the file
                is_file = "/" in v or (key != "kit" and path.lower().endswith(LIBRARY_FILE_EXT))
                refs.append(("%s.%s" % (sb, key), v, "file" if is_file else "name", path if is_file else None))
        kit = sampler.get("kit")
        named_kit = isinstance(kit, str)   # a library name or a folder in the song: bare map values are inside it
        for key in ("map", "kit"):   # key -> file (or {"file": ...}); "kit" as an object is a map of its own
            m = sampler.get(key)
            if not isinstance(m, dict):
                continue
            for k, v in m.items():
                f = v.get("file") if isinstance(v, dict) else v
                if not isinstance(f, str):
                    continue
                w = "%s.%s.%s" % (sb, key, k)
                if key == "map" and named_kit and "/" not in f:
                    refs.append((w, f, "name", None))   # a file inside the kit's folder
                else:
                    file_or_lib(w, f)

    for ti, t in enumerate(job.get("tracks") or []):
        if not isinstance(t, dict):
            continue
        base = "tracks[%d]" % ti
        sound(t, base)
        clips = t.get("clips") if isinstance(t.get("clips"), list) else []
        for ci, c in enumerate(clips):
            if not isinstance(c, dict):
                continue
            if isinstance(c.get("file"), str):
                file_or_lib("%s.clips[%d].file" % (base, ci), c["file"])
            elif isinstance(c.get("file"), dict):   # the song's own audio, with its own effects
                fx(c["file"].get("fx"), "%s.clips[%d].file.fx" % (base, ci))
        fb = t.get("fallback")
        for fi, s in enumerate(fb if isinstance(fb, list) else [fb]):
            if isinstance(s, dict):
                sound(s, "%s.fallback[%d]" % (base, fi) if isinstance(fb, list) else base + ".fallback")
    buses = job.get("buses") if isinstance(job.get("buses"), list) else []
    for bi, b in enumerate(buses):
        if isinstance(b, dict):
            fx(b.get("fx"), "buses[%d].fx" % bi)
    if isinstance(job.get("master"), dict):
        fx(job["master"].get("fx"), "master.fx")
    deliver = job.get("deliver")
    for di, d in enumerate(deliver if isinstance(deliver, list) else [deliver]):
        if isinstance(d, dict) and isinstance(d.get("file"), str):
            refs.append(("deliver[%d].file" % di, d["file"], "output", d["file"]))
    return refs


def check_ids(job, jp, problems):
    """Section 4.1: explicit IDs use a-z 0-9 - _ . and are unique within their list."""
    for list_name in ("tracks", "buses", "markers"):
        items = job.get(list_name)
        if not isinstance(items, list):
            continue
        explicit = {}
        effective = {}
        for i, item in enumerate(items):
            if not isinstance(item, dict):
                continue
            where = "%s[%d]" % (list_name, i)
            has_id = "id" in item
            if has_id:
                v = item["id"]
                if not matches(ID_RE, v):
                    problems.error(jp, "%s.id %r must use only a-z 0-9 - _ ." % (where, v), "job-id")
                elif v in explicit:
                    problems.error(jp, "%s.id %r is also %s[%d]'s id" % (where, v, list_name, explicit[v]),
                                   "job-id-duplicate")
                else:
                    explicit[v] = i
            eid = item.get("id") if has_id else item.get("name")
            if not isinstance(eid, str):
                continue
            if eid in effective:
                j, other_explicit = effective[eid]
                if not (has_id and other_explicit):
                    problems.warn(jp, "%s and %s[%d] have the same ID %r (a missing id is the name)"
                                  % (where, list_name, j, eid), "job-id-duplicate")
            else:
                effective[eid] = (i, has_id)


def check_job(tree, manifest, problems):
    """The job exists, is JSON with tracks, and every file it names stays in the song."""
    jp = manifest.get("job")
    if not isinstance(jp, str) or path_problems(jp):
        return None
    if not tree.exists(jp):
        variant = tree.case_variant(jp)
        problems.error(jp, "the job is missing" + (" (the song has %s, which differs only in case)" % variant
                                                    if variant else ""), "job-missing")
        return None
    job = load_json_file(tree, jp, problems, "job-json")
    if job is None:
        return None
    if not isinstance(job, dict):
        problems.error(jp, "is not a JSON object", "job-json")
        return None
    tracks = job.get("tracks")
    if not isinstance(tracks, list) or not tracks:
        problems.error(jp, "tracks must be a non-empty array", "job-tracks")
    else:
        for i, t in enumerate(tracks):
            if not isinstance(t, dict):
                problems.error(jp, "tracks[%d] must be an object" % i, "job-tracks")
    check_ids(job, jp, problems)

    listed = listed_files(manifest)
    refs = job_references(job)
    # sounds.json beside the job (docs/job-format.md, "Sounds"): the instruments it sets read files of the song too
    sp = posixpath.join(posixpath.dirname(jp), "sounds.json")
    sounds = load_json_file(tree, sp, problems, "sounds-json") if tree.exists(sp) else None
    if isinstance(sounds, dict) and isinstance(sounds.get("tracks"), list):
        extra = []
        for e in sounds["tracks"]:
            if isinstance(e, dict) and "plugin" in e:
                t = {k: e[k] for k in ("plugin", "preset", "state", "params") if k in e}
                extra.append(t)
        refs += [("sounds.json " + w, v, k, t) for w, v, k, t in job_references({"tracks": extra})]
    for where, value, kind, target in refs:
        if kind == "lib":   # one file of a sample library, by name
            if any(seg in ("", ".", "..") for seg in value[len(LIB_PREFIX):].split("/")):
                problems.error(jp, "%s %r has an empty, '.' or '..' segment" % (where, value), "path-traversal")
            continue
        if kind == "name":   # a library by name: never a path, so never a way out
            if ".." in re.split(r"[/\\]", value):
                problems.error(jp, "%s %r has a '..' segment" % (where, value), "path-traversal")
            continue
        ref = "%s %r" % (where, value) if target == value else "%s %r (file %r)" % (where, value, target)
        bad = path_problems(target)
        for code, message in bad:
            problems.error(jp, "%s %s" % (ref, message), code)
        if bad or kind == "output":
            continue
        if tree.exists(target) or tree.is_dir(target):
            if tree.exists(target):
                entry = listed.get(target)
                if entry is None:
                    problems.warn(jp, "%s is not listed in the manifest's files (pack lists it)" % ref, "unlisted-file")
                elif entry.get("role") != "media":
                    problems.warn(jp, "%s is listed with role %r, not media" % (ref, entry.get("role")), "file-role")
            continue
        variant = tree.case_variant(target)
        problems.error(jp, "%s is not in the song%s" % (ref, " (it has %s)" % variant if variant else ""),
                       "job-ref-missing")

    # requires is a summary of the job; its tracks should exist.
    ids = set()
    for t in tracks if isinstance(tracks, list) else []:
        if isinstance(t, dict):
            eid = t.get("id", t.get("name"))
            if isinstance(eid, str):
                ids.add(eid)
    for i, r in enumerate(manifest.get("requires") or []):
        if known_requirement(r) and isinstance(r.get("track"), str) and r["track"] not in ids:
            problems.warn(MANIFEST, "requires[%d].track %r is not a track ID in the job" % (i, r["track"]),
                          "requires-track")
    return job


# ---------------------------------------------------------------------------
# History (section 6)


def read_log(tree, problems):
    """Parse history/log.jsonl into [(line number, entry)] (section 6).

    Empty lines are skipped; a CR before the LF is tolerated. A last line with
    no LF is an interrupted write: kept with a warning when it parses, else ignored.
    """
    if not tree.exists(LOG):
        return None
    try:
        raw = read_bytes(tree, LOG, MAX_LOG)
    except ReadProblem as e:
        problems.error(LOG, str(e), "read-failed")
        return []
    if raw.startswith(BOM):
        problems.error(LOG, "starts with a byte order mark (I-JSON)", "json-bom")
        raw = raw[len(BOM):]
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        problems.error(LOG, "is not UTF-8 (%s at byte %d)" % (e.reason, e.start), "log-json")
        return []
    ends_with_newline = text.endswith("\n")
    lines = text.split("\n")
    if ends_with_newline:
        lines.pop()
    out = []
    for n, line in enumerate(lines, 1):
        cut = n == len(lines) and not ends_with_newline
        if not line.strip():
            continue
        try:
            entry = parse_json(line)
        except JsonProblem as e:
            if cut:
                problems.warn(LOG, "line %d has no line ending and isn't JSON (an interrupted write?); ignored" % n,
                              "log-truncated")
            else:
                problems.error(LOG, "line %d %s" % (n, e), e.code or "log-json")
            continue
        if cut:
            problems.warn(LOG, "line %d has no line ending (an interrupted write?)" % n, "log-truncated")
        out.append((n, entry))
    return out


def check_entry(entry, n, problems):
    """One log line against history-entry.schema.json. Returns True when usable."""
    f = Fields(problems, LOG, "log-entry")
    w = "line %d:" % n
    if not isinstance(entry, dict):
        f.err(w, "is not a JSON object")
        return False
    ok = f.integer(entry, "rev", w, required=True, minimum=1) is not None
    f.time(entry, "time", w, required=True)
    op = f.known(entry, "op", OPS, "save", w, required=True, code="log-op")
    ok = f.integer(entry, "parent", w, required=True, minimum=0) is not None and ok
    by = f.obj(entry, "by", w, required=True)
    if by is not None:
        f.string(by, "name", w + " by", required=True, nonempty=True)
        f.known(by, "kind", KINDS, "human", w + " by", required=True, code="author-kind")
    message = f.string(entry, "message", w)
    named = f.boolean(entry, "named", w)
    files = f.obj(entry, "files", w, required=True)
    folded = {}
    for p, h in (files or {}).items():
        for code, msg in path_problems(p):
            f.err(w, "files key %r %s" % (p, msg), code)
        if isinstance(p, str) and fold(p) in folded:
            f.err(w, "files keys %r and %r collide when case is ignored" % (folded[fold(p)], p), "case-collision")
        folded.setdefault(fold(p), p)
        kind = hash_ref(h)
        if kind == "unsupported":
            f.warn(w, "files[%r] uses a hash this reader doesn't support: %r" % (p, h), "hash-unsupported")
        elif kind == "invalid":
            f.err(w, "files[%r] must be \"sha256:\" and 64 lowercase hex digits" % p)
    render = f.obj(entry, "render", w)
    if render is not None:
        for k in ("report", "mix"):
            f.hashref(render, k, w + " render")
        for k in ("lufs", "lra", "truePeak"):
            f.number(render, k, w + " render")
        f.number(render, "seconds", w + " render", minimum=0)
        for i, s in enumerate(f.array(render, "sections", w + " render") or []):
            if not isinstance(s, dict):
                f.err(w, "render.sections[%d] must be an object" % i)
                continue
            f.string(s, "name", "%s render.sections[%d]" % (w, i), required=True)
            f.number(s, "lufs", "%s render.sections[%d]" % (w, i))
    f.integer(entry, "target", w, minimum=1)
    if op == "render" and "render" not in entry:
        f.err(w, "render entry has no render")
    if op in ("undo", "redo", "restore"):
        if "target" not in entry:
            f.err(w, "%s entry has no target" % op)
    if named is True and not message:
        f.err(w, "named is true but there is no message")
    return ok and files is not None


def entry_op(e):
    """The op as this reader reads it: one it doesn't know is a save (section 10)."""
    return e.get("op") if e.get("op") in OPS else "save"


def verify_object(tree, rel, expected, limit, keep=False):
    """Decompress one object and compare the SHA-256 of its content with its name.

    Output is produced in bounded steps and capped at limit bytes (zlib bombs).
    Returns (content when keep else None, problem code or None, message, bytes decompressed).
    """
    step = 1 << 20
    d = zlib.decompressobj()
    h = hashlib.sha256()
    total = 0
    parts = []
    try:
        with tree.open(rel) as f:
            while not d.eof:
                buf = f.read(1 << 16)
                if not buf:
                    break
                while True:
                    out = d.decompress(buf, step)
                    total += len(out)
                    if total > limit:
                        return None, "object-too-large", "decompresses to more than %d bytes" % limit, total
                    h.update(out)
                    if keep:
                        parts.append(out)
                    buf = d.unconsumed_tail
                    if d.eof or (not buf and len(out) < step):
                        break
            trailing = d.unused_data or f.read(1)
    except zlib.error as e:
        return None, "object-zlib", "is not valid zlib data (%s)" % e, total
    except (ReadProblem, zipfile.BadZipFile, OSError, EOFError) as e:
        return None, "read-failed", "can't be read: %s" % e, total
    if not d.eof:
        return None, "object-zlib", "is a cut-off zlib stream", total
    if trailing:
        return None, "object-zlib", "has data after its zlib stream", total
    digest = h.hexdigest()
    if digest != expected:
        return None, "object-hash", "content hashes to %s, not its name" % digest, total
    return b"".join(parts), None, "", total


def file_sha256(tree, rel):
    """SHA-256 (hex) of a file in the song, or None when there is none."""
    if path_problems(rel) or not tree.exists(rel):
        return None
    try:
        return hash_file(tree, rel)[0]
    except ReadProblem:
        return None


def check_stacks(numbered, problems):
    """Undo and redo targets against the two stacks of section 6 (warnings; skipped for a pruned log)."""
    revs = [int(e["rev"]) for _, e in numbered]
    if any(b != a + 1 for a, b in zip(revs, revs[1:])):
        return
    undo, redo, before = [], [], None
    for n, e in numbered:
        op, rev = entry_op(e), int(e["rev"])
        target = int(e["target"]) if is_int(e.get("target")) else None
        if op in ("undo", "redo"):
            # undo restores the revision below the top of undo; redo restores the top of redo
            want = (undo[-2] if len(undo) > 1 else None) if op == "undo" else (redo[-1] if redo else None)
            if target is not None and target != want:
                problems.warn(LOG, "line %d: %s restores rev %d; reading the log, it %s" % (
                    n, op, target, "should restore rev %d" % want if want else "has nothing to " + op), "log-stack")
            src, dst = (undo, redo) if op == "undo" else (redo, undo)
            if src:
                dst.append(src.pop())
        elif op == "restore" or before is None or e["files"] != before:
            undo.append(rev)
            redo = []
        before = e["files"]


def check_history(tree, manifest, problems, max_total=DEFAULT_MAX_TOTAL):
    """The log's entries, their sequence, and every object's name and content."""
    objects = {}
    for n in tree.list():
        if not n.startswith("history/") or n == LOG:
            continue
        if n == LOCK:   # a writer's lock: readers ignore it (section 6)
            if tree.kind == "package":
                problems.warn(n, "is a writer's lock, which packages never carry; ignored", "package-lock")
            continue
        m = re.fullmatch(OBJECT_RE, n)
        base = posixpath.basename(n)
        if m and m.group(2).startswith(m.group(1)):
            objects[m.group(2)] = n
        elif m:
            problems.error(n, "is in the wrong folder for its name", "object-name")
        elif n.startswith(OBJECTS) and not base.startswith("."):
            problems.error(n, "is not named history/objects/<2 hex>/<64 lowercase hex>", "object-name")
        elif tree.kind == "package":
            problems.warn(n, "is not part of history (log.jsonl and objects/)", "package-extra")
        else:
            problems.warn(n, "is not part of history (log.jsonl and objects/)", "history-extra")

    lines = read_log(tree, problems)
    if lines is None:
        if objects:
            problems.warn(OBJECTS, "has objects but there is no history/log.jsonl", "log-missing")
        return []

    entries = []
    numbered = []
    revs = {}
    prev = 0
    used = set()
    for n, e in lines:
        if not check_entry(e, n, problems):
            continue
        rev, parent = int(e["rev"]), int(e["parent"])
        w = "line %d:" % n
        if rev <= prev:
            problems.error(LOG, "%s rev %d is not greater than rev %d before it" % (w, rev, prev), "log-order")
        if parent >= rev:
            problems.error(LOG, "%s parent %d is not before rev %d" % (w, parent, rev), "log-parent")
        elif parent == 0 and entries:
            problems.error(LOG, "%s parent is 0 but this is not the first revision" % w, "log-parent")
        elif parent and parent not in revs and parent > prev:
            problems.warn(LOG, "%s parent %d is not in the log (pruned?)" % (w, parent), "log-parent")
        elif parent and entries and parent != prev:
            problems.warn(LOG, "%s parent %d is not the revision before this one in the log (%d)" % (w, parent, prev),
                          "log-parent")
        if is_int(e.get("target")):
            v = int(e["target"])
            if v >= rev:
                problems.error(LOG, "%s target %d is not before rev %d" % (w, v, rev), "log-target")
            elif v not in revs:
                problems.warn(LOG, "%s target %d is not in the log (pruned?)" % (w, v), "log-target")
        if e.get("op") in ("undo", "redo", "restore"):
            target = revs.get(int(e["target"])) if is_int(e.get("target")) else None
            if target is not None and target.get("files") != e.get("files"):
                problems.warn(LOG, "%s files differ from those of the target, rev %s" % (w, e["target"]), "log-target")
        for p, h in e["files"].items():
            if hash_ref(h) != "ok":
                continue
            hexd = h[7:]
            used.add(hexd)
            if hexd not in objects and not (tree.kind == "package" and file_sha256(tree, p) == hexd):
                # a package may leave out an object that is the same bytes as its file at that path (section 5)
                problems.error(LOG, "%s %s is %s, which is not in history/objects/" % (w, p, h[:19] + "..."),
                               "object-missing")
        revs[rev] = e
        prev = max(prev, rev)
        entries.append(e)
        numbered.append((n, e))
    check_stacks(numbered, problems)

    budget = max_total   # all objects together, decompressed (section 5: history objects included)
    for hexd, rel in sorted(objects.items()):
        _, code, message, size = verify_object(tree, rel, hexd, budget)
        budget -= size
        if code == "object-too-large":
            problems.error(rel, "takes the history's objects past %d bytes decompressed (the limit)" % max_total, code)
            break
        if code:
            problems.error(rel, message, code)
        elif hexd not in used:
            problems.warn(rel, "is not used by any revision", "object-orphan")

    render = manifest.get("render") if isinstance(manifest.get("render"), dict) else {}
    r = render.get("revision")
    if is_int(r) and entries and int(r) not in revs:
        problems.warn(MANIFEST, "render.revision %s is not in the log" % r, "render-revision")
    elif is_int(r) and int(r) in revs and revs[int(r)].get("op") != "render":
        problems.warn(MANIFEST, "render.revision %s is a %s entry, not a render" % (r, revs[int(r)].get("op")),
                      "render-revision")
    jp = manifest.get("job")
    if entries and isinstance(jp, str) and jp not in entries[-1].get("files", {}):
        problems.warn(LOG, "the latest revision does not track the job %r" % jp, "log-job")
    return entries


# ---------------------------------------------------------------------------
# Comments (section 7)


def check_review(tree, revs, problems):
    """review.json in either comment shape; anchors must name revisions that exist."""
    if not tree.exists(REVIEW):
        return None
    data = load_json_file(tree, REVIEW, problems, "review-json")
    if data is None:
        return None
    f = Fields(problems, REVIEW, "review")
    if not isinstance(data, dict):
        f.err("", "is not a JSON object")
        return None
    comments = f.array(data, "comments", required=True)
    ids = set()
    for i, c in enumerate(comments or []):
        w = "comments[%d]" % i
        if not isinstance(c, dict):
            f.err(w, "must be an object")
            continue
        cid = f.string(c, "id", w, required=True, nonempty=True)
        if cid is not None:
            if cid in ids:
                f.err(w + ".id", "%r is used twice" % cid, "review-id")
            ids.add(cid)
        status = f.known(c, "status", STATUSES, "open", w, required=True, code="review-status")
        f.string(c, "text", w, required=True)
        new = any(k in c for k in NEW_FIELDS)
        legacy = any(k in c for k in LEGACY_FIELDS)
        if new and legacy:
            f.warn(w, "mixes the anchor shape with anchor fields on the comment itself", "review-shape")
        if new:
            f.time(c, "created", w, strict=False)
        else:
            f.string(c, "created", w)
        f.person(c, "author", w)
        if "anchor" in c:
            for k in ("created", "author"):
                if k not in c:
                    f.warn(w + "." + k, "is missing (a comment from before the format?)", "review-legacy")
        anchor = f.obj(c, "anchor", w)
        if anchor is not None:
            a = w + ".anchor"
            rev = f.integer(anchor, "revision", a, minimum=1)
            if "revision" not in anchor:
                f.warn(a + ".revision", "is missing (a comment from before the format?)", "review-legacy")
            elif rev is not None and revs is not None and rev not in revs:
                f.warn(a + ".revision", "%d is not in the history (pruned?)" % rev, "review-revision")
            f.hashref(anchor, "render", a)
            f.pair(anchor, "time", a)
            f.pair(anchor, "bars", a, integer=True, minimum=1)
            f.pair(anchor, "beats", a)
            for j, t in enumerate(f.array(anchor, "tracks", a) or []):
                if not isinstance(t, str) or not t:
                    f.err("%s.tracks[%d]" % (a, j), "must be a track ID")
            for j, note in enumerate(f.array(anchor, "notes", a) or []):
                nw = "%s.notes[%d]" % (a, j)
                if not isinstance(note, dict):
                    f.err(nw, "must be an object")
                    continue
                f.string(note, "track", nw, required=True, nonempty=True)
                f.number(note, "beat", nw, required=True, minimum=0)
                key = f.integer(note, "key", nw, required=True, minimum=0)
                if key is not None and key > 127:
                    f.err(nw + ".key", "must be a MIDI key 0-127")
            f.string(anchor, "ref", a)
        for j, r in enumerate(f.array(c, "replies", w) or []):
            rw = "%s.replies[%d]" % (w, j)
            if not isinstance(r, dict):
                f.err(rw, "must be an object")
                continue
            f.person(r, "author", rw)
            if "author" not in r:
                f.warn(rw + ".author", "is missing (a reply from before the format?)", "review-legacy")
            f.time(r, "time", rw, strict=False)
            rev = f.integer(r, "revision", rw, minimum=1)
            if rev is not None and revs is not None and rev not in revs:
                f.warn(rw + ".revision", "%d is not in the history" % rev, "review-revision")
            f.string(r, "text", rw, required=True)
        resolved = f.obj(c, "resolved", w)
        if resolved is not None:
            rw = w + ".resolved"
            rev = f.integer(resolved, "revision", rw, required=True, minimum=1)
            if rev is not None and revs is not None and rev not in revs:
                f.warn(rw + ".revision", "%d is not in the history" % rev, "review-revision")
            f.time(resolved, "time", rw, strict=False)
            f.person(resolved, "by", rw)
            if status == "open":
                f.warn(w, "is resolved but its status is open", "review-status")
        # Comments written before the spec (section 7): anchor fields on the comment.
        f.string(c, "ref", w)
        f.pair(c, "time", w)
        f.pair(c, "bars", w, integer=True, minimum=1)
        f.pair(c, "beats", w)
        for j, t in enumerate(f.array(c, "tracks", w) or []):
            if not isinstance(t, str):
                f.err("%s.tracks[%d]" % (w, j), "must be a string")
        for j, note in enumerate(f.array(c, "notes", w) or []):
            nw = "%s.notes[%d]" % (w, j)
            if not isinstance(note, dict):
                f.err(nw, "must be an object")
                continue
            f.string(note, "track", nw, required=True)
            if "key" in note and not (is_int(note["key"]) or isinstance(note["key"], str)):
                f.err(nw + ".key", "must be a key name or MIDI number")
        f.number(c, "render", w)
        f.string(c, "reply", w)
    return data


# ---------------------------------------------------------------------------
# Whole-song validation


def check_package_contents(tree, manifest, problems):
    """Section 5: a package carries the manifest, job, listed files, review and history only."""
    allowed = {MANIFEST, REVIEW}
    if isinstance(manifest.get("job"), str):
        allowed.add(manifest["job"])
    allowed.update(listed_files(manifest))
    render = manifest.get("render")
    if isinstance(render, dict):
        allowed.update(v for k, v in render.items() if k in ("mix", "picture", "report") and isinstance(v, str))
    for n in tree.list():
        if n in allowed or n.startswith("history/"):
            continue
        problems.warn(n, "is not the manifest, the job, review.json, history/ or a file the manifest lists",
                      "package-extra")


def song_paths(manifest, job):
    """The song's own files (section 2), history/ aside.

    The manifest, the job, review.json and every listed, rendered or referenced file.
    """
    paths = {MANIFEST, REVIEW}
    if isinstance(manifest.get("job"), str):
        paths.add(manifest["job"])
    for entry in manifest.get("files") or []:
        if isinstance(entry, dict) and isinstance(entry.get("path"), str):
            paths.add(entry["path"])
    render = manifest.get("render")
    if isinstance(render, dict):
        paths.update(v for k, v in render.items() if k in ("mix", "picture", "report") and isinstance(v, str))
    if isinstance(job, dict):
        paths.update(target for _, _, kind, target in job_references(job) if kind == "file" and target)
    return paths


def validate_song(tree, problems, max_total=DEFAULT_MAX_TOTAL):
    """Check a song's contents (folder or package).

    Returns {"manifest", "job", "history", "review"} with what could be read,
    or None when the manifest is missing, unreadable or refused.
    """
    manifest = None
    if not tree.exists(MANIFEST):
        variant = tree.case_variant(MANIFEST)
        problems.error(MANIFEST, "is missing" + (" (the song has %s)" % variant if variant else ""), "manifest-missing")
    else:
        manifest = load_json_file(tree, MANIFEST, problems, "manifest-json")
        if manifest is not None and not isinstance(manifest, dict):
            problems.error(MANIFEST, "is not a JSON object", "manifest-json")
            manifest = None
    if manifest is not None:
        try:
            check_version(manifest, problems)
        except Refused:
            manifest = None
    if manifest is None:
        report_folder_issues(tree, None, problems)
        return None
    check_manifest(manifest, problems)
    check_manifest_files(tree, manifest, problems)
    job = check_job(tree, manifest, problems)
    report_folder_issues(tree, song_paths(manifest, job), problems)
    history = check_history(tree, manifest, problems, max_total)
    revs = {int(e["rev"]) for e in history} if tree.exists(LOG) else None
    review = check_review(tree, revs, problems)
    if tree.kind == "package":
        check_package_contents(tree, manifest, problems)
    return {"manifest": manifest, "job": job, "history": history, "review": review}


# ---------------------------------------------------------------------------
# Output


def print_problems(label, problems, out=sys.stdout):
    for p in problems.items:
        out.write("%-8s %s: %s [%s]\n" % (p["severity"], printable(p["path"]), printable(p["message"]), p["code"]))
    n_err, n_warn = len(problems.errors), len(problems.warnings)
    verdict = "ok" if not n_err else "FAILED"
    out.write("%s: %s, %d error%s, %d warning%s\n" % (printable(label), verdict, n_err, "" if n_err == 1 else "s",
                                                     n_warn, "" if n_warn == 1 else "s"))


def emit_json(obj):
    sys.stdout.write(json.dumps(obj, indent=2, ensure_ascii=False) + "\n")


# ---------------------------------------------------------------------------
# Commands


def cmd_validate(args):
    problems = Problems()
    tree = open_song(args.song, problems, args.max_size, args.max_ratio)
    if tree is not None:
        try:
            validate_song(tree, problems, args.max_size)
        finally:
            tree.close()
    ok = not problems.errors
    if args.json:
        emit_json({"ok": ok, "song": args.song, "kind": tree.kind if tree else None, "problems": problems.items})
    else:
        print_problems(os.path.basename(os.path.normpath(args.song)), problems)
    return 0 if ok else 1


def safe_makedirs(root, rel_dir):
    """Create rel_dir under root one level at a time, refusing links and files in the way."""
    cur = root
    for seg in rel_dir.split("/") if rel_dir else []:
        cur = os.path.join(cur, seg)
        try:
            st = os.lstat(cur)
        except FileNotFoundError:
            os.mkdir(cur, 0o755)
            continue
        if not os.path.isdir(cur) or os.path.islink(cur):
            raise ReadProblem("%s is in the way (not a plain folder)" % cur)
    return cur


def extract(tree, dest):
    """Write every file of a checked package under dest; never follows links, never sets exec bits."""
    count = 0
    for d in sorted(tree.dir_entries):
        safe_makedirs(dest, d)
    for rel in tree.list():
        if rel == LOCK:   # a stale writer's lock would block the unpacked song's writers
            continue
        parent = safe_makedirs(dest, posixpath.dirname(rel))
        target = os.path.join(parent, posixpath.basename(rel))
        if os.path.islink(target):
            raise ReadProblem("%s is a symbolic link; not writing through it" % target)
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
        fd = os.open(target, flags, 0o644)
        with os.fdopen(fd, "wb") as out, tree.open(rel) as src:
            shutil.copyfileobj(src, out, 1 << 20)
        count += 1
    return count


def restore_objects(dest, entries):
    """Objects a package left out because they are its files (section 5): stored again from those files."""
    restored = 0
    for e in entries:
        for p, h in (e.get("files") or {}).items():
            if not matches(HASHREF_RE, h) or path_problems(p):
                continue
            obj = os.path.join(dest, *(OBJECTS + h[7:9] + "/" + h[7:]).split("/"))
            src = os.path.join(dest, *p.split("/"))
            if os.path.exists(obj) or not os.path.isfile(src) or os.path.islink(src):
                continue
            data = open(src, "rb").read()
            if hashlib.sha256(data).hexdigest() != h[7:]:
                continue
            os.makedirs(os.path.dirname(obj), exist_ok=True)
            with open(obj, "wb") as f:
                f.write(zlib.compress(data, 9))
            restored += 1
    return restored


def cmd_unpack(args):
    problems = Problems()
    if not os.path.isfile(args.package):
        problems.error(args.package, "is not a file", "not-found")
        print_problems(args.package, problems, sys.stderr)
        return 1
    tree = scan_package(args.package, problems, args.max_size, args.max_ratio)
    song = validate_song(tree, problems, args.max_size) if tree is not None else None
    label = os.path.basename(args.package)
    if problems.errors or song is None or tree is None:
        print_problems(label, problems, sys.stderr)
        sys.stderr.write("refusing to unpack %s\n" % printable(label))
        if tree is not None:
            tree.close()
        return 1
    try:
        slug = song["manifest"]["slug"]
        out = os.path.abspath(args.out or slug)
        if os.path.lexists(out):
            if not os.path.isdir(out):
                sys.stderr.write("%s exists and is not a folder\n" % out)
                return 1
            if os.listdir(out) and not args.force:
                sys.stderr.write("%s is not empty (use --force to write into it)\n" % out)
                return 1
        try:
            if os.path.isdir(out) and os.listdir(out):
                count = extract(tree, out)
                restore_objects(out, song.get("history") or [])
            else:
                # Stage next to the target, then move it into place in one step.
                parent = os.path.dirname(out)
                os.makedirs(parent, exist_ok=True)
                stage = tempfile.mkdtemp(prefix=".unpack-", dir=parent)
                try:
                    count = extract(tree, stage)
                    restore_objects(stage, song.get("history") or [])
                    os.chmod(stage, 0o755)
                    if os.path.isdir(out):
                        os.rmdir(out)
                    os.rename(stage, out)
                except BaseException:
                    shutil.rmtree(stage, ignore_errors=True)
                    raise
        except (ReadProblem, OSError, zipfile.BadZipFile, zlib.error) as e:
            sys.stderr.write("unpack failed: %s\n" % e)
            return 1
    finally:
        tree.close()
    for p in problems.warnings:
        sys.stderr.write("warning  %s: %s [%s]\n" % (printable(p["path"]), printable(p["message"]), p["code"]))
    print("unpacked %d files to %s" % (count, out))
    return 0


def load_song(path, max_total=DEFAULT_MAX_TOTAL, max_ratio=DEFAULT_MAX_RATIO):
    """Open and validate a song for the read-only commands. Returns (tree, song, problems)."""
    problems = Problems()
    tree = open_song(path, problems, max_total, max_ratio)
    song = validate_song(tree, problems, max_total) if tree is not None else None
    return tree, song, problems


def summary(song, problems, tree):
    m = song["manifest"]
    roles = {}
    for entry in m.get("files") or []:
        if isinstance(entry, dict):
            role = entry.get("role") if entry.get("role") in ROLES else "other"
            roles[role] = roles.get(role, 0) + 1
    history = song["history"] or []
    latest = history[-1] if history else None
    comments = (song["review"] or {}).get("comments") if isinstance(song["review"], dict) else None
    counts = {"open": 0, "done": 0}
    for c in comments or []:
        if isinstance(c, dict):
            counts["done" if c.get("status") == "done" else "open"] += 1
    requires = []
    for r in m.get("requires") or []:
        if known_requirement(r):
            plugin = r.get("plugin") if isinstance(r.get("plugin"), dict) else {}
            library = r.get("library") if isinstance(r.get("library"), dict) else {}
            requires.append({"track": r.get("track"), "plugin": plugin.get("name"), "format": plugin.get("format"),
                             "version": plugin.get("version"), "preset": r.get("preset"),
                             "library": {"kind": library.get("kind"), "name": library.get("name")} if library else None,
                             "fallback": r.get("fallback")})
    render = m.get("render") if isinstance(m.get("render"), dict) else None
    current = None
    if render and isinstance(m.get("job"), str) and tree.exists(m["job"]):
        try:
            current = render.get("job") == "sha256:" + hash_file(tree, m["job"])[0]
        except ReadProblem:
            current = None
    authors = [a for a in m.get("authors") or [] if isinstance(a, dict)]
    return {
        "title": m.get("title"), "slug": m.get("slug"), "id": m.get("id"),
        "formatVersion": m.get("formatVersion"), "minReaderVersion": m.get("minReaderVersion"),
        "license": m.get("license"), "authors": authors, "job": m.get("job"),
        "files": roles,
        "history": {"revisions": len(history), "latest": latest.get("rev") if latest else None,
                    "op": latest.get("op") if latest else None, "message": latest.get("message") if latest else None},
        "comments": counts,
        "requires": requires,
        "render": {"revision": render.get("revision"), "mix": render.get("mix"), "current": current} if render else None,
        "problems": {"errors": len(problems.errors), "warnings": len(problems.warnings)},
    }


def cmd_info(args):
    tree, song, problems = load_song(args.song)
    try:
        if song is None:
            print_problems(args.song, problems, sys.stderr)
            return 1
        s = summary(song, problems, tree)
    finally:
        if tree is not None:
            tree.close()
    if args.json:
        emit_json(s)
        return 0
    authors = ", ".join("%s (%s%s)" % (a.get("name"), a.get("role"), ", ai" if a.get("kind") == "ai" else "")
                        for a in s["authors"]) or "-"
    files = ", ".join("%s %d" % (k, v) for k, v in sorted(s["files"].items())) or "none listed"
    h = s["history"]
    hist = ("%d revision%s, latest r%s %s%s" % (h["revisions"], "" if h["revisions"] == 1 else "s", h["latest"],
                                                h["op"], ": " + h["message"] if h["message"] else "")
            if h["revisions"] else "none")
    reqs = []
    for r in s["requires"]:
        lib = r["library"]
        plugin = " ".join(str(x) for x in (r["plugin"], r["format"], r["version"]) if x) or (
            "%s %s" % (lib["kind"], lib["name"]) if lib else "?")
        preset = ", preset %s" % r["preset"] if r["preset"] else ""
        reqs.append("%s: %s%s, fallback %s" % (r["track"], plugin, preset, "yes" if r["fallback"] else "no"))
    reqs = "; ".join(reqs) or "none"
    render = s["render"]
    rows = [
        ("Title", s["title"]), ("Slug", s["slug"]), ("ID", s["id"]),
        ("Format", "%s (min reader %s)" % (s["formatVersion"], s["minReaderVersion"] or "default")),
        ("License", s["license"] or "none (all rights reserved)"), ("Authors", authors),
        ("Job", s["job"]), ("Files", files), ("History", hist),
        ("Comments", "%d open, %d done" % (s["comments"]["open"], s["comments"]["done"])),
        ("Requires", reqs),
        ("Render", "r%s %s, %s" % (render["revision"], render["mix"], {True: "current", False: "out of date",
                                                                       None: "unknown"}[render["current"]])
         if render else "none"),
        ("Problems", "%d error(s), %d warning(s) (run validate)" % (s["problems"]["errors"], s["problems"]["warnings"])
         if s["problems"]["errors"] or s["problems"]["warnings"] else "none"),
    ]
    for k, v in rows:
        print("%-10s %s" % (k + ":", printable(v)))
    return 0


def cmd_history(args):
    tree, song, problems = load_song(args.song)
    if tree is not None:
        tree.close()
    if song is None:
        print_problems(args.song, problems, sys.stderr)
        return 1
    rows = []
    for e in song["history"]:
        render = e.get("render") if isinstance(e.get("render"), dict) else {}
        rows.append({"rev": e.get("rev"), "time": e.get("time"), "op": e.get("op"),
                     "by": (e.get("by") or {}).get("name"), "message": e.get("message"),
                     "target": e.get("target"), "parent": e.get("parent"), "lufs": render.get("lufs"),
                     "files": len(e.get("files") or {})})
    if args.json:
        emit_json(rows)
        return 0
    if not rows:
        print("no history")
        return 0
    for r in rows:
        extra = ""
        if r["target"] is not None:
            extra += " (r%s, from r%s)" % (r["target"], r["parent"])
        if r["lufs"] is not None:
            extra += " [%.1f LUFS]" % r["lufs"]
        print(printable("r%-4s %-25s %-8s %-20s %s%s" % (r["rev"], r["time"], r["op"], r["by"], r["message"] or "", extra)))
    return 0


def cmd_cat(args):
    problems = Problems()
    tree = open_song(args.song, problems)
    if tree is None:
        print_problems(args.song, problems, sys.stderr)
        return 1
    try:
        lines = read_log(tree, Problems()) or []
        entry = None
        for _, e in lines:
            if isinstance(e, dict) and is_int(e.get("rev")) and int(e["rev"]) == args.rev:
                entry = e
        if entry is None:
            sys.stderr.write("revision %d is not in the history\n" % args.rev)
            return 1
        files = entry.get("files") if isinstance(entry.get("files"), dict) else {}
        path = args.path if args.path in files else posixpath.normpath(args.path)
        h = files.get(path)
        if hash_ref(h) == "unsupported":
            sys.stderr.write("r%d tracks %s with a hash this reader doesn't support: %s\n"
                             % (args.rev, printable(args.path), printable(h)))
            return 1
        if hash_ref(h) != "ok":
            sys.stderr.write("r%d does not track %s\n" % (args.rev, printable(args.path)))
            return 1
        rel = OBJECTS + h[7:9] + "/" + h[7:]
        if not tree.exists(rel):
            if tree.kind == "package" and file_sha256(tree, path) == h[7:]:   # left out: it is the file (section 5)
                with tree.open(path) as f:
                    content = f.read()
                sys.stdout.buffer.write(content)
                sys.stdout.flush()
                return 0
            sys.stderr.write("object %s is missing\n" % h)
            return 1
        content, code, message, _ = verify_object(tree, rel, h[7:], MAX_LOG, keep=True)
        if code:
            sys.stderr.write("%s: %s [%s]\n" % (rel, message, code))
            return 1
    finally:
        tree.close()
    sys.stdout.buffer.write(content)
    sys.stdout.flush()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="wavelength_song.py",
                                 description="Read, validate and unpack Wavelength songs (format 1.0) without the engine.")
    sub = ap.add_subparsers(dest="command", required=True)

    def limits(p):
        p.add_argument("--max-size", type=int, default=DEFAULT_MAX_TOTAL, help="largest total unpacked size, bytes")
        p.add_argument("--max-ratio", type=int, default=DEFAULT_MAX_RATIO, help="largest compression ratio per entry")

    p = sub.add_parser("validate", help="check a song folder or package against the spec")
    p.add_argument("song")
    p.add_argument("--json", action="store_true")
    limits(p)
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("unpack", help="check a package, then extract it safely")
    p.add_argument("package")
    p.add_argument("--out", help="target folder (default: ./<slug>)")
    p.add_argument("--force", action="store_true", help="write into a non-empty folder")
    limits(p)
    p.set_defaults(func=cmd_unpack)

    p = sub.add_parser("info", help="summarize a song")
    p.add_argument("song")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("history", help="list a song's revisions")
    p.add_argument("song")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_history)

    p = sub.add_parser("cat", help="print a tracked file as it was at a revision")
    p.add_argument("song")
    p.add_argument("rev", type=int)
    p.add_argument("path")
    p.set_defaults(func=cmd_cat)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
