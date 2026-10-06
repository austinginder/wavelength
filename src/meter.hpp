#pragma once
// The song's meter: "timeSignature" [3, 4] from bar 1, and "meterChanges" [{"bar": 9, "sig": [4, 4]}] from the
// bars they name. Bars count from 1; a bar's length in beats (quarter notes) is num * 4 / den.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace wl {

struct MeterMap {
    struct Seg {
        int bar = 1;        // the bar it starts at
        double beat = 0;    // and that bar's first beat
        int num = 4, den = 4;
        double bpb() const { return num * 4.0 / den; }
    };
    std::vector<Seg> segs{Seg{}};

    const Seg &atBeat(double beat) const {
        size_t i = 0;
        while (i + 1 < segs.size() && segs[i + 1].beat <= beat + 1e-9) ++i;
        return segs[i];
    }
    const Seg &atBar(double bar) const {
        size_t i = 0;
        while (i + 1 < segs.size() && segs[i + 1].bar <= bar + 1e-9) ++i;
        return segs[i];
    }
    // the first beat of a bar (1-based; a fraction goes into the bar), and the bar a beat falls in (0-based)
    double barToBeat(double bar) const { const Seg &s = atBar(bar); return s.beat + (bar - s.bar) * s.bpb(); }
    int barIndex(double beat) const {
        const Seg &s = atBeat(beat);
        return s.bar - 1 + (int)std::floor((beat - s.beat) / s.bpb() + 1e-9);
    }
    double barStart(double beat) const { return barToBeat(barIndex(beat) + 1); }
    bool constant() const { return segs.size() == 1; }

    // from a job's "timeSignature" and "meterChanges"; throws std::runtime_error with what's wrong
    static MeterMap fromJob(const nlohmann::json &j) {
        MeterMap m;
        auto sig = [](const nlohmann::json &v, const std::string &what) {
            if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
                throw std::runtime_error(what + " is [beats, note value], such as [3, 4] or [6, 8]");
            const double n = v[0].get<double>(), d = v[1].get<double>();
            if (n < 1 || n > 64 || n != std::floor(n) || (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32))
                throw std::runtime_error(what + ": 1 to 64 beats of a 1, 2, 4, 8, 16 or 32 note");
            return std::make_pair((int)n, (int)d);
        };
        if (j.contains("timeSignature")) std::tie(m.segs[0].num, m.segs[0].den) = sig(j["timeSignature"], "\"timeSignature\"");
        if (!j.contains("meterChanges")) return m;
        const nlohmann::json &c = j["meterChanges"];
        if (!c.is_array()) throw std::runtime_error("\"meterChanges\" is a list: [{\"bar\": 9, \"sig\": [4, 4]}, ...]");
        std::vector<std::pair<int, std::pair<int, int>>> list;
        for (const auto &e : c) {
            if (!e.is_object() || !e.contains("bar") || !e["bar"].is_number() || !e.contains("sig"))
                throw std::runtime_error("each \"meterChanges\" entry is {\"bar\": 9, \"sig\": [4, 4]}");
            const double bar = e["bar"].get<double>();
            if (bar < 2 || bar != std::floor(bar)) throw std::runtime_error("\"meterChanges\": a change starts at a whole bar, 2 or later (bar 1 is \"timeSignature\")");
            list.push_back({(int)bar, sig(e["sig"], "\"meterChanges\" sig")});
        }
        std::sort(list.begin(), list.end(), [](auto &a, auto &b) { return a.first < b.first; });
        for (size_t i = 1; i < list.size(); ++i)
            if (list[i].first == list[i - 1].first) throw std::runtime_error("\"meterChanges\": two changes at bar " + std::to_string(list[i].first));
        for (auto &[bar, s] : list) {
            const Seg &prev = m.segs.back();
            if (s.first == prev.num && s.second == prev.den) continue;   // no change
            Seg n;
            n.bar = bar;
            n.beat = prev.beat + (bar - prev.bar) * prev.bpb();
            n.num = s.first;
            n.den = s.second;
            m.segs.push_back(n);
        }
        return m;
    }
};

// A meter given as (beat, numerator, denominator) points, written into a job as "timeSignature" (the earliest) and
// "meterChanges" (each later one from the first bar line at or after it, counted in the meter before it). Importers
// use it: DAWproject, MIDI files, MusicXML and GarageBand projects all store the meter by position.
inline void writeMeter(nlohmann::json &job, std::vector<std::tuple<double, int, int>> pts) {
    std::stable_sort(pts.begin(), pts.end(), [](auto &a, auto &b) { return std::get<0>(a) < std::get<0>(b); });
    if (pts.empty()) return;
    int num = std::get<1>(pts[0]), den = std::get<2>(pts[0]), bar = 1;
    double beat = 0;
    job["timeSignature"] = {num, den};
    nlohmann::json changes = nlohmann::json::array();
    for (size_t k = 1; k < pts.size(); ++k) {
        const auto [b, n, d] = pts[k];
        if (n == num && d == den) continue;
        const double bpb = num * 4.0 / den;
        const int at = bar + std::max(0, (int)std::ceil((b - beat) / bpb - 1e-6));
        if (at == bar && !changes.empty() && changes.back()["bar"] == bar) changes.erase(changes.end() - 1);   // a later change at the same bar wins
        else if (at == 1) { job["timeSignature"] = {n, d}; num = n; den = d; continue; }
        beat += (at - bar) * bpb;
        bar = at;
        num = n;
        den = d;
        changes.push_back({{"bar", bar}, {"sig", {n, d}}});
    }
    if (!changes.empty()) job["meterChanges"] = changes;
}

} // namespace wl
