#include "alchemy.hpp"

#include "dsp.hpp"
#include "platform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

using nlohmann::json;
namespace fs = std::filesystem;

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
int ri(double v) { return (int)std::nearbyint(v); }   // halves to even, as the decoder
std::string fmt(const char *f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }
const double kNone = std::numeric_limits<double>::quiet_NaN();

// ---- the preset text ---------------------------------------------------------------------------
struct Param;
struct Mod { int type = 0, id = 0, modmap = 0; std::vector<Param> depth; };   // depth: its own value line
struct Param {
    std::string key, raw;
    bool has = false;   // a number (a value line's first)
    double value = 0;
    std::vector<Mod> mods;
    std::vector<std::vector<double>> matrix;
};
struct Section { std::string name; std::vector<Param> params; };

std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::vector<std::string> words(const std::string &s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}
bool digits(const std::string &t, size_t a, size_t b) {
    if (a >= b) return false;
    for (size_t i = a; i < b; ++i) if (t[i] < '0' || t[i] > '9') return false;
    return true;
}
bool isInt(const std::string &t) { return digits(t, 0, t.size()); }
bool isNum(const std::string &t) {   // -?\d+(\.\d+)?
    const size_t a = !t.empty() && t[0] == '-' ? 1 : 0, dot = t.find('.', a);
    return dot == std::string::npos ? digits(t, a, t.size()) : digits(t, a, dot) && digits(t, dot + 1, t.size());
}
double num(const std::string &t) { return std::strtod(t.c_str(), nullptr); }

struct Parser {
    std::vector<std::string> lines;
    // a value line: "v smooth n" + n modulation slots, "cols rows-1" + rows (a matrix), or a number (else text only)
    Param value(const std::string &key, const std::string &raw, size_t &i) {
        Param p;
        p.key = key;
        p.raw = raw;
        const auto t = words(raw);
        if (t.size() == 3 && isNum(t[0]) && isNum(t[1]) && isInt(t[2]) && key != "Value" && key != "Tie" && key != "Length" && key != "Swing") {
            p.has = true;
            p.value = num(t[0]);
            for (long k = std::atol(t[2].c_str()); k > 0; --k) {
                Mod m;
                for (const char *fld : {"Type", "Id", "ModMap", "Depth"}) {
                    if (i >= lines.size()) throw std::runtime_error(key + ": the text ends inside a modulation slot");
                    const std::string l = trim(lines[i++]);
                    const size_t eq = l.find(" = ");
                    const std::string k2 = l.substr(0, eq), v = eq == std::string::npos ? "" : l.substr(eq + 3);
                    if (k2 != fld) throw std::runtime_error(key + ": expected " + fld + ", got '" + l + "'");
                    if (fld[0] == 'D') m.depth.push_back(value("Depth", trim(v), i));
                    else (fld[0] == 'T' ? m.type : fld[0] == 'I' ? m.id : m.modmap) = std::atoi(v.c_str());
                }
                p.mods.push_back(std::move(m));
            }
            return p;
        }
        if (t.size() == 2 && isInt(t[0]) && isInt(t[1]) && i < lines.size() && !trim(lines[i]).empty() && lines[i].find('=') == std::string::npos &&
            ((lines[i][0] >= '0' && lines[i][0] <= '9') || lines[i][0] == '-')) {
            for (long k = std::atol(t[1].c_str()) + 1; k > 0 && i < lines.size(); --k) {
                std::vector<double> row;
                for (auto &w : words(lines[i++])) row.push_back(num(w));
                p.matrix.push_back(row);
            }
            return p;
        }
        if (t.size() == 1 && isNum(t[0])) p.has = true, p.value = num(t[0]);
        else if (t.size() == 2 && isNum(t[0]) && isNum(t[1]) && t[0].find('.') != std::string::npos) p.has = true, p.value = num(t[0]);   // "value smooth"
        return p;
    }
};

struct Preset {
    std::vector<Section> sections;
    std::map<std::string, std::vector<size_t>> by;   // a section name -> its sections, in order

    explicit Preset(const std::string &text) {
        Parser ps;
        std::string cur;
        for (char c : text) {
            if (c == '\r') continue;
            if (c == '\n') { ps.lines.push_back(cur); cur.clear(); } else cur += c;
        }
        ps.lines.push_back(cur);
        sections.push_back({"header", {}});
        for (size_t i = 0; i < ps.lines.size();) {
            const std::string line = ps.lines[i++], s = trim(line);
            if (s.empty()) continue;
            if (s.front() == '<' && s.back() == '>' && s.find('=') == std::string::npos) {
                sections.push_back({s.size() >= 2 ? s.substr(1, s.size() - 2) : "", {}});
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            Param p = ps.value(trim(line.substr(0, eq)), trim(line.substr(eq + 1)), i);
            sections.back().params.push_back(std::move(p));
        }
        for (size_t k = 0; k < sections.size(); ++k) by[sections[k].name].push_back(k);
    }
    size_t count(const std::string &name) const { auto it = by.find(name); return it == by.end() ? 0 : it->second.size(); }
    const std::vector<Param> &sec(const std::string &name, size_t idx = 0) const {
        static const std::vector<Param> none;
        auto it = by.find(name);
        return it == by.end() || idx >= it->second.size() ? none : sections[it->second[idx]].params;
    }
    const Param *get(const std::string &name, const std::string &key, size_t idx = 0) const {
        for (auto &p : sec(name, idx)) if (p.key == key) return &p;
        return nullptr;
    }
    // a number by name (its first occurrence), else def
    double val(const std::string &name, const std::string &key, double def = kNone) const {
        const Param *p = get(name, key);
        return p && p->has ? p->value : def;
    }
};

// a section as the decoder's dictionaries have it: a key's last occurrence
std::map<std::string, const Param *> keyed(const std::vector<Param> &ps) {
    std::map<std::string, const Param *> d;
    for (auto &p : ps) d[p.key] = &p;
    return d;
}
const Param *at(const std::map<std::string, const Param *> &d, const std::string &k) { auto it = d.find(k); return it == d.end() ? nullptr : it->second; }
bool is1(const Param *p) { return p && p->has && p->value == 1; }
double numOf(const Param *p, double def = 0) { return p && p->has ? p->value : def; }

// ---- modulation at rest --------------------------------------------------------------------------
// Sources (Type:Id): 1:n LFO n+1, 2:n AHDSR n+1, 3:n MSEG n+1, 4:n sequencer n+1, 0:n envelope follower; 5:n note
// properties (0 velocity, 1 key follow, 2 key follow with glide, 3 aftertouch, 4 speed, 5 held, 6/7 flip-flop, 8-10
// stepped, 11-14 random, 15 bend, 16 max, 17 polyphony); 6:n Perform (0-7 knobs, 8-11 the XY pads, 12-15 the A/D/S/R
// knobs, 16 the transform pad); 7:n MIDI controllers; 8:n arpeggiator steps. A Depth is 0..1 for -100%..+100%.
const char *kPerfKeys[16] = {"_Contrl1", "_Contrl2", "_Contrl3", "_Contrl4", "_Contrl5", "_Contrl6", "_Contrl7", "_Contrl8",
                             "_XyPad1x", "_XyPad1y", "_XyPad2x", "_XyPad2y", "_EnvAtt", "_EnvDec", "_EnvSust", "_EnvRel"};

std::string srcName(int t, int i) {
    static const char *note[18] = {"velocity", "keyfollow", "keyfollow-glide", "aftertouch", "speed", "held", "flipflop", "flipflop2", "stepped4",
                                   "stepped8", "stepped16", "random1", "random2", "random3", "random4", "pitchbend", "max", "polyphony"};
    static const char *kinds[9] = {"envfollow", "lfo", "ahdsr", "mseg", "seq", "note", "perform", "midi", "arp"};
    if (t == 5) return i >= 0 && i < 18 ? note[i] : "note" + std::to_string(i);
    if (t == 6) return "perform:" + (i >= 0 && i < 16 ? std::string(kPerfKeys[i] + 1) : "transform");
    return (t >= 0 && t < 9 ? std::string(kinds[t]) : "t" + std::to_string(t)) + std::to_string(i + 1);
}

struct Dyn { int t, i; double d; int mm; };   // a moving modulation: source type and id, depth -1..1, modulation map
struct Eff { double v = 0; std::vector<Dyn> dyn; };

struct Ctx {
    const Preset &P;
    std::array<double, 16> perf{};
    explicit Ctx(const Preset &p) : P(p) {
        std::map<std::string, double> vals;   // the Perform section's first numeric block: each control's saved position
        for (auto &q : P.sec("perform"))
            for (const char *k : kPerfKeys)
                if (q.key == k && q.has && !vals.count(k)) vals[k] = q.value;
        for (size_t k = 0; k < 16; ++k) perf[k] = vals.count(kPerfKeys[k]) ? vals[kPerfKeys[k]] : 0.0;
    }
    // a modulation map (1-based): its curve's first two rows, x and y, linear between points
    bool mapped(int idx, double x, double &y) const {
        if (idx <= 0 || (size_t)idx > P.count("modmap")) return false;
        for (auto &p : P.sec("modmap", (size_t)idx - 1)) {
            if (p.key != "MmpMap" || p.matrix.size() < 2 || p.matrix[0].empty()) continue;
            const auto &xs = p.matrix[0], &ys = p.matrix[1];
            auto Y = [&](size_t j) { return ys.empty() ? 0.0 : ys[std::min(j, ys.size() - 1)]; };
            if (x <= xs.front()) y = Y(0);
            else if (x >= xs.back()) y = Y(xs.size() - 1);
            else {
                const size_t j = (size_t)(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin()) - 1;
                const double t = (x - xs[j]) / std::max(1e-9, xs[j + 1] - xs[j]);
                y = Y(j) + t * (Y(j + 1) - Y(j));
            }
            return true;
        }
        return false;
    }
    // a modulator's value at rest; false when it moves (envelopes, LFOs, velocity, key)
    bool still(const Mod &m, double &v) const {
        if (m.type == 6) v = m.id >= 0 && m.id < 16 ? perf[(size_t)m.id] : 0.0;
        else if (m.type == 5 && m.id == 16) v = 1.0;
        else if (m.type == 7 || (m.type == 5 && (m.id == 3 || m.id == 15))) v = 0.0;   // wheels, controllers, aftertouch, bend
        else return false;
        double y;
        if (m.modmap && mapped(m.modmap, v, y)) v = y;
        return true;
    }
    double depth(const Mod &m) const {   // -1..1, its own still modulators included
        const Param *d = m.depth.empty() ? nullptr : &m.depth[0];
        double base = d && d->has ? d->value : 0.5;
        if (d)
            for (auto &mm : d->mods) {
                double s;
                if (still(mm, s) && !mm.depth.empty() && mm.depth[0].has) base += (2 * mm.depth[0].value - 1) * s;
            }
        return 2 * std::min(1.0, std::max(0.0, base)) - 1;
    }
    Eff eff(const Param *p) const {   // its value at rest (0..1) and its moving modulations
        Eff e;
        if (!p) return e;
        e.v = p->has ? p->value : 0.0;
        for (auto &m : p->mods) {
            const double d = depth(m);
            double s;
            if (still(m, s)) e.v += d * s;
            else if (std::fabs(d) > 1e-4) e.dyn.push_back({m.type, m.id, d, m.modmap});
        }
        e.v = std::min(1.0, std::max(0.0, e.v));
        return e;
    }
    double at(const Param *p) const { return eff(p).v; }
};

// ---- scales ------------------------------------------------------------------------------------
double semis(double v) { return (v - 0.5) * 96; }    // coarse tune +-48 (every saved value is a whole 1/96 step)
double cents(double v) { return (v - 0.5) * 200; }   // fine tune +-100 cents
double linDb(double v) { return 20 * std::log10(std::max(v, 1e-5)); }   // volumes are linear gains (0.7943 = -2 dB)
const double kCutOct = 128.0 / 12;   // key follow 100% + bend 3.2% (the factory convention) tracks 1:1 over 128 keys
double cutoffHz(double v) { return 20000.0 * std::pow(2.0, -(1 - v) * kCutOct); }   // a guess: 1.0 = 20 kHz
// a distortion amount (0..1) as saturate drive in dB: a guess kept mild until reference renders (3 + 24 x v made chords
// harsh: a half-way knob drove every note of a chord into one another at 15 dB)
double distortionDb(double v) { return 3 + 12 * std::min(1.0, std::max(0.0, v)); }
double envTime(double v) { return 20.0 * std::pow(std::max(0.0, v), 4.0); }       // AHDSR stages 0..20 s (a guess: v^4)
double lfoHz(double v) { return 220.0 * std::pow(std::max(0.0, v), 6.0); }        // free LFOs 0..220 Hz (a guess: v^6)
double glideSec(double v) { return std::max(0.0, v); }                            // a guess: 0..1 s
int unisonCount(double v) { return 1 + ri(15 * v); }                              // a guess: 1..16 voices
double detuneCents(double v, bool wide) { return wide ? (v <= 0.5 ? 400 * v : 200 + 400 * (v - 0.5)) : 50 * v; }   // guesses
double syncSemis(double v) { return 48 * v; }   // a guess: the synced oscillator up to 4 octaves above

// a synced rate (LFO, arpeggiator, delay, phaser) as a note value: 0.5 = a beat, about 0.095 an octave (a guess)
std::string syncNote(double v) {
    static const std::pair<double, const char *> notes[] = {
        {64, "16/1"}, {48, "12/1"}, {32, "8/1"}, {24, "6/1"}, {16, "4/1"}, {12, "3/1"}, {8, "2/1"}, {6, "3/2"}, {16.0 / 3, "4/3"},
        {4, "1/1"}, {3, "1/2D"}, {8.0 / 3, "1/2T"}, {2, "1/2"}, {1.5, "1/4D"}, {4.0 / 3, "1/4T"}, {1, "1/4"}, {0.75, "1/8D"},
        {2.0 / 3, "1/8T"}, {0.5, "1/8"}, {0.375, "1/16D"}, {1.0 / 3, "1/16T"}, {0.25, "1/16"}, {0.1875, "1/32D"}, {1.0 / 6, "1/32T"},
        {0.125, "1/32"}, {1.0 / 12, "1/64T"}, {0.0625, "1/64"}};
    const double beats = std::pow(2.0, 10.5 * (0.5 - v));
    const char *best = notes[0].second;
    double bd = 1e300;
    for (auto &[b, n] : notes) {
        const double dist = std::fabs(std::log2(b / beats));
        if (dist < bd) { bd = dist; best = n; }
    }
    return best;
}
// "1/2D" -> 1/2: the note value's fraction
double fraction(const std::string &s) {
    const size_t slash = s.find('/');
    return slash == std::string::npos ? num(s) : num(s.substr(0, slash)) / num(s.substr(slash + 1));
}

// The filter types (main, source, MM Filter and delay filters share one list of 39), inferred from their use: 0-8
// low-pass (6-8 24 dB), 9-16 band-pass (15-16 24 dB), 17-25 high-pass (23-25 24 dB), 26, 30, 32, 33 and 37 drive
// types (distortion, bit reduction, compression), the rest note-tuned (comb, ring, FM, formant, notch, peak)
std::pair<std::string, int> filterKind(double code) {
    const int c = (int)code;
    if (c >= 0 && c <= 8) return {"lowpass", c >= 6 ? 24 : 12};
    if (c >= 9 && c <= 16) return {"bandpass", c >= 15 ? 24 : 12};
    if (c >= 17 && c <= 25) return {"highpass", c >= 23 ? 24 : 12};
    if (c == 26 || c == 30 || c == 32 || c == 33 || c == 37) return {"drive", 0};
    return {"tuned", 0};
}

std::string replaceAll(std::string s, const std::string &a, const std::string &b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
    return s;
}

// A VA wave by its library name -> builtin wave, pulse width (NaN = none), sine partials, exact or a stand-in. The
// basic shapes and the named pulses play as themselves; other tables play as the shape their family is named for.
struct Wave { std::string wave; double pw = kNone; std::vector<int> partials; bool exact = false; };
Wave mapWave(const std::string &path) {
    const std::string name = replaceAll(replaceAll(path, "Alchemy/Libraries/WaveOsc/", ""), ".raw", "");
    if (name == "Basic/Saw") return {"saw", kNone, {}, true};
    if (name == "Basic/Square") return {"square", 0.5, {}, true};
    if (name == "Basic/Sine") return {"sine", kNone, {}, true};
    if (name == "Basic/Triangle") return {"triangle", kNone, {}, true};
    const size_t slash = name.find('/');
    const std::string cat = name.substr(0, slash), rest = slash == std::string::npos ? "" : name.substr(slash + 1);
    if (cat == "Saw") return {"saw", kNone, {}, rest.find("Sine") == std::string::npos};
    if (cat == "Square") return {"square", 0.5, {}, rest.find("Saw") == std::string::npos};
    if (cat == "Pulse") {
        for (size_t p = rest.find("Pulse - "); p != std::string::npos; p = rest.find("Pulse - ", p + 1)) {
            size_t e = p + 8;
            while (e < rest.size() && rest[e] >= '0' && rest[e] <= '9') ++e;
            if (e > p + 8) return {"square", std::atoi(rest.c_str() + p + 8) / 100.0, {}, true};
        }
        return {"square", 0.5, {}, rest == "DC"};
    }
    if (cat == "Sine") {
        if (rest.rfind("Triangle", 0) == 0) return {"triangle", kNone, {}, rest.find("Asym") == std::string::npos};
        if (rest.rfind("Sine - Add", 0) == 0 && digits(rest, 10, rest.size())) {   // a sum of sine partials named by their numbers
            Wave w{"sine", kNone, {}, true};
            for (size_t k = 10; k < rest.size(); ++k) w.partials.push_back(rest[k] - '0');
            return w;
        }
        return {"sine", kNone, {}, rest == "Sine - Mg" || rest == "Sine - DC" || rest == "Sine - Arp" || rest == "Sine - Root 1"};
    }
    return {"saw", kNone, {}, false};
}

std::string lfoShape(const std::string &path) {
    static const std::map<std::string, std::string> shapes = {{"Basic/Sine", "sine"}, {"Basic/Triangle", "triangle"}, {"Basic/Square", "square"},
                                                              {"Basic/Ramp Up", "ramp"}, {"Basic/Ramp Down", "saw"}, {"Basic/Random Hold", "random"},
                                                              {"Basic/Random Glide", "random"}};
    auto it = shapes.find(path);
    return it == shapes.end() ? "" : it->second;
}

std::string effectName(int t) {
    static const std::map<int, const char *> names = {{1, "Delay"}, {2, "Classic Reverb"}, {3, "Mod FX"}, {4, "Distortion"}, {5, "MM Filter"},
        {6, "Bandpass Filter"}, {7, "Phat Compressor"}, {8, "Bass Enhancer"}, {9, "Panner"}, {10, "Amp"}, {11, "Acoustic Reverb"},
        {12, "3-Band EQ"}, {13, "Band Reject"}, {20, "Phaser"}, {21, "Convolution Reverb"}, {22, "Vintage Compressor"}, {23, "Waveshaper"}};
    auto it = names.find(t);
    return it == names.end() ? "effect type " + std::to_string(t) : it->second;
}

// ---- content files: where the preset says, else by name under Logic's and GarageBand's content folders
std::string resolveFile(const std::string &path) {
    std::error_code ec;
    if (fs::exists(fs::u8path(path), ec)) return path;
    static std::map<std::string, std::string> index;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const fs::path &root : {fs::path("/Library/Application Support/Logic"), fs::path("/Library/Application Support/GarageBand"),
                                     fs::path("/Library/Audio/Apple Loops"), platform::homeDir() / "Music/Audio Music Apps"}) {
            std::error_code e2;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, e2); it != fs::recursive_directory_iterator(); it.increment(e2)) {
                if (e2) break;
                std::string n = it->path().filename().u8string(), ext = it->path().extension().u8string();
                for (auto *s : {&n, &ext}) for (auto &c : *s) c = (char)std::tolower((unsigned char)c);
                if ((ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".caf" || ext == ".aaz" || ext == ".exs") && it->is_regular_file(e2))
                    index.emplace(n, it->path().u8string());
            }
        }
    });
    std::string n = fs::u8path(path).filename().u8string();
    for (auto &c : n) c = (char)std::tolower((unsigned char)c);
    auto it = index.find(n);
    return it == index.end() ? "" : it->second;
}

