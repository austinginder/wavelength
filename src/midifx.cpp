#include "midifx.hpp"

#include "arp.hpp"
#include "automation.hpp"
#include "harmony.hpp"
#include "job.hpp"
#include "logic_patches.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

using nlohmann::json;

namespace wl {

namespace {
double r4(double v) { return std::round(v * 1e4) / 1e4; }
double r6(double v) { return std::round(v * 1e6) / 1e6; }

std::string pcName(int k) {
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((k % 12) + 12) % 12];
}
std::string keyName(int k) { return pcName(k) + std::to_string(k / 12 - 1); }   // 0-127, C4 = 60

std::string appleName(const std::string &type) {
    return type == "arp" ? "Arpeggiator" : type == "chord" ? "Chord Trigger" : type == "transpose" ? "Transposer" : type == "repeat" ? "Note Repeater" : type;
}

struct Range { int lo = 0, hi = 127; };
struct ChordFx { bool multi = false; std::vector<int> intervals; std::map<int, std::vector<int>> chords; Range range; int transpose = 0; };
struct TransposeFx { int semitones = 0; std::set<int> scale; };   // the scale's pitch classes (none = chromatic)
struct RepeatFx { double beats = 0.5, ms = -1, ramp = 1; int repeats = 3, transpose = 0; bool thru = true; Range range; };

bool knownKeys(const json &e, const std::set<std::string> &known, const std::string &w, std::string &err) {
    for (auto &[k, v] : e.items())
        if (!known.count(k)) {
            err = w + ": unknown setting '" + k + "' (it takes";
            for (auto &x : known) if (x != "type") err += " " + x + ",";
            err += " preset, patch)";
            return false;
        }
    return true;
}

bool integer(const json &j, int lo, int hi, int &out) {
    if (!j.is_number() || std::floor(j.get<double>()) != j.get<double>() || j.get<double>() < lo || j.get<double>() > hi) return false;
    out = (int)j.get<double>();
    return true;
}

// a key as a job writes it: a number, a note name, or digits in a string (a "chords" key)
bool keyOf(const json &k, int &out, const std::string &w, std::string &err) {
    try {
        if (k.is_string() && !k.get<std::string>().empty() && std::all_of(k.get<std::string>().begin(), k.get<std::string>().end(), [](char c) { return std::isdigit((unsigned char)c); }))
            out = std::stoi(k.get<std::string>());
        else if (k.is_number() && std::floor(k.get<double>()) != k.get<double>()) throw std::runtime_error("a key is a whole number");
        else out = parseKey(k);
    } catch (const std::exception &e) { err = w + ": " + e.what(); return false; }
    if (out < 0 || out > 127) { err = w + ": keys are 0-127"; return false; }
    return true;
}

bool rangeOf(const json &e, Range &r, const std::string &w, std::string &err) {
    if (!e.contains("range")) return true;
    const json &j = e["range"];
    if (!j.is_array() || j.size() != 2) { err = w + ": range is [low, high] keys, as numbers or names (\"C2\")"; return false; }
    return keyOf(j[0], r.lo, w + ": range", err) && keyOf(j[1], r.hi, w + ": range", err);
}

bool intervalsOf(const json &j, std::vector<int> &out, const std::string &w, std::string &err) {
    if (!j.is_array() || j.size() > 128) { err = w + ": a chord is a list of semitones from its key, [0, 4, 7]"; return false; }
    for (auto &x : j) {
        int i = 0;
        if (!integer(x, -127, 127, i)) { err = w + ": a chord's intervals are whole semitones (-127 to 127)"; return false; }
        out.push_back(i);
    }
    return true;
}

bool parseChord(const json &e, ChordFx &c, std::string &err) {
    const std::string w = "midiFx: chord";
    if (!knownKeys(e, {"type", "intervals", "chords", "range", "transpose"}, w, err)) return false;
    if (e.contains("intervals") == e.contains("chords")) {
        err = w + ": give \"intervals\": [0, 4, 7] (a chord on every note) or \"chords\": {\"C3\": [0, 4, 7], \"D3\": [0, 3, 7]} (a chord per key)";
        return false;
    }
    c.multi = e.contains("chords");
    if (!c.multi && !intervalsOf(e["intervals"], c.intervals, w, err)) return false;
    if (c.multi) {
        if (!e["chords"].is_object()) { err = w + ": chords maps keys to chords, {\"C3\": [0, 4, 7]}"; return false; }
        for (auto &[k, v] : e["chords"].items()) {
            int key = 0;
            if (!keyOf(json(k), key, w + ": chords", err) || !intervalsOf(v, c.chords[key], w, err)) return false;
        }
    }
    if (e.contains("transpose") && !integer(e["transpose"], -48, 48, c.transpose)) { err = w + ": transpose is -48 to 48 semitones"; return false; }
    return rangeOf(e, c.range, w, err);
}

// a pitch class: 0-11 or a note name without its octave ("Eb")
bool pitchClass(const json &j, int &pc) {
    if (j.is_number()) return integer(j, 0, 11, pc);
    if (!j.is_string()) return false;
    static const std::map<char, int> letters = {{'C', 0}, {'D', 2}, {'E', 4}, {'F', 5}, {'G', 7}, {'A', 9}, {'B', 11}};
    const std::string s = j.get<std::string>();
    if (s.empty() || !letters.count((char)std::toupper((unsigned char)s[0]))) return false;
    pc = letters.at((char)std::toupper((unsigned char)s[0]));
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] != '#' && s[i] != 'b') return false;
        pc += s[i] == '#' ? 1 : -1;
    }
    pc = ((pc % 12) + 12) % 12;
    return true;
}

