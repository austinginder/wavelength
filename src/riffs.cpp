#include "riffs.hpp"

#include "job.hpp"
#include "platform.hpp"
#include "song.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace wl {

using json = nlohmann::json;

namespace {

const char *kNoteNames[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

bool readJsonFile(const fs::path &p, json &out) {
    std::ifstream in(p);
    if (!in) return false;
    out = json::parse(in, nullptr, false);
    return !out.is_discarded();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string trimmed(const json &in, const char *k, size_t max) {
    std::string v = in.contains(k) && in[k].is_string() ? in[k].get<std::string>() : "";
    v.erase(0, v.find_first_not_of(" \t\n"));
    v.erase(v.find_last_not_of(" \t\n") + 1);
    return v.substr(0, max);
}

// the parts of a track or sounds.json entry that make its sound
json soundOf(const json &t) {
    json s = json::object();
    for (const char *k : {"plugin", "preset", "state", "params"})
        if (t.contains(k) && !(t[k].is_object() && t[k].empty())) s[k] = t[k];
    return s;
}

bool sameBytes(const fs::path &a, const fs::path &b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    if (!x || !y) return false;
    std::stringstream sx, sy;
    sx << x.rdbuf();
    sy << y.rdbuf();
    return sx.str() == sy.str();
}

// a preset file the sound names (relative to the library) copied beside the song's job, its path pointed there
bool copyStateFiles(json &sound, const fs::path &lib, const fs::path &base, std::string &err) {
    auto fix = [&](json &v) -> bool {
        if (!v.is_string()) return true;
        const std::string s = v.get<std::string>();
        const fs::path p = fs::u8path(s);
        std::error_code ec;
        if (s.rfind("lib:", 0) == 0 || p.is_absolute() || !fs::is_regular_file(lib / p, ec)) return true;
        fs::path rel = p;
        for (int n = 2; n < 1000; ++n) {
            const fs::path dst = base / rel;
            if (!fs::exists(dst, ec)) {
                fs::create_directories(dst.parent_path(), ec);
                if (!platform::copyFile(lib / p, dst, ec)) { err = "could not copy " + s + ": " + ec.message(); return false; }
                break;
            }
            if (sameBytes(lib / p, dst)) break;
            rel = p.parent_path() / (p.stem().u8string() + "-" + std::to_string(n) + p.extension().u8string());
        }
        v = rel.generic_u8string();
        return true;
    };
    if (!sound.contains("state")) return true;
    if (sound["state"].is_string()) return fix(sound["state"]);
    if (sound["state"].is_object() && sound["state"].contains("file")) return fix(sound["state"]["file"]);
    return true;
}

} // namespace

fs::path riffLibraryDir() {
    const char *e = std::getenv("WAVELENGTH_RIFFS");
    if (e && *e) return fs::u8path(e);
    return platform::dataDir() / "Riffs";
}

bool normalizeRiff(const json &in, json &out, std::string &err) {
    out = json::object();
    const std::string id = trimmed(in, "id", 40);
    if (!id.empty()) {
        if (!std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; })) { err = "a riff id is a-z, 0-9 and dashes"; return false; }
        out["id"] = id;
    }
    out["name"] = trimmed(in, "name", 80).empty() ? "Riff" : trimmed(in, "name", 80);
    out["track"] = trimmed(in, "track", 80);
    if (!trimmed(in, "group", 80).empty()) out["group"] = trimmed(in, "group", 80);
    if (!trimmed(in, "from", 120).empty()) out["from"] = trimmed(in, "from", 120);
    const double tempo = in.value("tempo", 0.0);
    if (!(tempo >= 20 && tempo <= 400)) { err = "a riff needs its tempo (20 to 400 BPM)"; return false; }
    out["tempo"] = std::round(tempo * 100) / 100;
    int num = 4, den = 4;
    if (in.contains("timeSignature") && in["timeSignature"].is_array() && in["timeSignature"].size() == 2 && in["timeSignature"][0].is_number_integer() &&
        in["timeSignature"][1].is_number_integer()) {
        num = in["timeSignature"][0].get<int>();
        den = in["timeSignature"][1].get<int>();
    }
    if (num < 1 || num > 32 || (den != 1 && den != 2 && den != 4 && den != 8 && den != 16)) { err = "a time signature like [4, 4]"; return false; }
    out["timeSignature"] = {num, den};
    const double bars = in.value("bars", 0.0), beats = bars * num * 4.0 / den;
    if (!(bars >= 0.25 && bars <= 64)) { err = "a riff is a quarter of a bar to 64 bars long"; return false; }
    out["bars"] = std::round(bars * 1000) / 1000;
    const std::string q = trimmed(in, "quantize", 8);
    static const std::set<std::string> grids = {"off", "1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
    out["quantize"] = grids.count(q) ? q : "off";
    if (!trimmed(in, "note", 2000).empty()) out["note"] = trimmed(in, "note", 2000);
    auto noteList = [&](const char *k, double digits, bool required) -> bool {
        if (!in.contains(k)) { if (required) err = "a riff needs its notes"; return !required; }
        if (!in[k].is_array() || in[k].size() > 2000) { err = std::string(k) + ": a list of at most 2000 notes"; return false; }
        json list = json::array();
        for (const auto &n : in[k]) {
            if (!n.is_object()) continue;
            int key;
            try { key = parseKey(n.value("key", json(60))); } catch (...) { err = "a note's key: 60 or a name like \"F#4\""; return false; }
            if (key < 0 || key > 127) continue;
            const double beat = n.value("beat", -1.0), dur = n.value("dur", 0.0), vel = n.value("vel", 0.8);
            if (!(beat >= 0 && beat <= beats) || !(dur > 0)) continue;
            const double m = std::pow(10.0, digits);
            list.push_back({{"beat", std::round(beat * m) / m}, {"dur", std::max(1 / m, std::round(std::min(dur, beats) * m) / m)},
                            {"key", std::string(kNoteNames[key % 12]) + std::to_string(key / 12 - 1)}, {"vel", std::round(std::clamp(vel, 0.0, 1.0) * 100) / 100}});
        }
        if (required && list.empty()) { err = "the riff has no notes inside its bars"; return false; }
        if (!list.empty()) out[k] = list;
        return true;
    };
    return noteList("notes", 4, true) && noteList("played", 3, false);
}

json readRiffFile(const fs::path &file) {
    json rf;
    if (!readJsonFile(file, rf) || !rf.is_object() || !rf.contains("riffs") || !rf["riffs"].is_array())
        rf = {{"format", "wavelength.riffs"}, {"formatVersion", "1.0"}, {"riffs", json::array()}};
    return rf;
}

bool writeRiffFile(const fs::path &file, json rf, std::string &err) {
    const std::vector<std::string> groups = riffGroups(rf);
    if (groups.empty()) rf.erase("groups"); else rf["groups"] = groups;
    std::error_code ec;
    if (rf["riffs"].empty() && groups.empty()) { fs::remove(file, ec); return true; }
    fs::create_directories(file.parent_path(), ec);
    return platform::writeFileAtomic(file, rf.dump(1) + "\n", err);
}

std::vector<std::string> riffGroups(const json &rf) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const json &g) {
        if (!g.is_string()) return;
        const std::string s = g.get<std::string>();
        if (!s.empty() && seen.insert(lower(s)).second) out.push_back(s);
    };
    if (rf.contains("groups") && rf["groups"].is_array()) for (const auto &g : rf["groups"]) add(g);
    if (rf.contains("riffs") && rf["riffs"].is_array())
        for (const auto &r : rf["riffs"]) if (r.is_object() && r.contains("group")) add(r["group"]);
    return out;
}

