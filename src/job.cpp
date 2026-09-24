#include "job.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <filesystem>
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

bool parseJob(const json &j, const std::string &baseDir, Job &out, std::string &err) {
    try {
        out.baseDir = baseDir;
        out.sampleRate = j.value("sampleRate", 48000);
        out.blockSize = j.value("blockSize", 512);
        out.tail = j.value("tail", 3.0);
        out.warmup = j.value("warmup", 0.4);
        out.length = j.value("length", 0.0);
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
            if (t.contains("params"))
                for (auto &[k, v] : t["params"].items()) tr.params.push_back({k, v.get<double>()});
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
            std::vector<json> ordered;
            for (auto &n : t.value("notes", json::array())) ordered.push_back(n);
            if (roll != 0)   // strum from the lowest note up (or down for negative roll)
                std::stable_sort(ordered.begin(), ordered.end(), [](const json &a, const json &b) {
                    return parseKey(a.at("key")) < parseKey(b.at("key"));
                });
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
                if (hVel > 0) v = (v > 1.0 ? v / 127.0 : v) * (1 + gauss(rng) * hVel);
                note.velocity = std::clamp(v > 1.0 ? v / 127.0 : v, 0.0, 1.0);
                note.channel = n.value("channel", 0);
                if (note.key < 0 || note.key > 127) throw std::runtime_error("note key out of range 0-127 in track '" + tr.name + "'");
                tr.notes.push_back(note);
            }
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
