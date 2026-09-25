#pragma once
// Breakpoint automation. Points are written in beats in the job and converted to seconds
// here, so tempo changes are respected. Values between points are interpolated linearly,
// or exponentially for frequency-like values (smoother sweeps).
#include "tempo.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wl {

// Low-frequency oscillator added on top of an envelope. Rate in Hz, or tempo-synced as a
// note value ("1/4", "1/8T" triplet, "1/16D" dotted, "2/1" = two bars of 4/4).
struct Lfo {
    enum Shape { Sine, Triangle, Square, Saw, Ramp, Random } shape = Sine;
    double hz = 0, beats = 0;     // one of them is set (beats per cycle when synced)
    double phase = 0;             // 0..1 start offset
    double depth = 0;             // in the value's units (octaves for exponential values)
    std::vector<std::pair<double, double>> depthPts;   // optional depth over time (seconds, depth)
    std::shared_ptr<TempoMap> tempo;

    double wave(double t) const {
        double cycles = hz > 0 ? t * hz : tempo->secToBeat(t) / beats;
        cycles += phase;
        const double f = cycles - std::floor(cycles);
        switch (shape) {
        case Sine: return std::sin(2 * M_PI * f);
        case Triangle: return f < 0.25 ? 4 * f : f < 0.75 ? 2 - 4 * f : 4 * f - 4;
        case Square: return f < 0.5 ? 1 : -1;
        case Saw: return 1 - 2 * f;          // falling
        case Ramp: return 2 * f - 1;         // rising
        case Random: {                       // sample and hold, deterministic per cycle
            uint32_t x = (uint32_t)(int64_t)std::floor(cycles) * 2654435761u;
            x ^= x >> 13; x *= 0x5bd1e995; x ^= x >> 15;
            return (x & 0xffff) / 32767.5 - 1;
        }
        }
        return 0;
    }
    double depthAt(double t) const {
        if (depthPts.empty()) return depth;
        if (t <= depthPts.front().first) return depthPts.front().second;
        if (t >= depthPts.back().first) return depthPts.back().second;
        for (size_t i = 1; i < depthPts.size(); ++i)
            if (t < depthPts[i].first) {
                const auto &a = depthPts[i - 1], &b = depthPts[i];
                return a.second + (b.second - a.second) * (t - a.first) / (b.first - a.first);
            }
        return depth;
    }

    // "1/8" -> 0.5 beats, "1/8T" -> 1/3, "1/4D" -> 1.5, "2/1" -> 8
    static double noteBeats(std::string s) {
        double mult = 1;
        if (!s.empty() && (s.back() == 'T' || s.back() == 't')) { mult = 2.0 / 3.0; s.pop_back(); }
        else if (!s.empty() && (s.back() == 'D' || s.back() == 'd' || s.back() == '.')) { mult = 1.5; s.pop_back(); }
        const size_t slash = s.find('/');
        if (slash == std::string::npos) throw std::runtime_error("LFO rate '" + s + "' must be Hz (a number) or a note value like \"1/8\"");
        const double num = std::stod(s.substr(0, slash)), den = std::stod(s.substr(slash + 1));
        return 4.0 * num / den * mult;
    }

    static Lfo parse(const nlohmann::json &j, const TempoMap &tm) {
        Lfo l;
        l.tempo = std::make_shared<TempoMap>(tm);
        const auto &r = j.at("rate");
        if (r.is_number()) l.hz = r.get<double>();
        else l.beats = noteBeats(r.get<std::string>());
        if (j.contains("beats")) l.beats = j["beats"].get<double>(), l.hz = 0;
        if (l.hz <= 0 && l.beats <= 0) throw std::runtime_error("LFO rate must be positive");
        static const std::pair<const char *, Shape> shapes[] = {{"sine", Sine}, {"triangle", Triangle}, {"square", Square},
                                                                {"saw", Saw}, {"ramp", Ramp}, {"random", Random}};
        const std::string sh = j.value("shape", "sine");
        bool found = false;
        for (auto &[n, v] : shapes) if (sh == n) { l.shape = v; found = true; }
        if (!found) throw std::runtime_error("LFO shape must be sine, triangle, square, saw, ramp or random");
        l.phase = j.value("phase", 0.0);
        const auto &d = j.at("depth");
        if (d.is_array()) {
            for (const auto &p : d) l.depthPts.push_back({tm.beatToSec(p.at(0).get<double>()), p.at(1).get<double>()});
            std::sort(l.depthPts.begin(), l.depthPts.end());
        } else l.depth = d.get<double>();
        return l;
    }
};