json libraryInstruments(const fs::path &lib) {
    json out = json::object(), job, sf;
    if (readJsonFile(lib / "job.json", job) && job.contains("tracks") && job["tracks"].is_array())
        for (const auto &t : job["tracks"])
            if (t.is_object() && t.contains("name") && t["name"].is_string()) out[t["name"].get<std::string>()] = soundOf(t);
    if (readJsonFile(lib / "sounds.json", sf) && sf.contains("tracks") && sf["tracks"].is_array())
        for (const auto &e : sf["tracks"]) {
            if (!e.is_object() || !e.contains("track") || !e["track"].is_string()) continue;
            const std::string name = e["track"].get<std::string>();
            const json s = soundOf(e);
            if (s.contains("plugin") || !out.contains(name)) out[name] = s;
            else for (auto &[k, v] : s.items()) out[name][k] = v;   // no plugin: only the keys it gives change
            if (e.contains("note") && e["note"].is_string()) out[name]["note"] = e["note"];
        }
    return out;
}

std::string soundLine(const json &s) {
    std::string plugin = s.value("plugin", std::string());
    for (const char *p : {"clap:", "vst3:", "vst2:", "au:"})
        if (plugin.rfind(p, 0) == 0) { plugin.erase(0, std::char_traits<char>::length(p)); break; }
    std::string out = plugin.empty() ? "no instrument" : plugin;
    if (s.contains("preset") && s["preset"].is_string()) out += " · " + fs::u8path(s["preset"].get<std::string>()).filename().u8string();
    else if (s.contains("state")) out += " · its own state";
    const size_t knobs = s.contains("params") && s["params"].is_object() ? s["params"].size() : 0;
    if (knobs) out += " · " + std::to_string(knobs) + (knobs == 1 ? " knob set" : " knobs set");
    return out;
}

