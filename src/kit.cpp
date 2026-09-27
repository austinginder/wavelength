#include "kit.hpp"

#include "catalog.hpp"
#include "platform.hpp"
#include "sampler.hpp"
#include "sf2.hpp"
#include "zip.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace wl {

namespace {

using json = nlohmann::json;

// one download: plugin bundles (every .clap/.vst3 inside) or data (a folder of the archive, kept whole)
struct KitFile {
    std::string platform;   // "linux-x86_64", "windows-x86_64", "macos", or "any"
    std::string url;
    int mb;                 // download size, for the listing
    bool plugins;           // true: take the plugin bundles; false: data
    std::string pick;       // data: the folder inside the archive to keep ("" = all of it)
    std::string into;       // data: its name in the entry's folder
    std::string version;    // plugins: when this platform gets another release than the entry's ("" = the entry's)
};

struct KitEntry {
    const char *name, *title, *version, *license, *homepage, *about;
    std::vector<const char *> provides;   // plugin names it installs (the first one decides "installed on this computer")
    std::vector<KitFile> files;
};

const std::string kSurge = "https://github.com/surge-synthesizer/releases-xt/releases/download/1.3.4/";
const std::string kObxf = "https://github.com/surge-synthesizer/OB-Xf/releases/download/v1.0.3/";
const std::string kDexed = "https://github.com/asb2m10/dexed/releases/download/v1.0.1/";

const std::vector<KitEntry> &entries() {
    static const std::vector<KitEntry> list = [] {
        auto u = [](const std::string &base, const char *file) { return base + file; };
        std::vector<KitEntry> l;
        l.push_back({"surge-xt", "Surge XT", "1.3.4", "GPL-3.0-or-later", "https://surge-synthesizer.github.io",
                     "hybrid synthesizer with about 3,300 factory and third-party patches (basses, leads, pads, plucks, FX), wavetables, and Surge XT Effects",
                     {"Surge XT", "Surge XT Effects"},
                     {{"linux-x86_64", u(kSurge, "surge-xt-linux-1.3.4-pluginsonly.tar.gz"), 92, true, "", "", ""},
                      {"windows-x86_64", u(kSurge, "surge-xt-win64-1.3.4-pluginsonly.zip"), 46, true, "", "", ""},
                      {"macos", u(kSurge, "surge-xt-macos-1.3.4-pluginsonly.zip"), 178, true, "", "", ""},
                      {"any", u(kSurge, "surge-xt-portable-content-1.3.4.tar.gz"), 241, false, "Surge Synth Team/SurgeXTData", "SurgeXTData", ""}}});
        l.push_back({"ob-xf", "OB-Xf", "1.0.3", "GPL-3.0-or-later", "https://github.com/surge-synthesizer/OB-Xf",
                     "Oberheim OB-X style polysynth with 488 factory patches (brass, strings, pads, leads)",
                     {"OB-Xf"},
                     {{"linux-x86_64", u(kObxf, "ob-xf-Linux-v1.0.3.zip"), 20, true, "", "", ""},
                      {"windows-x86_64", u(kObxf, "ob-xf-Windows-v1.0.3.zip"), 8, true, "", "", ""},
                      {"macos", u(kObxf, "ob-xf-macOS-v1.0.3.dmg"), 57, true, "", "", ""},
                      {"any", u(kObxf, "ob-xf-assets-v1.0.3.zip"), 22, false, "Surge Synth Team/OB-Xf", "OB-Xf", ""}}});
        l.push_back({"dexed", "Dexed", "1.0.1", "GPL-3.0-or-later", "https://asb2m10.github.io/dexed/",
                     "DX7 FM synthesizer with the classic cartridges built in (electric pianos, basses, bells, brass)",
                     {"Dexed"},
                     // 1.0.1's Linux build needs glibc 2.38; 0.9.8 runs where Wavelength does (glibc 2.35, Ubuntu 22.04)
                     {{"linux-x86_64", "https://github.com/asb2m10/dexed/releases/download/v0.9.8/dexed-0.9.8-lnx.zip", 8, true, "", "", "0.9.8"},
                      {"windows-x86_64", u(kDexed, "Dexed-1.0.1-win.zip"), 9, true, "", "", ""},
                      {"macos", u(kDexed, "Dexed-1.0.1-macOS.zip"), 16, true, "", "", ""}}});
        l.push_back({"soundfont", "MuseScore General", "0.2", "MIT", "https://musescore.org/en/handbook/3/soundfonts-and-sfz-files",
                     "General MIDI SoundFont: 128 instruments and the drum kits for builtin:sampler, and the fallback for MIDI and MusicXML imports",
                     {}, {{"any", "", 40, false, "", "", ""}}});
        return l;
    }();
    return list;
}

std::string platformKey() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows-" + platform::hostArch();
#else
    return "linux-" + platform::hostArch();
#endif
}

