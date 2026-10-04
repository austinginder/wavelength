#include "logic_patches.hpp"

#include "alchemy.hpp"
#include "bplist.hpp"
#include "platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace wl {

namespace {
std::string lower(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// instruments by the names channel strips give them; anything else in a slot is an effect
const std::set<std::string> &synthNames() {
    static const std::set<std::string> s = {"Alchemy", "Retro Synth", "ES2", "ES1", "ES E", "ES M", "ES P", "EFM1", "Sculpture",
                                            "Vintage B3", "Clav", "E-Piano", "Ultrabeat", "Church Orga", "Klopfgeist", "EVOC PS",
                                            "Mellotron", "Studio Piano", "Studio Strin", "Studio Horns", "Drum Machin", "Quick Sampl"};
    return s;
}

struct Record { size_t at, size; std::string name, preset; bool bypassed = false, midiFx = false; int order = 0; };

// The plugin records of a channel strip: where each payload is, its size and its plugin's name
bool records(const std::vector<uint8_t> &d, std::vector<Record> &out) {
    const uint8_t magic[4] = {'U', 'C', 'u', 'A'};
    const auto first = std::search(d.begin(), d.begin() + (long)std::min<size_t>(d.size(), 4096), magic, magic + 4);
    for (size_t pos = (size_t)(first - d.begin()); pos + 36 <= d.size() && !std::memcmp(&d[pos], "UCuA", 4);) {
        const uint32_t n = le32(&d[pos + 0x1c]);
        Record r{pos + 36, n, "", "", false, false, 0};
        const uint8_t *pl = &d[pos + 36];
        if (n >= 140 && pos + 36 + 140 <= d.size() && (!std::memcmp(pl + 132, "MELC", 4) || !std::memcmp(pl + 132, "GAME", 4))) {
            for (size_t k = 0; k < 12 && pl[120 + k]; ++k) r.name += (char)pl[120 + k];
            for (size_t k = 14; k < 120 && pl[k] >= 32 && pl[k] < 127; ++k) r.preset += (char)pl[k];
            r.bypassed = pl[112] != 0;
            r.order = pl[6] | pl[7] << 8;          // the insert's position among the audio effects (1, 2, ...)
            r.midiFx = n >= 152 && (le32(pl + 148) & 0x02000000);   // the plug-in's flags: MIDI effect (Arpeggiator, ...)
        }
        out.push_back(r);
        pos += 36 + (size_t)n;
    }
    return !out.empty();
}

// the settings block inside a plug-in record's payload [a, b)
PatchPlugin settingsOf(const std::vector<uint8_t> &d, size_t a, size_t b, const std::string &name) {
    PatchPlugin p;
    p.name = name;
    for (size_t g = a + 12; g + 12 <= b; ++g) {
        const bool le = !std::memcmp(&d[g], "GAMETSPP", 8), be = !le && !std::memcmp(&d[g], "EMAGPPST", 8);
        if (!le && !be) continue;
        auto u32 = [&](size_t o) { return be ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3] : le32(&d[o]); };
        const size_t s = g - 12;
        const uint32_t count = u32(s + 8);
        if (count > 4096 || s + 24 + 4 * (size_t)count > b) break;
        p.id = u32(g + 8);
        // the values follow the header, except Vintage B3's: its 26 preset-key registrations (234 int32) come
        // first, so its values end the block
        const size_t blockEnd = std::min(b, s + (size_t)u32(s)), at = name == "Vintage B3" && blockEnd >= s + 24 + 4 * (size_t)count ? blockEnd - 4 * (size_t)count : s + 24;
        for (uint32_t i = 0; i < count; ++i) {   // value 0 is reserved: params[n] = parameter #n
            const uint32_t bits = u32(at + 4 * (size_t)i);
            float f;
            std::memcpy(&f, &bits, 4);
            p.values.push_back(std::isfinite(f) ? f : 0.f);
            if (i) p.params.push_back(p.values.back());
        }
        if (le) p.block.assign(d.begin() + (long)s, d.begin() + (long)blockEnd);
        break;
    }
    return p;
}

bool readWhole(const std::string &path, std::vector<uint8_t> &d) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    d.resize((size_t)in.tellg());
    in.seekg(0);
    return (bool)in.read(reinterpret_cast<char *>(d.data()), (std::streamsize)d.size());
}

// the instrument record of a channel strip: the first record named as an instrument
const Record *instrumentOf(const std::vector<Record> &recs) {
    for (auto &r : recs)
        if (isSamplerInstrument(r.name) || synthNames().count(r.name)) return &r;
    return nullptr;
}

// Alchemy keeps its settings as text: from "<alchemypreset>" (after a header and sometimes a binary block, past the
// plug-in data's start at payload +140) to the last line end before the first byte that isn't text
std::string alchemyText(const std::vector<uint8_t> &d, size_t a, size_t b) {
    static const char tag[] = "<alchemypreset>";
    if (b <= a + 140) return "";
    const auto it = std::search(d.begin() + (long)(a + 140), d.begin() + (long)b, tag, tag + 15);
    const size_t s = (size_t)(it - d.begin());
    size_t e = s;
    while (e < b && (d[e] == '\t' || d[e] == '\r' || d[e] == '\n' || (d[e] >= 32 && d[e] < 127))) ++e;
    if (e < b) while (e > s && d[e - 1] != '\n') --e;
    return std::string(d.begin() + (long)s, d.begin() + (long)e);
}

std::vector<std::string> channelFiles(const std::string &patchDir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(patchDir, ec))
        if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".cst") files.push_back(e.path().string());
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        const bool ra = fs::path(a).filename() == "#Root.cst", rb = fs::path(b).filename() == "#Root.cst";
        return ra != rb ? ra : a < b;
    });
    return files;
}
} // namespace