// a sample file's rate from its header (WAV, AIFF, CAF); 0 = unknown
double sampleRate(const std::string &path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::vector<uint8_t> b(4096);
    in.read(reinterpret_cast<char *>(b.data()), (std::streamsize)b.size());
    b.resize((size_t)in.gcount());
    auto find = [&](const char *tag) { auto it = std::search(b.begin(), b.end(), tag, tag + 4); return it == b.end() ? std::string::npos : (size_t)(it - b.begin()); };
    if (b.size() >= 4 && !std::memcmp(b.data(), "RIFF", 4)) {
        const size_t i = find("fmt ");
        if (i != std::string::npos && i + 16 <= b.size()) return b[i + 12] | b[i + 13] << 8 | b[i + 14] << 16 | (uint32_t)b[i + 15] << 24;
    } else if (b.size() >= 4 && !std::memcmp(b.data(), "FORM", 4)) {
        const size_t i = find("COMM");
        if (i != std::string::npos && i + 26 <= b.size()) {   // an 80-bit extended rate
            const int e = b[i + 16] << 8 | b[i + 17];
            uint64_t mant = 0;
            for (int k = 0; k < 8; ++k) mant = mant << 8 | b[i + 18 + (size_t)k];
            return std::floor((double)mant * std::pow(2.0, e - 16383 - 63));
        }
    } else if (b.size() >= 4 && !std::memcmp(b.data(), "caff", 4)) {
        const size_t i = find("desc");
        if (i != std::string::npos && i + 20 <= b.size()) {
            uint64_t bits = 0;
            for (int k = 0; k < 8; ++k) bits = bits << 8 | b[i + 12 + (size_t)k];
            double v;
            std::memcpy(&v, &bits, 8);
            return std::floor(v);
        }
    }
    return 0;
}

// ---- the decoded patch ---------------------------------------------------------------------------
struct FMod { std::string name; double d; int t, i; };   // a moving modulation of a filter's cutoff (depth rounded)
struct Filt {
    std::string where, kind;
    int code = 0, slope = 0, slot = 0;
    double cutoff = 0, res = 0, drive = 0, fxSend = 0;
    std::vector<FMod> mods;
    std::string fxRack;
    std::vector<std::string> skipped;   // filters left out for it (covering little of the level)
};
struct AZone {
    std::string file, found;
    int key = 60, lokey = 0, hikey = 127, lovel = 0, hivel = 127, loopMode = 0;
    double volDb = 0, tune = 0, fine = 0, pan = 0;
    bool loop = false, hasEnd = false;
    double loopStart = 0, loopEnd = 0, end = 0;
};
struct Source {
    char letter = 'A';
    bool on = false, audible = false;
    double weight = 0, ampDb = 0, tune = 0, fine = 0, pan = 0;
    int keytrack = 0;   // 0 off, 1 key + bend, 2 key
    // elements
    bool va = false, noise = false, additive = false, spectral = false, granular = false, sampler = false;
    Wave wave;
    std::string vaShape;
    double vaVolDb = 0, sym = 0.5, sync = 0, detune = 0;
    int nosc = 1;
    bool wide = false;
    std::string noiseShape;
    double noiseVolDb = 0, lowcut = 0, highcut = 1;
    bool analysis = false;   // additive: from analysis data (else drawn partials)
    bool adComplex = false;  // additive: each partial plays the Shape wave (else a sine)
    std::string adShape;
    Eff adNosc, adSym, adVol;
    double adPvar = 0;
    struct AdUnit { bool on = false; int type = 0; std::array<Eff, 4> p; };
    std::array<AdUnit, 3> adUnits;   // the additive element's three effect units: levels, tunings, pan or output
    bool stereo = false, formant = false;
    bool fmtSynth = false;                      // the formant filter's synthesized section: four slots, Select between them
    double fmtSelect = 0, fmtShift = 0.5;
    std::array<std::string, 4> fmtSlots;
    Eff grSpeed, grSize, grRand, pos;   // granular: Speed (0.5 = 100%), grain size, random position, Position
    double sampVolDb = 0, position = 0;
    bool spNoise = false;                       // the spectral element in Noise mode (an Add+Spec import's noisy part)
    double spVolDb = 0, spLowcut = 0, spHighcut = 1;
    bool reverse = false;
    std::vector<AZone> zones;
    std::vector<Filt> filters;
    bool parallel = false;
    std::string first = "F1", second = "?";   // where its signal goes, and its send
    double send = 0;
    std::map<std::string, std::vector<Dyn>> dyn;
    bool content() const { return sampler || granular || additive || spectral; }
};
struct Effect {
    int rack = 0, slot = 0, type = 0;
    std::string name;
    bool on = true;
    std::map<std::string, double> params;   // numbers only, at rest (rounded to 4 places)
    std::map<std::string, std::vector<std::pair<std::string, double>>> dyn;
    double g(const std::string &k, double def = 0) const { auto it = params.find(k); return it == params.end() ? def : it->second; }
};
struct ArpBlock { std::string order, rate; bool free = false; int octaves = 1; double gate = 0; };
struct Route { std::string src, tgt; double d; };

// ---- additive elements -----------------------------------------------------------------------------
// An additive element starts as Num Partials oscillators on the harmonic series at equal levels (the guide: "None
// results in an equal volume level for all partials"), each a sine (Sine mode) or the Shape wave (Complex mode, by
// the shape its name gives, as the VA oscillators); its .aaz analysis data, when it has some, sets their levels. Three
// effect units then shape them: unit 1 the levels, unit 2 the tunings, unit 3 pans the partials or filters the
// element. Their types are global ids (0 None, 1 Harmonic, 2 Pulse/Saw, 3 Saw+Noise, 5 Beating, 6 Stretch, 7 Shift,
// 9-12 Alchemy 1.x's pitch profiles Harmonic, Dbl-Odds, Unison and 1245-1346 (named by the profile files the converted
// patches still carry), 13 Spread, 14 Auto Pan, 15-17 1.x pan profiles, 18 amplitude Noise, 20 pitch Noise, 21 Strum,
// 22 EQ, 23 Comb, 24 Filter, 25 Ripples), inferred from the factory patches (13 sits on 26 of 26 stereo sources, 23's
// Frequency follows the key, Perform labels such as "Random LFO" and "Chorus") and the guide's and Camel Audio's
// manual's descriptions; the scales marked "a guess" await reference renders. Num Partials = 1 + 117.6 v up to
// v = 0.34 (calibrated on drawn .aaz files whose knob the designers set to their partial count), then a guessed curve
// up to the guide's 600.
double numPartials(double v) {
    v = std::min(1.0, std::max(0.0, v));
    return 1 + 117.6 * v + 481.4 * std::pow(std::max(0.0, (v - 0.34) / 0.66), 2.5);
}

// harmonics 1..M of one cycle of f sampled at 2048 points, as complex amplitudes (a sine at harmonic m gives 1)
std::vector<std::complex<double>> cycleSpectrum(const std::function<double(double)> &f, size_t M) {
    const size_t n = 2048;
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = f((i + 0.5) / n);
    dsp::fft(a);   // the samples sit half a step into the cycle: e^(-pi i m / n)
    std::vector<std::complex<double>> out;
    for (size_t m = 1; m <= M; ++m)
        out.push_back(2.0 * (a[m % n] * std::polar(1.0, -dsp::kPi * (double)m / n)) / (double)n * std::complex<double>(0, 1));
    return out;
}

// the shape wave of a Complex element over one cycle: a basic wave (sine partials for "Sine - Add1248"), its
// symmetry lengthening one half of the cycle and shortening the other (0.5 = unchanged)
std::function<double(double)> shapeCycle(const Wave &w, double sym) {
    std::function<double(double)> f;
    const double pw = std::isnan(w.pw) || w.pw == 0 ? 0.5 : w.pw;
    if (!w.partials.empty()) f = [p = w.partials](double ph) { double s = 0; for (int h : p) s += std::sin(2 * dsp::kPi * h * ph); return s / (double)p.size(); };
    else if (w.wave == "saw") f = [](double ph) { return ph > 0 ? 2 * ph - 1 : 0.0; };
    else if (w.wave == "square") f = [pw](double ph) { return (ph < pw ? 1.0 : -1.0) - (2 * pw - 1); };
    else if (w.wave == "triangle") f = [](double ph) { return 1 - 4 * std::fabs(ph - 0.5); };
    else f = [](double ph) { return std::sin(2 * dsp::kPi * ph); };
    const double s = std::min(0.98, std::max(0.02, sym));
    if (std::fabs(s - 0.5) < 1e-4) return f;
    return [f, s](double ph) { return f(ph < s ? ph / (2 * s) : 0.5 + (ph - s) / (2 * (1 - s))); };
}

// Odd/Even (Harmonic, Pulse/Saw): low values raise the odd partials (the fundamental too), high ones the even ones
// while the fundamental stays; unity at 0.5
double oddEven(int h, double v) {
    if (h == 1) return v < 0.5 ? 2 * (1 - v) : 1.0;
    return h % 2 == 0 ? 2 * v : 2 * (1 - v);
}
bool powerOf3(int h) { while (h % 3 == 0) h /= 3; return h == 1; }
// oscillator k's place in a stack of K, -1..1: the centre first, then alternating sides outwards
double stackPos(int k, int K) {
    if (K <= 1 || !k) return 0.0;
    const int half = std::max(1, K / 2), j = (k + 1) / 2;
    return (double)j / half * (k % 2 ? 1 : -1);
}

struct AdElement {
    double n = 1;                                   // Num Partials
    std::vector<std::array<double, 3>> oscs;        // the partial oscillators: level, ratio, pan
    std::vector<std::array<double, 3>> partials;    // as sine partials, every oscillator's wave spread out: level (loudest 1), ratio, pan
    double shiftHz = 0;
    Wave wave;
    double sym = 0.5;
    bool simple = true;                             // plain sine partials (a sine shape, no symmetry)
    bool pulseSaw = false;                          // unit 1 is Pulse/Saw: ps (0 pulse, 1 saw), sync, odd/even, tone
    double ps = 1, sync = 0, oddEven = 0.5, tone = 0.5;
    bool othersOn = false;                          // units 2 and 3 hold something besides None, Comb, Filter and EQ
    std::vector<std::string> notes, unexpressed, unknown;
    json post = json::array();                      // unit 3 as effects after the voices (Comb, Filter, EQ)
    json formant = json::array();                   // the synthesized formant filter as EQ peaks (merged over the sources)
    std::vector<std::complex<double>> shape;        // the shape wave's harmonics (Complex mode)
};

