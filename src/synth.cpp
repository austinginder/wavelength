#include "synth.hpp"

#include "automation.hpp"
#include "dsp.hpp"
#include "effects.hpp"
#include "retro_synth.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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

struct Osc {
    enum Wave { Saw, Square, Triangle, Sine, Noise } wave = Saw;
    double level = 1, pitch = 0, pw = 0.5, decay = 0;   // pitch in semitones; decay: own amplitude decay (s), 0 = none
    bool fm = false;
    double fmRatio = 1, fmIndex = 0, fmDecay = 0, fmSustain = 0;
    bool filtered = true;   // false: joins after the filter (Retro Synth's sine level)
    bool sync = false;      // hard sync: restarts whenever the first oscillator starts a cycle
};

struct SynthLfo {
    Lfo lfo;
    enum Dest { Pitch, Cutoff, Amp, Pw, Pan } to = Pitch;
    double delay = 0, fade = 0;   // seconds after the note starts
};

struct Patch {
    std::vector<Osc> osc;
    double p[P_COUNT] = {};
    int unison = 1;
    enum FType { Off, Lowpass, Highpass, Bandpass } ftype = Lowpass;
    int slope = 12;
    double velToCutoff = 0;       // octaves at velocity 0 (1 = an octave darker when soft)
    Adsr amp, fenv{0, 0.3, 0, 0.3};
    double ampVel = 0.6;
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

Adsr parseAdsr(const json &o, Adsr d, const std::string &where, std::vector<std::string> &warnings) {
    if (o.is_null()) return d;
    if (!o.is_object()) throw std::runtime_error("synth: '" + where + "' must be an object with attack, decay, sustain, release");
    checkKeys(o, {"attack", "decay", "sustain", "release", "velocity"}, where + ".", warnings);
    d.a = std::max(0.0, num(o, "attack", d.a, where + "."));
    d.d = std::max(0.0, num(o, "decay", d.d, where + "."));
    d.s = std::clamp(num(o, "sustain", d.s, where + "."), 0.0, 1.0);
    d.r = std::max(0.0, num(o, "release", d.r, where + "."));
    return d;
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
        checkKeys(o, {"wave", "level", "octave", "semi", "cents", "pw", "decay", "fm", "filter", "sync"}, w, warnings);
        Osc x;
        const std::string wave = o.value("wave", "saw");
        if (wave == "saw") x.wave = Osc::Saw;
        else if (wave == "square" || wave == "pulse") x.wave = Osc::Square;
        else if (wave == "triangle" || wave == "tri") x.wave = Osc::Triangle;
        else if (wave == "sine") x.wave = Osc::Sine;
        else if (wave == "noise") x.wave = Osc::Noise;
        else throw std::runtime_error("synth: " + w + "wave '" + wave + "' must be saw, square (or pulse), triangle, sine or noise");
        x.level = std::max(0.0, num(o, "level", 1, w));
        x.pitch = 12 * num(o, "octave", 0, w) + num(o, "semi", 0, w) + num(o, "cents", 0, w) / 100;
        x.pw = std::clamp(num(o, "pw", 0.5, w), 0.02, 0.98);
        x.decay = std::max(0.0, num(o, "decay", 0, w));
        x.filtered = o.value("filter", true);
        x.sync = o.value("sync", false);
        if (x.sync && i == 0) { warnings.push_back("synth: osc[0].sync: the first oscillator is the one others sync to; ignored"); x.sync = false; }
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
    if (j.contains("filter")) {
        const auto &f = j["filter"];
        if (!f.is_object()) throw std::runtime_error("synth: 'filter' must be an object");
        checkKeys(f, {"type", "slope", "cutoff", "resonance", "keytrack", "env", "velocity", "drive"}, "filter.", warnings);
        const std::string t = f.value("type", "lowpass");
        if (t == "lowpass" || t == "lp") P.ftype = Patch::Lowpass;
        else if (t == "highpass" || t == "hp") P.ftype = Patch::Highpass;
        else if (t == "bandpass" || t == "bp") P.ftype = Patch::Bandpass;
        else if (t == "off" || t == "none") P.ftype = Patch::Off;
        else throw std::runtime_error("synth: filter.type '" + t + "' must be lowpass, highpass, bandpass or off");
        P.slope = f.value("slope", 12);
        if (P.slope != 12 && P.slope != 24) throw std::runtime_error("synth: filter.slope is 12 or 24 (dB per octave)");
        P.p[P_CUTOFF] = std::clamp(num(f, "cutoff", 8000, "filter."), 20.0, 20000.0);
        P.p[P_RES] = std::clamp(num(f, "resonance", 0, "filter."), 0.0, 1.0);
        P.p[P_KEYTRACK] = std::clamp(num(f, "keytrack", 0, "filter."), 0.0, 1.0);
        P.p[P_ENV] = std::clamp(num(f, "env", 0, "filter."), -8.0, 8.0);
        P.p[P_DRIVE] = std::clamp(num(f, "drive", 0, "filter."), 0.0, 1.0);
        P.velToCutoff = std::clamp(num(f, "velocity", 0, "filter."), 0.0, 4.0);
    }
    P.amp = parseAdsr(j.value("amp", json()), Adsr{}, "amp", warnings);
    if (j.contains("amp") && j["amp"].is_object()) P.ampVel = std::clamp(num(j["amp"], "velocity", 0.6, "amp."), 0.0, 1.0);
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
            checkKeys(l, {"rate", "depth", "shape", "phase", "to", "delay", "fade", "beats"}, "lfo.", warnings);
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
};

struct Voice {
    const Patch *P = nullptr;
    double sr = 48000;
    double key = 60, target = 60, vel = 1;   // key: current pitch (glides to target)
    double noteStart = 0;                     // seconds, song time of the (last) attack
    const std::vector<std::pair<double, double>> *bend = nullptr;
    size_t bendStart = 0;                     // frame the bend points are measured from
    std::vector<Unit> units;                  // osc-major: units[o * unison + u]
    Env amp, fenv;
    Svf fL1, fR1, fL2, fR2;
    dsp::Noise noise;
    double subPhase = 0;
    bool done = false;
    std::vector<double> oscDecay;             // per osc own-decay coefficient
    std::vector<double> oscDecayLevel;
    std::vector<double> fmLevel;              // per osc FM index envelope (1 at the attack)
    std::vector<double> fmCoef;

