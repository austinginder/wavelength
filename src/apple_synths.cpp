#include "apple_synths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

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

// the guesses (see the header): A1 a normalized cutoff -> 20 Hz .. 20 kHz exponentially, A2 an intensity of 1 =
// 10 octaves of cutoff, A3 EFM1's FM Intensity 1 = index 8; velocity-split values resolve at a typical velocity
double cutoffHz(double x) { return 20.0 * std::pow(1000.0, std::clamp(x, 0.0, 1.0)); }
const double kCutOct = 10, kVel = 0.8, kFmIndex = 8;
double lerp(double a, double b, double t) { return a + (b - a) * t; }

// an ES2 router intensity on a pitch target in cents (Apple's guide: 8 = 10 c, 20 = 50 c, 28 = a semitone, 36 = 2,
// 76 = an octave, 100 = 3 octaves, linear between)
double es2Cents(double intensity) {
    static const double pts[7][2] = {{0, 0}, {8, 10}, {20, 50}, {28, 100}, {36, 200}, {76, 1200}, {100, 3600}};
    const double v = std::fabs(intensity) * 100;
    double c = 3600;
    for (int i = 1; i < 7; ++i)
        if (v <= pts[i][0]) { c = pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (v - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]); break; }
    return std::copysign(c, intensity);
}
// ES2's mix triangle -> each oscillator's level ([1..3]), barycentric; the corners (from the strips): oscillator 1
// at (1, 1.1547), 2 at (0, 0.57735), 3 at (1, 0)
std::array<double, 4> es2Weights(double x, double y) {
    const double ax = 0, ay = 0.57735, bx = 1, by = 0, cx = 1, cy = 1.1547, det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    const double w2 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / det, w3 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / det;
    return {0, std::max(0.0, 1 - w2 - w3), std::max(0.0, w2), std::max(0.0, w3)};
}
std::string es2Source(int s) {
    static const char *names[23] = {"LFO1", "LFO2", "ENV1", "ENV2", "ENV3", "PadX", "PadY", "Max", "Kybd", "Velo", "Bender", "ModWhl",
                                    "Touch", "Whl+To", "CtrlA", "CtrlB", "CtrlC", "CtrlD", "CtrlE", "CtrlF", "RndN01", "RndN02", "SideCh"};
    return s >= 0 && s < 23 ? names[s] : std::to_string(s);
}
// router targets 0-21 and 29-38 fitted from the strips' use; 22-24 read as pan, amp and (perhaps) LFO1 asymmetry
std::string es2Target(int t) {
    static const char *names[39] = {"Pitch123", "Pitch1", "Pitch2", "Pitch3", "Detune", "OscWaves", "Osc1Wave", "Osc2Wave", "Osc3Wave",
                                    "SineLev1", "OscLScle", "Osc1Levl", "Osc2Levl", "Osc3Levl", "Cutoff1", "Reso1", "Cutoff2", "Reso2",
                                    "LPF FM", "Cut1+2", "Cut1inv2", "FltBlend", "Pan?", "Amp?", "LFO1Asym?", nullptr, nullptr, nullptr, nullptr,
                                    "LFO1Rate", "Env2Atck", "Env2Dec", "Env2Rel", "Env2Time", "Env3Atck", "Env3Dec", "Env3Rel", "Env3Time", "Glide"};
    return t >= 0 && t < 39 && names[t] ? names[t] : "#" + std::to_string(t);
}
// an ES2 oscillator's wave position in words (Digiwaves by number only)
std::string es2WaveName(int o, double wave, double digi) {
    static const char *first[6] = {"", "triangle", "saw", "rectangle", "pulse", "sine"}, *other[7] = {"", "ring", "sync saw", "sync rect", "triangle", "saw", "square"};
    const int v = ri(wave);
    if (v == 0) return "digiwave #" + std::to_string((int)digi);
    if (o == 1 && v >= 1 && v <= 5) return first[v];
    if (o != 1 && v >= 1 && v <= 6) return o == 3 && v == 1 ? "noise" : other[v];
    if (o == 1) return "sine + FM " + std::to_string(ri((v - 5) / 99.0 * 100)) + "%";
    return "pulse (width position " + std::to_string(v) + "/104)";
}
} // namespace