// One additive element as partials. base: harmonic number -> level from its .aaz data (null = none: equal levels).
AdElement decodeAdditive(const Source &s, const std::map<int, double> *base) {
    AdElement e;
    double N = numPartials(s.adNosc.v);
    if (base && !base->empty()) N = std::min(N, (double)base->rbegin()->first);
    const int K = std::max(1, std::min(600, (int)std::ceil(N - 1e-9)));
    e.n = N;
    std::vector<double> amp((size_t)K), ratio((size_t)K), pan((size_t)K, 0.0);
    for (int k = 0; k < K; ++k) {
        amp[(size_t)k] = std::min(1.0, std::max(0.0, N - k));   // partial k+1 fades in with the knob
        if (base) { auto it = base->find(k + 1); amp[(size_t)k] *= it == base->end() ? 0.0 : it->second; }
        ratio[(size_t)k] = k + 1;
    }
    for (size_t u = 0; u < 3; ++u) {
        const auto &unit = s.adUnits[u];
        if (!unit.on || unit.type == 0) continue;
        const int t = unit.type;
        double p[4];
        for (int j = 0; j < 4; ++j) p[j] = unit.p[(size_t)j].v;
        if (u > 0 && t != 22 && t != 23 && t != 24) e.othersOn = true;
        if (t == 1) {   // Harmonic: Fundamental, Octaves, Odd/Even, Fifths, each a pair of profiles crossfaded (unity at 0.5)
            for (int k = 0; k < K; ++k) {
                const int h = k + 1;
                double g = h == 1 ? 2 * p[0] : 2 * (1 - p[0]);
                g *= (h & (h - 1)) == 0 ? 2 * p[1] : 2 * (1 - p[1]);
                g *= oddEven(h, p[2]);
                g *= powerOf3(h) ? 2 * p[3] : 2 * (1 - p[3]);
                amp[(size_t)k] *= g;
            }
        } else if (t == 2 || t == 3) {   // Pulse/Saw (1.0 a saw's 1/k, 0 a pulse: odd partials only), Saw+Noise; Odd/Even, Tone tilt
            double PS = 1, SY = 0, OE = 0.5, TN = p[3];
            if (t == 2) {
                PS = p[0], SY = p[1], OE = p[2];
                e.pulseSaw = true, e.ps = PS, e.sync = SY, e.oddEven = OE, e.tone = TN;
            } else if (p[0] > 0.005) e.unexpressed.push_back("Saw+Noise: noise " + fmt("%.2f", p[0]) + " on the partial levels (random, moving)");
            if (SY > 0.002) {   // the levels of a hard-synced wave, its slave Sync x 48 semitones up (a guess, as the VA sync)
                const double R = std::pow(2.0, 48 * SY / 12);
                const auto spec = cycleSpectrum([R, PS](double ph) { const double q = std::fmod(ph * R, 1.0); return PS * (2 * q - 1) + (1 - PS) * (q < 0.5 ? 1.0 : -1.0); },
                                                (size_t)std::max(K, 1));
                for (int k = 0; k < K; ++k) amp[(size_t)k] *= std::abs(spec[(size_t)k]) / std::max(1e-9, std::abs(spec[0]));
                e.notes.push_back("Pulse/Saw sync " + fmt("%.2f", SY) + " (about " + fmt("%.0f", 48 * SY) + " semitones, a guess): levels of a hard-synced wave");
            } else
                for (int k = 0; k < K; ++k) amp[(size_t)k] *= ((k + 1) % 2 ? 1.0 : PS) / (k + 1);
            for (int k = 0; k < K; ++k) amp[(size_t)k] *= oddEven(k + 1, OE) * std::pow((double)(k + 1), 2 * (TN - 0.5));
        } else if (t == 5) {   // Beating: every step-th partial retuned (Partial: every 2nd .. 16th, a guess)
            const int step = 2 + ri(14 * p[1]);
            for (int k = 0; k < K; ++k)
                if ((k + 1) % step == 0) ratio[(size_t)k] = p[2] >= 0.5 ? (k + 1) - 2 * (1 - p[0]) : (k + 1) - 2 + 3 * p[0];
            e.notes.push_back("Beating: partials " + std::to_string(step) + ", " + std::to_string(2 * step) + ", ... retuned (Amount " + fmt("%.2f", p[0]) +
                              ", Tuned " + (p[2] >= 0.5 ? "on" : "off") + "; scales from the guide's text)");
        } else if (t == 6) {   // Stretch: intervals scaled (0.5 harmonic, 0 unison, 1 = 2n-1); String a stiff string's inharmonicity (a guess)
            const double B = 0.002 * p[1] * p[1];
            for (int k = 0; k < K; ++k) ratio[(size_t)k] = (1 + (ratio[(size_t)k] - 1) * 2 * p[0]) * std::sqrt(1 + B * ((double)(k + 1) * (k + 1) - 1));
            if (std::fabs(p[0] - 0.5) > 0.01 || p[1] > 0.01) e.notes.push_back("Stretch Amount " + fmt("%.2f", p[0]) + ", String " + fmt("%.2f", p[1]) + " (the scales are a guess)");
        } else if (t == 7) {   // Shift: Pitch (+-12 semitones, key-tracked) and Frequency (+-1000 Hz), guesses
            const double d = std::pow(2.0, 24 * (p[0] - 0.5) / 12) - 1;
            for (auto &x : ratio) x += d;
            e.shiftHz += 2000 * (p[1] - 0.5);
        } else if (t == 9) {   // 1.x Harmonic pitch profile: 0 unison, 0.5 harmonic, 1.0 partial n at 2n-1
            for (auto &x : ratio) x = 1 + (x - 1) * 2 * p[0];
        } else if (t == 11) {   // 1.x Unison pitch profile: a stack detuned over 50 cents at 1.0 (a guess)
            for (int k = 0; k < K; ++k) ratio[(size_t)k] = std::pow(2.0, 50 * p[0] * stackPos(k, K) / 2 / 1200);
            e.notes.push_back("Unison profile " + fmt("%.2f", p[0]) + ": " + std::to_string(K) + " oscillators detuned over " + fmt("%.0f", 50 * p[0]) + " cents (a guess)");
        } else if (t == 10 || t == 12) {   // 1.x pitch profiles: a 0% and a 100% table crossfaded by the knob (Camel 10.5). Dbl-Odds:
            // 1 1 2 2 3 3 ... to the odd harmonics 1 3 5 7 ...; 1245-1346: 1 2 4 5 7 8 ... to 1 3 4 6 7 9 ... (past the
            // names' own numbers the tables are a guess)
            for (int k = 0; k < K; ++k) {
                const int n = k + 1;
                const double a = t == 10 ? (n + 1) / 2 : n + (n - 1) / 2, b = t == 10 ? 2 * n - 1 : n + n / 2;
                ratio[(size_t)k] = (1 - p[0]) * a + p[0] * b;
            }
            e.notes.push_back(std::string("Alchemy 1.x pitch profile ") + (t == 10 ? "Dbl-Odds" : "1245-1346") + " at " + fmt("%.2f", p[0]) +
                              " (its tables past the profile's name are a guess)");
        } else if (t == 18) {   // amplitude Noise (Amount, Rate, Smooth, Min Partial): levels move at random; at rest their mean
            const int from = std::max(1, std::min(K, 1 + ri(p[3] * (K - 1))));
            for (int k = from - 1; k < K; ++k) amp[(size_t)k] *= 1 - p[0] / 2;
            if (p[0] > 0.005) e.unexpressed.push_back("amplitude Noise " + fmt("%.2f", p[0]) + " (partial levels moving at random from partial " + std::to_string(from) + ")");
        } else if (t == 25) {   // Ripples (Amount, Group, Period, Phase): a raised-cosine ripple over the levels, Period 2-64 partials (a guess)
            const double per = 2 + 62 * p[2];
            for (int k = 0; k < K; ++k) amp[(size_t)k] *= 1 - p[0] * (1 - std::cos(2 * dsp::kPi * ((double)k / per + p[3]))) / 2;
            if (p[0] > 0.005) e.notes.push_back("Ripples " + fmt("%.2f", p[0]) + " every " + fmt("%.0f", per) + " partials (scales a guess; Group not read)");
        } else if (t == 20) {
            if (p[0] > 0.005) e.unexpressed.push_back("pitch Noise " + fmt("%.2f", p[0]) + " (random partial detuning, moving)");
        } else if (t >= 15 && t <= 17) {   // 1.x pan profiles: the stack spread, odd and even apart, or low to high
            const double w = 2 * (p[0] - 0.5);
            for (int k = 0; k < K; ++k)
                pan[(size_t)k] = t == 17 ? w * stackPos(k, K) : t == 16 ? ((k + 1) % 2 ? -w : w) : w * (2.0 * k / std::max(1, K - 1) - 1);
            if (std::fabs(w) > 0.02 && !s.stereo) std::fill(pan.begin(), pan.end(), 0.0);   // a mono source: no effect
        } else if (t == 13 || t == 14) {   // Spread / Auto Pan: p1 Amount, p3 Rate (Perform knobs labelled "Spread", "Stereo", "Mod Speed",
            // "Rotary" drive them), p2 / p4 Ramp and Cycles (which is which a guess). At rest every other partial to the other
            // side, the low ones less (p2 as Ramp); a mono source: no effect. The movement is not played
            if (s.stereo && p[0] > 0.005) {
                for (int k = 0; k < K; ++k) {
                    const double w = p[1] + (1 - p[1]) * (K > 1 ? (double)k / (K - 1) : 1.0);
                    pan[(size_t)k] = std::max(-1.0, std::min(1.0, p[0] * w * (k % 2 ? -1.0 : 1.0)));
                }
                if (p[2] > 0.005) e.unexpressed.push_back(std::string(t == 13 ? "Spread" : "Auto Pan") + " movement (Rate " + fmt("%.2f", p[2]) + ")");
            }
        } else if (t == 23) {   // Comb: 16 Hz - 20 kHz (exponential, a guess) as a short feedback delay
            if (p[0] > 0.005) {
                const double fq = 16 * std::pow(1250.0, p[2]);
                e.post.push_back({{"type", "delay"}, {"ms", r(1000 / fq, 1000)}, {"feedback", r(std::min(0.95, std::fabs(2 * p[3] - 1)))}, {"mix", r(std::min(1.0, p[0]))},
                                  {"pingpong", false}, {"highpass", 20.0}, {"lowpass", 20000.0}});
                e.notes.push_back("Comb " + fmt("%.0f", fq) + " Hz as a feedback delay (damping not played)");
            }
        } else if (t == 24) {   // Filter: LP-HP 0 low-pass, 0.5 band-pass, 1 high-pass; cutoff as Comb's
            if (p[0] > 0.005)
                e.post.push_back({{"type", "filter"}, {"mode", p[1] < 0.33 ? "lowpass" : p[1] < 0.67 ? "bandpass" : "highpass"},
                                  {"cutoff", r(std::min(20000.0, 16 * std::pow(1250.0, p[2])), 10)}, {"resonance", r(0.7071 + 8 * p[3] * p[3])}, {"mix", r(std::min(1.0, p[0]))}});
        } else if (t == 22) {   // EQ: four bands of +-18 dB (a guess)
            json bands = json::array();
            const std::pair<double, const char *> at[4] = {{120, "lowshelf"}, {310, "peak"}, {2000, "peak"}, {5000, "highshelf"}};
            for (int j = 0; j < 4; ++j)
                if (std::fabs(36 * (p[j] - 0.5)) > 0.2) bands.push_back({{"type", at[j].second}, {"freq", at[j].first}, {"gain", r(36 * (p[j] - 0.5), 10)}, {"q", 0.7}});
            if (!bands.empty()) e.post.push_back({{"type", "eq"}, {"bands", bands}});
        } else if (t == 21) {
            if (p[0] > 0.005) e.unexpressed.push_back("Strum " + fmt("%.2f", p[0]) + " (partial levels moving with the chosen partial)");
        } else if (std::fabs(p[0]) > 0.005 || std::fabs(p[1]) > 0.005 || std::fabs(p[2]) > 0.005 || std::fabs(p[3]) > 0.005) {
            e.unknown.push_back("unit " + std::to_string(u + 1) + ": type " + std::to_string(t) + " at " + fmt("%.2f", p[0]) + ", " + fmt("%.2f", p[1]) + ", " +
                                fmt("%.2f", p[2]) + ", " + fmt("%.2f", p[3]));
        }
    }
    // the wave each partial oscillator plays
    if (s.adComplex) {
        e.wave = mapWave(s.adShape);
        if (!e.wave.exact) e.notes.push_back("shape " + replaceAll(replaceAll(s.adShape, "Alchemy/Libraries/WaveOsc/", ""), ".raw", "") + " plays as " + e.wave.wave);
    } else e.wave = Wave{"sine", kNone, {}, true};
    e.sym = s.adSym.v;
    e.simple = e.wave.wave == "sine" && e.wave.partials.empty() && std::fabs(e.sym - 0.5) < 1e-4;
    for (int k = 0; k < K; ++k)
        if (amp[(size_t)k] > 1e-9) e.oscs.push_back({amp[(size_t)k], ratio[(size_t)k], pan[(size_t)k]});
    // as sine partials: oscillator k's harmonic m sits at ratio r_k x m (harmonics up to ratio 600)
    e.shape = e.simple ? std::vector<std::complex<double>>{1.0} : cycleSpectrum(shapeCycle(e.wave, e.sym), 256);
    struct Acc { std::complex<double> z; double w = 0, pan = 0; };
    std::map<double, Acc> comp;
    for (auto &[a, rt, pn] : e.oscs)
        for (size_t m = 1; m <= e.shape.size(); ++m) {
            const std::complex<double> c = e.shape[m - 1];
            if (std::abs(c) < 1e-6 || rt * (double)m > 600 + 1e-9) continue;
            Acc &x = comp[r(rt * (double)m, 1e6)];
            x.z += a * c;
            x.w += a * std::abs(c);
            x.pan += a * std::abs(c) * pn;
        }
    double top = 0;
    for (auto &[rt, x] : comp) top = std::max(top, std::abs(x.z));
    if (top <= 0) top = 1;
    for (auto &[rt, x] : comp)
        if (std::abs(x.z) / top >= 1e-3) e.partials.push_back({std::abs(x.z) / top, rt, x.w ? x.pan / x.w : 0.0});   // down to 60 dB under the loudest
    return e;
}

// An .aaz file's partials at rest: their levels averaged over its loop, by harmonic number (the loudest 1). The layout
// (inferred from the bytes of the installed files and the CSV form in Camel's manual, ch. 22: AttackPeakTime, LoopStart,
// LoopEnd, Length, PitchOffset, NumPartials, then per partial StartPhase, Time, Amp, Pitch, Pan), little-endian:
// "AAZ", version 8, a name ("EditorData" = drawn in the editor), u32 0, f32 attack peak, loop start, loop end, length
// (seconds), u32 has-partials, a level envelope and a pitch envelope (u32 n, n f32 times, n f32 values each), u32
// partial count, then per partial f32 start phase, u32 n, n f32 breakpoint times, f32 peak level, f32, u8 coding (0:
// what is read here), n level bytes (0-255 of the peak), ceil(5n / 4) bytes of pitch and pan codes (not read: drawn
// data holds zeros, and with Pitch Variation 0 Alchemy keeps the partials harmonic); coding 1 holds wider codes, 2n or
// 2n + 1 bytes. Version 6 files have the pitch envelope only. Files in other versions (0x11) return false with the
// reason.
struct Aaz { std::map<int, double> levels; bool drawn = false, moving = false; std::string why; };
bool readAaz(const std::string &path, Aaz &out) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t o = 0;
    auto need = [&](size_t n) { if (o + n > b.size()) throw std::runtime_error("it ends early"); };
    auto u32 = [&]() { need(4); uint32_t v; std::memcpy(&v, b.data() + o, 4); o += 4; return v; };
    auto f32 = [&]() { need(4); float v; std::memcpy(&v, b.data() + o, 4); o += 4; return (double)v; };
    try {
        if (b.size() < 8 || std::memcmp(b.data(), "AAZ", 3) != 0) { out.why = "not an .aaz file"; return false; }
        if (b[3] != 8 && b[3] != 6) { out.why = "version " + std::to_string(b[3]); return false; }
        o = 4;
        while (o < b.size() && b[o]) ++o;
        out.drawn = std::string(b.begin() + 4, b.begin() + (long)o) == "EditorData";
        ++o;
        u32();
        f32();
        const double loopStart = f32(), loopEnd = f32(), length = f32();
        if (!u32()) { out.why = "it holds no partials (spectral data only)"; return false; }
        for (int env = 0; env < (b[3] == 6 ? 1 : 2); ++env) { const uint32_t n = u32(); need(8 * (size_t)n); o += 8 * (size_t)n; }   // version 6: no level envelope
        // does a partial record start at `at`? (start phase 0..1, a breakpoint count that fits, its first times rising
        // within the note, then a coding byte 0 or 1)
        auto startsPartial = [&](size_t at) {
            if (at + 8 > b.size()) return false;
            float sp; uint32_t m;
            std::memcpy(&sp, b.data() + at, 4); std::memcpy(&m, b.data() + at + 4, 4);
            if (!(sp >= 0 && sp <= 1.0001f) || m > 200000 || at + 8 + 4 * (size_t)m + 9 > b.size()) return false;
            float prev = 0;
            for (size_t i = 0; i < std::min<size_t>(m, 16); ++i) {
                float t; std::memcpy(&t, b.data() + at + 8 + 4 * i, 4);
                if (!(t >= prev && t <= length + 1)) return false;
                prev = t;
            }
            return b[at + 8 + 4 * (size_t)m + 8] <= 1;
        };
        const uint32_t count = u32();
        std::vector<double> lv;
        for (uint32_t k = 0; k < count; ++k) {
            f32();
            const uint32_t n = u32();
            if (n > 200000) throw std::runtime_error("a partial has " + std::to_string(n) + " breakpoints");
            std::vector<double> times;
            for (uint32_t i = 0; i < n; ++i) times.push_back(f32());
            const double peak = f32();
            f32();
            need(1);
            const uint8_t coding = b[o++];
            if (coding > 1) { out.why = "a partial coding " + std::to_string(coding) + " not read"; return false; }
            // coding 0: ceil(5n / 4) bytes of pitch and pan codes; coding 1 (wide pitch moves): 2n + 1 for an odd n, 2n or
            // 2n + 1 for an even n (where the next partial record starts)
            size_t codes = coding ? 2 * (size_t)n + n % 2 : (5 * (size_t)n + 3) / 4;
            if (coding && n % 2 == 0 && k + 1 < count && !startsPartial(o + n + codes) && startsPartial(o + n + codes + 1)) ++codes;
            need(n + codes);
            const uint8_t *a = b.data() + o;
            o += n + codes;
            double sum = 0, lo = 255, hi = 0, top = 0;
            size_t inLoop = 0;
            for (uint32_t i = 0; i < n; ++i) {
                top = std::max(top, (double)a[i]);
                if (times[i] >= loopStart - 0.002 && times[i] <= loopEnd + 0.002) { sum += a[i]; lo = std::min(lo, (double)a[i]); hi = std::max(hi, (double)a[i]); ++inLoop; }
            }
            if (!inLoop) sum = lo = hi = top, inLoop = 1;
            lv.push_back(peak * sum / (double)inLoop / 255);
            out.moving |= peak * top / 255 > 1e-3 && hi - lo > 25;
        }
        double top = 0;
        for (double x : lv) top = std::max(top, x);
        for (size_t k = 0; k < lv.size(); ++k)
            if (top > 0 && lv[k] / top > 1e-3) out.levels[(int)k + 1] = lv[k] / top;
        if (out.levels.empty()) { out.why = "its partials are silent"; return false; }
        return true;
    } catch (const std::exception &e) {
        out.why = e.what();
        return false;
    }
}

struct Decoder {
    const Preset &P;
    Ctx C;
    std::string name;
    std::vector<std::string> notes;
    // master
    double volumeDb = 0, amp = 0, pervolDb = 0, tune = 0, fine = 0, glide = 0;
    int voices = 8, trigger = 0;
    std::vector<Dyn> ampDyn;
    std::vector<std::string> moving;   // what moves the morph / xfade pad
    std::array<Source, 4> src;
    std::vector<Filt> mains;
    double sendF1 = 0, sendF2 = 0, series = 0;
    std::map<std::string, std::vector<Effect>> racks;
    std::vector<ArpBlock> arps;
    std::vector<Route> routes;
    std::array<AdElement, 4> adds;      // each audible additive source's element
    std::array<std::string, 4> adWhy;   // why one can't play here ("" = it can)
    // An Amp effect an envelope opens plays at its held level, and of several source racks (A-D) only the one carrying
    // the most level (read at rest, such Amps silenced patches like Artificial Persona and stacked racks cut others)
    bool held = true;

    Decoder(const Preset &p, const std::string &n) : P(p), C(p), name(n) { decode(); }

    Filt filterInfo(double code, const Param *cut, const Param *res, const Param *drv, const std::string &where) const {
        Filt f;
        std::tie(f.kind, f.slope) = filterKind(code);
        f.where = where;
        f.code = (int)code;
        const Eff c = C.eff(cut);
        f.cutoff = r(c.v, 10000);
        f.res = r(C.at(res));
        f.drive = drv ? r(C.at(drv)) : 0.0;
        for (auto &d : c.dyn) f.mods.push_back({srcName(d.t, d.i), r(d.d), d.t, d.i});
        return f;
    }