// the release this platform gets
std::string versionHere(const KitEntry &e) {
    for (auto &f : e.files) if (f.plugins && f.platform == platformKey() && !f.version.empty()) return f.version;
    return e.version;
}

bool availableHere(const KitEntry &e) {
    if (std::string(e.name) == "soundfont") return true;
    for (auto &f : e.files) if (f.plugins && f.platform == platformKey()) return true;
    return false;
}

bool under(const fs::path &p, const fs::path &dir) {
    const std::string a = p.lexically_normal().string(), b = dir.lexically_normal().string();
    return a.size() > b.size() && a.compare(0, b.size(), b) == 0;
}

// the entry's installed.json ({} when it isn't installed)
json readMarker(const std::string &name) {
    std::ifstream in(kitDir() / name / "installed.json");
    json j = json::object();
    try { if (in) in >> j; } catch (...) { j = json::object(); }
    return j.is_object() ? j : json::object();
}

// where one of the entry's plugins is installed outside the kit ("" = nowhere)
std::string installedElsewhere(const KitEntry &e) {
    for (const char *p : e.provides) {
        PluginInfo info;
        std::string err;
        if (resolvePlugin(p, info, err, true) && !under(info.bundlePath, kitDir())) return info.bundlePath;
    }
    return "";
}

bool run(const std::vector<std::string> &args) {
    platform::Process p;
    if (!platform::spawn(args, p, false, false)) return false;
    std::string crash;
    while (!platform::finished(p, crash)) platform::pumpEvents(50);
    return crash.empty();
}

bool download(const std::string &url, const fs::path &to, int mb, std::string &err) {
    std::error_code ec;
    fs::remove(to, ec);
    if (url.rfind("file://", 0) == 0) {   // a local file (tests, a mirror on disk)
        fs::copy_file(fs::u8path(url.substr(7)), to, ec);
        if (ec) { err = "cannot read " + url + ": " + ec.message(); return false; }
        return true;
    }
    const std::string curl = platform::findProgram("curl"), wget = curl.empty() ? platform::findProgram("wget") : "";
    if (curl.empty() && wget.empty()) { err = "downloads need curl (or wget) on the PATH"; return false; }
    if (mb > 0) std::fprintf(stderr, "downloading %s (%d MB)\n", url.c_str(), mb);
    if (!curl.empty()) run({curl, "-fL", "-sS", "--retry", "2", "-o", to.string(), url});
    else run({wget, "-q", "--tries=3", "-O", to.string(), url});
    if (!fs::exists(to, ec) || fs::file_size(to, ec) < (mb > 0 ? 100000u : 16u)) { err = "download failed: " + url; return false; }
    return true;
}

// a zip into `dir`, with our own reader (containers often have no unzip); names that climb out are refused
bool unzipTo(const fs::path &zip, const fs::path &dir, std::string &err) {
    Zip z;
    if (!z.open(zip.string(), err)) return false;
    for (const auto &name : z.names()) {
        if (name.empty() || name.find("..") != std::string::npos || name[0] == '/' || name.find(':') != std::string::npos) continue;
        const fs::path to = dir / fs::u8path(name);
        std::error_code ec;
        if (name.back() == '/') { fs::create_directories(to, ec); continue; }
        fs::create_directories(to.parent_path(), ec);
        std::vector<uint8_t> data;
        if (!z.read(name, data, err)) return false;
        std::ofstream out(to, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
        if (!out) { err = "cannot write " + to.string(); return false; }
        out.close();
        fs::permissions(to, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, ec);
    }
    return true;
}

bool extract(const fs::path &archive, const fs::path &dir, std::string &err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string name = archive.filename().string();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".zip") return unzipTo(archive, dir, err);
    if (name.size() > 7 && name.substr(name.size() - 7) == ".tar.gz") {
        const std::string tar = platform::findProgram("tar");
        if (tar.empty()) { err = "extracting " + name + " needs tar on the PATH"; return false; }
        if (!run({tar, "-xzf", archive.string(), "-C", dir.string()})) { err = "tar could not extract " + name; return false; }
        return true;
    }
#ifdef __APPLE__
    if (name.size() > 4 && name.substr(name.size() - 4) == ".dmg") {   // mounted read-only, the bundles copied out
        const fs::path mnt = dir.parent_path() / (dir.filename().string() + "-mnt");
        fs::create_directories(mnt, ec);
        if (!run({"/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly", "-noverify", "-quiet", "-mountpoint", mnt.string(), archive.string()})) {
            err = "hdiutil could not mount " + name;
            return false;
        }
        fs::copy(mnt, dir, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        run({"/usr/bin/hdiutil", "detach", "-quiet", mnt.string()});
        fs::remove_all(mnt, ec);
        if (ec) { err = "cannot copy from " + name + ": " + ec.message(); return false; }
        return true;
    }
#endif
    err = "cannot extract " + name;
    return false;
}

// every plugin bundle under `dir` (not inside another bundle)
std::vector<fs::path> bundlesIn(const fs::path &dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        const std::string ext = it->path().extension().string();
        if (it->path().string().find("__MACOSX") != std::string::npos) continue;
        if (ext == ".clap" || ext == ".vst3") { out.push_back(it->path()); if (it->is_directory(ec)) it.disable_recursion_pending(); }
    }
    return out;
}

