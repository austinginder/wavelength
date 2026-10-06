# Releasing

Releases are cut by hand on a Mac; binaries are built locally, not in CI.

1. In `changelog.md`, rename `## [Unreleased]` to `## [x.y.z] - YYYY-MM-DD` (add an empty `## [Unreleased]` above it). In `roadmap.md`, mark what shipped as `released x.y.z` (rows still saying `x.y.z` were on main), add rows for shipped features the roadmap doesn't list, and set the legend's on-main version to the next one; update the readme's "Next after" line. Check that `wavelength help` and AGENTS.md's "Known limits" don't still call a shipped feature missing.
2. In `CMakeLists.txt`, set `project(... VERSION x.y.z)` and clear `WAVELENGTH_VERSION_SUFFIX`; set the same version in `.claude-plugin/plugin.json` (Claude Code updates plugins by it).
3. `scripts/check.sh` must pass, and `wavelength compat --report compat.md` (every installed plugin, one worker at a time, about an hour; cached results of unchanged plugins are reused within one engine version) must show no plugin newly failing against the last release's report. Compare the two reports' Failing sections; a plugin that fails for its own reasons (a crash in its code, a licence or ROM window) stays listed there.
4. Commit `🚀 RELEASE: vx.y.z`, tag `vx.y.z`, push the commit and the tag.
5. `gh release create vx.y.z --title vx.y.z --notes-file <the version's changelog section>`.
6. With Docker Desktop running: `scripts/build-release.sh vx.y.z --upload`. It builds and smoke-tests every target and attaches the archives and `SHA256SUMS.txt` to the release:

   | Archive | Built |
   |---|---|
   | `wavelength-macos-universal.tar.gz` | natively (arm64 + x86_64, macOS 12+) |
   | `wavelength-linux-x86_64.tar.gz`, `wavelength-linux-arm64.tar.gz` | in Docker, Ubuntu 22.04 (glibc 2.35+) |
   | `wavelength-windows-x86_64.zip` | in Docker with llvm-mingw, smoke-tested under Wine |

   Archive names carry no version, so `releases/latest/download/<name>` links always fetch the newest release. The build needs about 2 GB of free disk; each target's build folder is deleted after packaging. `--only <target>` rebuilds one target. Then `scripts/release-test.sh vx.y.z` runs `scripts/release-test.py` against every archive (macOS arm64 and x86_64 natively, Linux in Docker, Windows under Wine): the song workflow, the format fixtures, a package that must be refused, a package of plugin and library tracks that must play through its fallbacks, the built-in docs, kit and the MCP server over stdin/stdout. Run it before `--upload`, or build without `--upload` first.
7. Bump to the next `-dev` version (`VERSION` + `WAVELENGTH_VERSION_SUFFIX "-dev"`, and `.claude-plugin/plugin.json`) and push.

The macOS binary is not notarized (there is no Developer ID certificate yet); the README tells browser downloads to clear the quarantine flag.