GarageBandSynth vintageB3Patch(const std::vector<float> &params) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "Vintage B3";
    auto &notes = out.notes;
    // the upper manual's drawbars (#11-19: 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1') as sines, 3 dB a step below 8
    static const int feet[9] = {-12, 7, 0, 12, 19, 24, 28, 31, 36};
    json oscs = json::array();
    for (int i = 0; i < 9; ++i) {
        const int n = ri(V(11 + (size_t)i));
        if (n <= 0) continue;
        json o = {{"wave", "sine"}, {"level", r(std::pow(10.0, -3.0 * (8 - n) / 20))}};
        if (feet[i]) o["semi"] = feet[i];
        oscs.push_back(o);
    }
    // percussion: the 2nd or 3rd harmonic fading over its time, level 1 ~ 0.7 of a full drawbar (a guess)
    if (V(46) >= 0.5) {
        oscs.push_back({{"wave", "sine"}, {"semi", V(49) >= 0.5 ? 19 : 12}, {"level", r(std::clamp(V(51), 0.05, 1.5) * 0.7)}, {"decay", r(std::max(0.05, V(50) / 1000))}});
        if (V(138) < 0.5) notes.push_back("the B3's percussion is single-trigger (only once all keys are up); here every note retriggers it");
    }
    if (V(94) > 0.05) {   // key click: a short noise burst
        oscs.push_back({{"wave", "noise"}, {"level", r(std::min(0.4, 0.04 * V(94)))}, {"decay", r(std::max(0.002, V(97) / 1000), 10000)}});
        notes.push_back("key click as a short noise burst (level and length scales a guess, colour ignored)");
    }
    json synth = {{"osc", oscs}, {"filter", {{"type", "off"}}},
                  {"amp", {{"attack", 0.004}, {"decay", 0.1}, {"sustain", 1}, {"release", r(std::max(0.01, V(41) / 1000 * 2))}, {"velocity", 0}}}};
    if (V(35) >= 0.5) {   // upper vibrato: the scanner as a pitch LFO, V1/V2/V3 (C1/C2/C3) 8/15/24 cents deep (a guess)
        static const double depth[7] = {0, 0.08, 0.15, 0.24, 0.08, 0.15, 0.24};
        const int vt = ri(V(37));
        if (vt >= 1 && vt <= 6) {
            synth["lfo"] = {{"rate", r(V(39))}, {"depth", depth[vt]}, {"to", "pitch"}};
            notes.push_back("scanner vibrato as a pitch LFO (V1/V2/V3 8/15/24 cents deep, a guess)");
        }
    }
    // like KY Organ at -12, moved by the organ's own Volume (from the factory patches' typical -6 dB) and Expression:
    // a patch that turns the organ down for its distortion or EQ boosts keeps that gain structure
    const double expression = V(104) > 0 && V(104) <= 1 ? 20 * std::log10(std::max(0.01, V(104))) : 0.0;
    synth["level"] = r(std::clamp(-12 + (V(105) + 6) + expression, -40.0, 12.0), 100);
    out.synth = synth;
    // effects: EQ, distortion (and the wah) under Master FX, the rotor cabinet, the reverb
    if (V(152) >= 0.5) {
        if (V(153) >= 0.5 && (std::fabs(V(54)) > 0.05 || std::fabs(V(55)) > 0.05 || std::fabs(V(56)) > 0.05)) {
            json bands = json::array();
            auto band = [&](json b, double gain) { if (std::fabs(r(gain, 100)) > 0.05) { b["gain"] = r(gain, 100); bands.push_back(b); } };
            band({{"type", "lowshelf"}, {"freq", 120}}, V(54));
            band({{"type", "peak"}, {"freq", 1000}, {"q", 0.7}}, V(55));
            band({{"type", "highshelf"}, {"freq", 4000}}, V(56));
            out.fx.push_back({{"type", "eq"}, {"bands", bands}});
            notes.push_back("the EQ's band frequencies aren't stored: 120 Hz, 1 kHz and 4 kHz assumed");
        }
        if (V(154) >= 0.5 && V(66) > 0.01) {
            out.fx.push_back({{"type", "saturate"}, {"drive", r(V(66) * 24, 10)}, {"match", true}});
            notes.push_back("distortion: drive 1 read as 24 dB of tanh drive; its type and tone ignored");
        }
        if (V(156) >= 0.5) notes.push_back("the wah is on (mode " + std::to_string((int)V(59)) + ", pedal " + num(V(63), 2) + ") and isn't played");
    }
    if (V(128) >= 0.5) {   // rotor speed 0 slow, 1 brake (rotors stopped), 2 fast
        const int sp = ri(V(75));
        json rot = {{"type", "rotary"}, {"speed", sp == 2 ? 1 : 0}, {"hornFast", r(V(77), 100)}};
        if (sp == 1) { rot["bypass"] = true; notes.push_back("rotor brake (rotors stopped): rotary bypassed; a stopped Leslie's colour isn't modelled"); }
        out.fx.push_back(rot);
    }
    if (V(152) >= 0.5 && V(155) >= 0.5 && V(71) > 0) {
        out.fx.push_back({{"type", "reverb"}, {"decay", 1.6}, {"size", 0.6}, {"mix", r(std::min(0.5, V(71) / 200 * 0.6))}});
        notes.push_back("reverb type not decoded: a mid-size room, its mix scaled from Reverb Level");
    }
    const int kb = ri(V(118));
    if (kb != 0) {
        static const char *modes[3] = {"single", "split", "multi"};
        notes.push_back(std::string("Keyboard Mode ") + (kb > 0 && kb < 3 ? modes[kb] : "?") + ": the upper manual plays every key; whether "
                        "GarageBand also plays the lower manual below the split (" + std::to_string((int)V(122)) + ") is unconfirmed");
    }
    return out;
}

