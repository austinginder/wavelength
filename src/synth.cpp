#include "synth.hpp"

#include "automation.hpp"
#include "dsp.hpp"
#include "effects.hpp"
#include "loudness.hpp"
#include "retro_synth.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace wl {

namespace {

using json = nlohmann::json;
constexpr double TAU = 2 * dsp::kPi;

// ---- patches -----------------------------------------------------------------------------------
// Each patch is the "synth" object an agent could write itself; "about" describes it for `presets`.
// Levels are set so every patch playing a phrase in its own register measures about the same LUFS.
const char *kPatches = R"JSON({
"Init": {"about": "Plain saw through an open low-pass: the starting point", "level": -5,
  "osc": [{"wave": "saw"}], "filter": {"cutoff": 8000}, "amp": {"attack": 0.003, "decay": 0.2, "sustain": 1, "release": 0.12}},

"BA Sub": {"about": "Clean sine sub bass with a little triangle so it reads on small speakers", "level": -3,
  "osc": [{"wave": "sine"}, {"wave": "triangle", "level": 0.22}], "filter": {"type": "off"},
  "amp": {"attack": 0.004, "decay": 0.2, "sustain": 1, "release": 0.1}},
"BA Analog": {"about": "Round Moog-style saw bass with a sub and a short filter pluck", "level": -2,
  "osc": [{"wave": "saw"}], "sub": 0.15,
  "filter": {"slope": 24, "cutoff": 380, "resonance": 0.15, "keytrack": 0.3, "env": 2.2},
  "filterEnv": {"attack": 0, "decay": 0.25, "sustain": 0.15, "release": 0.2},
  "amp": {"attack": 0.003, "decay": 0.4, "sustain": 0.85, "release": 0.12}},
"BA Reese": {"about": "Two detuned saws beating slowly under a dark filter: drum and bass, dubstep", "level": -5,
  "osc": [{"wave": "saw", "cents": -11}, {"wave": "saw", "cents": 11}], "sub": 0.15,
  "filter": {"slope": 24, "cutoff": 900, "resonance": 0.1, "drive": 0.3},
  "amp": {"attack": 0.005, "decay": 0.3, "sustain": 1, "release": 0.15}},
"BA Acid": {"about": "303-style acid: resonant 24 dB filter, snappy envelope, slides between overlapping notes; automate cutoff and resonance", "level": -6,
  "osc": [{"wave": "saw"}], "mono": true, "glide": 0.06,
  "filter": {"slope": 24, "cutoff": 320, "resonance": 0.72, "keytrack": 0.2, "env": 3.2, "velocity": 1.2, "drive": 0.35},
  "filterEnv": {"attack": 0, "decay": 0.18, "sustain": 0, "release": 0.1},
  "amp": {"attack": 0.002, "decay": 0.3, "sustain": 0.9, "release": 0.06}},
"BA Pluck": {"about": "Short plucked square bass for off-beat and house lines", "level": 2,
  "osc": [{"wave": "square"}], "sub": 0.15,
  "filter": {"slope": 24, "cutoff": 260, "resonance": 0.2, "env": 3},
  "filterEnv": {"attack": 0, "decay": 0.12, "sustain": 0, "release": 0.1},
  "amp": {"attack": 0.002, "decay": 0.35, "sustain": 0, "release": 0.1}},
"BA FM": {"about": "Punchy DX-style FM bass with a bright attack", "level": -1,
  "osc": [{"wave": "sine", "fm": {"ratio": 1, "index": 3.2, "decay": 0.25, "sustain": 0.25}}], "sub": 0.15,
  "filter": {"cutoff": 3200},
  "amp": {"attack": 0.002, "decay": 0.5, "sustain": 0.7, "release": 0.1}},

"LD Saw": {"about": "Classic two-saw lead with delayed vibrato and glide", "level": -4,
  "osc": [{"wave": "saw", "cents": -6}, {"wave": "saw", "cents": 6}], "mono": true, "glide": 0.04,
  "filter": {"cutoff": 5500, "resonance": 0.1, "env": 0.8},
  "filterEnv": {"attack": 0, "decay": 0.3, "sustain": 0.5, "release": 0.2},
  "amp": {"attack": 0.005, "decay": 0.2, "sustain": 0.9, "release": 0.2},
  "lfo": {"rate": 5.5, "depth": 0.12, "to": "pitch", "delay": 0.35, "fade": 0.3}},
"LD Square": {"about": "Hollow square lead with slow pulse-width movement", "level": -8,
  "osc": [{"wave": "square", "pw": 0.5}], "filter": {"cutoff": 4500},
  "amp": {"attack": 0.004, "decay": 0.2, "sustain": 0.9, "release": 0.15},
  "lfo": [{"rate": 0.4, "depth": 0.25, "to": "pw"}, {"rate": 5.2, "depth": 0.1, "to": "pitch", "delay": 0.4, "fade": 0.3}]},
"LD Supersaw": {"about": "Wide 7-voice supersaw for trance leads and anthems", "level": -4,
  "osc": [{"wave": "saw"}], "unison": {"voices": 7, "detune": 38, "spread": 0.9},
  "filter": {"cutoff": 9000},
  "amp": {"attack": 0.005, "decay": 0.3, "sustain": 0.9, "release": 0.3}},
"LD Soft": {"about": "Soft, flute-like triangle lead with vibrato", "level": -4,
  "osc": [{"wave": "triangle"}, {"wave": "sine", "octave": 1, "level": 0.25}], "filter": {"cutoff": 3000},
  "amp": {"attack": 0.02, "decay": 0.2, "sustain": 0.9, "release": 0.25},
  "lfo": {"rate": 5, "depth": 0.15, "to": "pitch", "delay": 0.4, "fade": 0.4}},
"LD Chip": {"about": "NES-style 25% pulse lead, no filter", "level": -7,
  "osc": [{"wave": "square", "pw": 0.25}], "filter": {"type": "off"},
  "amp": {"attack": 0.001, "decay": 0.1, "sustain": 0.8, "release": 0.03}},

"PD Warm": {"about": "Warm analog pad: two detuned saws, slow attack, a breathing filter", "level": -7,
  "osc": [{"wave": "saw"}, {"wave": "saw", "cents": 4, "level": 0.8}], "unison": {"voices": 2, "detune": 14, "spread": 0.8},
  "filter": {"slope": 24, "cutoff": 1400, "resonance": 0.1, "keytrack": 0.3},
  "amp": {"attack": 0.6, "decay": 1, "sustain": 0.9, "release": 1.2},
  "lfo": {"rate": 0.25, "depth": 0.3, "to": "cutoff"}},
"PD Supersaw": {"about": "Wide, bright supersaw pad for breakdowns", "level": -8,
  "osc": [{"wave": "saw"}], "unison": {"voices": 7, "detune": 30, "spread": 1},
  "filter": {"cutoff": 3500},
  "amp": {"attack": 0.8, "decay": 1, "sustain": 1, "release": 1.5}},
"PD Strings": {"about": "String-machine ensemble (Solina-style)", "level": -8,
  "osc": [{"wave": "saw"}], "unison": {"voices": 5, "detune": 16, "spread": 0.8},
  "filter": {"cutoff": 2600, "resonance": 0.05, "keytrack": 0.4},
  "amp": {"attack": 0.35, "decay": 0.5, "sustain": 0.9, "release": 0.6},
  "lfo": {"rate": 5, "depth": 0.06, "to": "pitch", "delay": 0.3, "fade": 0.4}},
"PD Glass": {"about": "Glassy FM pad with a soft attack", "level": -9,
  "osc": [{"wave": "sine", "fm": {"ratio": 3, "index": 1.2, "decay": 2.5, "sustain": 0.3}}, {"wave": "sine", "octave": 1, "level": 0.2}],
  "unison": {"voices": 2, "detune": 8, "spread": 0.7}, "filter": {"type": "off"},
  "amp": {"attack": 0.4, "decay": 1, "sustain": 0.8, "release": 1.5}},
"PD Dark": {"about": "Dark, slowly moving pad for intros and ambient beds", "level": -7,
  "osc": [{"wave": "triangle"}, {"wave": "saw", "cents": 5, "level": 0.5}], "unison": {"voices": 2, "detune": 10, "spread": 0.6},
  "filter": {"slope": 24, "cutoff": 700, "resonance": 0.15},
  "amp": {"attack": 1.2, "decay": 1, "sustain": 1, "release": 2},
  "lfo": {"rate": "4/1", "depth": 0.5, "to": "cutoff"}},

"PL Pluck": {"about": "Bright plucked saw for arps and melodies", "level": 1,
  "osc": [{"wave": "saw"}, {"wave": "square", "octave": 1, "level": 0.3}],
  "filter": {"slope": 24, "cutoff": 500, "resonance": 0.25, "env": 4},
  "filterEnv": {"attack": 0, "decay": 0.16, "sustain": 0, "release": 0.15},
  "amp": {"attack": 0.001, "decay": 0.4, "sustain": 0, "release": 0.25}},
"PL Bell": {"about": "FM bell with a long decay", "level": -10,
  "osc": [{"wave": "sine", "fm": {"ratio": 4, "index": 2.5, "decay": 1.2, "sustain": 0.1}}, {"wave": "sine", "semi": 17.6, "level": 0.18, "decay": 0.9}],
  "filter": {"type": "off"},
  "amp": {"attack": 0.001, "decay": 2.5, "sustain": 0, "release": 1.5}},
"PL Mallet": {"about": "Marimba-like FM mallet", "level": -3,
  "osc": [{"wave": "sine", "fm": {"ratio": 4, "index": 1.5, "decay": 0.12}}, {"wave": "sine", "octave": 2, "level": 0.15, "decay": 0.08}],
  "filter": {"type": "off"},
  "amp": {"attack": 0.001, "decay": 0.6, "sustain": 0, "release": 0.3}},
"PL Chip Arp": {"about": "Thin 12.5% pulse for fast chiptune arpeggios", "level": 1,
  "osc": [{"wave": "square", "pw": 0.125}], "filter": {"type": "off"},
  "amp": {"attack": 0.001, "decay": 0.12, "sustain": 0.2, "release": 0.02}},

"KY Electric Piano": {"about": "DX7-style FM electric piano with a bell-like tine", "level": -2,
  "osc": [{"wave": "sine", "fm": {"ratio": 1, "index": 1.6, "decay": 1.0, "sustain": 0.2}}, {"wave": "sine", "octave": 2, "level": 0.15, "decay": 0.15}],
  "filter": {"type": "off"},
  "amp": {"attack": 0.002, "decay": 3, "sustain": 0, "release": 0.4, "velocity": 0.8}},
"KY Organ": {"about": "Drawbar organ: sines at 8', 4', 2 2/3' and 2' with a light vibrato", "level": -12,
  "osc": [{"wave": "sine"}, {"wave": "sine", "octave": 1, "level": 0.6}, {"wave": "sine", "octave": 1, "semi": 7, "level": 0.35},
          {"wave": "sine", "octave": 2, "level": 0.3}],
  "filter": {"type": "off"},
  "amp": {"attack": 0.005, "decay": 0.1, "sustain": 1, "release": 0.05, "velocity": 0.2},
  "lfo": {"rate": 6.5, "depth": 0.05, "to": "pitch"}},

"BR Brass": {"about": "Analog brass: the filter swells open with each attack", "level": -5,
  "osc": [{"wave": "saw", "cents": -5}, {"wave": "saw", "cents": 5}],
  "filter": {"cutoff": 700, "resonance": 0.1, "env": 2.4, "keytrack": 0.3, "velocity": 0.8},
  "filterEnv": {"attack": 0.06, "decay": 0.5, "sustain": 0.6, "release": 0.3},
  "amp": {"attack": 0.03, "decay": 0.3, "sustain": 0.85, "release": 0.2},
  "lfo": {"rate": 5, "depth": 0.08, "to": "pitch", "delay": 0.5, "fade": 0.4}},
"BR Stab": {"about": "Short brassy stab for chords and hits", "level": -2,
  "osc": [{"wave": "saw"}], "unison": {"voices": 3, "detune": 14, "spread": 0.6},
  "filter": {"cutoff": 900, "env": 3},
  "filterEnv": {"attack": 0, "decay": 0.2, "sustain": 0.1, "release": 0.15},
  "amp": {"attack": 0.002, "decay": 0.35, "sustain": 0.2, "release": 0.15}},

"FX Zap": {"about": "Laser zap: the pitch falls from three octaves up", "level": 1,
  "osc": [{"wave": "square"}], "pitchEnv": {"amount": 36, "decay": 0.06}, "filter": {"type": "off"},
  "amp": {"attack": 0.001, "decay": 0.2, "sustain": 0, "release": 0.05}},
"FX Noise Sweep": {"about": "Filtered noise that opens over the note: automate cutoff for risers", "level": 7,
  "osc": [], "noise": 1,
  "filter": {"cutoff": 300, "resonance": 0.35, "env": 5},
  "filterEnv": {"attack": 1.5, "decay": 1, "sustain": 0, "release": 0.5},
  "amp": {"attack": 0.2, "decay": 1, "sustain": 1, "release": 0.8}}
})JSON";

