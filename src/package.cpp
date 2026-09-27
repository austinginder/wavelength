#include "package.hpp"

#include "catalog.hpp"
#include "history.hpp"
#include "job.hpp"
#include "platform.hpp"
#include "sha256.hpp"
#include "zip.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace wl::package {

namespace {

constexpr uint64_t kMaxUnpacked = 4ull << 30;   // 4 GiB in all
constexpr double kMaxRatio = 200;              // per entry over 1 MiB

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string ext(const std::string &p) { return lower(fs::path(p).extension().string()); }

// audio, images and other compressed data are stored as they are
bool storeAsIs(const std::string &p) {
    static const std::set<std::string> kinds = {".mp3", ".flac", ".ogg", ".opus", ".wav", ".aif", ".aiff", ".png", ".jpg", ".jpeg", ".webp", ".gif",
                                                ".zip", ".gz", ".sf3", ".mid"};
    return kinds.count(ext(p)) > 0;
}

std::string mediaTypeOf(const std::string &p) {
    static const std::map<std::string, std::string> types = {
        {".json", "application/json"}, {".md", "text/markdown"}, {".txt", "text/plain"}, {".py", "text/x-python"}, {".js", "text/javascript"},
        {".sh", "text/x-shellscript"}, {".mp3", "audio/mpeg"}, {".flac", "audio/flac"}, {".wav", "audio/wav"}, {".ogg", "audio/ogg"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".mid", "audio/midi"}, {".sfz", "text/plain"}};
    auto it = types.find(ext(p));
    return it == types.end() ? "application/octet-stream" : it->second;
}

std::vector<uint8_t> bytes(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// what the song needs from the computer that renders it: plugins and named libraries, per track
json requiresOf(const json &job) {
    json out = json::array();
    for (auto &t : job.value("tracks", json::array())) {
        if (!t.is_object()) continue;
        const std::string plugin = t.value("plugin", std::string()), id = t.contains("id") && t["id"].is_string() ? t["id"].get<std::string>() : t.value("name", std::string());
        const bool fallback = t.contains("fallback");
        if (plugin.rfind("builtin:", 0) != 0) {
            json p = {{"name", plugin}};
            PluginInfo info;
            std::string err;
            if (resolvePlugin(plugin, info, err, true)) p = {{"name", info.name}, {"format", info.format}, {"id", info.id}, {"version", info.version}};
            json r = {{"track", id}, {"plugin", p}, {"fallback", fallback}};
            if (t.contains("preset") && t["preset"].is_string()) r["preset"] = t["preset"];
            out.push_back(r);
        } else if (plugin == "builtin:sampler" && t.contains("sampler") && t["sampler"].is_object()) {
            for (const char *kind : {"multisample", "kit", "soundfont", "sfz"}) {
                const auto &s = t["sampler"];
                if (!s.contains(kind) || !s[kind].is_string()) continue;
                const std::string v = s[kind].get<std::string>();
                if (v.find('/') != std::string::npos || v.find('.') != std::string::npos) continue;   // a file in the song, not a library
                out.push_back({{"track", id}, {"library", {{"kind", kind}, {"name", v}}}, {"fallback", fallback}});
            }
        }
    }
    return out;
}

// section 5 checks on a package's entries; problems go into `problems`, `err` gets the first error
bool checkEntries(Zip &z, json &problems) {
    auto problem = [&](const std::string &sev, const std::string &path, const std::string &msg) {
        problems.push_back({{"severity", sev}, {"path", path}, {"message", msg}});
    };
    const auto &entries = z.entries();
    bool ok = true;
    if (entries.empty() || entries[0].name != "mimetype") problem("warning", "mimetype", "the first entry should be `mimetype`");
    else {
        std::vector<uint8_t> mt;
        std::string err;
        if (entries[0].method != 0) problem("warning", "mimetype", "`mimetype` should be stored uncompressed");
        if (z.read("mimetype", mt, err) && std::string(mt.begin(), mt.end()) != kSongMediaType)
            { problem("error", "mimetype", "`mimetype` must hold " + std::string(kSongMediaType)); ok = false; }
    }
    std::set<std::string> seen;
    uint64_t total = 0;
    for (const auto &e : entries) {
        std::string why;
        const std::string name = e.name.size() > 1 && e.name.back() == '/' ? e.name.substr(0, e.name.size() - 1) : e.name;
        if (!checkSongPath(name, why)) { problem("error", e.name, why); ok = false; continue; }
        if (e.flags & 1) { problem("error", e.name, "encrypted entries are not allowed"); ok = false; }
        if (((e.externalAttr >> 16) & 0170000) == 0120000) { problem("error", e.name, "symbolic links are not allowed"); ok = false; }
        if (!seen.insert(lower(name)).second) { problem("error", e.name, "two entries with the same name, compared case-insensitively"); ok = false; }
        total += e.size;
        if (e.size > (1u << 20) && e.compSize > 0 && (double)e.size / e.compSize > kMaxRatio) { problem("error", e.name, "compressed too far (a zip bomb?)"); ok = false; }
    }
    if (total > kMaxUnpacked) { problem("error", "", "the package unpacks to more than 4 GiB"); ok = false; }
    if (!seen.count("wavelength.json")) { problem("error", "wavelength.json", "no manifest"); ok = false; }
    return ok;
}

bool extractTo(Zip &z, const fs::path &dir, std::string &err) {
    std::error_code ec;
    for (const auto &e : z.entries()) {
        if (e.name.empty()) continue;
        if (e.name.back() == '/') { fs::create_directories(dir / fs::u8path(e.name), ec); continue; }
        if (e.name == "mimetype") continue;
        std::vector<uint8_t> data;
        if (!z.read(e.name, data, err)) return false;
        if (!writeText(dir / fs::u8path(e.name), std::string(data.begin(), data.end()), err)) return false;
    }
    return true;
}

void validateFolder(const fs::path &dir, json &problems) {
    auto problem = [&](const std::string &sev, const std::string &path, const std::string &msg) {
        problems.push_back({{"severity", sev}, {"path", path}, {"message", msg}});
    };
    Song song;
    std::string err;
    if (!openSong(dir.string(), song, err)) { problem("error", "", err); return; }
    if (!song.hasManifest()) { problem("error", "wavelength.json", "no manifest (wavelength upgrade makes one)"); return; }
    const json &m = song.manifest;
    for (const char *k : {"formatVersion", "id", "title", "slug", "job"})
        if (!m.contains(k) || !m[k].is_string() || m[k].get<std::string>().empty()) problem("error", "wavelength.json", std::string("missing \"") + k + "\"");
    std::string why;
    if (!checkSongPath(song.jobFile(), why)) problem("error", "wavelength.json", "job: " + why);
    std::error_code ec;
    for (auto &f : m.value("files", json::array())) {
        const std::string p = f.value("path", std::string());
        if (!checkSongPath(p, why)) { problem("error", p, why); continue; }
        const fs::path full = song.dir / fs::u8path(p);
        if (fs::is_symlink(full, ec)) { problem("error", p, "a symbolic link"); continue; }
        if (!fs::exists(full, ec)) { if (f.value("role", std::string()) != "render") problem("warning", p, "listed in the manifest but missing"); continue; }
        if (f.contains("sha256") && sha256File(full.string()) != f["sha256"].get<std::string>()) problem("error", p, "does not match its sha256");
        if (f.contains("size") && (uint64_t)fs::file_size(full, ec) != f["size"].get<uint64_t>()) problem("error", p, "does not match its size");
    }
    json job;
    try { job = json::parse(readText(song.jobPath())); } catch (const std::exception &e) { problem("error", song.jobFile(), std::string("not valid JSON: ") + e.what()); return; }
    Job parsed;
    if (!parseJob(job, song.dir.string(), parsed, err, false)) problem("error", song.jobFile(), err);
    for (auto &[where, path] : jobFileRefs(job)) {
        if (!checkSongPath(path, why)) { problem("error", song.jobFile(), where + ": " + why + " (files the job uses belong in the song, e.g. media/; name outside sounds instead)"); continue; }
        if (!fs::exists(song.dir / fs::u8path(path), ec)) problem("error", song.jobFile(), where + ": " + path + " is missing");
    }
    std::vector<json> entries;
    std::string warning;
    if (!history::read(song, entries, err, &warning)) problem("error", "history/log.jsonl", err);
    if (!warning.empty()) problem("warning", "history/log.jsonl", warning);
    std::set<std::string> checked;
    for (auto &e : entries) {
        const json files = e.value("files", json::object());
        for (auto &[path, ref] : files.items()) {
            const std::string h = ref.get<std::string>();
            if (!checked.insert(h).second) continue;
            std::string content, oerr;
            if (!history::readObject(song, h, content, oerr)) problem("error", "history/objects", "revision " + e["rev"].dump() + " " + path + ": " + oerr);
        }
    }
    if (fs::exists(song.dir / "review.json", ec)) {
        try { const json r = json::parse(readText(song.dir / "review.json")); if (!r.contains("comments")) problem("warning", "review.json", "no comments list"); }
        catch (...) { problem("error", "review.json", "not valid JSON"); }
    }
}

} // namespace

bool isPackage(const std::string &path) { return ext(path) == ".wavelength"; }

bool pack(Song &song, std::string out, const PackOptions &opt, json &result, std::string &err) {
    if (!song.hasManifest()) { err = song.dir.filename().string() + " has no wavelength.json (wavelength upgrade makes one)"; return false; }
    json job;
    try { job = json::parse(readText(song.jobPath())); } catch (const std::exception &e) { err = song.jobFile() + " is not valid JSON: " + e.what(); return false; }
    // every file the job uses must travel with it
    std::error_code ec;
    std::string why, outside;
    for (auto &[where, path] : jobFileRefs(job))
        if (!checkSongPath(path, why) || !fs::exists(song.dir / fs::u8path(path), ec)) outside += "\n  " + where + ": " + path;
    if (!outside.empty()) {
        err = "the job uses files outside the song (move them into media/, or name the preset or library instead; `wavelength upgrade` "
              "fixes most):" + outside;
        return false;
    }
    // the manifest, brought up to date (`updated` only moves when something in it changed, so packing an
    // unchanged song again gives the same package)
    json &m = song.manifest;
    const json before = m;
    m["generator"] = {{"name", "wavelength"}, {"version", WAVELENGTH_VERSION}};
    m["requires"] = requiresOf(job);
    json files = json::array();
    for (auto f : m.value("files", json::array())) {
        const std::string p = f.value("path", std::string()), role = f.value("role", std::string());
        if (!checkSongPath(p, why)) { err = "wavelength.json: " + why; return false; }
        const fs::path full = song.dir / fs::u8path(p);
        if (fs::is_regular_file(full, ec) && (role == "media" || role == "render")) {
            f["sha256"] = sha256File(full.string());
            f["size"] = (uint64_t)fs::file_size(full, ec);
        }
        if (!f.contains("mediaType")) f["mediaType"] = mediaTypeOf(p);
        files.push_back(f);
    }
    m["files"] = files;
    json a = before, b = m;
    a.erase("updated"); b.erase("updated");
    if (a != b) {
        m["updated"] = nowRfc3339();
        if (!writeManifest(song, err)) return false;
    }

    ZipWriter z;
    json listed = json::array();
    auto addFile = [&](const std::string &rel) {
        const fs::path full = song.dir / fs::u8path(rel);
        if (!fs::is_regular_file(full, ec) || fs::is_symlink(full, ec)) return;
        z.add(rel, bytes(full), storeAsIs(rel));
        listed.push_back(rel);
    };
    z.add("mimetype", std::string(kSongMediaType), true);
    addFile("wavelength.json");
    addFile(song.jobFile());
    for (auto &f : m["files"]) {
        const std::string role = f.value("role", std::string());
        if (role == "render" && !opt.render) continue;
        addFile(f.value("path", std::string()));
    }
    if (opt.review) addFile("review.json");
    if (opt.history && fs::exists(song.dir / "history" / "log.jsonl", ec)) {
        std::vector<json> entries;
        if (!history::read(song, entries, err)) return false;
        addFile("history/log.jsonl");
        std::set<std::string> objs;
        for (auto &e : entries) {
            const json fs2 = e.value("files", json::object());
            for (auto &[p, ref] : fs2.items()) objs.insert(ref.get<std::string>().substr(7));
        }
        for (auto &h : objs) addFile("history/objects/" + h.substr(0, 2) + "/" + h);
    }
    if (out.empty()) out = m.value("slug", song.dir.filename().string()) + ".wavelength";
    if (!z.write(out, err)) return false;
    result = {{"ok", true}, {"file", out}, {"bytes", (uint64_t)fs::file_size(out, ec)}, {"entries", listed.size() + 1}, {"requires", m["requires"]}};
    return true;
}

bool unpack(const std::string &file, std::string outDir, bool force, json &result, std::string &err) {
    Zip z;
    if (!z.open(file, err)) return false;
    json problems = json::array();
    if (!checkEntries(z, problems)) {
        for (auto &p : problems) if (p["severity"] == "error") { err = file + ": " + p.value("path", std::string()) + ": " + p.value("message", std::string()); break; }
        return false;
    }
    json manifest;
    {
        std::vector<uint8_t> mj;
        if (!z.read("wavelength.json", mj, err)) return false;
        try { manifest = json::parse(std::string(mj.begin(), mj.end())); } catch (...) { err = "the package's wavelength.json is not valid JSON"; return false; }
    }
    const std::string minv = manifest.value("minReaderVersion", std::string("1.0"));
    if (!minv.empty() && minv[0] != '1') { err = "the song needs a reader for format " + minv + "; this Wavelength reads format 1"; return false; }
    std::error_code ec;
    if (outDir.empty()) {
        std::string slug = manifest.value("slug", fs::path(file).stem().string()), why;
        if (!checkSongPath(slug, why) || slug.find('/') != std::string::npos) slug = fs::path(file).stem().string();
        outDir = (fs::path(file).parent_path() / slug).string();
    }
    if (fs::exists(outDir, ec) && !fs::is_empty(outDir, ec) && !force) { err = outDir + " exists and is not empty (--force to unpack over it)"; return false; }
    fs::create_directories(outDir, ec);
    if (!extractTo(z, outDir, err)) return false;
    result = {{"ok", true}, {"folder", outDir}, {"title", manifest.value("title", std::string())}, {"warnings", problems}};
    return true;
}

json validate(const std::string &target) {
    json problems = json::array();
    std::error_code ec;
    if (fs::is_regular_file(target, ec) && ext(target) != ".json") {   // a package (a folder, job.json or wavelength.json otherwise)
        Zip z;
        std::string err;
        if (!z.open(target, err)) problems.push_back({{"severity", "error"}, {"path", target}, {"message", err}});
        else if (checkEntries(z, problems)) {
            const fs::path tmp = fs::temp_directory_path(ec) / ("wavelength-validate-" + std::to_string(platform::processId()));
            fs::remove_all(tmp, ec);
            if (!extractTo(z, tmp, err)) problems.push_back({{"severity", "error"}, {"path", target}, {"message", err}});
            else validateFolder(tmp, problems);
            fs::remove_all(tmp, ec);
        }
    } else validateFolder(target, problems);
    bool ok = true;
    for (auto &p : problems) ok &= p["severity"] != "error";
    return {{"ok", ok}, {"problems", problems}};
}

std::string cached(const std::string &file, std::string &err) {
    const std::string h = sha256File(file);
    if (h.empty()) { err = "cannot read " + file; return ""; }
    const fs::path dir = platform::cacheDir() / "packages" / h.substr(0, 16);
    std::error_code ec;
    if (fs::exists(dir / "wavelength.json", ec)) return dir.string();
    fs::remove_all(dir, ec);
    json r;
    if (!unpack(file, dir.string(), true, r, err)) return "";
    return dir.string();
}

} // namespace wl::package
