#include "retro_synth.hpp"

#include "alchemy.hpp"
#include "apple_keys.hpp"
#include "apple_synths.hpp"
#include "bplist.hpp"
#include "logic_patches.hpp"
#include "xml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

using nlohmann::json;

namespace fs = std::filesystem;

namespace wl {

namespace {
double r(double v, double unit = 1000) { return std::round(v * unit) / unit; }
std::string fmt(double v) { char b[32]; std::snprintf(b, sizeof b, "%g", r(v, 100)); return b; }

// the scales, measured on a GarageBand bounce of eight Analog patches (C3, C4 and C5 at velocity 100): normalized cutoff
// -> 24 Hz .. 24 kHz exponentially (a quarter octave over 20 Hz .. 20 kHz; less velocity darkening would fit as well,
// one velocity can't tell them apart), the filter envelope's full depth 8.5 octaves, and decay and release times the
// time to fall 20 dB (a tenth of the way), where builtin:synth's reach 99%: they play kEnvTime as long (attacks as
// given). Still guesses: the LFO's cutoff depth (half the cutoff range), sync 1 = 4 octaves up, FM 1 = index 8
double cutoffHz(double x) { return std::clamp(24.0 * std::pow(1000.0, std::clamp(x, 0.0, 1.0)), 20.0, 20000.0); }
const double kOctaves = std::log2(1000.0), kEnvOct = 8.5, kLfoCutOct = kOctaves / 2, kSyncSemis = 48, kFmIndex = 8, kEnvTime = 2;
// an oscillator's start phase: Retro Synth restarts its oscillators at each note with their fundamentals in phase
// (a saw's half a cycle on, a triangle's a quarter), so unison voices start together and beat apart
json phased(json o) {
    const std::string w = o.value("wave", "saw");
    o["phase"] = w == "saw" ? 0.5 : w == "triangle" ? 0.25 : 0.0;
    return o;
}
double fmRatio(double harmonic, double inharmonic) { return std::round(1 + 15 * harmonic) + 0.5 * inharmonic; }

// a tempo-synced rate (a note length in whole notes) as "num/den"
std::string noteRate(double v) {
    int bn = 1, bd = 1;
    double best = 1e9;
    for (int d = 1; d <= 96; ++d) {
        const int n = (int)std::lround(v * d);
        if (n > 0 && std::fabs((double)n / d - v) < best - 1e-12) { best = std::fabs((double)n / d - v); bn = n; bd = d; }
    }
    return std::to_string(bn) + "/" + std::to_string(bd);
}
const char *waveName(double code) {
    switch ((int)std::lround(code)) { case 0: return "triangle"; case 2: return "square"; case 3: return "noise"; default: return "saw"; }
}
const char *lfoShape(double code) {
    switch ((int)std::lround(code)) { case 1: case 2: return "square"; case -1: return "ramp"; case -2: return "saw"; case 4: return "random"; default: return "triangle"; }
}

// Table mode. A wavetable is a list of waves, each kept as its harmonics' amplitudes (float32, harmonic 1 first, at
// most 384, trailing zeros left out; a unit-peak saw's fundamental is 2/pi). A patch on a wavetable carries all of it
// in its settings block, in a "1PTW" chunk after the values: a binary property list {Spectra: [data per wave], UUID,
// Version}. Patches without one play Retro Synth's built-in Digiwaves, which no data file holds.
struct RetroTable {
    std::string uuid;
    std::vector<std::vector<float>> waves;
};

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool retroTable(const std::vector<uint8_t> &block, RetroTable &t) {
    if (block.size() < 24) return false;
    for (size_t at = 24 + 4 * (size_t)le32(&block[8]); at + 8 <= block.size();) {   // the chunks after the values
        const uint32_t size = le32(&block[at + 4]);
        if (size < 8 || at + size > block.size()) break;
        json pl;
        if (!std::memcmp(&block[at], "1PTW", 4) && parseBinaryPlist(&block[at + 8], size - 8, pl, true) && pl.is_object() &&
            pl.contains("Spectra") && pl["Spectra"].is_array()) {
            for (auto &w : pl["Spectra"]) {
                if (!w.is_binary()) continue;
                const auto &b = w.get_binary();
                std::vector<float> a(std::min<size_t>(b.size() / 4, 1024));
                for (size_t k = 0; k < a.size(); ++k) {
                    const uint32_t bits = le32(&b[4 * k]);
                    std::memcpy(&a[k], &bits, 4);
                    if (!std::isfinite(a[k]) || a[k] < 0) a[k] = 0;
                }
                t.waves.push_back(std::move(a));
            }
            if (pl.contains("UUID") && pl["UUID"].is_string()) t.uuid = pl["UUID"].get<std::string>();
            return !t.waves.empty();
        }
        at += size;
    }
    return false;
}

// Apple's wavetables' names by UUID, from RetroSynthWavetable.plist in the installed Retro Synth settings folder
// (read once, names only; empty when it isn't installed)
const std::map<std::string, std::string> &retroTableNames() {
    static std::map<std::string, std::string> names;
    static std::once_flag once;
    std::call_once(once, [] {
        for (auto &root : pluginSettingsRoots()) {
            std::ifstream in(fs::u8path(root) / "Retro Synth" / "RetroSynthWavetable.plist", std::ios::binary);
            if (!in) continue;
            std::stringstream text;
            text << in.rdbuf();
            std::string err;
            const auto doc = xml::parse(text.str(), err);
            const xml::Node *top = doc ? doc->child("dict") : nullptr;
            if (!top) continue;
            for (size_t i = 0; i + 1 < top->children.size(); ++i) {   // <key>name</key> <dict>... <key>UUID</key> <string>
                const xml::Node &k = *top->children[i], &d = *top->children[i + 1];
                if (k.tag != "key" || d.tag != "dict") continue;
                for (size_t j = 0; j + 1 < d.children.size(); ++j)
                    if (d.children[j]->tag == "key" && d.children[j]->text == "UUID" && d.children[j + 1]->tag == "string")
                        names[d.children[j + 1]->text] = k.text;
            }
            if (!names.empty()) break;
        }
    });
    return names;
}

// the table's wave at position x (0..1 over its waves), between its two neighbours
std::vector<double> retroWave(const RetroTable &t, double x) {
    const size_t n = t.waves.size();
    const double p = std::clamp(x, 0.0, 1.0) * (double)(n - 1);
    const size_t i = std::min(n - 1, (size_t)p);
    const double f = p - (double)i;
    std::vector<double> a;
    auto add = [&](const std::vector<float> &w, double g) {
        if (a.size() < w.size()) a.resize(w.size());
        for (size_t k = 0; k < w.size(); ++k) a[k] += g * w[k];
    };
    add(t.waves[i], 1 - f);
    if (f > 1e-9 && i + 1 < n) add(t.waves[i + 1], f);
    return a;
}

// an additive oscillator playing harmonic amplitudes `a` at `level`: partials 80 dB under the loudest are left out,
// and a wave holding more power than one full sine is scaled to it with the level carrying the rest (builtin:synth
// scales such lists down), so the amplitudes play as stored. A silent wave gives a silent oscillator.
json retroAdditive(const std::vector<double> &a, double level) {
    double top = 0, power = 0;
    for (double v : a) { top = std::max(top, v); power += v * v; }
    if (top <= 0) return {{"wave", "sine"}, {"level", 0.0}};
    const double s = std::max(1.0, std::sqrt(power));
    json parts = json::array();
    for (size_t k = 0; k < a.size(); ++k)
        if (a[k] > 1e-4 * top) parts.push_back({r(a[k] / s, 1e6), (int)k + 1});
    return {{"wave", "additive"}, {"partials", parts}, {"level", r(level * s)}};
}
} // namespace

GarageBandSynth retroSynthPatch(const std::vector<float> &params, const std::vector<uint8_t> &block) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "Retro Synth";
    auto &notes = out.notes;
    static const char *engines[4] = {"Analog", "Sync", "Table", "FM"};
    const int e = (int)std::lround(V(201));
    out.engine = e >= 0 && e < 4 ? engines[e] : "Analog";
    const std::string &eng = out.engine;
    json synth = json::object(), oscs = json::array();
    const double mix = V(209), tune = V(4);
    auto tuned = [&](json o) { if (tune != 0) o["cents"] = r(o.value("cents", 0.0) + tune, 100); return o; };
    if (eng == "Analog" || eng == "Table") {
        json o1 = {{"wave", eng == "Table" ? "saw" : waveName(V(301))}, {"level", r(1 - mix)}};
        json o2 = {{"wave", eng == "Table" ? "saw" : waveName(V(303))}, {"level", r(mix)}, {"semi", (int)std::lround(V(307))}, {"cents", r(V(308), 100)}};
        if (o1["wave"] == "square") o1["pw"] = r(std::max(0.02, V(302) / 100));
        if (o2["wave"] == "square") o2["pw"] = r(std::max(0.02, V(304) / 100));
        RetroTable tab;
        if (eng == "Table" && retroTable(block, tab)) {   // Shape 1 and 2: positions in the patch's own wavetable
            json a2 = retroAdditive(retroWave(tab, V(306)), mix);
            a2["semi"] = o2["semi"];
            a2["cents"] = o2["cents"];
            o1 = retroAdditive(retroWave(tab, V(305)), 1 - mix);
            o2 = a2;
            const auto &names = retroTableNames();
            const auto it = names.find(tab.uuid);
            notes.push_back("Table mode: the patch's wavetable" + (it != names.end() ? " \"" + it->second + "\"" : std::string()) + " (" +
                            std::to_string(tab.waves.size()) + " waves) at positions " + fmt(V(305)) + " and " + fmt(V(306)) + ", as additive oscillators");
            if (V(319) >= 0.5) { if (std::fabs(V(208)) > 0.01) notes.push_back("Table mode: the formant stretch isn't played"); }
            else if (V(208) < -0.01 && -V(208) * std::max(0.0, 1 - V(606)) > 0.005)
                notes.push_back("Table mode: the LFO's sweep through the wavetable isn't played: each oscillator holds its position");
        } else if (eng == "Table")
            notes.push_back("Table mode: Retro Synth's built-in Digiwaves (positions " + fmt(V(305)) + " and " + fmt(V(306)) +
                            ") aren't in GarageBand's data files: saws stand in");
        for (auto &o : {o1, o2}) if (o["level"].get<double>() > 0.001) oscs.push_back(tuned(o));
    } else if (eng == "Sync") {   // the first oscillator is the master; the second restarts with it, tuned up by the sync amount
        json o1 = {{"wave", waveName(V(309))}, {"level", r(1 - mix)}};
        json o2 = {{"wave", waveName(V(311))}, {"level", r(mix)}, {"semi", r(V(313) * kSyncSemis, 100)}, {"sync", true}};
        if (o1["wave"] == "square") o1["pw"] = r(std::max(0.02, V(310) / 100));
        if (o2["wave"] == "square") o2["pw"] = r(std::max(0.02, V(312) / 100));
        oscs.push_back(tuned(o1));   // kept at any level: the second follows its cycle
        if (mix > 0.001) oscs.push_back(tuned(o2));
        notes.push_back("Sync: the synced oscillator sits " + std::to_string((int)std::lround(V(313) * kSyncSemis)) + " semitones up (sync amount " + fmt(V(313)) + ", scale a guess)");
    } else {   // FM: a sine carrier, its sine modulator at the ratio the harmonic and inharmonic controls give
        const double ratio = fmRatio(V(315), V(316)), idx = V(314) * kFmIndex, sm = V(208), tgt = V(318);
        json car = {{"wave", "sine"}, {"level", r(1 - mix)}, {"fm", {{"ratio", r(ratio)}, {"index", r(idx)}}}};
        if (sm > 0.01 && tgt < 0.75) {   // filter envelope -> FM amount: the FM index envelope
            const double peak = idx + sm * kFmIndex;
            car["fm"] = {{"ratio", r(ratio)}, {"index", r(peak)}, {"decay", r(std::max(0.001, V(703) / 1000 * kEnvTime), 10000)},
                         {"sustain", r(peak > 0 ? (idx + sm * kFmIndex * V(704)) / peak : 0)}};
        } else if (std::fabs(sm) > 0.01) notes.push_back("FM: modulation of the FM amount or harmonic isn't played");
        oscs.push_back(tuned(car));
        if (mix > 0.001) oscs.push_back(tuned({{"wave", V(317) > 0.5 ? "saw" : "sine"}, {"level", r(mix)}, {"semi", r(12 * std::log2(std::max(0.01, ratio)), 100)}}));
        notes.push_back("FM: ratio " + fmt(ratio) + " and index " + fmt(idx) + " from guessed scales");
    }
    if (V(210) > 0.001) oscs.push_back(tuned({{"wave", "sine"}, {"level", r(V(210))}, {"filter", false}}));   // Sine Level: after the filter
    // filter: type 0/1 and the modeled types (9+) low-pass 24, 2/3 low-pass 12, 4 high-pass, 5 band-pass, 6/7 band reject and
    // peak as an eq band; cutoff, resonance, key follow, envelope depth (octaves), velocity
    json filt = json::object();
    const int ftype = (int)std::lround(V(402));
    const double fc = cutoffHz(V(403)), envOct = std::clamp(V(407) * kEnvOct, -8.0, 8.0);
    if (V(401) < 0.5 || ftype == 6 || ftype == 7) filt["type"] = "off";
    else {
        if (ftype <= 1 || ftype >= 8) filt = {{"type", "lowpass"}, {"slope", 24}};
        else if (ftype <= 3) filt = {{"type", "lowpass"}, {"slope", 12}};
        else filt = {{"type", ftype == 4 ? "highpass" : "bandpass"}, {"slope", 12}};
        filt["cutoff"] = r(fc, 10);
        filt["resonance"] = r(V(404));
        filt["keytrack"] = r(V(405));
        if (std::fabs(envOct) > 0.001) filt["env"] = r(envOct);
        if (ftype >= 8 && V(408) > 0.001) filt["drive"] = r(std::min(1.0, V(408)));
        else if (ftype < 8 && std::fabs(V(408)) > 0.01) { filt["drive"] = r(std::min(1.0, 0.3 * std::fabs(V(408)))); notes.push_back("Filter FM played as drive"); }
        double velOct = 0;
        if (envOct > 0 && V(706) > 0) velOct += envOct * V(706) * 0.5;
        if ((int)std::lround(V(102)) == 2 && V(103) > 0) velOct += V(103) * 4;
        if (velOct > 0.01) filt["velocity"] = r(std::min(4.0, velOct));
    }
    for (auto &o : oscs) o = phased(o);
    synth["osc"] = oscs;
    synth["filter"] = filt;
    synth["amp"] = {{"attack", r(V(802) / 1000, 100000)}, {"decay", r(V(803) / 1000 * kEnvTime, 10000)}, {"sustain", r(V(804), 10000)},
                    {"release", r(V(805) / 1000 * kEnvTime, 10000)}, {"velocity", r(V(806))}};
    synth["filterEnv"] = {{"attack", r(V(702) / 1000, 100000)}, {"decay", r(V(703) / 1000 * kEnvTime, 10000)}, {"sustain", r(V(704), 10000)},
                          {"release", r(V(705) / 1000 * kEnvTime, 10000)}};
    // voices: 0 mono, 1 legato, n voices; Double / unison counts with the voice detune and spread. The voices start in
    // phase and add up (count voices play 10 log10(count) dB over one: builtin:synth keeps the level of one), detuned
    // 40 x^1.5 cents from lowest to highest (0.42: 11 cents, 0.12: 1.7, from two bounced patches' beats)
    const int nv = (int)std::lround(V(1)), un = (int)std::lround(V(2));
    if (nv <= 1) { synth["mono"] = true; synth["legato"] = nv == 1; }
    const int count = std::min(8, un <= 0 ? 1 : un == 1 ? 2 : un);
    if (count > 1)
        synth["unison"] = {{"voices", count}, {"detune", r(std::min(100.0, 40 * std::pow(std::max(0.0, V(12)), 1.5)), 100)}, {"spread", r(V(11))}};
    // glide (0) or autobend (1) at #203-207
    if (V(203) >= 0.5) {
        if (V(204) < 0.5) { if (V(207) > 1) synth["glide"] = r(V(207) / 1000, 10000); }
        else if (std::fabs(V(206)) >= 0.01 && V(207) > 0) synth["pitchEnv"] = {{"amount", r(V(206))}, {"decay", r(std::max(0.001, V(207) / 3000), 100000)}};
    }
    // LFO -> cutoff (or pulse width), vibrato -> pitch; #606 and #656 are the mod wheel's share of each depth ("via
    // amount"): the rest plays with the wheel down, the whole depth with it up (an LFO's "wheel" depth)
    json lfos = json::array();
    auto rate = [&](size_t syncId, size_t rateId) -> json { return V(syncId) >= 0.5 ? json(noteRate(V(rateId))) : json(r(V(rateId), 10000)); };
    const double lw = std::clamp(V(606), 0.0, 1.0), vw = std::clamp(V(656), 0.0, 1.0);
    auto lfo = [&](json l, double full, double share, double digits) {
        l["depth"] = r(full * (1 - share), digits);
        if (share > 0.01) l["wheel"] = r(full, digits);
        lfos.push_back(l);
    };
    if (V(406) > 0.001 && filt.value("type", "off") != "off") {
        const double d = V(406) * kLfoCutOct;
        if (d * std::max(1 - lw, lw) > 0.01) lfo({{"rate", rate(603, 604)}, {"shape", lfoShape(V(602))}, {"to", "cutoff"}}, d, lw, 1000);
    }
    if (V(208) < -0.01 && (eng == "Analog" || eng == "Sync")) {
        bool pulse = false;
        for (auto &o : oscs) pulse |= o.value("wave", "") == "square";
        const double d = -V(208) * 0.45;
        if (pulse && d > 0.005) lfo({{"rate", rate(603, 604)}, {"shape", lfoShape(V(602))}, {"to", "pw"}}, d, lw, 1000);
    } else if (V(208) > 0.01 && eng != "FM" && !(eng == "Table" && V(319) >= 0.5)) notes.push_back("the filter envelope's modulation of the oscillator shape isn't played");
    if (V(202) > 0.005)
        lfo({{"rate", rate(653, 654)}, {"shape", lfoShape(V(652))}, {"to", "pitch"}}, V(202), vw, 10000);
    if (lw > 0.01 || vw > 0.01) notes.push_back("the mod wheel brings in its share of the LFO and vibrato depths (" + std::to_string((int)std::lround(100 * lw)) + " % and " + std::to_string((int)std::lround(100 * vw)) + " %)");
    if (!lfos.empty()) { if (lfos.size() > 4) lfos.erase(lfos.begin() + 4, lfos.end()); synth["lfo"] = lfos; }
    synth["level"] = r(V(5) + 10 * std::log10((double)count), 100);
    out.synth = synth;
    out.transpose = (int)std::lround(V(3));
    // effects: Retro Synth's chorus or flanger (a short chorus), and a band-reject / peak filter type as an eq band
    if (V(401) >= 0.5 && (ftype == 6 || ftype == 7))
        out.fx.push_back({{"type", "eq"}, {"bands", json::array({{{"type", "peak"}, {"freq", r(fc, 10)}, {"q", r(0.7 + 8 * V(404), 100)},
                                                                   {"gain", ftype == 6 ? -24.0 : r(6 + 12 * V(404), 10)}}})}});
    if (V(501) >= 0.5 && V(504) > 0.01) {
        json c = {{"type", "chorus"}, {"rate", r(std::max(0.05, V(505)))}, {"mix", r(V(504) * 0.5)}};
        if ((int)std::lround(V(502)) == 1) { c["delay"] = 2; c["depth"] = 1.5; notes.push_back("its flanger plays as a short chorus"); }
        out.fx.push_back(c);
    }
    return out;
}