namespace {
using nlohmann::json;
double r2(double v) { return std::round(v * 100) / 100; }
double r4(double v) { return std::round(v * 10000) / 10000; }

// an Apple low or high cut (6-48 dB/oct, Q as resonance) as 12 dB/oct biquads: Butterworth stages, the sharpest
// one taking the Q; an odd order gets a Q 0.5 stage (Wavelength's eq has no first-order band)
void cutStages(json &bands, const char *type, double freq, int slopeDb, double q) {
    static const std::map<int, std::vector<double>> butter = {{2, {0.7071}}, {4, {0.5412, 1.3066}}, {6, {0.5176, 0.7071, 1.9319}},
                                                              {8, {0.5098, 0.6013, 0.9, 2.5629}}};
    int order = std::max(1, (int)std::lround(slopeDb / 6.0));
    if (order % 2) { bands.push_back({{"type", type}, {"freq", r2(freq)}, {"q", 0.5}}); --order; }
    if (!order) return;
    std::vector<double> qs = butter.count(order) ? butter.at(order) : butter.at(8);
    qs.back() = r4(qs.back() * (q > 0 ? q : 0.7071) / 0.7071);
    for (double x : qs) bands.push_back({{"type", type}, {"freq", r2(freq)}, {"q", x}});
}
double dbLin(double db) { return db <= -96 ? 0.0 : std::pow(10.0, db / 20); }
// independent dry and wet levels (linear) as Wavelength's crossfade mix and a make-up gain (0 = none)
std::pair<double, double> mixGain(double dry, double wet) {
    if (dry + wet <= 0) return {0, 0};
    const double g = 20 * std::log10(dry + wet);
    return {r4(wet / (dry + wet)), std::fabs(g) > 0.05 ? r2(g) : 0};
}
// a reverb's damping from its high cut: 1 kHz and below fully damped, 10 kHz and up open
double damping(double highCutHz) { return r2(std::clamp(1 - std::log10(std::max(highCutHz, 1000.0) / 1000), 0.0, 1.0)); }
} // namespace