const json &patchBank() {
    static const json bank = json::parse(kPatches);
    return bank;
}

std::string categoryOf(const std::string &name) {
    static const std::pair<const char *, const char *> cats[] = {
        {"BA ", "Bass"}, {"LD ", "Lead"}, {"PD ", "Pad"}, {"PL ", "Pluck"}, {"KY ", "Keys"}, {"BR ", "Brass"}, {"FX ", "FX"}};
    for (auto &[p, c] : cats) if (name.rfind(p, 0) == 0) return c;
    return "Init";
}

// ---- parameters (the names "params" and "automation.params" take) ------------------------------
const std::vector<SynthParamInfo> kParams = {
    {"cutoff", "Hz", "filter cutoff (before key tracking, envelope, velocity and LFO)", 20, 20000, 8000, true},
    {"resonance", "", "filter resonance, 0 flat to 1 ringing", 0, 1, 0, false},
    {"drive", "", "saturation into the filter, 0 clean to 1 hot", 0, 1, 0, false},
    {"env", "oct", "filter envelope amount in octaves (negative closes the filter)", -8, 8, 0, false},
    {"keytrack", "", "how far the cutoff follows the note (1 = a full octave per octave, from C4)", 0, 1, 0, false},
    {"detune", "cents", "unison spread from lowest to highest voice", 0, 100, 0, false},
    {"spread", "", "unison stereo width", 0, 1, 0.5, false},
    {"pw", "", "pulse width of square oscillators (0.5 square, 0.1 thin)", 0.02, 0.98, 0.5, false},
    {"fm", "x", "multiplies every FM index (brightness of FM sounds)", 0, 4, 1, false},
    {"lfo", "x", "multiplies every LFO depth (0 stops vibrato, filter wobble...)", 0, 4, 1, false},
    {"sub", "", "sub oscillator level (square an octave down)", 0, 1, 0, false},
    {"noise", "", "noise level", 0, 1, 0, false},
    {"glide", "s", "portamento time (mono patches; poly patches ignore it)", 0, 2, 0, false},
    {"level", "dB", "output level of the patch", -40, 12, 0, false},
};
enum ParamId { P_CUTOFF, P_RES, P_DRIVE, P_ENV, P_KEYTRACK, P_DETUNE, P_SPREAD, P_PW, P_FM, P_LFO, P_SUB, P_NOISE, P_GLIDE, P_LEVEL, P_COUNT };

int paramIndex(std::string n) {
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    if (n == "filter.cutoff" || n == "frequency" || n == "freq") n = "cutoff";
    if (n == "filter.resonance" || n == "res" || n == "q") n = "resonance";
    for (size_t i = 0; i < kParams.size(); ++i) if (kParams[i].name == n) return (int)i;
    return -1;
}

std::string paramNames() {
    std::string s;
    for (auto &p : kParams) s += (s.empty() ? "" : ", ") + p.name;
    return s;
}

// ---- the patch as data -------------------------------------------------------------------------
struct Adsr { double a = 0.003, d = 0.3, s = 1.0, r = 0.15; };

// An additive oscillator's sine partials, ready to play. A harmonic set (every partial a whole multiple of the note, no
// shiftHz) plays from mip-mapped single-cycle tables, one per third of an octave of fundamentals, each holding the
// partials that stay under 0.45 x the sample rate at the top of its band (faded out from 0.40) and 16 x oversampled
// for linear interpolation. An inharmonic or shifted set plays as a bank of sines, each faded the same way as it nears
// 0.45 x the sample rate.
struct AdditiveSet {
    struct Part { double amp, ratio, phase, gl, gr; };   // amplitude, frequency ratio, start phase, left and right gains
    std::vector<Part> parts;                              // ratio order, scaled together (summed power at most a sine's)
    bool tables = false, stereo = false;
    double shiftHz = 0;
    struct Table { double fmax; size_t n; std::vector<float> l, r; };   // for fundamentals up to fmax Hz; n + 1 samples
    std::vector<Table> mip;
    int pick(double hz) const {
        for (size_t k = 0; k < mip.size(); ++k) if (mip[k].fmax >= hz) return (int)k;
        return (int)mip.size() - 1;
    }
};

struct Osc {
    enum Wave { Saw, Square, Triangle, Sine, Noise, Additive } wave = Saw;
    double level = 1, pitch = 0, pw = 0.5, decay = 0;   // pitch in semitones; decay: own amplitude decay (s), 0 = none
    bool fm = false;
    double fmRatio = 1, fmIndex = 0, fmDecay = 0, fmSustain = 0;
    bool filtered = true;   // false: joins after the filters (Retro Synth's sine level)
    int join = 0;           // filtered: the filter it joins the chain at (0: all of them)
    bool sync = false;      // hard sync: restarts whenever the first oscillator starts a cycle
    std::shared_ptr<const AdditiveSet> add;   // Additive: its partials
    double lowcut = 0, highcut = 0;           // Noise: its band in Hz (0 = open), 12 dB/oct each
    double keyDb = 0, keyCenter = 60;         // keytrack: dB per octave away from keyCenter, never above level
    double phase = -1;                        // start phase (cycles), every unison copy alike; -1: each copy its own
};

struct SynthLfo {
    Lfo lfo;
    enum Dest { Pitch, Cutoff, Amp, Pw, Pan } to = Pitch;
    size_t filter = 0;            // Cutoff: which of the filters
    double delay = 0, fade = 0;   // seconds after the note starts
    // the depth with the mod wheel (CC 1) all the way up and with full channel pressure: the track's controller curves
    // move the depth from "depth" toward them (NaN: not moved)
    double wheel = NAN, pressure = NAN;
};

// One filter of the voice's chain (the first one's cutoff, resonance, key tracking, envelope and velocity amounts
// are kept with the patch's parameters, so they can be automated)
struct Stage {
    enum Type { Off, Lowpass, Highpass, Bandpass, Peak, Notch, Formant, Comb, Ring, Fm, Downsample } type = Lowpass;
    int slope = 12;
    double cutoff = 8000, res = 0, keytrack = 0, env = 0, velocity = 0;
    double mix = 1;              // the share of the voice it takes (the rest passes by)
    bool parallel = false;       // takes the same signal as the filter before it, their outputs added
    double gain = 0, q = 2;      // peak: gain in dB; peak, notch, formant: Q
    bool negative = false;       // comb: negative feedback
    double damp = 0;             // comb: high frequencies lost on each pass, 0-1
    double offset = 0, weird = 0;          // ring: dry voice on the carrier, carrier shape
    double depth = 0, feedback = 0;        // fm: the voice's and the carrier's own phase modulation (radians)
};

struct Patch {
    std::vector<Osc> osc;
    double p[P_COUNT] = {};
    int unison = 1;
    Stage::Type ftype = Stage::Lowpass;
    Stage first;                  // the first filter's own settings beyond the parameters (mix, q, gain, comb, ring, fm)
    std::vector<Stage> more;      // the filters after it, in series or parallel
    int slope = 12;
    double velToCutoff = 0;       // octaves at velocity 0 (1 = an octave darker when soft)
    Adsr amp, fenv{0, 0.3, 0, 0.3};
    double ampVel = 0.6;
    double ampKey = 0;            // 1: amp decay and release halve every octave above C4
    double pitchEnvAmt = 0, pitchEnvDecay = 0.05;
    bool mono = false, legato = true;
    std::vector<SynthLfo> lfos;
};

void checkKeys(const json &o, std::initializer_list<const char *> keys, const std::string &where, std::vector<std::string> &warnings) {
    for (auto &[k, v] : o.items()) {
        bool ok = false;
        for (const char *x : keys) ok |= k == x;
        if (!ok) warnings.push_back("synth: unknown setting '" + where + k + "' ignored");
    }
}

double num(const json &o, const char *k, double def, const std::string &where) {
    if (!o.contains(k)) return def;
    const auto &v = o[k];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        double x;
        if (Envelope::builtinText(v.get<std::string>(), x)) return x;
    }
    throw std::runtime_error("synth: '" + where + k + "' must be a number (or text like \"800 Hz\"), not " + v.dump());
}

// The amplitude of each harmonic m of a "partialWave" (sine 1, saw 1/m, a pulse of width pw its own series, triangle
// odd 1/m^2), the fundamental's 1, as complex numbers: the real part sine phase, the imaginary part cosine phase
std::complex<double> waveHarmonic(const std::string &wave, double pw, int m) {
    if (wave == "saw") return {1.0 / m, 0};
    if (wave == "triangle") return m % 2 ? std::complex<double>(0, -1.0 / ((double)m * m)) : 0.0;
    if (wave == "square") {   // +1 for pw of the cycle, -1 after: (2 / pi m) (1 - cos 2 pi m pw, sin 2 pi m pw), over 4 / pi
        const double a = TAU * m * pw;
        return std::complex<double>(1 - std::cos(a), std::sin(a)) / (2.0 * m);
    }
    return m == 1 ? 1.0 : 0.0;
}