    void decode() {
        // master: volume, amp, the Perform section's volume, tuning, voices, trigger mode, glide
        volumeDb = r(linDb(C.at(P.get("master", "Volume"))), 100);
        const Eff a = C.eff(P.get("master", "Amp"));
        amp = r(a.v);
        ampDyn = a.dyn;
        double pervol = 0.794;
        for (auto &q : P.sec("perform")) if (q.key == "PerVol") { pervol = q.has ? q.value : 0.794; break; }
        pervolDb = r(linDb(pervol), 100);
        tune = r(semis(C.at(P.get("master", "TuneCrs"))), 100);
        fine = r(cents(C.at(P.get("master", "TuneFine"))), 10);
        voices = (int)P.val("master", "NmVoices", 8);
        trigger = (int)P.val("master", "PlayMode", 0);
        glide = r(C.at(P.get("master", "Porto")), 10000);
        // the morph / xfade pad's weights: the corners (inferred, the only reading that leaves no factory patch silent)
        // A top-left, B top-right, C bottom-left, D bottom-right; linear modes run A -> B -> C -> D along X
        const int mode = (int)P.val("morph", "MorAll", 2);
        const Eff X = C.eff(P.get("morph", "MorAllX")), Y = C.eff(P.get("morph", "MorAllY"));
        for (auto *e : {&X, &Y}) for (auto &d : e->dyn) moving.push_back(srcName(d.t, d.i));
        std::array<double, 4> w{};
        if (mode >= 2 && mode <= 5) w = {(1 - X.v) * Y.v, X.v * Y.v, (1 - X.v) * (1 - Y.v), X.v * (1 - Y.v)};
        else for (int k = 0; k < 4; ++k) w[(size_t)k] = std::max(0.0, 1 - std::fabs(X.v * 3 - k));
        for (int k = 0; k < 4; ++k) if (!(int)P.val("morph", "S" + std::to_string(k + 1) + "MorOn", 1)) w[(size_t)k] = 1.0;
        // the sources A-D and their elements
        size_t zi = 0;
        static const std::map<int, std::pair<const char *, const char *>> routeOf = {{0, {"F1", "F2"}}, {1, {"F1", "FX A"}}, {2, {"F2", "FX A"}},
            {3, {"F1", "FX B"}}, {4, {"F2", "FX B"}}, {5, {"F1", "FX C"}}, {6, {"F2", "FX C"}}, {7, {"F1", "FX D"}}, {8, {"F2", "FX D"}}};
        for (size_t si = 0; si < 4; ++si) {
            const auto d = keyed(P.sec("source", si));
            auto E = [&](const char *k) { return C.eff(at(d, k)); };
            Source &s = src[si];
            s.letter = (char)('A' + si);
            const size_t nz = at(d, "ZnNum") ? (size_t)std::max(0.0, numOf(at(d, "ZnNum"))) : 1;
            std::vector<const std::vector<Param> *> zones;
            for (size_t k = zi; k < zi + nz && k < P.count("zone"); ++k) zones.push_back(&P.sec("zone", k));
            zi += nz;
            s.on = is1(at(d, "SOn"));
            s.weight = r(w[si]);
            const Eff sa = E("SAmp");
            double sv = sa.v;
            if (sv < 0.01)   // an amp that only opens under an envelope or LFO
                for (auto &x : sa.dyn) if (x.t >= 1 && x.t <= 4 && x.d > 0) sv = std::max(sv, x.d);
            s.ampDb = r(linDb(sv), 100);
            s.tune = r(semis(E("STunCrs").v), 100);
            s.fine = r(cents(E("STunFin").v), 10);
            s.pan = r(E("SPan").v * 2 - 1);
            s.keytrack = (int)numOf(at(d, "SKeyTrk"), 2);
            if (is1(at(d, "SVAOn"))) {
                s.va = true;
                const std::string raw = at(d, "SVAShpe") ? at(d, "SVAShpe")->raw : "";
                s.wave = mapWave(raw);
                s.vaShape = replaceAll(raw, "Alchemy/Libraries/WaveOsc/", "");
                s.vaVolDb = r(linDb(E("SVAVol").v), 100);
                s.sym = r(E("SVASym").v);
                s.sync = r(E("SVASync").v, 10000);
                s.nosc = unisonCount(E("SVANOsc").v);
                s.detune = r(E("SVAUnis").v);
                s.wide = P.val("extensions", std::string(1, s.letter) + "-VAWideUnison") == 1;
            }
            if (is1(at(d, "SNsOn")) && E("SNsLp").v > E("SNsHp").v + 0.05) {
                s.noise = true;
                const std::string raw = at(d, "SNsShpe") ? at(d, "SNsShpe")->raw : "";
                s.noiseShape = replaceAll(raw.substr(raw.rfind('/') == std::string::npos ? 0 : raw.rfind('/') + 1), ".wav", "");
                s.noiseVolDb = r(linDb(E("SNsVol").v), 100);
                s.lowcut = r(E("SNsHp").v);
                s.highcut = r(E("SNsLp").v);
            }
            if (is1(at(d, "SAdOn"))) {
                s.additive = true;
                s.analysis = is1(at(d, "SAdMode"));
                s.adComplex = is1(at(d, "SAdShMd"));
                s.adShape = at(d, "SAdShpe") ? at(d, "SAdShpe")->raw : "";
                s.adNosc = E("SAdNOsc");
                s.adSym = E("SAdSym");
                s.adVol = E("SAdVol");
                s.adPvar = E("SAdPVar").v;
                for (int k = 0; k < 3; ++k) {
                    auto &u = s.adUnits[(size_t)k];
                    u.on = is1(at(d, "SAdE" + std::to_string(k + 1) + "On"));
                    u.type = (int)numOf(at(d, "SAdEfT" + std::to_string(k + 1)));
                    for (int j = 0; j < 4; ++j) u.p[(size_t)j] = E(("SAdEP" + std::to_string(4 * k + j + 1)).c_str());
                }
            }
            s.stereo = is1(at(d, "SStereo"));
            s.formant = is1(at(d, "SFmtAOn"));
            s.fmtSynth = is1(at(d, "SFmtSOn"));
            s.fmtSelect = E("SFmtSSe").v;
            s.fmtShift = E("SFmtSSh").v;
            for (int k = 0; k < 4; ++k) {
                const Param *f = at(d, ("SFmtSS" + std::to_string(k + 1)).c_str());
                s.fmtSlots[(size_t)k] = f ? f->raw : "";
            }
            if (is1(at(d, "SSpOn"))) {
                s.spectral = true;
                s.spNoise = is1(at(d, "SSpMode"));
                s.spVolDb = r(linDb(E("SSpVol").v), 100);
                s.spLowcut = r(E("SSpHp").v);
                s.spHighcut = r(E("SSpLp").v);
            }
            if (is1(at(d, "SGrOn"))) {
                (is1(at(d, "SGrMode")) ? s.sampler : s.granular) = true;
                s.sampVolDb = r(linDb(E(s.sampler ? "SSampVl" : "SGrVol").v), 100);
                s.position = r(E("SPos").v);
                s.reverse = is1(at(d, "SGrSRev"));
                s.grSpeed = E("SStrtch");
                s.grSize = E("SGrSize");
                s.grRand = E("SGrRTim");
                s.pos = E("SPos");
            }
            // its content: each zone's file (found where the preset says, else by name), keys, level, tuning, loop
            for (auto *z : zones) {
                const auto zd = keyed(*z);
                const std::string f = at(zd, "ZnFile") ? at(zd, "ZnFile")->raw : "";
                if (f.empty()) continue;
                AZone x;
                x.file = f;
                x.key = (int)numOf(at(zd, "ZnKey")); x.lokey = (int)numOf(at(zd, "ZnLoKey")); x.hikey = (int)numOf(at(zd, "ZnHiKey"));
                x.lovel = (int)numOf(at(zd, "ZnLoVel")); x.hivel = (int)numOf(at(zd, "ZnHiVel"));
                x.volDb = r(linDb(numOf(at(zd, "ZnVolume"))), 100);
                x.tune = r(semis(numOf(at(zd, "ZnTunCrs"))), 100);
                x.fine = r(cents(numOf(at(zd, "ZnTunFin"))), 10);
                x.pan = r(numOf(at(zd, "ZnPan")) * 2 - 1);
                x.loopMode = (int)numOf(at(zd, "ZnLoopMd"));
                const Param *mk = at(zd, "ZnSlceMk");
                if (mk && !mk->matrix.empty() && !mk->matrix[0].empty()) {   // slice markers in seconds: the loop points and the end
                    const auto &marks = mk->matrix[0];
                    const long ls = (long)numOf(at(zd, "ZnLoopS")), le = (long)numOf(at(zd, "ZnLoopE"));
                    if (ls >= 0 && le >= 0 && (size_t)ls < marks.size() && (size_t)le < marks.size()) { x.loop = true; x.loopStart = marks[(size_t)ls]; x.loopEnd = marks[(size_t)le]; }
                    x.hasEnd = true;
                    x.end = marks.back();
                }
                s.zones.push_back(x);
            }
            if (at(d, "SFile") && !at(d, "SFile")->raw.empty() && s.zones.empty()) {
                AZone x;
                x.file = at(d, "SFile")->raw;
                x.volDb = -4.0;
                s.zones.push_back(x);
            }
            if (s.content()) for (auto &z : s.zones) z.found = resolveFile(z.file);
            for (int k = 1; k <= 3; ++k) {
                const std::string K = "SF" + std::to_string(k);
                if (is1(at(d, K + "On")))
                    s.filters.push_back(filterInfo(numOf(at(d, K + "Type")), at(d, K + "Cut"), at(d, K + "Res"), at(d, K + "Drv"),
                                                   std::string(1, s.letter) + " filter " + std::to_string(k)));
            }
            s.parallel = is1(at(d, "SFiPar"));
            const Param *rot = at(d, std::string("S") + s.letter + "EfxRot");
            auto it = routeOf.find(rot ? (int)numOf(rot) : 0);
            if (it != routeOf.end()) s.first = it->second.first, s.second = it->second.second;
            s.send = r(E("SFilMix").v);
            for (const char *k : {"SAmp", "STunCrs", "STunFin", "SPan", "SVASym", "SVASync", "SVANOsc", "SVAVol", "SNsVol", "SAdVol"})
                if (at(d, k)) s.dyn[k] = E(k).dyn;
            const bool sounds = s.va || s.noise || s.additive || s.spectral || s.granular || s.sampler;
            s.audible = s.on && s.weight >= 0.05 && sv >= 0.01 && sounds;
        }
        // the two main filters, their effects sends (to the Main rack or A-D) and the series amount
        static const char *rackNames[5] = {"Main", "A", "B", "C", "D"};
        for (int k = 1; k <= 2; ++k) {
            const std::string K = "F" + std::to_string(k);
            if (P.val("filters", K + "On") != 1) continue;
            Filt f = filterInfo(P.val("filters", K + "Type", 0), P.get("filters", K + "Cut"), P.get("filters", K + "Res"), P.get("filters", K + "Drive"),
                                "main filter " + std::to_string(k));
            f.fxSend = r(C.at(P.get("filters", K + "FxMix")));
            f.fxRack = rackNames[(((int)P.val("filters", K + "EfxRot", 0) % 5) + 5) % 5];
            f.slot = k;
            mains.push_back(f);
        }
        sendF1 = r(C.at(P.get("filters", "F1FxMix")));
        sendF2 = r(C.at(P.get("filters", "F2FxMix")));
        series = r(C.at(P.get("filters", "FSerMix")));
        // the effects racks (Main, A-D): EffectRack, EffectSlot, EffectType, then the effect's parameters; the
        // section's "Slt..On" flags, 16 a rack, switch slots on
        const auto &fxs = P.sec("effects");
        std::vector<double> flags;
        for (auto &p : fxs)
            if (p.key.size() >= 5 && p.key.compare(0, 3, "Slt") == 0 && p.key.compare(p.key.size() - 2, 2, "On") == 0) flags.push_back(p.has ? p.value : 0.0);
        Effect cur;                  // the effect being read: rack and slot come first, then its type, then its parameters
        Effect *placed = nullptr;    // once typed, where it went
        bool have = false;
        for (auto &p : fxs) {
            if (p.key == "EffectRack") { cur = Effect{}; cur.rack = (int)numOf(&p); have = true; placed = nullptr; }
            else if (p.key == "EffectSlot" && have) (placed ? placed->slot : cur.slot) = (int)numOf(&p);
            else if (p.key == "EffectType" && have) {
                Effect e = placed ? *placed : cur;
                e.type = (int)numOf(&p);
                e.name = effectName(e.type);
                const int fi = e.rack * 16 + e.slot;
                e.on = fi >= 0 && (size_t)fi < flags.size() ? flags[(size_t)fi] != 0 : true;
                auto &list = racks[e.rack >= 0 && e.rack < 5 ? rackNames[e.rack] : "?"];
                list.push_back(e);
                placed = &list.back();
            } else if (placed && p.key != "ExtraData" && p.key != "AutomatableParams" && p.key != "NonAutomatableParams" &&
                       p.key.rfind("Legacy", 0) != 0 && p.key.rfind("EfxMast", 0) != 0) {
                if (!p.has) { placed->params.erase(p.key); continue; }   // text: as good as unset
                const Eff e = p.mods.empty() ? Eff{p.value, {}} : C.eff(&p);
                placed->params[p.key] = r(e.v, 10000);
                if (!e.dyn.empty()) {
                    auto &L = placed->dyn[p.key];
                    L.clear();
                    for (auto &x : e.dyn) L.push_back({srcName(x.t, x.i), r(x.d)});
                }
            }
        }
        // the arpeggiator: five blocks (All, A-D); Mode is a modulation target (0..1 = off + 7 orders)
        std::map<std::string, std::vector<const Param *>> blocks;
        for (auto &p : P.sec("arp")) blocks[p.key].push_back(&p);
        auto blk = [&](const char *k, size_t b) -> const Param * { auto it = blocks.find(k); return it == blocks.end() || b >= it->second.size() ? nullptr : it->second[b]; };
        static const char *orders[8] = {"", "up", "down", "updown", "downup", "played", "random", "chord"};
        for (size_t b = 0; b < (blocks.count("ArpMode") ? blocks["ArpMode"].size() : 0); ++b) {
            const int order = ri(C.at(blk("ArpMode", b)) * 7);
            if (!order) continue;
            ArpBlock x;
            x.order = order >= 1 && order <= 7 ? orders[order] : "up";
            x.free = !is1(blk("ArpSync", b));
            // a synced rate at rest, with the Perform knobs that move it (as every other arp setting is read)
            const double rate = x.free ? numOf(blk("ArpRate", b)) : C.at(blk("ArpRate", b));
            x.rate = x.free ? fmt("%.3f", rate) : syncNote(rate);
            x.octaves = 1 + ri(C.at(blk("ArpOct", b)) * 3);
            x.gate = r(C.at(blk("ArpSustn", b)));
            arps.push_back(x);
        }
        // the moving modulations, as (source, section.key, depth)
        static const std::set<std::string> skip = {"header", "perform", "modmap", "zone", "mseg", "seq", "waveshaperenv"};
        for (auto &sec : P.sections) {
            if (skip.count(sec.name)) continue;
            for (auto &p : sec.params)
                if (!p.mods.empty())
                    for (auto &x : C.eff(&p).dyn) routes.push_back({srcName(x.t, x.i), sec.name + "." + p.key, r(x.d)});
        }
        for (size_t si = 0; si < 4; ++si)
            if (src[si].audible && src[si].additive) additive(si);
    }

    // an audible additive source's element, its .aaz data read when it has some, and why it can't play here. The effect
    // units act on the data's partials by their harmonic numbers (Pitch Variation 0 or drawn data keeps them harmonic, so
    // pitch correction changes nothing either); its synthesized formant filter becomes EQ peaks after the voices.
    void additive(size_t si) {
        const Source &s = src[si];
        const std::string L(1, s.letter);
        std::string &why = adWhy[si];
        Aaz data;
        std::string file;
        if (s.analysis) {
            const AZone *z = nullptr;
            for (auto &x : s.zones) if (!x.found.empty()) { z = &x; break; }
            if (!z) { if (s.zones.empty()) why = "its additive source " + L + " names no analysis data, so only GarageBand and Logic can play it"; return; }
            file = fs::u8path(z->found).filename().u8string();
            if (!readAaz(z->found, data)) {
                why = "its additive source " + L + " plays analysis data (\"" + file + "\") in a form not read here (" + data.why + "), so only GarageBand and Logic play it";
                return;
            }
        }
        AdElement &e = adds[si] = decodeAdditive(s, s.analysis ? &data.levels : nullptr);
        if (s.analysis) {
            e.notes.insert(e.notes.begin(), std::string(data.drawn ? "partials drawn in" : "partials analysed from a sample in") + " \"" + file + "\"" +
                                                (data.moving ? ": their levels move over time, played as their loop's average" : ""));
            if (s.zones.size() > 1) e.notes.push_back("its other " + std::to_string(s.zones.size() - 1) + " analysis zones play as the first");
            if (!data.drawn && s.adPvar > 0.005) e.notes.push_back("Pitch Variation " + fmt("%.2f", s.adPvar) + ": the analysed pitch drift is not played");
        }
        if (s.fmtSynth) formantEq(s, e);
        if (s.formant && s.analysis) why = "its additive source " + L + " goes through Alchemy's analysed formant filter, which isn't decoded, so only GarageBand and Logic play it";
        else if (!e.unknown.empty())
            why = "its additive source " + L + " has an effect unit that isn't decoded (" + e.unknown[0] + "), so only GarageBand and Logic play it";
    }

    // The synthesized formant filter's vowel slots as EQ peaks after the voices: F1-F3 of the vowel (Peterson & Barney's
    // averages for a male voice, public phonetics data, not Alchemy's tables), Select crossfading the two slots around it
    // (1 + 3 x Select), Shift +-24 semitones (a guess); Size and Center are not played. Off slots pass; comb and
    // parallel slots have no counterpart here and are noted.
    void formantEq(const Source &s, AdElement &e) const {
        static const std::map<char, std::array<double, 3>> F = {{'A', {730, 1090, 2440}}, {'E', {530, 1840, 2480}}, {'I', {270, 2290, 3010}},
                                                                {'O', {570, 840, 2410}}, {'U', {300, 870, 2240}}};
        const double pos = 3 * std::min(1.0, std::max(0.0, s.fmtSelect));
        const int lo = std::min(2, (int)pos);
        const double frac = pos - lo, shift = std::pow(2.0, 48 * (s.fmtShift - 0.5) / 12);
        json bands = json::array();
        std::string names;
        for (int k = lo; k <= lo + 1; ++k) {
            const double w = k == lo ? 1 - frac : frac;
            if (w < 0.02) continue;
            std::string slot = s.fmtSlots[(size_t)k];
            slot = slot.substr(slot.rfind('/') == std::string::npos ? 0 : slot.rfind('/') + 1);
            if (slot == "Off.csv") continue;
            if (slot.rfind("Vowel", 0) != 0 || slot.size() < 10 || !F.count(slot[7])) { e.unexpressed.push_back("formant filter " + slot.substr(0, slot.size() - 4)); continue; }
            const auto &f = F.at(slot[7]);
            const double gains[3] = {10, 8, 5}, qs[3] = {4, 5, 6};
            for (int j = 0; j < 3; ++j) bands.push_back({{"type", "peak"}, {"freq", r(std::min(18000.0, f[(size_t)j] * shift), 1)}, {"gain", r(gains[j] * w, 10)}, {"q", qs[j]}});
            names += (names.empty() ? "" : " and ") + slot.substr(0, slot.size() - 4);
        }
        if (!bands.empty()) {
            e.formant = bands;
            e.notes.push_back("formant filter " + names + " as EQ peaks at the vowel's formants (Size and Center not played)");
        }
    }