json patchEffects(const std::vector<PatchPlugin> &chain, std::vector<std::string> &notes) {
    json fx = json::array();
    for (auto &p : chain) {
        auto v = [&](size_t n, double def = 0) { return n < p.params.size() ? (double)p.params[n] : def; };
        auto add = [&](const json &e) { fx.push_back(e); };
        auto addMixGain = [&](json e, double dry, double wet) {
            const auto [mix, g] = mixGain(dry, wet);
            e["mix"] = mix;
            add(e);
            if (g != 0) add({{"type", "gain"}, {"db", g}});
        };
        if (p.bypassed) { notes.push_back(p.name + ": switched off in the patch (a Smart Control knob turns it on), left out"); continue; }
        if (p.params.empty()) { notes.push_back(p.name + ": no settings saved, left out"); continue; }
        if (p.name == "Channel EQ") {   // 8 bands of (on, Hz, gain dB or slope n = 6n dB/oct, Q) from #0; master gain #32
            json bands = json::array();
            if (v(0)) cutStages(bands, "highpass", v(1), (int)std::lround(v(2)) * 6, v(3));
            const char *kinds[6] = {"lowshelf", "peak", "peak", "peak", "peak", "highshelf"};
            for (int b = 1; b <= 6; ++b)
                if (v(4 * b) && v(4 * b + 2) != 0)
                    bands.push_back({{"type", kinds[b - 1]}, {"freq", r2(v(4 * b + 1))}, {"gain", r2(v(4 * b + 2))}, {"q", r4(v(4 * b + 3))}});
            if (v(28)) cutStages(bands, "lowpass", v(29), (int)std::lround(v(30)) * 6, v(31));
            const bool onlyLowCut = v(0) && std::all_of(bands.begin(), bands.end(), [](const json &b) { return b["type"] == "highpass"; });
            if ((int)std::lround(v(45)) == 4 && onlyLowCut)   // #45 = 4 read as side-only processing: a low cut on the sides = mono below it
                fx.push_back({{"type", "width"}, {"monoBelow", r2(v(1))}});
            else if (!bands.empty()) fx.push_back({{"type", "eq"}, {"bands", bands}});
            if (v(32) != 0) fx.push_back({{"type", "gain"}, {"db", r2(v(32))}});
            if (v(41) && std::any_of(bands.begin(), bands.end(), [](const json &b) { return b["type"] != "highpass" && b["type"] != "lowpass"; }))
                notes.push_back("Channel EQ: Gain-Q coupling is on in GarageBand (Q as saved here)");
        } else if (p.name == "Single EQ") {   // Single Band EQ, as channel strips name it
            const int mode = (int)std::lround(v(0));
            json bands = json::array();
            if (mode == 0) cutStages(bands, "highpass", v(1), (int)std::lround(v(2)) * 6, v(4));
            else if (mode == 4) cutStages(bands, "lowpass", v(1), (int)std::lround(v(2)) * 6, v(4));
            else if (v(3) != 0) bands.push_back({{"type", mode == 1 ? "lowshelf" : mode == 3 ? "highshelf" : "peak"}, {"freq", r2(v(1))}, {"gain", r2(v(3))}, {"q", r4(v(5, 0.7071))}});
            if (!bands.empty()) fx.push_back({{"type", "eq"}, {"bands", bands}});
        } else if (p.name == "Compressor") {
            // #0 threshold, #1 ratio, #2 attack ms, #3 release ms, #4 make-up, #5 knee 0-1, #7 auto gain (off / 0 dB / -12 dB),
            // #8 output distortion (off / soft / hard / clip), #11 limiter threshold, #12 limiter, #24 mix %, #26/#27 in/out gain
            const double T = v(0, -20), R = std::max(1.0, v(1, 2));
            double makeup = v(4);
            const int autoGain = (int)std::lround(v(7));
            if (autoGain == 1 || autoGain == 2) makeup += -T * (1 - 1 / R) - (autoGain == 2 ? 12 : 0);   // the gain reduction of a full-scale input
            if (v(26) != 0) fx.push_back({{"type", "gain"}, {"db", r2(v(26))}});
            json c = {{"type", "compressor"}, {"threshold", r2(T)}, {"ratio", r2(R)}, {"attack", r2(std::max(0.05, v(2, 10)))},
                      {"release", r2(v(3, 100))}, {"knee", r2(10 * v(5, 0.7))}, {"makeup", r2(makeup)}};
            if (p.params.size() > 24 && v(24) < 100) c["mix"] = r4(v(24) / 100);
            fx.push_back(c);
            if (v(27) != 0) fx.push_back({{"type", "gain"}, {"db", r2(v(27))}});
            const int dist = (int)std::lround(v(8));
            if (dist >= 1 && dist <= 3) fx.push_back({{"type", "clip"}, {"ceiling", 0}, {"kneeDb", dist == 1 ? 3.0 : dist == 2 ? 1.0 : 0.1}, {"intended", true}});
            if (v(12)) fx.push_back({{"type", "limiter"}, {"ceiling", r2(v(11))}});
            if (v(9) != 0) notes.push_back("Compressor: its circuit type's character is not modelled");
        } else if (p.name == "Tape Delay") {
            // #3 feedback %, #4 high cut, #5 low cut (in the loop), #6 sync, #7 note (1/x of a whole note), #19 dry %, #20 wet %,
            // #22 time ms (unsynced; the legacy 23-value layout has coarse #1 + fine #2 instead)
            const bool legacy = p.params.size() < 25;
            const double dry = (legacy ? 100 : v(19, 100)) / 100, wet = (legacy ? v(0, 30) : v(20)) / 100;
            if (wet <= 0) { notes.push_back("Tape Delay: Wet is 0 as saved (GarageBand's Delay knob raises it), left out"); continue; }
            json d = {{"type", "delay"}, {"feedback", r4(std::min(v(3) / 100, 0.97))}, {"highpass", r2(v(5, 20))}, {"lowpass", r2(v(4, 20000))},
                      {"mix", r4(wet / (dry + wet))}};
            if (v(6) && v(7) > 0) d["time"] = r4(4 / v(7));
            else d["ms"] = r2(std::max(1.0, legacy ? v(1) + v(2) : v(22, 200)));
            fx.push_back(d);
            if (std::fabs(dry + wet - 1) > 1e-3) fx.push_back({{"type", "gain"}, {"db", r2(20 * std::log10(dry + wet))}});
            if (std::fabs(v(8)) > 0.5 && !legacy) notes.push_back("Tape Delay: its groove (swung repeats) is not played");
            if (v(11) || v(13)) notes.push_back("Tape Delay: wow and flutter are not played");
        } else if (p.name == "Overdrive") {   // #0 drive (dB of tanh drive: a guess), #1 tone (a low-pass after it: a guess), #2 output dB
            if (v(0) > 0) add({{"type", "saturate"}, {"drive", r2(v(0))}});
            else notes.push_back("Overdrive: Drive 0 as saved (a Smart Control knob raises it)");
            if (v(1, 20000) < 19000) add({{"type", "eq"}, {"bands", json::array({{{"type", "lowpass"}, {"freq", r2(v(1))}, {"q", 0.7071}}})}});
            if (v(2) != 0) add({{"type", "gain"}, {"db", r2(v(2))}});
        } else if (p.name == "Bitcrusher") {   // #0 drive dB, #1 clip level dB, #3 bits, #4 downsampling, #5 mix %
            if (v(0) != 0) add({{"type", "gain"}, {"db", r2(v(0))}});
            if (v(0) != 0 || v(1) < 0) add({{"type", "clip"}, {"ceiling", r2(v(1))}, {"kneeDb", 0.1}, {"intended", true}});
            const double mix = v(5, 100) / 100;
            if (mix > 0 && (v(3, 24) < 24 || v(4, 1) > 1)) {
                json b = {{"type", "bitcrush"}, {"bits", r2(v(3, 24))}, {"downsample", r2(v(4, 1))}};
                if (mix < 1) b["mix"] = r4(mix);
                add(b);
            }
        } else if (p.name == "Chorus") {   // #0 mix %, #1 intensity % (0.08 ms of depth per %: a guess), #2 rate Hz
            if (v(0) > 0) add({{"type", "chorus"}, {"rate", r4(v(2, 0.5))}, {"depth", r2(v(1) * 0.08)}, {"delay", 10}, {"mix", r4(v(0) / 100)}});
            else notes.push_back("Chorus: Mix 0 as saved (a Smart Control knob raises it)");
        } else if (p.name == "Ensemble") {   // #0 mix %, #3 LFO 1 rate, #4 LFO 1 intensity, #10 spread %
            if (v(0) > 0) {
                add({{"type", "chorus"}, {"rate", r4(v(3, 0.5))}, {"depth", r2(v(4, 20) * 0.1)}, {"delay", 12}, {"mix", r4(v(0) / 100)}});
                if (v(10, 100) > 100) add({{"type", "width"}, {"amount", r2(std::min(2.0, v(10) / 100))}});
                notes.push_back("Ensemble: its voices and second LFO play as one chorus");
            }
        } else if (p.name == "Flanger") {   // #0 mix %, #1 intensity %, #2 rate Hz, #3 feedback % (no feedback here)
            if (v(0) > 0) {
                add({{"type", "chorus"}, {"rate", r4(v(2, 0.2))}, {"depth", r2(0.5 + v(1, 50) * 0.05)}, {"delay", 0.5}, {"mix", r4(v(0) / 100)}});
                notes.push_back("Flanger: played as a short chorus, without its feedback");
            }
        } else if (p.name == "Tremolo") {   // #0 depth %, #1 rate Hz (a note index when synced, #8), #3 smoothing %, #4 stereo phase degrees
            if (v(0) > 0) {
                json t = {{"type", "tremolo"}, {"depth", r4(v(0) / 100)}, {"shape", v(3, 100) >= 50 ? "sine" : "square"},
                          {"spread", r4(std::fmod(v(4), 360.0) / 360)}};
                if (v(8) != 0) { t["rate"] = "1/8"; notes.push_back("Tremolo: its synced rate plays as 1/8"); }
                else t["rate"] = r4(v(1, 1));
                add(t);
            }
        } else if (p.name == "ClipDist") {   // Clip Distortion: #0 drive dB, #1 tone Hz (high-pass before), #3 mix %, #4 clip filter, #5 LP filter,
                                             // #6/#7 high shelf dB/Hz, #8/#9 input/output gain
            const double mix = v(3, 50) / 100;
            const bool wetOnly = mix >= 0.99;
            if (v(8) != 0) add({{"type", "gain"}, {"db", r2(v(8))}});
            if (wetOnly && v(1, 20) > 25) add({{"type", "eq"}, {"bands", json::array({{{"type", "highpass"}, {"freq", r2(v(1))}, {"q", 0.5}}})}});
            if (mix > 0) add({{"type", "saturate"}, {"drive", r2(v(0))}, {"mix", r4(mix)}});
            if (wetOnly && v(4, 20000) < 19000) add({{"type", "eq"}, {"bands", json::array({{{"type", "lowpass"}, {"freq", r2(v(4))}, {"q", 0.5}}})}});
            json bands = json::array();
            if (v(5, 20000) < 19000) bands.push_back({{"type", "lowpass"}, {"freq", r2(v(5))}, {"q", 0.7071}});
            if (v(6) != 0) bands.push_back({{"type", "highshelf"}, {"freq", r2(v(7, 3100))}, {"gain", r2(v(6))}, {"q", 0.7071}});
            if (!bands.empty()) add({{"type", "eq"}, {"bands", bands}});
            if (v(9) != 0) add({{"type", "gain"}, {"db", r2(v(9))}});
        } else if (p.name == "Limiter") {   // #0 gain dB, #1 lookahead ms, #3 release ms, #4 output dB, #6 inter-sample peaks
            if (v(0) != 0) add({{"type", "gain"}, {"db", r2(v(0))}});
            add({{"type", "limiter"}, {"ceiling", r2(v(4))}, {"release", r2(std::max(1.0, v(3, 80)))}, {"lookahead", r2(v(1, 5))}, {"truePeak", v(6) != 0}});
        } else if (p.name == "Gain") {   // #2 balance, #4 gain dB, #8 mono (polarity and swap aren't played)
            if (v(4) != 0) add({{"type", "gain"}, {"db", r2(v(4))}});
            if (v(8) != 0) add({{"type", "width"}, {"amount", 0}});
            if (v(2) != 0) add({{"type", "pan"}, {"position", r4(v(2) / 100)}});
            if (v(5) != 0 || v(6) != 0) notes.push_back("Gain: its polarity flip is not played");
        } else if (p.name == "Enveloper") {   // transient shaping isn't played: only its output level (#6)
            if (v(6) != 0) add({{"type", "gain"}, {"db", r2(v(6))}});
            notes.push_back("Enveloper: its transient shaping is not played");
        } else if (p.name == "St-Delay") {   // Stereo Delay: one delay from the left side; ping-pong when its crossfeed bounces
            const double mix = (v(0) + v(1)) / 200;
            if (mix <= 0) { notes.push_back("Stereo Delay: Mix 0 as saved (a Smart Control knob raises it), left out"); continue; }
            json d = {{"type", "delay"}};
            if (v(10) != 0 && v(11) > 0) d["time"] = r4(4 / v(11) * (1 + v(12) / 100));
            else d["ms"] = r2(std::max(1.0, v(2)));
            double fb = (v(4) + v(5)) / 200;
            const double xf = std::max(v(6), v(7)) / 100;
            const int inL = (int)std::lround(v(15, 1)), inR = (int)std::lround(v(16, 2));   // 0 off, 1 left, 2 right, 3 L+R, 4 L-R
            const bool pingpong = xf >= 0.5 && (inL == 0 || inR == 0 || (inL == 3 && inR == 3));
            if (pingpong) fb = std::max({v(4), v(5), std::sqrt(std::max(0.0, v(6) * v(7)))}) / 100;
            d["feedback"] = r4(std::min(fb, 0.97));
            d["highpass"] = r2(v(23) > 0 ? v(23) : v(9) > 0 ? v(9) : 20);
            d["lowpass"] = r2(v(22) > 0 ? v(22) : v(8) > 0 ? v(8) : 20000);
            d["pingpong"] = pingpong;
            d["mix"] = r4(mix);
            add(d);
            if (std::fabs(v(2) - v(3)) > 1 || std::fabs(v(11) - v(13)) > 1e-3) notes.push_back("Stereo Delay: its two sides' times differ, the left one plays");
        } else if (p.name == "Delay D") {   // Delay Designer: values[0..7] = sync, grid, swing, feedback on, feedback tap, feedback dB, dry dB,
                                            // wet dB; then chunks "TapA".."TapZ" of 20 floats (ms, steps, level dB, mute, pan, ..., HP, LP, filter on)
            auto val = [&](size_t i, double def = 0) { return i < p.values.size() ? (double)p.values[i] : def; };
            struct Tap { double ms, steps, hp, lp; bool mute, filter; };
            std::vector<Tap> taps;
            for (size_t i = 24 + 4 * p.values.size(); i + 8 <= p.block.size();) {
                const uint32_t n = le32(&p.block[i + 4]);
                if (n < 8 || i + n > p.block.size()) break;
                const bool tap = p.block[i + 3] == 'T' && p.block[i + 2] == 'a' && p.block[i + 1] == 'p';   // "TapX", stored reversed
                if (tap && n >= 8 + 80) {
                    float f[20];
                    std::memcpy(f, &p.block[i + 8], 80);
                    taps.push_back({f[0], f[1], f[13], f[14], f[3] != 0, f[15] != 0});
                }
                i += n;
            }
            const double dry = dbLin(val(6, -6)), wet = dbLin(val(7, -12));
            const bool fbOn = val(3) != 0;
            const size_t idx = (size_t)std::max(0.0, val(4));
            const Tap *t = fbOn && idx < taps.size() ? &taps[idx] : nullptr;
            for (auto &x : taps) if (!t && !x.mute) t = &x;
            if (!t || wet <= 0) { notes.push_back("Delay Designer: no tap or Wet off as saved, left out"); continue; }
            json d = {{"type", "delay"}};
            if (val(0) != 0 && t->steps > 0 && val(1) > 0) d["time"] = r4(t->steps * val(1) * 4);
            else d["ms"] = r2(std::max(1.0, t->ms));
            d["feedback"] = fbOn ? r4(std::min(dbLin(val(5, -100)), 0.97)) : 0.0;
            d["highpass"] = t->filter ? r2(t->hp) : 20.0;
            d["lowpass"] = t->filter ? r2(t->lp) : 20000.0;
            addMixGain(d, dry, wet);
            if (taps.size() > 1) notes.push_back("Delay Designer: " + std::to_string(taps.size()) + " taps, one plays");
        } else if (p.name == "Space D") {   // Space Designer: its room by name (the IR file, as convolve plays it) or, synthesized, a reverb
            auto w = [&](size_t i) -> double {   // word i of the block (words past its values sit in the trailing structure)
                if (24 + 4 * i + 4 > p.block.size()) return 0;
                float f;
                std::memcpy(&f, &p.block[24 + 4 * i], 4);
                return std::isfinite(f) ? f : 0;
            };
            std::string ir;
            if (p.block.size() > 31) ir.assign(reinterpret_cast<const char *>(&p.block[31]), std::min<size_t>(p.block[30], p.block.size() - 31));
            if (const size_t dot = ir.rfind('.'); dot != std::string::npos) ir.resize(dot);
            const double dry = dbLin(w(20)), wet = dbLin(w(21)), predelay = w(22), length = w(23);
            if (w(111) != 0 && !ir.empty()) {   // a sampled IR: Length is % of it
                json c = {{"type", "convolve"}, {"ir", ir}};
                if (predelay > 0) c["predelay"] = r2(std::min(predelay, 500.0));
                const double secs = std::atof(ir.c_str());   // "06.6s Botta Church-OST": 6.6 s
                if (length > 0 && length < 99.5 && secs > 0) c["length"] = r2(secs * length / 100);
                addMixGain(c, dry, wet);
            } else addMixGain({{"type", "reverb"}, {"decay", r2(length > 0 ? length : 2.0)}, {"predelay", r2(predelay)}}, dry, wet);
            if (w(28) != 0) notes.push_back("Space Designer: its reversed IR plays forwards");
        } else if (p.name == "SilverVerb") {   // #1 predelay, #3 room size, #4 high cut, #5 low cut, #6 density/time, #10 dry, #11 wet
            const double size = v(3, 70), decay = 0.3 + 3.2 * (size / 200) * (0.3 + 0.7 * v(6, 100) / 100);
            const double dry = (p.params.size() > 10 ? v(10) : 100 - v(0, 30)) / 100, wet = (p.params.size() > 11 ? v(11) : v(0, 30)) / 100;
            addMixGain({{"type", "reverb"}, {"decay", r2(decay)}, {"size", r2(std::min(1.0, size / 200))}, {"predelay", r2(v(1, 20))},
                        {"damping", damping(v(4, 12000))}, {"highpass", r2(v(5, 20))}}, dry, wet);
        } else if (p.name == "PtVerb") {   // PlatinumVerb: #1 predelay, #2 room size m, #5 ER/reverb balance, #6 initial delay, #7 spread, #8 time,
                                           // #12 high cut, #18 dry %, #19 wet %
            addMixGain({{"type", "reverb"}, {"decay", r2(v(8, 2))}, {"size", r2(std::min(1.0, v(2, 20) / 100))},
                        {"predelay", r2(v(1) + v(6) * v(5, 50) / 100)}, {"damping", damping(v(12, 6000))}, {"width", r2(std::min(1.0, v(7, 100) / 100))}},
                       v(18, 100) / 100, v(19, 20) / 100);
        } else if (p.name == "ChromaVerb" || p.name == "AlgoVerb") {   // #0 dry %, #1 wet %, #3 predelay, #5 decay s, #8 size, #12 width,
                                                                        // #13/#14 mono maker, #33 damping high-shelf ratio, #36.. output EQ
            const double dry = v(0) / 100, wet = v(1, 100) / 100;
            addMixGain({{"type", "reverb"}, {"decay", r2(v(5, 2))}, {"size", r2(std::min(1.0, v(8, 50) / 100))}, {"predelay", r2(v(3))},
                        {"damping", r2(std::clamp(1 - v(33, 1) * 0.5, 0.0, 1.0))}, {"width", r2(std::min(1.0, v(12, 100) / 100))}}, dry, wet);
            json bands = json::array();
            if (v(36) != 0 && dry == 0) {   // the output EQ shapes the wet signal: with no dry signal it applies to all of it
                if (v(37) != 0) bands.push_back({{"type", "highpass"}, {"freq", r2(v(38, 20))}, {"q", r4(v(40, 0.71))}});
                const char *kinds[4] = {"lowshelf", "peak", "peak", "highshelf"};
                for (size_t k = 0; k < 4; ++k) {
                    const size_t b = 41 + 4 * k;
                    if (v(b) != 0 && v(b + 2) != 0) bands.push_back({{"type", kinds[k]}, {"freq", r2(v(b + 1))}, {"gain", r2(v(b + 2))}, {"q", r4(v(b + 3, 0.71))}});
                }
                if (v(57) != 0) bands.push_back({{"type", "lowpass"}, {"freq", r2(v(58, 20000))}, {"q", r4(v(60, 0.71))}});
            }
            if (!bands.empty()) add({{"type", "eq"}, {"bands", bands}});
            if (v(13) != 0) add({{"type", "width"}, {"monoBelow", r2(v(14, 120))}});
            notes.push_back(p.name + ": its room type plays as the built-in reverb");
        } else if (p.name == "Noise Gate") {   // #0 threshold dB, #1 hysteresis dB, #2 reduction dB, #3 attack, #4 hold, #5 release ms, #8 lookahead
            if (v(0, -100) > -90)
                add({{"type", "gate"}, {"threshold", r2(v(0))}, {"hysteresis", r2(std::fabs(v(1, -3)))}, {"depth", r2(std::fabs(v(2, -100)))},
                     {"attack", r2(std::max(0.1, v(3)))}, {"hold", r2(v(4))}, {"release", r2(std::max(0.5, v(5)))}, {"lookahead", r2(v(8))}});
        } else if (p.name == "Phaser") {   // #0 LFO rate Hz, #3 feedback %, #4/#5 sweep floor/ceiling Hz, #6 stages, #9 output mix +-100
            if (std::fabs(v(9)) > 0.5)
                add({{"type", "phaser"}, {"rate", r4(std::max(0.02, v(0, 0.5)))}, {"floor", r2(std::max(20.0, v(4, 200)))},
                     {"ceiling", r2(std::max(v(4, 200) * 1.05, v(5, 4000)))}, {"stages", (int)std::lround(std::clamp(v(6, 6), 2.0, 24.0))},
                     {"feedback", r4(std::clamp(v(3) / 100, -0.95, 0.95))}, {"mix", r4(std::min(1.0, std::fabs(v(9)) / 100))}});
            if (v(9) < 0) notes.push_back("Phaser: its phase-inverted mix plays in phase");
        } else {
            notes.push_back(p.name + ": GarageBand's own effect, not played");
        }
    }
    return fx;
}


