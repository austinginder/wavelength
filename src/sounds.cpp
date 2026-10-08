#include "sounds.hpp"

#include "fallback.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace wl {

using json = nlohmann::json;

const char *const kSoundEntryKeys[4] = {"plugin", "preset", "state", "params"};

namespace {
// "clap:Wavelength Trance" and "wavelength trance" name the same instrument
std::string plainPlugin(std::string s) {
    for (const char *p : {"clap:", "vst3:", "vst2:", "au:"})
        if (s.rfind(p, 0) == 0) { s.erase(0, std::char_traits<char>::length(p)); break; }
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
} // namespace

json applySounds(const json &jobIn, const std::string &baseDir, std::vector<std::pair<std::string, std::string>> *unmatched) {
    namespace fs = std::filesystem;
    std::ifstream in(fs::path(baseDir) / "sounds.json");
    if (!in || !jobIn.is_object() || !jobIn.contains("tracks") || !jobIn["tracks"].is_array()) return jobIn;
    auto miss = [&](const std::string &track, const std::string &why) { if (unmatched) unmatched->push_back({track, why}); };
    json file;
    try { in >> file; } catch (...) { miss("", "sounds.json is not valid JSON; no sounds applied"); return jobIn; }
    if (!file.is_object() || !file.contains("tracks") || !file["tracks"].is_array()) return jobIn;
    json j = jobIn;
    for (const auto &e : file["tracks"]) {
        if (!e.is_object()) continue;
        const std::string name = e.value("track", std::string());
        json *t = nullptr;
        for (auto &x : j["tracks"])
            if (x.is_object() && (x.value("name", std::string()) == name || x.value("id", std::string()) == name)) { t = &x; break; }
        if (!t) { miss(name, "a sound for '" + name + "', but the job has no track by that name (add the track, or remove the sound on the Sounds page)"); continue; }
        json sound = json::object();
        for (const char *k : kSoundEntryKeys) if (e.contains(k)) sound[k] = e[k];
        if (!sound.contains("plugin")) {   // no plugin: only the keys it gives change
            for (auto &[k, v] : sound.items()) (*t)[k] = v;
            continue;
        }
        std::string why;
        if (!soundAvailable(sound, baseDir, why)) {
            miss(name, why + ", so the track plays its sound from the job");
            continue;
        }
        if (plainPlugin(sound["plugin"].get<std::string>()) != plainPlugin(t->value("plugin", std::string()))) {
            useSound(*t, sound);   // another instrument: the old one's settings and curves go
            continue;
        }
        for (const char *k : {"plugin", "preset", "state", "params"}) t->erase(k);   // the same one: its whole patch
        for (auto &[k, v] : sound.items()) (*t)[k] = v;
    }
    return j;
}

json jobWithSounds(const json &job, const std::string &baseDir) {
    namespace fs = std::filesystem;
    std::ifstream in(fs::path(baseDir) / "sounds.json");
    if (!in || !job.is_object()) return job;
    const json file = json::parse(in, nullptr, false);
    if (!file.is_object() || !file.contains("tracks") || !file["tracks"].is_array()) return job;
    json j = job;
    if (!j.contains("tracks") || !j["tracks"].is_array()) j["tracks"] = json::array();
    for (const auto &e : file["tracks"]) {
        if (!e.is_object()) continue;
        json t = {{"name", "sounds.json: " + e.value("track", std::string())}};
        for (const char *k : kSoundEntryKeys) if (e.contains(k)) t[k] = e[k];
        if (t.contains("plugin")) j["tracks"].push_back(t);
    }
    return j;
}

} // namespace wl