    void start(const Patch &patch, double sampleRate, uint32_t seed) {
        P = &patch; sr = sampleRate;
        noise = dsp::Noise(seed | 1);
        const int U = P->unison;
        units.assign(P->osc.size() * (size_t)U, Unit{});
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
                x.phase = U > 1 ? (hash32(seed * 131 + (uint32_t)(o * 17 + (size_t)u)) & 0xffffff) / 16777216.0 : 0.0;
                x.fmPhase = 0;
            }
        oscDecay.assign(P->osc.size(), 1.0);
        oscDecayLevel.assign(P->osc.size(), 1.0);
        fmLevel.assign(P->osc.size(), 1.0);
        fmCoef.assign(P->osc.size(), 1.0);
        for (size_t o = 0; o < P->osc.size(); ++o) {
            if (P->osc[o].decay > 0) oscDecay[o] = std::exp(-4.6 / (P->osc[o].decay * sr));
            if (P->osc[o].fm && P->osc[o].fmDecay > 0) fmCoef[o] = std::exp(-4.6 / (P->osc[o].fmDecay * sr));
        }
        amp.setup(P->amp, sr);
        fenv.setup(P->fenv, sr);
    }
    void attack(double k, double v, double t0, bool retrigger) {
        target = k;
        vel = v;
        if (retrigger) {
            noteStart = t0;
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
    case Osc::Noise: return 0;   // handled by the voice
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

bool renderSynth(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err) {
    const double sr = job.sampleRate;
    Patch P;
    std::vector<Envelope> autos((size_t)P_COUNT);   // parameter curves over song time
    std::vector<bool> automated((size_t)P_COUNT, false);
    bool pwParam = false;   // "pw" set by params or automation: it replaces every pulse oscillator's own width
    double patchTranspose = 0;          // a GarageBand patch's own transposition (semitones)
    json patchFx = json::array();       // and its effects, after the voices ("synth": {"effects": false} leaves them out)
    try {
        // the named patch, then the track's "synth" object merged over it
        json patch = json::object();
        const std::string preset = track.preset.empty() ? "Init" : track.preset;
        const json &bank = patchBank();
        std::string found;
        for (auto &[n, p] : bank.items()) if (n == preset) found = n;
        GarageBandSynth gb;
        if (found.empty() && garageBandSynthPatch(preset, gb)) {   // a GarageBand Retro Synth patch, re-created here
            found = "Init";
            patchTranspose = gb.transpose;
            patchFx = gb.fx;
            warnings.push_back("preset '" + gb.name + "' is GarageBand's " + gb.instrument + " patch" + (gb.engine.empty() ? "" : " (" + gb.engine + " mode)") +
                               ", re-created on builtin:synth: an approximation");
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
    if (!track.ccAutomation.empty() || !track.pressureAutomation.empty())
        warnings.push_back("builtin:synth ignores MIDI CC and pressure automation; automate its parameters by name (automation.params)");
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
                const double since = t - v.noteStart;
                for (const auto &l : P.lfos) {
                    double dep = l.lfo.depthAt(t) * lfoMul;
                    if (since < l.delay) dep = 0;
                    else if (l.fade > 0 && since < l.delay + l.fade) dep *= (since - l.delay) / l.fade;
                    const double wv = l.lfo.wave(t), w = wv * dep;
                    switch (l.to) {
                    case SynthLfo::Pitch: lfoPitch += w; break;
                    case SynthLfo::Cutoff: lfoCut += w; break;
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
                        const double pan = std::clamp(x.pan * spread, -1.0, 1.0);
                        const double ang = (pan + 1) * dsp::kPi / 4;
                        uL[o * (size_t)U + (size_t)u] = std::cos(ang) * M_SQRT2;
                        uR[o * (size_t)U + (size_t)u] = std::sin(ang) * M_SQRT2;
                    }
                {   // the voice's own pan (pan LFO)
                    const double ang = (std::clamp(lfoPan, -1.0, 1.0) + 1) * dsp::kPi / 4;
                    panL = std::cos(ang) * M_SQRT2; panR = std::sin(ang) * M_SQRT2;
                }
                if (P.ftype != Patch::Off) {
                    const double oct = keytrack * (v.key - 60) / 12.0 + envAmt * v.fenv.level + P.velToCutoff * (v.vel - 1) + lfoCut;
                    const double fc = std::clamp(cutoffBase * std::pow(2.0, oct), 20.0, nyq);
                    const double g = std::tan(dsp::kPi * fc / sr);
                    const double kRes = std::pow(0.05 / 1.414, res);   // 1 at res 0, down to Q ~20
                    if (P.slope == 24) {
                        v.fL1.set(g, 1.848); v.fR1.set(g, 1.848);
                        v.fL2.set(g, 0.765 * kRes); v.fR2.set(g, 0.765 * kRes);
                    } else { v.fL1.set(g, 1.414 * kRes); v.fR1.set(g, 1.414 * kRes); }
                }
                driveGain = 1 + 7 * drive;
                driveNorm = 1.0 / std::tanh(driveGain);
            }
            const double a = v.amp.next(sr);
            v.fenv.next(sr);
            if (v.amp.stage == 4) { v.done = true; break; }
            double L = 0, R = 0, postL = 0, postR = 0;   // post: oscillators that skip the filter
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
                const double g = osc.level * own * unitNorm;
                double &sumL = osc.filtered ? L : postL, &sumR = osc.filtered ? R : postR;
                for (int u = 0; u < U; ++u) {
                    const size_t idx = o * (size_t)U + (size_t)u;
                    Unit &x = v.units[idx];
                    const double s = osc.wave == Osc::Noise ? v.noise.next() : oscSample(osc, x, pw, fmAmt);
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
            if (P.ftype != Patch::Off) {
                double bL, hL, bR, hR;
                const double lL = v.fL1.run(L, bL, hL), lR = v.fR1.run(R, bR, hR);
                double oL = P.ftype == Patch::Lowpass ? lL : P.ftype == Patch::Highpass ? hL : bL * v.fL1.k;
                double oR = P.ftype == Patch::Lowpass ? lR : P.ftype == Patch::Highpass ? hR : bR * v.fR1.k;
                if (P.slope == 24) {   // a second, resonant stage of the same kind
                    const double l2L = v.fL2.run(oL, bL, hL), l2R = v.fR2.run(oR, bR, hR);
                    oL = P.ftype == Patch::Lowpass ? l2L : P.ftype == Patch::Highpass ? hL : bL * v.fL2.k;
                    oR = P.ftype == Patch::Lowpass ? l2R : P.ftype == Patch::Highpass ? hR : bR * v.fR2.k;
                }
                L = oL; R = oR;
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
    return true;
}

} // namespace wl