namespace {
// A low- or band-pass that reads as closed at middle C even with its envelope fully open, in front of nearly all the
// level: what opens it in GarageBand (an unmapped control or modulation, a filter type only inferred) isn't re-created,
// and the patch would play choked (A Simpler Time read as a 20 Hz band-pass that its effects then drove into fuzz).
// Its cutoff in Hz, else 0. With a chain of filters, any of them that takes most of the voice.
double closedFilterHz(const nlohmann::json &s) {
    if (!s.contains("filter")) return 0;
    const nlohmann::json list = s["filter"].is_array() ? s["filter"] : nlohmann::json::array({s["filter"]});
    for (size_t k = 0; k < list.size(); ++k) {
        const auto &f = list[k];
        if (!f.is_object()) continue;
        const std::string type = f.value("type", "lowpass");
        if (type != "lowpass" && type != "bandpass") continue;
        const double cut = f.value("cutoff", 20000.0), env = std::max(0.0, f.value("env", 0.0));
        if (cut * std::pow(2.0, env) >= 80 || f.value("mix", 1.0) < 0.8) continue;
        double in = 0, all = 0;   // the oscillators' level through the filter (joining the chain at it or before), and in all
        for (auto &o : s.value("osc", nlohmann::json::array())) {
            const double l = o.value("level", 1.0);
            const auto &j = o.contains("filter") ? o["filter"] : nlohmann::json(true);
            all += l;
            if (j.is_number() ? j.get<double>() <= (double)k : j.is_boolean() && j.get<bool>()) in += l;
        }
        if (all > 0 && all - in < 0.2 * all) return cut;   // unless a fifth of the level skips it (80s Sine Synth: its sine)
    }
    return 0;
}
} // namespace

