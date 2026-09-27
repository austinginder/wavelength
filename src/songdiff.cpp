#include "songdiff.hpp"

#include "job.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

namespace wl {

namespace {

using json = nlohmann::json;

std::string show(const json &v) {
    if (v.is_null()) return "none";
    std::string s = v.is_string() ? v.get<std::string>() : v.dump(-1, ' ', false, json::error_handler_t::replace);
    return s.size() > 60 ? s.substr(0, 57) + "..." : s;
}

std::string partId(const json &p) {
    if (p.contains("id") && p["id"].is_string()) return p["id"].get<std::string>();
    return p.value("name", std::string());
}

void addBars(std::vector<std::pair<int, int>> &into, int a, int b) {
    into.push_back({std::min(a, b), std::max(a, b)});
    std::sort(into.begin(), into.end());
    std::vector<std::pair<int, int>> merged;
    for (auto &r : into)
        if (!merged.empty() && r.first <= merged.back().second + 1) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    into.swap(merged);
}

std::string barsText(const std::vector<std::pair<int, int>> &bars) {
    std::string s;
    for (auto &[a, b] : bars) s += (s.empty() ? "" : ", ") + (a == b ? std::to_string(a) : std::to_string(a) + "-" + std::to_string(b));
    return (bars.size() == 1 && bars[0].first == bars[0].second ? "bar " : "bars ") + s;
}

// ---- curves: [[beat, value], ...], {"points": [...]}, {"value": v, "lfo": ...} or a plain value ----
struct Curve {
    std::vector<std::pair<double, json>> pts;   // beat, value (a number or text)
    json lfo;                                   // compared as a whole
    bool ok = false;
};

Curve curveOf(const json &c) {
    Curve cv;
    const json *pts = nullptr;
    if (c.is_number() || c.is_string()) { cv.pts.push_back({0, c}); cv.ok = true; return cv; }
    if (c.is_object()) {
        if (c.contains("lfo")) cv.lfo = c["lfo"];
        if (c.contains("points")) pts = &c["points"];
        else if (c.contains("value")) { cv.pts.push_back({0, c["value"]}); cv.ok = true; return cv; }
    } else if (c.is_array()) pts = &c;
    if (!pts || !pts->is_array()) return cv;
    for (auto &p : *pts)
        if (p.is_array() && p.size() >= 2 && p[0].is_number()) cv.pts.push_back({p[0].get<double>(), p[1]});
        else if (p.is_object() && p.contains("beat")) cv.pts.push_back({p["beat"].get<double>(), p.value("value", json())});
    std::stable_sort(cv.pts.begin(), cv.pts.end(), [](auto &a, auto &b) { return a.first < b.first; });
    cv.ok = true;
    return cv;
}

json valueAt(const Curve &c, double beat) {
    if (c.pts.empty()) return json();
    if (beat <= c.pts.front().first) return c.pts.front().second;
    if (beat >= c.pts.back().first) return c.pts.back().second;
    for (size_t i = 1; i < c.pts.size(); ++i)
        if (beat < c.pts[i].first) {
            const auto &a = c.pts[i - 1], &b = c.pts[i];
            if (a.second.is_number() && b.second.is_number()) {
                const double t = (beat - a.first) / std::max(1e-9, b.first - a.first);
                return a.second.get<double>() + (b.second.get<double>() - a.second.get<double>()) * t;
            }
            return a.second;
        }
    return c.pts.back().second;
}

bool sameValue(const json &a, const json &b) {
    if (a.is_number() && b.is_number()) return std::fabs(a.get<double>() - b.get<double>()) < 1e-6;
    return a == b;
}

// bars where two curves differ; `whole` when an LFO or an unreadable curve changed
std::vector<std::pair<int, int>> curveBars(const json &ja, const json &jb, double bpb, int lastBar, bool &whole) {
    std::vector<std::pair<int, int>> bars;
    if (ja == jb) return bars;
    const Curve a = curveOf(ja), b = curveOf(jb);
    if (!a.ok || !b.ok || a.lfo != b.lfo) { whole = true; addBars(bars, 1, std::max(1, lastBar)); return bars; }
    std::set<double> beats;
    for (auto &p : a.pts) beats.insert(p.first);
    for (auto &p : b.pts) beats.insert(p.first);
    std::vector<double> xs(beats.begin(), beats.end());
    std::vector<double> probe = xs;
    for (size_t i = 1; i < xs.size(); ++i) probe.push_back((xs[i - 1] + xs[i]) / 2);
    std::sort(probe.begin(), probe.end());
    double prevBeat = 0;
    for (double x : probe) {
        const bool diff = !sameValue(valueAt(a, x), valueAt(b, x));
        const int bar = (int)std::floor(x / bpb + 1e-9) + 1;
        // a difference found at this probe may start right after the last probe that matched: include its bar
        if (diff) addBars(bars, x == probe.front() ? bar : (int)std::floor(prevBeat / bpb + 1e-9) + 1, bar);
        prevBeat = x;
    }
    return bars;
}

// ---- notes, per bar ----
using NoteKey = std::tuple<long, int, long, long>;   // start (1/48 beat), key, length (1/48 beat), velocity (1/100)
std::map<int, std::multiset<NoteKey>> notesByBar(const Job &job, const Track &t) {
    std::map<int, std::multiset<NoteKey>> out;
    const double bpb = job.tsigNum * 4.0 / job.tsigDen;
    for (const auto &n : t.notes) {
        const double b = job.tempo.secToBeat(n.start), e = job.tempo.secToBeat(n.start + n.length);
        out[(int)std::floor(b / bpb + 1e-9) + 1].insert({std::lround(b * 48), n.key, std::lround((e - b) * 48), std::lround(n.velocity * 100)});
    }
    return out;
}

int lastBarOf(const Job &job) {
    const double bpb = job.tsigNum * 4.0 / job.tsigDen;
    double end = 0;
    for (auto &t : job.tracks) for (auto &n : t.notes) end = std::max(end, job.tempo.secToBeat(n.start + n.length));
    return (int)std::ceil(end / bpb);
}

const char *kSoundKeys[] = {"plugin", "preset", "state", "params", "synth", "sampler", "shepard", "transpose", "articulations",
                            "velocityTo", "fallback", "warmup", "bendRange", "roll"};
const char *kMixKeys[] = {"gain", "pan", "mute", "output", "sends", "stem", "harmony", "groove"};

// a setting's change, naming the keys of an object that changed
void compareKey(const json &a, const json &b, const std::string &key, std::vector<std::string> &lines) {
    const json va = a.contains(key) ? a[key] : json(), vb = b.contains(key) ? b[key] : json();
    if (va == vb) return;
    if (va.is_object() && vb.is_object()) {
        std::string keys;
        std::set<std::string> all;
        for (auto &[k, v] : va.items()) all.insert(k);
        for (auto &[k, v] : vb.items()) all.insert(k);
        for (auto &k : all) {
            const json x = va.contains(k) ? va[k] : json(), y = vb.contains(k) ? vb[k] : json();
            if (x != y) keys += (keys.empty() ? "" : "; ") + k + " " + show(x) + " -> " + show(y);
        }
        lines.push_back(key + ": " + keys);
    } else lines.push_back(key + " " + show(va) + " -> " + show(vb));
}

void compareFx(const json &fa, const json &fb, double bpb, int lastBar, PartDiff &p, std::vector<std::string> *songLines) {
    const json a = fa.is_array() ? fa : json::array(), b = fb.is_array() ? fb : json::array();
    auto say = [&](const std::string &s) { if (songLines) songLines->push_back(s); else p.changes.push_back(s); };
    if (a.size() != b.size()) { say("fx: " + std::to_string(a.size()) + " -> " + std::to_string(b.size()) + " effects"); p.whole = true; }
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        const json &x = a[i], &y = b[i];
        if (x == y) continue;
        const std::string ta = x.value("type", std::string("plugin")), tb = y.value("type", std::string("plugin"));
        const std::string where = "fx[" + std::to_string(i) + "] " + tb;
        if (ta != tb) { say("fx[" + std::to_string(i) + "] " + ta + " -> " + tb); p.whole = true; continue; }
        std::vector<std::string> lines;
        std::set<std::string> keys;
        for (auto &[k, v] : x.items()) keys.insert(k);
        for (auto &[k, v] : y.items()) keys.insert(k);
        for (auto &k : keys) {
            if (k == "automate" || k == "lfo") continue;
            const size_t before = lines.size();
            compareKey(x, y, k, lines);
            if (lines.size() > before) p.whole = true;
        }
        const json autoA = x.value("automate", json::object()), autoB = y.value("automate", json::object());
        std::set<std::string> params;
        for (auto &[k, v] : autoA.items()) params.insert(k);
        for (auto &[k, v] : autoB.items()) params.insert(k);
        for (auto &k : params) {
            bool whole = false;
            const auto bars = curveBars(autoA.contains(k) ? autoA[k] : json(), autoB.contains(k) ? autoB[k] : json(), bpb, lastBar, whole);
            if (bars.empty()) continue;
            for (auto &[s, e] : bars) addBars(p.bars, s, e);
            lines.push_back(k + " automation (" + barsText(bars) + ")");
        }
        if (x.value("lfo", json()) != y.value("lfo", json())) { lines.push_back("lfo"); p.whole = true; }
        std::string joined;
        for (auto &l : lines) joined += (joined.empty() ? "" : ", ") + l;
        if (!joined.empty()) say(where + ": " + joined);
    }
}

void compareAutomation(const json &a, const json &b, double bpb, int lastBar, PartDiff &p) {
    const json aa = a.is_object() ? a : json::object(), ab = b.is_object() ? b : json::object();
    std::set<std::string> keys;
    for (auto &[k, v] : aa.items()) keys.insert(k);
    for (auto &[k, v] : ab.items()) keys.insert(k);
    for (auto &k : keys) {
        const json x = aa.contains(k) ? aa[k] : json(), y = ab.contains(k) ? ab[k] : json();
        if (x == y) continue;
        // params and cc hold one curve per name; rides may be one curve or named curves
        const bool nested = (k == "params" || k == "cc" || (k == "rides" && ((x.is_object() && !x.contains("points") && !x.contains("value")) ||
                                                                             (y.is_object() && !y.contains("points") && !y.contains("value")))));
        std::vector<std::pair<std::string, std::pair<json, json>>> curves;
        if (nested) {
            const json ox = x.is_object() ? x : json::object(), oy = y.is_object() ? y : json::object();
            std::set<std::string> names;
            for (auto &[n, v] : ox.items()) names.insert(n);
            for (auto &[n, v] : oy.items()) names.insert(n);
            for (auto &n : names) curves.push_back({k + "." + n, {ox.contains(n) ? ox[n] : json(), oy.contains(n) ? oy[n] : json()}});
        } else curves.push_back({k, {x, y}});
        for (auto &[name, pair] : curves) {
            bool whole = false;
            const auto bars = curveBars(pair.first, pair.second, bpb, lastBar, whole);
            if (bars.empty()) continue;
            for (auto &[s, e] : bars) addBars(p.bars, s, e);
            p.changes.push_back("automation " + name + " (" + barsText(bars) + ")");
        }
    }
}

std::string describe(const json &t) {
    std::string s = t.value("plugin", std::string());
    if (t.contains("preset") && t["preset"].is_string()) s += " '" + t["preset"].get<std::string>() + "'";
    return s;
}

} // namespace

