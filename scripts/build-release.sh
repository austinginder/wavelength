#!/usr/bin/env bash
# Build release binaries for every platform from a git tag, on this Mac (no CI):
#   macOS    universal (arm64 + x86_64), natively
#   Linux    x86_64 and arm64, in Docker (Ubuntu 22.04)
#   Windows  x86_64, cross-compiled in Docker with llvm-mingw
# Each build renders examples/mastering.json as a smoke test (Windows under Wine). Archives and
# SHA256SUMS.txt land in dist/<tag>/; --upload attaches them to the GitHub release for the tag.
#
# Usage: scripts/build-release.sh v0.1.1 [--upload] [--only macos|linux-x86_64|linux-arm64|windows-x86_64]
# Needs Docker running, and gh for --upload.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)
tag=${1:?usage: scripts/build-release.sh <tag> [--upload] [--only target]}
shift
upload=0; only=""
while [ $# -gt 0 ]; do
  case "$1" in
    --upload) upload=1 ;;
    --only) only=$2; shift ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
  shift
done
git rev-parse -q --verify "refs/tags/$tag" > /dev/null || { echo "no tag $tag"; exit 1; }
dist="$root/dist/$tag"
work="$root/dist/.work-$tag"
rm -rf "$work"; mkdir -p "$dist" "$work"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src" && git archive "$tag" | tar x -C "$work/src"

want() { [ -z "$only" ] || [ "$only" = "$1" ]; }
docs="LICENSE THIRD_PARTY.md README.md CHANGELOG.md AGENTS.md docs examples"

# package <target> <binary> [licenses dir]: wavelength-<target>/ with the binary, the docs and
# the licences of anything linked in statically. Names carry no version, so
# releases/latest/download/<name> links stay stable.
package() {
  local target=$1 bin=$2 licenses=${3:-} name="wavelength-$1"
  rm -rf "$work/pkg/$name"; mkdir -p "$work/pkg/$name"
  cp "$bin" "$work/pkg/$name/"
  (cd "$work/src" && cp -R $docs "$work/pkg/$name/")
  [ -n "$licenses" ] && cp -R "$licenses" "$work/pkg/$name/licenses"
  rm -f "$dist/$name".*
  if [[ $target == windows* ]]; then (cd "$work/pkg" && zip -qr "$dist/$name.zip" "$name")
  else (cd "$work/pkg" && COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs -czf "$dist/$name.tar.gz" "$name"); fi
  echo "packaged $name"
}

# smoke <runner...>: render the built-in-only example and check the report
smoke() {
  local out
  out=$("$@" 2>/dev/null) || { echo "smoke test failed: $out"; exit 1; }
  echo "$out" | python3 -c 'import json,sys; r=json.load(sys.stdin); m=r["mix"]; assert not m["levels"]["silent"]; print("  smoke ok:", m["lufs"], "LUFS")'
}

if want macos; then
  echo "== macOS universal"
  cmake -S "$work/src" -B "$work/macos" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 > /dev/null
  cmake --build "$work/macos" -j 10 > /dev/null
  for arch in arm64 x86_64; do
    smoke arch -$arch "$work/macos/wavelength" render "$work/src/examples/mastering.json" --out "$work/smoke-macos-$arch" --json
  done
  package macos-universal "$work/macos/wavelength"
  rm -rf "$work/macos"   # builds hold a few hundred MB of fetched SDKs
fi

for arch in x86_64 arm64; do
  want "linux-$arch" || continue
  echo "== Linux $arch"
  platform=linux/amd64; [ $arch = arm64 ] && platform=linux/arm64
  docker build -q --platform $platform -t "wavelength-build-linux-$arch" -f scripts/docker/linux.Dockerfile scripts/docker > /dev/null
  docker run --rm --platform $platform -v "$work:/work" -w /work "wavelength-build-linux-$arch" bash -c \
    "cmake -S src -B linux-$arch -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build linux-$arch -j 10 > /dev/null && strip linux-$arch/wavelength"
  smoke docker run --rm --platform $platform -v "$work:/work" -w /work "wavelength-build-linux-$arch" \
    ./linux-$arch/wavelength render src/examples/mastering.json --out /tmp/smoke --json
  package "linux-$arch" "$work/linux-$arch/wavelength"
  rm -rf "$work/linux-$arch"
done

if want windows-x86_64; then
  echo "== Windows x86_64"
  docker run --rm -v "$work:/work" -w /work mstorsjo/llvm-mingw:latest bash -c \
    "cmake -S src -B windows -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/work/src/cmake/mingw-x86_64.cmake > /dev/null && cmake --build windows -j 10 > /dev/null && x86_64-w64-mingw32-strip windows/wavelength.exe \
     && mkdir -p windows/licenses && m=/opt/llvm-mingw/x86_64-w64-mingw32/share/mingw32 \
     && cp \$m/COPYING.MinGW-w64-runtime.txt \$m/COPYING.winpthreads.txt windows/licenses/ \
     && cp /opt/llvm-mingw/LICENSE.TXT windows/licenses/LLVM-LICENSE.txt \
     && cp windows/_deps/zlib-src/LICENSE windows/licenses/zlib-LICENSE.txt"
  docker build -q --platform linux/amd64 -t wavelength-wine -f scripts/docker/wine.Dockerfile scripts/docker > /dev/null
  smoke docker run --rm --platform linux/amd64 -v "$work:/work" -w /work wavelength-wine \
    wine64 windows/wavelength.exe render 'src\examples\mastering.json' --out 'C:\smoke' --json
  for f in COPYING.MinGW-w64-runtime.txt COPYING.winpthreads.txt LLVM-LICENSE.txt zlib-LICENSE.txt; do
    [ -s "$work/windows/licenses/$f" ] || { echo "missing licence $f for the Windows build"; exit 1; }
  done
  package windows-x86_64 "$work/windows/wavelength.exe" "$work/windows/licenses"
  rm -rf "$work/windows"
fi

(cd "$dist" && rm -f SHA256SUMS.txt && shasum -a 256 wavelength-* > SHA256SUMS.txt && cat SHA256SUMS.txt)
if [ $upload = 1 ]; then
  gh release upload "$tag" "$dist"/wavelength-* "$dist/SHA256SUMS.txt" --clobber
  echo "uploaded to $(gh release view "$tag" --json url --jq .url)"
fi
