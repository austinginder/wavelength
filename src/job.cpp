#include "job.hpp"
#include "harmony.hpp"

#include "platform.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace wl {

TempoMap::TempoMap(std::vector<TempoPoint> points) : pts_(std::move(points)) {
    if (pts_.empty()) pts_.push_back({0, 120});
    std::sort(pts_.begin(), pts_.end(), [](auto &a, auto &b) { return a.beat < b.beat; });
    if (pts_.front().beat > 0) pts_.insert(pts_.begin(), {0, pts_.front().bpm});
    pts_.front().ramp = false;
    secAt_.resize(pts_.size());
    secAt_[0] = 0;
    for (size_t i = 1; i < pts_.size(); ++i) secAt_[i] = secAt_[i - 1] + segSec(i - 1, pts_[i].beat - pts_[i - 1].beat);
}

// seconds spent in segment i (from point i towards point i+1) over `db` beats
double TempoMap::segSec(size_t i, double db) const {
    const double a = pts_[i].bpm;
    if (i + 1 >= pts_.size() || !pts_[i + 1].ramp) return db * 60.0 / a;
    const double k = (pts_[i + 1].bpm - a) / std::max(1e-9, pts_[i + 1].beat - pts_[i].beat);   // bpm per beat
    if (std::fabs(k) < 1e-9) return db * 60.0 / a;
    return 60.0 / k * std::log((a + k * db) / a);
}

double TempoMap::beatToSec(double beat) const {
    size_t i = pts_.size() - 1;
    while (i > 0 && pts_[i].beat > beat) --i;
    return secAt_[i] + segSec(i, beat - pts_[i].beat);
}

double TempoMap::secToBeat(double sec) const {
    size_t i = pts_.size() - 1;
    while (i > 0 && secAt_[i] > sec) --i;
    const double a = pts_[i].bpm, ds = sec - secAt_[i];
    if (i + 1 < pts_.size() && pts_[i + 1].ramp) {
        const double k = (pts_[i + 1].bpm - a) / std::max(1e-9, pts_[i + 1].beat - pts_[i].beat);
        if (std::fabs(k) >= 1e-9) return pts_[i].beat + a * (std::exp(ds * k / 60.0) - 1) / k;
    }
    return pts_[i].beat + ds * a / 60.0;
}

double TempoMap::bpmAtBeat(double beat) const {
    size_t i = pts_.size() - 1;
    while (i > 0 && pts_[i].beat > beat) --i;
    if (i + 1 < pts_.size() && pts_[i + 1].ramp)
        return pts_[i].bpm + (pts_[i + 1].bpm - pts_[i].bpm) * (beat - pts_[i].beat) / std::max(1e-9, pts_[i + 1].beat - pts_[i].beat);
    return pts_[i].bpm;
}

int parseKey(const json &k) {
    if (k.is_number()) return k.get<int>();
    if (!k.is_string()) throw std::runtime_error("note key must be a number or a name like \"C4\"");
    std::string s = k.get<std::string>();
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    if (s.empty() || s[0] < 'A' || s[0] > 'G') throw std::runtime_error("bad note name '" + s + "'");
    int semis = base[s[0] - 'A'];
    size_t i = 1;
    while (i < s.size() && (s[i] == '#' || s[i] == 'b')) semis += s[i++] == '#' ? 1 : -1;
    if (i >= s.size()) throw std::runtime_error("note name '" + s + "' needs an octave, e.g. C4");
    int octave = std::stoi(s.substr(i));
    return 12 * (octave + 1) + semis;
}

namespace {

std::string keyName(int k) {
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[((k % 12) + 12) % 12]) + std::to_string(k / 12 - 1);
}

// After a track's notes are parsed: range warnings, keyswitch notes for articulation
// changes, and velocity-driven controller curves ("velocityTo").
// Gain curves (automation.gain and rides) that start late hold their first value from the top of the song:
// warn when that value is not 0 dB and something sounds through this fader before the curve begins.
void lateGainWarnings(const json &au, double firstSound, std::vector<std::string> &warnings) {
    double fb, fv;
    auto check = [&](const std::string &what, const json &c) {
        if (firstPoint(c, fb, fv) && fb > 0 && fb > firstSound + 1e-6 && std::fabs(fv) > 1e-9) warnings.push_back(lateCurveWarning(what, fb, fv, 0));
    };
    if (au.contains("gain")) check("gain", au["gain"]);
    if (au.contains("rides")) {
        const auto &r = au["rides"];
        const bool named = r.is_object() && !r.contains("points") && !r.contains("value");
        if (named) for (auto &[k, v] : r.items()) check("ride '" + k + "'", v);
        else check("rides", r);
    }
}

