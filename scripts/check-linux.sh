#!/usr/bin/env bash
# The regression checks on Linux, in Docker: builds this checkout into build-linux-<arch> (ignored by git) and runs
# scripts/check.sh there, with no plugins and no sample libraries installed, so it shows what works on a fresh
# machine (examples play their built-in fallbacks; checks that need a Mac or installed plugins are skipped).
# Usage: scripts/check-linux.sh [arm64|x86_64]   (default: this machine's architecture, which runs natively)
set -euo pipefail
cd "$(dirname "$0")/.."
arch="${1:-$(uname -m | sed 's/aarch64/arm64/')}"
platform=linux/arm64; [ "$arch" = x86_64 ] && platform=linux/amd64
docker build -q --platform "$platform" -t "wavelength-check-linux-$arch" -f scripts/docker/check.Dockerfile scripts/docker > /dev/null
docker run --rm --platform "$platform" -v "$PWD:/src" -w /src -e BUILD="build-linux-$arch" -e JOBS="${JOBS:-6}" \
  "wavelength-check-linux-$arch" scripts/check.sh
