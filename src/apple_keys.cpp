#include "apple_keys.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>

using nlohmann::json;

#ifdef __clang__
#pragma clang fp contract(off)   // no fused multiply-adds: values round exactly as the decoder's do, ties included
#endif

namespace wl {

namespace {
// v rounded to 1/unit as the decoder (Python's round()) does: the exact value decides, an exact half goes to even
double r(double v, double unit = 1000) {
    const double x = v * unit, lo = std::floor(x);
    if (x - lo != 0.5) return std::round(x) / unit;
    const double lost = std::fma(v, unit, -x);   // what the product rounded away
    return (lost < 0 ? lo : lost > 0 ? lo + 1 : std::fmod(lo, 2) == 0 ? lo : lo + 1) / unit;
}
int ri(double v) { return (int)std::nearbyint(v); }   // a stored switch or count (halves to even, as the decoder)
std::string num(double v, int digits, bool sign = false) { char b[48]; std::snprintf(b, sizeof b, sign ? "%+.*f" : "%.*f", digits, v); return b; }
std::string gnum(double v) { char b[48]; std::snprintf(b, sizeof b, "%g", v); return b; }
std::string inum(double v) { return std::to_string((long long)v); }   // a value printed whole (cut toward zero)
// a sum as the decoder's: Neumaier's compensated summation (Python's sum() of floats)
template <class C> double fsum(const C &xs) {
    double s = 0, c = 0;
    for (double x : xs) {
        const double t = s + x;
        c += std::fabs(s) >= std::fabs(x) ? (s - t) + x : (x - t) + s;
        s = t;
    }
    return c != 0 && std::isfinite(c) ? s + c : s;
}
double lerp(double a, double b, double t) { return a + (b - a) * t; }
// guess A1 (as apple_synths.cpp): a normalized 0..1 cutoff spans 20 Hz .. 20 kHz exponentially
double cutoffHz(double x) { return 20.0 * std::pow(1000.0, std::clamp(x, 0.0, 1.0)); }
const double kVel = 0.8;

// a tempo-synced rate stored as a fraction of a whole note (0.25 = a quarter, 0.1875 = a dotted eighth) as a note
// value ("1/4", "3/16"): the nearest fraction with a denominator up to 64; "" when none is within 0.002
std::string noteValue(double v) {
    if (!(v > 0 && v <= 8)) return "";
    double bn = 0, best = 1e9;
    int bd = 1;
    for (int d = 1; d <= 64; ++d) {
        const double n = std::round(v * d), e = std::fabs(n / d - v);
        if (e < best) { best = e; bn = n; bd = d; }
    }
    return best > 0.002 ? "" : std::to_string((long long)bn) + "/" + std::to_string(bd);
}
// a rate in Hz, or a note value when synced ("1/2" when the stored value isn't one)
json rateOf(double v, bool synced, std::vector<std::string> &notes, const std::string &what) {
    if (synced) {
        const std::string nv = noteValue(v);
        if (!nv.empty()) return nv;
        notes.push_back(what + ": synced rate " + gnum(v) + " isn't a note value; \"1/2\" used");
        return "1/2";
    }
    return r(std::max(0.01, v));
}
// guess: a pre-drive Tone knob (Hz) as a high-shelf cut at 1.5 kHz, 0 dB at its top, -12 dB at its bottom (null: none)
json toneShelf(double hz, double lo = 180, double hi = 10000) {
    if (hz >= hi * 0.95) return nullptr;
    const double t = std::log(hi / std::clamp(hz, lo, hi)) / std::log(hi / lo);
    return {{"type", "highshelf"}, {"freq", 1500}, {"gain", r(-12 * t, 10)}};
}
json adsr(double a, double d, double s, double rel, double vel = -1) {
    json o = {{"attack", r(std::max(0.0, a), 10000)}, {"decay", r(std::max(0.001, d))}, {"sustain", r(std::clamp(s, 0.0, 1.0))},
              {"release", r(std::max(0.005, rel))}};
    if (vel >= 0) o["velocity"] = r(std::clamp(vel, 0.0, 1.0), 100);
    return o;
}
// how close a re-creation gets, as the decoder judged it
std::string closeness(bool approximate) { return approximate ? "an approximate re-creation" : "a rough re-creation"; }

// ---- Vintage Electric Piano
// model index -> family and model; Apple's guide names the models but not their menu order, so each index is
// identified by the factory settings that use it (1, 4, 8, 10, 11 and 19 up are unused: perhaps Metal, Attack and
// Funk Piano among them; they play as a generic tine piano)
struct EPModel { int index; const char *family, *what; bool sure; };
const EPModel kEPModels[] = {
    {0, "tine", "Rhodes Mk I (suitcase or stage)", true}, {2, "tine", "Rhodes Mk II stage?", false}, {3, "tine", "Rhodes Mk II stage?", false},
    {5, "tine", "Rhodes suitcase (\"Mk IV\")", false}, {6, "reed", "Wurlitzer 200A", true}, {7, "reed", "Wurlitzer", false},
    {9, "electra", "Hohner Electra Piano", true}, {12, "tine", "Rhodes suitcase Mk II", false}, {13, "tine", "Rhodes suitcase, bright", false},
    {14, "reed", "Wurlitzer 240V", true}, {15, "tine", "Rhodes Mk I classic (the default model)", true}, {16, "reed", "Wurlitzer, modern", true},
    {17, "reed", "Wurlitzer, classic", true}, {18, "tine", "Rhodes Mk II, bright", false}};
// guess: relative tine brightness by model
double epBright(int model) { return model == 13 || model == 18 ? 1.25 : model == 2 || model == 3 ? 1.12 : model == 12 ? 1.08 : 1.0; }
// the pickup's bark: harmonics 2-8 of GarageBand's Classic Electric Piano (tine) and Wurlitzer Classic (reed) at C3,
// measured from a bounce (t10), relative to the strongest
const double kTineBark[7] = {1.0, 0.55, 0.62, 0.385, 0.19, 0.22, 0.038};
const double kReedBark[7] = {0.30, 1.0, 0.51, 0.22, 0.355, 0.157, 0.105};
json barkPartials(const double (&a)[7]) {
    json p = json::array();
    for (int k = 0; k < 7; ++k) p.push_back({a[k], k + 2});
    return p;
}

// ---- Vintage Clav
// model index -> family (14 models; Apple's guide names them but not their order: identified by the factory settings)
const char *clavFamily(int model) {
    static const char *f[14] = {"classic", "classic", "classic", "funk", "classic", "classic", "mellow", "harpsichord", "wood",
                                "harpsichord", "sitar", "wood", "dulcimer", "harp"};
    return model >= 0 && model < 14 ? f[model] : nullptr;
}
const char *clavModel(int model) {
    static const char *w[14] = {"Classic I (the D6)", "Classic II or Vintage I?", "a bright Classic or Belltone?", "Funktone?", "unidentified",
                                "Vintage II?", "Mellotone?", "Harpsitone I?", "Woodtone or Plectratone?", "Harpsitone II?", "Sitartone",
                                "Woodtone or Plectratone?", "Dulcitone or Belltone?", "Plectratone (harp-like)"};
    return model >= 0 && model < 14 ? w[model] : "unknown";
}

// ---- Sculpture
// the 22 morphable values, at #443 + 27 k for morph point k (0 the centre, 1..4 the corners A..D)
enum { kStiff, kInner, kMedia, kTension, kO1Pos, kO1Str, kO1Timbre, kO1Var, kPickA = 16, kPickB, kWsIn, kWsVar, kCutoff, kReso, kMorphCount };
using Morph = std::array<double, kMorphCount>;
// guess: the centre fades out with the distance max(|x|, |y|) and the corners (A (-1, +1), B (+1, +1), C (-1, -1),
// D (+1, -1): confirmed by the presets whose edit buffer sits at a corner) blend bilinearly
std::vector<double> morphWeights(double x, double y) {
    const double rr = std::clamp(std::max(std::fabs(x), std::fabs(y)), 0.0, 1.0);
    static const double corners[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
    std::vector<double> w = {1 - rr};
    for (auto &c : corners) w.push_back(rr * (1 + c[0] * std::clamp(x, -1.0, 1.0)) * (1 + c[1] * std::clamp(y, -1.0, 1.0)) / 4);
    return w;
}
template <class F> Morph morphValues(const F &V, const std::vector<double> &w) {
    Morph m{};
    for (size_t i = 0; i < m.size(); ++i) {
        std::vector<double> terms;
        for (size_t k = 0; k < w.size(); ++k) terms.push_back(w[k] * V(443 + 27 * k + i));
        m[i] = fsum(terms);
    }
    return m;
}
// object types 1..8 excite the string, 9..14 disturb or damp it (11-13 read as Bouncing, Mass and Bound), 15 External
const char *objectName(int t) {
    static const char *n[16] = {"#0", "Impulse", "Strike", "GravStrike", "Pick", "Bow", "Bow Wide", "Blow", "Noise", "Disturb", "Disturb 2-sided",
                                "Bouncing?", "Mass?", "Bound?", "Damp", "External"};
    return t >= 0 && t < 16 ? n[t] : "?";
}
const char *objectClass(int t) {
    static const char *c[9] = {"", "plucked", "struck", "struck", "plucked", "bowed", "bowed", "blown", "noise"};
    return t >= 1 && t <= 8 ? c[t] : "";
}
std::string targetName(int t) {
    switch (t) {
    case 5: return "Object 1 Strength";
    case 25: return "Pickup A Position";
    case 26: return "Pickup B Position";
    case 27: return "Pickup A+B Position?";
    case 33: return "Waveshaper Input Scale?";
    case 34: return "Waveshaper Variation?";
    case 35: return "Filter Cutoff";
    case 36: return "Filter Resonance?";
    default: return "target #" + std::to_string(t);
    }
}
// the modulation routes: source, its enable, target and amount parameters (the LFOs, jitter, note-on random,
// control envelopes, velocity and Ctrl A/B share one target list)
struct Route { const char *src; size_t on, tgt, amt; };
const Route kRoutes[] = {{"LFO1", 140, 141, 143}, {"LFO1", 145, 146, 148}, {"LFO2", 158, 159, 161}, {"LFO2", 163, 164, 166},
                         {"Jitter1", 181, 182, 184}, {"Jitter1", 186, 187, 189}, {"Jitter2", 199, 200, 202}, {"Jitter2", 204, 205, 207},
                         {"NoteOnRnd1", 209, 210, 211}, {"NoteOnRnd2", 212, 213, 214}, {"Env1", 221, 222, 224}, {"Env1", 226, 227, 229},
                         {"Env2", 271, 272, 274}, {"Env2", 276, 277, 279}, {"Velo1", 123, 124, 125}, {"Velo2", 127, 128, 129},
                         {"CtrlA", 415, 416, 417}, {"CtrlA", 419, 420, 421}, {"CtrlB", 423, 424, 425}, {"CtrlB", 427, 428, 429}};
const double kEnvCutOct = 5;   // guess: an envelope or LFO amount of 1 on Filter Cutoff = 5 octaves

// guess: seconds for a free string to fall ~40 dB: Media Loss 0 ~9 s, 0.5 ~1.1 s, 1 0.14 s; Inner Loss shortens it by
// up to half (its losses also dull the tone: the filter envelope)
double stringDecay(double media, double inner) {
    return std::clamp(9.0 * std::exp(-4.2 * std::clamp(media, 0.0, 1.0)) * (1 - 0.5 * std::clamp(inner, 0.0, 1.0)), 0.04, 20.0);
}
// a control envelope (Env 1 at #220, Env 2 at #270): its points (seconds, level), sustain point, whether it runs (run
// mode bit 2) and whether it holds a recorded curve
struct CtrlEnv { bool run, recorded, finish; int sus; std::array<std::array<double, 2>, 5> pts; };
template <class F> CtrlEnv ctrlEnv(const F &V, size_t base) {
    CtrlEnv e{};
    e.pts[0] = {0.0, V(base + 22)};
    double t = 0;
    const double scale = V(base + 44) > 0 ? V(base + 44) / 100.0 : 1.0;
    for (size_t k = 0; k < 4; ++k) {
        t += std::max(0.0, V(base + 23 + 4 * k)) / 1000.0 * scale;
        e.pts[k + 1] = {t, V(base + 26 + 4 * k)};
    }
    e.run = (ri(V(base)) & 2) != 0;
    e.sus = ri(V(base + 40));
    e.recorded = V(base + 16) >= 1.5;
    e.finish = ri(V(base + 39)) == 2;
    return e;
}
// a point envelope as an ADSR: attack to its peak, decay to the sustain point's level, release to the end; a sustain
// point at or before the peak, or sustain mode 2 (finish: a guess), runs through every point (null: no peak)
json envAdsr(const CtrlEnv &e, double &peak) {
    size_t pi = 0;
    for (size_t i = 1; i < e.pts.size(); ++i) if (std::fabs(e.pts[i][1]) > std::fabs(e.pts[pi][1])) pi = i;
    peak = e.pts[pi][1];
    if (std::fabs(peak) < 1e-6) { peak = 0; return nullptr; }
    const size_t sp = (size_t)std::clamp(e.sus, 0, 4);
    const double a = e.pts[pi][0], end = e.pts[4][0];
    if (sp <= pi || e.finish) return adsr(a, std::max(0.005, end - a), std::clamp(e.pts[4][1] / peak, 0.0, 1.0), 0.1);
    const double rel = sp < 4 ? std::max(0.01, end - e.pts[sp][0]) : 0.3;
    return adsr(a, std::max(0.005, e.pts[sp][0] - a), std::clamp(e.pts[sp][1] / peak, 0.0, 1.0), rel);
}
} // namespace

GarageBandSynth vintageEPPatch(const std::vector<float> &params) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "Vintage Electric Piano";
    auto &notes = out.notes;
    const int model = ri(V(0));
    const EPModel *m = nullptr;
    for (auto &x : kEPModels) if (x.index == model) m = &x;
    const std::string fam = m ? m->family : "tine";
    out.engine = fam == "electra" ? "Electra" : fam;
    const double bright = epBright(model);
    // Decay 130 (its default) = an 11.5 s fall at C4 on a tine, 6.8 s on a reed (fitted, t10), 4.5 s on the Electra (a guess),
    // scaling with Decay^0.75 (a guess); Release 100 = 0.12 s
    const double decay = std::clamp((fam == "tine" ? 11.5 : fam == "reed" ? 6.8 : 4.5) * std::pow(std::max(V(1), 10.0) / 130.0, 0.75), 0.25, 40.0);
    const double rel = std::clamp(0.12 * std::max(V(2), 5.0) / 100.0, 0.02, 3.0);
    const double bell = std::max(0.0, V(3));
    json oscs = json::array(), filt, amp;
    // fitted to GarageBand's Classic Electric Piano and Wurlitzer Classic at C3, C4 and C5 (t10): the pickup's bark
    // (the measured harmonics, through the filter) rules the low keys and fades above C3; the body, a sine with a
    // ratio-1 FM edge that skips the filter, grows up to F#4; lower keys ring longer
    if (fam == "tine") {
        // the bark is gone by C4; plus the tine bell: a ratio-14 FM "ding" (sidebands at 13f and 15f) dying within a few
        // hundred ms, its level from Bell Volume
        oscs.push_back({{"wave", "sine"}, {"filter", false}, {"keytrack", 16.6}, {"keycenter", 66},
                        {"fm", {{"ratio", 1}, {"index", r(1.89 * bright, 100)}, {"decay", r(std::clamp(0.155 * std::sqrt(decay), 0.1, 2.0))}, {"sustain", 0.22}}}});
        oscs.push_back({{"wave", "additive"}, {"partials", barkPartials(kTineBark)}, {"level", 0.41}, {"keytrack", -31.5}, {"keycenter", 48}, {"decay", r(1.72 * decay)}});
        if (bell > 0.02)
            oscs.push_back({{"wave", "sine"}, {"level", r(std::min(0.5, 0.14 * std::pow(bell, 0.7)))}, {"decay", r(std::clamp(0.3 * std::pow(bell, 0.25), 0.08, 0.8))},
                            {"filter", false}, {"fm", {{"ratio", 14}, {"index", r(std::min(3.0, 0.9 + 0.25 * bell), 100)}, {"decay", 0.05}}}});
        filt = {{"type", "lowpass"}, {"slope", 12}, {"cutoff", r(1910 * bright, 10)}, {"velocity", 1.0}};
        amp = adsr(0.002, decay, 0, rel, 0.75);
        amp["keytrack"] = 1;
    } else if (fam == "reed") {
        // the reed's odd-rich bark fades slowly (about 2 dB an octave) and dies within about 1.6 s; Bell Volume moves
        // it (a guess, from the Wurlitzer Classic's 1.18)
        oscs.push_back({{"wave", "sine"}, {"filter", false}, {"keytrack", 12.1}, {"keycenter", 66},
                        {"fm", {{"ratio", 1}, {"index", 1.64}, {"decay", r(std::clamp(0.127 * std::sqrt(decay), 0.05, 1.5))}, {"sustain", 0.22}}}});
        oscs.push_back({{"wave", "additive"}, {"partials", barkPartials(kReedBark)}, {"level", r(0.8 * std::pow(std::clamp(bell, 0.3, 2.5) / 1.18, 0.3))},
                        {"keytrack", -2.1}, {"keycenter", 48}, {"decay", r(std::clamp(0.625 * std::sqrt(decay), 0.3, 4.0))}});
        filt = {{"type", "lowpass"}, {"slope", 12}, {"cutoff", 1050.0}, {"velocity", 2.2}};
        amp = adsr(0.002, decay, 0, rel * 0.8, 0.8);
        amp["keytrack"] = 0.78;
        notes.push_back("the reed as a sine body and its measured bark under a velocity-opened filter (builtin:synth has no velocity -> FM)");
    } else {   // Electra: brighter, faster-decaying, harmonically rich
        oscs.push_back({{"wave", "sine"}, {"fm", {{"ratio", 1}, {"index", 2.0}, {"decay", 0.3}, {"sustain", 0.2}}}});
        oscs.push_back({{"wave", "saw"}, {"level", 0.25}, {"decay", r(std::clamp(0.25 * std::pow(decay, 0.5), 0.1, 1.0))}});
        filt = {{"type", "lowpass"}, {"slope", 12}, {"cutoff", 3500.0}, {"keytrack", 0.5}, {"velocity", 1.5}};
        amp = adsr(0.002, decay * 0.5, 0, rel, 0.8);
    }
    if (std::fabs(V(9)) >= 0.5) for (auto &o : oscs) o["cents"] = r(V(9), 10);
    json synth = {{"osc", oscs}, {"filter", filt}, {"amp", amp}};
    if (ri(V(8)) == 1) synth["mono"] = true;
    // a level per family (see the header): the piano's own Volume isn't applied
    synth["level"] = fam == "tine" ? -5.6 : fam == "reed" ? -3.5 : -4.7;
    out.synth = synth;
    const bool sure = m && m->sure;
    notes.insert(notes.begin(), "model " + std::to_string(model) + " = " + (m ? m->what : "unused by the factory settings, played as a generic tine piano") +
                                    ": a " + out.engine + " voice" + (m && !sure ? " (model identification uncertain)" : ""));
    notes.insert(notes.begin(), closeness(fam == "tine" && m));
    notes.push_back("Decay " + gnum(V(1)) + " -> amp decay " + num(decay, 2) + " s, Release " + gnum(V(2)) + " -> " + num(rel, 2) +
                    " s at C4 (scales a guess)" + (fam == "electra" ? "; no key scaling of the decay" : "; low keys ring longer (fitted)"));
    if (V(4) > 0.05) notes.push_back("Damper Volume " + num(V(4), 2) + " (felt noise at key-up) isn't played");
    if (V(10) > 0.05 || std::fabs(V(5)) > 0.05 || std::fabs(V(6)) > 0.05)
        notes.push_back("Warmth " + num(V(10), 2) + " and stretch tuning " + num(V(5), 2) + ", " + num(V(6), 2) + " (per-note detune) aren't played");
    if (V(7) > 5) notes.push_back("Stereo " + inum(V(7)) + " % (bass left, treble right) isn't played: one pan for all keys");
    // effects in the piano's order: drive, EQ (after the drive), phaser, tremolo, chorus
    if (V(31) >= 0.5 && V(13) > 0.3) {
        const json sh = toneShelf(V(14));
        if (!sh.is_null()) out.fx.push_back({{"type", "eq"}, {"bands", json::array({sh})}});
        out.fx.push_back({{"type", "saturate"}, {"drive", r(std::min(24.0, 1.6 * V(13)), 10)}, {"match", true}});
        notes.push_back("Drive Gain " + gnum(V(13)) + " read as " + num(std::min(24.0, 1.6 * V(13)), 1) + " dB of tanh drive, Tone " + gnum(V(14)) +
                        " Hz as a high-shelf cut (guesses); Drive Mode " + inum(V(38)) + " ignored");
    }
    json bands = json::array();
    if (V(30) >= 0.5) {
        const int lf = fam == "tine" ? 150 : 200, hf = fam == "tine" ? 3500 : 2500;
        if (std::fabs(V(12)) >= 0.25) bands.push_back({{"type", "lowshelf"}, {"freq", lf}, {"gain", r(V(12), 10)}});
        if (std::fabs(V(11)) >= 0.25) bands.push_back({{"type", "highshelf"}, {"freq", hf}, {"gain", r(V(11), 10)}});
        if (!bands.empty())
            notes.push_back("EQ Bass and Treble as shelves at " + std::to_string(lf) + " Hz and " + std::to_string(hf) + " Hz (the piano picks shelf or peak and "
                            "the frequency per model: guesses)");
    }
    if (V(35) > 1) {
        bands.push_back({{"type", "lowshelf"}, {"freq", 100}, {"gain", r(3 * V(35) / 100.0, 10)}});
        notes.push_back("Bass Boost " + inum(V(35)) + " % read as up to +3 dB at 100 Hz (a guess)");
    }
    if (!bands.empty()) out.fx.push_back({{"type", "eq"}, {"bands", bands}});
    if (V(32) >= 0.5 && V(15) > 0)
        out.fx.push_back({{"type", "phaser"}, {"rate", rateOf(V(15), V(36) >= 0.5, notes, "phaser")}, {"stages", 4},
                          {"feedback", r(0.85 * std::clamp(V(16), 0.0, 100.0) / 100.0, 100)}, {"spread", r(std::clamp(V(17), 0.0, 180.0) / 360.0)}, {"mix", 0.5}});
    if (V(33) >= 0.5 && V(19) > 0.5)
        out.fx.push_back({{"type", "tremolo"}, {"rate", rateOf(V(18), V(37) >= 0.5, notes, "tremolo")}, {"depth", r(std::clamp(V(19), 0.0, 100.0) / 100.0, 100)},
                          {"spread", r(std::clamp(V(20), 0.0, 180.0) / 360.0)}});
    if (V(34) >= 0.5 && V(21) > 0.5) {
        out.fx.push_back({{"type", "chorus"}, {"rate", r(std::max(0.05, V(26)), 100)}, {"depth", r(1 + 4 * V(21) / 100.0, 100)}, {"delay", 7},
                          {"mix", r(0.25 + 0.25 * std::min(1.0, V(21) / 100.0), 100)}});
        notes.push_back("Chorus intensity " + inum(V(21)) + " as 1..5 ms of depth, mix 0.25..0.5 (a guess)");
    }
    return out;
}

GarageBandSynth vintageClavPatch(const std::vector<float> &params) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "Vintage Clav";
    auto &notes = out.notes;
    const int model = ri(V(10));
    const std::string fam = clavFamily(model) ? clavFamily(model) : "classic";
    out.engine = fam;
    const double damper = std::clamp(V(7), 0.0, 1.0);
    // the pickups: the pulse width plays the harmonic comb of a pickup at fraction x of the string (min(x, 1 - x));
    // Pickup Mode 0..3 (= AB + 2 CD) read as Lower, Upper, Lower-Upper (out of phase), Lower+Upper (order a guess)
    const int mode = ri(V(28));
    const double lower = (V(33) + V(34)) / 200.0, upper = (V(35) + V(36)) / 200.0;
    const double pw = std::clamp(mode == 0 ? std::min(lower, 1 - lower) : mode == 1 ? std::min(upper, 1 - upper)
                                                                         : (std::min(lower, 1 - lower) + std::min(upper, 1 - upper)) / 2, 0.06, 0.5);
    json oscs = json::array();
    double baseCut = 0, envOct = 0, fdec = 0;
    if (fam == "classic" || fam == "funk" || fam == "mellow") {
        oscs.push_back({{"wave", "square"}, {"pw", r(pw)}});
        oscs.push_back({{"wave", "saw"}, {"level", fam != "mellow" ? 0.45 : 0.2}});
        baseCut = fam != "mellow" ? 2600 : 1500, envOct = fam != "mellow" ? 3.0 : 2.0, fdec = fam != "mellow" ? 0.35 : 0.3;
    } else if (fam == "harpsichord") {
        oscs.push_back({{"wave", "saw"}});
        oscs.push_back({{"wave", "square"}, {"pw", r(pw)}, {"level", 0.5}, {"octave", 1}});
        baseCut = 4500, envOct = 1.5, fdec = 0.6;
    } else if (fam == "sitar") {
        oscs.push_back({{"wave", "saw"}});
        oscs.push_back({{"wave", "square"}, {"pw", 0.12}, {"level", 0.4}});
        baseCut = 3500, envOct = 2.0, fdec = 0.8;
    } else if (fam == "wood") {
        oscs.push_back({{"wave", "triangle"}});
        oscs.push_back({{"wave", "sine"}, {"fm", {{"ratio", 3.5}, {"index", 1.5}, {"decay", 0.12}}}, {"level", 0.4}});
        baseCut = 2000, envOct = 2.5, fdec = 0.12;
    } else if (fam == "dulcimer") {
        oscs.push_back({{"wave", "square"}, {"pw", r(pw)}, {"level", 0.6}});
        oscs.push_back({{"wave", "sine"}, {"fm", {{"ratio", 2.0}, {"index", 1.2}, {"decay", 0.4}, {"sustain", 0.1}}}});
        baseCut = 3000, envOct = 2.0, fdec = 0.3;
    } else {   // harp
        oscs.push_back({{"wave", "triangle"}});
        oscs.push_back({{"wave", "square"}, {"pw", r(pw)}, {"level", 0.3}});
        baseCut = 2200, envOct = 1.5, fdec = 0.25;
    }
    // Brilliance and Shape brighten the strike, String Damping mellows it, Damper mutes it (scales a guess)
    const double cut = baseCut * std::pow(2.0, 1.2 * V(14)) * std::pow(2.0, -1.0 * V(21)) * std::pow(2.0, -2.0 * damper);
    json filt = {{"type", "lowpass"}, {"slope", fam == "classic" || fam == "funk" ? 24 : 12}, {"cutoff", r(std::clamp(cut, 200.0, 16000.0), 10)}, {"keytrack", 0.6},
                 {"env", r(envOct * (1 + 0.3 * V(13)), 100)}, {"velocity", r(std::clamp(1.2 + 0.6 * V(13), 0.2, 3.0), 100)}};
    if (fam == "funk") filt["resonance"] = 0.25;
    const json fenv = adsr(0, std::clamp(fdec * std::pow(2.0, -1.2 * V(21)) * (1 - 0.6 * damper), 0.03, 2.0), 0, 0.1);
    // String Decay -1..1 scales the ring (about 2.5 s), Damper shortens it; the string stops when the key comes up
    double dec = std::clamp(2.5 * std::pow(2.0, 2.0 * V(22)) * (1 - 0.85 * damper), 0.08, 20.0);
    if (fam == "harp" || fam == "dulcimer" || fam == "sitar") dec *= 1.8;
    const double rel = std::clamp(0.06 * std::pow(2.0, 3.0 * V(25)), 0.02, 4.0);
    json synth = {{"filter", filt}, {"filterEnv", fenv}, {"amp", adsr(0.001, dec, 0, rel, 0.75)}};
    if (V(23) > 0.3 && fam != "wood" && fam != "dulcimer") {   // String Stiffness: an inharmonic partial (a guess)
        oscs.push_back({{"wave", "sine"}, {"level", r(0.3 * V(23), 100)}, {"fm", {{"ratio", r(1 + 2.5 * V(23), 100)}, {"index", 1.0}, {"decay", 0.2}}}, {"decay", 0.6}});
        notes.push_back("String Stiffness " + num(V(23), 2) + " as an inharmonic FM partial (a guess)");
    }
    if (V(20) > 0.05) {
        synth["pitchEnv"] = {{"amount", r(0.3 * V(20), 100)}, {"decay", 0.08}};
        notes.push_back("String Tension Mod " + num(V(20), 2) + " as a +" + num(0.3 * V(20), 2) + " semitone pitch drop-in over 80 ms (a guess)");
    }
    if (std::fabs(V(2)) >= 0.5) for (auto &o : oscs) o["cents"] = r(V(2), 10);
    synth["osc"] = oscs;
    if (ri(V(1)) <= 1) synth["mono"] = true;
    // a level per family (see the header), moved by 10 % of the Clav's own Level
    static const std::pair<const char *, double> levels[8] = {{"classic", -2.8}, {"dulcimer", -6.1}, {"funk", -4.1}, {"harp", -5.4},
                                                                {"harpsichord", -2.9}, {"mellow", -2.6}, {"sitar", 2.9}, {"wood", -4.9}};
    double base = 0;
    for (auto &[f, l] : levels) if (fam == f) base = l;
    synth["level"] = r(std::clamp(base + 0.1 * V(9), -40.0, 12.0), 100);
    out.synth = synth;
    notes.insert(notes.begin(), "model " + std::to_string(model) + " = " + clavModel(model) + ": a " + fam + " voice (the family per model a guess)");
    notes.insert(notes.begin(), closeness(fam == "classic" || fam == "funk" || fam == "mellow"));
    notes.push_back("pickups (mode " + std::to_string(mode) + ", lower " + num(100 * lower, 0) + " %, upper " + num(100 * upper, 0) + " %) as pulse width " + num(pw, 2) +
                    " (the comb of a pickup at that string fraction)");
    notes.push_back("String Decay " + num(V(22), 2) + ", Damper " + num(damper, 2) + " -> amp decay " + num(dec, 2) + " s; String Release " + num(V(25), 2) + " -> " +
                    num(rel, 2) + " s (scales a guess)");
    if (V(15) > -0.95) notes.push_back("the release click (intensity " + num(V(15), 2) + ") isn't played");
    if (V(26) > -0.95) notes.push_back("Pitch Fall " + num(V(26), 2) + " at key-up isn't played");
    if (V(3) > 0.05 || V(5) > 0.05) notes.push_back("Warmth and stretch tuning aren't played");
    if (V(31) > 0.05 || V(32) > 0.05) notes.push_back("the pickup and key stereo spreads aren't played");
    // the tone switches as EQ: #init has all four on, so "all on" must be a full sound. Read (a guess) as the D6's
    // switched capacitors: each bass cut alone a high-pass (Brilliant 500 Hz, Treble 250 Hz, Medium 120 Hz), engaged
    // together their capacitors add, so the cut is 1 / sum(1 / f) (all three: 70 Hz); Soft a gentle treble shelf
    json bands = json::array();
    std::vector<double> caps;
    for (auto [i, f] : {std::pair<size_t, double>{38, 500.0}, {39, 250.0}, {40, 120.0}}) if (V(i) >= 0.5) caps.push_back(1.0 / f);
    if (!caps.empty()) {
        const double hp = 1.0 / fsum(caps);
        if (hp > 60) bands.push_back({{"type", "highpass"}, {"freq", r(hp, 10)}, {"q", 0.6}});
    }
    if (V(41) >= 0.5) bands.push_back({{"type", "highshelf"}, {"freq", 2500}, {"gain", -6}});
    if (mode == 2) {
        bands.push_back({{"type", "lowshelf"}, {"freq", 300}, {"gain", -6}});
        notes.push_back("out-of-phase pickups (mode 2, if the order guess holds) thinned with a -6 dB low shelf");
    }
    if (!bands.empty()) {
        out.fx.push_back({{"type", "eq"}, {"bands", bands}});
        notes.push_back(std::string("Brilliant/Treble/Medium/Soft (") + (V(38) >= 0.5 ? "1" : "0") + (V(39) >= 0.5 ? "1" : "0") + (V(40) >= 0.5 ? "1" : "0") +
                        (V(41) >= 0.5 ? "1" : "0") + ") as a high-pass at 1 / sum(1 / f) of the engaged bass cuts and a treble shelf (a guess)");
    }
    // the effects section (Fx Bypass 1: all off), in a fixed order: wah, compressor, distortion, modulation (a guess)
    if (V(55) < 0.5) {
        if (V(56) >= 0.5) {
            const int wm = ri(V(61));   // 0 Classic, 1 Retro, 2 Modern, 3 Opto 1, 4 Opto 2, 5 Resonant LP, 6 Resonant HP, 7 Peak
            const double range = std::clamp(V(45), 0.1, 2.0), lo = 350.0, hi = 350.0 * std::pow(2.0, 1.5 + 1.5 * range);
            if (wm >= 0 && wm <= 5 && V(48) > 0.5) {
                out.fx.push_back({{"type", "autowah"}, {"min", r(lo, 10)}, {"max", r(std::min(hi, 6000.0), 10)}, {"resonance", wm != 5 ? 3 : 4},
                                  {"sensitivity", r(std::clamp(V(48) / 2, 0.0, 12.0), 10)}, {"mode", wm == 5 ? "lowpass" : "bandpass"}, {"mix", 1}});
                notes.push_back("wah mode " + std::to_string(wm) + " with Env Depth " + num(V(48), 1) + " as an auto-wah " + num(lo, 0) + ".." +
                                num(std::min(hi, 6000.0), 0) + " Hz (sensitivity scale a guess)");
            } else {
                const double f0 = lo * std::pow(2.0, std::clamp(V(47), 0.0, 1.0) * (1.5 + 1.5 * range));
                if (wm == 6) out.fx.push_back({{"type", "filter"}, {"mode", "highpass"}, {"cutoff", r(f0, 10)}, {"resonance", 2.0}});
                else if (wm == 5) out.fx.push_back({{"type", "filter"}, {"mode", "lowpass"}, {"cutoff", r(f0, 10)}, {"resonance", 2.0}});
                else out.fx.push_back({{"type", "eq"}, {"bands", json::array({{{"type", "peak"}, {"freq", r(f0, 10)}, {"q", 3}, {"gain", 9}}})}});
                notes.push_back("wah mode " + std::to_string(wm) + " fixed at pedal " + num(V(47), 2) + " (" + num(f0, 0) + " Hz): its pedal controller isn't followed");
            }
        }
        if (V(58) >= 0.5 && V(49) > 1.05) {
            out.fx.push_back({{"type", "compressor"}, {"threshold", -22}, {"ratio", r(V(49), 10)}, {"attack", 3}, {"release", 120},
                              {"makeup", r(std::min(10.0, 2 * std::log2(V(49))), 10)}});
            notes.push_back("compressor threshold, attack and make-up are guesses (only the ratio is stored)");
        }
        if (V(57) >= 0.5 && V(50) > 0.3) {
            const json sh = toneShelf(V(51), 200, 20000);
            if (!sh.is_null()) out.fx.push_back({{"type", "eq"}, {"bands", json::array({sh})}});
            out.fx.push_back({{"type", "saturate"}, {"drive", r(std::min(24.0, 1.2 * V(50)), 10)}, {"match", true}});
            notes.push_back("Dist Gain " + num(V(50), 1) + " as " + num(std::min(24.0, 1.2 * V(50)), 1) + " dB of tanh drive, Tone " + inum(V(51)) + " Hz as a shelf (guesses)");
        }
        if (V(59) >= 0.5 && V(53) > 0.5) {
            const int mm = ri(V(52));   // 1 phaser, 2 flanger, 3 chorus
            const double it = V(53) / 100.0;
            const json rate = rateOf(V(54), V(60) >= 0.5, notes, "modulation effect"), hz = rate.is_string() ? json(0.5) : rate;
            if (mm == 1) out.fx.push_back({{"type", "phaser"}, {"rate", rate}, {"feedback", r(0.2 + 0.6 * it, 100)}, {"mix", 0.5}});
            else if (mm == 2) {
                out.fx.push_back({{"type", "chorus"}, {"rate", hz}, {"delay", 2}, {"depth", r(0.5 + 2 * it, 100)}, {"mix", 0.5}});
                notes.push_back("the flanger plays as a short chorus (no feedback)");
            } else out.fx.push_back({{"type", "chorus"}, {"rate", hz}, {"depth", r(1 + 5 * it, 100)}, {"mix", r(0.3 + 0.2 * it, 100)}});
        }
        if (ri(V(43)) != 720) notes.push_back("FX Order " + inum(V(43)) + " differs from the factory order: the effects play wah, compressor, distortion, modulation");
    }
    return out;
}