// "partials" ([amplitude, ratio] or [amplitude, ratio, pan] each) and "harmonics" ({"count", "tilt", "odd", "even"}),
// each partial playing "partialWave" (its harmonics spread into sine partials at their own ratios), plus "shiftHz",
// as the sine partials an additive oscillator plays at this sample rate
std::shared_ptr<const AdditiveSet> parseAdditive(const json &o, const std::string &w, double sr, double pw, std::vector<std::string> &warnings) {
    struct In { double amp, ratio, pan; };
    std::vector<In> in;
    if (o.contains("harmonics")) {
        const json &h = o["harmonics"];
        if (!h.is_object() && !h.is_number()) throw std::runtime_error("synth: " + w + "harmonics is a count or {\"count\": 64, \"tilt\": -6, \"odd\": 1, \"even\": 1}");
        if (h.is_object()) checkKeys(h, {"count", "tilt", "odd", "even"}, w + "harmonics.", warnings);
        const int count = (int)std::clamp(h.is_number() ? h.get<double>() : num(h, "count", 64, w + "harmonics."), 1.0, 1024.0);
        const double tilt = h.is_number() ? -6 : std::clamp(num(h, "tilt", -6, w + "harmonics."), -48.0, 24.0);
        const double odd = h.is_number() ? 1 : std::max(0.0, num(h, "odd", 1, w + "harmonics.")), even = h.is_number() ? 1 : std::max(0.0, num(h, "even", 1, w + "harmonics."));
        for (int k = 1; k <= count; ++k) in.push_back({std::pow((double)k, tilt / 6.0206) * (k % 2 ? odd : even), (double)k, 0});
    }
    if (o.contains("partials")) {
        const json &l = o["partials"];
        if (!l.is_array()) throw std::runtime_error("synth: " + w + "partials is a list of [amplitude, ratio] or [amplitude, ratio, pan]");
        for (auto &p : l) {
            if (!p.is_array() || p.size() < 2 || p.size() > 3 || !p[0].is_number() || !p[1].is_number() || (p.size() == 3 && !p[2].is_number()))
                throw std::runtime_error("synth: " + w + "partials: each is [amplitude, ratio] or [amplitude, ratio, pan], not " + p.dump());
            const double ratio = p[1].get<double>();
            if (!(ratio > 0) || ratio > 1024) throw std::runtime_error("synth: " + w + "partials: a ratio is above 0 and at most 1024, not " + p.dump());
            in.push_back({std::max(0.0, p[0].get<double>()), ratio, p.size() == 3 ? std::clamp(p[2].get<double>(), -1.0, 1.0) : 0.0});
        }
    }
    if (in.empty()) throw std::runtime_error("synth: " + w + "an additive oscillator needs \"partials\" ([[1, 1], [0.5, 2], ...]) or \"harmonics\"");
    if (in.size() > 1024) throw std::runtime_error("synth: " + w + "partials: at most 1024");
    std::string wave = o.value("partialWave", "sine");
    if (wave == "pulse") wave = "square";
    if (wave == "tri") wave = "triangle";
    if (wave != "sine" && wave != "saw" && wave != "square" && wave != "triangle")
        throw std::runtime_error("synth: " + w + "partialWave '" + wave + "' must be sine, saw, square (or pulse) or triangle");
    // every partial's harmonics (one for a sine), summed where they meet at a ratio
    struct Acc { std::complex<double> z; double weight = 0, pan = 0; };
    std::map<long long, Acc> at;
    for (auto &p : in) {
        if (p.amp <= 0) continue;
        for (int m = 1; p.ratio * m <= 1024 + 1e-9 && (wave != "sine" || m == 1); ++m) {
            const std::complex<double> c = waveHarmonic(wave, pw, m);
            if (std::abs(c) < 1e-6) continue;
            Acc &a = at[std::llround(p.ratio * m * 1e6)];
            a.z += p.amp * c;
            a.weight += p.amp * std::abs(c);
            a.pan += p.amp * std::abs(c) * p.pan;
        }
    }
    auto set = std::make_shared<AdditiveSet>();
    double top = 0, power = 0;
    for (auto &[k, a] : at) top = std::max(top, std::abs(a.z));
    for (auto &[k, a] : at) {
        const double amp = std::abs(a.z);
        if (amp < 1e-6 * top) continue;
        const double pan = a.weight > 0 ? a.pan / a.weight : 0, ang = (pan + 1) * dsp::kPi / 4;
        const bool panned = std::fabs(pan) > 1e-6;
        set->parts.push_back({amp, (double)k / 1e6, std::arg(a.z), panned ? std::cos(ang) * M_SQRT2 : 1.0, panned ? std::sin(ang) * M_SQRT2 : 1.0});
        set->stereo |= panned;
        power += amp * amp;
    }
    if (set->parts.empty()) warnings.push_back("synth: " + w + "partials: every amplitude is 0: it makes no sound");
    for (auto &p : set->parts) p.amp /= std::sqrt(std::max(1.0, power));
    set->shiftHz = std::clamp(num(o, "shiftHz", 0, w), -20000.0, 20000.0);
    set->tables = set->shiftHz == 0;
    for (auto &p : set->parts) set->tables &= std::fabs(p.ratio - std::round(p.ratio)) < 1e-6;
    if (!set->tables || set->parts.empty()) return set;
    // the tables: harmonic h at fundamental fmax keeps (0.45 sr - h fmax) / (0.05 sr) of its level, up to 1
    const int hmax = (int)std::lround(set->parts.back().ratio);
    std::vector<double> prev;
    for (double fmax = 0.40 * sr / hmax;; fmax *= std::pow(2.0, 1.0 / 3)) {
        std::vector<double> g(set->parts.size());
        int kmax = 0;
        for (size_t k = 0; k < g.size(); ++k) {
            const double h = std::round(set->parts[k].ratio);
            g[k] = std::clamp((0.45 * sr - h * fmax) / (0.05 * sr), 0.0, 1.0);
            if (g[k] > 0) kmax = (int)h;
        }
        if (!kmax) break;
        if (g == prev) { set->mip.back().fmax = fmax; continue; }   // the same partials: the band below grows
        prev = g;
        size_t n = 256;
        while (n < 16 * (size_t)kmax) n *= 2;
        AdditiveSet::Table t{fmax, n, {}, {}};
        for (int side = 0; side < (set->stereo ? 2 : 1); ++side) {
            // x(j) = sum of g a sin(2 pi h j / n + phase): the imaginary part of sum g a e^(i phase) e^(i 2 pi h j / n)
            std::vector<std::complex<double>> s(n);
            for (size_t k = 0; k < g.size(); ++k) {
                const auto &p = set->parts[k];
                if (g[k] > 0) s[(size_t)std::lround(p.ratio)] = std::conj(std::polar(g[k] * p.amp * (side ? p.gr : set->stereo ? p.gl : 1.0), p.phase));
            }
            dsp::fft(s);
            auto &out = side ? t.r : t.l;
            out.resize(n + 1);
            for (size_t j = 0; j < n; ++j) out[j] = (float)-s[j].imag();
            out[n] = out[0];
        }
        set->mip.push_back(std::move(t));
        if (fmax >= 0.45 * sr) break;
    }
    return set;
}

Adsr parseAdsr(const json &o, Adsr d, const std::string &where, std::vector<std::string> &warnings) {
    if (o.is_null()) return d;
    if (!o.is_object()) throw std::runtime_error("synth: '" + where + "' must be an object with attack, decay, sustain, release");
    if (where == "amp") checkKeys(o, {"attack", "decay", "sustain", "release", "velocity", "keytrack"}, where + ".", warnings);
    else checkKeys(o, {"attack", "decay", "sustain", "release", "velocity"}, where + ".", warnings);
    d.a = std::max(0.0, num(o, "attack", d.a, where + "."));
    d.d = std::max(0.0, num(o, "decay", d.d, where + "."));
    d.s = std::clamp(num(o, "sustain", d.s, where + "."), 0.0, 1.0);
    d.r = std::max(0.0, num(o, "release", d.r, where + "."));
    return d;
}

// One filter object: its type, the keys every type takes, and its type's own
Stage parseStage(const json &f, const std::string &w, bool first, std::vector<std::string> &warnings) {
    if (!f.is_object()) throw std::runtime_error("synth: '" + w.substr(0, w.size() - 1) + "' must be an object like {\"type\": \"lowpass\", \"cutoff\": 800}");
    Stage s;
    const std::string t = f.value("type", "lowpass");
    static const std::pair<const char *, Stage::Type> types[] = {
        {"lowpass", Stage::Lowpass}, {"lp", Stage::Lowpass}, {"highpass", Stage::Highpass}, {"hp", Stage::Highpass}, {"bandpass", Stage::Bandpass},
        {"bp", Stage::Bandpass}, {"peak", Stage::Peak}, {"notch", Stage::Notch}, {"formant", Stage::Formant}, {"comb", Stage::Comb},
        {"ring", Stage::Ring}, {"fm", Stage::Fm}, {"downsample", Stage::Downsample}, {"off", Stage::Off}, {"none", Stage::Off}};
    bool known = false;
    for (auto &[name, ty] : types) if (t == name) s.type = ty, known = true;
    if (!known) throw std::runtime_error("synth: " + w + "type '" + t + "' must be lowpass, highpass, bandpass, peak, notch, formant, comb, ring, fm, downsample or off");
    checkKeys(f, {"type", "slope", "cutoff", "resonance", "keytrack", "env", "velocity", "drive", "mix", "parallel", "gain", "q", "negative", "damp", "offset",
                  "weird", "depth", "feedback"}, w, warnings);
    // the keys only some types take: which ones, for the warning when another type has them
    const bool svf = s.type == Stage::Lowpass || s.type == Stage::Highpass || s.type == Stage::Bandpass || s.type == Stage::Off;
    const std::pair<const char *, bool> own[] = {{"slope", svf}, {"resonance", svf || s.type == Stage::Comb}, {"gain", s.type == Stage::Peak},
        {"q", s.type == Stage::Peak || s.type == Stage::Notch || s.type == Stage::Formant}, {"negative", s.type == Stage::Comb}, {"damp", s.type == Stage::Comb},
        {"offset", s.type == Stage::Ring}, {"weird", s.type == Stage::Ring}, {"depth", s.type == Stage::Fm}, {"feedback", s.type == Stage::Fm}, {"drive", first},
        {"parallel", !first}};
    for (auto &[k, ok] : own)
        if (f.contains(k) && !ok)
            warnings.push_back("synth: " + w + k + (std::string(k) == "drive" ? " only applies to the first filter" : std::string(k) == "parallel" ? " applies to the filters after the first" :
                                                    " doesn't apply to a " + t + " filter") + "; ignored");
    s.slope = f.value("slope", 12);
    if (s.slope != 12 && s.slope != 24) throw std::runtime_error("synth: " + w + "slope is 12 or 24 (dB per octave)");
    s.cutoff = std::clamp(num(f, "cutoff", 8000, w), 20.0, 20000.0);
    s.res = std::clamp(num(f, "resonance", 0, w), 0.0, s.type == Stage::Comb ? 0.99 : 1.0);
    s.keytrack = std::clamp(num(f, "keytrack", 0, w), 0.0, 1.0);
    s.env = std::clamp(num(f, "env", 0, w), -8.0, 8.0);
    s.velocity = std::clamp(num(f, "velocity", 0, w), 0.0, 4.0);
    s.mix = std::clamp(num(f, "mix", 1, w), 0.0, 1.0);
    s.gain = std::clamp(num(f, "gain", 12, w), -24.0, 24.0);
    s.q = std::clamp(num(f, "q", 2, w), 0.3, 30.0);
    s.parallel = !first && f.value("parallel", false);
    s.negative = f.value("negative", false);
    s.damp = std::clamp(num(f, "damp", 0, w), 0.0, 1.0);
    s.offset = std::clamp(num(f, "offset", 0, w), 0.0, 1.0);
    s.weird = std::clamp(num(f, "weird", 0, w), 0.0, 1.0);
    s.depth = std::clamp(num(f, "depth", 1, w), 0.0, 20.0);
    s.feedback = std::clamp(num(f, "feedback", 0, w), 0.0, 1.5);
    return s;
}

