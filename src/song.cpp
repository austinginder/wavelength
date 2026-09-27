#include "song.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace wl {

namespace {

using json = nlohmann::json;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// the manifest's keys in the order people read them (others follow, sorted)
const char *kManifestOrder[] = {"format", "formatVersion", "minReaderVersion", "generator", "id", "derivedFrom", "title", "slug",
                                "created", "updated", "authors", "prompt", "summary", "description", "license", "tags", "job",
                                "files", "render", "requires", "extensions", "extensionsRequired", "metadata"};

std::string titleFromSlug(const std::string &slug) {
    std::string t;
    bool up = true;
    for (char c : slug) {
        if (c == '-' || c == '_') { t += ' '; up = true; continue; }
        t += up ? (char)std::toupper((unsigned char)c) : c;
        up = false;
    }
    return t;
}

std::string slugOf(const std::string &name) {
    std::string s;
    for (char c : name) {
        const unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) s += (char)std::tolower(u);
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "song" : s;
}

} // namespace

std::string Song::title() const {
    const std::string t = manifest.value("title", std::string());
    return t.empty() ? titleFromSlug(dir.filename().string()) : t;
}

bool openSong(const std::string &arg, Song &song, std::string &err) {
    std::error_code ec;
    fs::path p = fs::absolute(arg.empty() ? "." : arg, ec).lexically_normal();
    if (!p.empty() && p.filename().empty()) p = p.parent_path();   // a trailing slash
    if (fs::is_regular_file(p, ec)) p = p.parent_path();           // job.json or wavelength.json
    if (!fs::is_directory(p, ec)) { err = arg + " is not a song folder"; return false; }
    song = Song{};
    song.dir = p;
    const fs::path m = p / "wavelength.json";
    if (fs::exists(m, ec)) {
        try { song.manifest = json::parse(readText(m)); }
        catch (const std::exception &e) { err = m.string() + " is not valid JSON: " + e.what(); return false; }
        if (!song.manifest.is_object() || song.manifest.value("format", std::string()) != kSongFormat) {
            err = m.string() + " is not a Wavelength song manifest (\"format\" must be \"" + kSongFormat + "\")";
            return false;
        }
        const std::string minv = song.manifest.value("minReaderVersion", std::string("1.0"));
        if (!minv.empty() && minv[0] != '1') {
            err = p.filename().string() + " needs a reader for song format " + minv + "; this Wavelength reads format 1";
            return false;
        }
        std::string why;
        const std::string job = song.jobFile();
        if (!checkSongPath(job, why) || job.find('/') != std::string::npos) {
            err = m.string() + ": \"job\" must name a file at the top of the song folder (" + (why.empty() ? job : why) + ")";
            return false;
        }
    }
    if (!fs::exists(song.jobPath(), ec)) { err = "no job in " + p.string() + " (" + song.jobFile() + ")"; return false; }
    return true;
}

bool songOfJob(const std::string &jobPath, Song &song) {
    std::error_code ec;
    const fs::path dir = fs::absolute(jobPath, ec).parent_path();
    if (!fs::exists(dir / "wavelength.json", ec)) return false;
    std::string err;
    if (!openSong(dir.string(), song, err) || !song.hasManifest()) return false;
    return fs::equivalent(song.jobPath(), fs::absolute(jobPath), ec);
}

bool writeManifest(const Song &song, std::string &err) {
    nlohmann::ordered_json o;
    std::set<std::string> done;
    for (const char *k : kManifestOrder)
        if (song.manifest.contains(k)) { o[k] = nlohmann::ordered_json::parse(song.manifest[k].dump()); done.insert(k); }
    for (auto &[k, v] : song.manifest.items())
        if (!done.count(k)) o[k] = nlohmann::ordered_json::parse(v.dump());
    return writeText(song.dir / "wavelength.json", o.dump(2) + "\n", err);
}