    // ---- what reaches what
    std::vector<const Source *> audible() const {
        std::vector<const Source *> out;
        for (auto &s : src) if (s.audible) out.push_back(&s);
        return out;
    }
    // the main filters the audible sources reach and the effects racks they feed
    void path(std::vector<const Filt *> &mf, std::set<std::string> &rk) const {
        std::set<std::string> toF;
        for (auto *s : audible()) {
            if (s->send < 0.99) toF.insert(s->first);
            if (s->send > 0.01) {
                if (s->second == "F1" || s->second == "F2") toF.insert(s->second);
                else rk.insert(s->second.substr(s->second.size() - 1));   // "FX A" -> rack A
            }
        }
        for (auto &f : mains)
            if (toF.count("F" + std::to_string(f.slot)) || (f.slot == 2 && toF.count("F1") && series > 0.01)) {
                mf.push_back(&f);
                if (f.fxSend > 0.01) rk.insert(f.fxRack);
            }
        bool other = false;
        for (auto &x : rk) other |= x != "Main";
        if (other) rk.insert("Main");   // racks A-D feed the Main rack (inferred)
    }
    double filterSend(const std::string &dest) const { return dest == "F1" ? sendF1 : dest == "F2" ? sendF2 : 1.0; }
    // the share of the (level-weighted) signal that reaches the effects racks; the rest goes dry to the output
    double fxFraction() const {
        double tot = 0, acc = 0;
        for (auto *s : audible()) {
            const double w = std::pow(10.0, s->ampDb / 20) * s->weight;
            double frac = 0;
            frac += (1 - s->send) * filterSend(s->first);
            frac += s->send * filterSend(s->second);
            tot += w;
            acc += w * frac;
        }
        return tot != 0 ? acc / tot : 1.0;
    }

    // ---- classes: the reason a patch can't play here, else "" (and whether it plays samples)
    std::string refusal(bool &samples, std::string &what) const {
        samples = false;
        const auto srcs = audible();
        if (srcs.empty()) { what = "silent"; return "none of its Alchemy sources sounds with the controls at rest, so only GarageBand and Logic can play it"; }
        std::set<std::string> missing;
        std::string example;
        bool analysis = false, scratch = false, spectral = false, granular = false;
        for (auto *s : srcs) {
            if (s->content())
                for (auto &z : s->zones)
                    if (z.found.empty() && missing.insert(z.file).second && example.empty()) example = fs::u8path(z.file).filename().u8string();
            if (s->additive) (s->analysis ? analysis : scratch) = true;
            spectral |= s->spectral;
            granular |= s->granular;
            samples |= s->sampler;
        }
        std::vector<std::string> kinds;
        if (analysis) kinds.push_back("additive resynthesis");
        if (scratch) kinds.push_back("additive synthesis from drawn partials");
        if (spectral) kinds.push_back("spectral synthesis");
        if (granular) kinds.push_back("granular synthesis");
        std::string synthesis;
        for (size_t k = 0; k < kinds.size(); ++k) synthesis += (k ? k + 1 == kinds.size() ? " and " : ", " : "") + kinds[k];
        if (!missing.empty()) {
            what = "content not installed";
            const std::string n = std::to_string(missing.size()) + " of its Alchemy content files " + (missing.size() == 1 ? "isn't" : "aren't") +
                                  " installed here (such as \"" + example + "\")";
            return synthesis.empty() ? n + ": GarageBand's Sound Library has them" : n + " and it plays them with " + synthesis + ", which only GarageBand and Logic play";
        }
        bool pitched = false;   // a spectral element in Pitch mode, or one without additive data beside it (Noise mode beside it plays as noise)
        for (auto *s : srcs) pitched |= s->spectral && (!s->spNoise || !s->additive || !s->analysis);
        if (spectral && pitched) {
            what = kinds[0].substr(0, kinds[0].find(' '));
            return "its sounds come from Alchemy's " + synthesis + ", which only GarageBand and Logic play";
        }
        // additive sources play on builtin:synth, granular ones as samples on builtin:sampler: one or the other
        const bool additive = analysis || scratch;
        bool layerable = additive;   // each additive element decodes and has no Hz shift: its partials can be SFZ *sine regions
        for (auto *s : srcs) layerable &= !s->additive || (adWhy[(size_t)(s->letter - 'A')].empty() && adds[(size_t)(s->letter - 'A')].shiftHz == 0);
        if (additive && (samples || granular) && layerable && granularWhy().empty()) {
            samples = true;
            what = "samples";
            return "";
        }
        if (additive && (samples || granular)) {
            what = "additive";
            return "it layers Alchemy's additive synthesis with samples: builtin:synth plays the one and builtin:sampler the other, not both in one patch, "
                   "so only GarageBand and Logic play it";
        }
        if (granular) {
            const std::string why = granularWhy();
            what = "granular";
            samples = why.empty();
            return why;
        }
        for (size_t si = 0; si < 4; ++si)
            if (src[si].audible && src[si].additive && !adWhy[si].empty()) { what = "additive"; return adWhy[si]; }
        what = additive ? "additive" : samples ? "samples" : "virtual analog";
        return "";
    }

    // Granular sources play as their samples when the grains stand still: at Speed 100% (0.5) the zone itself, frozen
    // (Speed 0) a short loop at Position, a slow scan the zone at its own rate. Moving Position or grain size, Speed
    // above 100%, grains of a few milliseconds and random grain positions need a grain engine: the reason, else "".
    std::string granularWhy() const {
        for (auto *s : audible()) {
            if (!s->granular) continue;
            std::vector<std::string> issues;
            // a moving position or grain size, and random grain starts, play at rest (toSampler notes them); a scan above
            // 100% and grains of a few milliseconds have no stand-in
            if (s->grSpeed.v > 0.75) issues.push_back("scans its sample faster than 100% (Speed " + fmt("%.2f", s->grSpeed.v) + ")");
            if (s->grSize.v < 0.05) issues.push_back("plays grains of a few milliseconds (a buzz)");
            if (issues.empty()) continue;
            std::string l;
            for (size_t k = 0; k < issues.size(); ++k) l += (k ? k + 1 == issues.size() ? " and " : ", " : "") + issues[k];
            return std::string("its granular source ") + s->letter + " " + l + ", which needs Alchemy's grain engine: only GarageBand and Logic play it";
        }
        return "";
    }

    // ---- envelopes and LFOs
    struct Adsr { bool ok = false; double attack = 0, decay = 0, sustain = 0, release = 0, hold = 0; };
    Adsr ahdsr(int i) const {
        Adsr e;
        if (i < 0 || (size_t)i >= P.count("ahdsr")) return e;
        const auto d = keyed(P.sec("ahdsr", (size_t)i));
        auto E = [&](const char *k) { return at(d, k) ? C.at(at(d, k)) : 0.0; };
        e.ok = true;
        e.attack = r(envTime(E("AhdAttck")), 10000);
        e.decay = r(envTime(E("AhdDecay")), 10000);
        e.sustain = r(E("AhdSustn"));
        e.release = r(envTime(E("AhdRelse")), 10000);
        e.hold = envTime(E("AhdHold"));
        return e;
    }
    json adsrJson(const Adsr &e) const { return {{"attack", e.attack}, {"decay", e.decay}, {"sustain", e.sustain}, {"release", e.release}}; }
    // the AHDSR on Master Amp, else on the loudest audible source's Amp; which = "master" or the source's letter
    Adsr ampEnvelope(std::string &which) const {
        int idx = -1;
        which = "master";
        for (auto &x : ampDyn) if (x.t == 2) { idx = x.i; break; }
        if (idx < 0) {
            auto srcs = audible();
            std::stable_sort(srcs.begin(), srcs.end(), [](const Source *a, const Source *b) { return a->weight > b->weight; });
            for (auto *s : srcs) {
                auto it = s->dyn.find("SAmp");
                if (it != s->dyn.end()) for (auto &x : it->second) if (x.t == 2) { idx = x.i; break; }
                if (idx >= 0) { which = std::string(1, s->letter); break; }
            }
        }
        if (idx < 0) { which.clear(); return Adsr{}; }
        return ahdsr(idx);
    }
    json lfoSpec(int i, double depth, const std::string &to) {
        if (i < 0 || (size_t)i >= P.count("lfo")) return nullptr;
        const auto d = keyed(P.sec("lfo", (size_t)i));
        const std::string L = "LFO " + std::to_string(i + 1);
        const std::string path = replaceAll(replaceAll(at(d, "LfoShape") ? at(d, "LfoShape")->raw : "", "Alchemy/Libraries/WaveLfo/", ""), ".raw", "");
        std::string shape = lfoShape(path);
        if (shape.empty()) { notes.push_back(L + ": shape " + path + " plays as a sine"); shape = "sine"; }
        const double rv = C.at(at(d, "LfoRate"));
        json spec = {{"rate", is1(at(d, "LfoSync")) ? json(syncNote(rv)) : json(r(std::max(0.01, lfoHz(rv))))}, {"depth", r(depth * 0.5, 10000)},
                     {"shape", shape}, {"to", to}};
        if (!is1(at(d, "LfoBiPol"))) notes.push_back(L + " is unipolar: plays bipolar around the middle of its range (cutoff base raised to match)");
        const double dl = envTime(C.at(at(d, "LfoDelay"))), fa = envTime(C.at(at(d, "LfoAttck")));
        if (dl > 0.001) spec["delay"] = r(dl);
        if (fa > 0.001) spec["fade"] = r(fa);
        if (is1(at(d, "LfoTrig"))) notes.push_back(L + " restarts with each note in Alchemy; builtin:synth LFOs run free");
        return spec;
    }

    // ---- the one per-voice filter builtin:synth gets: the one covering most of the level, then the most modulated;
    // others covering half the level play after the voices, the rest are left out
    const Filt *pickFilter(const std::vector<const Filt *> &mf, std::vector<const Filt *> &rest, std::vector<const Filt *> &special, std::vector<std::string> &skipped) const {
        const auto srcs = audible();
        std::map<char, double> lv;
        double tot = 0;
        for (auto *s : srcs) { lv[s->letter] = std::pow(10.0, s->ampDb / 20) * s->weight; }
        for (auto *s : srcs) tot += lv[s->letter];
        if (tot == 0) tot = 1.0;
        double share1 = 0, share2 = 0;
        for (auto *s : srcs)
            for (auto [dest, part] : {std::pair<std::string, double>{s->first, 1 - s->send}, {s->second, s->send}}) {
                if (dest == "F1") share1 += part * lv[s->letter] / tot;
                else if (dest == "F2") share2 += part * lv[s->letter] / tot;
            }
        share2 += share1 * series;
        struct Cand { const Filt *f; double cov; bool par; };
        std::vector<Cand> cands;
        for (auto *f : mf) cands.push_back({f, std::min(1.0, f->slot == 1 ? share1 : share2), false});
        for (auto *s : srcs) for (auto &f : s->filters) cands.push_back({&f, lv[s->letter] / tot, s->parallel});
        auto transparent = [](const Filt *f, bool par) {
            if (!f->mods.empty()) return false;
            return (f->kind == "lowpass" && f->cutoff >= 0.99) || (f->kind == "highpass" && f->cutoff <= 0.01) || (par && f->kind == "lowpass" && f->cutoff <= 0.02);
        };
        auto linear = [](const Filt *f) { return f->kind == "lowpass" || f->kind == "highpass" || f->kind == "bandpass"; };
        std::vector<Cand> lin;
        for (auto &c : cands) {
            if (!linear(c.f)) special.push_back(c.f);
            else if (!transparent(c.f, c.par)) lin.push_back(c);
        }
        if (lin.empty()) return nullptr;
        auto dyn = [](const Filt *f) { int n = 0; for (auto &m : f->mods) n += (m.t >= 1 && m.t <= 4) || (m.t == 5 && m.i == 0); return n; };
        auto kt = [](const Filt *f) { for (auto &m : f->mods) if (m.t == 5 && (m.i == 1 || m.i == 2)) return true; return false; };
        auto key = [&](const Cand &c) { return std::make_tuple(c.cov >= 0.5, dyn(c.f), kt(c.f), c.cov, c.f->kind == "lowpass", c.par ? c.f->cutoff : -c.f->cutoff); };
        std::stable_sort(lin.begin(), lin.end(), [&](const Cand &a, const Cand &b) { return key(a) > key(b); });
        for (size_t k = 1; k < lin.size(); ++k) {
            if (lin[k].cov >= 0.5) rest.push_back(lin[k].f);
            else skipped.push_back(lin[k].f->where);
        }
        return lin[0].f;
    }

    // ---- the moving modulations builtin:synth can't play, for the notes
    std::vector<std::string> unmapped(bool minor, bool additive = false) const {
        static const std::map<std::string, std::set<std::string>> mappable = {
            {"master.Amp", {"ahdsr", "velocity", "lfo"}}, {"source.SAmp", {"ahdsr", "lfo", "velocity"}}, {"master.Volume", {"velocity", "lfo"}},
            {"master.TuneFine", {"lfo", "ahdsr", "mseg"}}, {"master.TuneCrs", {"lfo", "ahdsr", "mseg"}}, {"source.STunFin", {"lfo", "ahdsr", "mseg"}},
            {"source.STunCrs", {"lfo", "ahdsr", "mseg"}}, {"source.SVASym", {"lfo"}}, {"source.SPan", {"lfo", "random"}}, {"master.Pan", {"lfo"}},
            {"filters.F1Cut", {"ahdsr", "lfo", "velocity", "keyfollow", "pitchbend"}}, {"filters.F2Cut", {"ahdsr", "lfo", "velocity", "keyfollow", "pitchbend"}},
            {"source.SF1Cut", {"ahdsr", "lfo", "velocity", "keyfollow", "pitchbend"}}, {"source.SF2Cut", {"ahdsr", "lfo", "velocity", "keyfollow", "pitchbend"}},
            {"source.SF3Cut", {"ahdsr", "lfo", "velocity", "keyfollow", "pitchbend"}}, {"extensions.A-NsTun", {"keyfollow", "pitchbend"}},
            {"extensions.B-NsTun", {"keyfollow", "pitchbend"}}, {"extensions.C-NsTun", {"keyfollow", "pitchbend"}}, {"extensions.D-NsTun", {"keyfollow", "pitchbend"}}};
        static const std::set<std::pair<std::string, std::string>> second = {   // level tilts and per-note drift: audible but second order
            {"keyfollow", "master.Amp"}, {"keyfollow", "source.SAmp"}, {"random", "source.STunFin"}, {"random", "source.SPan"},
            {"velocity", "source.SNsVol"}, {"velocity", "source.SVAVol"}, {"velocity", "source.SAmp"}, {"keyfollow", "source.SNsHp"}, {"keyfollow", "source.SNsLp"}};
        std::set<std::string> out;
        for (auto &rt : routes) {
            std::string kind = rt.src;
            while (!kind.empty() && kind.back() >= '0' && kind.back() <= '9') kind.pop_back();
            if (kind == "keyfollow-glide") kind = "keyfollow";
            bool ignorable = false;
            for (const char *x : {"aftertouch", "pitchbend", "midi", "perform", "arp", "speed", "held", "polyphony"}) ignorable |= kind.rfind(x, 0) == 0;
            auto starts = [&](const char *x) { return rt.tgt.rfind(x, 0) == 0; };
            if (ignorable || std::fabs(rt.d) < 0.02 || (rt.tgt.size() >= 6 && rt.tgt.compare(rt.tgt.size() - 6, 6, ".Depth") == 0) || starts("arp.") ||
                starts("effects.") || starts("ahdsr.") || starts("lfo.") || starts("morph."))
                continue;
            auto it = mappable.find(rt.tgt);
            if (it != mappable.end() && (it->second.count(kind) || (kind.rfind("random", 0) == 0 && it->second.count("random")))) continue;
            if (kind == "ahdsr" && (rt.tgt == "source.SNsVol" || rt.tgt == "source.SVAVol" || rt.tgt == "source.SAmp" || (additive && rt.tgt == "source.SAdVol")) && rt.d > 0) {
                const Adsr e = ahdsr(std::atoi(rt.src.c_str() + 5) - 1);
                if (e.ok && e.sustain < 0.05) continue;   // a decaying level: the oscillator's own decay
            }
            if (minor && second.count({kind, rt.tgt})) continue;
            out.insert(rt.src + "->" + rt.tgt);
        }
        return {out.begin(), out.end()};
    }

