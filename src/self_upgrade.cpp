#include "self_upgrade.hpp"

#include "kit.hpp"
#include "platform.hpp"
#include "sha256.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef WAVELENGTH_VERSION
#define WAVELENGTH_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace wl {

namespace {

using json = nlohmann::json;

const char *kReleasesApi = "https://api.github.com/repos/austinginder/wavelength/releases/latest";

struct Version {
    int part[3] = {0, 0, 0};
    std::string pre;   // "dev" in "0.4.0-dev": before 0.4.0
};

Version parseVersion(std::string s) {
    Version v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    const size_t dash = s.find('-');
    if (dash != std::string::npos) v.pre = s.substr(dash + 1);
    std::stringstream ss(s.substr(0, dash));
    std::string n;
    for (int i = 0; i < 3 && std::getline(ss, n, '.'); ++i)
        v.part[i] = !n.empty() && n.find_first_not_of("0123456789") == std::string::npos && n.size() < 7 ? std::stoi(n) : 0;
    return v;
}

// the release archive for this computer, as scripts/build-release.sh names them
std::string assetName() {
#if defined(__APPLE__)
    return "wavelength-macos-universal.tar.gz";
#elif defined(_WIN32)
    return "wavelength-windows-x86_64.zip";
#else
    return "wavelength-linux-" + platform::hostArch() + ".tar.gz";
#endif
}

#ifdef _WIN32
const char *kExe = "wavelength.exe";
#else
const char *kExe = "wavelength";
#endif

std::string readAll(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// what `<bin> version --json` says it is ("" when it doesn't run)
std::string versionOf(const fs::path &bin) {
    platform::Process p;
    std::string out, crash;
    if (!platform::spawn({bin.string(), "version", "--json"}, p, true, true)) return "";
    platform::readOutput(p, out, 60);
    while (!platform::finished(p, crash)) platform::pumpEvents(20);
    try { return json::parse(out).value("version", std::string()); } catch (...) { return ""; }
}

} // namespace

int compareVersions(const std::string &a, const std::string &b) {
    const Version x = parseVersion(a), y = parseVersion(b);
    for (int i = 0; i < 3; ++i)
        if (x.part[i] != y.part[i]) return x.part[i] < y.part[i] ? -1 : 1;
    if (x.pre == y.pre) return 0;
    if (x.pre.empty()) return 1;   // a release comes after its own pre-releases and dev builds
    if (y.pre.empty()) return -1;
    return x.pre < y.pre ? -1 : 1;
}

bool selfUpgrade(const UpgradeOptions &opt, json &result, std::string &err) {
    const std::string current = WAVELENGTH_VERSION;
    std::error_code ec;
    fs::path self = fs::weakly_canonical(fs::u8path(platform::selfExecutable()), ec);   // through any symlink to the real file
    if (ec || self.empty()) { err = "cannot find this program's own file"; return false; }
    fs::remove(self.string() + ".old", ec);   // left by an upgrade on Windows, which can't delete a running program
    const fs::path work = platform::cacheDir() / "upgrade";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code e; fs::remove_all(p, e); } } cleanup{work};

    // the latest release (WAVELENGTH_RELEASES_API points elsewhere, for tests: a file:// URL works)
    const char *env = std::getenv("WAVELENGTH_RELEASES_API");
    const std::string api = env && *env ? env : kReleasesApi;
    if (!downloadFile(api, work / "release.json", 0, err)) { err = "could not reach the GitHub releases (" + api + "): " + err; return false; }
    json release;
    try { release = json::parse(readAll(work / "release.json")); } catch (...) { err = "the releases answer isn't JSON"; return false; }
    std::string latest = release.value("tag_name", std::string());
    if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
    if (latest.empty()) { err = "the latest release has no version (tag_name)"; return false; }
    std::string assetUrl, sumsUrl;
    uint64_t assetBytes = 0;
    for (auto &a : release.value("assets", json::array())) {
        if (!a.is_object()) continue;
        const std::string name = a.value("name", std::string()), url = a.value("browser_download_url", std::string());
        if (name == assetName()) { assetUrl = url; assetBytes = a.value("size", (uint64_t)0); }
        else if (name == "SHA256SUMS.txt") sumsUrl = url;
    }
    const bool newer = compareVersions(latest, current) > 0;
    result = {{"ok", true}, {"current", current}, {"latest", latest}, {"upgradeAvailable", newer}, {"path", self.string()},
              {"url", release.value("html_url", std::string())}, {"notes", json::array()}};
    if (opt.check) return true;
    if (!newer && !opt.force) {
        result["upgraded"] = false;
        result["notes"].push_back(compareVersions(current, latest) > 0 ? "wavelength " + current + " is newer than the latest release (" + latest + ")"
                                                                  : "wavelength " + current + " is the latest release");
        return true;
    }
    if (current.find("-dev") != std::string::npos && !opt.force) {
        err = "this is a development build (" + current + ") from a source checkout: pull and rebuild it, or pass --force to replace it with release " + latest;
        return false;
    }
    if (assetUrl.empty()) { err = "release " + latest + " has no build for this computer (" + assetName() + ")"; return false; }
    if (sumsUrl.empty()) { err = "release " + latest + " has no SHA256SUMS.txt to check the download against"; return false; }

    // download, check, unpack, and run the new binary once before it replaces this one
    const fs::path archive = work / assetName();
    if (!downloadFile(sumsUrl, work / "SHA256SUMS.txt", 0, err)) return false;
    if (!downloadFile(assetUrl, archive, (int)std::max<uint64_t>(1, assetBytes >> 20), err)) return false;
    std::string expected;
    std::istringstream sums(readAll(work / "SHA256SUMS.txt"));
    for (std::string line; std::getline(sums, line);) {
        std::istringstream l(line);
        std::string hash, name;
        l >> hash >> name;
        if (!name.empty() && name[0] == '*') name.erase(0, 1);
        if (name == assetName()) expected = hash;
    }
    if (expected.empty()) { err = "SHA256SUMS.txt has no line for " + assetName(); return false; }
    if (sha256File(archive.string()) != expected) { err = assetName() + " doesn't match its SHA-256 in the release: not installing it"; return false; }
    const fs::path unpacked = work / "x";
    if (!extractArchive(archive, unpacked, err)) return false;
    const fs::path folder = unpacked / assetName().substr(0, assetName().find('.'));
    const fs::path fresh = folder / kExe;
    if (!fs::is_regular_file(fresh, ec)) { err = "the archive has no " + std::string(kExe) + " where the release puts it"; return false; }
    fs::permissions(fresh, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    const std::string reported = versionOf(fresh);
    if (reported != latest) { err = "the downloaded build doesn't run here (it reported '" + reported + "', expected " + latest + ")"; return false; }

    // into place: next to this one, then renamed over it (a running program keeps its old file)
    const fs::path dir = self.parent_path(), staged = dir / (std::string(kExe) + ".new");
    {   // the bytes only (copying permission bits fails on some mounted folders), then made executable
        std::ifstream in(fresh, std::ios::binary);
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
        if (!in || !out) {
            out.close();
            fs::remove(staged, ec);
            err = "cannot write to " + dir.string() + ": upgrade with the permissions of that folder's owner";
            return false;
        }
    }
    fs::permissions(staged, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
#ifndef _WIN32
    if ((fs::status(staged, ec).permissions() & fs::perms::owner_exec) == fs::perms::none) {
        fs::remove(staged, ec);
        err = "cannot make " + staged.string() + " executable";
        return false;
    }
#endif
#ifdef _WIN32
    const fs::path old = self.string() + ".old";
    fs::rename(self, old, ec);   // Windows lets a running program be renamed, not replaced
    if (ec) { fs::remove(staged, ec); err = "cannot move " + self.string() + " aside: " + ec.message(); return false; }
    fs::rename(staged, self, ec);
    if (ec) { std::error_code e2; fs::rename(old, self, e2); err = "cannot put the new build in place: " + ec.message(); return false; }
#else
    fs::rename(staged, self, ec);
    if (ec) { std::error_code e2; fs::remove(staged, e2); err = "cannot put the new build in place: " + ec.message(); return false; }
#endif
    // a release folder (the binary beside AGENTS.md) gets the release's docs and examples too
    if (fs::exists(dir / "AGENTS.md", ec)) {
        for (auto &e : fs::directory_iterator(folder, ec)) {
            if (e.path().filename() == kExe) continue;
            fs::copy(e.path(), dir / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        }
        result["notes"].push_back("updated the docs and examples beside it");
    }
    result["upgraded"] = true;
    result["notes"].push_back("wavelength " + current + " -> " + latest);
    return true;
}

} // namespace wl