json newManifest(const fs::path &dir) {
    json m = {{"format", kSongFormat}, {"formatVersion", kSongFormatVersion}, {"id", newUuid()},
              {"title", titleFromSlug(dir.filename().string())}, {"slug", slugOf(dir.filename().string())},
              {"created", nowRfc3339()}, {"job", "job.json"}, {"files", json::array()}};
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> files;   // path, role
    static const std::set<std::string> notSources = {"job.json", "wavelength.json", "review.json", "site.json", "report.json", "out.json"};
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string name = e.path().filename().string(), ext = lower(e.path().extension().string());
        if (name.empty() || name[0] == '.' || name.find(".bak") != std::string::npos) continue;
        if (ext == ".py" || ext == ".sh" || ext == ".js" || (ext == ".json" && !notSources.count(name))) files.push_back({name, "source"});
        else if (ext == ".md" || ext == ".txt") files.push_back({name, "notes"});
    }
    for (auto it = fs::recursive_directory_iterator(dir / "media", fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().filename().string()[0] != '.')
            files.push_back({fs::relative(it->path(), dir, ec).generic_string(), "media"});
    }
    std::sort(files.begin(), files.end());
    for (auto &[p, role] : files) m["files"].push_back({{"path", p}, {"role", role}});
    return m;
}

bool checkSongPath(const std::string &rel, std::string &err) {
    if (rel.empty()) { err = "an empty path"; return false; }
    if (rel[0] == '/' || rel.find('\\') != std::string::npos || (rel.size() > 1 && rel[1] == ':')) { err = "'" + rel + "' is not a relative path"; return false; }
    std::stringstream ss(rel);
    for (std::string seg; std::getline(ss, seg, '/');) {
        if (seg.empty() || seg == ".") { err = "'" + rel + "' has an empty or '.' segment"; return false; }
        if (seg == "..") { err = "'" + rel + "' leaves the song folder (..)"; return false; }
        std::string bare = lower(seg);
        while (!bare.empty() && (bare.back() == ' ' || bare.back() == '.')) bare.pop_back();   // Windows drops these: ".git." is .git
        if (bare == ".git" || lower(seg) == "git~1") { err = "'" + rel + "': nothing in a song may be named .git"; return false; }
        for (unsigned char c : seg)
            if (c < 0x20 || c == 0x7f) { err = "'" + rel + "' has a control character"; return false; }
        if (seg.size() > 255) { err = "'" + rel + "': a name longer than 255 bytes"; return false; }
    }
    return true;
}

std::vector<std::string> trackedFiles(const Song &song) {
    std::set<std::string> out = {song.jobFile()};
    for (auto &f : song.manifest.value("files", json::array())) {
        const std::string role = f.value("role", std::string()), path = f.value("path", std::string());
        std::string err;
        std::error_code ec;
        if ((role == "source" || role == "notes" || role == "media") && checkSongPath(path, err) && fs::is_regular_file(song.dir / path, ec))
            out.insert(path);
    }
    return std::vector<std::string>(out.begin(), out.end());
}