Patch parsePatch(const json &j, const Job &job, std::vector<std::string> &warnings) {
    Patch P;
    for (size_t i = 0; i < kParams.size(); ++i) P.p[i] = kParams[i].def;
    checkKeys(j, {"about", "osc", "sub", "noise", "unison", "filter", "amp", "filterEnv", "pitchEnv", "lfo", "glide", "mono", "legato", "level"}, "", warnings);
    const json oscs = j.contains("osc") ? j["osc"] : json::array({json{{"wave", "saw"}}});
    if (!oscs.is_array()) throw std::runtime_error("synth: 'osc' must be a list of oscillators, e.g. [{\"wave\": \"saw\"}]");
    if (oscs.size() > 12) throw std::runtime_error("synth: at most 12 oscillators");
    for (size_t i = 0; i < oscs.size(); ++i) {
        const auto &o = oscs[i];
        const std::string w = "osc[" + std::to_string(i) + "].";
        if (!o.is_object()) throw std::runtime_error("synth: each oscillator is an object like {\"wave\": \"saw\"}");
        Osc x;
        const std::string wave = o.value("wave", "saw");
        if (wave == "saw") x.wave = Osc::Saw;
        else if (wave == "square" || wave == "pulse") x.wave = Osc::Square;
        else if (wave == "triangle" || wave == "tri") x.wave = Osc::Triangle;
        else if (wave == "sine") x.wave = Osc::Sine;
        else if (wave == "noise") x.wave = Osc::Noise;
        else if (wave == "additive") x.wave = Osc::Additive;
        else throw std::runtime_error("synth: " + w + "wave '" + wave + "' must be saw, square (or pulse), triangle, sine, noise or additive");
        checkKeys(o, {"wave", "level", "octave", "semi", "cents", "pw", "decay", "fm", "filter", "sync", "partials", "harmonics", "partialWave", "shiftHz",
                      "lowcut", "highcut", "keytrack", "keycenter", "phase"}, w, warnings);
        for (const char *k : {"partials", "harmonics", "partialWave", "shiftHz"})
            if (x.wave != Osc::Additive && o.contains(k)) warnings.push_back("synth: " + w + k + " only applies to \"wave\": \"additive\"; ignored");
        x.level = std::max(0.0, num(o, "level", 1, w));
        x.pitch = 12 * num(o, "octave", 0, w) + num(o, "semi", 0, w) + num(o, "cents", 0, w) / 100;
        x.pw = std::clamp(num(o, "pw", 0.5, w), 0.02, 0.98);
        x.decay = std::max(0.0, num(o, "decay", 0, w));
        x.keyDb = std::clamp(num(o, "keytrack", 0, w), -48.0, 48.0);
        if (o.contains("keycenter")) {
            if (!o.contains("keytrack")) warnings.push_back("synth: " + w + "keycenter only applies with keytrack; ignored");
            x.keyCenter = parseKey(o["keycenter"]);
        }
        if (o.contains("filter") && o["filter"].is_number()) x.join = (int)std::max(0.0, std::floor(o["filter"].get<double>()));   // resolved with the filters
        else x.filtered = o.value("filter", true);
        x.sync = o.value("sync", false);
        if (o.contains("phase")) { const double ph = num(o, "phase", 0, w); x.phase = ph - std::floor(ph); }
        if (o.contains("lowcut") || o.contains("highcut")) {
            if (x.wave != Osc::Noise) warnings.push_back("synth: " + w + "lowcut and highcut only apply to \"wave\": \"noise\"; ignored");
            else {
                x.lowcut = std::clamp(num(o, "lowcut", 0, w), 0.0, 20000.0);
                x.highcut = std::clamp(num(o, "highcut", 0, w), 0.0, 20000.0);
            }
        }
        if (x.sync && i == 0) { warnings.push_back("synth: osc[0].sync: the first oscillator is the one others sync to; ignored"); x.sync = false; }
        if (x.wave == Osc::Additive) {
            x.add = parseAdditive(o, w, job.sampleRate, x.pw, warnings);
            if (x.sync && !x.add->tables) { warnings.push_back("synth: " + w + "sync: an inharmonic or shifted additive oscillator can't restart; ignored"); x.sync = false; }
        }
        if (o.contains("fm")) {
            const auto &f = o["fm"];
            if (!f.is_object()) throw std::runtime_error("synth: " + w + "fm is {\"ratio\": 2, \"index\": 1.5, \"decay\": 0.4}");
            checkKeys(f, {"ratio", "index", "decay", "sustain"}, w + "fm.", warnings);
            if (x.wave != Osc::Sine) warnings.push_back("synth: " + w + "fm only works on a sine oscillator; ignored");
            else {
                x.fm = true;
                x.fmRatio = std::clamp(num(f, "ratio", 1, w + "fm."), 0.01, 32.0);
                x.fmIndex = std::clamp(num(f, "index", 1, w + "fm."), 0.0, 20.0);
                x.fmDecay = std::max(0.0, num(f, "decay", 0, w + "fm."));
                x.fmSustain = std::clamp(num(f, "sustain", 0, w + "fm."), 0.0, 1.0);
            }
        }
        P.osc.push_back(x);
    }
    P.p[P_SUB] = std::clamp(num(j, "sub", 0, ""), 0.0, 1.0);
    P.p[P_NOISE] = std::clamp(num(j, "noise", 0, ""), 0.0, 1.0);
    P.p[P_LEVEL] = std::clamp(num(j, "level", 0, ""), -40.0, 12.0);
    P.p[P_GLIDE] = std::clamp(num(j, "glide", 0, ""), 0.0, 2.0);
    P.mono = j.value("mono", false);
    P.legato = j.value("legato", true);
    if (j.contains("unison")) {
        const auto &u = j["unison"];
        if (u.is_number()) P.unison = u.get<int>();
        else if (u.is_object()) {
            checkKeys(u, {"voices", "detune", "spread"}, "unison.", warnings);
            P.unison = u.value("voices", 1);
            P.p[P_DETUNE] = std::clamp(num(u, "detune", 20, "unison."), 0.0, 100.0);
            P.p[P_SPREAD] = std::clamp(num(u, "spread", 0.5, "unison."), 0.0, 1.0);
        } else throw std::runtime_error("synth: 'unison' is a voice count or {\"voices\": 7, \"detune\": 30, \"spread\": 0.8}");
        if (u.is_number()) P.p[P_DETUNE] = 20;
        P.unison = std::clamp(P.unison, 1, 16);
    }
    if (j.contains("filter")) {   // one filter, or a list of them in series (the first one's values are the parameters)
        const json &fj = j["filter"];
        const json list = fj.is_array() ? fj : json::array({fj});
        if (list.empty() || list.size() > 4) throw std::runtime_error("synth: 'filter' is an object or a list of 1 to 4 of them, played in series");
        for (size_t i = 0; i < list.size(); ++i) {
            const std::string w = fj.is_array() ? "filter[" + std::to_string(i) + "]." : std::string("filter.");
            const Stage s = parseStage(list[i], w, i == 0, warnings);
            if (i) { P.more.push_back(s); continue; }
            P.first = s;
            P.ftype = s.type;
            P.slope = s.slope;
            P.p[P_CUTOFF] = s.cutoff;
            P.p[P_RES] = s.res;
            P.p[P_KEYTRACK] = s.keytrack;
            P.p[P_ENV] = s.env;
            P.p[P_DRIVE] = std::clamp(num(list[i], "drive", 0, w), 0.0, 1.0);
            P.velToCutoff = s.velocity;
        }
    }
    for (auto &o : P.osc) {   // an oscillator joining at filter n: past the last one it skips them all; inside a parallel group, at its start
        if (o.join > (int)P.more.size()) o.filtered = false, o.join = 0;
        while (o.join > 0 && P.more[(size_t)o.join - 1].parallel) --o.join;
    }
    P.amp = parseAdsr(j.value("amp", json()), Adsr{}, "amp", warnings);
    if (j.contains("amp") && j["amp"].is_object()) {
        P.ampVel = std::clamp(num(j["amp"], "velocity", 0.6, "amp."), 0.0, 1.0);
        P.ampKey = std::clamp(num(j["amp"], "keytrack", 0, "amp."), -2.0, 2.0);
    }
    P.fenv = parseAdsr(j.value("filterEnv", json()), P.fenv, "filterEnv", warnings);
    if (j.contains("pitchEnv")) {
        const auto &pe = j["pitchEnv"];
        checkKeys(pe, {"amount", "decay"}, "pitchEnv.", warnings);
        P.pitchEnvAmt = std::clamp(num(pe, "amount", 0, "pitchEnv."), -48.0, 48.0);
        P.pitchEnvDecay = std::max(0.001, num(pe, "decay", 0.05, "pitchEnv."));
    }
    if (j.contains("lfo")) {
        const json list = j["lfo"].is_array() ? j["lfo"] : json::array({j["lfo"]});
        if (list.size() > 4) throw std::runtime_error("synth: at most 4 LFOs");
        for (const auto &l : list) {
            if (!l.is_object() || !l.contains("rate") || !l.contains("depth"))
                throw std::runtime_error("synth: an LFO is {\"rate\": 5 or \"1/8\", \"depth\": 0.2, \"to\": \"pitch\"}");
            checkKeys(l, {"rate", "depth", "shape", "phase", "to", "filter", "delay", "fade", "beats", "wheel", "pressure"}, "lfo.", warnings);
            SynthLfo s;
            s.lfo = Lfo::parse(l, job.tempo);
            const std::string to = l.value("to", "pitch");
            if (to == "pitch") s.to = SynthLfo::Pitch;
            else if (to == "cutoff") s.to = SynthLfo::Cutoff;
            else if (to == "amp" || to == "level") s.to = SynthLfo::Amp;
            else if (to == "pw") s.to = SynthLfo::Pw;
            else if (to == "pan") s.to = SynthLfo::Pan;
            else throw std::runtime_error("synth: lfo.to '" + to + "' must be pitch (semitones), cutoff (octaves), amp (0-1), pw or pan");
            s.delay = std::max(0.0, num(l, "delay", 0, "lfo."));
            s.fade = std::max(0.0, num(l, "fade", 0, "lfo."));
            if (l.contains("wheel")) s.wheel = num(l, "wheel", 0, "lfo.");
            if (l.contains("pressure")) s.pressure = num(l, "pressure", 0, "lfo.");
            if (l.contains("filter")) {
                const double k = num(l, "filter", 0, "lfo.");
                if (s.to != SynthLfo::Cutoff) warnings.push_back("synth: lfo.filter only applies to \"to\": \"cutoff\"; ignored");
                else if (k < 0 || k > (double)P.more.size() || k != std::floor(k))
                    warnings.push_back("synth: lfo.filter names no filter (0 is the first, " + std::to_string(P.more.size()) + " the last); the first moves");
                else s.filter = (size_t)k;
            }
            P.lfos.push_back(s);
        }
    }
    return P;
}

