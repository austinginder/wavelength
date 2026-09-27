#!/usr/bin/env bash
# Starts Wavelength's MCP server (`wavelength mcp`) for the Claude Code plugin: uses the installed
# engine, or installs the release build on first run (install.sh). Everything but the server itself
# goes to stderr: stdout belongs to the MCP connection.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
home="${WAVELENGTH_HOME:-$HOME/.wavelength}"
# an engine that has `docs` has the MCP server too (both arrived in 0.4.0)
serves() { [ -n "$1" ] && [ -x "$1" ] && "$1" docs --json >/dev/null 2>&1; }
bin=""
for candidate in "$(command -v wavelength 2>/dev/null || true)" "$home/bin/wavelength" "$home/bin/wavelength.exe"; do
  if serves "$candidate"; then bin="$candidate"; break; fi
done
if [ -z "$bin" ]; then
  mode=""
  [ -x "$home/bin/wavelength" ] || [ -x "$home/bin/wavelength.exe" ] && mode="--update"
  info="$(bash "$here/install.sh" $mode)" || true
  bin="$(printf '%s\n' "$info" | sed -n 's/^WAVELENGTH=//p' | tail -1)"
fi
if ! serves "$bin"; then
  echo "wavelength: no engine with an MCP server found or installable (it needs Wavelength 0.4.0 or newer;" \
       "run install.sh --source to build the latest from source)" >&2
  exit 1
fi
exec "$bin" mcp
