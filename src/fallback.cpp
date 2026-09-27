#include "fallback.hpp"

#include "catalog.hpp"
#include "job.hpp"
#include "sampler.hpp"

#include <algorithm>
#include <filesystem>
#include <map>

namespace wl {

namespace {

using json = nlohmann::json;

// what makes a track's sound: replaced as a whole by a fallback (keys the fallback doesn't give are dropped,
// so a synth never gets a library's articulations or a plugin's parameter names)
const char *kSoundKeys[] = {"plugin", "preset", "state", "params", "synth", "sampler", "shepard", "drum", "warmup", "bendRange",
                            "articulations", "range", "velocityTo"};

// true when this computer can play `sound` (a track or a fallback entry); `why` says what is missing
bool available(const json &sound, const std::string &baseDir, std::string &why) {
    const std::string plugin = sound.value("plugin", "");
    if (plugin.empty()) { why = "no plugin"; return false; }
    // a library's file by name ("lib:Legend 909/Kick.wav") has to be installed here
    auto libMissing = [&](const json &v, const std::string &what) {
        if (!v.is_string() || v.get<std::string>().rfind("lib:", 0) != 0 || !resolveLibraryFile(v.get<std::string>()).empty()) return false;
        why = what + " '" + v.get<std::string>() + "' is not on this computer";
        return true;
    };
    if (sound.contains("clips") && sound["clips"].is_array())
        for (auto &c : sound["clips"])
            if (c.is_object() && c.contains("file") && libMissing(c["file"], "clip")) return false;
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
        if (s.contains("sample") && libMissing(s["sample"], "sample")) return false;
        for (const char *k : {"map", "kit"})
            if (s.contains(k) && s[k].is_object())
                for (auto &[key, v] : s[k].items())
                    if (libMissing(v.is_object() && v.contains("file") ? v["file"] : v, "kit sample")) return false;
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

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// a word of `text`, or the start of one for words longer than three letters: "string" in "strings", "tom"
// not in "atom" or "tomorrow", "arp" not in "harp"
bool word(const std::string &text, const std::string &w) {
    const bool whole = w.size() <= 3 && std::isalnum((unsigned char)w.back());
    for (size_t at = text.find(w); at != std::string::npos; at = text.find(w, at + 1)) {
        if (at > 0 && std::isalpha((unsigned char)text[at - 1])) continue;
        const size_t end = at + w.size();
        if (whole && end < text.size() && std::isalpha((unsigned char)text[end])) continue;
        return true;
    }
    return false;
}

bool anyWord(const std::string &text, std::initializer_list<const char *> words) {
    for (const char *w : words) if (word(text, w)) return true;
    return false;
}

json synthSound(const std::string &patch) { return {{"plugin", "builtin:synth"}, {"preset", patch}}; }

json gmSound(int program) {
    std::string sf = defaultSoundFont();
    sf = sf.empty() ? "MuseScore_General" : std::filesystem::path(sf).stem().string();
    return {{"plugin", "builtin:sampler"}, {"sampler", {{"soundfont", sf}, {"program", program}}}};
}

struct NoteStats {
    int count = 0, distinct = 0, lowest = 127, median = 60, maxPoly = 0;
    double meanDur = 0;
};

NoteStats statsOf(const json &track) {
    NoteStats st;
    std::vector<int> keys;
    std::vector<std::pair<double, double>> spans;
    double dur = 0;
    for (auto &n : track.value("notes", json::array())) {
        if (!n.is_object() || !n.contains("key")) continue;
        int k;
        try { k = parseKey(n["key"]); } catch (...) { continue; }
        const double b = n.value("beat", 0.0), d = n.value("dur", 1.0);
        keys.push_back(k);
        spans.push_back({b, b + d});
        dur += d;
    }
    st.count = (int)keys.size();
    if (keys.empty()) return st;
    std::vector<int> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    st.lowest = sorted.front();
    st.median = sorted[sorted.size() / 2];
    st.distinct = (int)(std::unique(sorted.begin(), sorted.end()) - sorted.begin());
    st.meanDur = dur / keys.size();
    std::sort(spans.begin(), spans.end());
    for (size_t i = 0; i < spans.size() && i < 4000; ++i) {   // the most notes sounding at once
        int poly = 0;
        for (size_t j = i; j < spans.size() && spans[j].first < spans[i].first + 1e-6; ++j) ++poly;
        for (size_t j = 0; j < i; ++j) if (spans[j].second > spans[i].first + 1e-6) ++poly;
        st.maxPoly = std::max(st.maxPoly, poly);
        if (st.maxPoly >= 6) break;
    }
    return st;
}

} // namespace

json suggestFallback(const json &track) {
    if (!track.is_object() || track.contains("fallback")) return nullptr;
    const std::string plugin = track.value("plugin", std::string());
    const json sampler = track.value("sampler", json::object());
    bool named = false;   // a sampler playing a named library, which another computer may not have
    for (const char *k : {"multisample", "kit", "soundfont", "sfz", "sample"})
        if (sampler.contains(k) && sampler[k].is_string()) {
            const std::string v = sampler[k].get<std::string>();
            named |= v.rfind("lib:", 0) == 0 || (v.find('/') == std::string::npos && v.find('.') == std::string::npos);
        }
    if (plugin.rfind("builtin:", 0) == 0 && !(plugin == "builtin:sampler" && named)) return nullptr;

    std::string stateName;
    if (track.contains("state")) {
        const json &s = track["state"];
        const std::string f = s.is_string() ? s.get<std::string>() : s.is_object() ? s.value("file", std::string()) : "";
        stateName = std::filesystem::path(f).stem().string();
    }
    std::string sampleName;
    for (const char *k : {"multisample", "kit", "soundfont", "sfz", "sample"})
        if (sampler.contains(k) && sampler[k].is_string()) sampleName += " " + sampler[k].get<std::string>();
    std::string spaced;   // "ChipKick" reads as "chip kick"
    for (char c : track.value("name", std::string())) {
        if (std::isupper((unsigned char)c) && !spaced.empty() && std::islower((unsigned char)spaced.back())) spaced += ' ';
        spaced += c;
    }
    const std::string name = lowered(spaced);
    const std::string text = name + " | " + lowered(track.value("preset", std::string()) + " " + stateName + " " + sampleName + " " + plugin);
    const NoteStats st = statsOf(track);
    json out = {{"role", ""}, {"why", ""}, {"fallback", json::array()}};
    auto give = [&](const std::string &role, const std::string &why, json list) {
        out["role"] = role;
        out["why"] = why;
        out["fallback"] = list;
        return out;
    };

    // ---- drums: the built-in GM kit, moved onto the right key for a one-sound track
    const bool kit = sampler.contains("kit") || anyWord(text, {"drum", "kit", "909", "808", "707", "linn"});
    static const std::vector<std::pair<std::vector<const char *>, int>> drumKeys = {
        {{"kick", "bd", "bassdrum", "bass drum"}, 36}, {{"snare", "sd", "clap"}, 38}, {{"rim", "perc", "cowbell", "shaker", "tamb", "conga", "bongo"}, 37},
        {{"open hat", "open hi", "oh", "hat open", "hi-hat open", "hihat open", "hat 02 open", "hat 01 open"}, 46}, {{"hat", "hh", "hihat", "hi-hat"}, 42},
        {{"crash", "cymbal"}, 49}, {{"ride"}, 51}, {{"tom"}, 45}};
    int drumKey = 0;
    std::string drumWord;
    for (auto &[words, key] : drumKeys) {
        for (const char *w : words) if (word(name, w) || word(text, w)) { drumKey = key; drumWord = w; break; }
        if (drumKey) break;
    }
    const bool gmNotes = st.count > 0 && st.distinct >= 3 && st.lowest >= 35 && st.median <= 59;
    const bool oneSample = plugin == "builtin:sampler" && sampler.contains("sample");
    const bool chipVoice = anyWord(text, {"chip", "nes", "8-bit", "8bit", "gameboy", "relica"});   // chip drums are noise at any pitch
    if (drumKey && (st.distinct <= 2 || oneSample || chipVoice)) {   // one drum sound, whatever keys it is played on
        static const std::map<int, const char *> drumNames = {{36, "kick"}, {37, "rim"}, {38, "snare"}, {42, "hat"}, {46, "open hat"},
                                                              {49, "crash"}, {51, "ride"}, {45, "tom"}};
        const bool gmKey = st.distinct == 1 && st.lowest == drumKey;
        json d = {{"plugin", "builtin:drums"}};
        if (!gmKey) d["drum"] = drumNames.at(drumKey);
        return give("drums", "a one-sound drum track ('" + drumWord + "'): the built-in " + drumNames.at(drumKey), json::array({d}));
    }

    // ---- orchestral and acoustic parts: a General MIDI program, then a synth patch for computers without the SoundFont
    static const std::vector<std::tuple<std::vector<const char *>, int, const char *, const char *>> acoustic = {
        {{"pizz"}, 45, "PL Pluck", "pizzicato strings"}, {{"trem"}, 44, "PD Strings", "tremolo strings"},
        {{"violin", "viola", "strings", "string ensemble", "ensemble", "spiccato", "legato strings"}, 48, "PD Strings", "strings"},
        {{"cello", "celli"}, 48, "PD Strings", "cellos"}, {{"contrabass", "double bass", "basses"}, 43, "BA Analog", "basses"},
        {{"harpsichord", "harpsich"}, 6, "PL Pluck", "harpsichord"}, {{"harp"}, 46, "PL Pluck", "harp"}, {{"timpani", "timp"}, 47, "PL Mallet", "timpani"},
        {{"choir", "vox", "voice", "voices", "aah", "ooh", "chant"}, 52, "PD Glass", "choir"},
        {{"horn"}, 60, "BR Brass", "horns"}, {{"trumpet"}, 56, "BR Brass", "trumpets"}, {{"trombone"}, 57, "BR Brass", "trombones"},
        {{"tuba"}, 58, "BA Analog", "tuba"}, {{"brass"}, 61, "BR Brass", "brass"}, {{"piccolo"}, 72, "LD Soft", "piccolo"},
        {{"flute"}, 73, "LD Soft", "flute"}, {{"oboe"}, 68, "LD Soft", "oboe"}, {{"clarinet"}, 71, "LD Soft", "clarinet"},
        {{"bassoon"}, 70, "BA Analog", "bassoon"}, {{"piano", "grand"}, 0, "KY Electric Piano", "piano"},
        {{"celesta"}, 8, "PL Bell", "celesta"},
        {{"glock"}, 9, "PL Bell", "glockenspiel"}, {{"marimba"}, 12, "PL Mallet", "marimba"}, {{"xylophone"}, 13, "PL Mallet", "xylophone"},
        {{"tubular", "chimes"}, 14, "PL Bell", "tubular bells"}, {{"organ"}, 19, "KY Organ", "organ"}, {{"guitar"}, 25, "PL Pluck", "guitar"}};
    const bool orchestral = anyWord(text, {"bbc", "spitfire", "orchestra", "symphon", "discover", "nksf"});
    auto acousticBy = [&](const std::string &where) -> json {
        for (auto &[words, program, patch, what] : acoustic)
            for (const char *w : words)
                if (word(where, w))
                    return give(what, std::string("sounds like ") + what + ": General MIDI program " + std::to_string(program) + ", then " + patch,
                                json::array({gmSound(program), synthSound(patch)}));
        return nullptr;
    };
    if (json a = acousticBy(name); !a.is_null()) return a;
    if (kit || (drumKey && gmNotes)) return give("drums", "a drum kit on General MIDI keys: the built-in kit", json::array({{{"plugin", "builtin:drums"}}}));
    if (orchestral || plugin == "builtin:sampler")
        if (json a = acousticBy(text); !a.is_null()) return a;
    if (orchestral) return give("strings", "an orchestral part: General MIDI strings, then PD Strings", json::array({gmSound(48), synthSound("PD Strings")}));

    // ---- synth parts by name, preset and notes
    const bool chip = anyWord(text, {"chip", "nes", "8-bit", "8bit", "gameboy", "relica"});
    std::string patch, role, why;
    auto pick = [&](const std::string &p, const std::string &r, const std::string &w) { if (patch.empty()) { patch = p; role = r; why = w; } };
    if (anyWord(text, {"riser", "sweep", "noise", "whoosh", "uplift", "downlift"})) pick("FX Noise Sweep", "fx", "a riser or sweep");
    if (anyWord(text, {"zap", "laser"})) pick("FX Zap", "fx", "a zap");
    if (anyWord(text, {"sfx", "fx"})) pick(chip ? "FX Zap" : "FX Noise Sweep", "fx", "sound effects");
    if (anyWord(text, {"sub"})) pick("BA Sub", "bass", "a sub bass");
    if (anyWord(text, {"reese"})) pick("BA Reese", "bass", "a Reese bass");
    if (anyWord(text, {"acid", "303"})) pick("BA Acid", "bass", "an acid bass");
    if (anyWord(text, {"bass", "ba "})) pick(chip ? "BA Pluck" : anyWord(text, {"fm", "dx"}) ? "BA FM" : anyWord(text, {"pluck", "offbeat", "pulse"}) ? "BA Pluck" : "BA Analog", "bass", "a bass");
    if (anyWord(text, {"arp", "arps", "arpeggio"})) pick(chip ? "PL Chip Arp" : "PL Pluck", "arp", "an arpeggio");
    if (anyWord(text, {"pad", "pads", "atmo", "drone", "ambient", "texture"}))
        pick(anyWord(text, {"supersaw", "trance", "hyper"}) ? "PD Supersaw" : anyWord(text, {"dark"}) ? "PD Dark" : anyWord(text, {"glass", "bell"}) ? "PD Glass"
             : anyWord(text, {"string"}) ? "PD Strings" : "PD Warm", "pad", "a pad");
    if (anyWord(text, {"bell", "chime"})) pick("PL Bell", "pluck", "a bell");
    if (anyWord(text, {"mallet"})) pick("PL Mallet", "pluck", "a mallet");
    if (anyWord(text, {"stab"})) pick("BR Stab", "stab", "a stab");
    if (anyWord(text, {"supersaw", "hypersaw"})) pick(st.meanDur >= 1.5 ? "PD Supersaw" : "LD Supersaw", st.meanDur >= 1.5 ? "pad" : "lead", "a supersaw");
    if (anyWord(name, {"chord", "chords", "comp"})) pick(st.meanDur < 1 ? "PL Pluck" : "PD Warm", st.meanDur < 1 ? "pluck" : "pad", "chords");
    if (anyWord(text, {"pluck", "pl "})) pick("PL Pluck", "pluck", "a pluck");
    if (anyWord(text, {"keys", "rhodes", "ep", "e.piano", "epiano", "wurli"})) pick("KY Electric Piano", "keys", "keys");
    if (anyWord(text, {"lead", "melody", "hook", "theme", "solo", "ld "}))
        pick(chip ? "LD Chip" : anyWord(text, {"supersaw", "trance", "anthem"}) ? "LD Supersaw" : anyWord(text, {"square"}) ? "LD Square"
             : anyWord(text, {"soft", "whistle"}) ? "LD Soft" : "LD Saw", "lead", "a lead");
    if (chip && anyWord(name, {"tri", "triangle"})) pick("LD Soft", "lead", "a triangle voice");
    if (chip) pick(st.median < 48 ? "BA Pluck" : "LD Chip", st.median < 48 ? "bass" : "lead", "a chiptune voice");
    if (patch.empty() && st.count) {   // by the notes alone
        if (st.median < 48) pick("BA Analog", "bass", "low notes (a bass)");
        else if (st.meanDur >= 2 && st.maxPoly >= 2) pick("PD Warm", "pad", "long chords (a pad)");
        else if (st.maxPoly >= 3) pick("KY Electric Piano", "keys", "chords (keys)");
        else if (st.meanDur <= 0.5 && st.count >= 32) pick("PL Pluck", "pluck", "many short notes (a pluck)");
        else pick("LD Saw", "lead", "a single line (a lead)");
    }
    if (patch.empty()) pick("PD Warm", "pad", "no notes to judge by");
    return give(role, why + ": " + patch, json::array({synthSound(patch)}));
}

bool soundAvailable(const json &sound, const std::string &baseDir, std::string &why) { return available(sound, baseDir, why); }

void useSound(json &t, const json &f) {
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
}

int applyFallbacks(json &job, const std::string &baseDir, std::vector<std::string> &notes, bool force) {
    int swaps = 0;
    if (!job.is_object() || !job.contains("tracks") || !job["tracks"].is_array()) return 0;
    for (auto &t : job["tracks"]) {
        if (!t.is_object() || !t.contains("fallback")) continue;
        const json list = t["fallback"].is_array() ? t["fallback"] : json::array({t["fallback"]});
        t.erase("fallback");
        std::string why;
        if (force) why = "--fallbacks";
        else if (available(t, baseDir, why)) continue;
        const std::string name = t.value("name", "track");
        bool swapped = false;
        for (const auto &f : list) {
            std::string whyNot;
            if (!f.is_object() || !available(f, baseDir, whyNot)) continue;
            useSound(t, f);
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
