#include "job.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace wl {

TempoMap::TempoMap(std::vector<TempoPoint> points) : pts_(std::move(points)) {
    if (pts_.empty()) pts_.push_back({0, 120});
    std::sort(pts_.begin(), pts_.end(), [](auto &a, auto &b) { return a.beat < b.beat; });
    if (pts_.front().beat > 0) pts_.insert(pts_.begin(), {0, pts_.front().bpm});
    secAt_.resize(pts_.size());
    secAt_[0] = 0;
    for (size_t i = 1; i < pts_.size(); ++i)
        secAt_[i] = secAt_[i - 1] + (pts_[i].beat - pts_[i - 1].beat) * 60.0 / pts_[i - 1].bpm;
}

double TempoMap::beatToSec(double beat) const {
    size_t i = pts_.size() - 1;
    while (i > 0 && pts_[i].beat > beat) --i;
    return secAt_[i] + (beat - pts_[i].beat) * 60.0 / pts_[i].bpm;
}

double TempoMap::secToBeat(double sec) const {
    size_t i = pts_.size() - 1;
    while (i > 0 && secAt_[i] > sec) --i;
    return pts_[i].beat + (sec - secAt_[i]) * pts_[i].bpm / 60.0;
}

double TempoMap::bpmAtBeat(double beat) const {
    size_t i = pts_.size() - 1;
    while (i > 0 && pts_[i].beat > beat) --i;
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
        else for (auto &p : j["tempo"]) tp.push_back({p.value("beat", 0.0), p.at("bpm").get<double>()});
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
            if (t.contains("state") && !t["state"].is_null()) {
                const auto &s = t["state"];
                tr.stateFile = s.is_string() ? s.get<std::string>() : s.at("file").get<std::string>();
                tr.stateFormat = s.is_object() ? s.value("format", "auto") : "auto";
                if (fs::path(tr.stateFile).is_relative()) tr.stateFile = (fs::path(baseDir) / tr.stateFile).string();
            }
            if (t.contains("params"))
                for (auto &[k, v] : t["params"].items()) tr.params.push_back({k, v.get<double>()});
            if (t.contains("fx")) {
                if (!t["fx"].is_array()) throw std::runtime_error("track '" + tr.name + "': \"fx\" must be an array of effects");
                tr.fx = t["fx"];
            }
            const json sends = t.value("sends", json::object());   // named: items() must not outlive its object
            for (auto &[bus, db] : sends.items()) tr.sends.push_back({bus, db.get<double>()});
            if (t.contains("automation")) {
                const auto &au = t["automation"];
                if (au.contains("gain")) tr.gainAutomation = Envelope::parse(au["gain"], out.tempo, false);
                const json autoParams = au.value("params", json::object());
                for (auto &[k, v] : autoParams.items()) tr.paramAutomation.push_back({k, Envelope::parse(v, out.tempo, false)});
            }
            for (auto &n : t.value("notes", json::array())) {
                Note note;
                if (n.contains("time")) {
                    note.start = n["time"].get<double>();
                    note.length = n.value("length", 0.5);
                } else {
                    double b = n.at("beat").get<double>(), d = n.value("dur", 1.0);
                    note.start = out.tempo.beatToSec(b);
                    note.length = out.tempo.beatToSec(b + d) - note.start;
                }
                note.key = parseKey(n.at("key"));
                double v = n.value("vel", 0.8);
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
            out.buses.push_back(std::move(bus));
        }
        if (j.contains("master")) {
            out.masterFx = j["master"].value("fx", json::array());
            out.masterGainDb = j["master"].value("gain", 0.0);
        }
        for (auto &m : j.value("markers", json::array())) {
            const double beat = m.at("beat").get<double>();
            out.markers.push_back({beat, out.tempo.beatToSec(beat), m.value("name", "")});
        }
        std::sort(out.markers.begin(), out.markers.end(), [](auto &a, auto &b) { return a.beat < b.beat; });
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
