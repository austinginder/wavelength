#include "arp.hpp"

#include "automation.hpp"
#include "job.hpp"
#include "logic_patches.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <random>
#include <set>

using nlohmann::json;

namespace wl {

namespace {
struct Held { double start, end, vel; int key; size_t index; };
using Item = std::pair<int, const Held *>;   // a sounding key and the held note it comes from

std::vector<Item> reversed(std::vector<Item> v) { std::reverse(v.begin(), v.end()); return v; }
// a, then b without its first `from` and last `cut` items
std::vector<Item> joined(std::vector<Item> a, const std::vector<Item> &b, size_t from, size_t cut) {
    for (size_t i = from; i + cut < b.size(); ++i) a.push_back(b[i]);
    return a;
}
// seq in groups of `size`, each played in the order `perm` (positions a short last group lacks are skipped)
std::vector<Item> grouped(const std::vector<Item> &seq, size_t size, std::initializer_list<size_t> perm) {
    std::vector<Item> out;
    for (size_t g = 0; g < seq.size(); g += size)
        for (size_t i : perm) if (g + i < seq.size()) out.push_back(seq[g + i]);
    return out;
}

// One cycle of an order and its variation (1-4, as Logic's Arpeggiator numbers them). asc = low to high, played = in
// the order struck.
std::vector<Item> orderCycle(const std::vector<Item> &asc, const std::vector<Item> &played, const std::string &order, int var) {
    if (order == "up" || order == "down") {
        const std::vector<Item> L = order == "up" ? asc : reversed(asc);
        return var == 2 ? grouped(L, 4, {1, 0, 2, 3}) : var == 3 ? grouped(L, 4, {2, 0, 1, 3}) : var == 4 ? grouped(L, 3, {0, 2, 1}) : L;
    }
    if (order == "updown" || order == "downup") {
        const std::vector<Item> L = order == "updown" ? asc : reversed(asc);
        if (var == 1) return joined(L, reversed(L), 0, 0);   // the turning notes twice
        if (var == 2) return joined(L, reversed(L), 1, 1);   // once
        const std::vector<Item> F = var == 3 ? grouped(L, 2, {1, 0}) : grouped(L, 3, {0, 2, 1});
        return joined(F, reversed(F), 0, 0);
    }
    if (order == "outsidein") {
        std::vector<Item> out;
        const long m = (long)asc.size();
        if (var <= 2) {   // alternating from the top (1) or the bottom (2) toward the middle
            long lo = 0, hi = m - 1;
            for (bool top = var == 1; lo <= hi; top = !top) out.push_back(top ? asc[(size_t)hi--] : asc[(size_t)lo++]);
            return out;
        }
        if (!m) return out;   // inside out from the upper (3) or lower (4) middle note
        const long s = var == 3 ? m / 2 : (m - 1) / 2;
        out.push_back(asc[(size_t)s]);
        for (long d = 1; (long)out.size() < m; ++d)
            for (long i : var == 3 ? std::array<long, 2>{s - d, s + d} : std::array<long, 2>{s + d, s - d})
                if (i >= 0 && i < m) out.push_back(asc[(size_t)i]);
        return out;
    }
    if (order == "played") {
        if (var == 2) return reversed(played);
        if (var == 3) return joined(played, reversed(played), 0, 0);
        if (var == 4) return joined(played, reversed(played), 1, 1);
        return played;
    }
    return asc;   // random, chord: the set to pick from
}

// Inversions mode: the order the voicings play in (0 = as held, j = its j lowest notes an octave up), per order and
// variation, from Logic's inversion table (written for four; a smaller range skips the voicings it lacks)
const std::vector<int> &inversionOrder(const std::string &order, int var) {
    static const std::map<std::string, std::array<std::vector<int>, 4>> t = {
        {"up", {{{0, 1, 2, 3}, {1, 0, 2, 3}, {2, 0, 1, 3}, {0, 2, 1, 3}}}},
        {"down", {{{3, 2, 1, 0}, {2, 3, 1, 0}, {1, 3, 2, 0}, {3, 1, 2, 0}}}},
        {"updown", {{{0, 1, 2, 3, 3, 2, 1, 0}, {1, 0, 3, 2, 2, 3, 0, 1}, {0, 2, 1, 3, 3, 1, 2, 0}, {0, 1, 2, 3, 2, 1}}}},
        {"downup", {{{3, 2, 1, 0, 0, 1, 2, 3}, {2, 3, 0, 1, 1, 0, 3, 2}, {3, 1, 2, 0, 0, 2, 1, 3}, {3, 2, 1, 0, 1, 2}}}},
        {"outsidein", {{{3, 0, 2, 1}, {0, 3, 1, 2}, {1, 2, 0, 3}, {2, 1, 3, 0}}}},
        {"played", {{{0, 1, 2, 3}, {3, 2, 1, 0}, {0, 1, 2, 3, 3, 2, 1, 0}, {0, 1, 2, 3, 2, 1}}}}};
    auto it = t.find(order);
    return (it == t.end() ? t.at("up") : it->second)[(size_t)var - 1];
}

struct Step { int kind = 0; double vel = -1, len = 1; };   // kind 0 note, 1 chord, 2 rest; vel < 0 = as played

bool parseSteps(const json &j, std::vector<Step> &steps, std::string &err) {
    const char *how = "arp: steps is a list of velocities (0-1), \"rest\", or {\"vel\", \"len\", \"chord\": true}";
    if (!j.is_array() || j.empty() || j.size() > 128) { err = how; return false; }
    for (auto &s : j) {
        Step st;
        if (s.is_number()) st.vel = s.get<double>();
        else if (s.is_string() && (s == "rest" || s == "-")) st.kind = 2;
        else if (s.is_object()) {
            for (auto &[k, v] : s.items())
                if (k != "vel" && k != "len" && k != "chord" && k != "rest") { err = "arp: a step takes vel, len, chord, rest (not '" + k + "')"; return false; }
            st.kind = s.value("rest", false) ? 2 : s.value("chord", false) ? 1 : 0;
            st.vel = s.value("vel", -1.0);
            st.len = s.value("len", 1.0);
        } else { err = how; return false; }
        if ((s.is_number() || (s.is_object() && s.contains("vel"))) && !(st.vel >= 0 && st.vel <= 1)) { err = "arp: a step's vel is 0-1"; return false; }
        if (!(st.len >= 0.01 && st.len <= 16)) { err = "arp: a step's len is 0.01-16 steps"; return false; }
        steps.push_back(st);
    }
    return true;
}
} // namespace

bool resolveArp(const json &arp, json &out, std::vector<std::string> &notes, std::string &err) {
    if (arp.is_string()) {   // a patch's Arpeggiator, else a preset
        std::string e1, e2;
        if (appleArpeggiator(arp.get<std::string>(), false, out, notes, e1) || appleArpeggiator(arp.get<std::string>(), true, out, notes, e2)) return true;
        err = "arp: '" + arp.get<std::string>() + "' is neither a GarageBand or Logic patch with an Arpeggiator nor an Arpeggiator preset (`wavelength presets arp`)";
        return false;
    }
    if (!arp.is_object()) { err = "\"arp\" is an object: {\"rate\": \"1/16\", \"order\": \"up\", \"octaves\": 1, \"gate\": 0.8}, or a patch or preset name"; return false; }
    out = arp;
    if (arp.contains("patch") || arp.contains("preset")) {
        if (arp.contains("patch") && arp.contains("preset")) { err = "arp: give a patch or a preset, not both"; return false; }
        const bool preset = arp.contains("preset");
        const json &name = preset ? arp["preset"] : arp["patch"];
        if (!name.is_string()) { err = std::string("arp: ") + (preset ? "preset" : "patch") + " is a name"; return false; }
        if (!appleArpeggiator(name.get<std::string>(), preset, out, notes, err)) { err = "arp: " + err; return false; }
        for (auto &[k, v] : arp.items()) if (k != "patch" && k != "preset") out[k] = v;
    }
    for (auto it = out.begin(); it != out.end();) it = it->is_null() ? out.erase(it) : std::next(it);   // null = the setting left out
    return true;
}

std::string arpSummary(const json &a) {
    auto num = [](double v) { std::string s = std::to_string(std::round(v * 1000) / 1000); s.erase(s.find_last_not_of('0') + 1); if (s.back() == '.') s.pop_back(); return s; };
    std::string s = !a.contains("rate") ? "1/16" : a["rate"].is_string() ? a["rate"].get<std::string>() : a["rate"].is_number() ? num(a["rate"].get<double>()) + " beats" : "?";
    s += " " + (a.contains("order") && a["order"].is_string() ? a["order"].get<std::string>() : std::string("up"));
    if (a.contains("variation") && a["variation"].is_number() && a["variation"] != 1) s += " (variation " + num(a["variation"].get<double>()) + ")";
    if (a.contains("octaves") && a["octaves"].is_number() && a["octaves"] != 1) s += (a.value("inversions", false) ? " in " : " over ") + num(a["octaves"].get<double>()) + (a.value("inversions", false) ? " inversions" : " octaves");
    if (a.contains("steps") && a["steps"].is_array()) {
        size_t slots = 0;
        for (auto &st : a["steps"]) slots += st.is_object() && st.contains("len") && st["len"].is_number() ? (size_t)std::max(1.0, std::ceil(st["len"].get<double>() - 1e-6)) : 1;
        s += ", a " + std::to_string(slots) + "-step grid";
    }
    return s;
}

bool arpeggiate(const json &notes, const json &arpIn, json &out, std::string &err) {
    out = json::array();
    json arp;
    std::vector<std::string> ignored;
    if (!resolveArp(arpIn, arp, ignored, err)) return false;
    static const std::set<std::string> known = {"rate", "order", "variation", "octaves", "inversions", "gate", "lengthRandom", "swing", "velocity", "cycle", "steps", "seed"};
    for (auto &[k, v] : arp.items())
        if (!known.count(k)) { err = "arp: unknown setting '" + k + "' (it takes rate, order, variation, octaves, inversions, gate, lengthRandom, swing, velocity, cycle, steps, seed, patch, preset)"; return false; }
    double S = 0.25;
    try {
        const json &r = arp.contains("rate") ? arp["rate"] : json("1/16");
        S = r.is_number() ? r.get<double>() : Lfo::noteBeats(r.get<std::string>());
    } catch (const std::exception &) { err = "arp: rate is a note value (\"1/16\", \"1/8T\", \"1/8D\") or beats"; return false; }
    if (!(S >= 1.0 / 64)) { err = "arp: rate must be at least 1/256 of a whole note"; return false; }
    std::string order;
    int var = 1, octaves = 1, cycleN = 0;
    bool inversions = false, cycleGrid = false;
    double gate = 0.8, lengthRandom = 0, swing = 0.5, vBase = 80.0 / 127, vRange = 1, vRandom = 0;
    uint32_t seed = 1;
    std::vector<Step> steps;
    try {
        order = arp.value("order", "up");
        static const std::set<std::string> orders = {"up", "down", "updown", "downup", "outsidein", "played", "random", "chord"};
        if (!orders.count(order)) { err = "arp: order is up, down, updown, downup, outsidein, played, random or chord"; return false; }
        var = arp.value("variation", 1);
        if (var < 1 || var > 4) { err = "arp: variation is 1-4"; return false; }
        octaves = std::clamp(arp.value("octaves", 1), 1, 4);
        inversions = arp.value("inversions", false);
        gate = std::clamp(arp.value("gate", 0.8), 0.01, 1.5);
        lengthRandom = std::clamp(arp.value("lengthRandom", 0.0), 0.0, 1.0);
        swing = arp.value("swing", 0.5);
        if (!(swing >= 0.5 && swing <= 0.99)) { err = "arp: swing is 0.5 (straight) to 0.99"; return false; }
        if (arp.contains("velocity")) {   // {"base", "range", "random"}: Logic's velocity base, range and random, as 0-1
            const json &v = arp["velocity"];
            if (!v.is_object()) { err = "arp: velocity is {\"base\": 0.63, \"range\": 1, \"random\": 0}"; return false; }
            for (auto &[k, x] : v.items()) if (k != "base" && k != "range" && k != "random") { err = "arp: velocity takes base, range, random"; return false; }
            vBase = std::clamp(v.value("base", vBase), 1.0 / 127, 1.0);
            vRange = std::clamp(v.value("range", 1.0), 0.0, 1.0);
            vRandom = std::clamp(v.value("random", 0.0), 0.0, 1.0);
        }
        if (arp.contains("cycle")) {
            const json &c = arp["cycle"];
            if (c.is_string() && c == "grid") cycleGrid = true;
            else if (c.is_number_integer() && c.get<int>() >= 1 && c.get<int>() <= 32) cycleN = c.get<int>();
            else if (!(c.is_string() && c == "played")) { err = "arp: cycle is \"played\", \"grid\" or 1-32 notes"; return false; }
        }
        if (arp.contains("steps") && !parseSteps(arp["steps"], steps, err)) return false;
        seed = (uint32_t)arp.value("seed", 1);
    } catch (const json::exception &e) { err = std::string("arp: ") + e.what(); return false; }
    // the grid's slots: each step fills ceil(len) of them (a longer step ties over the next), rests one each
    struct Slot { const Step *step; int offset; };
    std::vector<Slot> slots;
    for (auto &st : steps)
        for (int o = 0, span = std::max(1, (int)std::ceil(st.len - 1e-6)); o < span; ++o) slots.push_back({&st, o});
    std::mt19937 rng(seed);   // raw outputs only: the same numbers on every platform
    auto u01 = [&] { return rng() / 4294967296.0; };

    std::vector<Held> held;
    for (size_t i = 0; i < notes.size(); ++i) {
        const json &n = notes[i];
        if (!n.is_object() || !n.contains("beat") || !n.contains("key")) continue;   // "time" notes and the like play as written
        const double b = n["beat"].get<double>();
        held.push_back({b, b + std::max(1e-3, n.value("dur", 1.0)), n.value("vel", 0.8), parseKey(n["key"]), i});
    }
    if (held.empty()) { out = notes; return true; }
    // phrases: one starts when a note goes down with none held and ends when the last is released (releases come
    // first at the same moment, so back-to-back chords start new phrases; overlapping ones carry on)
    struct Phrase { double start, end; std::vector<size_t> members; };
    std::vector<Phrase> phrases;
    std::vector<std::tuple<double, int, size_t>> ev;
    for (size_t i = 0; i < held.size(); ++i) { ev.emplace_back(held[i].start, 1, i); ev.emplace_back(held[i].end, 0, i); }
    std::sort(ev.begin(), ev.end());
    int down = 0;
    for (auto &[t, on, i] : ev) {
        if (on) {
            if (!down++) phrases.push_back({t, t, {}});
            phrases.back().members.push_back(i);
        } else if (!--down) phrases.back().end = t;
    }

    auto emit = [&](double t, double lenSteps, int key, double vel, const Held *h) {
        if (key < 0 || key > 127) return;   // octaves past the MIDI range are dropped
        double d = lenSteps * S * gate;
        if (lengthRandom > 0) d *= std::max(0.05, 1 + (u01() - 0.5) * lengthRandom);
        double b = t;
        if (swing > 0.5 && (std::lround(t / S) & 1)) b += (swing - 0.5) * 2 * S;   // the off-steps later
        if (vRange < 1 || vRandom > 0) {   // squeezed toward the base by range, randomized within that window
            const double lo = vBase - (vBase - 1.0 / 127) * vRange, hi = vBase + (1 - vBase) * vRange;
            vel = vBase + (vel - vBase) * vRange;
            if (vRandom > 0) vel = std::clamp(vel + (2 * u01() - 1) * vRandom * (hi - lo), lo, hi);
            vel = std::clamp(vel, 1.0 / 127, 1.0);
        }
        json n = notes[h->index];
        n["beat"] = std::round(b * 1e6) / 1e6;
        n["dur"] = std::round(d * 1e6) / 1e6;
        n["key"] = key;
        n["vel"] = std::round(vel * 1e4) / 1e4;
        out.push_back(n);
    };
    size_t gridNotes = 0;
    for (auto &s : slots) gridNotes += s.offset == 0 && s.step->kind == 0;
    for (auto &ph : phrases) {
        const long k0 = (long)std::ceil(ph.start / S - 0.1 - 1e-9);   // up to a tenth of a step late still starts on it
        size_t n = 0;                // note steps played in this phrase
        std::vector<Item> shuffled;  // random variation 2: a shuffled cycle, no repeats
        for (long j = 0; j == 0 || (k0 + j) * S < ph.end - 1e-9; ++j) {
            const double t = (k0 + j) * S, at = j == 0 ? ph.start : t;   // the first step plays the notes that began the phrase
            std::map<int, const Held *> byKey;   // the notes held, one per key (the latest struck)
            for (size_t m : ph.members) {
                const Held &h = held[m];
                if (h.start <= at + 1e-9 && h.end > at + 1e-9) byKey[h.key] = &h;
            }
            if (byKey.empty()) continue;
            std::vector<Item> asc, ply;
            for (auto &[k, h] : byKey) asc.push_back({k, h});
            ply = asc;
            std::stable_sort(ply.begin(), ply.end(), [](const Item &a, const Item &b) {
                return a.second->start != b.second->start ? a.second->start < b.second->start : a.second->index < b.second->index;
            });
            std::vector<Item> seq;
            if (inversions) {
                for (int i : inversionOrder(order, var)) {
                    if (i >= octaves) continue;
                    std::vector<Item> inv = asc;
                    for (int r = 0; r < i && !inv.empty(); ++r) { Item x = inv.front(); inv.erase(inv.begin()); x.first += 12; inv.push_back(x); }
                    const std::vector<Item> c = orderCycle(inv, inv, order, var);
                    seq.insert(seq.end(), c.begin(), c.end());
                }
            } else {
                std::vector<Item> ext, extPlayed;
                for (int o = 0; o < octaves; ++o) {
                    for (auto &x : asc) ext.push_back({x.first + 12 * o, x.second});
                    for (auto &x : ply) extPlayed.push_back({x.first + 12 * o, x.second});
                }
                seq = orderCycle(ext, extPlayed, order, var);
            }
            if (seq.empty()) continue;
            const Slot *gs = slots.empty() ? nullptr : &slots[(size_t)j % slots.size()];
            if (gs && (gs->offset || gs->step->kind == 2)) continue;   // tied over or a rest: the cycle waits
            const double len = gs ? std::min(gs->step->len, (double)(slots.size() - (size_t)j % slots.size())) : 1.0;
            const double stepVel = gs ? gs->step->vel : -1;
            if (gs && gs->step->kind == 1) {   // a chord step: every held key as written, the loudest at the step's velocity
                double vmax = 0;
                for (auto &x : asc) vmax = std::max(vmax, x.second->vel);
                for (auto &x : asc) emit(t, len, x.first, stepVel < 0 || vmax <= 0 ? x.second->vel : stepVel * x.second->vel / vmax, x.second);
                continue;
            }
            if (order == "chord") {
                for (auto &x : seq) emit(t, len, x.first, stepVel < 0 ? x.second->vel : stepVel, x.second);
                ++n;
                continue;
            }
            const size_t L = seq.size(), N = std::max<size_t>(1, cycleN ? (size_t)cycleN : cycleGrid ? (gridNotes ? gridNotes : (size_t)std::lround(4 / S)) : L);
            const size_t c = n % N;
            Item pick = seq[c % L];
            if (order == "random") {
                if (var == 2) {   // each cycle a new order, no repeats
                    if (shuffled.size() != L || c % L == 0) {
                        shuffled = seq;
                        for (size_t i = L - 1; i > 0; --i) std::swap(shuffled[i], shuffled[rng() % (i + 1)]);
                    }
                    pick = shuffled[c % L];
                } else if (var == 1) pick = seq[rng() % L];
                else {   // weighted toward the low (3) or high (4) notes
                    double r = u01() * (double)(L * (L + 1) / 2);
                    for (size_t i = 0; i < L; ++i) {
                        r -= (double)(var == 3 ? L - i : i + 1);
                        if (r < 0 || i + 1 == L) { pick = seq[i]; break; }
                    }
                }
            }
            emit(t, len, pick.first, stepVel < 0 ? pick.second->vel : stepVel, pick.second);
            ++n;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const json &a, const json &b) {
        const double x = a["beat"].get<double>(), y = b["beat"].get<double>();
        return x != y ? x < y : a["key"].get<int>() < b["key"].get<int>();
    });
    for (auto &n : notes) if (!(n.is_object() && n.contains("beat") && n.contains("key"))) out.push_back(n);
    return true;
}

} // namespace wl
