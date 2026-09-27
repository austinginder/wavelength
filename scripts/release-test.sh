#!/usr/bin/env bash
# Run scripts/release-test.py against every archive scripts/build-release.sh made for a tag: macOS
# (arm64 and x86_64 slices) natively, Linux in the same Docker images the build used, Windows under Wine.
#
# Usage: scripts/release-test.sh v0.4.0 [--only macos|linux-x86_64|linux-arm64|windows-x86_64]
# Needs Docker running and the images from a build-release.sh run of this machine.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)
tag=${1:?usage: scripts/release-test.sh <tag> [--only target]}
only=${3:-}
[ "${2:-}" = "--only" ] || only=""
dist="$root/dist/$tag"
[ -d "$dist" ] || { echo "no $dist: run scripts/build-release.sh $tag first"; exit 1; }
work="$root/dist/.test-$tag"
rm -rf "$work"; mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
want() { [ -z "$only" ] || [ "$only" = "$1" ]; }
status=0
run() { "$@" || status=1; }

if want macos; then
  mkdir -p "$work/macos" && tar -xzf "$dist/wavelength-macos-universal.tar.gz" -C "$work/macos"
  bin="$work/macos/wavelength-macos-universal/wavelength"
  run python3 scripts/release-test.py "$work/run-macos-arm64" macos-arm64 arch -arm64 "$bin"
  run python3 scripts/release-test.py "$work/run-macos-x86_64" macos-x86_64 arch -x86_64 "$bin"
fi
for arch in x86_64 arm64; do
  want "linux-$arch" || continue
  platform=linux/amd64; [ $arch = arm64 ] && platform=linux/arm64
  mkdir -p "$work/linux-$arch" && tar -xzf "$dist/wavelength-linux-$arch.tar.gz" -C "$work/linux-$arch"
  run docker run --rm --platform $platform -v "$root:/repo:ro" -v "$work:/w" "wavelength-build-linux-$arch" \
    python3 /repo/scripts/release-test.py /w/run-linux-$arch linux-$arch /w/linux-$arch/wavelength-linux-$arch/wavelength
done
if want windows-x86_64; then
  mkdir -p "$work/windows" && (cd "$work/windows" && unzip -q "$dist/wavelength-windows-x86_64.zip")
  run docker run --rm --platform linux/amd64 -v "$root:/repo:ro" -v "$work:/w" wavelength-wine \
    python3 /repo/scripts/release-test.py /w/run-windows windows wine64 /w/windows/wavelength-windows-x86_64/wavelength.exe
fi
[ $status = 0 ] && echo "release tests passed for $tag" || echo "release tests FAILED for $tag"
exit $status