    // ---- the effects
    json effectJson(const Effect &e) {
        auto g = [&](const char *k, double def = 0) { return e.g(k, def); };
        std::string moving;
        for (auto &[k, v] : e.dyn) moving += (moving.empty() ? "" : ", ") + k;
        // an Amp whose level an envelope opens plays at the level it holds while a note is held, or at its peak when the
        // envelope decays to nothing (held: see `held`)
        double heldAmp = g("AmpAmp", 0.5);
        bool envAmp = false, decays = false;
        if (held && e.type == 10 && e.dyn.count("AmpAmp") && e.dyn.size() == 1) {
            envAmp = true;
            for (auto &[src, d] : e.dyn.at("AmpAmp")) {
                if (src.rfind("ahdsr", 0) != 0) { envAmp = false; continue; }
                const Adsr x = ahdsr(std::atoi(src.c_str() + 5) - 1);
                if (x.ok) heldAmp += d * (x.sustain < 0.05 ? 1.0 : x.sustain), decays |= x.sustain < 0.05;
            }
            heldAmp = std::min(1.0, std::max(0.0, heldAmp));
            if (envAmp) notes.push_back(decays ? "Amp: its level follows a decaying envelope, played at its peak" : "Amp: its level follows an envelope, played where it holds");
        }
        if (!moving.empty() && !envAmp) {
            bool levelsOnly = true, open = false;
            for (auto &[k, v] : e.dyn) levelsOnly &= k.size() > 3 && (k.compare(k.size() - 3, 3, "Mix") == 0 || k.compare(k.size() - 3, 3, "Wet") == 0);
            for (auto &[k, v] : e.params) open |= k.size() > 3 && (k.compare(k.size() - 3, 3, "Mix") == 0 || k.compare(k.size() - 3, 3, "Wet") == 0) && v > 0.005;
            if (!levelsOnly || open) notes.push_back(e.name + ": " + moving + " move under modulation, played at rest");
        }
        const int t = e.type;
        if (t == 1) {   // Delay
            const double mix = g("DelMix");
            if (mix < 0.005) return nullptr;
            json j = {{"type", "delay"}, {"feedback", r(std::min(0.95, (g("DelFback") + g("DelFbaR")) / 2))}, {"mix", r(mix)}, {"pingpong", g("DelCross") > 0.5}};
            if (g("DelSync") == 1) {
                const std::string n = syncNote(g("DelLRate"));
                const double f = fraction(n.substr(0, n.find_last_not_of("DT") + 1));
                j["time"] = r(4 * f * (n.back() == 'D' ? 1.5 : n.back() == 'T' ? 2.0 / 3 : 1), 10000);
            } else j["ms"] = r(1000 * std::pow(g("DelLRate"), 2.0), 10);
            if (g("DelF1On") == 1 && filterKind(g("DelF1Typ")).first == "lowpass") j["lowpass"] = r(cutoffHz(g("DelF1Cut")), 1);
            if (g("DelF2On") == 1 && filterKind(g("DelF2Typ")).first == "highpass") j["highpass"] = r(cutoffHz(g("DelF2Cut")), 1);
            return j;
        }
        if (t == 2 || t == 11 || t == 21) {   // Classic, Acoustic and Convolution Reverb
            const double mix = t == 2 ? g("RevMix") : t == 11 ? g("AcoMix") : g("CRWet") * 0.5;
            if (mix < 0.005) return nullptr;
            const double size = t == 2 ? g("RevSize", 0.5) : t == 11 ? g("AcoSize", 0.77) : 0.7;
            const double decay = 0.3 * std::pow(2.0, 6 * (t == 11 ? g("AcoTime", 0.6) : size));   // a guess
            if (t == 21) notes.push_back("Convolution Reverb: its impulse response is Alchemy content; a synthetic reverb stands in");
            return {{"type", "reverb"}, {"decay", r(std::min(20.0, decay), 100)}, {"size", r(size)}, {"mix", r(std::min(1.0, mix))}};
        }
        if (t == 3) {   // Mod FX: a chorus (with feedback and a short delay it's a flanger, played as a short chorus)
            const double mix = g("ModMix");
            if (mix < 0.005) return nullptr;
            const bool flanger = g("ModDelay") < 0.05 && g("ModFback") > 0.2;
            if (flanger) notes.push_back("Mod FX with feedback plays as a short chorus (no flanger feedback)");
            return {{"type", "chorus"}, {"rate", r(std::max(0.05, 10 * std::pow(g("ModRate"), 2.0)))}, {"depth", r(1 + 6 * g("ModDepth"), 100)},
                    {"delay", r(flanger ? 2 : 1 + 30 * g("ModDelay"), 100)}, {"mix", r(std::min(1.0, mix / 2))}};
        }
        if (t == 4) {   // Distortion: tube / mech / exciter as saturation, crush as a bitcrusher
            json out = json::array();
            const double drv = std::max({g("DisTube"), g("DisMech"), g("DisXcita")});
            if (drv > 0.005) out.push_back({{"type", "saturate"}, {"drive", r(distortionDb(drv), 10)}, {"mix", 1.0}});
            if (g("DisCrush") > 0.005) out.push_back({{"type", "bitcrush"}, {"bits", r(std::max(2.0, 16 - 14 * g("DisCrush")), 10)}, {"mix", 1.0}});
            return out.empty() ? json(nullptr) : out;
        }
        if (t == 5) {   // MM Filter
            const auto kind = filterKind(g("FilType")).first;
            const double mix = g("FilMix");
            if (mix < 0.005) return nullptr;
            if (kind == "lowpass" || kind == "highpass" || kind == "bandpass")
                return {{"type", "filter"}, {"mode", kind}, {"cutoff", r(std::min(20000.0, std::max(20.0, cutoffHz(g("FilCut")))), 10)},
                        {"resonance", r(0.7071 + 8 * std::pow(g("FilRes"), 2.0))}, {"mix", r(mix)}};
            if (kind == "drive" && g("FilRes") > 0.01) return {{"type", "saturate"}, {"drive", r(distortionDb(g("FilCut")), 10)}, {"mix", r(std::min(1.0, mix * g("FilRes")))}};
            notes.push_back("MM Filter type #" + std::to_string((int)g("FilType")) + " (tuned family) not played");
            return nullptr;
        }
        if (t == 6) {   // Bandpass Filter: a low and a high cut
            json b = json::array();
            if (g("BpfLoCut") > 0.01) b.push_back({{"type", "highpass"}, {"freq", r(cutoffHz(g("BpfLoCut")), 10)}, {"q", r(0.7071 + 4 * std::pow(g("BpfLoRes"), 2.0))}});
            // a high cut stored at 0 is off, not a 12 Hz low-pass (Ghost Voices would be silent; no factory patch is)
            if (g("BpfHiCut") > 0.01 && g("BpfHiCut") < 0.99) b.push_back({{"type", "lowpass"}, {"freq", r(cutoffHz(g("BpfHiCut")), 10)}, {"q", r(0.7071 + 4 * std::pow(g("BpfHiRes"), 2.0))}});
            return b.empty() ? json(nullptr) : json{{"type", "eq"}, {"bands", b}};
        }
        if (t == 7) {   // Phat Compressor: one amount
            const double a = g("ComAmnt");
            if (a < 0.01) return nullptr;
            return {{"type", "compressor"}, {"threshold", r(-30 * a, 10)}, {"ratio", 4.0}, {"release", r(20 + 600 * g("ComRelse"), 1)}, {"makeup", r(12 * a, 10)}};
        }
        if (t == 8) {   // Bass Enhancer: a low shelf
            if (g("BEnhAmnt") < 0.01) return nullptr;
            return {{"type", "eq"}, {"bands", json::array({{{"type", "lowshelf"}, {"freq", r(40 + 160 * g("BEnhTune", 0.5), 1)}, {"gain", r(12 * g("BEnhAmnt"), 10)}}})}};
        }
        if (t == 9) {   // Panner
            const double pos = 2 * g("PanPan", 0.5) - 1;
            return std::fabs(pos) > 0.02 ? json{{"type", "pan"}, {"position", r(pos)}} : json(nullptr);
        }
        if (t == 10) {   // Amp: 0.5 = unity (the default; a guess)
            const double db = linDb(std::max((envAmp ? heldAmp : g("AmpAmp", 0.5)) * 2, 1e-4));
            return std::fabs(db) > 0.1 ? json{{"type", "gain"}, {"db", r(std::max(-60.0, std::min(24.0, db)), 100)}} : json(nullptr);
        }
        if (t == 12) {   // 3-Band EQ: +-18 dB (a guess)
            json b = json::array();
            for (auto [band, kind] : {std::pair<const char *, const char *>{"Lo", "lowshelf"}, {"Mi", "peak"}, {"Hi", "highshelf"}}) {
                const std::string k = std::string("Eq3") + band;
                const double gn = 36 * (g((k + "Gai").c_str(), 0.5) - 0.5);
                if (std::fabs(gn) > 0.2)
                    b.push_back({{"type", kind}, {"freq", r(std::min(20000.0, std::max(20.0, cutoffHz(g((k + "Frq").c_str())))), 1)}, {"gain", r(gn, 10)},
                                 {"q", r(0.3 + 2.7 * g((k + "Bw").c_str(), 0.5), 100)}});
            }
            return b.empty() ? json(nullptr) : json{{"type", "eq"}, {"bands", b}};
        }
        if (t == 20) {   // Phaser
            const double mix = g("PhaMix");
            if (mix < 0.005) return nullptr;
            return {{"type", "phaser"}, {"rate", g("PhaSync") == 1 ? json(syncNote(g("PhaRate"))) : json(r(std::max(0.02, 10 * std::pow(g("PhaRate"), 2.0))))},
                    {"feedback", r(std::min(0.9, g("PhaFeedb") * 0.9))}, {"stages", (int)std::min(24.0, std::max(2.0, 2.0 * ri(1 + 11 * g("PhaPoles"))))},
                    {"mix", r(std::min(1.0, mix))}};
        }
        if (t == 22) {   // Vintage Compressor
            return {{"type", "compressor"}, {"threshold", r(-40 + 40 * g("CoPThrsh", 0.5), 10)}, {"ratio", r(1 + 9 * g("CoPRatio", 0.5), 100)},
                    {"attack", r(0.5 + 50 * g("CoPAttck"), 10)}, {"release", r(20 + 600 * g("CoPRelse", 0.5), 1)}, {"makeup", r(36 * (g("CoPOGain", 0.333) - 0.333), 10)}};
        }
        if (t == 23 && g("WSMix") > 0.005) notes.push_back("Waveshaper (a drawn curve) not played");
        if (t == 13) notes.push_back("Band Reject: not played");
        return nullptr;
    }

    // the drive in dB of the distortion-type filters (their cutoff knob as the amount, their Res as the mix),
    // each weighted by the share of the level its source carries (a main filter's by all of it)
    double voiceDriveDb(const std::vector<const Filt *> &special) const {
        double tot = 0, acc = 0;
        for (auto *s : audible()) tot += std::pow(10.0, s->ampDb / 20) * s->weight;
        if (tot <= 0) return 0;
        for (auto *f : special) {
            if (f->kind != "drive" || f->res <= 0.01) continue;
            double w = tot;
            if (f->where.size() > 2 && f->where[1] == ' ') {   // "A filter 2": source A's
                w = 0;
                for (auto *s : audible()) if (s->letter == f->where[0]) w = std::pow(10.0, s->ampDb / 20) * s->weight;
            }
            acc += w * distortionDb(f->cutoff) * std::min(1.0, f->res);
        }
        return acc / tot;
    }
    bool voiceDrive = false;   // building for builtin:synth: distortion-type filters go to its filter's drive

    // the effects racks the signal reaches (A-D, then Main), after the filters builtin:synth can't hold per voice
    json effects(const std::set<std::string> &rk, const std::vector<const Filt *> &rest, const std::vector<const Filt *> &special) {
        json fx = json::array();
        for (auto *f : rest) {
            fx.push_back({{"type", "filter"}, {"mode", f->kind}, {"cutoff", r(std::min(20000.0, std::max(20.0, cutoffHz(f->cutoff))), 10)},
                          {"resonance", r(0.7071 + 8 * std::pow(f->res, 2.0))}});
            notes.push_back(f->where + ": played after the voices as a static filter");
        }
        // distortion-type filters work inside each voice in Alchemy: builtin:synth plays them as its filter's drive (set
        // with the filter); the sampler has no drive per voice, so one saturate after it, as hard as their level-weighted mix
        const double dDb = voiceDriveDb(special);
        if (!voiceDrive && dDb > 0.05) {
            fx.push_back({{"type", "saturate"}, {"drive", r(dDb, 10)}, {"mix", 1.0}});
            notes.push_back("distortion-type filters as one saturate after the instrument (a guess; Alchemy distorts each voice)");
        }
        for (auto *f : special)
            if (f->kind == "tuned") notes.push_back(f->where + ": tuned filter #" + std::to_string(f->code) + " (comb, ring, FM or formant family) not played");
        std::vector<std::string> order;
        for (const char *k : {"A", "B", "C", "D", "Main"}) if (rk.count(k)) order.push_back(k);
        if (held && order.size() > 2) {   // racks A-D each serve their own sources: in series one's pan or filter would cut the others
            std::map<std::string, double> lv;
            for (auto *s : audible())
                if (s->send > 0.01 && s->second.rfind("FX ", 0) == 0) lv[s->second.substr(3)] += std::pow(10.0, s->ampDb / 20) * s->weight * s->send;
            std::string keep = order[0];
            for (auto &k : order) if (k != "Main" && lv[k] > lv[keep]) keep = k;
            std::vector<std::string> kept, all, left;
            for (auto &k : order) {
                if (k != "Main") all.push_back(k);
                if (k == "Main" || k == keep) kept.push_back(k);
                else left.push_back(k);
            }
            auto list = [](const std::vector<std::string> &v) {
                std::string l;
                for (size_t k = 0; k < v.size(); ++k) l += (k ? k + 1 == v.size() ? " and " : ", " : "") + v[k];
                return l;
            };
            notes.push_back("effects racks " + list(all) + " each serve their own sources in Alchemy: rack " + keep + " (the most level) plays, " + list(left) + " left out");
            order = kept;
        }
        const double frac = fxFraction();
        if (!order.empty() && frac < 0.2) { notes.push_back("the effects racks get " + fmt("%.0f", 100 * frac) + "% of the signal (the rest is dry): left out"); return fx; }
        if (!order.empty() && frac < 0.8) notes.push_back("the effects racks get " + fmt("%.0f", 100 * frac) + "% of the signal: wet mixes scaled, inline effects kept");
        for (auto &k : order) {
            auto it = racks.find(k);
            if (it == racks.end()) continue;
            for (auto &e : it->second) {
                if (!e.on) continue;
                json j = effectJson(e);
                for (auto &x : j.is_array() ? j : j.is_null() ? json::array() : json::array({j})) {
                    const std::string ty = x["type"];
                    if (frac < 0.8 && (ty == "reverb" || ty == "delay" || ty == "chorus" || ty == "phaser") && x.contains("mix")) x["mix"] = r(x["mix"].get<double>() * frac);
                    fx.push_back(x);
                }
            }
        }
        return fx;
    }

    // its arpeggiator (the first block switched on) as a track "arp"
    json arp(std::vector<std::string> &an) const {
        if (arps.empty()) return nullptr;
        const ArpBlock &a = arps[0];
        if (a.free) an.push_back("arp: its free-running rate (" + a.rate + ") plays as 1/16");
        if (arps.size() > 1) an.push_back("arp: several arpeggiators (per source): the first one plays for all");
        an.push_back("arp: its step sequencer (per-step velocity, length, ties), shuffle and key velocity are not played");
        return {{"rate", a.free ? std::string("1/16") : a.rate}, {"order", a.order}, {"octaves", std::max(1, std::min(4, a.octaves))},
                {"gate", r(std::max(0.05, std::min(1.0, a.gate > 0 ? a.gate : 0.5)))}};
    }

    // ---- builtin:synth
    struct Osc {
        std::string wave;
        double level = 0, pw = kNone, cents = 0, decay = 0;
        int octave = 0, semi = 0;
        bool hasCents = false, sync = false, hasDecay = false;
        char from = 0;                 // the source it comes from
        json extra = json::object();   // an additive oscillator's partials and shift
        json j() const {
            json o = {{"wave", wave}, {"level", level}};
            for (auto &[k, v] : extra.items()) o[k] = v;
            if (octave) o["octave"] = octave;
            if (semi) o["semi"] = semi;
            if (hasCents) o["cents"] = cents;
            if (!std::isnan(pw)) o["pw"] = pw;
            if (sync) o["sync"] = true;
            if (hasDecay) o["decay"] = decay;
            return o;
        }
    };
    static void splitPitch(Osc &o, double pitch) {
        o.octave = std::fabs(pitch) >= 11.5 ? ri(pitch / 12) : 0;
        o.semi = ri(pitch - 12 * o.octave);
        const double c = (pitch - 12 * o.octave - o.semi) * 100;
        o.hasCents = std::fabs(c) > 0.5;
        o.cents = o.hasCents ? r(c, 10) : 0;
    }

    // a source's oscillators (VA and noise), or an additive element's
    struct Group {
        char letter;
        std::vector<Osc> oscs;
        int n = 1;
        double det = 0;
        bool wide = false, synced = false;
        double base = 0;
        const AdElement *el = nullptr;   // additive: its element, pitch, level, own decay, how it plays
        double pitch = 0, gain = 0, decay = 0, spread = -1;
        std::string how;
    };