class Envelope {
public:
    Envelope() = default;
    Envelope(double constant) : pts_{{0.0, constant}}, step_{false} {}

    // [[beat, value], ...] or [{"beat": b, "value": v}, ...]; `exp` = exponential interpolation.
    // A point [beat, value, "step"] holds the previous value until `beat`, then jumps.
    // Object form: {"points": [...], "curve": "linear" | "exp" | "step", "lfo": {...}} or
    // {"value": v, "lfo": {...}} for a steady value with an LFO on it.
    static Envelope parse(const nlohmann::json &j, const TempoMap &tempo, bool exp) {
        Envelope e;
        e.exp_ = exp;
        const nlohmann::json *points = &j;
        bool allStep = false;
        if (j.is_object()) {
            const std::string curve = j.value("curve", exp ? "exp" : "linear");
            if (curve == "exp") e.exp_ = true;
            else if (curve == "linear") e.exp_ = false;
            else if (curve == "step") allStep = true;
            else throw std::runtime_error("automation curve must be linear, exp or step");
            if (j.contains("lfo")) e.lfo_ = std::make_shared<Lfo>(Lfo::parse(j["lfo"], tempo));
            const std::string scale = j.value("scale", "plain");
            if (scale == "normalized") e.normalized_ = true;
            else if (scale != "plain") throw std::runtime_error("automation \"scale\" must be plain or normalized");
            if (j.contains("points")) points = &j["points"];
            else if (j.contains("value")) { e.pts_.push_back({0.0, j["value"].get<double>()}); e.step_.push_back(false); return e; }
            else throw std::runtime_error("automation object needs \"points\" or \"value\"");
        }
        if (!points->is_array() || points->empty()) throw std::runtime_error("automation must be a non-empty array of [beat, value] points");
        std::vector<std::tuple<double, double, bool>> pts;
        for (const auto &p : *points) {
            double beat, value;
            bool step = allStep;
            if (p.is_array()) {
                beat = p.at(0).get<double>(); value = p.at(1).get<double>();
                if (p.size() > 2 && p[2].is_string()) {
                    const std::string m = p[2].get<std::string>();
                    if (m != "step" && m != "hold") throw std::runtime_error("automation point mode must be \"step\"");
                    step = true;
                }
            } else { beat = p.at("beat").get<double>(); value = p.at("value").get<double>(); step = step || p.value("step", false); }
            if (e.exp_ && value <= 0) throw std::runtime_error("exponential automation values must be > 0");
            pts.push_back({tempo.beatToSec(beat), value, step});
        }
        std::stable_sort(pts.begin(), pts.end(), [](auto &a, auto &b) { return std::get<0>(a) < std::get<0>(b); });
        for (auto &[t, v, st] : pts) { e.pts_.push_back({t, v}); e.step_.push_back(st); }
        return e;
    }

    bool empty() const { return pts_.empty(); }
    // "scale": "normalized": values are 0..1 of a plugin parameter's range (as DAWs store automation)
    bool normalized() const { return normalized_; }
    Envelope scaled(double lo, double hi) const {   // normalized -> the parameter's own range
        Envelope e = *this;
        for (auto &p : e.pts_) p.second = lo + std::clamp(p.second, 0.0, 1.0) * (hi - lo);
        if (e.lfo_) { auto l = std::make_shared<Lfo>(*e.lfo_); l->depth *= hi - lo; for (auto &d : l->depthPts) d.second *= hi - lo; e.lfo_ = l; }
        e.normalized_ = false;
        return e;
    }
    bool constant() const { return pts_.size() <= 1 && !lfo_; }