// ---- DSP ---------------------------------------------------------------------------------------
inline double blep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

// Zero-delay-feedback state-variable filter (Simper): stable under fast modulation.
struct Svf {
    double ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, k = 1.414;
    void set(double g, double kk) {
        k = kk;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // returns low; band and high through the references
    inline double run(double v0, double &band, double &high) {
        const double v3 = v0 - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        band = v1;
        high = v0 - k * v1 - v2;
        return v2;
    }
};

// One filter of a voice: its state, and its coefficients for the current block
struct StageState {
    Svf l1, r1, l2, r2;
    std::vector<float> bl, br;   // comb: the delay lines (a power of two long)
    size_t w = 0;
    double lpl = 0, lpr = 0;     // comb: the damping inside its loop
    double phase = 1, yl = 0, yr = 0, hl = 0, hr = 0;   // ring, fm, downsample: the carrier's or the hold's phase; fm: last outputs; downsample: held values
    double delay = 2, fb = 0, a = 1, norm = 1, inc = 0, boost = 0;
    void start(const Stage &s, double sr) {
        if (s.type != Stage::Comb) return;
        size_t n = 16;
        while (n < (size_t)(sr / 20) + 8) n *= 2;   // the longest delay: one cycle of 20 Hz
        bl.assign(n, 0.0f);
        br.assign(n, 0.0f);
    }
    // the block's coefficients at cutoff fc (Hz) and resonance res
    void tune(const Stage &s, Stage::Type type, int slope, double fc, double res, double sr) {
        const double g = std::tan(dsp::kPi * fc / sr);
        switch (type) {
        case Stage::Lowpass: case Stage::Highpass: case Stage::Bandpass: {
            const double kRes = std::pow(0.05 / 1.414, res);   // 1 at res 0, down to Q ~20
            if (slope == 24) {
                l1.set(g, 1.848); r1.set(g, 1.848);
                l2.set(g, 0.765 * kRes); r2.set(g, 0.765 * kRes);
            } else { l1.set(g, 1.414 * kRes); r1.set(g, 1.414 * kRes); }
            break;
        }
        case Stage::Peak: {   // a bell: the band boosted (or cut) by gain at constant Q
            const double A = std::pow(10.0, s.gain / 40), k = 1 / (s.q * A);
            l1.set(g, k); r1.set(g, k);
            boost = k * (A * A - 1);
            break;
        }
        case Stage::Notch: case Stage::Formant: l1.set(g, 1 / s.q); r1.set(g, 1 / s.q); break;
        case Stage::Comb: {   // a delay of one cycle of fc, res of it fed back; damp: a one-pole low-pass in the loop, whose own
                              // delay at fc comes off the line's so the peaks stay in tune
            fb = (s.negative ? -1 : 1) * std::min(res, 0.99);
            a = s.damp > 0 ? 1 - std::exp(-TAU * 20000 * std::pow(2.0, -7 * s.damp) / sr) : 1.0;
            const double wc = TAU * fc / sr, lag = std::atan2((1 - a) * std::sin(wc), 1 - (1 - a) * std::cos(wc)) / wc;
            delay = std::max(2.0, sr / fc - lag);
            norm = std::sqrt(1 - fb * fb);   // white noise passes at its level
            break;
        }
        case Stage::Ring: case Stage::Fm: inc = fc / sr; break;
        case Stage::Downsample: inc = std::min(1.0, 2 * fc / sr); break;   // held at twice the cutoff: the cutoff is its Nyquist
        case Stage::Off: break;
        }
    }
    inline double comb(std::vector<float> &b, double &lp, double x) {
        const size_t mask = b.size() - 1;
        double rp = (double)w - delay;
        if (rp < 0) rp += (double)b.size();
        const size_t i0 = (size_t)rp;
        const double fr = rp - (double)i0, d = b[i0 & mask] + fr * (b[(i0 + 1) & mask] - b[i0 & mask]);
        lp += a * (d - lp);
        const double y = x + fb * lp;
        b[w] = (float)y;
        return y * norm;
    }
    // one sample of both channels through it
    inline void run(const Stage &s, Stage::Type type, int slope, double &L, double &R) {
        const double xL = L, xR = R;
        double bL, hL, bR, hR;
        switch (type) {
        case Stage::Lowpass: case Stage::Highpass: case Stage::Bandpass: {
            const double lL = l1.run(L, bL, hL), lR = r1.run(R, bR, hR);
            double oL = type == Stage::Lowpass ? lL : type == Stage::Highpass ? hL : bL * l1.k;
            double oR = type == Stage::Lowpass ? lR : type == Stage::Highpass ? hR : bR * r1.k;
            if (slope == 24) {   // a second, resonant stage of the same kind
                const double l2L = l2.run(oL, bL, hL), l2R = r2.run(oR, bR, hR);
                oL = type == Stage::Lowpass ? l2L : type == Stage::Highpass ? hL : bL * l2.k;
                oR = type == Stage::Lowpass ? l2R : type == Stage::Highpass ? hR : bR * r2.k;
            }
            L = oL; R = oR;
            break;
        }
        case Stage::Peak: l1.run(L, bL, hL); r1.run(R, bR, hR); L += boost * bL; R += boost * bR; break;
        case Stage::Notch: l1.run(L, bL, hL); r1.run(R, bR, hR); L -= l1.k * bL; R -= r1.k * bR; break;
        case Stage::Formant: l1.run(L, bL, hL); r1.run(R, bR, hR); L = bL; R = bR; break;   // a resonance: q times the level at the centre
        case Stage::Comb:
            L = comb(bl, lpl, L);
            R = comb(br, lpr, R);
            w = (w + 1) & (bl.size() - 1);
            break;
        case Stage::Ring: {   // the voice times a sine (squared towards a square by weird) plus offset
            double c = std::sin(TAU * phase);
            if (s.weird > 0) c += s.weird * (std::tanh(4 * c) / std::tanh(4.0) - c);
            L *= c + s.offset;
            R *= c + s.offset;
            phase += inc;
            phase -= std::floor(phase);
            break;
        }
        case Stage::Fm:   // a sine at the cutoff, its phase moved by the voice (depth) and by itself (feedback)
            yl = std::sin(TAU * phase + s.depth * xL + s.feedback * yl);
            yr = std::sin(TAU * phase + s.depth * xR + s.feedback * yr);
            L = 0.8 * yl;   // about one oscillator's level
            R = 0.8 * yr;
            phase += inc;
            phase -= std::floor(phase);
            break;
        case Stage::Downsample:
            phase += inc;
            if (phase >= 1) { phase -= std::floor(phase); hl = L; hr = R; }
            L = hl; R = hr;
            break;
        case Stage::Off: break;
        }
    }
};

struct Env {
    int stage = 4;   // 0 attack, 1 decay, 2 sustain, 3 release, 4 off
    double level = 0, a = 0, d = 0, s = 0, r = 0, dCoef = 0, rCoef = 0;
    void setup(const Adsr &x, double sr) {
        a = x.a; s = x.s;
        dCoef = x.d > 0 ? std::exp(-4.6 / (x.d * sr)) : 0.0;   // ~99% of the way in `decay` seconds
        rCoef = x.r > 0 ? std::exp(-4.6 / (x.r * sr)) : 0.0;
        d = x.d; r = x.r;
    }
    void gate(bool on) {
        if (on) stage = 0;   // restarts from the current level (no click on retrigger)
        else if (stage < 3) stage = 3;
    }
    inline double next(double sr) {
        switch (stage) {
        case 0:
            level += a > 0 ? 1.0 / (a * sr) : 1.0;
            if (level >= 1) { level = 1; stage = 1; }
            break;
        case 1:
            level = s + (level - s) * dCoef;
            if (std::fabs(level - s) < 1e-5) { level = s; stage = s > 0 ? 2 : 4; }   // decayed to nothing: done
            break;
        case 2: level = s; break;
        case 3:
            level *= rCoef;
            if (level < 1e-5) { level = 0; stage = 4; }
            break;
        default: level = 0;
        }
        return level;
    }
};

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

struct Unit {                       // one oscillator copy (an osc x a unison voice)
    double phase = 0, fmPhase = 0, inc = 0;
    double detune = 0;   // -0.5..0.5 of the unison detune
    double pan = 0;      // -1..1 at full spread
    int table = 0;       // an additive oscillator's table for its pitch now
    size_t sines = 0;    // an additive sine bank: where its partials' phasors start in the voice's list,
    size_t sinesOn = 0;  // how many of them sound (the rest are above 0.45 x the sample rate)
    double sinesInc = -1;   // and the pitch they were set for
    dsp::Biquad noiseHp, noiseLp;   // a noise oscillator's band
};

struct Phasor { double c, s, rc, rs, g; };   // a sine partial: cos and sin of its phase, its step, its level now

struct Voice {
    const Patch *P = nullptr;
    double sr = 48000;
    double key = 60, target = 60, vel = 1;   // key: current pitch (glides to target)
    double noteStart = 0;                     // seconds, song time of the (last) attack
    const std::vector<std::pair<double, double>> *bend = nullptr;
    size_t bendStart = 0;                     // frame the bend points are measured from
    std::vector<Unit> units;                  // osc-major: units[o * unison + u]
    Env amp, fenv;
    StageState f0;                            // the first filter
    std::vector<StageState> fx;               // and the ones after it
    dsp::Noise noise;
    double subPhase = 0;
    bool done = false;
    std::vector<double> oscDecay;             // per osc own-decay coefficient
    std::vector<double> oscDecayLevel;
    std::vector<double> fmLevel;              // per osc FM index envelope (1 at the attack)
    std::vector<double> fmCoef;
    std::vector<double> oscKey;               // per osc level from its keytrack at the target key
    std::vector<Phasor> sines;                // additive sine banks' partials, per unit