    // An additive element as builtin:synth oscillators: the waves it amounts to where it amounts to some (a saw or
    // square spectrum as that wave, a Pulse/Saw synced as a hard-synced saw, one basic wave, a detuned stack as
    // unison, up to 12 sines or waves at their ratios), else one additive oscillator holding its partials (`one`: always).
    void additiveOscs(Group &gp, bool one) const {
        const AdElement &e = *gp.el;
        gp.oscs.clear();
        gp.n = 1, gp.det = 0, gp.spread = -1, gp.synced = false;
        const bool exact = e.wave.partials.empty() && (e.wave.wave == "square" || std::fabs(e.sym - 0.5) < 1e-3);   // a basic wave plays the shape
        auto wave = [&](const std::string &w, double ratio, double level) {
            Osc o;
            o.wave = w;
            o.level = level;
            splitPitch(o, gp.pitch + 12 * std::log2(ratio));
            if (w == "square" && w == e.wave.wave) {
                const double pw = (std::isnan(e.wave.pw) || e.wave.pw == 0 ? 0.5 : e.wave.pw) + (e.sym - 0.5);
                if (std::fabs(pw - 0.5) > 0.005) o.pw = r(std::min(0.95, std::max(0.05, pw)));
            }
            return o;
        };
        auto additive = [&](const std::vector<std::array<double, 3>> &parts, double pitch) {
            Osc o;
            o.wave = "additive";
            o.level = gp.gain;
            splitPitch(o, pitch);
            bool panned = false;
            for (auto &p : parts) panned |= std::fabs(p[2]) > 0.01;
            json list = json::array();
            for (auto &p : parts)
                if (p[1] > 1e-6) list.push_back(panned ? json{r(p[0], 10000), r(p[1], 100000), r(p[2])} : json{r(p[0], 10000), r(p[1], 100000)});
            o.extra["partials"] = list;
            if (e.shiftHz != 0) o.extra["shiftHz"] = r(e.shiftHz, 10);
            return o;
        };
        double lo = 1e300, hi = 0, amax = 0, spread = 0;
        for (auto &o : e.oscs) lo = std::min(lo, o[1]), hi = std::max(hi, o[1]), amax = std::max(amax, o[0]), spread = std::max(spread, std::fabs(o[2]));
        bool panned = false;
        for (auto &p : e.partials) panned |= std::fabs(p[2]) > 0.01;
        const bool stack = e.oscs.size() > 1 && lo > 0 && hi / lo < std::pow(2.0, 1.0 / 12);
        bool harmonic = true;   // every partial oscillator on a whole multiple of the note: one table holds them all
        for (auto &o : e.oscs) harmonic &= std::fabs(o[1] - std::round(o[1])) < 1e-6;
        std::vector<std::array<double, 3>> shape;   // one partial oscillator's own harmonics (its shape wave)
        double ctop = 0;
        for (auto &c : e.shape) ctop = std::max(ctop, std::abs(c));
        for (size_t m = 0; m < e.shape.size(); ++m)
            if (ctop > 0 && std::abs(e.shape[m]) / ctop >= 1e-3) shape.push_back({std::abs(e.shape[m]) / ctop, (double)(m + 1), 0.0});
        bool saw = false, sq = false;   // N >= 48 sines at a saw's or a square's levels
        if (e.simple && e.n >= 48) {
            std::map<double, double> lv;
            for (auto &o : e.oscs) lv[r(o[1], 1e6)] = o[0];
            const double a1 = lv.count(1.0) ? lv[1.0] : 0;
            saw = sq = a1 > 0;
            for (int h = 1; h <= 48 && a1 > 0; ++h) {
                const double v = (lv.count(h) ? lv[h] : 0.0) * h / a1;
                saw &= std::fabs(v - 1) < 0.03;
                sq &= std::fabs(v - h % 2) < 0.03;
            }
        }
        const bool synced = e.simple && e.pulseSaw && e.sync > 0.002 && e.n >= 32 && !e.othersOn && std::min(std::fabs(e.ps), std::fabs(e.ps - 1)) < 0.02 &&
                            std::fabs(e.oddEven - 0.5) < 0.02;
        if (one || e.shiftHz != 0 || e.partials.empty()) {
            if (!e.partials.empty()) gp.oscs.push_back(additive(e.partials, gp.pitch));
            gp.how = std::to_string(e.partials.size()) + " sine partials as an additive oscillator" + (e.shiftHz != 0 ? ", shifted " + fmt("%+.1f", e.shiftHz) + " Hz" : "");
        } else if (saw || sq) {
            gp.oscs.push_back(wave(saw ? "saw" : "square", 1, gp.gain));
            gp.how = fmt("%.0f", e.n) + " partials of a " + (saw ? "saw" : "square") + " spectrum: a band-limited " + (saw ? "saw" : "square");
        } else if (synced) {   // a hard-synced wave Sync x 48 semitones above the master (a guess, as the VA sync)
            Osc o = wave(e.ps > 0.5 ? "saw" : "square", std::pow(2.0, syncSemis(e.sync) / 12), gp.gain);
            o.sync = true;
            gp.oscs.push_back(o);
            gp.synced = true, gp.base = gp.pitch;
            gp.how = std::string("a Pulse/Saw spectrum with Sync: a hard-synced ") + o.wave + (std::fabs(e.tone - 0.5) > 0.02 ? " (its Tone tilt is not played)" : "");
        } else if (e.oscs.size() == 1 && (exact || e.partials.size() > 12)) {
            gp.oscs.push_back(exact ? wave(e.wave.wave, e.oscs[0][1], gp.gain) : additive(shape, gp.pitch + 12 * std::log2(e.oscs[0][1])));
            gp.how = exact ? "one " + e.wave.wave + " oscillator" : "one additive oscillator playing its " + e.wave.wave + " shape";
        } else if (stack) {   // detuned copies of the shape: unison around their middle, their levels evened
            const double mid = std::sqrt(lo * hi);
            gp.oscs.push_back(exact ? wave(e.wave.wave, mid, gp.gain) : additive(shape, gp.pitch + 12 * std::log2(mid)));
            gp.n = std::min(16, (int)e.oscs.size());
            gp.det = 1200 * std::log2(hi / lo);
            gp.spread = r(spread, 100);
            gp.how = "a stack of " + std::to_string(e.oscs.size()) + " detuned " + (exact ? e.wave.wave : "additive " + e.wave.wave) + " oscillators as unison (their levels evened)";
        } else if (e.partials.size() <= 12) {
            for (auto &p : e.partials) if (p[1] > 1e-6) gp.oscs.push_back(wave("sine", p[1], gp.gain * p[0]));
            gp.how = std::to_string(gp.oscs.size()) + " sine partials as sine oscillators" + (panned ? " (their pan not played)" : "");
        } else if (e.oscs.size() <= 12 && (exact || !harmonic)) {   // each oscillator as the wave, or (inharmonic) an additive one playing its shape
            for (auto &o : e.oscs)
                gp.oscs.push_back(exact ? wave(e.wave.wave, o[1], gp.gain * o[0] / amax) : additive(shape, gp.pitch + 12 * std::log2(o[1])));
            if (!exact) for (size_t k = 0; k < e.oscs.size(); ++k) gp.oscs[k].level = gp.gain * e.oscs[k][0] / amax;
            gp.how = std::to_string(e.oscs.size()) + (exact ? " " + e.wave.wave + " oscillators" : " additive oscillators playing its " + e.wave.wave + " shape") +
                     " at their ratios" + (spread > 0.01 ? " (their pan not played)" : "");
        } else {
            gp.oscs.push_back(additive(e.partials, gp.pitch));
            gp.how = std::to_string(e.partials.size()) + " sine partials as an additive oscillator";
        }
        for (auto &o : gp.oscs) if (gp.decay > 0 && o.level > 0) o.decay = r(gp.decay, 10000), o.hasDecay = true;
    }

    json toSynth(json &fxOut) {
        std::vector<const Filt *> mf;
        std::set<std::string> rk;
        path(mf, rk);
        const auto srcs = audible();
        const double masterTune = tune + fine / 100;
        std::string envSrc;
        ampEnvelope(envSrc);
        // a source's own decay: an AHDSR that fades a level to nothing (its amp's, when it isn't the amp envelope)
        auto ownDecay = [&](const Source *s, const char *k) {
            auto it = s->dyn.find(k);
            if (it != s->dyn.end())
                for (auto &x : it->second)
                    if (x.t == 2 && x.d > 0 && !(std::string(k) == "SAmp" && envSrc == std::string(1, s->letter))) {
                        const Adsr e = ahdsr(x.i);
                        if (e.ok && e.sustain < 0.05) return e.attack + e.decay;
                    }
            return 0.0;
        };
        std::vector<Group> groups;
        for (auto *s : srcs) {
            const double base = masterTune + s->tune + s->fine / 100, gain = std::pow(10.0, s->ampDb / 20) * s->weight;
            Group gp;
            gp.letter = s->letter;
            const std::string L(1, s->letter);
            if (s->va) {
                const double g = gain * std::pow(10.0, s->vaVolDb / 20);
                const std::vector<int> partials = s->wave.partials.empty() ? std::vector<int>{1} : s->wave.partials;
                const bool sync = s->sync > 0.002;
                if (sync) gp.synced = true, gp.base = base;
                for (size_t k = 0; k < partials.size(); ++k) {
                    Osc o;
                    o.wave = s->wave.wave;
                    o.level = g / (double)partials.size();
                    splitPitch(o, base + 12 * std::log2((double)std::max(1, partials[k])) + (sync ? syncSemis(s->sync) : 0));
                    if (o.wave == "square") {
                        const double pw = (std::isnan(s->wave.pw) || s->wave.pw == 0 ? 0.5 : s->wave.pw) + (s->sym - 0.5);
                        if (std::fabs(pw - 0.5) > 0.005) o.pw = r(std::min(0.95, std::max(0.05, pw)));
                    } else if (std::fabs(s->sym - 0.5) > 0.05 && k == 0)
                        notes.push_back(L + ": symmetry " + fmt("%.2f", s->sym) + " on a " + o.wave + " is not played");
                    o.sync = sync;
                    gp.oscs.push_back(o);
                }
                gp.n = s->nosc;
                gp.det = detuneCents(s->detune, s->wide);
                gp.wide = s->wide;
                if (!s->wave.partials.empty()) notes.push_back(L + ": " + s->vaShape + " as " + std::to_string(partials.size()) + " sine partials");
                else if (!s->wave.exact) notes.push_back(L + ": wavetable " + s->vaShape + " plays as " + s->wave.wave);
            }
            if (s->spectral && s->spNoise) {   // an Add+Spec import's noisy part: a band of noise (its level a guess, 12 dB under the element)
                Osc o;
                o.wave = "noise";
                o.level = gain * std::pow(10.0, (s->spVolDb - 12) / 20);
                if (s->spLowcut > 0.01) o.extra["lowcut"] = r(cutoffHz(s->spLowcut), 1);
                if (s->spHighcut < 0.99) o.extra["highcut"] = r(cutoffHz(s->spHighcut), 1);
                gp.oscs.push_back(o);
                notes.push_back(L + ": spectral element in Noise mode as a band of noise (its level a guess)");
            }
            if (s->noise) {
                Osc o;
                o.wave = "noise";
                o.level = gain * std::pow(10.0, s->noiseVolDb / 20);
                if (s->lowcut > 0.01) o.extra["lowcut"] = r(cutoffHz(s->lowcut), 1);     // its band, at middle C
                if (s->highcut < 0.99) o.extra["highcut"] = r(cutoffHz(s->highcut), 1);
                gp.oscs.push_back(o);
                std::string lowered = s->noiseShape;
                for (auto &c : lowered) c = (char)std::tolower((unsigned char)c);
                if (lowered != "white") notes.push_back(L + ": " + s->noiseShape + " noise plays as white noise");
            }
            const double dva = ownDecay(s, "SVAVol"), dnz = ownDecay(s, "SNsVol"), dall = ownDecay(s, "SAmp");
            for (auto &o : gp.oscs) {
                const double own = o.wave == "noise" ? dnz : dva;
                double dd = 0;
                bool any = false;
                for (double x : {own, dall}) if (x > 0) { dd = any ? std::min(dd, x) : x; any = true; }
                if (dd > 0 && o.level > 0) o.decay = r(dd, 10000), o.hasDecay = true;
            }
            if (std::fabs(s->pan) > 0.1) notes.push_back(L + ": pan " + fmt("%+.2f", s->pan) + " not played (no per-oscillator pan)");
            groups.push_back(gp);
            if (s->additive) {
                Group ag;
                ag.letter = s->letter;
                ag.el = &adds[(size_t)(s->letter - 'A')];
                ag.pitch = base;
                ag.gain = gain * s->adVol.v;
                const double dad = ownDecay(s, "SAdVol");
                ag.decay = dad > 0 && dall > 0 ? std::min(dad, dall) : std::max(dad, dall);
                additiveOscs(ag, false);
                groups.push_back(ag);
            }
        }
        // more than 12 oscillators: the additive elements playing as several sines or waves become one additive oscillator
        for (;;) {
            size_t total = 0;
            bool sync = false;
            Group *big = nullptr;
            for (auto &gp : groups) {
                total += gp.oscs.size();
                sync |= gp.synced;
                if (gp.el && gp.oscs.size() > 1 && (!big || gp.oscs.size() > big->oscs.size())) big = &gp;
            }
            if (total + sync <= 12 || !big) break;
            additiveOscs(*big, true);
        }
        for (auto &gp : groups) {
            if (!gp.el) continue;
            const std::string L(1, gp.letter);
            notes.push_back(L + ": additive, " + gp.how);
            for (auto &x : gp.el->notes) notes.push_back(L + ": " + x);
            for (auto &x : gp.el->unexpressed) notes.push_back(L + ": " + x + " not played");
        }
        // hard sync: a silent master first, at the first synced source's pitch
        std::vector<Osc> oscs;
        const Group *firstSynced = nullptr;
        for (auto &gp : groups) if (gp.synced) { if (!firstSynced) firstSynced = &gp; else if (std::fabs(gp.base - firstSynced->base) > 0.01) notes.push_back(std::string(1, gp.letter) + ": hard sync follows source " + firstSynced->letter + "'s pitch"); }
        if (firstSynced) { Osc m; m.wave = "sine"; splitPitch(m, firstSynced->base); oscs.push_back(m); }
        // unison: one count for all when the sources agree, else detuned copies when they fit in 12
        std::vector<int> counts;
        for (auto &gp : groups) {
            bool tonal = false;
            for (auto &o : gp.oscs) tonal |= o.wave != "noise";
            if (tonal) counts.push_back(gp.n);
        }
        const std::set<int> distinct(counts.begin(), counts.end());
        size_t total = oscs.size();
        for (auto &gp : groups) total += (size_t)gp.n * gp.oscs.size();
        const bool explicitCopies = distinct.size() > 1 && total <= 12;
        for (auto &gp : groups)
            for (auto &o : gp.oscs) {
                o.from = gp.letter;
                if (explicitCopies && gp.n > 1 && o.wave != "noise") {
                    const double baseC = 100.0 * (12 * o.octave + o.semi) + (o.hasCents ? o.cents : 0);
                    for (int k = 0; k < gp.n; ++k) {
                        Osc c = o;
                        c.level = o.level / std::sqrt((double)gp.n);
                        splitPitch(c, (baseC + gp.det * ((double)k / (gp.n - 1) - 0.5)) / 100);
                        oscs.push_back(c);
                    }
                } else oscs.push_back(o);
            }
        if (explicitCopies) notes.push_back("per-source unison played as detuned oscillator copies (no stereo spread)");
        double top = 0;
        for (auto &o : oscs) if (o.level > 0) top = std::max(top, o.level);
        if (top <= 0) top = 1;
        for (auto &o : oscs) o.level = r(o.level / top);
        json synth = json::object(), oj = json::array();
        for (size_t k = 0; k < oscs.size() && k < 12; ++k) oj.push_back(oscs[k].j());
        synth["osc"] = oj;
        if (oscs.size() > 12) notes.push_back(std::to_string(oscs.size()) + " oscillators: the first 12 kept");
        if (!counts.empty() && !explicitCopies && *std::max_element(counts.begin(), counts.end()) > 1) {
            const int vmax = *std::max_element(counts.begin(), counts.end());
            double det = -1e300;
            bool wide = false;
            double stackSpread = -1;   // an additive stack's own spread (its pan profile)
            for (auto &gp : groups) { if (gp.n == vmax) det = std::max(det, gp.det), stackSpread = std::max(stackSpread, gp.spread); wide |= gp.wide; }
            synth["unison"] = {{"voices", std::min(16, vmax)}, {"detune", r(std::min(100.0, det), 10)}, {"spread", stackSpread >= 0 ? stackSpread : wide ? 0.8 : 0.5}};
            if (det > 100) notes.push_back("unison detune " + fmt("%.0f", det) + " cents capped at 100");
            if (distinct.size() > 1) notes.push_back("sources have different unison counts: all oscillators use " + std::to_string(vmax));
        }
        // the filter
        std::vector<const Filt *> rest, special;
        std::vector<std::string> skipped;
        const Filt *prim = pickFilter(mf, rest, special, skipped);
        if (prim) {
            json fl = {{"type", prim->kind}, {"slope", prim->slope}, {"cutoff", r(std::min(20000.0, std::max(20.0, cutoffHz(prim->cutoff))), 10)}, {"resonance", r(prim->res)}};
            double sweep = 0;
            for (auto &m : prim->mods) if ((m.t == 3 || m.t == 4) && m.d > 0) sweep += m.d;
            sweep /= 2;
            if (sweep != 0) {
                fl["cutoff"] = r(std::min(20000.0, std::max(20.0, cutoffHz(std::min(1.0, prim->cutoff + sweep)))), 10);
                notes.push_back("filter: its MSEG or sequencer sweep plays as the middle of its range");
            }
            for (auto &w : skipped) notes.push_back(w + ": affects one quiet source only, not played");
            bool haveEnv = false;
            int envI = 0;
            double envD = 0;
            for (auto &m : prim->mods) {
                if (m.t == 2 && !haveEnv) { haveEnv = true; envI = m.i; envD = m.d; }
                else if (m.t == 5 && (m.i == 1 || m.i == 2)) fl["keytrack"] = r(std::min(1.0, std::max(0.0, m.d)));
                else if (m.t == 5 && m.i == 0) {
                    fl["velocity"] = r(std::min(4.0, std::max(0.0, m.d * kCutOct)));
                    fl["cutoff"] = r(std::min(20000.0, std::max(20.0, cutoffHz(std::min(1.0, prim->cutoff + std::max(0.0, m.d))))), 10);
                }
            }
            if (haveEnv) {
                const Adsr fe = ahdsr(envI);
                if (fe.ok) {
                    synth["filterEnv"] = adsrJson(fe);
                    fl["env"] = r(std::max(-8.0, std::min(8.0, envD * kCutOct)));
                }
            }
            if (prim->drive > 0.01) fl["drive"] = r(std::min(1.0, prim->drive));
            synth["filter"] = fl;
            if (prim->where.size() > 2 && prim->where[1] == ' ' && prim->where[0] >= 'A' && prim->where[0] <= 'D') {
                // a source's own filter: the other sources skip it, unless they have one of its kind (not played, so it stands in)
                auto through = [&](char letter) {
                    if (letter == prim->where[0]) return true;
                    for (auto *s : audible())
                        if (s->letter == letter)
                            for (auto &f : s->filters)
                                if (f.kind == prim->kind && !(f.mods.empty() && ((f.kind == "lowpass" && f.cutoff >= 0.99) || (f.kind == "highpass" && f.cutoff <= 0.01)))) return true;
                    return false;
                };
                std::set<char> skip;
                for (size_t k = 0; k < oscs.size() && k < 12; ++k)
                    if (oscs[k].from && oscs[k].level > 0 && !through(oscs[k].from)) synth["osc"][k]["filter"] = false, skip.insert(oscs[k].from);
                std::string who;
                for (char c : skip) who += (who.empty() ? "" : ", ") + std::string(1, c);
                if (!who.empty()) notes.push_back("filter: " + prim->where + " only, so source" + (skip.size() > 1 ? "s " : " ") + who + " skip it");
            }
            notes.push_back("filter: " + prim->where + " type #" + std::to_string(prim->code) + " as " + prim->kind + " " + std::to_string(prim->slope) + " dB (type order inferred)");
        } else synth["filter"] = {{"type", "off"}};
        if (const double dDb = voiceDriveDb(special); dDb > 0.05) {   // distortion-type filters: the voice's drive (gain 1 + 7 x drive)
            const double d = std::min(1.0, (std::pow(10.0, dDb / 20) - 1) / 7);
            synth["filter"]["drive"] = r(std::max(synth["filter"].value("drive", 0.0), d));
            notes.push_back("distortion-type filters as the voice's drive (a guess)");
        }
        // the amplitude envelope and its velocity
        std::string which;
        const Adsr env = ampEnvelope(which);
        if (env.ok) {
            json a = adsrJson(env);
            a["velocity"] = 0.0;
            for (auto &x : ampDyn) if (x.t == 5 && x.i == 0) { a["velocity"] = r(std::min(1.0, std::max(0.0, x.d))); break; }
            synth["amp"] = a;
            if (env.hold > 0.005) notes.push_back("amp envelope hold " + fmt("%.3f", env.hold) + " s not played");
            if (which != "master") notes.push_back("amp envelope taken from source " + which);
        } else notes.push_back("no AHDSR on the amplitude: the default envelope plays");
        // a pitch envelope: an AHDSR on coarse tune that decays to nothing
        for (auto &rt : routes)
            if (rt.src.rfind("ahdsr", 0) == 0 && (rt.tgt == "master.TuneCrs" || rt.tgt == "source.STunCrs") && std::fabs(rt.d) > 0.005) {
                const Adsr pe = ahdsr(std::atoi(rt.src.c_str() + 5) - 1);
                if (pe.ok && pe.sustain < 0.05) {
                    synth["pitchEnv"] = {{"amount", r(std::max(-48.0, std::min(48.0, rt.d * 96)), 100)}, {"decay", r(std::max(0.001, pe.decay / 4.6), 10000)}};
                    break;
                }
            }
        // LFOs to pitch, cutoff, amp, pulse width and pan
        json lfos = json::array();
        std::set<std::pair<int, std::string>> seen;
        const std::string fw = prim ? prim->where : "";
        bool square = false;
        for (auto &o : synth["osc"]) square |= o["wave"] == "square";
        for (auto &rt : routes) {
            if (rt.src.rfind("lfo", 0) != 0 || std::fabs(rt.d) < 0.01) continue;
            const int i = std::atoi(rt.src.c_str() + 3) - 1;
            std::string to;
            double dep = 0;
            const bool mainF = fw.rfind("main", 0) == 0;
            if (rt.tgt == "master.TuneFine" || rt.tgt == "source.STunFin") to = "pitch", dep = rt.d * 2.0;   // +-100 cents per 1.0
            else if (rt.tgt == "master.TuneCrs" || rt.tgt == "source.STunCrs") to = "pitch", dep = rt.d * 96;
            else if (!fw.empty() && (((rt.tgt == "filters.F1Cut" || rt.tgt == "filters.F2Cut") && mainF) || (rt.tgt.rfind("source.SF", 0) == 0 && !mainF)))
                to = "cutoff", dep = rt.d * kCutOct;
            else if (rt.tgt == "master.Amp" || rt.tgt == "source.SAmp" || rt.tgt == "master.Volume") to = "amp", dep = std::fabs(rt.d) * 2;
            else if (rt.tgt == "source.SVASym" && square) to = "pw", dep = rt.d;
            else if (rt.tgt == "source.SPan" || rt.tgt == "master.Pan") to = "pan", dep = rt.d * 2;
            if (to.empty() || seen.count({i, to}) || lfos.size() >= 4) continue;
            json spec = lfoSpec(i, dep, to);
            if (spec.is_null()) continue;
            lfos.push_back(spec);
            seen.insert({i, to});
            bool bip = false;   // a unipolar LFO on the cutoff sweeps above it: the base moves up half its depth
            for (auto &p : P.sec("lfo", (size_t)i)) bip |= p.key == "LfoBiPol" && p.has && p.value == 1;
            if (to == "cutoff" && !bip && synth["filter"].contains("cutoff"))
                synth["filter"]["cutoff"] = r(std::min(20000.0, std::max(20.0, synth["filter"]["cutoff"].get<double>() * std::pow(2.0, dep / 2))), 10);
        }
        if (!lfos.empty()) synth["lfo"] = lfos;
        // voices, glide, level (master volume and the Perform volume against their usual 0.63 and 0.794, and Amp)
        if (voices <= 1) {
            synth["mono"] = true;
            synth["legato"] = trigger != 1;
            if (glide > 0.001) synth["glide"] = r(std::min(2.0, glideSec(glide)));
        }
        const double level = volumeDb - linDb(0.63) + pervolDb - linDb(0.794) + linDb(std::max(amp, 1e-3));
        synth["level"] = r(std::max(-40.0, std::min(12.0, level)), 100);
        voiceDrive = true;
        fxOut = effects(rk, rest, special);
        voiceDrive = false;
        bool additive = false;
        for (auto &gp : groups) {   // an additive element's Comb, Filter or EQ unit, after the voices
            if (!gp.el) continue;
            additive = true;
            if (gp.el->post.empty()) continue;
            fxOut.insert(fxOut.begin(), gp.el->post.begin(), gp.el->post.end());
            if (srcs.size() > 1) notes.push_back(std::string(1, gp.letter) + ": its additive Comb, Filter or EQ unit plays after the voices, on every source");
        }
        json fb = json::array();   // the formant filters of every additive source as one EQ, each source's peaks scaled by its share
        int nf = 0;
        for (auto &gp : groups) nf += gp.el && !gp.el->formant.empty();
        for (auto &gp : groups)
            if (gp.el) for (auto b : gp.el->formant) { b["gain"] = r(b["gain"].get<double>() / nf, 10); fb.push_back(b); }
        if (!fb.empty()) fxOut.insert(fxOut.begin(), json{{"type", "eq"}, {"bands", fb}});
        const auto un = unmapped(false, additive || !fb.empty());
        if (!un.empty()) {
            std::string l;
            for (size_t k = 0; k < un.size() && k < 6; ++k) l += (k ? "; " : "") + un[k];
            notes.push_back("not played: " + l + (un.size() > 6 ? " ..." : ""));
        }
        return synth;
    }