bool moveOrCopy(const fs::path &from, const fs::path &to, std::string &err) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::remove_all(to, ec);
    fs::rename(from, to, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) { err = "cannot move " + from.string() + " to " + to.string() + ": " + ec.message(); return false; }
    return true;
}

// Linux: what the loader says about a plugin's binary ("" when it loads), and every system library it
// misses (from ldd: the loader names only the first)
std::string loadProblem(const fs::path &bundle, std::set<std::string> &missing) {
#if defined(__APPLE__) || defined(_WIN32)
    (void)bundle; (void)missing;
    return "";
#else
    std::error_code ec;
    fs::path bin = bundle;
    if (fs::is_directory(bundle, ec)) {   // a VST3 bundle: Contents/<arch>-linux/<name>.so
        bin = bundle / "Contents" / (platform::hostArch() == "arm64" ? "aarch64-linux" : "x86_64-linux") / (bundle.stem().string() + ".so");
    }
    if (!fs::exists(bin, ec)) return "";
    const std::string why = platform::libraryLoadError(bin.string());
    if (why.empty()) return "";
    const std::string ldd = platform::findProgram("ldd");
    platform::Process p;
    std::string text, crash;
    if (!ldd.empty() && platform::spawn({ldd, bin.string()}, p, true, true)) {
        platform::readOutput(p, text, 30);
        while (!platform::finished(p, crash)) platform::pumpEvents(10);
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);)
            if (line.find("=> not found") != std::string::npos) {
                const size_t a = line.find_first_not_of(" \t"), b = line.find(' ', a);
                if (a != std::string::npos) missing.insert(line.substr(a, b - a));
            }
    }
    const size_t colon = why.find(": cannot open");
    if (missing.empty() && colon != std::string::npos) missing.insert(why.substr(0, colon));
    return why;
#endif
}

// the Debian/Ubuntu package of a shared library (the usual audio-plugin set; others by name)
std::string debianPackage(const std::string &lib) {
    static const std::pair<const char *, const char *> known[] = {
        {"libasound.so.2", "libasound2"}, {"libfreetype.so.6", "libfreetype6"}, {"libfontconfig.so.1", "libfontconfig1"},
        {"libX11.so.6", "libx11-6"}, {"libXext.so.6", "libxext6"}, {"libXcursor.so.1", "libxcursor1"}, {"libXrandr.so.2", "libxrandr2"},
        {"libXinerama.so.1", "libxinerama1"}, {"libXrender.so.1", "libxrender1"}, {"libGL.so.1", "libgl1"}, {"libcurl.so.4", "libcurl4"},
        {"libxcb.so.1", "libxcb1"}, {"libxcb-cursor.so.0", "libxcb-cursor0"}, {"libxcb-xkb.so.1", "libxcb-xkb1"},
        {"libxkbcommon.so.0", "libxkbcommon0"}, {"libxkbcommon-x11.so.0", "libxkbcommon-x11-0"}, {"libcairo.so.2", "libcairo2"},
        {"libgtk-3.so.0", "libgtk-3-0"}, {"libjack.so.0", "libjack-jackd2-0"}, {"libatomic.so.1", "libatomic1"}, {"libgomp.so.1", "libgomp1"},
        {"libpng16.so.16", "libpng16-16"}, {"libz.so.1", "zlib1g"}, {"libuuid.so.1", "libuuid1"}, {"libglib-2.0.so.0", "libglib2.0-0"}};
    for (auto &[l, pkg] : known) if (lib == l) return pkg;
    return "";
}