GarageBandSynth es2Patch(const std::vector<float> &params) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "ES2";
    auto &notes = out.notes;
    // ES2's own row order: 0 glide, 1 analog, 3 keyboard mode; per oscillator (7, 12, 17) coarse, fine, -, wave
    // position, Digiwave; 22/23 the triangle; 24-36 filters, drive, Sine Level, volume; 37-86 ten router slots
    // (source, target, via, intensity, intensity at via max); 87-111 LFOs and envelopes; 112-118 effects and the
    // planar pad; 119 unison; 128-144 the vector envelope
    json synth = json::object(), oscs = json::array();
    const auto wts = es2Weights(V(22), V(23));
    double pitch[4] = {};
    for (int o = 1; o <= 3; ++o) pitch[o] = V(2 + 5 * o) + V(3 + 5 * o) / 100;
    auto wave = [&](int o) { return ri(V(5 + 5 * o)); };
    const bool synced = (wts[2] >= 0.02 && (wave(2) == 2 || wave(2) == 3)) || (wts[3] >= 0.02 && (wave(3) == 2 || wave(3) == 3));
    for (int o = 1; o <= 3; ++o) {
        const double lvl = wts[o];
        const std::string on = "osc" + std::to_string(o);
        if (lvl < 0.02) {   // out of the mix; oscillator 1 stays (silent) when 2 or 3 sync to it
            if (o == 1 && synced) { json m = {{"wave", "sine"}, {"level", 0}}; if (std::fabs(pitch[1]) > 1e-6) m["semi"] = r(pitch[1]); oscs.push_back(m); }
            continue;
        }
        const int v = wave(o), digi = ri(V(6 + 5 * o));
        json x = json::object();
        if (v == 0) {
            x["wave"] = "sine";
            if (digi) notes.push_back(on + ": Digiwave #" + std::to_string(digi) + " plays as a sine (Apple's wavetables aren't copied)");
        } else if (o == 1 && v == 5) x["wave"] = "sine";
        else if (o == 1 && v > 5) {   // positions 6..104: FM from oscillator 2, 0..100%
            x["wave"] = "sine";
            x["fm"] = {{"ratio", r(std::pow(2.0, (pitch[2] - pitch[1]) / 12), 10000)}, {"index", r((v - 5) / 99.0 * 6)}};
            notes.push_back("osc1 FM " + std::to_string(ri((v - 5) / 99.0 * 100)) + "% from osc2 (" + es2WaveName(2, V(15), V(16)) +
                            "): a sine modulator, index scale a guess (100% = 6)");
        } else if (o == 1 && v >= 1 && v <= 4) {
            x["wave"] = v == 1 ? "triangle" : v == 2 ? "saw" : "square";
            if (v == 4) x["pw"] = 0.25;
        } else if (o == 2 && v == 1) { x["wave"] = "sine"; notes.push_back("osc2 ring modulation (osc1 x osc2 square) isn't played: a plain sine stands in"); }
        else if (o == 3 && v == 1) x["wave"] = "noise";
        else if (v == 2 || v == 3) {   // the sync waves: hard sync to oscillator 1, which leads the list
            x["wave"] = v == 2 ? "saw" : "square";
            x["sync"] = true;
            notes.push_back(on + " wave " + std::to_string(v) + " read as a " + (v == 2 ? "saw" : "square") + " hard-synced to osc1 (order unconfirmed)");
        } else if (v >= 4 && v <= 6) x["wave"] = v == 4 ? "triangle" : v == 5 ? "saw" : "square";
        else { x["wave"] = "square"; x["pw"] = r(std::max(0.03, 0.5 - (v - 6) / 98.0 * 0.47)); }   // positions 7..104 narrow the pulse
        if (std::fabs(pitch[o]) > 1e-6 && x["wave"] != "noise") x["semi"] = r(pitch[o]);
        x["level"] = r(lvl);
        oscs.push_back(x);
    }
    if (V(35) > 0.02) {   // Sine Level: a sine at oscillator 1's pitch, after the filters as in ES2
        json s = {{"wave", "sine"}, {"level", r(V(35))}, {"filter", false}};
        if (pitch[1] != 0) s["semi"] = r(pitch[1]);
        oscs.push_back(s);
    }
    synth["osc"] = oscs;
    // the filter the blend favours (-1 filter 1 .. +1 filter 2); a closed high-pass filter 1 counts as filter 2
    const double blend = V(25);
    const int f1 = ri(V(26));
    const bool f1Closed = f1 == 1 && V(27) < 0.02;
    json filt;
    std::vector<int> cutTargets;   // the router targets that move the kept filter's cutoff
    if (blend >= 0 || f1Closed) {
        filt = {{"type", "lowpass"}, {"slope", ri(V(30)) == 0 ? 12 : 24}, {"cutoff", r(cutoffHz(V(32)), 10)}, {"resonance", r(V(33))}};
        cutTargets = {16, 19};
        if (blend > -0.9 && blend < 0.9 && !f1Closed) notes.push_back("filter blend " + num(blend, 2, true) + ": both filters sound in ES2; only filter 2 kept");
    } else {
        static const char *types[5] = {"lowpass", "highpass", "bandpass", "lowpass", "bandpass"};   // Lo, Hi, Peak, BR, BP
        filt = {{"type", f1 >= 0 && f1 <= 4 ? types[f1] : "lowpass"}, {"slope", 12}, {"cutoff", r(cutoffHz(V(27)), 10)}, {"resonance", r(V(28))}};
        if (f1 == 2 || f1 == 3) notes.push_back("filter 1 Peak / Notch played as band-pass / low-pass");
        cutTargets = {14, 19, 20};
        if (blend > -0.9) notes.push_back("filter blend " + num(blend, 2, true) + ": filter 2 also sounds in ES2; only filter 1 kept");
    }
    if (V(29) > 0.01) filt["drive"] = r(std::min(1.0, V(29)));
    // the router: ENV2, velocity, keyboard, Max and pad routes to the kept cutoff, LFO routes, ENV1 -> pitch
    auto isCut = [&](int t) { return std::find(cutTargets.begin(), cutTargets.end(), t) != cutTargets.end(); };
    const double padX = V(117), padY = V(118);
    double envOct = 0, velOct = 0, staticOct = 0;
    json lfos = json::array();
    for (int s = 0; s < 10; ++s) {
        const size_t b = 37 + 5 * (size_t)s;
        const int src = ri(V(b)), tgt = ri(V(b + 1)), via = ri(V(b + 2));
        if (std::fabs(V(b + 3)) < 1e-4 && (via == 0 || std::fabs(V(b + 4)) < 1e-4)) continue;   // empty (stored as LFO1 -> Pitch123 at 0)
        const double in = r(V(b + 3)), inVia = r(V(b + 4));
        const std::string slot = "router slot " + std::to_string(s + 1) + ": ", route = es2Source(src) + " -> " + es2Target(tgt);
        double amt = in;
        if (via == 2) amt = lerp(in, inVia, kVel);   // via velocity: at a typical velocity; 3/4 (wheel, touch): at rest
        else if (via != 0 && via != 3 && via != 4) notes.push_back(slot + "via #" + std::to_string(via) + " unknown; its minimum intensity used");
        const bool cut = isCut(tgt);
        if (src == 3 && cut) {   // ENV2 -> cutoff: the filter envelope
            envOct += amt * kCutOct;
            if (via == 2 && inVia > in) velOct += (inVia - in) * kCutOct * kVel;
        } else if (src == 9 && cut) { if (amt > 0) velOct += amt * kCutOct; }
        else if (src == 8 && cut) filt["keytrack"] = r(std::clamp(2 * amt, 0.0, 1.0));   // 0.5 = proportional
        else if (src == 7 && cut) staticOct += amt * kCutOct;
        else if ((src == 5 || src == 6) && cut) staticOct += (src == 5 ? padX : padY) * amt * kCutOct;
        else if ((src == 0 || src == 1) && (via == 0 || via == 2)) {
            static const char *shapes[7] = {"triangle", "saw", "ramp", "square", "square", "random", "random"};
            const double rate = V(src == 0 ? 89 : 91);
            const int w = ri(V(src == 0 ? 90 : 92));
            json lfo = {{"rate", rate > 0 ? json(r(rate)) : json("1/8")}};
            if (rate <= 0) notes.push_back("LFO" + std::to_string(src + 1) + " rate " + gnum(rate) + " (0 = dc, < 0 = tempo sync) unmapped; \"1/8\" used");
            if (w >= 0 && w < 7) lfo["shape"] = shapes[w];
            bool pulse = false;
            for (auto &o : oscs) pulse |= o.contains("pw");
            if (tgt >= 0 && tgt <= 3) {
                lfo["depth"] = r(es2Cents(amt) / 100);
                lfo["to"] = "pitch";
                if (tgt) notes.push_back(slot + route + " (one oscillator) plays on all of them");
            } else if (cut) { lfo["depth"] = r(amt * kCutOct); lfo["to"] = "cutoff"; }
            else if (tgt >= 5 && tgt <= 8 && pulse) { lfo["depth"] = r(std::fabs(amt) * 0.4); lfo["to"] = "pw"; }
            else if (tgt == 22 || tgt == 23) { lfo["depth"] = r(std::min(1.0, std::fabs(amt))); lfo["to"] = tgt == 23 ? "amp" : "pan"; }
            else { notes.push_back(slot + route + " not mapped"); continue; }
            if (src == 0 && V(87) > 0) { lfo["delay"] = r(V(87) / 1000 / 2); lfo["fade"] = r(V(87) / 1000 / 2); }   // LFO1 EG: half delay, half fade-in
            lfos.push_back(lfo);
        } else if (src == 2 && tgt >= 0 && tgt <= 3 && (via == 0 || via == 2))   // ENV1 -> pitch: the pitch envelope
            synth["pitchEnv"] = {{"amount", r(es2Cents(amt) / 100, 100)}, {"decay", r(std::max(0.001, V(94) / 1000 / 4.6), 10000)}};
        else if (src == 2 && cut) notes.push_back(slot + "ENV1 -> cutoff (" + num(amt, 2, true) + ") not played (one filter envelope: ENV2's)");
        else if (via == 3 || via == 4) {}   // mod wheel or aftertouch at rest
        else if ((src == 5 || src == 6) && std::fabs((src == 5 ? padX : padY) * amt) < 0.02) {}   // pad centred
        else notes.push_back(slot + route + " (" + num(amt, 2, true) + ") not mapped");
    }
    for (int k = 0; k < 2; ++k) {   // the planar pad's vector targets (their list read as the router's: unconfirmed)
        const double pad = k == 0 ? padX : padY, amt = V(142 + 2 * (size_t)k);
        const int tgt = ri(V(141 + 2 * (size_t)k));
        if (isCut(tgt) && std::fabs(pad * amt) > 0.01) {
            staticOct += pad * amt * kCutOct;
            notes.push_back(std::string("planar pad ") + (k == 0 ? "X" : "Y") + " = " + num(pad, 2, true) + " -> vector target #" + std::to_string(tgt) +
                            " read as a cutoff offset (target list unconfirmed)");
        }
    }
    if (staticOct != 0) filt["cutoff"] = r(std::clamp(filt["cutoff"].get<double>() * std::pow(2.0, staticOct), 20.0, 20000.0), 10);
    if (std::fabs(envOct) > 0.05) {
        filt["env"] = r(std::clamp(envOct, -8.0, 8.0), 100);
        synth["filterEnv"] = {{"attack", r(lerp(V(98), V(103), kVel) / 1000, 10000)}, {"decay", r(std::max(0.001, V(99) / 1000))},
                              {"sustain", r(V(101))}, {"release", r(std::max(0.001, V(102) / 1000))}};
        if (std::fabs(V(100)) > 0.5) notes.push_back("ENV2 sustain time " + gnum(V(100)) + " ms (fall or rise) isn't played");
    }
    if (velOct > 0.05) filt["velocity"] = r(std::min(4.0, velOct), 100);
    synth["filter"] = filt;
    if (!lfos.empty()) {
        if (lfos.size() > 4) { notes.push_back("more than four LFO routes: the first four kept"); lfos.erase(lfos.begin() + 4, lfos.end()); }
        synth["lfo"] = lfos;
    }
    synth["amp"] = {{"attack", r(std::max(0.001, lerp(V(105), V(110), kVel) / 1000), 10000)}, {"decay", r(std::max(0.005, V(106) / 1000))},
                    {"sustain", r(V(108))}, {"release", r(std::max(0.005, V(109) / 1000))}, {"velocity", r(V(111))}};
    if (std::fabs(V(107)) > 0.5) notes.push_back("ENV3 sustain time " + gnum(V(107)) + " ms (fall or rise) isn't played");
    const int km = ri(V(3));   // 0 poly, 1 mono, 2 legato
    if (km == 1 || km == 2) { synth["mono"] = true; synth["legato"] = km == 2; }
    if (V(0) > 0.5) {
        synth["glide"] = r(V(0) / 1000, 10000);
        if (km == 0) notes.push_back("glide on a poly patch: builtin:synth glides only in mono");
    }
    if (V(119) >= 0.5 || V(1) > 0.25) synth["unison"] = {{"voices", 2}, {"detune", r(5 + 30 * V(1), 10)}, {"spread", 0.7}};   // Analog as detune
    synth["level"] = r(std::clamp(-6 + (V(36) + 6.5), -40.0, 12.0), 100);   // a calibrated level, moved by the plugin's Volume from its typical -6.5 dB
    out.synth = synth;
    // effects: distortion as saturation, the modulation effect as a chorus
    if (V(112) > 0.5) {
        out.fx.push_back({{"type", "saturate"}, {"drive", r(std::min(24.0, V(112) * 0.3), 10)}, {"match", true}});
        notes.push_back("distortion 0..100 read as 0..30 dB of tanh drive (a guess); its type and tone ignored");
    }
    if (V(116) > 0.5) {
        out.fx.push_back({{"type", "chorus"}, {"rate", r(V(115))}, {"depth", 4}, {"mix", r(std::min(0.5, V(116) / 100 * 0.6))}});
        if (ri(V(114)) != 0) notes.push_back("modulation effect type " + std::to_string(ri(V(114))) + " (flanger or phaser?) plays as a chorus");
    }
    if ((V(128) != 0 && V(128) != 1) || std::fabs(V(133)) > 0.01) notes.push_back("the vector envelope isn't played");
    notes.push_back("cutoff in Hz and router cutoff depths are guesses (A1, A2: intensity 1 = 10 octaves)");
    return out;
}