    double at(double sec) const {
        if (pts_.empty()) return 0;
        double v;
        if (sec <= pts_.front().first) v = pts_.front().second;
        else if (sec >= pts_.back().first) v = pts_.back().second;
        else {
            auto it = std::upper_bound(pts_.begin(), pts_.end(), std::make_pair(sec, -1e300));
            const size_t bi = (size_t)(it - pts_.begin());
            const auto &b = *it, &a = *(it - 1);
            if (step_[bi]) v = a.second;
            else {
                double t = (sec - a.first) / std::max(1e-12, b.first - a.first);
                v = exp_ ? a.second * std::pow(b.second / a.second, t) : a.second + (b.second - a.second) * t;
            }
        }
        if (lfo_) {
            const double w = lfo_->wave(sec) * lfo_->depthAt(sec);
            v = exp_ ? v * std::pow(2.0, w) : v + w;
        }
        return v;
    }

private:
    std::vector<std::pair<double, double>> pts_;   // (seconds, value)
    std::vector<bool> step_;                        // jump (hold, then step) into this point
    bool exp_ = false;
    bool normalized_ = false;
    std::shared_ptr<Lfo> lfo_;
};

// Gain curves that add up: a track's written fader curve plus any number of "rides" layered on top
// (dB), so section rides never overwrite the fader automation.
struct GainCurve {
    std::vector<Envelope> parts;
    GainCurve &operator=(Envelope e) { parts.clear(); if (!e.empty()) parts.push_back(std::move(e)); return *this; }
    void add(Envelope e) { if (!e.empty()) parts.push_back(std::move(e)); }
    bool empty() const { return parts.empty(); }
    double at(double sec) const { double v = 0; for (auto &p : parts) v += p.at(sec); return v; }
};

// "rides": one curve ([[beat, dB], ...] or a curve object) or named curves {"sections": ..., "fills": ...}
inline void addRides(GainCurve &into, const nlohmann::json &rides, const TempoMap &tempo) {
    const bool named = rides.is_object() && !rides.contains("points") && !rides.contains("value");
    if (named) for (auto &[k, v] : rides.items()) into.add(Envelope::parse(v, tempo, false));
    else into.add(Envelope::parse(rides, tempo, false));
}

// The earliest point of an automation curve as written ([[beat, value], ...] or {"points": ...}).
// A curve holds this value before its first point, which surprises when the point is late.
inline bool firstPoint(const nlohmann::json &j, double &beat, double &value) {
    const nlohmann::json *pts = j.is_object() ? (j.contains("points") ? &j["points"] : nullptr) : &j;
    if (!pts || !pts->is_array() || pts->empty()) return false;
    bool found = false;
    for (const auto &p : *pts) {
        double b, v;
        if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number()) { b = p[0].get<double>(); v = p[1].get<double>(); }
        else if (p.is_object() && p.contains("beat") && p.contains("value")) { b = p["beat"].get<double>(); v = p["value"].get<double>(); }
        else continue;
        if (!found || b < beat) { beat = b; value = v; found = true; }
    }
    return found;
}
inline std::string lateCurveWarning(const std::string &what, double beat, double value, double expected) {
    char buf[300];
    std::snprintf(buf, sizeof buf, "%s automation starts at beat %g, and a curve holds its first value (%g) before that, not %g: "
                  "add a point at beat 0 with the value the song should start at", what.c_str(), beat, value, expected);
    return buf;
}

// A number in the job that may be automated: `"cutoff": 800` plus optional
// `"automate": {"cutoff": [[0, 200], [16, 8000]]}` on the same object.
// `"lfo": {"cutoff": {"rate": "1/8", "depth": 1.5}}` adds an LFO to either.
inline Envelope param(const nlohmann::json &obj, const char *key, double def, const TempoMap &tempo, bool exp = false) {
    nlohmann::json spec;
    if (obj.contains("automate") && obj["automate"].contains(key)) {
        const auto &a = obj["automate"][key];
        spec = a.is_object() ? a : nlohmann::json{{"points", a}};
    } else spec = {{"value", obj.value(key, def)}};
    if (obj.contains("lfo") && obj["lfo"].contains(key)) spec["lfo"] = obj["lfo"][key];
    if (!spec.contains("curve")) spec["curve"] = exp ? "exp" : "linear";
    return Envelope::parse(spec, tempo, exp);
}

} // namespace wl