bool installEntry(const KitEntry &e, bool force, json &done, std::string &err) {
    const fs::path root = kitDir(), stage = root / (".stage-" + std::string(e.name)), dl = root / (".download-" + std::string(e.name));
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::remove_all(dl, ec);
    fs::create_directories(stage, ec);
    fs::create_directories(dl, ec);
    struct Cleanup { fs::path a, b; ~Cleanup() { std::error_code c; fs::remove_all(a, c); fs::remove_all(b, c); } } cleanup{stage, dl};
    int files = 0;
    for (const auto &f : e.files) {
        if (f.platform != "any" && f.platform != platformKey()) continue;
        const std::string url = f.url, file = url.substr(url.rfind('/') + 1);
        if (!download(url, dl / file, f.mb, err)) return false;
        const fs::path x = dl / ("x-" + std::to_string(files++));
        if (!extract(dl / file, x, err)) return false;
        if (f.plugins) {
            const auto found = bundlesIn(x);
            if (found.empty()) { err = file + " holds no .clap or .vst3 plugin"; return false; }
            for (const auto &b : found)
                if (!moveOrCopy(b, stage / "plugins" / b.filename(), err)) return false;
        } else {
            const fs::path from = f.pick.empty() ? x : x / fs::u8path(f.pick);
            if (!fs::exists(from, ec)) { err = file + " has no " + f.pick; return false; }
            if (!moveOrCopy(from, stage / fs::u8path(f.into), err)) return false;
        }
        fs::remove(dl / file, ec);
    }
    std::ofstream(stage / "installed.json") << json{{"name", e.name}, {"version", versionHere(e)}, {"installed", (long long)std::time(nullptr)}}.dump(2) << "\n";
    const fs::path to = root / e.name;
    if (!moveOrCopy(stage, to, err)) return false;
    done.push_back({{"name", e.name}, {"version", versionHere(e)}, {"folder", to.string()}});
    return true;
}

} // namespace

bool downloadFile(const std::string &url, const fs::path &to, int mb, std::string &err) { return download(url, to, mb, err); }
bool extractArchive(const fs::path &archive, const fs::path &dir, std::string &err) { return extract(archive, dir, err); }

fs::path kitDir() { return platform::dataDir() / "kit"; }

std::vector<std::string> kitPluginFolders() {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto &d : fs::directory_iterator(kitDir(), ec))
        if (d.is_directory(ec) && d.path().filename().string()[0] != '.' && fs::is_directory(d.path() / "plugins", ec))
            out.push_back((d.path() / "plugins").string());
    std::sort(out.begin(), out.end());
    return out;
}

json kitList() {
    json list = json::array();
    std::error_code ec;
    for (const auto &e : entries()) {
        int mb = 0;
        for (auto &f : e.files) if (f.platform == "any" || f.platform == platformKey()) mb += f.mb;
        json j = {{"name", e.name}, {"title", e.title}, {"version", versionHere(e)}, {"license", e.license}, {"homepage", e.homepage},
                  {"about", e.about}, {"downloadMB", mb}, {"plugins", e.provides}};
        const json marker = readMarker(e.name);
        if (std::string(e.name) == "soundfont") {
            const fs::path sf = fs::path(soundFontDir()) / "MuseScore_General.sf3";
            j["status"] = fs::exists(sf, ec) ? "installed" : "available";
            if (fs::exists(sf, ec)) j["where"] = sf.string();
        } else if (marker.contains("version")) {
            j["status"] = marker.value("version", "") == versionHere(e) ? "installed" : "update available";
            j["where"] = (kitDir() / e.name).string();
        } else if (!availableHere(e)) j["status"] = "not available for " + platformKey();
        else {
            const std::string other = installedElsewhere(e);
            j["status"] = other.empty() ? "available" : "installed on this computer";
            if (!other.empty()) j["where"] = other;
        }
        list.push_back(j);
    }
    return list;
}