bool diffJobs(const json &ja, const json &jb, const std::string &baseDir, SongDiff &d, std::string &err) {
    d = SongDiff{};
    Job a, b;
    if (!parseJob(ja, baseDir, a, err, false)) { err = "the older job: " + err; return false; }
    if (!parseJob(jb, baseDir, b, err, false)) { err = "the newer job: " + err; return false; }
    const double bpb = b.tsigNum * 4.0 / b.tsigDen;
    const int lastBar = std::max(lastBarOf(a), lastBarOf(b));

    for (const char *k : {"tempo", "timeSignature", "keys", "chords", "groove", "length", "tail", "sampleRate", "leadIn"}) compareKey(ja, jb, k, d.song);
    {   // markers by name
        std::map<std::string, double> ma, mb;
        for (auto &m : ja.value("markers", json::array())) ma[m.value("name", std::string())] = m.value("beat", 0.0);
        for (auto &m : jb.value("markers", json::array())) mb[m.value("name", std::string())] = m.value("beat", 0.0);
        auto bar = [&](double beat) { return std::to_string((int)std::floor(beat / bpb + 1e-9) + 1); };
        for (auto &[n, beat] : mb)
            if (!ma.count(n)) d.song.push_back("marker '" + n + "' added at bar " + bar(beat));
            else if (std::fabs(ma[n] - beat) > 1e-9) d.song.push_back("marker '" + n + "' moved from bar " + bar(ma[n]) + " to bar " + bar(beat));
        for (auto &[n, beat] : ma) if (!mb.count(n)) d.song.push_back("marker '" + n + "' removed (was at bar " + bar(beat) + ")");
    }
    {   // master
        const json xa = ja.value("master", json::object()), xb = jb.value("master", json::object());
        if (xa != xb) {
            PartDiff p;
            std::vector<std::string> lines;
            for (const char *k : {"gain", "loudness", "loudnessGain"}) compareKey(xa, xb, k, lines);
            for (auto &l : lines) d.song.push_back("master " + l);
            compareFx(xa.value("fx", json::array()), xb.value("fx", json::array()), bpb, lastBar, p, &d.song);
            for (auto &l : d.song) if (l.rfind("fx", 0) == 0) l = "master " + l;
            compareAutomation(xa.value("automation", json::object()), xb.value("automation", json::object()), bpb, lastBar, p);
            for (auto &c : p.changes) d.song.push_back("master " + c);
        }
    }
    // tracks and buses, matched by ID (or name)
    auto parts = [&](const char *list, const char *kind) {
        const json la = ja.value(list, json::array()), lb = jb.value(list, json::array());
        std::map<std::string, size_t> ia, ib;
        for (size_t i = 0; i < la.size(); ++i) ia[partId(la[i])] = i;
        for (size_t i = 0; i < lb.size(); ++i) ib[partId(lb[i])] = i;
        for (size_t i = 0; i < lb.size(); ++i) {
            const json &y = lb[i];
            PartDiff p;
            p.id = partId(y);
            p.name = y.value("name", p.id);
            p.kind = kind;
            if (!ia.count(p.id)) {
                p.status = "added";
                p.whole = true;
                if (std::string(kind) == "track") p.changes.push_back(describe(y));
                d.parts.push_back(p);
                continue;
            }
            const json &x = la[ia[p.id]];
            p.status = "changed";
            if (x.value("name", std::string()) != p.name) p.changes.push_back("renamed from '" + x.value("name", std::string()) + "'");
            const size_t before = p.changes.size();
            for (const char *k : kSoundKeys) compareKey(x, y, k, p.changes);
            for (const char *k : kMixKeys) compareKey(x, y, k, p.changes);
            if (p.changes.size() > before) p.whole = true;
            compareFx(x.value("fx", json::array()), y.value("fx", json::array()), bpb, lastBar, p, nullptr);
            compareAutomation(x.value("automation", json::object()), y.value("automation", json::object()), bpb, lastBar, p);
            if (std::string(kind) == "track") {
                if (x.value("clips", json::array()) != y.value("clips", json::array())) { p.changes.push_back("audio clips changed"); p.whole = true; }
                // notes, bar by bar (the parsed jobs keep the tracks in the same order as the JSON)
                const Track *ta = ia[p.id] < a.tracks.size() ? &a.tracks[ia[p.id]] : nullptr, *tb = i < b.tracks.size() ? &b.tracks[i] : nullptr;
                if (ta && tb) {
                    auto na = notesByBar(a, *ta), nb = notesByBar(b, *tb);
                    std::set<int> barsAll;
                    for (auto &[bar, s] : na) barsAll.insert(bar);
                    for (auto &[bar, s] : nb) barsAll.insert(bar);
                    std::vector<std::pair<int, int>> changed;
                    size_t ca = 0, cb = 0;
                    for (int bar : barsAll)
                        if (na[bar] != nb[bar]) { addBars(changed, bar, bar); ca += na[bar].size(); cb += nb[bar].size(); }
                    if (!changed.empty()) {
                        for (auto &[s, e] : changed) addBars(p.bars, s, e);
                        p.changes.push_back("notes in " + barsText(changed) + " (" + std::to_string(ca) + " -> " + std::to_string(cb) + " notes)");
                    }
                }
            }
            if (!p.changes.empty()) d.parts.push_back(p);
        }
        for (auto &[id, i] : ia)
            if (!ib.count(id)) {
                PartDiff p;
                p.id = id; p.name = la[i].value("name", id); p.kind = kind; p.status = "removed"; p.whole = true;
                d.parts.push_back(p);
            }
    };
    parts("tracks", "track");
    parts("buses", "bus");
    return true;
}