bool parseTranspose(const json &e, TransposeFx &t, std::string &err) {
    const std::string w = "midiFx: transpose";
    if (!knownKeys(e, {"type", "semitones", "scale"}, w, err)) return false;
    if (e.contains("semitones") && !integer(e["semitones"], -48, 48, t.semitones)) { err = w + ": semitones is -48 to 48"; return false; }
    if (!e.contains("scale")) return true;
    if (!parseScale(e["scale"], t.scale, err)) { err = w + ": " + err; return false; }
    return true;
}

bool parseRepeat(const json &e, RepeatFx &r, std::string &err) {
    const std::string w = "midiFx: repeat";
    if (!knownKeys(e, {"type", "time", "ms", "repeats", "transpose", "ramp", "thru", "range"}, w, err)) return false;
    if (e.contains("time") && e.contains("ms")) { err = w + ": give time or ms, not both"; return false; }
    try {
        if (e.contains("time")) r.beats = e["time"].is_number() ? e["time"].get<double>() : Lfo::noteBeats(e["time"].get<std::string>());
    } catch (const std::exception &) { err = w + ": time is a note value (\"1/16\", \"1/8T\", \"1/8D\") or beats"; return false; }
    if (!(r.beats > 0 && r.beats <= 64)) { err = w + ": time is more than 0 and at most 64 beats"; return false; }
    if (e.contains("ms") && !(e["ms"].is_number() && e["ms"].get<double>() > 0 && e["ms"].get<double>() <= 60000)) { err = w + ": ms is 0 to 60000"; return false; }
    if (e.contains("ms")) r.ms = e["ms"].get<double>();
    if (e.contains("repeats") && !integer(e["repeats"], 0, 99, r.repeats)) { err = w + ": repeats is 0-99"; return false; }
    if (e.contains("transpose") && !integer(e["transpose"], -48, 48, r.transpose)) { err = w + ": transpose is -48 to 48 semitones a repeat"; return false; }
    if (e.contains("ramp")) {
        if (!e["ramp"].is_number() || !(e["ramp"].get<double>() >= 0.01 && e["ramp"].get<double>() <= 2)) { err = w + ": ramp is 0.01-2 (each repeat's velocity, times the one before)"; return false; }
        r.ramp = e["ramp"].get<double>();
    }
    if (e.contains("thru")) {
        if (!e["thru"].is_boolean()) { err = w + ": thru is true (the note plays too) or false (only its repeats)"; return false; }
        r.thru = e["thru"].get<bool>();
    }
    return rangeOf(e, r.range, w, err);
}

// ---- notes

struct Item { json n; double beat, dur, vel; int key; };   // a note with a beat and a key, read

bool split(const json &notes, std::vector<Item> &items, json &others, std::string &err) {
    others = json::array();
    try {
        for (auto &n : notes) {
            if (!n.is_object() || !n.contains("beat") || !n.contains("key")) { others.push_back(n); continue; }
            items.push_back({n, n["beat"].get<double>(), n.value("dur", 1.0), n.value("vel", 0.8), parseKey(n["key"])});
        }
    } catch (const std::exception &e) { err = std::string("midiFx: ") + e.what(); return false; }
    return true;
}

json joined(const std::vector<Item> &items, const json &others) {
    json out = json::array();
    for (auto &x : items) out.push_back(x.n);
    for (auto &n : others) out.push_back(n);
    return out;
}

int midiVel(double v) { return (int)std::clamp(std::nearbyint(v > 1 ? v : v * 127), 1.0, 127.0); }   // as the effect receives it