bool kitInstall(const std::vector<std::string> &names, bool force, json &result, std::string &err) {
    result = {{"installed", json::array()}, {"skipped", json::array()}, {"problems", json::array()}};
    for (const auto &n : names) {
        bool known = false;
        for (auto &e : entries()) known |= n == e.name;
        if (!known) {
            std::string all;
            for (auto &e : entries()) all += (all.empty() ? "" : ", ") + std::string(e.name);
            err = "no kit entry '" + n + "' (entries: " + all + ")";
            return false;
        }
    }
    std::error_code ec;
    fs::create_directories(kitDir(), ec);
    bool plugins = false;
    for (const auto &e : entries()) {
        if (!names.empty() && std::find(names.begin(), names.end(), e.name) == names.end()) continue;
        auto skip = [&](const std::string &why) { result["skipped"].push_back({{"name", e.name}, {"why", why}}); };
        if (std::string(e.name) == "soundfont") {
            std::string path;
            const bool had = fs::exists(fs::path(soundFontDir()) / "MuseScore_General.sf3", ec);
            if (had && !force) { skip("already installed"); continue; }
            if (!installSoundFont(force, path, err)) return false;
            result["installed"].push_back({{"name", e.name}, {"version", e.version}, {"folder", path}});
            continue;
        }
        if (!availableHere(e)) { skip("not available for " + platformKey()); continue; }
        if (!force && readMarker(e.name).value("version", "") == versionHere(e)) { skip("already in the kit"); continue; }
        const std::string other = force ? "" : installedElsewhere(e);
        if (!other.empty()) { skip("already installed on this computer (" + other + "); --force installs the kit's copy too"); continue; }
        std::fprintf(stderr, "installing %s %s (%s)\n", e.title, versionHere(e).c_str(), e.license);
        if (!installEntry(e, force, result["installed"], err)) { err = std::string(e.title) + ": " + err; return false; }
        plugins = true;
    }
    if (plugins) {   // the catalog picks the new plugins up; a plugin that won't load says which library it misses
        std::fprintf(stderr, "scanning plugins\n");
        std::vector<std::string> warnings;
        scanPlugins(true, warnings);
        std::set<std::string> missing;
        for (auto &inst : result["installed"]) {
            const fs::path folder = fs::path(inst["folder"].get<std::string>()) / "plugins";
            for (const auto &b : bundlesIn(folder)) {
                const std::string why = loadProblem(b, missing);
                if (!why.empty()) result["problems"].push_back({{"plugin", b.filename().string()}, {"error", why}});
            }
        }
        if (!missing.empty()) {   // one command that installs them all, then the plugins load (plugins --rescan)
            std::string pkgs, unknown;
            std::set<std::string> seen;
            for (auto &l : missing) {
                const std::string pkg = debianPackage(l);
                if (pkg.empty()) unknown += (unknown.empty() ? "" : ", ") + l;
                else if (seen.insert(pkg).second) pkgs += " " + pkg;
            }
            result["missingLibraries"] = missing;
            result["fix"] = (pkgs.empty() ? std::string() : "apt-get install -y" + pkgs + " (Debian/Ubuntu; other distributions have packages of the same libraries)") +
                            (unknown.empty() ? "" : std::string(pkgs.empty() ? "" : "; ") + "also install: " + unknown) +
                            ", then run `wavelength plugins --rescan`";
        }
    }
    return true;
}

bool kitRemove(const std::string &name, std::string &err) {
    const fs::path dir = kitDir() / name;
    std::error_code ec;
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name[0] == '.' || !fs::is_directory(dir, ec)) {
        err = "no kit entry '" + name + "' is installed";
        return false;
    }
    fs::remove_all(dir, ec);
    if (ec) { err = "cannot remove " + dir.string() + ": " + ec.message(); return false; }
    std::vector<std::string> warnings;
    scanPlugins(true, warnings);
    return true;
}

bool installSoundFont(bool force, std::string &path, std::string &err) {
    const fs::path dir = soundFontDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string base = "https://ftp.osuosl.org/pub/musescore/soundfont/MuseScore_General/";
    for (const std::string name : {"MuseScore_General_License.md", "MuseScore_General.sf3"}) {
        const fs::path to = dir / name, part = dir / (name + ".part");
        if (fs::exists(to, ec) && !force) continue;
        if (!download(base + name, part, name.size() > 4 && name.substr(name.size() - 4) == ".sf3" ? 40 : 0, err)) { fs::remove(part, ec); return false; }
        SoundFont check;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".sf3" && !check.open(part.string(), err)) {
            fs::remove(part, ec);
            err = "download of " + name + " failed: " + err;
            return false;
        }
        fs::rename(part, to, ec);
    }
    path = (dir / "MuseScore_General.sf3").string();
    return true;
}

} // namespace wl