std::string riffNoteLine(const json &r, size_t maxNotes) {
    int num = 4, den = 4;
    if (r.contains("timeSignature") && r["timeSignature"].is_array() && r["timeSignature"].size() == 2) {
        num = r["timeSignature"][0].get<int>();
        den = r["timeSignature"][1].get<int>();
    }
    const double bpb = num * 4.0 / den;
    std::vector<std::pair<double, std::string>> notes;
    for (const auto &n : r.value("notes", json::array()))
        if (n.is_object() && n.contains("key")) notes.push_back({n.value("beat", 0.0), n["key"].is_string() ? n["key"].get<std::string>() : std::to_string(n["key"].get<int>())});
    std::stable_sort(notes.begin(), notes.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::string out;
    int bar = 0;
    double last = -1;
    size_t shown = 0;
    for (const auto &[beat, key] : notes) {
        if (shown >= maxNotes) { out += " … (" + std::to_string(notes.size() - shown) + " more)"; break; }
        const int b = (int)std::floor(beat / bpb + 1e-6);
        if (out.empty()) for (; bar < b; ++bar) out += "- | ";   // bars of rest before the first note
        else if (std::fabs(beat - last) < 1e-6) out += "+";
        else {
            out += b > bar ? " | " : " ";
            for (; bar + 1 < b; ++bar) out += "- | ";   // an empty bar between
        }
        bar = b;
        out += key;
        last = beat;
        ++shown;
    }
    return out;
}

bool resolveRiffs(const json &rf, const std::vector<std::string> &refs, std::vector<json> &out, std::string &err) {
    const json &list = rf["riffs"];
    std::set<std::string> taken;
    auto take = [&](const json &r) { if (taken.insert(r.value("id", std::string())).second) out.push_back(r); };
    for (const std::string &ref : refs) {
        if (lower(ref).rfind("group:", 0) == 0) {
            const std::string g = lower(ref.substr(6));
            size_t n = 0;
            for (const auto &r : list) if (lower(r.value("group", std::string())) == g) { take(r); ++n; }
            if (!n) { err = "no riffs in a group named '" + ref.substr(6) + "' (wavelength riffs lists the groups)"; return false; }
            continue;
        }
        const json *hit = nullptr;
        for (const auto &r : list) if (r.value("id", std::string()) == ref) hit = &r;
        if (!hit) {
            std::vector<const json *> named;
            for (const auto &r : list) if (lower(r.value("name", std::string())) == lower(ref)) named.push_back(&r);
            if (named.size() > 1) {
                err = std::to_string(named.size()) + " riffs are named '" + ref + "':";
                for (const json *r : named) err += " " + r->value("id", std::string());
                err += "; name one by its id";
                return false;
            }
            if (named.size() == 1) hit = named[0];
        }
        if (!hit) { err = "no riff '" + ref + "' in the library (wavelength riffs lists them)"; return false; }
        take(*hit);
    }
    return true;
}

json useRiffs(const fs::path &songIn, const std::vector<json> &riffs, const fs::path &lib, std::string &err) {
    std::error_code ec;
    const fs::path dir = fs::absolute(songIn, ec).lexically_normal();
    if (fs::exists(dir, ec) && !fs::is_directory(dir, ec)) { err = dir.u8string() + " is a file, not a song folder"; return {}; }
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) { err = "could not make " + dir.u8string(); return {}; }
    Song song;
    std::string serr;
    const bool manifest = openSong(dir.u8string(), song, serr) && song.hasManifest();
    const std::string jobRel = manifest ? song.jobFile() : "job.json";
    const fs::path jobFile = dir / fs::u8path(jobRel), base = jobFile.parent_path();

    json job, sf;
    const bool hadJob = readJsonFile(jobFile, job) && job.is_object();
    if (!hadJob) job = json::object();
    if (!job.contains("tracks") || !job["tracks"].is_array()) job["tracks"] = json::array();
    if (!readJsonFile(base / "sounds.json", sf) || !sf.is_object() || !sf.contains("tracks") || !sf["tracks"].is_array())
        sf = {{"format", "wavelength.sounds"}, {"formatVersion", "1.0"}, {"tracks", json::array()}};
    json rf = readRiffFile(base / "riffs.json");
    const json inst = libraryInstruments(lib);

    // the sound a song track has now: its sounds.json entry, else the job's track; null when there is no such track
    auto songSound = [&](const std::string &name) -> json {
        for (const auto &e : sf["tracks"]) if (e.is_object() && e.value("track", std::string()) == name && e.contains("plugin")) return soundOf(e);
        for (const auto &t : job["tracks"]) if (t.is_object() && t.value("name", std::string()) == name) return soundOf(t);
        return nullptr;
    };
    std::set<std::string> ids, copied;
    for (const auto &r : rf["riffs"]) {
        ids.insert(r.value("id", std::string()));
        if (r.contains("from")) copied.insert(r.value("from", std::string()));
    }
    std::map<std::string, std::string> trackFor;   // library instrument -> the song's track
    json riffsOut = json::array(), skipped = json::array(), tracksOut = json::array();
    bool jobChanged = !hadJob, sfChanged = false;
    for (const json &r : riffs) {
        const std::string libId = r.value("id", std::string()), tag = "library:" + libId, name = r.value("name", std::string());
        if (copied.count(tag)) { skipped.push_back({{"id", libId}, {"name", name}, {"why", "already in the song"}}); continue; }
        const std::string instName = r.value("track", std::string());
        if (!inst.contains(instName)) { err = "riff " + libId + " (" + name + ") plays on '" + instName + "', which the library has no instrument for"; return {}; }
        std::string track;
        if (trackFor.count(instName)) track = trackFor[instName];
        else {
            json sound = inst[instName];
            const std::string note = sound.value("note", std::string());
            sound.erase("note");
            if (!copyStateFiles(sound, lib, base, err)) return {};
            const std::string stem = instName.empty() ? "Riffs" : instName;
            bool added = false;
            for (int n = 2; n < 1000; ++n) {
                const std::string candidate = n == 2 ? stem : stem + " " + std::to_string(n - 1);
                const json have = songSound(candidate);
                if (have.is_null()) {
                    json e = {{"track", candidate}};
                    for (auto &[k, v] : sound.items()) e[k] = v;
                    if (!note.empty()) e["note"] = note;
                    sf["tracks"].push_back(e);
                    json t = {{"name", candidate}};
                    for (auto &[k, v] : sound.items()) t[k] = v;
                    t["notes"] = json::array();
                    job["tracks"].push_back(t);
                    track = candidate;
                    added = sfChanged = jobChanged = true;
                    break;
                }
                if (have == sound) { track = candidate; break; }
            }
            if (track.empty()) { err = "no free track name for " + stem; return {}; }
            trackFor[instName] = track;
            tracksOut.push_back({{"name", track}, {"instrument", instName}, {"sound", soundLine(sound)}, {"added", added}});
        }
        json c = r;
        int n = 1;
        while (ids.count("riff-" + std::to_string(n))) ++n;
        c["id"] = "riff-" + std::to_string(n);
        ids.insert(c["id"]);
        c["track"] = track;
        c["from"] = tag;
        c.erase("group");   // the library's filing; "from" says where it came from
        rf["riffs"].push_back(c);
        copied.insert(tag);
        riffsOut.push_back({{"id", c["id"]}, {"name", name}, {"track", track}, {"from", tag}});
    }
    if (!hadJob && !riffs.empty()) {   // a new song: the first riff's tempo and meter
        job["tempo"] = riffs[0].value("tempo", 120.0);
        const json meter = riffs[0].value("timeSignature", json::array({4, 4}));
        if (meter != json::array({4, 4})) job["timeSignature"] = meter;
    }
    if (!riffsOut.empty()) {
        if (jobChanged && !platform::writeFileAtomic(jobFile, job.dump(1) + "\n", err)) return {};
        if (sfChanged && !platform::writeFileAtomic(base / "sounds.json", sf.dump(1) + "\n", err)) return {};
        if (!writeRiffFile(base / "riffs.json", rf, err)) return {};
        if (manifest) {   // the manifest lists them as sources, the way the Sounds page does
            json &files = song.manifest["files"];
            if (!files.is_array()) files = json::array();
            for (const fs::path &p : {base / "riffs.json", base / "sounds.json"}) {
                const std::string rel = fs::relative(p, dir, ec).generic_u8string();
                if (!fs::exists(p, ec) || rel == song.jobFile()) continue;
                if (std::none_of(files.begin(), files.end(), [&](const json &f) { return f.value("path", std::string()) == rel; }))
                    files.push_back({{"path", rel}, {"role", "source"}, {"mediaType", "application/json"}});
            }
            if (!writeManifest(song, err)) return {};
        }
    }
    return {{"song", dir.u8string()}, {"riffs", riffsOut}, {"skipped", skipped}, {"tracks", tracksOut}, {"job", jobRel}, {"jobCreated", !hadJob && !riffsOut.empty()}};
}

} // namespace wl