    void start(const Patch &patch, double sampleRate, uint32_t seed) {
        P = &patch; sr = sampleRate;
        noise = dsp::Noise(seed | 1);
        const int U = P->unison;
        units.assign(P->osc.size() * (size_t)U, Unit{});
        sines.clear();
        // unison voices: detune spread evenly from lowest to highest; the centre voice sits in the middle,
        // the others alternate sides as they move out, so neither side is all flat or all sharp
        const double c = (U - 1) / 2.0;
        for (size_t o = 0; o < P->osc.size(); ++o)
            for (int u = 0; u < U; ++u) {
                Unit &x = units[o * (size_t)U + (size_t)u];
                const double dist = u - c;
                x.detune = U > 1 ? dist / (2 * c) : 0.0;   // -0.5..0.5, times the detune setting
                double pan = 0;
                if (U > 1 && std::fabs(dist) > 1e-9) {
                    const int ring = (int)std::lround(std::fabs(dist) + (U % 2 == 0 ? 0.5 : 0.0));
                    const double side = ((dist < 0) == (ring % 2 == 1)) ? -1.0 : 1.0;
                    pan = side * (0.3 + 0.7 * std::fabs(dist) / c);
                }
                x.pan = pan;
                x.phase = P->osc[o].phase >= 0 ? P->osc[o].phase : U > 1 ? (hash32(seed * 131 + (uint32_t)(o * 17 + (size_t)u)) & 0xffffff) / 16777216.0 : 0.0;
                x.fmPhase = 0;
                if (P->osc[o].lowcut > 0) x.noiseHp.set(dsp::Biquad::HighPass, P->osc[o].lowcut, 0.7071, 0, sr);
                if (P->osc[o].highcut > 0) x.noiseLp.set(dsp::Biquad::LowPass, P->osc[o].highcut, 0.7071, 0, sr);
                const AdditiveSet *a = P->osc[o].add.get();
                if (a && !a->tables) {   // a sine bank: each partial starts where the unit's phase puts it
                    x.sines = sines.size();
                    for (auto &p : a->parts) {
                        const double ph = p.phase + TAU * p.ratio * x.phase;
                        sines.push_back({std::cos(ph), std::sin(ph), 1, 0, 0});
                    }
                }
            }
        oscDecay.assign(P->osc.size(), 1.0);
        oscDecayLevel.assign(P->osc.size(), 1.0);
        fmLevel.assign(P->osc.size(), 1.0);
        fmCoef.assign(P->osc.size(), 1.0);
        oscKey.assign(P->osc.size(), 1.0);
        for (size_t o = 0; o < P->osc.size(); ++o) {
            if (P->osc[o].decay > 0) oscDecay[o] = std::exp(-4.6 / (P->osc[o].decay * sr));
            if (P->osc[o].fm && P->osc[o].fmDecay > 0) fmCoef[o] = std::exp(-4.6 / (P->osc[o].fmDecay * sr));
        }
        f0 = StageState{};
        f0.start(P->first, sr);
        fx.assign(P->more.size(), StageState{});
        for (size_t k = 0; k < fx.size(); ++k) fx[k].start(P->more[k], sr);
        amp.setup(P->amp, sr);
        fenv.setup(P->fenv, sr);
    }
    void attack(double k, double v, double t0, bool retrigger) {
        target = k;
        vel = v;
        for (size_t o = 0; o < oscKey.size(); ++o)
            if (P->osc[o].keyDb != 0) oscKey[o] = std::pow(10.0, std::min(0.0, P->osc[o].keyDb * (k - P->osc[o].keyCenter) / 240.0));
        if (retrigger) {
            noteStart = t0;
            if (P->ampKey != 0) {
                Adsr a = P->amp;
                const double scale = std::pow(2.0, -P->ampKey * (k - 60) / 12.0);
                a.d *= scale;
                a.r *= scale;
                amp.setup(a, sr);
            }
            amp.gate(true);
            fenv.gate(true);
            for (size_t o = 0; o < oscDecayLevel.size(); ++o) { oscDecayLevel[o] = 1; fmLevel[o] = 1; }
        }
    }
};

inline double oscSample(const Osc &o, const Unit &x, double pw, double fmAmt) {
    const double t = x.phase, dt = x.inc;
    switch (o.wave) {
    case Osc::Saw: return 2 * t - 1 - blep(t, dt);
    case Osc::Square: {
        double v = (t < pw ? 1.0 : -1.0) + blep(t, dt);
        double t2 = t + 1 - pw;
        t2 -= std::floor(t2);
        v -= blep(t2, dt);
        return v - (2 * pw - 1);   // centred for any width
    }
    case Osc::Triangle: return 1 - 4 * std::fabs(t - 0.5);
    case Osc::Sine:
        if (o.fm) return std::sin(TAU * t + fmAmt * std::sin(TAU * x.fmPhase));
        return std::sin(TAU * t);
    case Osc::Noise: case Osc::Additive: return 0;   // handled by the voice
    }
    return 0;
}

} // namespace

std::vector<SynthPatchInfo> synthPatches() {
    std::vector<SynthPatchInfo> out;
    for (auto &[name, p] : patchBank().items()) out.push_back({name, categoryOf(name), p.value("about", "")});
    std::stable_sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.name == "Init" ? b.name != "Init" : (b.name != "Init" && a.name < b.name); });
    for (auto &[name, what] : garageBandSynthPatches())   // GarageBand's synth patches, re-created here
        out.push_back({name, "GarageBand", "GarageBand's " + what + " patch, re-created on builtin:synth: an approximation"});
    return out;
}

const std::vector<SynthParamInfo> &synthParams() { return kParams; }


bool isSynthExpParam(const std::string &name) {
    const int i = paramIndex(name);
    return i >= 0 && kParams[(size_t)i].exp;
}

bool isBuiltinSynthPatch(const std::string &name) { return patchBank().contains(name); }

namespace fs = std::filesystem;

namespace {
std::string lowerAscii(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }
thread_local bool tLevelProbe = false;   // rendering a level probe: no level fix inside it
// synthParamValues: renderSynth stops once the patch's parameters are known, before and after the track's "params"
thread_local std::pair<std::vector<double>, std::vector<double>> *tParamProbe = nullptr;

// GarageBand's factory patches are roughly level-matched, but a re-creation can land far from them while its filter
// and resonance scales are guesses (a self-oscillating resonance, a filter closed until modulation that isn't
// mapped). A patch whose probe (C3 held, then a C minor chord, effects included) measures outside -33..-17 LUFS,
// 8 dB either side of the re-creations' typical -25, is moved to the nearer edge (by at most +24 dB). Retro Synth's
// scales are measured (a bounce of eight of its patches spread over 12 dB, as GarageBand plays them): only a probe more
// than 12 dB from its patches' typical -20.5 LUFS moves. The dB to add after its effects; cached per patch.
double garageBandLevelFix(const std::string &name, bool retro) {
    static std::mutex m;
    static std::map<std::string, double> cache;
    {
        std::lock_guard<std::mutex> lock(m);
        auto it = cache.find(name);
        if (it != cache.end()) return it->second;
    }
    Job job;
    Track t;
    t.name = "level probe";
    t.plugin = "builtin:synth";
    t.preset = name;
    for (auto [start, key] : std::initializer_list<std::pair<double, int>>{{0, 48}, {2, 60}, {2, 63}, {2, 67}})
        t.notes.push_back({start, 1.5, key, 0, 0.8, {}, {}});
    Audio a;
    a.resize((size_t)(4.5 * job.sampleRate));
    std::vector<std::string> w;
    std::string e;
    double fix = 0;
    tLevelProbe = true;
    const bool ok = renderSynth(job, t, a, w, e);
    tLevelProbe = false;
    const double lufs = ok ? integratedLufs(a, job.sampleRate) : 0;
    const double lo = retro ? -32.5 : -33, hi = retro ? -8.5 : -17;
    if (ok && std::isfinite(lufs) && lufs > -120) fix = lufs > hi ? hi - lufs : lufs < lo ? std::min(24.0, lo - lufs) : 0;
    fix = std::round(fix * 10) / 10;
    std::lock_guard<std::mutex> lock(m);
    cache[name] = fix;
    return fix;
}
} // namespace

bool synthParamValues(const Job &job, const Track &track, std::vector<double> &patch, std::vector<double> &current, std::string &err) {
    std::pair<std::vector<double>, std::vector<double>> probe;
    tParamProbe = &probe;
    Audio none;
    std::vector<std::string> warnings;
    const bool ok = renderSynth(job, track, none, warnings, err);
    tParamProbe = nullptr;
    if (ok && probe.second.size() != kParams.size()) { err = "track '" + track.name + "': the patch did not load"; return false; }
    patch = probe.first;
    current = probe.second;
    return ok;
}