json patchChainEffects(const std::vector<PatchChannel> &chans, std::vector<std::string> &notes) {
    const PatchChannel *inst = nullptr;
    int n = 0;
    for (auto &c : chans)
        if (!c.instrument.empty()) { if (!inst) inst = &c; ++n; }
    std::vector<PatchPlugin> chain;
    if (n == 1) {
        chain = inst->chain;
        for (auto &m : inst->midiChain)
            if (m.name == "Arpeggiator") { if (!m.bypassed) notes.push_back("Arpeggiator: plays as the track's \"arp\" (\"arp\": false plays the notes as written)"); }
            else notes.push_back(m.name + ": a MIDI effect, not played (the notes play as written)");
    } else if (n > 1) notes.push_back("several instrument channels: their own effects are left out");
    if (!chans.empty() && chans.front().instrument.empty() && &chans.front() != inst) chain.insert(chain.end(), chans.front().chain.begin(), chans.front().chain.end());
    return patchEffects(chain, notes);
}

std::vector<PatchSend> readPatchSends(const std::string &patchDir) {
    std::vector<PatchSend> out;
    std::string text;
    if (!platform::plistToJson((fs::path(patchDir) / "data.plist").string(), text)) return out;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("channels") || !j["channels"].is_array()) return out;
    for (auto &ch : j["channels"]) {
        if (!ch.is_object() || !ch.contains("Channel_sends") || !ch["Channel_sends"].is_array()) continue;
        for (auto &snd : ch["Channel_sends"]) {
            if (!snd.is_object() || !snd.value("auxTag", std::string()).size() || snd.value("sendIsMuted", false)) continue;
            const double v = snd.value("sendVolume", 0.0);
            if (v <= 0) continue;
            PatchSend p;
            p.channel = ch.value("Channel_name", std::string());
            p.aux = snd["auxTag"].get<std::string>();
            p.room = p.aux.substr(p.aux.find('/') == std::string::npos ? 0 : p.aux.find('/') + 1);
            p.db = 20 * std::log10(v);
            out.push_back(p);
        }
    }
    return out;
}

