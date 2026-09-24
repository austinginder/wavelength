#pragma once
// Breakpoint automation. Points are written in beats in the job and converted to seconds
// here, so tempo changes are respected. Values between points are interpolated linearly,
// or exponentially for frequency-like values (smoother sweeps).
#include "tempo.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wl {

class Envelope {
public:
    Envelope() = default;
    Envelope(double constant) : pts_{{0.0, constant}} {}

    // [[beat, value], ...] or [{"beat": b, "value": v}, ...]; `exp` = exponential interpolation
    static Envelope parse(const nlohmann::json &j, const TempoMap &tempo, bool exp) {
        Envelope e;
        e.exp_ = exp;
        if (!j.is_array() || j.empty()) throw std::runtime_error("automation must be a non-empty array of [beat, value] points");
        for (const auto &p : j) {
            double beat, value;
            if (p.is_array()) { beat = p.at(0).get<double>(); value = p.at(1).get<double>(); }
            else { beat = p.at("beat").get<double>(); value = p.at("value").get<double>(); }
            if (exp && value <= 0) throw std::runtime_error("exponential automation values must be > 0");
            e.pts_.push_back({tempo.beatToSec(beat), value});
        }
        std::sort(e.pts_.begin(), e.pts_.end());
        return e;
    }

    bool empty() const { return pts_.empty(); }
    bool constant() const { return pts_.size() <= 1; }

    double at(double sec) const {
        if (pts_.empty()) return 0;
        if (sec <= pts_.front().first) return pts_.front().second;
        if (sec >= pts_.back().first) return pts_.back().second;
        auto it = std::upper_bound(pts_.begin(), pts_.end(), std::make_pair(sec, -1e300));
        const auto &b = *it, &a = *(it - 1);
        double t = (sec - a.first) / std::max(1e-12, b.first - a.first);
        if (exp_) return a.second * std::pow(b.second / a.second, t);
        return a.second + (b.second - a.second) * t;
    }

private:
    std::vector<std::pair<double, double>> pts_;   // (seconds, value)
    bool exp_ = false;
};

// A number in the job that may be automated: `"cutoff": 800` plus optional
// `"automate": {"cutoff": [[0, 200], [16, 8000]]}` on the same object.
inline Envelope param(const nlohmann::json &obj, const char *key, double def, const TempoMap &tempo, bool exp = false) {
    if (obj.contains("automate") && obj["automate"].contains(key)) return Envelope::parse(obj["automate"][key], tempo, exp);
    return Envelope(obj.value(key, def));
}

} // namespace wl