bool renderSynth(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err) {
    const double sr = job.sampleRate;
    Patch P;
    std::vector<Envelope> autos((size_t)P_COUNT);   // parameter curves over song time
    std::vector<bool> automated((size_t)P_COUNT, false);
    bool pwParam = false;   // "pw" set by params or automation: it replaces every pulse oscillator's own width
    double patchTranspose = 0;          // a GarageBand patch's own transposition (semitones)
    double levelFix = 0;                // dB after its effects that brings a re-created patch near the others' level
    json patchFx = json::array();       // and its effects, after the voices ("synth": {"effects": false} leaves them out)
    try {
        // the named patch, then the track's "synth" object merged over it
        json patch = json::object();
        const std::string preset = track.preset.empty() ? "Init" : track.preset;
        const json &bank = patchBank();
        std::string found;
        for (auto &[n, p] : bank.items()) if (n == preset) found = n;
        GarageBandSynth gb;
        std::string refused;   // a GarageBand patch on an instrument re-created here that can't play on it, and why
        std::string gbKey = preset;   // its name, or a patch folder's path (from the job's folder: an imported project's track)
        if (found.empty() && (gbKey.find('/') != std::string::npos || (gbKey.size() > 6 && lowerAscii(gbKey.substr(gbKey.size() - 6)) == ".patch"))) {
            const fs::path pp = fs::u8path(gbKey);
            if (pp.is_relative() && !job.baseDir.empty()) gbKey = (fs::u8path(job.baseDir) / pp).lexically_normal().u8string();
        }
        if (found.empty() && garageBandSynthPatch(gbKey, gb, &refused)) {   // a GarageBand synth patch, re-created here
            found = "Init";
            patchTranspose = gb.transpose;
            patchFx = gb.fx;
            if (!tLevelProbe) levelFix = garageBandLevelFix(gbKey, gb.instrument == "Retro Synth");
            char fixText[96] = "";
            if (levelFix != 0) std::snprintf(fixText, sizeof fixText, "; its level moved %+.1f dB toward its peers' (scales not yet calibrated)", levelFix);
            warnings.push_back("preset '" + gb.name + "' is GarageBand's " + garageBandSynthKind(gb) + " patch, re-created on builtin:synth: an approximation" + fixText);
        }
        if (found.empty() && !refused.empty()) {
            err = "track '" + track.name + "': GarageBand's patch '" + preset + "' doesn't play on builtin:synth: " + refused;
            return false;
        }
        if (found.empty()) {
            std::string lower = preset;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            std::vector<std::string> hits;
            for (auto &[n, p] : bank.items()) {
                std::string ln = n;
                std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                if (ln == lower) { hits = {n}; break; }
                if (ln.find(lower) != std::string::npos) hits.push_back(n);
            }
            if (hits.size() != 1) {
                std::string names;
                for (auto &[n, p] : bank.items()) names += (names.empty() ? "" : ", ") + n;
                err = "track '" + track.name + "': builtin:synth has no patch '" + preset + "'" + (hits.size() > 1 ? " (several match)" : "") +
                      "; patches: " + names;
                return false;
            }
            found = hits[0];
        }
        patch = bank[found];
        if (!gb.name.empty()) patch.merge_patch(gb.synth);
        if (!track.synth.is_null()) {
            if (!track.synth.is_object()) { err = "track '" + track.name + "': \"synth\" must be an object (see docs/job-format.md)"; return false; }
            json own = track.synth;
            if (own.contains("effects")) { if (!own["effects"].get<bool>()) patchFx = json::array(); own.erase("effects"); }
            patch.merge_patch(own);
        }
        P = parsePatch(patch, job, warnings);
        for (size_t o = 0; o < P.osc.size(); ++o) {   // a sine bank plays at most 512 partials across its unison voices
            const auto &a = P.osc[o].add;
            const size_t keep = std::max<size_t>(1, 512 / (size_t)P.unison);
            if (!a || a->tables || a->parts.size() <= keep) continue;
            auto t = std::make_shared<AdditiveSet>(*a);
            std::vector<size_t> idx(t->parts.size());
            for (size_t k = 0; k < idx.size(); ++k) idx[k] = k;
            std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return a->parts[x].amp > a->parts[y].amp; });
            idx.resize(keep);
            std::sort(idx.begin(), idx.end());
            t->parts.clear();
            for (size_t k : idx) t->parts.push_back(a->parts[k]);
            P.osc[o].add = t;
            warnings.push_back("synth: osc[" + std::to_string(o) + "]: " + std::to_string(a->parts.size()) + " inharmonic partials x " + std::to_string(P.unison) +
                               " unison voices: the loudest " + std::to_string(keep) + " play");
        }
        if (tParamProbe) tParamProbe->first.assign(P.p, P.p + P_COUNT);
        // "params": overrides by name
        for (const auto &ps : track.params) {
            const int i = paramIndex(ps.key);
            if (i < 0) { err = "track '" + track.name + "': builtin:synth has no parameter '" + ps.key + "' (it has: " + paramNames() + ")"; return false; }
            double v = ps.value;
            if (!ps.text.empty() && !Envelope::builtinText(ps.text, v)) {
                err = "track '" + track.name + "': parameter '" + ps.key + "' can't read '" + ps.text + "' (a number, a note name, or text like \"800 Hz\")";
                return false;
            }
            P.p[i] = std::clamp(v, kParams[(size_t)i].min, kParams[(size_t)i].max);
            if (i == P_PW) pwParam = true;
        }
        if (tParamProbe) { tParamProbe->second.assign(P.p, P.p + P_COUNT); return true; }
        for (const auto &[name, env] : track.paramAutomation) {
            const int i = paramIndex(name);
            if (i < 0) { err = "track '" + track.name + "': automation of unknown builtin:synth parameter '" + name + "' (it has: " + paramNames() + ")"; return false; }
            Envelope e = env;
            std::string bad;
            if (e.needsText() && !e.resolveText(Envelope::builtinText, bad)) {
                err = "track '" + track.name + "': the '" + name + "' curve can't read '" + bad + "' (a number, a note name, or text like \"800 Hz\")";
                return false;
            }
            autos[(size_t)i] = e;
            automated[(size_t)i] = true;
            if (i == P_PW) pwParam = true;
        }
    } catch (const std::exception &e) {
        err = "track '" + track.name + "': " + e.what();
        return false;
    }
    if (P.osc.empty() && P.p[P_NOISE] <= 0 && P.p[P_SUB] <= 0 && !automated[P_NOISE] && !automated[P_SUB])
        warnings.push_back("synth: the patch has no oscillators, sub or noise: it makes no sound");
    // the mod wheel and channel pressure move the LFO depths that name them; other controllers aren't played
    const Envelope *wheelEnv = nullptr, *pressureEnv = track.pressureAutomation.empty() ? nullptr : &track.pressureAutomation;
    std::string ignored;
    bool wheelUsed = false, pressureUsed = false;
    for (const auto &l : P.lfos) { wheelUsed |= !std::isnan(l.wheel); pressureUsed |= !std::isnan(l.pressure); }
    for (const auto &[num, env] : track.ccAutomation) {
        if (num == 1 && wheelUsed) wheelEnv = &env;
        else ignored += (ignored.empty() ? "" : ", ") + std::string("CC ") + std::to_string(num);
    }
    if (pressureEnv && !pressureUsed) { ignored += (ignored.empty() ? "" : ", ") + std::string("pressure"); pressureEnv = nullptr; }
    if (!ignored.empty())
        warnings.push_back("builtin:synth: " + ignored + " not played (the mod wheel and pressure move LFO depths that name them, \"wheel\" and \"pressure\"; "
                           "automate other parameters by name, automation.params)");
    auto param = [&](int i, double t) {
        return automated[(size_t)i] ? std::clamp(autos[(size_t)i].at(t), kParams[(size_t)i].min, kParams[(size_t)i].max) : P.p[i];
    };

    // notes in time order
    std::vector<size_t> order(track.notes.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return track.notes[a].start < track.notes[b].start; });
    const size_t frames = out.frames();
    const double nyq = 0.45 * sr;
    const int U = P.unison;
    // gain: equal loudness for any unison count and oscillator mix
    double oscPower = 0;
    for (auto &o : P.osc) oscPower += o.level * o.level;
    const double unitNorm = 1.0 / std::sqrt(std::max(1.0, oscPower) * U);
    const double baseGain = 0.3;

    // one voice per note (poly), or one voice for the whole track (mono): events drive it
    struct Event { size_t frame; size_t note; bool on; };
    std::vector<Event> events;
    for (size_t i : order) {
        const auto &n = track.notes[i];
        const size_t on = (size_t)std::llround(n.start * sr), off = (size_t)std::llround((n.start + n.length) * sr);
        if (on >= frames) continue;
        events.push_back({on, i, true});
        events.push_back({std::max(off, on + 1), i, false});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.frame != b.frame ? a.frame < b.frame : (!a.on && b.on); });

    auto runVoice = [&](Voice &v, size_t from, size_t to) {
        // renders frames [from, to) of one voice into `out`
        const size_t block = 16;
        double pwNow = 0.5, fmMul = 1, lfoMul = 1, cutoffBase = 8000, res = 0, drive = 0, envAmt = 0, keytrack = 0, sub = 0, nz = 0, level = 1,
               spread = 0.5, detune = 0, glideCoef = 0, subInc = 0;
        double lfoPitch = 0, lfoCut = 0, lfoAmp = 0, lfoPw = 0, lfoPan = 0;
        std::vector<double> lfoMore(P.more.size(), 0.0);   // LFOs on the cutoffs of the filters after the first
        double panL = 1, panR = 1;
        std::vector<double> uL(v.units.size(), 1.0), uR(v.units.size(), 1.0);
        double driveGain = 1, driveNorm = 1;
        for (size_t f = from; f < to && f < frames; ++f) {
            const double t = f / sr;
            if ((f - from) % block == 0) {
                cutoffBase = param(P_CUTOFF, t); res = param(P_RES, t); drive = param(P_DRIVE, t); envAmt = param(P_ENV, t);
                keytrack = param(P_KEYTRACK, t); detune = param(P_DETUNE, t); spread = param(P_SPREAD, t); pwNow = param(P_PW, t);
                fmMul = param(P_FM, t); lfoMul = param(P_LFO, t); sub = param(P_SUB, t); nz = param(P_NOISE, t);
                level = dsp::dbToLin(param(P_LEVEL, t));
                const double glide = param(P_GLIDE, t);
                glideCoef = P.mono && glide > 0 ? std::exp(-4.6 / (glide * sr / block)) : 0.0;
                lfoPitch = lfoCut = lfoAmp = lfoPw = lfoPan = 0;
                std::fill(lfoMore.begin(), lfoMore.end(), 0.0);
                const double since = t - v.noteStart;
                const double wheelNow = wheelEnv ? std::clamp(wheelEnv->at(t) / 127.0, 0.0, 1.0) : 0.0;
                const double pressureNow = pressureEnv ? std::clamp(pressureEnv->at(t) / 127.0, 0.0, 1.0) : 0.0;
                for (const auto &l : P.lfos) {
                    double dep = l.lfo.depthAt(t);
                    if (!std::isnan(l.wheel)) dep += (l.wheel - l.lfo.depth) * wheelNow;
                    if (!std::isnan(l.pressure)) dep += (l.pressure - l.lfo.depth) * pressureNow;
                    dep *= lfoMul;
                    if (since < l.delay) dep = 0;
                    else if (l.fade > 0 && since < l.delay + l.fade) dep *= (since - l.delay) / l.fade;
                    const double wv = l.lfo.wave(t), w = wv * dep;
                    switch (l.to) {
                    case SynthLfo::Pitch: lfoPitch += w; break;
                    case SynthLfo::Cutoff: (l.filter ? lfoMore[l.filter - 1] : lfoCut) += w; break;
                    case SynthLfo::Amp: lfoAmp += std::fabs(dep) * (0.5 - 0.5 * wv); break;   // depth 1 = down to silence
                    case SynthLfo::Pw: lfoPw += w; break;
                    case SynthLfo::Pan: lfoPan += w; break;
                    }
                }
                // glide towards the target pitch (control rate)
                v.key = glideCoef > 0 ? v.target + (v.key - v.target) * glideCoef : v.target;
                double pitch = v.key + patchTranspose + lfoPitch;
                if (!track.bendAutomation.empty()) pitch += track.bendAutomation.at(t);
                if (v.bend && !v.bend->empty()) {   // per-note bend: (seconds after the note start, semitones)
                    const double s = (double)(f - v.bendStart) / sr;
                    const auto &b = *v.bend;
                    double semis = b.front().second;
                    if (s >= b.back().first) semis = b.back().second;
                    else
                        for (size_t k = 1; k < b.size(); ++k)
                            if (s < b[k].first) {
                                const double u = (s - b[k - 1].first) / std::max(1e-9, b[k].first - b[k - 1].first);
                                semis = b[k - 1].second + (b[k].second - b[k - 1].second) * std::clamp(u, 0.0, 1.0);
                                break;
                            }
                    pitch += semis;
                }
                if (P.pitchEnvAmt != 0) pitch += P.pitchEnvAmt * std::exp(-since / P.pitchEnvDecay);
                subInc = std::min(nyq, 440.0 * std::pow(2.0, (pitch - 12 - 69) / 12.0)) / sr;
                for (size_t o = 0; o < P.osc.size(); ++o)
                    for (int u = 0; u < U; ++u) {
                        Unit &x = v.units[o * (size_t)U + (size_t)u];
                        const double semis = pitch + P.osc[o].pitch + x.detune * detune / 100.0;
                        x.inc = std::min(nyq, 440.0 * std::pow(2.0, (semis - 69) / 12.0)) / sr;
                        if (const AdditiveSet *a = P.osc[o].add.get()) {
                            if (a->tables) x.table = a->pick(x.inc * sr);
                            else {
                                Phasor *ph = v.sines.data() + x.sines;
                                if (x.inc != x.sinesInc) {   // each partial's step and its level under 0.45 x the sample rate
                                    x.sinesInc = x.inc;
                                    x.sinesOn = 0;
                                    for (size_t k = 0; k < a->parts.size(); ++k) {
                                        const double hz = a->parts[k].ratio * x.inc * sr + a->shiftHz, step = TAU * hz / sr;
                                        ph[k].rc = std::cos(step);
                                        ph[k].rs = std::sin(step);
                                        ph[k].g = a->parts[k].amp * std::clamp((0.45 * sr - std::fabs(hz)) / (0.05 * sr), 0.0, 1.0);
                                        if (ph[k].g > 0) x.sinesOn = k + 1;
                                    }
                                }
                                for (size_t k = 0; k < x.sinesOn; ++k) {   // back onto the unit circle (the silent ones wait)
                                    const double m = (3 - ph[k].c * ph[k].c - ph[k].s * ph[k].s) / 2;
                                    ph[k].c *= m;
                                    ph[k].s *= m;
                                }
                            }
                        }
                        const double pan = std::clamp(x.pan * spread, -1.0, 1.0);
                        const double ang = (pan + 1) * dsp::kPi / 4;
                        uL[o * (size_t)U + (size_t)u] = std::cos(ang) * M_SQRT2;
                        uR[o * (size_t)U + (size_t)u] = std::sin(ang) * M_SQRT2;
                    }
                {   // the voice's own pan (pan LFO)
                    const double ang = (std::clamp(lfoPan, -1.0, 1.0) + 1) * dsp::kPi / 4;
                    panL = std::cos(ang) * M_SQRT2; panR = std::sin(ang) * M_SQRT2;
                }
                if (P.ftype != Stage::Off) {
                    const double oct = keytrack * (v.key - 60) / 12.0 + envAmt * v.fenv.level + P.velToCutoff * (v.vel - 1) + lfoCut;
                    const double fc = std::clamp(cutoffBase * std::pow(2.0, oct), 20.0, nyq);
                    v.f0.tune(P.first, P.ftype, P.slope, fc, res, sr);
                }
                for (size_t k = 0; k < P.more.size(); ++k) {   // the filters after the first: their own cutoffs
                    const Stage &s = P.more[k];
                    const double oct = s.keytrack * (v.key - 60) / 12.0 + s.env * v.fenv.level + s.velocity * (v.vel - 1) + lfoMore[k];
                    v.fx[k].tune(s, s.type, s.slope, std::clamp(s.cutoff * std::pow(2.0, oct), 20.0, nyq), s.res, sr);
                }
                driveGain = 1 + 7 * drive;
                driveNorm = 1.0 / std::tanh(driveGain);
            }
            const double a = v.amp.next(sr);
            v.fenv.next(sr);
            if (v.amp.stage == 4) { v.done = true; break; }
            double L = 0, R = 0, postL = 0, postR = 0;   // post: oscillators that skip the filter
            double joinL[4] = {}, joinR[4] = {};          // ones that join the filters at a later one
            for (size_t o = 0; o < P.osc.size(); ++o) {
                const Osc &osc = P.osc[o];
                double fmAmt = 0;
                if (osc.fm) {
                    const double e = osc.fmSustain + (1 - osc.fmSustain) * v.fmLevel[o];
                    fmAmt = osc.fmIndex * fmMul * e;
                    v.fmLevel[o] *= v.fmCoef[o];
                }
                double own = 1;
                if (osc.decay > 0) { own = v.oscDecayLevel[o]; v.oscDecayLevel[o] *= v.oscDecay[o]; }
                const double pw = std::clamp((pwParam ? pwNow : osc.pw) + lfoPw, 0.02, 0.98);
                const double g = osc.level * own * v.oscKey[o] * unitNorm;
                double &sumL = !osc.filtered ? postL : osc.join ? joinL[osc.join] : L, &sumR = !osc.filtered ? postR : osc.join ? joinR[osc.join] : R;
                for (int u = 0; u < U; ++u) {
                    const size_t idx = o * (size_t)U + (size_t)u;
                    Unit &x = v.units[idx];
                    if (osc.add) {   // additive: its table (left and right when panned) or its sine bank
                        const AdditiveSet &a = *osc.add;
                        double sl = 0, sr2 = 0;
                        if (a.tables) {
                            if (!a.mip.empty()) {
                                const auto &tb = a.mip[(size_t)x.table];
                                const double pos = x.phase * (double)tb.n;
                                const size_t j = std::min((size_t)pos, tb.n - 1);
                                const double fr = pos - (double)j;
                                sl = tb.l[j] + fr * (tb.l[j + 1] - tb.l[j]);
                                sr2 = a.stereo ? tb.r[j] + fr * (tb.r[j + 1] - tb.r[j]) : sl;
                            }
                        } else {
                            Phasor *ph = v.sines.data() + x.sines;
                            for (size_t k = 0; k < x.sinesOn; ++k) {
                                Phasor &q = ph[k];
                                const double s = q.s * q.g;
                                sl += s * a.parts[k].gl;
                                sr2 += s * a.parts[k].gr;
                                const double c = q.c * q.rc - q.s * q.rs;
                                q.s = q.c * q.rs + q.s * q.rc;
                                q.c = c;
                            }
                        }
                        x.phase += x.inc;
                        if (osc.sync) {
                            const Unit &m = v.units[(size_t)u];
                            if (m.phase < m.inc && m.inc > 0) x.phase = m.phase * (x.inc / m.inc);
                        }
                        if (x.phase >= 1) x.phase -= std::floor(x.phase);
                        sumL += sl * g * uL[idx];
                        sumR += sr2 * g * uR[idx];
                        continue;
                    }
                    double s;
                    if (osc.wave == Osc::Noise) {
                        s = v.noise.next();
                        if (osc.lowcut > 0) s = x.noiseHp.process(s);
                        if (osc.highcut > 0) s = x.noiseLp.process(s);
                    } else s = oscSample(osc, x, pw, fmAmt);
                    x.phase += x.inc;
                    if (osc.sync) {   // restart with the first oscillator's cycle (its unit of the same unison voice)
                        const Unit &m = v.units[(size_t)u];
                        if (m.phase < m.inc && m.inc > 0) x.phase = m.phase * (x.inc / m.inc);
                    }
                    if (x.phase >= 1) x.phase -= std::floor(x.phase);
                    if (osc.fm) { x.fmPhase += x.inc * osc.fmRatio; x.fmPhase -= std::floor(x.fmPhase); }
                    sumL += s * g * uL[idx];
                    sumR += s * g * uR[idx];
                }
            }
            if (sub > 0) {   // a square an octave below the voice
                double sq = (v.subPhase < 0.5 ? 1.0 : -1.0) + blep(v.subPhase, subInc);
                double t2 = v.subPhase + 0.5;
                t2 -= std::floor(t2);
                sq -= blep(t2, subInc);
                v.subPhase += subInc;
                if (v.subPhase >= 1) v.subPhase -= 1;
                L += sq * sub * 0.7;
                R += sq * sub * 0.7;
            }
            if (nz > 0) {
                const double s = v.noise.next() * nz * 0.5;
                L += s; R += s;
            }
            if (drive > 0) { L = std::tanh(L * driveGain) * driveNorm; R = std::tanh(R * driveGain) * driveNorm; }
            if (P.more.empty() && P.first.mix >= 1) {
                if (P.ftype != Stage::Off) v.f0.run(P.first, P.ftype, P.slope, L, R);
            } else {   // in groups: a filter and those after it marked parallel share one signal, each taking its mix of it
                double inL = L, inR = R, dry = 1;
                L = R = 0;
                for (size_t k = 0; k <= P.more.size(); ++k) {
                    const Stage &s = k ? P.more[k - 1] : P.first;
                    if (k && !s.parallel) {   // a new group: what the last one made goes in
                        dry = std::max(0.0, dry);
                        if (drive > 0 && (joinL[k] != 0 || joinR[k] != 0)) {   // the drive takes what joins here too
                            joinL[k] = std::tanh(joinL[k] * driveGain) * driveNorm;
                            joinR[k] = std::tanh(joinR[k] * driveGain) * driveNorm;
                        }
                        L += dry * inL + joinL[k]; R += dry * inR + joinR[k];
                        inL = L; inR = R; L = R = 0; dry = 1;
                    }
                    double oL = inL, oR = inR;
                    if (k) v.fx[k - 1].run(s, s.type, s.slope, oL, oR);
                    else if (P.ftype != Stage::Off) v.f0.run(s, P.ftype, P.slope, oL, oR);
                    L += s.mix * oL; R += s.mix * oR;
                    dry -= s.mix;
                }
                dry = std::max(0.0, dry);
                L += dry * inL; R += dry * inR;
            }
            L += postL; R += postR;
            const double velGain = 1 - P.ampVel * (1 - v.vel);
            const double g = a * velGain * level * baseGain * (1 - std::clamp(lfoAmp, 0.0, 1.0));
            out.left[f] += (float)(L * g * panL);
            out.right[f] += (float)(R * g * panR);
        }
    };

    if (!P.mono) {
        // poly: every note is its own voice, from its note-on to the end of its release
        for (size_t i : order) {
            const auto &n = track.notes[i];
            const size_t on = (size_t)std::llround(n.start * sr);
            if (on >= frames) continue;
            const size_t off = std::max(on + 1, (size_t)std::llround((n.start + n.length) * sr));
            Voice v;
            v.start(P, sr, hash32((uint32_t)i * 2654435761u + 7));
            v.key = v.target = n.key;
            v.bend = &n.bend;
            v.bendStart = on;
            v.attack(n.key, n.velocity, n.start, true);
            runVoice(v, on, off);
            v.amp.gate(false);
            v.fenv.gate(false);
            if (!v.done) runVoice(v, off, frames);
        }
    } else {
        // mono: one voice; a note that starts while another is held slides (legato) without a new attack
        Voice v;
        v.start(P, sr, 99991);
        size_t pos = 0;
        long held = -1;          // the note sounding now
        bool alive = false;
        for (const auto &e : events) {
            if (alive && e.frame > pos) { runVoice(v, pos, e.frame); if (v.done) alive = false; }
            pos = std::max(pos, e.frame);
            const auto &n = track.notes[e.note];
            if (e.on) {
                const bool slide = alive && held >= 0 && P.legato;
                if (!alive) { v.key = n.key; v.done = false; v.amp.level = 0; v.fenv.level = 0; }
                if (P.p[P_GLIDE] <= 0 && !automated[P_GLIDE]) v.key = n.key;
                v.bend = &n.bend;
                v.bendStart = e.frame;
                v.attack(n.key, n.velocity, n.start, !slide);
                held = (long)e.note;
                alive = true;
            } else if ((long)e.note == held) {
                v.amp.gate(false);
                v.fenv.gate(false);
                held = -1;
            }
        }
        if (alive) runVoice(v, pos, frames);
    }
    if (!patchFx.empty()) {   // a GarageBand patch's chorus or flanger and its own effects
        const FxContext ctx{job, false, nullptr};
        for (size_t i = 0; i < patchFx.size(); ++i) {
            auto fx = makeEffect(patchFx[i], job, "patch effect " + std::to_string(i + 1), err);
            if (!fx) { warnings.push_back(err + ": left out"); err.clear(); continue; }   // e.g. a Space Designer room that isn't installed
            if (!fx->process(out, ctx, err)) return false;
            for (auto &w : fx->warnings) warnings.push_back("patch effect: " + w);
        }
    }
    if (levelFix != 0) {
        const float g = (float)dsp::dbToLin(levelFix);
        for (size_t i = 0; i < out.frames(); ++i) out.left[i] *= g, out.right[i] *= g;
    }
    return true;
}

} // namespace wl