bool isSamplerInstrument(const std::string &name) { return name == "Sampler" || name == "EXS24" || name == "Drum Kit"; }

std::vector<std::string> logicPatchRoots() {
    std::vector<std::string> out;
#if defined(__APPLE__)
    std::error_code ec;
    for (const fs::path &p : {fs::path("/Applications/GarageBand.app/Contents/Resources/Patches/Instrument"),
                              fs::path("/Applications/Logic Pro.app/Contents/Resources/Patches/Instrument"),
                              fs::path("/Applications/Logic Pro X.app/Contents/Resources/Patches/Instrument"),
                              fs::path("/Library/Application Support/Logic/Patches/Instrument"),
                              platform::homeDir() / "Music/Audio Music Apps/Patches/Instrument"})
        if (fs::is_directory(p, ec)) out.push_back(p.string());
#endif
    for (auto &p : platform::envPathList("WAVELENGTH_LOGIC_PATCHES")) out.push_back(p);
    return out;
}

const std::vector<LogicPatch> &logicPatches() {
    static std::vector<LogicPatch> patches;
    static std::once_flag once;
    std::call_once(once, [] {
        for (auto &root : logicPatchRoots()) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_directory(ec) || lower(it->path().extension().string()) != ".patch") continue;
                it.disable_recursion_pending();
                LogicPatch p;
                p.name = it->path().stem().u8string();
                p.path = it->path().string();
                const fs::path rel = it->path().lexically_relative(root);
                p.category = rel.begin() != rel.end() && std::next(rel.begin()) != rel.end() ? rel.begin()->u8string() : "";
                for (auto &f : channelFiles(p.path)) {
                    std::vector<uint8_t> d;
                    std::vector<Record> recs;
                    if (!readWhole(f, d) || !records(d, recs)) continue;
                    for (auto &x : recs) p.arpeggiator |= x.midiFx && x.name == "Arpeggiator" && !x.bypassed;
                    const Record *r = instrumentOf(recs);
                    if (!r) continue;
                    if (p.instrument.empty() || (!p.sampler && isSamplerInstrument(r->name))) p.instrument = r->name;
                    p.sampler |= isSamplerInstrument(r->name);
                }
                if (!p.instrument.empty()) patches.push_back(p);
            }
        }
        std::stable_sort(patches.begin(), patches.end(), [](const LogicPatch &a, const LogicPatch &b) { return lower(a.name) < lower(b.name); });
    });
    return patches;
}