GarageBandSynth es1Patch(const std::vector<float> &params) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "ES1";
    auto &notes = out.notes;
    json synth = json::object();
    // oscillator: Wave 0/1 triangle/saw (order unconfirmed), 2 square, 3..102 pulses narrowing to 3% (a guess)
    const double wv = V(16);
    json osc = {{"wave", wv < 0.5 ? "triangle" : wv < 1.5 ? "saw" : "square"}};
    if (wv >= 2.5) { osc["pw"] = r(std::max(0.03, 0.5 - (wv - 2) / 100 * 0.47)); notes.push_back("pulse width: Wave 2..102 read as 50%..3% (a guess)"); }
    notes.push_back("Wave positions 0 and 1 read as triangle and saw (order unconfirmed)");
    // Osc Mix (#2) as the sub's share; the sub wave (#17): 0 square an octave down, 1/2 square and pulse two
    // octaves down, 3/4 the mixed variations, 5 noise, 6/7 off or external (guesses)
    const int octv = ri(V(18)) - 2;   // 32' .. 2'
    const double sub = std::clamp(V(2) / 100, 0.0, 1.0);
    osc["level"] = r(1 - sub * 0.5);
    if (octv) osc["octave"] = octv;
    if (V(0) != 0) osc["cents"] = r(V(0), 100);
    json oscs = json::array();
    oscs.push_back(osc);
    const int sw = ri(V(17));
    if (sub > 0.01) {
        if (sw == 0) synth["sub"] = r(sub * 0.7);
        else if (sw == 1 || sw == 2) {
            json s = {{"wave", "square"}, {"octave", octv - 2}, {"level", r(sub)}};
            if (sw == 2) s["pw"] = 0.25;
            oscs.push_back(s);
        } else if (sw == 5) { synth["noise"] = r(sub * 0.5); notes.push_back("sub wave 5 read as noise (a guess)"); }
        else if (sw == 3 || sw == 4) {
            oscs.push_back({{"wave", "square"}, {"octave", octv - 1}, {"level", r(sub)}});
            notes.push_back("sub wave " + std::to_string(sw) + " (a mixed variation) as a square an octave down");
        } else if (sw >= 6) notes.push_back("sub wave " + std::to_string(sw) + " read as off or external: no sub");
    }
    synth["osc"] = oscs;
    // filter: slope 0/1 12 dB, 2/3 24 dB; cutoff (A1), resonance, key follow, drive
    json filt = {{"type", "lowpass"}, {"slope", ri(V(19)) <= 1 && ri(V(19)) >= 0 ? 12 : 24}, {"cutoff", r(cutoffHz(V(10) / 100), 10)},
                 {"resonance", r(std::min(1.0, V(7) / 100))}, {"keytrack", r(std::min(1.0, V(20) / 100))}};
    if (V(3) > 0) filt["drive"] = r(std::min(1.0, V(3) / 100));
    // amp ADSR; VCA mode 0 attack-gate-release, 1 ADSR, 2 gate-release
    const double a = V(9) / 1000, d = V(11) / 1000, s = V(12) / 100, rel = V(8) / 1000;
    const int amode = ri(V(21));
    json amp = {{"attack", r(a, 10000)}, {"decay", r(d)}, {"sustain", r(s)}, {"release", r(rel)}};
    if (amode == 0) amp["sustain"] = 1.0;
    else if (amode == 2) { amp["attack"] = 0.002; amp["sustain"] = 1.0; }
    const double lv0 = V(22), lv1 = V(23);   // level via velocity, min .. max
    amp["velocity"] = lv1 > 0 ? r(std::clamp(1 - lv0 / lv1, 0.0, 1.0)) : 0.0;
    synth["amp"] = amp;
    // the ADSR's share of the cutoff (via velocity, min .. max), then the mod envelope
    const double e0 = V(14), e1 = V(15), envOct = lerp(e0, e1, kVel) / 100 * kCutOct;
    json fenv;
    if (std::fabs(envOct) > 0.05) {
        filt["env"] = r(envOct, 100);
        if (e1 > e0) filt["velocity"] = r((e1 - e0) / 100 * kCutOct * kVel, 100);
        fenv = {{"attack", amp["attack"]}, {"decay", amp["decay"]}, {"sustain", r(s)}, {"release", amp["release"]}};
    }
    const double form = V(4), mi = lerp(V(5), V(6), kVel) / 100;   // form: ms, < 0 decay, > 0 attack
    const int md = ri(V(24));
    if (std::fabs(mi) > 0.01 && form != 0) {
        const double t = std::fabs(form) / 1000;
        if (md == 0 && form < 0) {
            synth["pitchEnv"] = {{"amount", r(mi * 12, 100)}, {"decay", r(t / 4.6, 10000)}};
            notes.push_back("mod env -> pitch: 100% read as 12 semitones (a guess)");
        } else if (md == 3) {
            if (fenv.is_null()) {
                filt["env"] = r(mi * kCutOct, 100);
                fenv = form > 0 ? json{{"attack", r(t)}, {"decay", 0.01}, {"sustain", 1}, {"release", 0.1}} : json{{"attack", 0}, {"decay", r(t)}, {"sustain", 0}, {"release", r(t)}};
            } else notes.push_back("mod env -> cutoff not played alongside the ADSR -> cutoff (one filter envelope)");
        } else if (md == 5 && form < 0) notes.push_back("mod env -> volume (decaying) left to the amp ADSR");
        else {
            static const char *dests[8] = {"pitch (attack)", "PW", "Mix", "Cutoff", "Reso", "Volume", "Filter FM", "LFO amp"};
            notes.push_back("mod env destination " + std::to_string(md) + (md >= 0 && md < 8 ? std::string(" (") + dests[md] + ")" : "") + " not supported");
        }
    }
    if (!fenv.is_null()) synth["filterEnv"] = fenv;
    synth["filter"] = filt;
    // LFO with the wheel at rest: 0 pitch (100% = a semitone), 1 PW, 3 cutoff (100% = 3 octaves), 5 volume (guesses)
    const double li = V(27) / 100;
    const int ld = ri(V(25));
    if (std::fabs(li) > 0.005) {
        static const char *shapes[6] = {"triangle", "ramp", "saw", "square", "random", "random"};   // 6 = external: none
        const double rate = V(34);
        const int lw = ri(V(26));
        json lfo = {{"rate", rate > 0 ? json(r(rate)) : json("1/8")}};
        if (rate <= 0) notes.push_back("LFO tempo-sync index " + gnum(rate) + " unmapped; \"1/8\" used");
        if (lw >= 0 && lw < 6) lfo["shape"] = shapes[lw];
        bool ok = true;
        if (ld == 0) { lfo["depth"] = r(li); lfo["to"] = "pitch"; notes.push_back("LFO pitch depth: 100% read as a semitone (a guess)"); }
        else if (ld == 1) { lfo["depth"] = r(li * 0.4); lfo["to"] = "pw"; }
        else if (ld == 3) { lfo["depth"] = r(li * 3); lfo["to"] = "cutoff"; notes.push_back("LFO cutoff depth: 100% read as 3 octaves (a guess)"); }
        else if (ld == 5) { lfo["depth"] = r(std::min(1.0, li)); lfo["to"] = "amp"; }
        else { ok = false; notes.push_back("LFO destination " + std::to_string(ld) + " (mix or resonance) not supported"); }
        if (ok) synth["lfo"] = lfo;
    }
    if (V(1) > 0) {
        synth["glide"] = r(V(1) / 1000, 10000);
        notes.push_back("glide plays only on mono patches in builtin:synth; ES1's voice mode is unknown");
    }
    synth["level"] = r(std::clamp(-4 + (V(31) + 1), -40.0, 12.0), 100);   // a calibrated level, moved by the plugin's Out Level from its typical -1 dB
    if (V(29) > 30) {
        synth["unison"] = {{"voices", 2}, {"detune", r(V(29) / 10, 10)}, {"spread", 0.4}};
        notes.push_back("Analog drift as a 2-voice unison");
    }
    out.synth = synth;
    // chorus: 1 C1, 2 C2, 3 Ensemble (rate, depth, mix)
    static const double chorus[3][3] = {{0.5, 3, 0.35}, {0.8, 5, 0.4}, {0.3, 7, 0.5}};
    const int ch = ri(V(33));
    if (ch >= 1 && ch <= 3) out.fx.push_back({{"type", "chorus"}, {"rate", chorus[ch - 1][0]}, {"depth", chorus[ch - 1][1]}, {"mix", chorus[ch - 1][2]}});
    notes.push_back("cutoff in Hz is guess A1 (the knob spans 20 Hz .. 20 kHz exponentially)");
    return out;
}

