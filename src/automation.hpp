#pragma once
// Breakpoint automation. Points are written in beats in the job and converted to seconds
// here, so tempo changes are respected. Values between points are interpolated linearly,
// or exponentially for frequency-like values (smoother sweeps).
#include "tempo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    // [beat, value, "switch"] holds too, then moves to the new value over a short ramp (default 5 ms,
    // "ramp" in ms on the object form): one point per on/off switch, without a click.
    // Object form: {"points": [...], "curve": "linear" | "exp" | "step" | "switch", "ramp": ms, "lfo": {...}}
    // or {"value": v, "lfo": {...}} for a steady value with an LFO on it.
    // Values may be text: display text ("800 Hz", "-6 dB", "97%") or a note name ("C#4" = its frequency);
    // "scale": "display" reads numbers as display values too. Built-in values are read here; with
    // `deferText` (plugin parameters) the curve keeps the text until resolveText() asks the plugin.
    static Envelope parse(const nlohmann::json &j, const TempoMap &tempo, bool exp, bool deferText = false) {
        Envelope e;
        e.exp_ = exp;
        const nlohmann::json *points = &j;
        int allMode = 0;           // 0 interpolate, 1 step, 2 switch
        bool display = false;      // "scale": "display": numbers are display values
        if (j.is_object()) {
            const std::string curve = j.value("curve", exp ? "exp" : "linear");
            if (curve == "exp") e.exp_ = true;
            else if (curve == "linear") e.exp_ = false;
            else if (curve == "step") allMode = 1;
            else if (curve == "switch") allMode = 2;
            else throw std::runtime_error("automation curve must be linear, exp, step or switch");
            if (j.contains("ramp")) {
                e.rampSec_ = j["ramp"].get<double>() / 1000.0;
                if (e.rampSec_ < 0 || e.rampSec_ > 10) throw std::runtime_error("automation \"ramp\" is milliseconds, 0 to 10000");
            }
            if (j.contains("lfo")) e.lfo_ = std::make_shared<Lfo>(Lfo::parse(j["lfo"], tempo));
            const std::string scale = j.value("scale", "plain");
            if (scale == "normalized") e.normalized_ = true;
            else if (scale == "display") display = true;
            else if (scale != "plain") throw std::runtime_error("automation \"scale\" must be plain, normalized or display");
            if (j.contains("points")) points = &j["points"];
            else if (j.contains("value")) {
                nlohmann::json pt = nlohmann::json::array({0.0, j["value"]});
                return finish(e, {rawPoint(pt, tempo, 0, display, e)}, deferText);
            } else throw std::runtime_error("automation object needs \"points\" or \"value\"");
        }
        if (!points->is_array() || points->empty()) throw std::runtime_error("automation must be a non-empty array of [beat, value] points");
        std::vector<Raw> raw;
        for (const auto &p : *points) raw.push_back(rawPoint(p, tempo, allMode, display, e));
        std::stable_sort(raw.begin(), raw.end(), [](const Raw &a, const Raw &b) { return a.sec < b.sec; });
        return finish(e, std::move(raw), deferText);
    }

    // Text values still waiting for a plugin to read them (plugin parameter curves).
    bool needsText() const { return !raw_.empty(); }
    // Reads every text value through `read(text, value)` (the plugin's own text-to-value; cache it),
    // then builds the curve. False (and `bad` = the text) when one could not be read.
    template <class Read> bool resolveText(Read read, std::string &bad) {
        for (auto &r : raw_)
            if (!r.text.empty() && !read(r.text, r.value)) { bad = r.text; return false; }
        auto raw = std::move(raw_);
        raw_.clear();
        build(raw);
        return true;
    }

    // A note name ("C#4", "Bb2", "A-1"; C4 = MIDI 60) as its frequency in Hz (A4 = 440).
    static bool noteHz(const std::string &s, double &hz) {
        static const int base[] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
        if (s.size() < 2) return false;
        const char c = (char)std::toupper((unsigned char)s[0]);
        if (c < 'A' || c > 'G') return false;
        int semis = base[c - 'A'];
        size_t i = 1;
        while (i < s.size() && (s[i] == '#' || s[i] == 'b')) semis += s[i++] == '#' ? 1 : -1;
        if (i >= s.size()) return false;
        size_t k = i + (s[i] == '-' ? 1 : 0);
        if (k >= s.size()) return false;
        for (size_t x = k; x < s.size(); ++x) if (!std::isdigit((unsigned char)s[x])) return false;
        const int midi = 12 * (std::stoi(s.substr(i)) + 1) + semis;
        hz = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        return true;
    }
    // Text for a built-in value: a note name (its Hz), or a number with an optional unit: Hz, kHz (x1000),
    // dB, % (/100: 50% = 0.5), ms, s, st.
    static bool builtinText(const std::string &text, double &v) {
        if (noteHz(text, v)) return true;
        const char *s = text.c_str();
        char *end = nullptr;
        v = std::strtod(s, &end);
        if (end == s) return false;
        std::string unit;
        for (const char *q = end; *q; ++q) if (!std::isspace((unsigned char)*q)) unit += (char)std::tolower((unsigned char)*q);
        if (unit.empty() || unit == "hz" || unit == "db" || unit == "ms" || unit == "s" || unit == "st") return true;
        if (unit == "khz" || unit == "k") { v *= 1000; return true; }
        if (unit == "%") { v /= 100; return true; }
        return false;
    }

    bool empty() const { return pts_.empty() && raw_.empty(); }
    // the curve's corners (seconds, value) after switches are expanded: for reports and checks
    const std::vector<std::pair<double, double>> &corners() const { return pts_; }
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
    struct Raw { double sec, value; std::string text; int mode; };   // mode: 0 interpolate, 1 step, 2 switch
    static Raw rawPoint(const nlohmann::json &p, const TempoMap &tempo, int allMode, bool display, const Envelope &e) {
        Raw r{0, 0, "", allMode};
        const nlohmann::json *v;
        if (p.is_array()) {
            r.sec = tempo.beatToSec(p.at(0).get<double>());
            v = &p.at(1);
            if (p.size() > 2 && p[2].is_string()) {
                const std::string m = p[2].get<std::string>();
                if (m == "step" || m == "hold") r.mode = 1;
                else if (m == "switch") r.mode = 2;
                else throw std::runtime_error("automation point mode must be \"step\" or \"switch\"");
            }
        } else {
            r.sec = tempo.beatToSec(p.at("beat").get<double>());
            v = &p.at("value");
            if (p.value("step", false)) r.mode = 1;
            if (p.value("switch", false)) r.mode = 2;
        }
        if (v->is_string()) r.text = v->get<std::string>();
        else if (display) { char buf[40]; std::snprintf(buf, sizeof buf, "%.10g", v->get<double>()); r.text = buf; }
        else r.value = v->get<double>();
        if (!r.text.empty() && e.normalized_) throw std::runtime_error("\"scale\": \"normalized\" curves take numbers 0..1, not text ('" + r.text + "')");
        return r;
    }
    static Envelope finish(Envelope &e, std::vector<Raw> raw, bool deferText) {
        const bool text = std::any_of(raw.begin(), raw.end(), [](const Raw &r) { return !r.text.empty(); });
        if (text && deferText) { e.raw_ = std::move(raw); return e; }
        for (auto &r : raw)
            if (!r.text.empty() && !builtinText(r.text, r.value))
                throw std::runtime_error("cannot read '" + r.text + "' as a value: use a number, a note name (\"C#4\" = its Hz) or a "
                                         "number with a unit (\"800 Hz\", \"1.2 kHz\", \"-6 dB\", \"50%\")");
        e.build(raw);
        return e;
    }
    // sorted raw points -> corners; a switch becomes a held point plus a short ramp
    void build(const std::vector<Raw> &pts) {
        pts_.clear(); step_.clear();
        for (size_t i = 0; i < pts.size(); ++i) {
            const Raw &r = pts[i];
            if (exp_ && r.value <= 0 && pts.size() > 1) throw std::runtime_error("exponential automation values must be > 0");
            if (r.mode != 2 || pts_.empty() || rampSec_ <= 0) { pts_.push_back({r.sec, r.value}); step_.push_back(r.mode != 0); continue; }
            // hold the previous value up to the switch, then ramp; the ramp never runs past the next point
            double ramp = rampSec_;
            if (i + 1 < pts.size()) ramp = std::min(ramp, std::max(0.0, pts[i + 1].sec - r.sec) * 0.5);
            pts_.push_back({r.sec, pts_.back().second});
            step_.push_back(true);
            pts_.push_back({r.sec + std::max(ramp, 1e-6), r.value});
            step_.push_back(false);
        }
    }
    std::vector<Raw> raw_;                          // points waiting for text to be read (plugin parameters)
    double rampSec_ = 0.005;                        // "switch" ramp
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
    } else if (obj.contains(key) && obj[key].is_string()) spec = {{"value", obj[key]}};   // "C#4", "2 kHz"
    else spec = {{"value", obj.value(key, def)}};
    if (obj.contains("lfo") && obj["lfo"].contains(key)) spec["lfo"] = obj["lfo"][key];
    if (!spec.contains("curve")) spec["curve"] = exp ? "exp" : "linear";
    return Envelope::parse(spec, tempo, exp);
}

} // namespace wl