const LogicPatch *logicPatchNamed(const std::string &name) {
    std::string q = lower(name);
    if (q.size() > 6 && q.compare(q.size() - 6, 6, ".patch") == 0) q.resize(q.size() - 6);
    for (auto &p : logicPatches()) if (lower(p.name) == q) return &p;
    return nullptr;
}

bool readPatchChannels(const std::string &patchDir, std::vector<PatchChannel> &out, std::string &err) {
    out.clear();
    for (auto &f : channelFiles(patchDir)) {
        std::vector<uint8_t> d;
        std::vector<Record> recs;
        if (!readWhole(f, d) || !records(d, recs)) continue;
        PatchChannel c;
        c.file = f;
        const Record *r = instrumentOf(recs);
        if (r) { c.instrument = r->name; c.preset = r->preset; c.settings = settingsOf(d, r->at, std::min(d.size(), r->at + r->size), r->name); }
        if (r && r->name == "Alchemy") c.alchemy = alchemyText(d, r->at, std::min(d.size(), r->at + r->size));
        // the audio effects in their insert order (records aren't stored in it); MIDI effects are named, not played
        std::vector<const Record *> fx;
        for (auto &x : recs) {
            if (x.name.empty() || &x == r) continue;
            c.effects.push_back(x.name);
            if (!x.midiFx) { fx.push_back(&x); continue; }
            c.midiEffects.push_back(x.name);
            c.midiChain.push_back(settingsOf(d, x.at, std::min(d.size(), x.at + x.size), x.name));
            c.midiChain.back().bypassed = x.bypassed;
        }
        std::stable_sort(fx.begin(), fx.end(), [](const Record *a, const Record *b) { return a->order < b->order; });
        for (auto *x : fx) {
            c.chain.push_back(settingsOf(d, x->at, std::min(d.size(), x->at + x->size), x->name));
            c.chain.back().bypassed = x->bypassed;
        }
        c.sampler = r && isSamplerInstrument(r->name);
        if (c.sampler) {
            c.data.swap(d);
            const size_t a = r->at, b = std::min(c.data.size(), r->at + r->size);
            // an instrument stored inside: the first EXS chunk header (its magic sits 16 bytes in)
            for (size_t p = a; p + 20 <= b; ++p)
                if ((c.data[p] == 'T' && !std::memcmp(&c.data[p], "TBOS", 4)) || (c.data[p] == 'S' && !std::memcmp(&c.data[p], "SOBT", 4)) ||
                    (c.data[p] == 'J' && !std::memcmp(&c.data[p], "JBOS", 4)) || (c.data[p] == 'S' && !std::memcmp(&c.data[p], "SOBJ", 4))) {
                    if (p >= a + 16) c.exsAt = p - 16;
                    break;
                }
            // else the instrument it names: "MELC" "PMAS" + 4 bytes + "<name>.exs"
            if (!c.exsAt)
                for (size_t p = a; p + 12 < b; ++p)
                    if (!std::memcmp(&c.data[p], "MELCPMAS", 8)) {
                        std::string n;
                        for (size_t k = p + 12; k < b && c.data[k] >= 32 && c.data[k] < 127; ++k) n += (char)c.data[k];
                        if (lower(fs::path(n).extension().string()) == ".exs") { c.exs = n; break; }
                    }
        }
        out.push_back(std::move(c));
    }
    if (out.empty()) { err = patchDir + " has no channel strips"; return false; }
    return true;
}