GarageBandSynth sculpturePatch(const std::vector<float> &params, std::string &why) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "Sculpture";
    std::vector<std::string> notes;
    why.clear();
    // ---- the morph position: the pad, or when the morph envelope runs its time-weighted average path
    const int run = ri(V(334));   // 0 off, 1 pad, 2 envelope, 3 envelope + pad, 4/5 point set or solo?
    double x = V(322), y = V(323);
    const bool envRunning = run >= 2 && run <= 5;
    double travel = 0;
    if (envRunning) {
        const double depth = V(380) / 100.0;
        std::array<double, 9> px{}, py{}, times{};
        for (size_t k = 0; k < 9; ++k) {
            px[k] = V(351 + 3 * k), py[k] = V(352 + 3 * k);
            times[k] = k == 0 ? 1.0 : std::max(1.0, V(350 + 3 * k));
        }
        const double tot = fsum(times);
        std::array<double, 9> tx{}, ty{};
        for (size_t k = 0; k < 9; ++k) tx[k] = times[k] * px[k], ty[k] = times[k] * py[k];
        const double ex = fsum(tx) / tot * depth + (run != 2 ? x : 0), ey = fsum(ty) / tot * depth + (run != 2 ? y : 0);
        // how far the material, exciter and cutoff travel along the path
        const Morph m0 = morphValues(V, morphWeights(ex, ey));
        for (size_t k = 0; k < 9; ++k) {
            const Morph mv = morphValues(V, morphWeights(px[k] * depth, py[k] * depth));
            for (int i : {kStiff, kInner, kMedia, kO1Str, kCutoff}) travel = std::max(travel, std::fabs(mv[(size_t)i] - m0[(size_t)i]));
        }
        x = ex, y = ey;
        notes.push_back("the morph envelope (run mode " + std::to_string(run) + ", depth " + inum(V(380)) + " %) isn't played: its time-weighted average position (" +
                        num(x, 2) + ", " + num(y, 2) + ") used");
    }
    const std::vector<double> w = run != 0 ? morphWeights(x, y) : std::vector<double>{1.0};   // morph off: the centre point
    const Morph M = morphValues(V, w);
    if (run != 0 && std::max(std::fabs(x), std::fabs(y)) > 0.05) {
        std::string blend;
        for (size_t k = 0; k < w.size(); ++k)
            if (w[k] > 0.02) blend += (blend.empty() ? "" : ", ") + std::string(k == 0 ? "centre" : std::string(1, "ABCD"[k - 1])) + " " + num(w[k], 2);
        notes.push_back("the morph pad at (" + num(x, 2) + ", " + num(y, 2) + "): points blended " + blend + " (the corners confirmed, the blend between them a guess)");
    }
    if (envRunning && travel > 0.35) {
        why = "Sculpture's morph envelope moves its material, exciter or cutoff by up to " + num(travel, 2) +
              " during a note: the morph is its sound, which a static builtin:synth voice can't play, so only GarageBand and Logic play it";
        return out;
    }
    // ---- the objects: 1 and 2 excite (or disturb), 3 disturbs or damps
    struct Obj { int n; bool on; int type, gate; double str, timbre, var, velo; };
    std::array<Obj, 3> objs{};
    static const size_t ids[3][4] = {{57, 58, 59, 65}, {69, 70, 71, 77}, {81, 82, 83, 0}};
    for (size_t k = 0; k < 3; ++k)
        objs[k] = {(int)k + 1, V(ids[k][0]) >= 0.5, ri(V(ids[k][1])), ri(V(ids[k][2])), M[kO1Str + 4 * k], M[kO1Timbre + 4 * k], M[kO1Var + 4 * k],
                   ids[k][3] ? V(ids[k][3]) : 0.0};
    for (auto &o : objs)
        if (o.on && o.type == 15) {
            why = "its object 2 is External: Sculpture's string is played by side-chain audio, which only GarageBand and Logic can feed it";
            return out;
        }
    bool strengthMod = false;   // object 1's strength under modulation (an envelope fading it in)
    for (auto &rt : kRoutes) strengthMod |= V(rt.on) >= 0.5 && ri(V(rt.tgt)) == 5;
    const Obj *exc = nullptr;
    for (auto &o : objs)
        if (!exc && o.on && o.type >= 1 && o.type <= 8 && (o.str > 0.02 || (o.n == 1 && strengthMod))) exc = &o;
    if (!exc) {
        why = "no object excites Sculpture's string (it sounds only from a side chain), so only GarageBand and Logic play it";
        return out;
    }
    const std::string cls = objectClass(exc->type);
    out.engine = cls;
    const bool continuous = (cls == "bowed" || cls == "blown" || cls == "noise") && exc->gate != 2;
    const double S = M[kStiff], IL = M[kInner], ML = M[kMedia], TM = M[kTension];
    double T = stringDecay(ML, IL), Trel = stringDecay(V(44), V(43));
    for (size_t k = 1; k < 3; ++k)
        if (objs[k].on && objs[k].type == 14 && objs[k].str > 0.02) T *= 1 - 0.6 * std::clamp(objs[k].str, 0.0, 1.0);
    for (size_t k = 1; k < 3; ++k)
        if (objs[k].on && objs[k].type == 14 && objs[k].str > 0.02) Trel *= 1 - 0.6 * std::clamp(objs[k].str, 0.0, 1.0);
    // ---- the voice: oscillators by exciter class and material
    const double timbre = std::clamp(exc->timbre, -1.0, 1.0);
    const double pw = std::clamp((std::min(M[kPickA], 1 - M[kPickA]) + std::min(M[kPickB], 1 - M[kPickB])) / 2, 0.05, 0.5);
    json oscs = json::array(), filt, fenv;
    const std::string material = S > 0.55 && IL < 0.45 ? "bell/glass" : S > 0.55 ? "wood" : IL > 0.55 ? "nylon/gut" : "metal string";
    if (cls == "plucked" || cls == "struck") {
        if (S > 0.55) {   // stiff: inharmonic partials (bells, bars, glass)
            const double ratio = IL < 0.45 ? r(1.4 + 2.6 * S, 100) : 4.0;
            oscs.push_back({{"wave", "sine"}, {"fm", {{"ratio", ratio}, {"index", r(std::clamp(2.6 * (1 - IL) + 0.6, 0.4, 4.0), 100)},
                                                      {"decay", r(std::clamp(0.35 * T, 0.05, 4.0))}, {"sustain", 0.08}}}});
            oscs.push_back({{"wave", "sine"}, {"semi", IL < 0.45 ? 17.6 : 24}, {"level", r(0.25 * (1 - 0.6 * IL), 100)}, {"decay", r(std::clamp(0.5 * T, 0.05, 6.0))}});
            filt = {{"type", "off"}};
        } else {
            if (cls == "struck") {
                oscs.push_back({{"wave", "triangle"}});
                oscs.push_back({{"wave", "square"}, {"pw", r(pw)}, {"level", 0.35}, {"decay", r(std::clamp(0.2 * T, 0.03, 2.0))}});
            } else oscs.push_back(pw < 0.12 ? json{{"wave", "saw"}} : json{{"wave", "square"}, {"pw", r(pw)}});
            // Inner Loss = high harmonics dying first: a filter envelope closing over the note
            const double base = 900 * std::pow(2.0, 2.2 * (1 - IL)) * std::pow(2.0, 1.0 * timbre);
            filt = {{"type", "lowpass"}, {"slope", 12}, {"cutoff", r(std::clamp(base, 120.0, 12000.0), 10)}, {"keytrack", 0.7},
                    {"env", r(std::clamp(3.0 + 1.0 * (exc->type == 1) - 1.5 * IL, 0.5, 4.5), 100)}, {"velocity", r(1.0 + exc->velo, 100)}};
            fenv = adsr(0, std::clamp(T * (0.08 + 0.35 * (1 - IL)), 0.03, 6.0), 0, 0.2);
        }
    } else if (cls == "bowed") {
        oscs.push_back({{"wave", "saw"}});
        if (exc->type == 6) oscs.push_back({{"wave", "saw"}, {"cents", 6}, {"level", 0.6}});
        filt = {{"type", "lowpass"}, {"slope", 12}, {"cutoff", r(std::clamp(3500 * std::pow(2.0, 1.0 * timbre) * std::pow(2.0, -1.5 * IL), 300.0, 12000.0), 10)},
                {"keytrack", 0.6}, {"velocity", 0.5}};
    } else if (cls == "blown") {
        oscs.push_back({{"wave", "triangle"}});
        oscs.push_back({{"wave", "sine"}, {"octave", 1}, {"level", 0.3}});
        oscs.push_back({{"wave", "noise"}, {"level", r(std::clamp(0.08 + 0.12 * exc->var, 0.03, 0.25), 100)}});
        filt = {{"type", "lowpass"}, {"slope", 12}, {"cutoff", r(std::clamp(2200 * std::pow(2.0, 1.2 * timbre), 300.0, 10000.0), 10)}, {"keytrack", 0.8}, {"velocity", 0.6}};
    } else {   // noise into a string: pitched breath, a resonant band-pass that follows the note and a quiet sine for pitch
        oscs.push_back({{"wave", "noise"}});
        oscs.push_back({{"wave", "sine"}, {"level", 0.35}, {"filter", false}});
        filt = {{"type", "bandpass"}, {"slope", 12}, {"cutoff", r(261.6 * (1 + 2 * std::clamp(timbre + 1, 0.0, 2.0) / 2), 10)}, {"keytrack", 1}, {"resonance", 0.8}};
        notes.push_back("the noise-excited string as note-tracking resonant noise and a sine: a rough stand-in for a breathy resonance");
    }
    // Resolution: few harmonics -> a low-pass at Resolution x the fundamental (at C3)
    const double res = V(35);
    if (res < 60 && filt["type"] == "lowpass") {
        filt["cutoff"] = r(std::min(filt["cutoff"].get<double>(), res * 130.8), 10);
        filt["keytrack"] = 1;
        notes.push_back("Resolution " + inum(res) + " (harmonics at C3) caps the low-pass at " + inum(res) + " x the fundamental");
    }
    // ---- Sculpture's own filter: type 1 low-pass, 2 high-pass, 3 peak, 4 band-pass, 5 notch (order a guess), cutoff A1
    json fx = json::array();
    if (V(106) >= 0.5) {
        const int ft = ri(V(107));
        const double fc = cutoffHz(M[kCutoff]), fres = M[kReso], kt = std::clamp(V(110), 0.0, 1.0), vs = std::clamp(V(111), 0.0, 1.0);
        const char *mode = ft == 1 ? "lowpass" : ft == 2 ? "highpass" : ft == 4 ? "bandpass" : nullptr;
        if (mode && cls == "noise") {   // the synth's filter is the noise string's pitch: Sculpture's goes after the voices
            fx.push_back({{"type", "filter"}, {"mode", mode}, {"cutoff", r(fc, 10)}, {"resonance", r(0.707 + 4 * fres, 100)}});
            notes.push_back(std::string("Sculpture's ") + mode + " filter as an effect after the voices (its key and velocity tracking lost)");
        } else if (mode) {
            json mine = {{"type", mode}, {"slope", 12}, {"cutoff", r(fc, 10)}, {"resonance", r(0.85 * fres, 100)}, {"keytrack", r(kt, 100)}};
            if (vs > 0.01) mine["velocity"] = r(2.5 * vs, 100);
            if (ft == 1 && filt["type"] == "lowpass") {
                // the string's brightness (base, falling from base x 2^env) seen through Sculpture's fixed low-pass
                const double base = filt["cutoff"].get<double>(), env = filt.value("env", 0.0);
                if (fc >= base) {
                    mine["cutoff"] = r(base, 10);
                    mine["keytrack"] = filt.contains("keytrack") ? filt["keytrack"] : json(kt);
                    const double top = std::min(base * std::pow(2.0, env), fc);
                    if (env > 0 && top > base * 1.05) mine["env"] = r(std::log2(top / base), 100);
                    if (vs <= 0.01 && filt.contains("velocity")) mine["velocity"] = filt["velocity"];
                } else if (env > 0) {
                    notes.push_back("Sculpture's low-pass (" + inum(fc) + " Hz) sits below the string's brightness: its decay is inaudible");
                    fenv = nullptr;
                }
            } else if (filt["type"] != "off" && filt.contains("env")) {
                notes.push_back(std::string("the string's brightness envelope gives way to Sculpture's ") + mode + " filter");
                fenv = nullptr;
            }
            filt = mine;
        } else if (ft == 3) {
            if (fres > 0.04)
                fx.push_back({{"type", "eq"}, {"bands", json::array({{{"type", "peak"}, {"freq", r(fc, 10)}, {"q", r(0.7 + 6 * fres, 100)}, {"gain", r(12 * fres, 10)}}})}});
            notes.push_back("the Peak filter (type 3, a guess) as an EQ peak, its gain and width from Resonance (guesses); its key and velocity tracking lost");
        } else if (ft == 5 && M[kCutoff] < 0.95) {
            fx.push_back({{"type", "eq"}, {"bands", json::array({{{"type", "peak"}, {"freq", r(fc, 10)}, {"q", r(0.7 + 6 * fres, 100)}, {"gain", -18}}})}});
            notes.push_back("the Notch filter (type 5, a guess) as an EQ cut");
        }
        notes.push_back("filter type " + std::to_string(ft) + " cutoff " + num(M[kCutoff], 2) + " -> " + num(fc, 0) + " Hz (guess A1)");
    }
    // ---- amplitude: Sculpture's envelope on top of the string's own decay
    const double att = lerp(V(115), V(116), kVel) / 1000.0, adec = V(117) / 1000.0, asus = std::clamp(V(118), 0.0, 1.0), arel = V(119) / 1000.0;
    const double velAmt = std::clamp(0.35 + 0.55 * std::clamp(exc->velo, 0.0, 1.0), 0.0, 1.0);
    json amp;
    if (continuous) {
        const double rel = std::clamp(std::min(std::max(arel, 0.02), Trel < 10 ? Trel * 1.5 : arel), 0.02, 12.0);
        amp = adsr(std::max(att, 0.005), std::max(adec, 0.01), asus, rel, velAmt);
        if (exc->gate == 0 && ML > 0.6) notes.push_back("high Media Loss with a key-on exciter: the tone stops quickly at key-up");
    } else {
        const double dec = asus >= 0.3 ? T : std::min(T, adec + asus * T), rel = std::clamp(std::min(std::max(arel, 0.01), Trel), 0.01, 12.0);
        amp = adsr(std::max(att, 0.0005), dec, 0, rel, velAmt);
    }
    json filterEnv = fenv;
    // ---- modulation
    json lfos = json::array();
    const int vibCtrl = ri(V(15));
    const double vdepth = vibCtrl == -1 ? V(30) : V(29);
    if (vdepth > 0.005 && V(28) > 0) {
        lfos.push_back({{"rate", r(V(28), 100)}, {"depth", r(vdepth)}, {"to", "pitch"}});
        notes.push_back("vibrato depth " + num(vdepth, 2) + " read as semitones (a guess)");
    }
    if (V(30) > 0.005 && vibCtrl != -1)
        notes.push_back("vibrato up to " + num(V(30), 2) + " on its controller (#" + std::to_string(vibCtrl) + ") isn't applied (the controller at rest)");
    json chorusRate;   // pickup movement, played as a chorus
    double movement = 0;   // unplayed LFO, jitter and envelope amounts plus chaotic objects: what a static voice can't have
    for (auto &rt : kRoutes) {
        if (V(rt.on) < 0.5) continue;
        const int tgt = ri(V(rt.tgt));
        const double amt = V(rt.amt);
        const std::string src = rt.src, tname = targetName(tgt);
        if (src.rfind("LFO", 0) == 0) {
            const size_t b = src == "LFO1" ? 132 : 150;   // the LFO's waveform; its rate at +1, envelope at +5, sync at +7
            const json rate = V(b + 1) > 0 ? rateOf(V(b + 1), V(b + 7) >= 0.5, notes, src) : json(0.5);
            if (std::fabs(amt) < 0.01) { notes.push_back(src + " -> " + tname + " at 0 (only via a controller) isn't played"); continue; }
            bool added = true;
            if (tgt == 35) lfos.push_back({{"rate", rate}, {"depth", r(amt * kEnvCutOct * 0.5, 100)}, {"to", "cutoff"}});
            else if (tgt == 5 && continuous) lfos.push_back({{"rate", rate}, {"depth", r(std::min(1.0, std::fabs(amt) * 0.6), 100)}, {"to", "amp"}});
            else {
                added = false;
                if (tgt >= 25 && tgt <= 28) { if (chorusRate.is_null()) chorusRate = rate.is_string() ? json(0.4) : rate; }
                else { notes.push_back(src + " -> " + tname + " (" + num(amt, 2, true) + ") isn't played"); movement += std::fabs(amt); }
            }
            const double eg = V(b + 5);
            if (eg != 0 && (tgt == 35 || tgt == 5)) {
                if (eg > 0 && added) lfos.back()["fade"] = r(eg / 1000.0);
                notes.push_back(eg > 0 ? src + " envelope " + gnum(eg) + " ms read as a fade-in" : src + " fade-out isn't played");
            }
        } else if (src.rfind("Env", 0) == 0) {
            const CtrlEnv env = ctrlEnv(V, src == "Env1" ? 220 : 270);
            if (!env.run) continue;
            if (tgt == 35 && std::fabs(amt) > 0.01) {
                double peak = 0;
                const json shape = envAdsr(env, peak);
                if (!shape.is_null() && filt["type"] != "off") {
                    filterEnv = shape;
                    filt["env"] = r(std::clamp(peak * amt * kEnvCutOct, -8.0, 8.0), 100);
                    notes.push_back(src + " -> cutoff (" + num(amt, 2, true) + ") as the filter envelope, " + num(filt["env"].get<double>(), 1, true) + " octaves (scale a guess)");
                }
            } else if (tgt == 5 && continuous && std::fabs(amt) > 0.01) {
                double peak = 0;
                const json shape = envAdsr(env, peak);
                if (!shape.is_null() && shape["attack"].get<double>() > amp["attack"].get<double>()) {
                    amp["attack"] = shape["attack"];
                    notes.push_back(src + " fades object 1 in: its " + num(shape["attack"].get<double>(), 2) + " s rise as the amp attack");
                }
            } else if (std::fabs(amt) > 0.01) {
                notes.push_back(src + " -> " + tname + " (" + num(amt, 2, true) + ") isn't played");
                movement += 0.5 * std::fabs(amt);   // a one-shot shape: half the weight of a cycling or random route
            }
            if (env.recorded) notes.push_back(src + " holds a recorded curve: only its points were read");
        } else if (src.rfind("Velo", 0) == 0) {
            if (tgt == 35 && amt > 0 && filt["type"] != "off") filt["velocity"] = r(std::clamp(filt.value("velocity", 0.0) + 2.5 * amt, 0.0, 4.0), 100);
            else if (std::fabs(amt) > 0.01) notes.push_back("velocity -> " + tname + " (" + num(amt, 2, true) + ") isn't played");
        } else if (std::fabs(amt) > 0.01) {
            notes.push_back(src + " -> " + tname + " (" + num(amt, 2, true) + ")" + (src.rfind("Ctrl", 0) == 0 ? " (the controller at rest: offset only)" : "") + " isn't played");
            if (src.rfind("Jitter", 0) == 0) movement += std::fabs(amt);
        }
    }
    json synth = json::object();
    if (lfos.size() > 4) lfos.erase(lfos.begin() + 4, lfos.end());
    if (!lfos.empty()) synth["lfo"] = lfos;
    if (TM > 0.05) {
        synth["pitchEnv"] = {{"amount", r(0.4 * TM, 100)}, {"decay", 0.06}};
        notes.push_back("Tension Mod " + num(TM, 2) + " as a short upward pitch drop-in (a guess)");
    }
    // the keyboard: mode 2 poly, 5 and 6 mono and legato (which is which a guess), Transpose in octaves, Tune, Warmth
    const int km = ri(V(2));
    if (km == 5 || km == 6 || ri(V(1)) == 1) {
        synth["mono"] = true;
        synth["legato"] = km == 6;
        if (V(3) > 0.5) synth["glide"] = r(V(3) / 1000.0);
    }
    const int tr = ri(V(10) / 12.0);
    if (tr)
        for (auto &o : oscs)
            if (o["wave"] != "noise") o["octave"] = o.value("octave", 0) + tr;
    if (std::fabs(V(4)) >= 0.5) for (auto &o : oscs) o["cents"] = r(V(4), 10);
    if (V(5) > 0.15) {
        synth["unison"] = {{"voices", 2}, {"detune", r(4 + 20 * V(5), 10)}, {"spread", 0.5}};
        notes.push_back("Warmth " + num(V(5), 2) + " as a two-voice unison");
    }
    // a level per class (see the header), moved by 10 % of Sculpture's own Level
    const double base = cls == "blown" ? -3.6 : cls == "bowed" ? -3.5 : cls == "noise" ? 3.3 : cls == "plucked" ? -8.5 : -4.0;
    synth["level"] = r(std::clamp(base + 0.1 * V(8), -40.0, 12.0), 100);
    // ---- after the voices: the waveshaper, Body EQ, pickup movement as a chorus, the delay
    if (V(100) >= 0.5) {
        fx.insert(fx.begin(), json{{"type", "saturate"}, {"drive", r(std::clamp(9 + 9 * M[kWsIn], 1.0, 24.0), 10)}, {"match", true}});
        notes.push_back("the waveshaper (type " + inum(V(101)) + ", input " + num(M[kWsIn], 2) + ", variation " + num(M[kWsVar], 2) +
                        ") as tanh saturation, after the voices rather than in each");
    }
    if (V(393) >= 0.5) {
        if (ri(V(394)) == 0) {
            json bands = json::array();
            const std::array<std::tuple<const char *, double, double>, 3> eq = {
                std::make_tuple("lowshelf", 200.0, V(395)), std::make_tuple("peak", 100 * std::pow(100.0, std::clamp(V(398), 0.0, 1.0)), V(396)),
                std::make_tuple("highshelf", 4000.0, V(397))};
            for (auto &[type, freq, g] : eq)
                if (std::fabs(g) > 0.03) {
                    json b = {{"type", type}, {"freq", r(freq, 10)}, {"gain", r(12 * g, 10)}};
                    if (std::string(type) == "peak") b["q"] = 0.8;
                    bands.push_back(b);
                }
            if (!bands.empty()) {
                fx.push_back({{"type", "eq"}, {"bands", bands}});
                notes.push_back("Body EQ (Basic EQ): Low, Mid and High +-1 read as +-12 dB (a guess), Mid Freq 100 Hz..10 kHz");
            }
        } else notes.push_back("Body EQ model " + inum(V(394)) + " (an instrument body response, intensity " + num(V(395), 2) + ") isn't played");
    }
    if (!chorusRate.is_null()) {
        fx.push_back({{"type", "chorus"}, {"rate", r(chorusRate.get<double>(), 100)}, {"depth", 3}, {"mix", 0.35}});
        notes.push_back("LFO movement of the pickups as a chorus (Apple's guide: pickup modulation gives chorus and width effects)");
    }
    if (V(401) >= 0.5 && V(402) > 0.5) {
        json d = {{"type", "delay"}, {"feedback", r(std::clamp(std::fabs(V(408)) / 100.0, 0.0, 0.95), 100)}, {"mix", r(V(402) / (100.0 + V(402)), 100)},
                  {"highpass", r(std::clamp(V(411), 20.0, 2000.0), 10)}, {"lowpass", r(std::clamp(V(410), 500.0, 20000.0), 10)},
                  {"pingpong", std::fabs(V(409)) > 25 || std::fabs(V(405)) > 0.5}};
        if (V(403) >= 0.5) d["time"] = r(V(404) * 4, 10000);   // synced: whole notes as beats
        else d["ms"] = r(V(404), 10);
        fx.push_back(d);
        if (V(408) < 0) notes.push_back("the delay's feedback is phase-inverted in Sculpture");
        if (std::fabs(V(406)) > 0.05 || std::fabs(V(407)) > 1) notes.push_back("the delay's Groove and Spread (left/right time offsets) aren't played");
    }
    // ---- the other objects, and how close it gets
    notes.insert(notes.begin(), "class " + cls + ": object " + std::to_string(exc->n) + " " + objectName(exc->type) + " (gate " + std::to_string(exc->gate) + "), " +
                                    material + " (stiffness " + num(S, 2) + ", inner loss " + num(IL, 2) + ", media loss " + num(ML, 2) + ") -> string decay " +
                                    num(T, 2) + " s (a guess)");
    bool others = false;
    for (auto &o : objs) {
        if (!o.on || &o == exc || o.str <= 0.02) continue;
        others = true;
        const std::string oc = objectClass(o.type), label = "object " + std::to_string(o.n) + " " + objectName(o.type) + " (strength " + num(o.str, 2) + ")";
        if (o.type == 14) notes.push_back(label + " shortens the decay");
        else if ((oc == "bowed" || oc == "blown" || oc == "noise") && (o.type == 8 || !continuous) && o.str > 0.1) {
            if (o.type == 8 && cls != "noise") oscs.push_back({{"wave", "noise"}, {"level", r(0.12 * std::min(1.0, o.str), 100)}});
            if (!continuous && o.str > 0.3) amp["sustain"] = r(0.35 * std::min(1.0, o.str));
            notes.push_back(label + (continuous ? " adds breath noise" : " keeps driving the string: amp sustain " + num(amp["sustain"].get<double>(), 2)));
        } else {
            notes.push_back(label + " isn't played");
            if (o.type == 11 || o.type == 12) movement += o.str;
        }
    }
    int jitter = 0;
    for (auto &rt : kRoutes) jitter += std::string(rt.src).rfind("Jitter", 0) == 0 && V(rt.on) >= 0.5 && std::fabs(V(rt.amt)) > 0.01;
    if (jitter) notes.push_back("jitter (random drift) on " + std::to_string(jitter) + " routes isn't played");
    const bool approximate = (cls == "plucked" || cls == "struck") && !others && !jitter && !envRunning;
    notes.insert(notes.begin(), closeness(approximate));
    if (continuous && movement >= 1.5) {
        why = "its sound is a sustained Sculpture texture made of movement builtin:synth can't play (jitter and LFO routes to string and object "
              "parameters, bouncing objects: " + num(movement, 2) + " of it, refused from 1.5), so only GarageBand and Logic play it";
        return out;
    }
    synth["osc"] = oscs;
    synth["filter"] = filt;
    synth["amp"] = amp;
    if (!filterEnv.is_null()) synth["filterEnv"] = filterEnv;
    out.synth = synth;
    out.fx = fx;
    out.notes = notes;
    return out;
}

} // namespace wl
