#include "job.hpp"

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
    if (t.contains("velocityTo")) {   // {"param": "Dynamics"} or {"cc": 1}, with "min"/"max" output values
        const json &v = t["velocityTo"];
        const bool cc = v.contains("cc");
        if (!cc && !v.contains("param")) throw std::runtime_error("track '" + tr.name + "': \"velocityTo\" needs \"param\" or \"cc\"");
        const double lo = v.value("min", cc ? 10.0 : 0.05), hi = v.value("max", cc ? 127.0 : 1.0);
        std::vector<std::pair<double, double>> onsets;   // (beat, velocity), one per start
        for (size_t i = 0; i < played; ++i) {
            const double b = std::round(tempo.secToBeat(tr.notes[i].start) * 1000) / 1000;
            if (!onsets.empty() && onsets.back().first == b) onsets.back().second = std::max(onsets.back().second, tr.notes[i].velocity);
            else onsets.push_back({b, tr.notes[i].velocity});
        }
        std::sort(onsets.begin(), onsets.end());
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
            tr.warmup = t.value("warmup", -1.0);
            tr.preset = t.value("preset", "");
            if (t.contains("state") && !t["state"].is_null()) {
                const auto &s = t["state"];
                tr.stateFile = s.is_string() ? s.get<std::string>() : s.at("file").get<std::string>();
                tr.stateFormat = s.is_object() ? s.value("format", "auto") : "auto";
                if (fs::path(tr.stateFile).is_relative()) tr.stateFile = (fs::path(baseDir) / tr.stateFile).string();
            }
            if (t.contains("sampler")) tr.sampler = t["sampler"];
            if (t.contains("clips")) {
                if (!t["clips"].is_array()) throw std::runtime_error("track '" + tr.name + "': \"clips\" must be an array");
                tr.clips = t["clips"];
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
                for (auto &[k, v] : autoParams.items()) tr.paramAutomation.push_back({k, Envelope::parse(v, out.tempo, false)});
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
                const std::string at = j["master"].value("loudnessGain", std::string("limiter"));
                if (at != "limiter" && at != "start") throw std::runtime_error("master \"loudnessGain\" is \"limiter\" (default) or \"start\"");
                out.loudnessAtStart = at == "start";
            }
            if (j["master"].contains("automation") && j["master"]["automation"].contains("gain"))
                out.masterGainAutomation = Envelope::parse(j["master"]["automation"]["gain"], out.tempo, false);
        }
        for (auto &m : j.value("markers", json::array())) {
            const double beat = m.at("beat").get<double>();
            out.markers.push_back({beat, out.tempo.beatToSec(beat), m.value("name", "")});
        }
        std::sort(out.markers.begin(), out.markers.end(), [](auto &a, auto &b) { return a.beat < b.beat; });
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
    } catch (const std::exception &e) {
        err = std::string("invalid job: ") + e.what();
        return false;
    }
    return true;
}

} // namespace wl
