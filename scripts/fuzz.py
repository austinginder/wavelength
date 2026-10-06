#!/usr/bin/env python3
"""Mutation fuzzing of Wavelength's file readers through `wavelength __parse`.

Takes seed files (generated here from the regression fixtures and examples), mutates them (bit flips, interesting
integers over 1-, 2-, 4- and 8-byte fields, inserted, deleted and repeated runs, truncation, splices of two seeds) and
feeds them to `__parse <format>` in batches. A batch that crashes, trips a sanitizer or hangs is re-run one file at a
time; each culprit is kept in <out>/<format>/crashes with the binary's stderr. A reader may refuse a file or throw (both
are answers); it must not crash, read outside its buffer or hang.

Build a sanitizer tree first, so reads outside a buffer stop the process instead of passing quietly:
  cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 \\
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \\
        -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
  cmake --build build-asan -j 8

Usage: scripts/fuzz.py [--binary build-asan/wavelength] [--formats bplist,zip,...] [--iterations 2000]
                       [--batch 50] [--timeout 30] [--seed 1] [--out out/fuzz]
With no --formats it runs every format that has seeds. Exit status 1 when anything crashed or hung.
"""
import argparse
import glob
import os
import random
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# format -> (seed globs relative to the seed folder, the file inside a folder seed to mutate or None)
FORMATS = {
    "bplist": (["band/**/*.plist", "exs/**/*.plist"], None),
    "zip": (["fixtures/*.wavelength", "export/*.dawproject"], None),
    "xml": (["scores/*.musicxml", "dspreset/**/*.dspreset", "band/**/ProjectInformation.plist"], None),
    "musicxml": (["scores/*.musicxml"], None),
    "midi": (["export/*.mid"], None),
    "dawproject": (["export/*.dawproject"], None),
    "sf2": (["sf2/*.sf2"], None),
    "audio": (["ir/*.wav", "loop/*.caf", "band/**/*.wav"], None),
    "sfz": (["sfz/*.sfz"], None),
    "exs": (["exs/**/*.exs"], None),
    "caf": (["loop/*.caf"], None),
    "dspreset": (["dspreset/**/*.dspreset"], None),
    "band": (["band/*.band"], "Alternatives/000/ProjectData"),
    "patch": (["exs/**/*.patch"], "*.cst"),
}

INTERESTING = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
               0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFF6]


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)


def make_seeds(binary, d):
    """Fixtures from the scripts the regression checks use, plus examples and exports of them."""
    os.makedirs(d, exist_ok=True)
    py = sys.executable
    run([py, os.path.join(HERE, "make-test-band.py"), os.path.join(d, "band")])
    run([py, os.path.join(HERE, "make-test-exs.py"), os.path.join(d, "exs")])
    os.makedirs(os.path.join(d, "sf2"), exist_ok=True)
    run([py, os.path.join(HERE, "make-test-sf2.py"), os.path.join(d, "sf2", "test.sf2")])
    run([py, os.path.join(HERE, "make-test-dspreset.py"), os.path.join(d, "dspreset")])
    os.makedirs(os.path.join(d, "loop"), exist_ok=True)
    run([py, os.path.join(HERE, "make-test-apple-loop.py"), os.path.join(d, "loop", "Test Loop.caf")])
    run([py, os.path.join(HERE, "make-song-fixtures.py"), os.path.join(d, "fixtures")])
    for sub in ("scores", "sfz", "ir"):
        shutil.copytree(os.path.join(REPO, "examples", sub), os.path.join(d, sub), dirs_exist_ok=True)
    os.makedirs(os.path.join(d, "export"), exist_ok=True)
    for name in ("arrangement-tour", "midifx-tour"):
        job = os.path.join(REPO, "examples", name + ".json")
        run([binary, "export", job, "--out", os.path.join(d, "export", name + ".mid")])
        run([binary, "export", job, "--out", os.path.join(d, "export", name + ".dawproject"), "--no-print"])


def seeds_for(fmt, d):
    globs, _ = FORMATS[fmt]
    found = []
    for g in globs:
        found += glob.glob(os.path.join(d, g), recursive=True)
    return sorted(set(found))


def mutate(data, rng, other):
    b = bytearray(data)
    for _ in range(rng.choice([1, 1, 2, 3, 6])):
        op = rng.randrange(8)
        n = len(b)
        if n == 0:
            b += bytes(rng.randrange(256) for _ in range(8))
            continue
        at = rng.randrange(n)
        if op == 0:   # flip a bit
            b[at] ^= 1 << rng.randrange(8)
        elif op == 1:   # a random byte
            b[at] = rng.randrange(256)
        elif op == 2:   # an interesting integer over a field, either byte order
            size = rng.choice([1, 2, 4, 8])
            v = rng.choice(INTERESTING) & ((1 << (8 * size)) - 1)
            raw = v.to_bytes(size, rng.choice(["little", "big"]))
            b[at:at + size] = raw
        elif op == 3:   # delete a run
            del b[at:at + rng.randrange(1, 64)]
        elif op == 4:   # insert a run
            b[at:at] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 32)))
        elif op == 5:   # repeat a run (nesting, duplicated chunks)
            run_ = b[at:at + rng.randrange(1, 64)]
            b[at:at] = run_ * rng.randrange(2, 64)
        elif op == 6:   # truncate
            del b[rng.randrange(n):]
        else:   # splice another seed in
            if other:
                o = rng.choice(other)
                s = rng.randrange(len(o)) if o else 0
                b[at:at + rng.randrange(1, 256)] = o[s:s + rng.randrange(1, 256)]
    return bytes(b)