void finishTrackNotes(const json &t, const TempoMap &tempo, Track &tr, const std::vector<int> &noteArt) {
    const size_t played = tr.notes.size();
    if (t.contains("range")) {
        const auto &r = t["range"];
        if (!r.is_array() || r.size() != 2) throw std::runtime_error("track '" + tr.name + "': \"range\" must be [lowest, highest]");
        const int lo = parseKey(r[0]), hi = parseKey(r[1]);
        size_t outside = 0;
        double first = -1;
        for (const auto &n : tr.notes)
            if (n.key < lo || n.key > hi) { if (!outside++) first = n.start; }
        if (outside)
            tr.warnings.push_back(std::to_string(outside) + " of " + std::to_string(played) + " notes outside the playable range " + keyName(lo) +
                                  "-" + keyName(hi) + " (first at beat " + std::to_string((int)std::floor(tempo.secToBeat(first))) +
                                  "); the instrument will likely be silent for them");
    }
    if (!t.contains("velocityTo") && std::any_of(tr.notes.begin(), tr.notes.end(), [](const Note &n) { return !n.dyn.empty(); }))
        tr.warnings.push_back("notes have \"dyn\" but the track has no \"velocityTo\" (the controller that sets loudness, e.g. "
                              "{\"param\": \"Dynamics\"} for BBC SO); dyn is ignored");
    if (t.contains("velocityTo")) {   // {"param": "Dynamics"} or {"cc": 1}, with "min"/"max" output values
        const json &v = t["velocityTo"];
        const bool cc = v.contains("cc");
        if (!cc && !v.contains("param")) throw std::runtime_error("track '" + tr.name + "': \"velocityTo\" needs \"param\" or \"cc\"");
        const double lo = v.value("min", cc ? 10.0 : 0.05), hi = v.value("max", cc ? 127.0 : 1.0);
        std::vector<std::pair<double, double>> onsets;   // (beat, level): one per start, or a note's own "dyn" curve
        for (size_t i = 0; i < played; ++i) {
            const Note &n = tr.notes[i];
            if (!n.dyn.empty()) {
                for (auto &[t, v] : n.dyn) onsets.push_back({std::round(tempo.secToBeat(n.start + t) * 1000) / 1000, v});
                continue;
            }
            onsets.push_back({std::round(tempo.secToBeat(n.start) * 1000) / 1000, n.velocity});
        }
        std::stable_sort(onsets.begin(), onsets.end(), [](auto &a, auto &b) { return a.first < b.first; });
        {   // one value per beat position: the loudest (a chord's notes start together)
            std::vector<std::pair<double, double>> merged;
            for (auto &o : onsets)
                if (!merged.empty() && merged.back().first == o.first) merged.back().second = std::max(merged.back().second, o.second);
                else merged.push_back(o);
            onsets.swap(merged);
        }
        json pts = json::array();
        for (auto &[b, vel] : onsets) pts.push_back({b, lo + (hi - lo) * vel});
        if (pts.empty()) pts.push_back({0.0, hi});
        Envelope env = Envelope::parse(pts, tempo, false);
        if (cc) {
            const int num = v["cc"].get<int>();
            bool taken = false;
            for (auto &[n, e] : tr.ccAutomation) taken |= n == num;
            if (taken) tr.warnings.push_back("velocityTo: CC " + std::to_string(num) + " already has automation; using that instead");
            else tr.ccAutomation.push_back({num, env});
        } else {
            const std::string name = v["param"].get<std::string>();
            bool taken = false;
            for (auto &[n, e] : tr.paramAutomation) taken |= n == name;
            if (taken) tr.warnings.push_back("velocityTo: '" + name + "' already has automation; using that instead");
            else tr.paramAutomation.push_back({name, env});
        }
    }
    // keyswitches: a short note just before the first note of each articulation change
    std::vector<size_t> order(played);
    for (size_t i = 0; i < played; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tr.notes[a].start < tr.notes[b].start; });
    int current = -1;
    double lastSwitch = -1;
    for (size_t i : order) {
        const int art = noteArt[i];
        if (art < 0 || art == current) continue;
        if (art > 127) throw std::runtime_error("track '" + tr.name + "': keyswitch key " + std::to_string(art) + " is out of range 0-127");
        Note ks{};
        ks.start = std::max({0.0, tr.notes[i].start - 0.03, lastSwitch + 0.005});
        ks.length = 0.02;
        ks.key = art;
        ks.channel = tr.notes[i].channel;
        ks.velocity = 0.5;
        tr.notes.push_back(ks);
        lastSwitch = ks.start;
        current = art;
    }
}