Item moved(const Item &x, int key) {
    Item c = x;
    c.key = key;
    c.n["key"] = key;
    return c;
}

// one note per key at a moment (MIDI can't strike a key twice at once): the louder, then the longer, stays
std::vector<Item> dedupe(const std::vector<Item> &items) {
    std::vector<Item> out;
    std::map<std::pair<long long, int>, size_t> at;
    for (auto &x : items) {
        const auto k = std::make_pair(std::llround(x.beat * 1e6), x.key);
        auto it = at.find(k);
        if (it == at.end()) { at[k] = out.size(); out.push_back(x); continue; }
        Item &b = out[it->second];
        if (std::make_pair(midiVel(x.vel), x.dur) > std::make_pair(midiVel(b.vel), b.dur)) b = x;
    }
    return out;
}

// a key struck again while it still sounds: the earlier note ends there
void cutRestruck(std::vector<Item> &items) {
    std::map<int, std::vector<size_t>> byKey;
    for (size_t i = 0; i < items.size(); ++i) byKey[items[i].key].push_back(i);
    for (auto &[k, list] : byKey) {
        std::stable_sort(list.begin(), list.end(), [&](size_t a, size_t b) { return items[a].beat < items[b].beat; });
        for (size_t i = 0; i + 1 < list.size(); ++i) {
            Item &a = items[list[i]];
            const Item &b = items[list[i + 1]];
            if (a.beat + a.dur > b.beat + 1e-9) { a.dur = std::max(1e-3, b.beat - a.beat); a.n["dur"] = r6(a.dur); }
        }
    }
}

bool inRange(int key, const Range &r, bool crossedInverts) {
    if (r.lo <= r.hi) return key >= r.lo && key <= r.hi;
    return crossedInverts ? !(key > r.hi && key < r.lo) : key >= r.hi && key <= r.lo;
}

std::vector<Item> chordStage(const std::vector<Item> &items, const ChordFx &c) {
    std::vector<Item> out;
    for (auto &x : items) {
        if (!inRange(x.key, c.range, false)) { out.push_back(x); continue; }   // outside the trigger range: as written
        static const std::vector<int> none;
        const auto it = c.chords.find(x.key);
        const std::vector<int> &ivs = !c.multi ? c.intervals : it == c.chords.end() ? none : it->second;   // an unmapped key is silent
        for (int i : ivs)
            if (const int k = x.key + i + c.transpose; k >= 0 && k <= 127) out.push_back(moved(x, k));
    }
    return dedupe(out);
}

// the nearest key in the scale (ties go down)
int snap(int key, const std::set<int> &scale) {
    if (scale.empty()) return key;
    for (int d = 0; d < 12; ++d)
        for (int k : {key - d, key + d})
            if (scale.count(((k % 12) + 12) % 12)) return k;
    return key;
}

std::vector<Item> transposeStage(const std::vector<Item> &items, const TransposeFx &t) {
    std::vector<Item> out;
    for (auto &x : items)
        if (const int k = snap(x.key + t.semitones, t.scale); k >= 0 && k <= 127) out.push_back(moved(x, k));
    return dedupe(out);
}

std::vector<Item> repeatStage(const std::vector<Item> &items, const RepeatFx &r, const SecondsToBeats &toBeats) {
    std::vector<Item> out;
    const double pct = r.ramp * 100;   // the plug-in's Velocity Ramp, in %
    for (auto &x : items) {
        const double D = r.ms < 0 ? r.beats : toBeats ? toBeats(x.beat, r.ms / 1000) : r.ms / 500;
        if (!inRange(x.key, r.range, true) || D <= 0) { out.push_back(x); continue; }
        if (r.thru) out.push_back(x);
        double v = midiVel(x.vel);
        for (int i = 1; i <= r.repeats; ++i) {
            v = std::clamp(v * pct / 100.0, 1.0, 127.0);   // each repeat at ramp x the one before
            const int k = x.key + i * r.transpose;
            if (k < 0 || k > 127) continue;   // past the MIDI range: dropped
            Item c = moved(x, k);
            c.beat = x.beat + i * D;
            c.vel = std::nearbyint(v) / 127;
            c.n["beat"] = r6(c.beat);
            c.n["vel"] = r4(c.vel);
            out.push_back(c);
        }
    }
    out = dedupe(out);
    cutRestruck(out);
    return out;
}

