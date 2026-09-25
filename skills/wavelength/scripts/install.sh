#!/usr/bin/env bash
# Finds or installs the Wavelength engine, then prints where everything is.
#
#   install.sh            use an installed wavelength, else download a release, else build from source
#   install.sh --source   always build from source (latest main)
#   install.sh --update   replace the installed copy (release, else source)
#
# Installs into $WAVELENGTH_HOME (default ~/.wavelength):
#   bin/wavelength   the engine
#   src/             the source checkout (docs, scripts, examples; the build when compiled here)
# Prints KEY=value lines at the end (WAVELENGTH, WAVELENGTH_DOCS, WAVELENGTH_SCRIPTS, VERSION).
set -euo pipefail

REPO="austinginder/wavelength"
HOME_DIR="${WAVELENGTH_HOME:-$HOME/.wavelength}"
BIN_DIR="$HOME_DIR/bin"
SRC_DIR="$HOME_DIR/src"
MODE="auto"
case "${1:-}" in
  --source) MODE="source" ;;
  --update) MODE="update" ;;
  "") ;;
  *) echo "usage: install.sh [--source | --update]" >&2; exit 2 ;;
esac

say() { printf '%s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Darwin" ] || die "Wavelength runs on macOS for now (it hosts macOS CLAP and VST3 plugins)."
ARCH="$(uname -m)"   # arm64 or x86_64

# 1. already installed?
find_existing() {
  for c in "${WAVELENGTH:-}" "$(command -v wavelength 2>/dev/null || true)" "$BIN_DIR/wavelength" "$SRC_DIR/build/wavelength"; do
    if [ -n "$c" ] && [ -x "$c" ] && "$c" version >/dev/null 2>&1; then echo "$c"; return 0; fi
  done
  return 1
}

# source checkout: docs and scripts come from here even when the binary is a release
ensure_source() {
  command -v git >/dev/null || die "git is required (xcode-select --install)"
  if [ -d "$SRC_DIR/.git" ]; then
    git -C "$SRC_DIR" fetch --quiet --tags origin || say "note: could not fetch updates for $SRC_DIR; using it as is"
  else
    mkdir -p "$HOME_DIR"
    git clone --quiet "https://github.com/$REPO.git" "$SRC_DIR" || die "could not clone https://github.com/$REPO"
  fi
}
# put the checkout on a ref (a release tag, or origin/main for source builds)
checkout_ref() {
  git -C "$SRC_DIR" checkout --quiet "$1" 2>/dev/null || say "note: could not switch $SRC_DIR to $1 (local changes?)"
}

# 2. a release asset for this Mac
install_release() {
  command -v curl >/dev/null || return 1
  local api="https://api.github.com/repos/$REPO/releases/latest" json url tmp
  json="$(curl -fsSL "$api" 2>/dev/null)" || return 1
  url="$(printf '%s' "$json" | grep -o '"browser_download_url": *"[^"]*"' | sed 's/.*"\(http[^"]*\)"/\1/' \
        | grep -i "macos" | grep -i -E "$ARCH|universal" | head -1)"
  [ -n "$url" ] || return 1
  say "downloading $url"
  tmp="$(mktemp -d)"
  curl -fsSL "$url" -o "$tmp/asset" || return 1
  case "$url" in
    *.zip) unzip -q "$tmp/asset" -d "$tmp/x" ;;
    *.tar.gz|*.tgz) mkdir -p "$tmp/x" && tar -xzf "$tmp/asset" -C "$tmp/x" ;;
    *) return 1 ;;
  esac
  local found
  found="$(find "$tmp/x" -type f -name wavelength -perm -u+x | head -1)"
  [ -n "$found" ] || return 1
  mkdir -p "$BIN_DIR"
  cp "$found" "$BIN_DIR/wavelength"
  xattr -d com.apple.quarantine "$BIN_DIR/wavelength" 2>/dev/null || true
  "$BIN_DIR/wavelength" version >/dev/null 2>&1 || return 1
}

# 3. build from source
install_source() {
  command -v cmake >/dev/null || die "CMake 3.20+ is required to build from source (brew install cmake)"
  xcode-select -p >/dev/null 2>&1 || die "the Xcode command line tools are required (xcode-select --install)"
  ensure_source
  checkout_ref origin/main
  say "building Wavelength (first build fetches the CLAP and VST3 SDKs; a few minutes)"
  cmake -S "$SRC_DIR" -B "$SRC_DIR/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$SRC_DIR/build" -j "$(sysctl -n hw.ncpu)" >/dev/null
  mkdir -p "$BIN_DIR"
  cp "$SRC_DIR/build/wavelength" "$BIN_DIR/wavelength"
}

BIN=""
if [ "$MODE" = "auto" ]; then BIN="$(find_existing || true)"; fi
if [ -z "$BIN" ]; then
  if [ "$MODE" != "source" ] && install_release; then say "installed the release build"
  else
    [ "$MODE" = "source" ] || say "no release build for macOS $ARCH; building from source"
    install_source
  fi
  BIN="$BIN_DIR/wavelength"
fi

# docs and helper scripts must match the binary: a checkout next to it (a source build
# elsewhere), else our checkout on the binary's release tag (or main for a -dev build)
VERSION="$("$BIN" version 2>/dev/null | awk '{print $2}')"
DOCS=""
if [ -f "$(dirname "$BIN")/../AGENTS.md" ]; then DOCS="$(cd "$(dirname "$BIN")/.." && pwd)"
else
  ensure_source
  case "$VERSION" in
    *-dev|"") : ;;   # built from main: the checkout already matches
    *) if git -C "$SRC_DIR" rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null; then checkout_ref "v$VERSION"; fi ;;
  esac
  DOCS="$SRC_DIR"
fi

command -v ffmpeg >/dev/null || say "note: ffmpeg is not installed (brew install ffmpeg); it is only needed to make MP3s"
[ -d "$BIN_DIR" ] && case ":$PATH:" in *":$BIN_DIR:"*) ;; *) say "note: add $BIN_DIR to PATH to run 'wavelength' directly";; esac

echo "WAVELENGTH=$BIN"
echo "WAVELENGTH_DOCS=$DOCS"
echo "WAVELENGTH_SCRIPTS=$DOCS/scripts"
echo "VERSION=$VERSION"