std::vector<std::string> pluginSettingsRoots() {
    std::vector<std::string> out;
#if defined(__APPLE__)
    std::error_code ec;
    for (const fs::path &p : {fs::path("/Applications/GarageBand.app/Contents/Resources/Plug-In Settings"),
                              fs::path("/Applications/Logic Pro.app/Contents/Resources/Plug-In Settings"),
                              fs::path("/Library/Application Support/Logic/Plug-In Settings"),
                              platform::homeDir() / "Music/Audio Music Apps/Plug-In Settings"})
        if (fs::is_directory(p, ec)) out.push_back(p.string());
#endif
    for (auto &p : platform::envPathList("WAVELENGTH_PLUGIN_SETTINGS")) out.push_back(p);
    return out;
}

const std::vector<std::pair<std::string, std::string>> &arpeggiatorPresets() {
    static std::vector<std::pair<std::string, std::string>> list;
    static std::once_flag once;
    std::call_once(once, [] {
        std::set<std::string> seen;
        for (auto &root : pluginSettingsRoots()) {
            std::error_code ec;
            for (auto &e : fs::directory_iterator(fs::path(root) / "Arpeggiator", ec))
                if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".pst" && seen.insert(lower(e.path().stem().u8string())).second)
                    list.push_back({e.path().stem().u8string(), e.path().string()});
        }
        std::sort(list.begin(), list.end(), [](auto &a, auto &b) { return lower(a.first) < lower(b.first); });
    });
    return list;
}

