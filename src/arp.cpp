#include "arp.hpp"

#include "automation.hpp"
#include "job.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>

using nlohmann::json;

namespace wl {

bool arpeggiate(const json &notes, const json &arp, json &out, std::string &err) {
    out = json::array();
    if (!arp.is_object()) { err = "\"arp\" is an object: {\"rate\": \"1/16\", \"order\": \"up\", \"octaves\": 1, \"gate\": 0.8}"; return false; }
    static const std::set<std::string> known = {"rate", "order", "octaves", "gate", "seed"};
    for (auto &[k, v] : arp.items())
        if (!known.count(k)) { err = "arp: unknown setting '" + k + "' (it takes rate, order, octaves, gate, seed)"; return false; }
    double step = 0.25;
    try {
        const json &r = arp.contains("rate") ? arp["rate"] : json("1/16");
        step = r.is_number() ? r.get<double>() : Lfo::noteBeats(r.get<std::string>());
    } catch (const std::exception &) { err = "arp: rate is a note value (\"1/16\", \"1/8T\", \"1/8D\") or beats"; return false; }
    if (!(step >= 1.0 / 64)) { err = "arp: rate must be at least 1/256 of a whole note"; return false; }
    const std::string order = arp.value("order", "up");
    static const std::set<std::string> orders = {"up", "down", "updown", "downup", "played", "random", "chord"};
    if (!orders.count(order)) { err = "arp: order is up, down, updown, downup, played, random or chord"; return false; }
    const int octaves = std::clamp(arp.value("octaves", 1), 1, 4);
    const double gate = std::clamp(arp.value("gate", 0.8), 0.05, 1.0);
    std::mt19937 rng((uint32_t)arp.value("seed", 1));

    struct Held { double start, end; int key; size_t index; };
    std::vector<Held> held;
    for (size_t i = 0; i < notes.size(); ++i) {
        const json &n = notes[i];
        if (!n.is_object() || !n.contains("beat") || !n.contains("key")) continue;   // "time" notes and the like play as written
        const double b = n["beat"].get<double>();
        held.push_back({b, b + n.value("dur", 1.0), parseKey(n["key"]), i});
    }
    if (held.empty()) { out = notes; return true; }
    std::stable_sort(held.begin(), held.end(), [](const Held &a, const Held &b) { return a.start < b.start; });
    const double first = held.front().start, last = std::max_element(held.begin(), held.end(), [](auto &a, auto &b) { return a.end < b.end; })->end;
    size_t pos = 0;           // place in the cycle
    bool phrase = false;      // notes were held at the previous step
    for (long k = (long)std::ceil(first / step - 1e-9); k * step < last - 1e-9; ++k) {
        const double s = k * step;
        std::vector<const Held *> now;
        for (auto &h : held) if (h.start <= s + 1e-9 && s < h.end - 1e-9) now.push_back(&h);
        if (now.empty()) { phrase = false; continue; }
        if (!phrase) pos = 0;
        phrase = true;
        // the cycle: the held keys over the octaves, in the order asked for
        std::vector<std::pair<int, const Held *>> seq;
        std::vector<const Held *> byKey = now;
        if (order != "played") std::stable_sort(byKey.begin(), byKey.end(), [](const Held *a, const Held *b) { return a->key < b->key; });
        for (int o = 0; o < octaves; ++o)
            for (auto *h : byKey) seq.push_back({h->key + 12 * o, h});
        if (order == "down") std::reverse(seq.begin(), seq.end());
        else if ((order == "updown" || order == "downup") && seq.size() > 2) {
            std::vector<std::pair<int, const Held *>> back(seq.rbegin() + 1, seq.rend() - 1);   // the turning notes once
            if (order == "downup") { std::reverse(seq.begin(), seq.end()); back.assign(seq.rbegin() + 1, seq.rend() - 1); }
            seq.insert(seq.end(), back.begin(), back.end());
        }
        auto emit = [&](int key, const Held *h) {
            json n = notes[h->index];
            n["beat"] = std::round(s * 1e6) / 1e6;
            n["dur"] = std::round(std::min(step * gate, h->end - s) * 1e6) / 1e6;
            n["key"] = key;
            out.push_back(n);
        };
        if (order == "chord") { for (auto &[key, h] : seq) emit(key, h); continue; }
        const size_t i = order == "random" ? (size_t)(rng() % seq.size()) : pos % seq.size();
        emit(seq[i].first, seq[i].second);
        ++pos;
    }
    for (auto &n : notes) if (!(n.is_object() && n.contains("beat") && n.contains("key"))) out.push_back(n);
    return true;
}

} // namespace wl
