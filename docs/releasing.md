# Releasing

Releases are cut by hand on a Mac; binaries are built locally, not in CI.

1. In `CHANGELOG.md`, rename `## [Unreleased]` to `## [x.y.z] - YYYY-MM-DD` (add an empty
   `## [Unreleased]` above it).
2. In `CMakeLists.txt`, set `project(... VERSION x.y.z)` and clear `WAVELENGTH_VERSION_SUFFIX`.
3. `scripts/check.sh` must pass.
4. Commit `🚀 RELEASE: vx.y.z`, tag `vx.y.z`, push the commit and the tag.
5. `gh release create vx.y.z --title vx.y.z --notes-file <the version's changelog section>`.
6. With Docker Desktop running: `scripts/build-release.sh vx.y.z --upload`. It builds and
   smoke-tests every target and attaches the archives and `SHA256SUMS.txt` to the release:

   | Archive | Built |
   |---|---|
   | `wavelength-macos-universal.tar.gz` | natively (arm64 + x86_64, macOS 12+) |
   | `wavelength-linux-x86_64.tar.gz`, `wavelength-linux-arm64.tar.gz` | in Docker, Ubuntu 22.04 (glibc 2.35+) |
   | `wavelength-windows-x86_64.zip` | in Docker with llvm-mingw, smoke-tested under Wine |

   Archive names carry no version, so `releases/latest/download/<name>` links always fetch the
   newest release. The build needs about 2 GB of free disk; each target's build folder is
   deleted after packaging. `--only <target>` rebuilds one target.
7. Bump to the next `-dev` version (`VERSION` + `WAVELENGTH_VERSION_SUFFIX "-dev"`) and push.

The macOS binary is not notarized (there is no Developer ID certificate yet); the README tells
browser downloads to clear the quarantine flag.