def target_in(folder, pattern):
    hits = sorted(glob.glob(os.path.join(folder, pattern)))
    return hits[0] if hits else None


def parse_batch(binary, fmt, paths, timeout):
    """(returncode or 'hang', stderr)"""
    try:
        p = subprocess.run([binary, "__parse", fmt] + paths, capture_output=True, text=True, timeout=timeout,
                           errors="replace", env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                                                      UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
        bad = p.returncode != 0 or "AddressSanitizer" in p.stderr or "runtime error:" in p.stderr
        return (p.returncode if bad else 0), p.stderr
    except subprocess.TimeoutExpired as e:
        return "hang", (e.stderr or b"").decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or "")


def fuzz_format(fmt, args, seed_dir, rng):
    seeds = seeds_for(fmt, seed_dir)
    inner = FORMATS[fmt][1]
    if not seeds:
        print("%-10s no seeds, skipped" % fmt)
        return 0
    seed_bytes = []
    for s in seeds:
        t = target_in(s, inner) if inner else s
        if t:
            with open(t, "rb") as f:
                seed_bytes.append((s, t, f.read()))
    if not seed_bytes:
        print("%-10s no seed has %s, skipped" % (fmt, inner))
        return 0
    crash_dir = os.path.join(args.out, fmt, "crashes")
    found = 0
    done = 0
    with tempfile.TemporaryDirectory(prefix="wl-fuzz-") as tmp:
        while done < args.iterations:
            paths = []
            for k in range(min(args.batch, args.iterations - done)):
                seed, target, data = rng.choice(seed_bytes)
                mutated = mutate(data, rng, [x[2] for x in seed_bytes])
                if inner:   # a folder format: a copy of the folder with its one file mutated
                    dst = os.path.join(tmp, "%d-%s" % (k, os.path.basename(seed)))
                    shutil.rmtree(dst, ignore_errors=True)
                    shutil.copytree(seed, dst)
                    with open(os.path.join(dst, os.path.relpath(target, seed)), "wb") as f:
                        f.write(mutated)
                else:
                    dst = os.path.join(tmp, "%d%s" % (k, os.path.splitext(seed)[1]))
                    with open(dst, "wb") as f:
                        f.write(mutated)
                paths.append(dst)
            code, _ = parse_batch(args.binary, fmt, paths, args.timeout * len(paths) // 10 + args.timeout)
            if code:
                for p in paths:   # find the culprits one at a time
                    c, err = parse_batch(args.binary, fmt, [p], args.timeout)
                    if not c:
                        continue
                    found += 1
                    os.makedirs(crash_dir, exist_ok=True)
                    name = "%s-%d-%s" % (fmt, found, "hang" if c == "hang" else "exit%s" % c)
                    keep = os.path.join(crash_dir, name + (os.path.splitext(p)[1] if not inner else ".band" if fmt == "band" else ".patch"))
                    (shutil.copytree if os.path.isdir(p) else shutil.copy)(p, keep)
                    with open(os.path.join(crash_dir, name + ".txt"), "w") as f:
                        f.write(err[-20000:])
                    print("  %s: %s (kept as %s)" % (fmt, "hang" if c == "hang" else "exit %s" % c, keep))
            done += len(paths)
    print("%-10s %d inputs from %d seeds, %d crash%s or hang%s" % (fmt, done, len(seed_bytes), found, "" if found == 1 else "es", "" if found == 1 else "s"))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--binary", default=os.path.join(REPO, "build-asan", "wavelength"))
    ap.add_argument("--formats", default=",".join(FORMATS))
    ap.add_argument("--iterations", type=int, default=2000)
    ap.add_argument("--batch", type=int, default=50)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default=os.path.join(REPO, "out", "fuzz"))
    args = ap.parse_args()
    args.binary = os.path.abspath(args.binary)
    if not os.path.exists(args.binary):
        sys.exit("no binary at %s (build the sanitizer tree first; see --help)" % args.binary)
    seed_dir = os.path.join(args.out, "seeds")
    shutil.rmtree(seed_dir, ignore_errors=True)
    make_seeds(args.binary, seed_dir)
    rng = random.Random(args.seed)
    total = 0
    for fmt in args.formats.split(","):
        if fmt not in FORMATS:
            sys.exit("unknown format %s (formats: %s)" % (fmt, ", ".join(FORMATS)))
        total += fuzz_format(fmt, args, seed_dir, rng)
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