bool resolveOne(const json &e, json &out, std::vector<std::string> &notes, std::string &err) {
    if (!e.is_object() || !e.contains("type") || !e["type"].is_string()) {
        err = "midiFx: each effect is an object with a \"type\": chord, transpose, repeat or arp";
        return false;
    }
    const std::string type = e["type"].get<std::string>();
    if (type == "arp") {   // the arpeggiator resolves its own patch or preset
        json a = e, empty;
        a.erase("type");
        if (!resolveArp(a, out, notes, err) || !arpeggiate(json::array(), out, empty, err)) return false;
        out["type"] = "arp";
        return true;
    }
    if (type != "chord" && type != "transpose" && type != "repeat") { err = "midiFx: type is chord, transpose, repeat or arp (not '" + type + "')"; return false; }
    out = e;
    if (e.contains("patch") || e.contains("preset")) {
        const bool preset = e.contains("preset");
        if (preset && e.contains("patch")) { err = "midiFx " + type + ": give a patch or a preset, not both"; return false; }
        const json &name = preset ? e["preset"] : e["patch"];
        if (!name.is_string()) { err = "midiFx " + type + ": " + (preset ? "preset" : "patch") + " is a name"; return false; }
        json base;
        if (preset) {
            if (!appleMidiFxPreset(type, name.get<std::string>(), base, notes, err)) { err = "midiFx " + type + ": " + err; return false; }
        } else {
            json chain;
            std::vector<std::string> ignored;
            if (!appleMidiFx(name.get<std::string>(), chain, ignored, err)) { err = "midiFx " + type + ": " + err; return false; }
            for (auto &x : chain) if (x.value("type", "") == type) { base = x; break; }
            if (base.is_null()) { err = "midiFx " + type + ": the patch '" + name.get<std::string>() + "' has no " + appleName(type) + " switched on"; return false; }
        }
        for (auto &[k, v] : e.items()) {
            if (k == "patch" || k == "preset") continue;
            if (k == "intervals") base.erase("chords");   // a chord of its own replaces the preset's
            if (k == "chords") base.erase("intervals");
            base[k] = v;
        }
        out = base;
    }
    for (auto it = out.begin(); it != out.end();) it = it->is_null() ? out.erase(it) : std::next(it);   // null = the setting left out
    ChordFx c;
    TransposeFx t;
    RepeatFx r;
    return type == "chord" ? parseChord(out, c, err) : type == "transpose" ? parseTranspose(out, t, err) : parseRepeat(out, r, err);
}
} // namespace

bool parseScale(const json &s, std::set<int> &scale, std::string &err) {
    scale.clear();
    if (s.is_string()) {   // a key: "A minor", "D dorian", "C minor pentatonic"
        std::string name = s.get<std::string>(), low = name;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const bool penta = low.size() > 10 && low.compare(low.size() - 10, 10, "pentatonic") == 0;
        if (penta) name.resize(name.size() - 10);
        int tonic = 0, mode = 0;
        bool minor = false;
        if (!parseKeyName(name, tonic, minor, err, &mode)) { err = "scale: " + err + " (or a list of notes, [\"C\", \"Eb\", \"G\"])"; return false; }
        static const std::vector<int> modes[6] = {{0, 2, 4, 5, 7, 9, 11}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 3, 5, 7, 9, 10},
                                                  {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10}};
        if (penta && mode > 1) { err = "a pentatonic scale is major or minor"; return false; }
        const std::vector<int> steps = penta ? (mode == 1 ? std::vector<int>{0, 3, 5, 7, 10} : std::vector<int>{0, 2, 4, 7, 9}) : modes[mode];
        for (int x : steps) scale.insert((tonic + x) % 12);
        return true;
    }
    if (!s.is_array() || s.empty()) { err = "scale is a key (\"A minor\", \"C minor pentatonic\") or a list of notes, [0, 3, 7] or [\"C\", \"Eb\", \"G\"]"; return false; }
    for (auto &x : s) {
        int pc = 0;
        if (!pitchClass(x, pc)) { err = "a scale's notes are 0-11 (C = 0) or names without an octave (\"Eb\")"; return false; }
        scale.insert(pc);
    }
    if (scale.size() == 12) scale.clear();
    return true;
}

bool resolveMidiFx(const json &fx, json &out, std::vector<std::string> &notes, std::string &err) {
    if (!fx.is_array()) {
        err = "\"midiFx\" is a list of MIDI effects, [{\"type\": \"chord\", \"intervals\": [0, 4, 7]}, {\"type\": \"repeat\", \"time\": \"1/8\"}], or false";
        return false;
    }
    out = json::array();
    for (auto &e : fx) {
        json r;
        if (!resolveOne(e, r, notes, err)) return false;
        out.push_back(r);
    }
    return true;
}