// ---- chord-following curves ------------------------------------------------------------------
// {"follow": "root" | "third" | "fifth" | "seventh", "octave": 4 | "from": "A2", "transpose": 0,
//  "as": "note" | "hz" | "midi", "curve": "switch" | "step", "ramp": ms}: a step curve through the chords
bool isFollow(const json &o) { return o.is_object() && o.contains("follow") && o["follow"].is_string(); }

bool hasFollow(const json &j) {
    if (isFollow(j)) return true;
    if (j.is_object()) { for (auto &[k, v] : j.items()) if (k != "notes" && hasFollow(v)) return true; }
    else if (j.is_array()) for (auto &v : j) if (!v.is_number() && hasFollow(v)) return true;
    return false;
}

void replaceFollow(json &j, const std::function<json(const json &)> &f) {
    if (isFollow(j)) { j = f(j); return; }
    if (j.is_object()) { for (auto &[k, v] : j.items()) if (k != "notes") replaceFollow(v, f); }
    else if (j.is_array()) for (auto &v : j) if (!v.is_number()) replaceFollow(v, f);
}

// "chords": [[beat, "C#m"], ...] or [{"bar": 9, "chord": "C#m"}, {"beat": 36, "chord": "A"}]
std::vector<std::pair<double, ChordTones>> parseChordList(const json &list, double beatsPerBar) {
    if (!list.is_array()) throw std::runtime_error("\"chords\" is a list: [[beat, \"C#m\"], ...] or [{\"bar\": 1, \"chord\": \"C#m\"}, ...]");
    std::vector<std::pair<double, ChordTones>> out;
    for (const auto &c : list) {
        double beat;
        std::string name;
        if (c.is_array() && c.size() == 2 && c[0].is_number() && c[1].is_string()) { beat = c[0].get<double>(); name = c[1].get<std::string>(); }
        else if (c.is_object() && c.contains("chord") && (c.contains("bar") || c.contains("beat"))) {
            beat = c.contains("bar") ? (c["bar"].get<double>() - 1) * beatsPerBar : c["beat"].get<double>();
            name = c["chord"].get<std::string>();
        } else throw std::runtime_error("each \"chords\" entry is [beat, \"C#m\"] or {\"bar\": 9, \"chord\": \"C#m\"}");
        if (name == "-" || name == "N.C." || name == "NC") continue;   // no chord: the previous one holds
        ChordTones t;
        std::string err;
        if (!parseChordName(name, t, err)) throw std::runtime_error("\"chords\": " + err);
        out.push_back({beat, t});
    }
    std::stable_sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.first < b.first; });
    return out;
}

json followCurve(const json &spec, const std::vector<std::pair<double, ChordTones>> &chords) {
    const std::string tone = spec["follow"].get<std::string>();
    if (tone != "root" && tone != "third" && tone != "fifth" && tone != "seventh")
        throw std::runtime_error("\"follow\" is \"root\", \"third\", \"fifth\" or \"seventh\" (of the job's \"chords\")");
    if (chords.empty()) throw std::runtime_error("\"follow\": the job has no \"chords\" and none could be read from its notes");
    int lowest = 12 * (spec.value("octave", 4) + 1);   // the tone lands in [lowest, lowest + 12)
    if (spec.contains("from")) lowest = parseKey(spec["from"]);
    const int transpose = spec.value("transpose", 0);
    const std::string as = spec.value("as", std::string("note"));
    if (as != "note" && as != "hz" && as != "midi") throw std::runtime_error("\"follow\" curves give \"as\": \"note\" (default), \"hz\" or \"midi\"");
    const std::string curve = spec.value("curve", std::string("switch"));
    if (curve != "switch" && curve != "step") throw std::runtime_error("\"follow\" curves are \"switch\" (default) or \"step\"");
    json out = spec;
    for (const char *k : {"follow", "octave", "from", "transpose", "as"}) out.erase(k);
    out["curve"] = curve;
    json pts = json::array();
    json last;
    for (size_t i = 0; i < chords.size(); ++i) {
        const ChordTones &c = chords[i].second;
        int iv = tone == "third" ? c.third : tone == "fifth" ? c.fifth : tone == "seventh" ? c.seventh : 0;
        if (iv < 0) iv = 0;   // a tone the chord doesn't have (the third of a power chord): the root
        const int pc = (c.root + iv) % 12;
        int key = lowest + ((pc - lowest) % 12 + 12) % 12 + transpose;
        json v;
        if (as == "midi") v = key;
        else if (as == "hz") v = std::round(440.0 * std::pow(2.0, (key - 69) / 12.0) * 100) / 100;
        else v = keyName(key);
        if (v == last) continue;
        last = v;
        pts.push_back({i == 0 ? 0.0 : chords[i].first, v});
    }
    out["points"] = pts;
    return out;
}

} // namespace