json diffToJson(const SongDiff &d) {
    json parts = json::array();
    for (auto &p : d.parts) {
        json bars = json::array();
        for (auto &[a, b] : p.bars) bars.push_back({a, b});
        parts.push_back({{"id", p.id}, {"name", p.name}, {"kind", p.kind}, {"status", p.status}, {"changes", p.changes}, {"bars", bars}, {"whole", p.whole}});
    }
    return {{"song", d.song}, {"parts", parts}};
}

std::string diffToText(const SongDiff &d) {
    std::string out;
    for (auto &l : d.song) out += l + "\n";
    for (auto &p : d.parts) {
        out += p.kind + " '" + p.name + "'";
        if (p.status != "changed") out += " " + p.status;
        std::string ch;
        for (auto &c : p.changes) ch += (ch.empty() ? "" : "; ") + c;
        out += ch.empty() ? "\n" : (p.status == "changed" ? ": " : " (") + ch + (p.status == "changed" ? "" : ")") + "\n";
    }
    return out.empty() ? "no changes\n" : out;
}

bool diffTouches(const SongDiff &d, const std::vector<std::string> &parts, int bar0, int bar1, std::vector<std::string> &why) {
    for (auto &l : d.song)
        if (l.rfind("tempo", 0) == 0 || l.rfind("timeSignature", 0) == 0 || l.rfind("master", 0) == 0) why.push_back(l);
    for (auto &p : d.parts) {
        if (!parts.empty() && std::find(parts.begin(), parts.end(), p.id) == parts.end() && std::find(parts.begin(), parts.end(), p.name) == parts.end())
            continue;
        std::string ch;
        for (auto &c : p.changes) ch += (ch.empty() ? "" : "; ") + c;
        if (p.status != "changed") { why.push_back(p.kind + " '" + p.name + "' " + p.status); continue; }
        bool hit = p.whole;
        for (auto &[a, b] : p.bars) hit |= !(b < bar0 || a > bar1);
        if (hit) why.push_back(p.kind + " '" + p.name + "': " + ch);
    }
    return !why.empty();
}

} // namespace wl