bool retroSynthParam(int param, double value, std::string &name, double &out) {
    switch (param) {
    case 403: name = "cutoff"; out = r(cutoffHz(value), 10); return true;
    case 404: name = "resonance"; out = r(std::clamp(value, 0.0, 1.0)); return true;
    case 407: name = "env"; out = r(std::clamp(value * kEnvOct, -8.0, 8.0)); return true;
    default: return false;
    }
}

bool garageBandSynthPatch(const std::string &name, GarageBandSynth &out, std::string *why) {
    // a patch by name, or a patch folder by path (an imported GarageBand project's track: its own channel strip)
    LogicPatch byPath;
    const LogicPatch *p = logicPatchNamed(name);
    std::error_code ec;
    if (!p && fs::is_directory(fs::u8path(name), ec)) {
        byPath.name = fs::u8path(name).stem().u8string();
        byPath.path = name;
        p = &byPath;
    }
    if (!p) return false;
    std::vector<PatchChannel> chans;
    std::string err;
    if (!readPatchChannels(p->path, chans, err)) return false;
    if (p == &byPath)
        for (auto &c : chans) byPath.sampler |= c.sampler;
    if (p->sampler) return false;
    for (auto &c : chans) {
        if (c.instrument == "Alchemy") {   // its preset text: virtual-analog patches play here, the rest say why not
            AlchemyPatch a = alchemyPatch(c.alchemy, p->name);
            if (a.kind != AlchemyPatch::Synth) {
                if (why) *why = a.kind == AlchemyPatch::Sampler ? "it plays Alchemy's samples: \"plugin\": \"builtin:sampler\", \"sampler\": {\"patch\": \"" + p->name + "\"}" : a.why;
                return false;
            }
            out = a.synth;
        } else if (c.settings.params.empty()) continue;
        else if (c.instrument == "Retro Synth") out = retroSynthPatch(c.settings.params, c.settings.block);
        else if (c.instrument == "Vintage B3") out = vintageB3Patch(c.settings.params);
        else if (c.instrument == "ES2") out = es2Patch(c.settings.params);
        else if (c.instrument == "ES1") out = es1Patch(c.settings.params);
        else if (c.instrument == "EFM1") out = efm1Patch(c.settings.params);
        else if (c.instrument == "E-Piano") out = vintageEPPatch(c.settings.params);
        else if (c.instrument == "Clav") out = vintageClavPatch(c.settings.params);
        else if (c.instrument == "Sculpture") {   // refused when its sound is side-chain audio or movement a static voice can't play
            std::string refused;
            out = sculpturePatch(c.settings.params, refused);
            if (!refused.empty()) { if (why) *why = refused; return false; }
        } else continue;
        if (const double hz = closedFilterHz(out.synth); hz > 0) {
            std::string type = "lowpass";
            for (auto &f : out.synth["filter"].is_array() ? out.synth["filter"] : nlohmann::json::array({out.synth["filter"]}))
                if (f.is_object() && f.value("cutoff", 20000.0) == hz) type = f.value("type", type);
            if (why) *why = "its filter reads as closed (a " + std::to_string((int)std::lround(hz)) + " Hz " + type +
                            " at middle C, its envelope open) with the controls at rest: what opens it in GarageBand isn't re-created";
            return false;
        }
        out.name = p->name;
        std::vector<std::string> fxNotes;
        for (auto &f : patchChainEffects(chans, fxNotes)) out.fx.push_back(f);
        out.notes.insert(out.notes.end(), fxNotes.begin(), fxNotes.end());
        return true;
    }
    return false;
}

std::string garageBandSynthKind(const GarageBandSynth &g) {
    return g.instrument + (g.engine.empty() ? "" : " (" + g.engine + (g.instrument == "Retro Synth" ? " mode)" : ")"));
}

const std::vector<std::pair<std::string, std::string>> &garageBandSynthPatches() {
    static std::vector<std::pair<std::string, std::string>> list;
    static std::once_flag once;
    std::call_once(once, [] {
        for (auto &p : logicPatches()) {
            if (p.sampler) continue;
            GarageBandSynth g;
            if (garageBandSynthPatch(p.name, g))
                list.push_back({p.name, garageBandSynthKind(g) + (p.arpeggiator || !g.arp.is_null() ? ", arpeggiated" : "")});
        }
    });
    return list;
}

} // namespace wl