GarageBandSynth efm1Patch(const std::vector<float> &params) {
    auto V = [&](size_t i) -> double { const double v = i < params.size() ? params[i] : 0.0; return std::fabs(v) > 1e25 ? 0.0 : v; };
    GarageBandSynth out;
    out.instrument = "EFM1";
    auto &notes = out.notes;
    json synth = json::object();
    // carrier and modulator harmonics (0 read as 0.5) plus fine; the carrier sounds at its harmonic, transposed
    const double c = (V(10) >= 1 ? V(10) : 0.5) + V(11), m = (V(18) >= 1 ? V(18) : 0.5) + V(19);
    if (V(10) < 1) notes.push_back("carrier harmonic 0 read as 0.5");
    const double semi = 12 * std::log2(std::max(0.01, c)) + V(3);
    // FM Intensity (index 8 at 1: guess A3) plus the mod envelope's share: the index starts at its peak and falls to the sustain
    const double I = V(12), D = V(34), peak = std::max(0.0, I + std::max(0.0, D)), sus = std::max(0.0, I + D * V(32));
    json fm = {{"ratio", r(c != 0 ? std::max(0.01, m / c) : 0.01, 10000)}, {"index", r(std::min(20.0, kFmIndex * peak))}};
    if (D > 0 && peak > 0) { fm["decay"] = r(std::max(0.005, V(31) / 1000)); fm["sustain"] = r(std::min(1.0, sus / peak)); }
    if (D < 0) notes.push_back("negative Mod Env -> FM (an inverted envelope) isn't played: the index stays put");
    if (V(29) > 5) notes.push_back("the Mod Env's attack on FM is ignored (the FM envelope starts at its peak)");
    json car = {{"wave", "sine"}, {"fm", fm}};
    if (std::fabs(semi) > 1e-6) car["semi"] = r(semi);
    if (V(4) != 0) car["cents"] = r(V(4), 100);
    json oscs = json::array();
    oscs.push_back(car);
    if (V(24) > 0.01) oscs.push_back({{"wave", "sine"}, {"semi", r(V(3) - 12)}, {"level", r(V(24))}});   // the sub: a sine an octave down
    synth["osc"] = oscs;
    if (V(15) >= 0.5) notes.push_back("Fixed Carrier (detached from the keyboard) isn't played: the carrier follows the key");
    if (V(20) > 0.01) notes.push_back("modulator wave " + gnum(V(20)) + " is a digital wave; the FM modulator here is a sine");
    if (std::fabs(V(35)) > 0.01) notes.push_back("Mod Env -> modulator pitch isn't played");
    synth["filter"] = {{"type", "off"}};
    synth["amp"] = {{"attack", r(std::max(0.001, V(40) / 1000), 10000)}, {"decay", r(std::max(0.005, V(42) / 1000))}, {"sustain", r(V(43))},
                    {"release", r(std::max(0.005, V(44) / 1000))}, {"velocity", r(V(6))}};
    const int voices = ri(V(1));   // 0 mono, 1 legato, 2-16 poly
    if (voices == 0 || voices == 1) {
        synth["mono"] = true;
        synth["legato"] = voices == 1;
        if (V(8) > 0) synth["glide"] = r(V(8) / 1000, 10000);
    }
    if (V(13) > 0.5) synth["unison"] = {{"voices", 2}, {"detune", r(V(13), 10)}, {"spread", 1}};   // stereo detune
    if (V(2) >= 0.5) notes.push_back("Unison (two layered voices) as the stereo-detune unison");
    const double la = V(53);   // LFO amount: < 0 vibrato, > 0 FM
    if (la < -0.005) {
        synth["lfo"] = {{"rate", r(V(48))}, {"depth", r(-la)}, {"to", "pitch"}};
        notes.push_back("LFO vibrato: amount 1 read as a semitone (a guess)");
    } else if (la > 0.005) notes.push_back("LFO -> FM (" + num(la, 2, true) + " at " + num(V(48), 2) + " Hz) isn't played (no LFO FM target)");
    synth["level"] = r(std::clamp(-6 + (V(5) + 6), -40.0, 12.0), 100);   // a calibrated level, moved by the plugin's Main Level from its typical -6 dB
    notes.push_back("FM index scale is guess A3 (FM Intensity 1 = index 8)");
    out.synth = synth;
    return out;
}

} // namespace wl