    // ---- the sampler: the audible sources as SFZ regions (one group's settings per source)
    void toSampler(AlchemyPatch &out) {
        json additivePost = json::array();
        std::vector<const Filt *> mf;
        std::set<std::string> rk;
        path(mf, rk);
        const auto srcs = audible();
        std::string which;
        const Adsr env = ampEnvelope(which);
        std::vector<const Filt *> rest, special;
        std::vector<std::string> skipped;
        const Filt *prim = pickFilter(mf, rest, special, skipped);
        bool hasVel = false;
        double vel = 0;
        for (auto &x : ampDyn) if (x.t == 5 && x.i == 0) { hasVel = true; vel = x.d; break; }
        out.sfz.dir = "/";
        for (auto *s : srcs) {
            const std::string L(1, s->letter);
            std::map<std::string, std::string> grp;
            const double tn = tune + s->tune, fn = fine + s->fine;
            grp["transpose"] = std::to_string(ri(tn));
            grp["tune"] = std::to_string(ri(fn + 100 * (tn - ri(tn))));
            if (env.ok) {
                grp["ampeg_attack"] = fmt("%.4f", env.attack);
                grp["ampeg_hold"] = fmt("%.4f", env.hold);
                grp["ampeg_decay"] = fmt("%.4f", env.decay);
                grp["ampeg_sustain"] = fmt("%.1f", 100 * env.sustain);
                grp["ampeg_release"] = fmt("%.4f", env.release);
            }
            grp["amp_veltrack"] = std::to_string(hasVel ? ri(100 * std::min(1.0, std::max(0.0, vel))) : 0);
            if (prim) {   // a static filter where its envelopes rest while a note is held
                const bool lp = prim->kind == "lowpass", hp = prim->kind == "highpass";
                grp["fil_type"] = lp ? (prim->slope == 24 ? "lpf_4p" : "lpf_2p") : hp ? (prim->slope == 24 ? "hpf_4p" : "hpf_2p") : "bpf_2p";
                double kt = 0, held = prim->cutoff;
                for (auto &m : prim->mods) if (m.t == 5 && (m.i == 1 || m.i == 2)) { kt = m.d; break; }
                for (auto &m : prim->mods)
                    if (m.t == 2) { const Adsr e = ahdsr(m.i); held += m.d * (e.ok ? e.sustain : 0); }
                held = std::min(1.0, std::max(0.0, held));
                grp["cutoff"] = fmt("%.1f", cutoffHz(held));
                grp["resonance"] = fmt("%.1f", 24 * prim->res);
                grp["fil_keytrack"] = std::to_string(ri(100 * kt));
                bool moves = false;
                for (auto &m : prim->mods) moves |= m.t >= 1 && m.t <= 4;
                if (moves) notes.push_back(prim->where + ": its envelope or LFO sweep is not played (the sampler's filter is static)");
            }
            if (s->pan != 0) grp["pan"] = fmt("%.0f", 100 * s->pan);
            if (s->keytrack == 0) grp["pitch_keytrack"] = "0";
            const double gainDb = s->ampDb + linDb(std::max(s->weight, 1e-4));
            auto region = [&](std::map<std::string, std::string> reg) {
                SfzRegion rg;
                rg.op = grp;
                for (auto &[k, v] : reg) rg.op[k] = v;
                out.sfz.regions.push_back(rg);
            };
            // granular: Speed 0.5 = 100% plays the zone, 0 freezes at Position (a short crossfaded loop there, about a
            // grain: the guide's 2-230 ms, linear a guess), a slow scan plays the zone at its own rate
            const bool frozen = s->granular && s->grSpeed.v < 0.005, scan = s->granular && !frozen && std::fabs(s->grSpeed.v - 0.5) >= 0.02;
            const double grain = 0.002 + 0.228 * s->grSize.v;
            if (s->granular) {
                for (auto [what, e] : {std::pair<const char *, const Eff *>{"position", &s->pos}, {"grain size", &s->grSize}})
                    if (!e->dyn.empty()) notes.push_back(L + ": granular " + what + " moves in Alchemy (" + srcName(e->dyn[0].t, e->dyn[0].i) + "): played at rest");
                if (s->grRand.v > 0.5) notes.push_back(L + ": granular, random grain starts (" + fmt("%.0f", 100 * s->grRand.v) + "%) not played");
                if (frozen) notes.push_back(L + ": granular, frozen at Position " + fmt("%.2f", s->position) + ": a " + fmt("%.0f", 1000 * grain) + " ms crossfaded loop there (the grain texture is lost)");
                else if (scan) notes.push_back(L + ": granular, scanning at Speed " + fmt("%.2f", s->grSpeed.v) + " (a time-stretch in Alchemy): plays at the sample's own rate");
                else notes.push_back(L + ": granular at Speed 100%: plays as the sample itself (the grain texture and Alchemy's time-kept transposition are lost)");
            }
            if (s->sampler || s->granular)
                for (auto &z : s->zones) {
                    std::map<std::string, std::string> reg = {{"sample", z.found}, {"pitch_keycenter", std::to_string(z.key)}, {"lokey", std::to_string(z.lokey)},
                        {"hikey", std::to_string(z.hikey)}, {"lovel", std::to_string(z.lovel)}, {"hivel", std::to_string(z.hivel)},
                        {"volume", fmt("%.2f", gainDb + s->sampVolDb + z.volDb)}};
                    if (z.tune != 0 || z.fine != 0) { reg["transpose"] = std::to_string(ri(z.tune)); reg["tune"] = std::to_string(ri(z.fine)); }
                    if (z.pan != 0) reg["pan"] = fmt("%.0f", 100 * z.pan);
                    const double sr = sampleRate(z.found);
                    if (z.loopMode >= 1 && z.loopMode <= 3 && z.loop && sr > 0 && z.loopEnd > z.loopStart) {   // loop points in seconds
                        reg["loop_mode"] = z.loopMode == 2 ? "loop_sustain" : "loop_continuous";
                        reg["loop_start"] = std::to_string((long long)(z.loopStart * sr));
                        reg["loop_end"] = std::to_string((long long)(z.loopEnd * sr));
                        if (z.loopMode == 3) notes.push_back(L + ": its forward-backward loop plays forward");
                    } else reg["loop_mode"] = "no_loop";
                    if (s->position > 0.001 && z.hasEnd && z.end != 0 && sr > 0) reg["offset"] = std::to_string((long long)(s->position * z.end * sr));
                    if (frozen && sr > 0) {
                        const double from = z.hasEnd && z.end > 0 ? s->position * z.end : 0;
                        reg["loop_mode"] = "loop_continuous";
                        reg["offset"] = reg["loop_start"] = std::to_string((long long)(from * sr));
                        reg["loop_end"] = std::to_string((long long)((from + grain) * sr));
                        reg["loop_crossfade"] = fmt("%.4f", grain / 4);
                    }
                    if (s->reverse) reg["direction"] = "reverse";
                    region(reg);
                    ++out.total;
                    out.samples += !z.found.empty();
                }
            if (s->va) {
                region({{"sample", "*" + s->wave.wave}, {"volume", fmt("%.2f", gainDb + s->vaVolDb)}});
                notes.push_back(L + ": VA " + s->vaShape + " layered as an SFZ *" + s->wave.wave + " generator (no unison, sync or pulse width)");
            }
            if (s->noise) region({{"sample", "*noise"}, {"volume", fmt("%.2f", gainDb + s->noiseVolDb)}});
            if (s->additive) {   // its strongest partials (up to 16, down to 40 dB under the loudest) as *sine regions at their ratios,
                // levels as the synth's additive oscillator scales them (the partials' power to one)
                const AdElement &e = adds[(size_t)(s->letter - 'A')];
                std::vector<std::array<double, 3>> ps = e.partials;
                std::sort(ps.begin(), ps.end(), [](const std::array<double, 3> &a, const std::array<double, 3> &b) { return a[0] > b[0]; });
                double power = 0;
                for (auto &p : e.partials) power += p[0] * p[0];
                size_t kept = 0;
                for (auto &p : ps) {
                    if (kept == 16 || p[0] < 0.01 || p[1] <= 0) break;
                    const double semis = tn + fn / 100 + 12 * std::log2(p[1]);
                    std::map<std::string, std::string> reg = {{"sample", "*sine"}, {"transpose", std::to_string(ri(semis))}, {"tune", std::to_string(ri(100 * (semis - ri(semis))))},
                        {"volume", fmt("%.2f", gainDb + linDb(s->adVol.v) + linDb(p[0] / std::sqrt(std::max(1.0, power))))}};
                    if (std::fabs(p[2]) > 0.01 || s->pan != 0) reg["pan"] = fmt("%.0f", 100 * std::max(-1.0, std::min(1.0, p[2] + s->pan)));
                    region(reg);
                    ++kept;
                }
                notes.push_back(L + ": additive, its " + std::to_string(kept) + " strongest partials as SFZ *sine regions");
                if (s->spectral && s->spNoise) {   // its Add+Spec noisy part: high-passed noise (level a guess, 12 dB under)
                    std::map<std::string, std::string> reg = {{"sample", "*noise"}, {"volume", fmt("%.2f", gainDb + s->spVolDb - 12)}};
                    if (s->spLowcut > 0.01) reg["fil_type"] = "hpf_2p", reg["cutoff"] = fmt("%.1f", cutoffHz(s->spLowcut)), reg["fil_keytrack"] = "0";
                    region(reg);
                    notes.push_back(L + ": spectral element in Noise mode as high-passed noise (its level a guess)");
                }
                for (auto &x : e.notes) notes.push_back(L + ": " + x);
                for (auto &x : e.unexpressed) notes.push_back(L + ": " + x + " not played");
                if (!e.formant.empty()) additivePost.push_back({{"type", "eq"}, {"bands", e.formant}});
                if (!e.post.empty() || !e.formant.empty()) {
                    additivePost.insert(additivePost.end(), e.post.begin(), e.post.end());
                    if (srcs.size() > 1) notes.push_back(L + ": its additive Comb, Filter, EQ or formant unit plays after the voices, on every source");
                }
            }
        }
        if (voices <= 1) {
            out.sampler["mono"] = true;
            if (glide > 0.001) out.sampler["glide"] = r(std::min(2.0, glideSec(glide)));
        }
        out.synth.fx = effects(rk, rest, special);
        out.synth.fx.insert(out.synth.fx.begin(), additivePost.begin(), additivePost.end());
        const auto un = unmapped(false, !additivePost.empty());
        if (!un.empty()) {
            std::string l;
            for (size_t k = 0; k < un.size() && k < 6; ++k) l += (k ? "; " : "") + un[k];
            notes.push_back("not played: " + l + (un.size() > 6 ? " ..." : ""));
        }
        const double lv = volumeDb - linDb(0.63) + pervolDb - linDb(0.794);
        if (std::fabs(lv) > 0.1) out.gain = r(lv, 100);
    }
};
} // namespace

bool alchemyMayPlaySamples(const std::string &text) {
    for (size_t p = text.find("SGrOn = 1"); p != std::string::npos; p = text.find("SGrOn = 1", p + 1))
        if (p + 9 >= text.size() || text[p + 9] == '\r' || text[p + 9] == '\n' || text[p + 9] == ' ') return true;
    return false;
}

AlchemyPatch alchemyPatch(const std::string &text, const std::string &name) {
    AlchemyPatch out;
    out.synth.name = name;
    out.synth.instrument = "Alchemy";
    if (text.empty()) {
        out.why = "its Alchemy channel stores no preset text, so only GarageBand and Logic can play it";
        out.what = "no preset";
        return out;
    }
    try {
        const Preset P(text);
        Decoder D(P, name);
        bool samples = false;
        out.why = D.refusal(samples, out.what);
        out.arp = D.arp(out.arpNotes);
        if (!out.why.empty()) return out;
        if (samples) {
            out.kind = AlchemyPatch::Sampler;
            D.toSampler(out);
        } else {
            out.kind = AlchemyPatch::Synth;
            out.synth.engine = out.what == "additive" ? "additive" : "virtual analog";
            out.synth.synth = D.toSynth(out.synth.fx);
            out.synth.arp = out.arp;
        }
        out.synth.notes = D.notes;
        out.synth.notes.insert(out.synth.notes.end(), out.arpNotes.begin(), out.arpNotes.end());
    } catch (const std::exception &e) {
        out = AlchemyPatch{};
        out.synth.name = name;
        out.synth.instrument = "Alchemy";
        out.why = std::string("its Alchemy preset text couldn't be read (") + e.what() + "), so only GarageBand and Logic can play it";
        out.what = "unreadable";
    }
    return out;
}

} // namespace wl