json arpeggiatorSettings(const PatchPlugin &p, std::vector<std::string> &notes) {
    auto v = [&](size_t n, double def = 0) { return n < p.params.size() && p.params[n] < 1e29f ? (double)p.params[n] : def; };
    if (p.bypassed || v(0, 1) < 0.5) return nullptr;   // #0 Play/Stop
    json a = json::object();
    // #4 rate: numerator + denominator / 1000 (1.016 = 1/16, 1.024 = 1/16 triplet, 3.016 = dotted 1/16)
    const double rate = v(4, 1.016);
    const int num = (int)std::floor(rate + 1e-4), den = (int)std::lround((rate - num) * 1000);
    if (num < 1 || den < 1) a["rate"] = "1/16";
    else if (num == 1 && (den & (den - 1)) == 0) a["rate"] = "1/" + std::to_string(den);
    else if (num == 1 && den % 3 == 0) a["rate"] = "1/" + std::to_string(den / 3 * 2) + "T";
    else if (num == 3 && den % 2 == 0) a["rate"] = "1/" + std::to_string(den / 2) + "D";
    else a["rate"] = r4(4.0 * num / den);
    static const char *orders[] = {"up", "down", "updown", "outsidein", "random", "played"};   // #6 Note Order
    a["order"] = orders[std::clamp((int)std::lround(v(6)), 0, 5)];
    if (const int var = std::clamp((int)std::lround(v(7, 1)), 1, 4); var != 1) a["variation"] = var;   // #7 Variation (1-4)
    if (const int oct = std::clamp((int)std::lround(v(9, 1)), 1, 4); oct != 1) a["octaves"] = oct;     // #9 Octave Range
    if (v(10) != 0) a["inversions"] = true;   // #10 Octaving Mode: 1 = inversions (5, in two patches, read as inversions)
    a["gate"] = r4(std::clamp(v(15, 90), 1.0, 150.0) / 100);   // #15 Note Length %
    if (v(16) > 0) a["lengthRandom"] = r4(std::min(100.0, v(16)) / 100);   // #16 Note Length Random %
    if (v(18, 50) > 50) a["swing"] = r4(std::min(99.0, v(18)) / 100);       // #18 Swing (50 = straight)
    // #19 velocity base, #20 range %, #21 alteration (0 random, 1 (de-)crescendo), #23 random %
    const double range = std::clamp(v(20, 100), 0.0, 100.0), random = v(21) < 0.5 ? std::clamp(v(23), 0.0, 100.0) : 0;
    if (range < 100 || random > 0) a["velocity"] = {{"base", r4(std::clamp(v(19, 80), 1.0, 127.0) / 127)}, {"range", r4(range / 100)}, {"random", r4(random / 100)}};
    if (v(21) >= 0.5 && v(22) != 0) notes.push_back("Arpeggiator: its velocity (de-)crescendo is left out");
    if (const int cyc = (int)std::lround(v(14)); cyc < 0) a["cycle"] = "grid";   // #14 Cycle Length: 0 as played, -1 by grid, 1-32 notes
    else if (cyc > 0) a["cycle"] = std::min(cyc, 32);
    if (v(11) >= 0.5) {   // #11 Grid: the rhythm grid in the "UGCD" chunk (a binary property list) after the values
        json grid;
        const size_t count = p.values.size();
        for (size_t at = 24 + 4 * count; at + 8 <= p.block.size();) {
            const uint32_t size = le32(&p.block[at + 4]);
            if (size < 8 || at + size > p.block.size()) break;
            if (!std::memcmp(&p.block[at], "DCGU", 4) && parseBinaryPlist(&p.block[at + 8], size - 8, grid)) break;
            at += size;
        }
        if (grid.is_object() && grid.contains("Steps") && grid["Steps"].is_array()) {
            // each step fills ceil(Length) slots (a Length over 1 ties), the first #13 Active Grid Length slots play
            const int active = std::clamp((int)std::lround(v(13, grid.value("ActiveSteps", 16.0))), 1, 128);
            json steps = json::array();
            int slot = 0;
            for (auto &st : grid["Steps"]) {
                if (slot >= active || !st.is_object()) break;
                const std::string type = st.value("Type", std::string("Rest"));
                double len = std::clamp(st.value("Length", 1.0), 0.01, 4.0);
                const int span = std::max(1, (int)std::ceil(len - 1e-6));
                len = std::min(len, (double)(active - slot));
                const double vel = r4(std::clamp(st.value("Velocity", 80.0), 1.0, 127.0) / 127);
                if (type == "Rest") steps.push_back("rest");
                else if (type == "Chord") steps.push_back({{"chord", true}, {"vel", vel}, {"len", r4(len)}});
                else if (std::fabs(len - 1) < 1e-6) steps.push_back(vel);
                else steps.push_back({{"vel", vel}, {"len", r4(len)}});
                if (type == "Rest") for (int k = 1; k < span && slot + k < active; ++k) steps.push_back("rest");
                slot += span;
            }
            for (; slot < active; ++slot) steps.push_back("rest");
            a["steps"] = steps;
        } else notes.push_back("Arpeggiator: its rhythm grid couldn't be read (plays every step)");
    }
    if (v(2) >= 0.5) notes.push_back("Arpeggiator: Latch is left out (the arpeggio plays while notes are held)");
    if (v(37) >= 0.5) notes.push_back("Arpeggiator: its keyboard split is left out");
    if ((int)std::lround(v(27, 3)) != 3) notes.push_back("Arpeggiator: its scale snapping is left out");
    return a;
}

bool appleArpeggiator(const std::string &name, bool preset, json &arp, std::vector<std::string> &notes, std::string &err) {
    if (preset) {
        const std::string q = lower(name);
        for (auto &[n, path] : arpeggiatorPresets()) {
            if (lower(n) != q) continue;
            std::vector<uint8_t> d;
            if (!readWhole(path, d)) { err = "can't read " + path; return false; }
            PatchPlugin p = settingsOf(d, 0, d.size(), "Arpeggiator");
            if (p.id != 300) { err = path + " isn't Arpeggiator settings"; return false; }
            arp = arpeggiatorSettings(p, notes);
            if (arp.is_null()) { err = "the Arpeggiator preset '" + n + "' is switched off"; return false; }
            return true;
        }
        err = "no Arpeggiator preset '" + name + "' (`wavelength presets arp` lists them)";
        return false;
    }
    std::error_code ec;
    std::string dir = name, shown = fs::u8path(name).stem().u8string();   // a patch folder, or a patch by name
    if (!fs::is_directory(fs::u8path(name), ec)) {
        const LogicPatch *lp = logicPatchNamed(name);
        if (!lp) { err = "no GarageBand or Logic patch '" + name + "'"; return false; }
        dir = lp->path, shown = lp->name;
    }
    std::vector<PatchChannel> chans;
    if (!readPatchChannels(dir, chans, err)) return false;
    for (auto &c : chans)
        for (auto &m : c.midiChain)
            if (m.name == "Arpeggiator" && m.id == 300 && !m.bypassed) {
                arp = arpeggiatorSettings(m, notes);
                if (!arp.is_null()) return true;
            }
    for (auto &c : chans)   // else Alchemy's own arpeggiator, when it's on
        if (c.instrument == "Alchemy" && !c.alchemy.empty()) {
            AlchemyPatch a = alchemyPatch(c.alchemy, shown);
            if (a.arp.is_null()) continue;
            arp = a.arp;
            notes.insert(notes.end(), a.arpNotes.begin(), a.arpNotes.end());
            return true;
        }
    err = "the patch '" + shown + "' has no Arpeggiator switched on";
    return false;
}

} // namespace wl
