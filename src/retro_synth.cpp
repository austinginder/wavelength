#include "retro_synth.hpp"

#include "alchemy.hpp"
#include "apple_keys.hpp"
#include "apple_synths.hpp"
#include "logic_patches.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>

using nlohmann::json;

namespace wl {

namespace {
double r(double v, double unit = 1000) { return std::round(v * unit) / unit; }
std::string fmt(double v) { char b[32]; std::snprintf(b, sizeof b, "%g", r(v, 100)); return b; }

// the scales that await reference renders (guesses): normalized cutoff -> 20 Hz .. 20 kHz exponentially, the
// filter envelope's full depth = the whole range, the LFO's = half of it, sync 1 = 4 octaves up, FM 1 = index 8
double cutoffHz(double x) { return std::clamp(20.0 * std::pow(1000.0, std::clamp(x, 0.0, 1.0)), 20.0, 20000.0); }
const double kOctaves = std::log2(1000.0), kEnvOct = kOctaves, kLfoCutOct = kOctaves / 2, kSyncSemis = 48, kFmIndex = 8;
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
} // namespace

GarageBandSynth retroSynthPatch(const std::vector<float> &params) {
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
        for (auto &o : {o1, o2}) if (o["level"].get<double>() > 0.001) oscs.push_back(tuned(o));
        if (eng == "Table")
            notes.push_back("Table mode: wavetable positions " + fmt(V(305)) + " and " + fmt(V(306)) +
                            " play as saws (Apple's tables aren't Wavelength's to copy)");
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
            car["fm"] = {{"ratio", r(ratio)}, {"index", r(peak)}, {"decay", r(std::max(0.001, V(703) / 1000), 10000)},
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
    synth["osc"] = oscs;
    synth["filter"] = filt;
    synth["amp"] = {{"attack", r(V(802) / 1000, 100000)}, {"decay", r(V(803) / 1000, 10000)}, {"sustain", r(V(804), 10000)},
                    {"release", r(V(805) / 1000, 10000)}, {"velocity", r(V(806))}};
    synth["filterEnv"] = {{"attack", r(V(702) / 1000, 100000)}, {"decay", r(V(703) / 1000, 10000)}, {"sustain", r(V(704), 10000)}, {"release", r(V(705) / 1000, 10000)}};
    // voices: 0 mono, 1 legato, n voices; Double / unison counts with the voice detune (cents: a guess) and spread
    const int nv = (int)std::lround(V(1)), un = (int)std::lround(V(2));
    if (nv <= 1) { synth["mono"] = true; synth["legato"] = nv == 1; }
    const int count = un <= 0 ? 1 : un == 1 ? 2 : un;
    if (count > 1) synth["unison"] = {{"voices", std::min(count, 8)}, {"detune", r(std::min(100.0, V(12) * 100), 100)}, {"spread", r(V(11))}};
    // glide (0) or autobend (1) at #203-207
    if (V(203) >= 0.5) {
        if (V(204) < 0.5) { if (V(207) > 1) synth["glide"] = r(V(207) / 1000, 10000); }
        else if (std::fabs(V(206)) >= 0.01 && V(207) > 0) synth["pitchEnv"] = {{"amount", r(V(206))}, {"decay", r(std::max(0.001, V(207) / 3000), 100000)}};
    }
    // LFO -> cutoff (or pulse width), vibrato -> pitch; the mod wheel's share ("via amount") is left at rest
    json lfos = json::array();
    auto rate = [&](size_t syncId, size_t rateId) -> json { return V(syncId) >= 0.5 ? json(noteRate(V(rateId))) : json(r(V(rateId), 10000)); };
    const double lg = std::max(0.0, 1 - V(606)), vg = std::max(0.0, 1 - V(656));
    if (V(406) > 0.001 && filt.value("type", "off") != "off") {
        const double d = V(406) * kLfoCutOct * lg;
        if (d > 0.01) lfos.push_back({{"rate", rate(603, 604)}, {"depth", r(d)}, {"shape", lfoShape(V(602))}, {"to", "cutoff"}});
    }
    if (V(208) < -0.01 && (eng == "Analog" || eng == "Sync")) {
        bool pulse = false;
        for (auto &o : oscs) pulse |= o.value("wave", "") == "square";
        const double d = -V(208) * 0.45 * lg;
        if (pulse && d > 0.005) lfos.push_back({{"rate", rate(603, 604)}, {"depth", r(d)}, {"shape", lfoShape(V(602))}, {"to", "pw"}});
    } else if (V(208) > 0.01 && eng != "FM") notes.push_back("the filter envelope's modulation of the oscillator shape isn't played");
    if (V(202) > 0.001 && V(202) * vg > 0.005)
        lfos.push_back({{"rate", rate(653, 654)}, {"depth", r(V(202) * vg, 10000)}, {"shape", lfoShape(V(652))}, {"to", "pitch"}});
    if (V(606) > 0.01 || V(656) > 0.01) notes.push_back("LFO and vibrato depths under the mod wheel play at rest (wheel down)");
    if (!lfos.empty()) { if (lfos.size() > 4) lfos.erase(lfos.begin() + 4, lfos.end()); synth["lfo"] = lfos; }
    synth["level"] = r(V(5), 100);
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

bool garageBandSynthPatch(const std::string &name, GarageBandSynth &out, std::string *why) {
    const LogicPatch *p = logicPatchNamed(name);
    if (!p || p->sampler) return false;
    std::vector<PatchChannel> chans;
    std::string err;
    if (!readPatchChannels(p->path, chans, err)) return false;
    for (auto &c : chans) {
        if (c.instrument == "Alchemy") {   // its preset text: virtual-analog patches play here, the rest say why not
            AlchemyPatch a = alchemyPatch(c.alchemy, p->name);
            if (a.kind != AlchemyPatch::Synth) {
                if (why) *why = a.kind == AlchemyPatch::Sampler ? "it plays Alchemy's samples: \"plugin\": \"builtin:sampler\", \"sampler\": {\"patch\": \"" + p->name + "\"}" : a.why;
                return false;
            }
            out = a.synth;
        } else if (c.settings.params.empty()) continue;
        else if (c.instrument == "Retro Synth") out = retroSynthPatch(c.settings.params);
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