std::vector<FileRef> fileRefs(json &job) {
    std::vector<FileRef> out;
    auto hasExt = [](const std::string &v, std::initializer_list<const char *> exts) {
        const std::string l = lower(v);
        for (const char *ext : exts)
            if (l.size() > std::strlen(ext) && l.compare(l.size() - std::strlen(ext), std::string::npos, ext) == 0) return true;
        return false;
    };
    auto add = [&](const std::string &where, json &slot, json &owner, bool isState = false) {
        if (!slot.is_string()) return;
        std::string path = slot.get<std::string>(), suffix;
        if (path.empty() || path.rfind("lib:", 0) == 0) return;   // a sample library's file, by name
        for (const char *pick : {".syx#", ".mtdrum#"})   // "<cartridge>.syx#3": one program of the file
            if (const size_t at = lower(path).rfind(pick); at != std::string::npos) {
                suffix = path.substr(at + std::strlen(pick) - 1);
                path.resize(at + std::strlen(pick) - 1);
            }
        out.push_back({where, path, suffix, &slot, &owner, isState});
    };
    auto state = [&](json &o, const std::string &where) {
        if (!o.contains("state")) return;
        json &s = o["state"];
        if (s.is_string()) add(where + " state", s, o, true);
        else if (s.is_object() && s.contains("file")) add(where + " state", s["file"], o, true);
    };
    auto chain = [&](json &o, const std::string &where) {
        if (!o.contains("fx") || !o["fx"].is_array()) return;
        for (size_t i = 0; i < o["fx"].size(); ++i)
            if (o["fx"][i].is_object()) state(o["fx"][i], where + " fx " + std::to_string(i + 1));
    };
    auto sound = [&](json &o, const std::string &where) {
        state(o, where);
        chain(o, where);
        if (!o.contains("sampler") || !o["sampler"].is_object()) return;
        json &sm = o["sampler"];
        for (const char *k : {"sample", "sfz", "soundfont", "multisample"})
            if (sm.contains(k) && sm[k].is_string()) {
                const std::string v = sm[k].get<std::string>();
                if (v.find('/') != std::string::npos || v.find('\\') != std::string::npos ||
                    hasExt(v, {".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".ogg", ".sfz", ".sf2", ".sf3", ".multisample"}))
                    add(where + " sampler." + k, sm[k], o);
            }
        const bool namedKit = sm.contains("kit") && sm["kit"].is_string();
        if (namedKit && sm["kit"].get<std::string>().find('/') != std::string::npos) add(where + " sampler.kit", sm["kit"], o);
        for (const char *k : {"map", "kit"})   // key -> file: names inside a kit folder, else files of the song
            if (sm.contains(k) && sm[k].is_object())
                for (auto &[key, v] : sm[k].items()) {
                    json &slot = v.is_object() && v.contains("file") ? v["file"] : v;
                    if (slot.is_string() && (!namedKit || slot.get<std::string>().find('/') != std::string::npos))
                        add(where + " sampler." + k + " " + key, slot, o);
                }
    };
    if (!job.is_object()) return out;
    if (job.contains("tracks") && job["tracks"].is_array())
        for (auto &t : job["tracks"]) {
            if (!t.is_object()) continue;
            const std::string where = "track '" + t.value("name", std::string("track")) + "'";
            sound(t, where);
            if (t.contains("clips") && t["clips"].is_array())
                for (auto &c : t["clips"])
                    if (c.is_object() && c.contains("file")) add(where + " clip", c["file"], c);
            if (t.contains("fallback")) {
                json &fb = t["fallback"];
                if (fb.is_object()) sound(fb, where + " fallback");
                else if (fb.is_array())
                    for (auto &f : fb) if (f.is_object()) sound(f, where + " fallback");
            }
        }
    if (job.contains("buses") && job["buses"].is_array())
        for (auto &b : job["buses"]) if (b.is_object()) chain(b, "bus '" + b.value("name", std::string("bus")) + "'");
    if (job.contains("master") && job["master"].is_object()) chain(job["master"], "master");
    return out;
}

std::vector<std::pair<std::string, std::string>> jobFileRefs(const json &job) {
    json copy = job;
    std::vector<std::pair<std::string, std::string>> out;
    for (auto &r : fileRefs(copy)) out.push_back({r.where, r.path});
    return out;
}

std::string newUuid() {
    std::random_device rd;
    std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    uint8_t b[16];
    for (int i = 0; i < 16; i += 8) { const uint64_t v = g(); std::memcpy(b + i, &v, 8); }
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);   // version 4
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);   // variant 10
    char s[37];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

std::string nowRfc3339() { return rfc3339(std::time(nullptr)); }

std::string rfc3339(std::time_t t) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &local);
    std::string s = buf;   // +hhmm -> +hh:mm
    if (s.size() > 5 && (s[s.size() - 5] == '+' || s[s.size() - 5] == '-')) s.insert(s.size() - 2, ":");
    return s;
}

json actor() {
    const char *name = std::getenv("WAVELENGTH_AUTHOR"), *kind = std::getenv("WAVELENGTH_AUTHOR_KIND");
    if (name && *name) return {{"name", name}, {"kind", kind && *kind ? kind : "human"}};
    if (std::getenv("CLAUDECODE")) return {{"name", "Claude Code"}, {"kind", "ai"}};
    if (std::getenv("CODEX_SANDBOX") || std::getenv("CODEX_HOME")) return {{"name", "Codex"}, {"kind", "ai"}};
    const char *user = std::getenv("USER");
    if (!user) user = std::getenv("USERNAME");
    return {{"name", user && *user ? user : "someone"}, {"kind", "human"}};
}

std::string readText(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeText(const fs::path &p, const std::string &text, std::string &err) {
    std::error_code ec;
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(text.data(), (std::streamsize)text.size());
        if (!out) { err = "cannot write " + p.string(); return false; }
    }
    fs::rename(tmp, p, ec);
    if (ec) { err = "cannot write " + p.string() + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

} // namespace wl
