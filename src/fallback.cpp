#include "fallback.hpp"

#include "catalog.hpp"
#include "sampler.hpp"

namespace wl {

namespace {

using json = nlohmann::json;

// what makes a track's sound: replaced as a whole by a fallback (keys the fallback doesn't give are dropped,
// so a synth never gets a library's articulations or a plugin's parameter names)
const char *kSoundKeys[] = {"plugin", "preset", "state", "params", "synth", "sampler", "shepard", "warmup", "bendRange",
                            "articulations", "range", "velocityTo"};

// true when this computer can play `sound` (a track or a fallback entry); `why` says what is missing
bool available(const json &sound, const std::string &baseDir, std::string &why) {
    const std::string plugin = sound.value("plugin", "");
    if (plugin.empty()) { why = "no plugin"; return false; }
    if (plugin == "builtin:sampler") {
        const json s = sound.value("sampler", json::object());
        for (const char *kind : {"multisample", "kit", "sfz", "soundfont"})
            if (s.contains(kind) && s[kind].is_string()) {
                std::string path, err;
                if (!findSampleEntry(kind, s[kind].get<std::string>(), baseDir, path, err)) {
                    why = std::string(kind) + " '" + s[kind].get<std::string>() + "' is not on this computer";
                    return false;
                }
            }
        return true;
    }
    if (plugin.rfind("builtin:", 0) == 0) return true;
    PluginInfo info;
    std::string err;
    if (!resolvePlugin(plugin, info, err)) { why = plugin + " is not installed (or is blocked)"; return false; }
    return true;
}

std::string describe(const json &sound) {
    std::string s = sound.value("plugin", "");
    if (sound.contains("preset") && sound["preset"].is_string()) s += " '" + sound["preset"].get<std::string>() + "'";
    return s;
}

} // namespace

int applyFallbacks(json &job, const std::string &baseDir, std::vector<std::string> &notes) {
    int swaps = 0;
    if (!job.is_object() || !job.contains("tracks") || !job["tracks"].is_array()) return 0;
    for (auto &t : job["tracks"]) {
        if (!t.is_object() || !t.contains("fallback")) continue;
        const json list = t["fallback"].is_array() ? t["fallback"] : json::array({t["fallback"]});
        t.erase("fallback");
        std::string why;
        if (available(t, baseDir, why)) continue;
        const std::string name = t.value("name", "track");
        bool swapped = false;
        for (const auto &f : list) {
            std::string whyNot;
            if (!f.is_object() || !available(f, baseDir, whyNot)) continue;
            for (const char *k : kSoundKeys) t.erase(k);
            if (t.contains("automation") && t["automation"].is_object())   // a plugin's own curves don't fit another sound
                for (const char *k : {"params", "cc", "pressure"}) t["automation"].erase(k);
            for (auto &[k, v] : f.items()) {
                if (k == "automation" && v.is_object() && t.contains("automation") && t["automation"].is_object())
                    for (auto &[ak, av] : v.items()) t["automation"][ak] = av;
                else t[k] = v;
            }
            if (!t.contains("articulations") && t.contains("notes") && t["notes"].is_array())   // keyswitches don't carry over
                for (auto &n : t["notes"]) if (n.is_object()) n.erase("art");
            notes.push_back("track '" + name + "': " + why + "; it plays its fallback " + describe(f));
            swapped = true;
            ++swaps;
            break;
        }
        if (!swapped) notes.push_back("track '" + name + "': " + why + ", and none of its fallbacks is available either");
    }
    return swaps;
}

} // namespace wl