bool runMidiFx(const json &notes, const json &chain, json &out, std::string &err, const SecondsToBeats &toBeats) {
    json cur = notes;
    for (auto &e : chain) {
        const std::string type = e.is_object() ? e.value("type", "") : "";
        if (type == "arp") {
            json a = e, next;
            a.erase("type");
            if (!arpeggiate(cur, a, next, err)) return false;
            cur = std::move(next);
            continue;
        }
        std::vector<Item> items;
        json others;
        if (!split(cur, items, others, err)) return false;
        if (type == "chord") {
            ChordFx c;
            if (!parseChord(e, c, err)) return false;
            items = chordStage(items, c);
        } else if (type == "transpose") {
            TransposeFx t;
            if (!parseTranspose(e, t, err)) return false;
            items = transposeStage(items, t);
        } else if (type == "repeat") {
            RepeatFx r;
            if (!parseRepeat(e, r, err)) return false;
            items = repeatStage(items, r, toBeats);
        } else { err = "midiFx: type is chord, transpose, repeat or arp"; return false; }
        cur = joined(items, others);
    }
    std::vector<Item> items;
    json others;
    if (!split(cur, items, others, err)) return false;
    std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.beat != b.beat ? a.beat < b.beat : a.key < b.key; });
    out = joined(items, others);
    return true;
}

std::string midiFxSummary(const json &e, bool apple) {
    const std::string type = e.is_object() ? e.value("type", "") : "";
    auto signedNum = [](int v) { return (v > 0 ? "+" : "") + std::to_string(v); };
    auto semitones = [](int v) { return std::to_string(v) + (std::abs(v) == 1 ? " semitone" : " semitones"); };
    auto range = [&](const char *what, int lo = 21, int hi = 108) -> std::string {   // shown when narrower than [lo, hi]
        Range r;
        std::string ignored;
        if (!rangeOf(e, r, "", ignored) || (r.lo <= lo && r.hi >= hi)) return "";
        return std::string(", ") + what + " " + keyName(r.lo) + " to " + keyName(r.hi);
    };
    std::string s;
    if (type == "arp") s = arpSummary(e);
    else if (type == "chord") {
        if (e.contains("chords") && e["chords"].is_object()) {
            int lo = 128, hi = -1;
            std::string ignored;
            for (auto &[k, v] : e["chords"].items()) { int key = 0; if (keyOf(json(k), key, "", ignored)) lo = std::min(lo, key), hi = std::max(hi, key); }
            s = std::to_string(e["chords"].size()) + " chords" + (hi >= lo ? " on keys " + keyName(lo) + " to " + keyName(hi) : "");
            s += hi >= lo ? range("played for keys", lo, hi) : "";
        } else {
            s = "a chord of";
            if (e.contains("intervals") && e["intervals"].is_array())
                for (auto &i : e["intervals"]) s += " " + (i.is_number() ? signedNum((int)i.get<double>()) : "?");
            s += " on every note" + range("for keys");
        }
        if (const int t = e.value("transpose", 0); t != 0) s += ", " + semitones(std::abs(t)) + (t > 0 ? " up" : " down");
    } else if (type == "transpose") {
        TransposeFx t;
        std::string ignored;
        parseTranspose(e, t, ignored);
        if (t.semitones) s = std::string(t.semitones > 0 ? "+" : "") + semitones(t.semitones);
        if (!t.scale.empty()) {
            s += s.empty() ? "onto" : ", onto";
            for (int pc : t.scale) s += " " + pcName(pc);
        }
        if (s.empty()) s = "no change";
    } else if (type == "repeat") {
        RepeatFx r;
        std::string ignored;
        parseRepeat(e, r, ignored);
        const std::string time = e.contains("ms") ? std::to_string((int)std::lround(r.ms)) + " ms" : e.contains("time") && e["time"].is_string() ? e["time"].get<std::string>() : r4(r.beats) == 0.5 && !e.contains("time") ? "1/8" : json(r4(r.beats)).dump() + " beats";
        s = std::to_string(r.repeats) + (r.repeats == 1 ? " repeat " : " repeats ") + time + " apart";
        if (r.transpose) s += std::string(", ") + (r.transpose > 0 ? "+" : "") + semitones(r.transpose) + " each";
        if (r.ramp != 1) s += ", velocity x" + json(r4(r.ramp)).dump() + " each";
        if (!r.thru) s += ", without the note itself";
        s += range("keys");
    } else s = type;
    return apple ? appleName(type) + " (" + s + ")" : s;
}

} // namespace wl
