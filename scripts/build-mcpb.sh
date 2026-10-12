#!/usr/bin/env bash
# Build wavelength.mcpb, an MCP Bundle of `wavelength mcp`, from a release's archives, and the
# server.json the MCP Registry lists it by. Claude Desktop installs a .mcpb with a double-click;
# the MCP Registry and Smithery list it. One bundle carries every platform's binary: macOS
# universal, Windows x86_64, and Linux x86_64 and arm64 behind a launcher that picks by `uname -m`
# (a bundle chooses by OS only). The manifest's tool list is the macOS binary's own tools/list.
#
# Usage: scripts/build-mcpb.sh <tag> [dist dir]   (default dist/<tag>; build-release.sh runs it)
# Writes <dist>/wavelength.mcpb and <dist>/server.json. Needs npx (Node) and a Mac.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)
tag=${1:?usage: scripts/build-mcpb.sh <tag> [dist dir]}
dist=$(cd "${2:-$root/dist/$tag}" && pwd)
version=${tag#v}
mcpb="@anthropic-ai/mcpb@2.1.2"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
b="$work/wavelength"
mkdir -p "$b/server/darwin" "$b/server/win32" "$b/server/linux/x86_64" "$b/server/linux/arm64"

# take <archive> <folder>: unpack a release archive into $work/<archive> and copy its binary to <folder>
take() {
  local x="$work/$1"
  [ -f "$dist/$1" ] || { echo "missing $dist/$1"; exit 1; }
  mkdir -p "$x"
  case "$1" in *.zip) unzip -q "$dist/$1" -d "$x" ;; *) tar -xzf "$dist/$1" -C "$x" ;; esac
  find "$x" -type f \( -name wavelength -o -name wavelength.exe \) -exec cp {} "$2/" \;
}
take wavelength-macos-universal.tar.gz "$b/server/darwin"
take wavelength-windows-x86_64.zip "$b/server/win32"
take wavelength-linux-x86_64.tar.gz "$b/server/linux/x86_64"
take wavelength-linux-arm64.tar.gz "$b/server/linux/arm64"
chmod +x "$b/server/darwin/wavelength" "$b"/server/linux/*/wavelength
cat > "$b/server/linux/run.sh" <<'SH'
#!/bin/sh
# the Linux binary for this machine
d="$(dirname "$0")"
case "$(uname -m)" in aarch64|arm64) a=arm64 ;; *) a=x86_64 ;; esac
exec "$d/$a/wavelength" "$@"
SH
chmod +x "$b/server/linux/run.sh"
# the licences: Wavelength's, what it bundles, and the Windows build's runtime
mac="$work/wavelength-macos-universal.tar.gz/wavelength-macos-universal"
cp "$mac/license" "$b/LICENSE"
cp "$mac/third_party.md" "$b/"
cp -R "$work/wavelength-windows-x86_64.zip/wavelength-windows-x86_64/licenses" "$b/licenses"
cp scripts/mcpb/icon.png "$b/icon.png"

python3 - "$b" "$version" "$dist" <<'PY'
import json, subprocess, sys
b, version, dist = sys.argv[1:4]
p = subprocess.Popen([b + "/server/darwin/wavelength", "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
def call(i, method, params):
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": i, "method": method, "params": params}) + "\n"); p.stdin.flush()
    while True:
        m = json.loads(p.stdout.readline())
        if m.get("id") == i: return m["result"]
call(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "build-mcpb", "version": "1"}})
tools = [{"name": t["name"], "description": t.get("title") or t["description"]} for t in call(2, "tools/list", {})["tools"]]
p.stdin.close(); p.wait(timeout=10)
run = ["mcp"]
manifest = {
    "manifest_version": "0.3",
    "name": "wavelength",
    "display_name": "Wavelength",
    "version": version,
    "description": "Compose, render, mix and master music through the synthesizers and sample libraries on this computer.",
    "long_description": "Wavelength is a headless music engine for AI agents. The agent writes a JSON job (notes, sounds, effects, mix); "
        "Wavelength plays it offline through the CLAP, VST3, VST2 and Audio Unit plugins and sample libraries installed here, or its "
        "built-in synth, drums and effects, and answers with a mix, stems, loudness per track and section, and a picture of the song. "
        "The tools read the guide, list instruments, presets, samples and parameters, draw patch cards, save presets, lint, render, "
        "stage faders, analyze audio, import MIDI, MusicXML, DAWproject, Bitwig and GarageBand projects, and keep a song's history "
        "and review comments.",
    "author": {"name": "Austin Ginder", "url": "https://wavelength.run"},
    "repository": {"type": "git", "url": "https://github.com/austinginder/wavelength"},
    "homepage": "https://wavelength.run",
    "documentation": "https://github.com/austinginder/wavelength/blob/main/AGENTS.md",
    "support": "https://github.com/austinginder/wavelength/issues",
    "icon": "icon.png",
    "server": {
        "type": "binary",
        "entry_point": "server/darwin/wavelength",
        "mcp_config": {
            "command": "${__dirname}/server/darwin/wavelength",
            "args": run,
            "platform_overrides": {
                "win32": {"command": "${__dirname}/server/win32/wavelength.exe", "args": run},
                "linux": {"command": "/bin/sh", "args": ["${__dirname}/server/linux/run.sh"] + run},
            },
        },
    },
    "tools": tools,
    "keywords": ["music", "audio", "synthesizer", "composition", "mixing", "mastering", "clap", "vst3", "midi"],
    "license": "MIT",
    "compatibility": {"platforms": ["darwin", "win32", "linux"]},
}
json.dump(manifest, open(b + "/manifest.json", "w"), indent=2)
print(f"manifest: {len(tools)} tools")
PY

npx -y "$mcpb" validate "$b/manifest.json"
rm -f "$dist/wavelength.mcpb"
npx -y "$mcpb" pack "$b" "$dist/wavelength.mcpb"

python3 - "$dist" "$tag" "$version" <<'PY'
import hashlib, json, sys
dist, tag, version = sys.argv[1:4]
sha = hashlib.sha256(open(dist + "/wavelength.mcpb", "rb").read()).hexdigest()
server = {
    "$schema": "https://static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json",
    "name": "io.github.austinginder/wavelength",
    "title": "Wavelength",
    "description": "Compose, render, mix and master music through the synths installed on this computer.",
    "repository": {"url": "https://github.com/austinginder/wavelength", "source": "github"},
    "websiteUrl": "https://wavelength.run",
    "version": version,
    "packages": [{
        "registryType": "mcpb",
        "identifier": f"https://github.com/austinginder/wavelength/releases/download/{tag}/wavelength.mcpb",
        "fileSha256": sha,
        "transport": {"type": "stdio"},
    }],
}
assert len(server["description"]) <= 100
json.dump(server, open(dist + "/server.json", "w"), indent=2)
print("server.json:", server["packages"][0]["identifier"], sha)
PY