json userJobDefaults(std::string *path) {
    std::string p;
    if (const char *env = getenv("WAVELENGTH_DEFAULTS")) p = env;
    else p = (platform::dataDir() / "defaults.json").string();
    if (path) *path = p;
    std::ifstream in(p);
    if (!in) return json::object();
    const json d = json::parse(in, nullptr, false);
    return d.is_object() ? d : json::object();
}

bool parseJob(const json &jobIn, const std::string &baseDir, Job &out, std::string &err, bool useDefaults) {
    // the user's defaults fill in top-level settings the job leaves out
    json j = jobIn;
    if (useDefaults) {
        const json defaults = userJobDefaults();   // named: items() must not outlive its object
        for (auto &[k, v] : defaults.items())
            if (!j.contains(k)) { j[k] = v; out.appliedDefaults.push_back(k); }
    }
    try {
        // curves that follow the chords ({"follow": "root", "octave": 4}) become step curves first: from the
        // job's "chords", or the chords lint --harmony reads from the notes
        if (hasFollow(j)) {
            const double bpb = j.contains("timeSignature") ? j["timeSignature"][0].get<double>() * 4 / j["timeSignature"][1].get<double>() : 4.0;
            std::vector<std::pair<double, ChordTones>> chords;
            if (j.contains("chords")) chords = parseChordList(j["chords"], bpb);
            else {
                json plain = j;
                replaceFollow(plain, [](const json &) { return json{{"value", 1.0}}; });
                Job notes;
                std::string perr;
                if (!parseJob(plain, baseDir, notes, perr, false)) throw std::runtime_error(perr);
                json detected = json::array();
                for (auto &[b, name] : detectChords(notes)) detected.push_back({b, name});
                chords = parseChordList(detected, bpb);
            }
            replaceFollow(j, [&](const json &spec) { return followCurve(spec, chords); });
        } else if (j.contains("chords")) parseChordList(j["chords"], 4);   // still check it
        out.baseDir = baseDir;
        out.parallel = j.value("parallel", -1);
        out.retries = std::clamp(j.value("retries", 2), 0, 10);
        out.sampleRate = j.value("sampleRate", 48000);
        out.blockSize = j.value("blockSize", 512);
        out.tail = j.value("tail", 3.0);
        out.warmup = j.value("warmup", 0.4);
        out.length = j.value("length", 0.0);
        out.leadIn = j.value("leadIn", 0.0);
        if (out.leadIn < 0 || out.leadIn > 30) throw std::runtime_error("\"leadIn\" is seconds of silence before the song, 0 to 30");
        if (j.contains("normalize") && !j["normalize"].is_null()) { out.hasNormalize = true; out.normalizeDb = j["normalize"].get<double>(); }
        if (j.contains("timeSignature")) { out.tsigNum = j["timeSignature"][0]; out.tsigDen = j["timeSignature"][1]; }

        std::vector<TempoPoint> tp;
        if (!j.contains("tempo") || j["tempo"].is_number()) tp.push_back({0, j.value("tempo", 120.0)});
        else for (auto &p : j["tempo"]) tp.push_back({p.value("beat", 0.0), p.at("bpm").get<double>(), p.value("ramp", false)});
        out.tempo = TempoMap(tp);

        if (!j.contains("tracks") || !j["tracks"].is_array() || j["tracks"].empty()) { err = "job needs a non-empty \"tracks\" array"; return false; }
        for (auto &t : j["tracks"]) {
            Track tr;
            tr.name = t.value("name", "track" + std::to_string(out.tracks.size() + 1));
            tr.plugin = t.at("plugin").get<std::string>();
            tr.gainDb = t.value("gain", 0.0);
            tr.pan = std::clamp(t.value("pan", 0.0), -1.0, 1.0);
            tr.mute = t.value("mute", false);
            tr.stem = t.value("stem", true);
            tr.harmony = t.value("harmony", true);
            tr.warmup = t.value("warmup", -1.0);
            tr.preset = t.value("preset", "");
            if (t.contains("state") && !t["state"].is_null()) {
                const auto &s = t["state"];
                tr.stateFile = s.is_string() ? s.get<std::string>() : s.at("file").get<std::string>();
                tr.stateFormat = s.is_object() ? s.value("format", "auto") : "auto";
                if (fs::path(tr.stateFile).is_relative()) tr.stateFile = (fs::path(baseDir) / tr.stateFile).string();
            }
            if (t.contains("sampler")) tr.sampler = t["sampler"];
            if (t.contains("shepard")) tr.shepard = t["shepard"];
            if (t.contains("clips")) {
                if (!t["clips"].is_array()) throw std::runtime_error("track '" + tr.name + "': \"clips\" must be an array");
                tr.clips = t["clips"];
            }
            {   // when the track first makes sound: automation that starts late is only audible if something plays before it
                for (auto &n : t.value("notes", json::array())) {
                    double b = 1e18;
                    if (n.contains("beat") && n["beat"].is_number()) b = n["beat"].get<double>();
                    else if (n.contains("time") && n["time"].is_number()) b = out.tempo.secToBeat(n["time"].get<double>());
                    tr.firstSoundBeat = std::min(tr.firstSoundBeat, b);
                }
                for (auto &c : t.value("clips", json::array()))
                    tr.firstSoundBeat = std::min(tr.firstSoundBeat, c.contains("beat") ? c["beat"].get<double>() : 0.0);
            }
            if (t.contains("params"))
                for (auto &[k, v] : t["params"].items()) tr.params.push_back(v.is_string() ? ParamSetting{k, 0, v.get<std::string>()} : ParamSetting{k, v.get<double>(), ""});
            if (t.contains("fx")) {
                if (!t["fx"].is_array()) throw std::runtime_error("track '" + tr.name + "': \"fx\" must be an array of effects");
                tr.fx = t["fx"];
            }
            const json sends = t.value("sends", json::object());   // named: items() must not outlive its object
            for (auto &[bus, db] : sends.items()) {
                if (db.is_array()) {   // automated send: [[beat, dB], ...]
                    tr.sendAutomation.push_back({bus, Envelope::parse(db, out.tempo, false)});
                    tr.sends.push_back({bus, 0.0});
                } else tr.sends.push_back({bus, db.get<double>()});
            }
            tr.output = t.value("output", "");
            tr.bendRange = t.value("bendRange", 2.0);
            if (t.contains("automation")) {
                const auto &au = t["automation"];
                if (au.contains("gain")) tr.gainAutomation = Envelope::parse(au["gain"], out.tempo, false);
                if (au.contains("rides")) addRides(tr.gainAutomation, au["rides"], out.tempo);
                if (au.contains("pan")) tr.panAutomation = Envelope::parse(au["pan"], out.tempo, false);
                if (au.contains("pitchbend")) tr.bendAutomation = Envelope::parse(au["pitchbend"], out.tempo, false);
                if (au.contains("pressure")) tr.pressureAutomation = Envelope::parse(au["pressure"], out.tempo, false);
                const json ccs = au.value("cc", json::object());
                for (auto &[k, v] : ccs.items()) {
                    const int num = std::stoi(k);
                    if (num < 0 || num > 127) throw std::runtime_error("track '" + tr.name + "': MIDI CC numbers are 0-127");
                    tr.ccAutomation.push_back({num, Envelope::parse(v, out.tempo, false)});
                }
                const json autoParams = au.value("params", json::object());
                for (auto &[k, v] : autoParams.items()) tr.paramAutomation.push_back({k, Envelope::parse(v, out.tempo, false, true)});
                double fb, fv;   // curves that start late hold their first value from the top of the song
                lateGainWarnings(au, tr.firstSoundBeat, tr.warnings);
                if (au.contains("pan") && firstPoint(au["pan"], fb, fv) && fb > 0 && fb > tr.firstSoundBeat + 1e-6 && std::fabs(fv - tr.pan) > 1e-9)
                    tr.warnings.push_back(lateCurveWarning("pan", fb, fv, tr.pan));
                for (auto &[k, v] : autoParams.items())
                    for (auto &ps : tr.params)
                        if (ps.key == k && ps.text.empty() && firstPoint(v, fb, fv) && fb > 0 && fb > tr.firstSoundBeat + 1e-6 && std::fabs(fv - ps.value) > 1e-9)
                            tr.warnings.push_back(lateCurveWarning("parameter '" + k + "'", fb, fv, ps.value));
            }
            // groove: the job's settings with the track's on top
            json groove = j.value("groove", json::object());
            if (t.contains("groove")) for (auto &[k, v] : t["groove"].items()) groove[k] = v;
            const double swing = std::clamp(groove.value("swing", 0.5), 0.0, 0.95), grid = groove.value("grid", 0.25);
            const double lay = groove.value("lay", 0.0);
            const json human = groove.value("humanize", json::object());
            const double hTime = human.value("time", 0.0), hVel = human.value("vel", 0.0);
            std::mt19937 rng((uint32_t)human.value("seed", (int)(std::hash<std::string>{}(tr.name) & 0x7fffffff)));
            std::normal_distribution<double> gauss(0.0, 1.0);
            const double roll = t.value("roll", 0.0);
            const int transpose = t.value("transpose", 0);   // semitones added to every note (octave-off presets, key changes)   // beats between notes that start together (strum)
            std::map<long, int> rolled;                  // start tick -> notes already rolled there
            // articulations: name -> keyswitch key, sent just before the notes that change it
            std::map<std::string, int> arts;
            const json artMap = t.value("articulations", json::object());
            for (auto &[name, key] : artMap.items()) arts[name] = parseKey(key);
            std::vector<int> noteArt;                    // keyswitch per note (-1 = none)
            std::vector<json> ordered;
            for (auto &n : t.value("notes", json::array())) ordered.push_back(n);
            if (roll != 0)   // strum from the lowest note up (or down for negative roll)
                std::stable_sort(ordered.begin(), ordered.end(), [](const json &a, const json &b) {
                    return parseKey(a.at("key")) < parseKey(b.at("key"));
                });
            bool bendWarned = false;
            for (auto &n : ordered) {
                Note note;
                double v = n.value("vel", 0.8);
                if (n.contains("time")) {
                    note.start = n["time"].get<double>();
                    note.length = n.value("length", 0.5);
                } else {
                    double b = n.at("beat").get<double>(), d = n.value("dur", 1.0);
                    const double orig = b;
                    if (swing != 0.5 && grid > 0) {   // push the off-grid step of every pair later
                        const double pos = b / grid, step = std::round(pos);
                        if (std::fabs(pos - step) < 0.02 && ((long)step & 1)) b += (swing - 0.5) * 2 * grid;
                    }
                    b += lay;
                    if (hTime > 0) b += gauss(rng) * hTime;
                    if (roll != 0) {
                        const long tick = std::lround(orig * 960);
                        const int k = rolled[tick]++;
                        b += (roll > 0 ? roll : -roll) * k;
                    }
                    b = std::max(0.0, b);
                    if (roll != 0) d = std::max(0.01, d - (b - orig));   // strummed notes still end together
                    note.start = out.tempo.beatToSec(b);
                    note.length = out.tempo.beatToSec(b + d) - note.start;
                }
                note.key = parseKey(n.at("key")) + transpose;
                if (n.contains("bend")) {   // [[beats after the note start, semitones], ...]
                    const double b0 = out.tempo.secToBeat(note.start);
                    for (auto &p : n["bend"]) note.bend.push_back({out.tempo.beatToSec(b0 + p.at(0).get<double>()) - note.start, p.at(1).get<double>()});
                    std::sort(note.bend.begin(), note.bend.end());
                }
                if (n.contains("vibrato")) {   // depth in semitones, or {"depth", "rate" Hz, "delay" beats, "rise" beats}
                    const auto &vb = n["vibrato"];
                    const double depth = vb.is_number() ? vb.get<double>() : vb.value("depth", 0.3);
                    const double rate = vb.is_number() ? 5.5 : vb.value("rate", 5.5);
                    const double b0 = out.tempo.secToBeat(note.start);
                    const double delayBeats = vb.is_number() ? 0.25 : vb.value("delay", 0.25);
                    const double riseBeats = vb.is_number() ? 0.25 : vb.value("rise", 0.25);
                    const double delay = out.tempo.beatToSec(b0 + delayBeats) - note.start;
                    const double rise = std::max(1e-3, out.tempo.beatToSec(b0 + delayBeats + riseBeats) - note.start - delay);
                    if (depth < 0 || depth > 2 || rate <= 0 || rate > 20)
                        throw std::runtime_error("track '" + tr.name + "': vibrato depth is 0-2 semitones and rate 0-20 Hz");
                    auto base = [&](double t) {   // the note's own bend curve (piecewise linear)
                        const auto &b = note.bend;
                        if (b.empty()) return 0.0;
                        if (t <= b.front().first) return b.front().second;
                        for (size_t i = 1; i < b.size(); ++i)
                            if (t < b[i].first) return b[i - 1].second + (b[i].second - b[i - 1].second) * (t - b[i - 1].first) / (b[i].first - b[i - 1].first);
                        return b.back().second;
                    };
                    std::vector<std::pair<double, double>> curve;
                    const double step = 1.0 / (rate * 12);   // 12 points per cycle
                    for (double t = 0; t <= note.length + 0.25; t += step) {
                        const double amount = t <= delay ? 0 : std::min(1.0, (t - delay) / rise);
                        curve.push_back({t, base(t) + depth * amount * std::sin(2 * M_PI * rate * std::max(0.0, t - delay))});
                    }
                    note.bend = std::move(curve);
                }
                if (n.contains("dyn")) {   // [start, end] over the note, or [[beats after the start, 0..1], ...]
                    const auto &d = n["dyn"];
                    const double b0 = out.tempo.secToBeat(note.start);
                    auto at = [&](double beats) { return out.tempo.beatToSec(b0 + beats) - note.start; };
                    if (d.is_array() && d.size() == 2 && d[0].is_number() && d[1].is_number())
                        note.dyn = {{0.0, d[0].get<double>()}, {note.length, d[1].get<double>()}};
                    else if (d.is_array())
                        for (auto &p : d) note.dyn.push_back({at(p.at(0).get<double>()), p.at(1).get<double>()});
                    else throw std::runtime_error("track '" + tr.name + "': note \"dyn\" is [start, end] or [[beats, level], ...] with levels 0-1");
                    for (auto &[t, v] : note.dyn) {
                        if (v > 1.0) v /= 127.0;   // MIDI-style values
                        v = std::clamp(v, 0.0, 1.0);
                    }
                    std::sort(note.dyn.begin(), note.dyn.end());
                }
                if (!note.bend.empty() && tr.plugin != "builtin:sampler" && !bendWarned) {
                    bendWarned = true;
                    tr.warnings.push_back("per-note \"bend\" and \"vibrato\" only play on builtin:sampler tracks; use automation.pitchbend for plugins");
                }
                if (hVel > 0) v = (v > 1.0 ? v / 127.0 : v) * (1 + gauss(rng) * hVel);
                note.velocity = std::clamp(v > 1.0 ? v / 127.0 : v, 0.0, 1.0);
                note.channel = n.value("channel", 0);
                if (note.key < 0 || note.key > 127) throw std::runtime_error("note key out of range 0-127 in track '" + tr.name + "'");
                int art = -1;
                if (n.contains("art") && !n["art"].is_null()) {
                    const auto &a = n["art"];
                    if (a.is_string() && arts.count(a.get<std::string>())) art = arts[a.get<std::string>()];
                    else if (a.is_string() && artMap.empty()) art = parseKey(a);
                    else if (a.is_number()) art = a.get<int>();
                    else throw std::runtime_error("track '" + tr.name + "': unknown articulation '" + a.dump() + "' (declare it in \"articulations\")");
                }
                noteArt.push_back(art);
                tr.notes.push_back(note);
            }
            finishTrackNotes(t, out.tempo, tr, noteArt);
            out.tracks.push_back(std::move(tr));
        }
        for (auto &b : j.value("buses", json::array())) {
            Bus bus;
            bus.name = b.at("name").get<std::string>();
            bus.gainDb = b.value("gain", 0.0);
            if (b.contains("fx")) bus.fx = b["fx"];
            bus.output = b.value("output", "");
            if (b.contains("automation") && b["automation"].contains("gain"))
                bus.gainAutomation = Envelope::parse(b["automation"]["gain"], out.tempo, false);
            if (b.contains("automation") && b["automation"].contains("rides")) addRides(bus.gainAutomation, b["automation"]["rides"], out.tempo);
            out.buses.push_back(std::move(bus));
        }
        if (j.contains("master")) {
            out.masterFx = j["master"].value("fx", json::array());
            out.masterGainDb = j["master"].value("gain", 0.0);
            if (j["master"].contains("loudness") && !j["master"]["loudness"].is_null()) {
                out.hasMasterLoudness = true;
                out.masterLoudness = j["master"]["loudness"].get<double>();
                if (out.masterLoudness > -3 || out.masterLoudness < -40)
                    throw std::runtime_error("master \"loudness\" is a target in LUFS between -40 and -3 (streaming: -14)");
                const std::string at = j["master"].value("loudnessGain", std::string("peak"));
                if (at != "peak" && at != "limiter" && at != "start")
                    throw std::runtime_error("master \"loudnessGain\" is \"peak\" (default), \"limiter\" or \"start\"");
                out.loudnessGain = at;
            }
            if (j["master"].contains("automation") && j["master"]["automation"].contains("gain"))
                out.masterGainAutomation = Envelope::parse(j["master"]["automation"]["gain"], out.tempo, false);
            if (j["master"].contains("automation") && j["master"]["automation"].contains("rides"))
                addRides(out.masterGainAutomation, j["master"]["automation"]["rides"], out.tempo);
        }
        for (auto &m : j.value("markers", json::array())) {
            const double beat = m.at("beat").get<double>();
            out.markers.push_back({beat, out.tempo.beatToSec(beat), m.value("name", ""), m.value("checks", true)});
        }
        std::sort(out.markers.begin(), out.markers.end(), [](auto &a, auto &b) { return a.beat < b.beat; });
        if (j.contains("keys")) {   // [{"bar": 1, "key": "D minor"}, {"bar": 69, "key": "E minor"}]: read by lint --harmony
            if (!j["keys"].is_array()) throw std::runtime_error("\"keys\" is a list: [{\"bar\": 1, \"key\": \"D minor\"}, ...]");
            const double beatsPerBar = out.tsigNum * 4.0 / out.tsigDen;
            for (auto &k : j["keys"]) {
                if (!k.is_object() || !k.contains("key") || !(k.contains("bar") || k.contains("beat")))
                    throw std::runtime_error("each \"keys\" entry needs \"key\" (\"D minor\") and \"bar\" (or \"beat\")");
                KeyMark km{};
                std::string kerr;
                if (!parseKeyName(k["key"].get<std::string>(), km.tonic, km.minor, kerr, &km.mode)) throw std::runtime_error("\"keys\": " + kerr);
                km.beat = k.contains("bar") ? (k["bar"].get<double>() - 1) * beatsPerBar : k["beat"].get<double>();
                km.checks = k.value("checks", true);
                out.keys.push_back(km);
            }
            std::sort(out.keys.begin(), out.keys.end(), [](auto &a, auto &b) { return a.beat < b.beat; });
        }
        const std::string stems = j.contains("stems") ? (j["stems"].is_string() ? j["stems"].get<std::string>() : std::to_string(j["stems"].get<int>())) : "float";
        if (stems == "float" || stems == "32") out.stemBits = 32;
        else if (stems == "24") out.stemBits = 24;
        else if (stems == "16") out.stemBits = 16;
        else if (stems == "none") out.stemBits = 0;
        else throw std::runtime_error("\"stems\" must be \"float\", \"24\", \"16\" or \"none\"");
        // outputs must name declared buses, and bus-to-bus routing must not loop
        auto busIndex = [&](const std::string &name) {
            for (size_t i = 0; i < out.buses.size(); ++i) if (out.buses[i].name == name) return (int)i;
            return -1;
        };
        for (const auto &t : out.tracks)
            if (!t.output.empty() && busIndex(t.output) < 0)
                throw std::runtime_error("track '" + t.name + "' outputs to unknown bus '" + t.output + "' (declare it in \"buses\")");
        for (const auto &b : out.buses) {
            if (b.output.empty()) continue;
            if (busIndex(b.output) < 0) throw std::runtime_error("bus '" + b.name + "' outputs to unknown bus '" + b.output + "'");
            std::string at = b.output;
            for (size_t hops = 0; !at.empty(); ++hops) {
                if (at == b.name || hops > out.buses.size()) throw std::runtime_error("bus '" + b.name + "' feeds back into itself");
                at = out.buses[busIndex(at)].output;
            }
        }
        // every send must go to a declared bus
        for (const auto &t : out.tracks)
            for (const auto &[bus, db] : t.sends) {
                bool found = false;
                for (const auto &b : out.buses) found |= b.name == bus;
                if (!found) throw std::runtime_error("track '" + t.name + "' sends to unknown bus '" + bus + "' (declare it in \"buses\")");
            }
        // when each bus and the master first hear something: the earliest track feeding them (output or send),
        // through any buses in between. Late gain curves before that hold nothing audible.
        for (const auto &t : out.tracks) {
            if (t.mute) continue;
            if (t.output.empty()) out.masterFirstSoundBeat = std::min(out.masterFirstSoundBeat, t.firstSoundBeat);
            else out.buses[(size_t)busIndex(t.output)].firstSoundBeat = std::min(out.buses[(size_t)busIndex(t.output)].firstSoundBeat, t.firstSoundBeat);
            for (const auto &s : t.sends) out.buses[(size_t)busIndex(s.first)].firstSoundBeat = std::min(out.buses[(size_t)busIndex(s.first)].firstSoundBeat, t.firstSoundBeat);
        }
        for (size_t pass = 0; pass < out.buses.size(); ++pass)
            for (const auto &b : out.buses)
                if (!b.output.empty()) out.buses[(size_t)busIndex(b.output)].firstSoundBeat = std::min(out.buses[(size_t)busIndex(b.output)].firstSoundBeat, b.firstSoundBeat);
        for (const auto &b : out.buses) if (b.output.empty()) out.masterFirstSoundBeat = std::min(out.masterFirstSoundBeat, b.firstSoundBeat);
        {
            const json buses = j.value("buses", json::array());
            for (size_t i = 0; i < buses.size() && i < out.buses.size(); ++i)
                if (buses[i].contains("automation")) lateGainWarnings(buses[i]["automation"], out.buses[i].firstSoundBeat, out.buses[i].warnings);
            if (j.contains("master") && j["master"].contains("automation"))
                lateGainWarnings(j["master"]["automation"], out.masterFirstSoundBeat, out.masterWarnings);
        }
    } catch (const std::exception &e) {
        err = std::string("invalid job: ") + e.what();
        return false;
    }
    return true;
}

} // namespace wl
