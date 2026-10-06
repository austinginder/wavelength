#include "logic_patches.hpp"

#include "alchemy.hpp"
#include "bplist.hpp"
#include "platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
        // a channel strip's plug-in data (from payload +140) starts with a 32-byte prefix and, for most plug-ins, a
        // table of (default, highest, default) step indices per parameter up to the block
        if (s >= a + 172 && (s - a - 172) % 12 == 0)
            for (size_t t = a + 172; t + 12 <= s; t += 12) { p.steps.push_back((int)le32(&d[t + 4])); p.stepDefaults.push_back((int)le32(&d[t])); }
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

// ---------------------------------------------------------------------------------------------------------------------
// Amp Designer ("Amp"), Bass Amp Designer ("Bass Amp") and Pedalboard as Wavelength's built-in effects.
// Paste into src/logic_patches.cpp inside the second anonymous namespace (after damping(), before patchEffects), then
// add the four branches shown at the end of this file to patchEffects. Uses json, r2, r4, dbLin, le32, PatchPlugin.
//
// Decoded from GarageBand's patches, the presets in Plug-In Settings, CSParameterOrder.plist and Apple's Logic Pro
// Effects guide (FORMAT.md in the research folder). The sound is an approximation: tanh stages ("saturate", level-
// matched), the tone stacks, cabinets and microphones as EQ curves, the spring reverb as Apple's spring impulse
// response (or the built-in reverb), the stompboxes as their nearest built-in effect. No Apple DSP is modelled.
// ---------------------------------------------------------------------------------------------------------------------
json bandOf(const char *type, double freq, double gain, double q) {
    json b = {{"type", type}, {"freq", r2(freq)}, {"q", r4(q)}};
    if (std::strcmp(type, "highpass") && std::strcmp(type, "lowpass") && std::strcmp(type, "bandpass")) b["gain"] = r2(gain);
    return b;
}
// an eq of the bands that do something (cuts always; shelves and peaks of 0.05 dB or more); null when none
json eqOf(const std::vector<json> &bands) {
    json bs = json::array();
    for (auto &b : bands)
        if (!b.is_null() && (b["type"] == "highpass" || b["type"] == "lowpass" || b["type"] == "bandpass" || std::fabs(b.value("gain", 0.0)) >= 0.05))
            bs.push_back(b);
    return bs.empty() ? json() : json{{"type", "eq"}, {"bands", bs}};
}
json gainOf(double db) { return std::fabs(db) >= 0.05 ? json{{"type", "gain"}, {"db", r2(db)}} : json(); }
// a tanh stage and (comp) the gain that takes back the loudness it adds for a DI guitar (bass) at about -21 LUFS with
// peaks near -6 dBFS, measured with Wavelength's saturate: an amp or pedal is level-neutral at that input and, driven,
// compresses what comes in hotter or quieter, as the real ones do
double satGain(double drive, bool bass) {
    static const double D[] = {0, 3, 6, 9, 12, 15, 18, 24, 30, 36, 42, 48};
    static const double gtr[] = {2.3, 3.9, 6.1, 8.7, 11.3, 13.7, 15.9, 19.4, 21.7, 23.1, 24.0, 24.5};
    static const double bas[] = {2.3, 3.8, 5.9, 8.1, 10.3, 12.1, 13.5, 15.4, 17.0, 18.2, 19.1, 19.8};
    const double *t = bass ? bas : gtr, d = std::max(0.0, drive);
    if (d >= 48) return t[11] + 0.08 * (d - 48);
    size_t i = 0;
    while (i + 1 < 12 && D[i + 1] <= d) ++i;
    return t[i] + (t[i + 1] - t[i]) * (d - D[i]) / (D[i + 1] - D[i]);
}
void addSat(json &fx, double drive, double mix = 1, bool comp = true, bool bass = false) {
    json s = {{"type", "saturate"}, {"drive", r2(std::max(0.0, drive))}};
    if (mix < 0.999) s["mix"] = r4(mix);
    fx.push_back(s);
    if (comp) {
        const double g = -20 * std::log10((1 - mix) + mix * std::pow(10.0, satGain(drive, bass) / 20));
        if (std::fabs(g) >= 0.05) fx.push_back({{"type", "gain"}, {"db", r2(g)}});
    }
}
// a 12/24/36 dB/oct low or high cut as Butterworth biquads
void cutInto(std::vector<json> &bands, const char *type, double freq, int slopeDb) {
    static const std::vector<double> q2 = {0.7071}, q4 = {0.5412, 1.3066}, q6 = {0.5176, 0.7071, 1.9319};
    const int order = std::clamp(2 * (int)std::lround(slopeDb / 12.0), 2, 6);
    for (double q : order == 2 ? q2 : order == 4 ? q4 : q6) bands.push_back(bandOf(type, freq, 0, q));
}
void addFx(json &fx, const json &e) { if (!e.is_null()) fx.push_back(e); }
// an amp's or a pedal's filters and tone controls, each held at its input's loudness: Apple's amps and drive pedals keep
// their level as Gain, Drive and the tone controls turn (their Output and Level don't follow them), and a fuzz after a wah
// isn't quieter for it; with the tanh stages' compensation (addSat) Output / Level is the only level change at a DI level
json matched(json fx) {
    for (auto &f : fx) {
        const std::string t = f.value("type", std::string());
        if ((t == "eq" || t == "filter" || t == "autowah") && !f.contains("match")) f["match"] = "static";
    }
    return fx;
}
double toDb(double lin) { return 20 * std::log10(std::max(lin, 1e-9)); }

// a synced rate or time stored as a fraction of a whole note, as Wavelength's note text ("1/8", "1/8T", "1/16D", "2/1",
// else "<fraction>/1", which Wavelength reads too)
std::string noteText(double x) {
    char buf[32];
    if (x >= 1 && std::fabs(x - std::round(x)) < 1e-3) { std::snprintf(buf, sizeof buf, "%d/1", (int)std::lround(x)); return buf; }
    for (int den : {1, 2, 4, 8, 16, 32, 64}) {
        if (std::fabs(x - 1.0 / den) < 1e-4) { std::snprintf(buf, sizeof buf, "1/%d", den); return buf; }
        if (std::fabs(x - 2.0 / 3.0 / den) < 1e-4) { std::snprintf(buf, sizeof buf, "1/%dT", den); return buf; }
        if (std::fabs(x - 1.5 / den) < 1e-4) { std::snprintf(buf, sizeof buf, "1/%dD", den); return buf; }
    }
    std::snprintf(buf, sizeof buf, "%g/1", std::round(x * 1e6) / 1e6);
    return buf;
}
json rateOf(bool sync, double v) { return sync ? json(noteText(v > 0 ? v : 0.125)) : json(r4(std::max(0.01, v))); }
// chorus takes Hz only: a synced rate as Hz at 120 BPM (a whole note = 2 s)
double hzAt120(bool sync, double v) { return sync && v > 0 ? r4(1 / (2 * v)) : r4(std::max(0.01, v)); }

// Apple's spring impulse response (GarageBand's and Logic's sound library, /Library/Audio/Impulse Responses)
bool springIrInstalled() {
    static const bool have = [] {
        std::error_code ec;
        const fs::path rel = "Apple/01 Large Spaces/06 Spring Reverbs/3.7s_Long Spring.SDIR";
        return fs::exists(fs::path("/Library/Audio/Impulse Responses") / rel, ec) ||
               fs::exists(platform::homeDir() / "Library/Audio/Impulse Responses" / rel, ec);
    }();
    return have;
}
json springOrReverb(bool spring, double lp, double decay, double mix, double hp = 200) {
    if (spring && springIrInstalled()) {
        json c = {{"type", "convolve"}, {"ir", "3.7s_Long Spring"}, {"length", r2(decay)}, {"highpass", hp}, {"mix", r4(mix)}};
        if (lp > 0) c["lowpass"] = r2(lp);
        return c;
    }
    return {{"type", "reverb"}, {"decay", r2(decay)}, {"size", spring ? 0.35 : 0.6}, {"predelay", spring ? 0 : 12}, {"damping", damping(lp)},
            {"highpass", hp}, {"mix", r4(mix)}};
}

// A chain blended with its own input, out = (1 - w) x + w chain(x): exact for one effect with a "mix", the magnitude of
// the blend for eq and gain, and effect by effect for a longer chain (an approximation). Stands in for parallel paths.
json blendChain(const json &fx, double w) {
    if (w >= 0.995) return fx;
    json out = json::array();
    if (w <= 0.005) return out;
    static const std::set<std::string> mixable = {"saturate", "chorus", "delay", "reverb", "convolve", "compressor", "filter", "phaser",
                                                  "rotary", "autowah", "bitcrush", "gate", "multiband"};
    auto keep = [&](double g) { return toDb((1 - w) + w * std::pow(10.0, g / 20)); };
    for (auto &f : fx) {
        const std::string t = f.value("type", std::string());
        if (mixable.count(t)) { json g = f; g["mix"] = r4(f.value("mix", 1.0) * w); out.push_back(g); }
        else if (t == "gain") addFx(out, gainOf(keep(f.value("db", 0.0))));
        else if (t == "tremolo") { json g = f; g["depth"] = r4(f.value("depth", 0.5) * w); out.push_back(g); }
        else if (t == "vibrato") { json g = f; g["depth"] = r2(f.value("depth", 20.0) * w); out.push_back(g); }
        else if (t == "eq") {
            std::vector<json> bs;
            std::set<std::pair<std::string, double>> shelves;   // a cut's Butterworth stages become one shelf
            for (auto &b : f["bands"]) {
                const std::string k = b.value("type", std::string());
                const double fr = b.value("freq", 1000.0), q = b.value("q", 0.7071);
                json nb;
                if (k == "highpass") nb = bandOf("lowshelf", fr, toDb(1 - w), 0.7071);
                else if (k == "lowpass") nb = bandOf("highshelf", fr, toDb(1 - w), 0.7071);
                else if (k == "bandpass") { addFx(out, gainOf(toDb(1 - w))); nb = bandOf("peak", fr, -toDb(1 - w), q); }
                else nb = bandOf(k.c_str(), fr, keep(b.value("gain", 0.0)), q);
                const std::string nt = nb["type"];
                if ((k == "highpass" || k == "lowpass") && !shelves.insert({nt, (double)nb["freq"]}).second) continue;
                bs.push_back(nb);
            }
            addFx(out, eqOf(bs));
        } else if (t == "clip" || t == "limiter") { if (w >= 0.5) out.push_back(f); }
        else out.push_back(f);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- Amp Designer
// #1 Gain, #2 Bass, #3 Mids, #4 Treble, #5 Presence, #6 Master (0-10), #7 Output dB, #9 Model (0 = a custom combo),
// #10 Amp, #11 EQ type, #12 Cabinet, #13 Mic, #14 mic position across (0 centre - 1 rim), #16 mic distance (1 = default),
// #23 Trem/Vib on, #24 0 Tremolo / 1 Vibrato, #25 Depth 0-10, #26 Sync, #27 Speed (Hz, or a whole-note fraction synced),
// #30 Reverb on, #31 Reverb type, #32 Reverb Level 0-10
struct AmpModel { const char *name; double hp, preHz, preDb, g0, span, pw; };
const AmpModel &ampModel(int id) {
    // preamp drive = g0 + span * (Gain / 10)^1.6 dB: the Clean / Crunch / Distorted presets of each amp land at about
    // 3-8 / 14-20 / 25+ dB of tanh drive for a DI guitar peaking near -6 dBFS; pw: power-amp drive at Master 10
    static const AmpModel amps[] = {
        {"British Combo", 90, 2500, 3, -4, 32, 8},   // fallback (id 5)
        {"Large Black Panel Combo", 70, 0, 0, -6, 32, 6}, {"Small Tweed Combo", 60, 0, 0, -2, 32, 8},
        {"Vintage British Stack", 90, 1200, 2, 0, 40, 8}, {"Modern American Stack", 120, 800, 3, 6, 36, 4},
        {"British Combo", 90, 2500, 3, -4, 32, 8}, {"Silver Panel Combo", 70, 0, 0, -6, 32, 6},
        {"Small Brown Panel Combo", 70, 0, 0, -4, 32, 8}, {"Mini Black Panel Combo", 80, 0, 0, -6, 32, 6},
        {"Large Tweed Combo", 60, 0, 0, -2, 32, 8}, {"Blues Blaster Combo", 70, 0, 0, -4, 32, 8},
        {"Mini Tweed Combo", 70, 0, 0, -2, 32, 8}, {"Modern British Stack", 110, 900, 2, 6, 36, 4},
        {"Brown Stack", 90, 1000, 2, 3, 40, 8}, {"British Blues Combo", 90, 1200, 2, 0, 40, 8},
        {"Studio Combo", 90, 900, 2, -2, 34, 6}, {"Small British Combo", 90, 2500, 2, -4, 32, 8},
        {"Boutique British Combo", 80, 2000, 1, -4, 32, 6}, {"Sunshine Stack", 90, 700, 2, 0, 40, 8},
        {"Small Sunshine Combo", 90, 2000, 2, -4, 34, 8}, {"Stadium Stack", 80, 0, 0, -6, 32, 4},
        {"Stadium Combo", 80, 0, 0, -6, 32, 4}, {"Boutique Retro Combo", 80, 2500, 1, -4, 32, 6},
        {"Pawnshop Combo", 120, 1500, 2, -1, 32, 10}, {"High Octane Stack", 120, 800, 3, 6, 36, 4},
        {"Turbo Stack", 120, 1000, 4, 9, 36, 4}, {"Transparent Preamp", 20, 0, 0, -12, 20, 0}};
    return id >= 1 && id <= 26 ? amps[id] : amps[0];
}
struct Peak { double hz, db, q; };
struct Cabinet { const char *name; double hp, hpQ, lp; int slope; std::vector<Peak> peaks; };
const Cabinet &ampCabinet(int id) {   // #12: the speaker's low resonance (high-pass), its top (low-pass), its peaks
    static const std::vector<Cabinet> cabs = {
        {"Direct", 0, 0, 0, 0, {}},
        {"Black Panel 4 x 10", 80, 0.9, 5000, 24, {{200, 1, 1}, {1800, 2, 1.2}}},
        {"Tweed 1 x 12", 90, 0.8, 4800, 24, {{400, -1.5, 1}, {2500, 2, 1.2}}},
        {"Vintage British 4 x 12", 85, 1.3, 5200, 36, {{550, -2, 1}, {2000, 3, 1.2}}},
        {"Modern American 4 x 12", 80, 1.3, 5000, 36, {{150, 2, 1}, {400, -1, 1}, {2500, 2.5, 1.4}}},
        {"British 2 x 12", 90, 0.9, 5500, 24, {{500, -1, 1}, {2500, 3, 1.2}}},
        {"Silver Panel 2 x 12", 80, 1.0, 5500, 24, {{120, 1.5, 1}, {2200, 2, 1.2}}},
        {"Brown Panel 1 x 12", 85, 0.8, 5000, 24, {{2200, 1.5, 1}}},
        {"Black Panel 1 x 10", 110, 0.8, 5000, 24, {{2500, 2.5, 1.2}}},
        {"Tweed 4 x 10", 80, 0.9, 5200, 24, {{2800, 2.5, 1.2}}},
        {"Brown Panel 1 x 15", 70, 0.9, 5000, 24, {{2500, 2, 1.4}}},
        {"Tweed 1 x 10", 110, 0.8, 4500, 24, {{2000, 2, 1.2}}},
        {"Modern British 4 x 12", 80, 1.3, 5500, 36, {{500, -3, 1}, {2800, 3, 1.4}}},
        {"Brown 4 x 12", 85, 1.3, 5000, 36, {{800, 1.5, 1}, {2400, 2.5, 1.4}}},
        {"British Blues 2 x 12", 85, 1.0, 5500, 24, {{2500, 2.5, 1.2}}},
        {"Studio 1 x 12", 90, 0.9, 5000, 24, {{900, 1.5, 1}, {3000, 2, 1.4}}},
        {"British 1 x 12", 100, 0.8, 5200, 24, {{3000, 2.5, 1.2}}},
        {"Boutique British 2 x 12", 85, 0.9, 6000, 24, {{700, 1.5, 1}, {3200, 3, 1.2}}},
        {"Sunshine 4 x 12", 85, 1.3, 4800, 36, {{700, 2, 1}, {2200, 2, 1.4}}},
        {"Sunshine 1 x 12", 95, 0.8, 5500, 24, {{3000, 3, 1.2}}},
        {"Stadium 4 x 12", 95, 1.3, 5500, 36, {{3000, 3.5, 1.4}}},
        {"Stadium 2 x 12", 85, 0.9, 5500, 24, {{2500, 2, 1.2}}},
        {"Boutique Retro 2 x 12", 85, 0.9, 6000, 24, {{700, 1.5, 1}, {3200, 3, 1.2}}},
        {"Pawnshop 1 x 8", 140, 0.9, 4000, 24, {{160, 2, 1.5}, {1500, 3, 1.2}}},
        {"High Octane 4 x 12", 80, 1.4, 6000, 36, {{500, -4, 0.9}, {3000, 3, 1.4}}},
        {"Turbo 4 x 12", 80, 1.4, 6500, 36, {{500, -6, 0.8}, {3500, 4, 1.4}}}};
    return id >= 0 && id < (int)cabs.size() ? cabs[(size_t)id] : cabs[0];
}
struct MicBand { const char *type; double hz, db, q; };
// #13 / Bass Amp #49: 1 Condenser 87, 3 Dynamic 20, 4 Dynamic 421 (Bass Amp Designer's three mics are stored as 1, 3, 4);
// 0, 2, 5, 6 read as Dynamic 57, Ribbon 121, Condenser 414, Dynamic 609 (a guess)
const std::vector<MicBand> &micBands(int id) {
    static const std::vector<std::vector<MicBand>> mics = {
        {{"lowshelf", 200, -2, 0.7}, {"peak", 4500, 3, 1.2}}, {{"highshelf", 8000, 1.5, 0.7}},
        {{"lowshelf", 150, 1.5, 0.7}, {"highshelf", 5000, -4, 0.7}}, {{"lowshelf", 250, -3, 0.7}, {"peak", 1500, 2, 1.0}},
        {{"highshelf", 5000, 2, 0.7}}, {{"highshelf", 8000, 2, 0.7}}, {{"lowshelf", 250, -2, 0.7}, {"peak", 3500, 3, 1.0}}};
    return id >= 0 && id < (int)mics.size() ? mics[(size_t)id] : mics[1];
}
// the cabinet's and microphone's curve: Apple's mic pad, the cone's centre fuller, the rim brighter and thinner, nearer more bass
void cabMicInto(std::vector<json> &bs, double hp, double hpQ, double lp, int slope, const std::vector<Peak> &peaks, int mic, double x, double z,
                double proximityHz) {
    if (hp <= 0) return;   // a Direct "cabinet": no speaker, no mic
    bs.push_back(bandOf("highpass", hp, 0, hpQ));
    for (auto &pk : peaks) bs.push_back(bandOf("peak", pk.hz, pk.db, pk.q));
    for (auto &m : micBands(mic)) bs.push_back(bandOf(m.type, m.hz, m.db, m.q));
    if (x > 0.01) { bs.push_back(bandOf("lowshelf", 200, -3 * x, 0.7071)); bs.push_back(bandOf("highshelf", 3000, 3 * x, 0.7071)); }
    if (z < 0.99) bs.push_back(bandOf("lowshelf", proximityHz, 2.5 * (1 - z), 0.7071));
    cutInto(bs, "lowpass", lp * (1 + 0.15 * x), slope);
}

json ampDesignerFx(const PatchPlugin &p, std::vector<std::string> &notes) {
    auto v = [&](size_t n, double def = 0) { return n < p.params.size() && p.params[n] < 1e29f ? (double)p.params[n] : def; };
    json fx = json::array();
    const int ampId = (int)std::lround(v(10, 5));
    const AmpModel &a = ampModel(ampId);
    const double gain = v(1, 5), bass = v(2, 5), mids = v(3, 5), treble = v(4, 5), presence = v(5, 5), master = v(6, 5);
    // preamp: the voicing's low cut and pre-emphasis, then the gain stage
    addFx(fx, eqOf({a.hp > 20 ? bandOf("highpass", a.hp, 0, 0.7071) : json(), a.preDb != 0 ? bandOf("peak", a.preHz, a.preDb, 0.8) : json()}));
    addSat(fx, a.g0 + a.span * std::pow(std::max(0.0, gain) / 10, 1.6));
    // tone stack, #11: 0 Modern, 1 British Bright, 2 Vintage, 3 U.S. Classic, 4 Boutique (the factory models' own EQs)
    struct Stack { double bHz, bR, mHz, mQ, m0, mR, tHz, t0, tR; };
    static const Stack stacks[] = {{90, 8, 700, 1.0, -4, 8, 3000, 0, 8}, {150, 5, 1000, 0.8, -1, 5, 3000, 2, 7}, {120, 6, 650, 0.8, -3, 5, 2200, 0, 7},
                                   {100, 6, 500, 0.7, -6, 5, 2500, 0, 7}, {110, 6, 800, 0.9, -1, 6, 3500, 1, 7}};
    const Stack &s = stacks[std::clamp((int)std::lround(v(11, 2)), 0, 4)];
    addFx(fx, eqOf({bandOf("lowshelf", s.bHz, s.bR * (bass - 5) / 5, 0.7071), bandOf("peak", s.mHz, s.m0 + s.mR * (mids - 5) / 5, s.mQ),
                    bandOf("highshelf", s.tHz, s.t0 + s.tR * (treble - 5) / 5, 0.7071)}));
    // effects section (before Presence and Master): tremolo or vibrato, then the reverb
    if (v(23) >= 0.5 && v(25) > 0) {
        const json rate = rateOf(v(26) >= 0.5, v(27, 5.4));
        if (v(24) >= 0.5) fx.push_back({{"type", "vibrato"}, {"rate", rate}, {"depth", r2(std::min(100.0, 5 * v(25)))}});
        else fx.push_back({{"type", "tremolo"}, {"rate", rate}, {"depth", r4(std::min(1.0, v(25) / 10))}, {"shape", "sine"}});
    } else if (v(23) >= 0.5) notes.push_back("Amp Designer: its tremolo/vibrato is on at Depth 0 (a Smart Control raises it), left out");
    if (v(30) >= 0.5 && v(32) > 0) {
        // #31: Vintage, Simple, Mellow, Bright, Dark, Resonant, Boutique Spring, Sweet, Rich, Warm Reverb
        struct Verb { bool spring; double lp, decay; };
        static const Verb verbs[] = {{true, 6000, 2.2}, {true, 4000, 2.0}, {true, 2500, 2.0}, {true, 5000, 2.0}, {true, 3000, 2.2},
                                     {true, 4000, 2.4}, {true, 5000, 2.4}, {false, 5000, 2.0}, {false, 8000, 2.6}, {false, 4000, 2.4}};
        const int t = ((int)std::lround(v(31)) % 10 + 10) % 10;
        fx.push_back(springOrReverb(verbs[t].spring, verbs[t].lp, verbs[t].decay, std::min(0.6, 0.06 * v(32))));
        if (t == 5) notes.push_back("Amp Designer: Resonant Spring's distorted midrange is not modelled");
    }
    // power amp: Master turned up saturates; Presence acts after it
    if (a.pw > 0 && master > 5) addSat(fx, a.pw * (master - 5) / 5);
    addFx(fx, eqOf({bandOf("highshelf", 4500, 1.5 * (presence - 5), 0.7071)}));
    // cabinet and microphone
    const Cabinet &c = ampCabinet((int)std::lround(v(12, ampId)));
    std::vector<json> cb;
    cabMicInto(cb, c.hp, c.hpQ, c.lp, c.slope, c.peaks, (int)std::lround(v(13, 1)), std::clamp(v(14), 0.0, 1.0), std::clamp(v(16, 1), 0.0, 1.0), 150);
    addFx(fx, eqOf(cb));
    fx = matched(fx);
    // Output, less the trim the presets set against Master's loudness (Output = 5.3 - 1.37 Master on average over 216
    // settings; Gain doesn't enter): the stages above are level-matched, so only the rest changes the level
    addFx(fx, gainOf(v(7) - (5.3 - 1.37 * master)));
    return fx;
}

// ------------------------------------------------------------------------------------------------ Bass Amp Designer
// #1 D.I. Boost dB, #2 HF Cut, #3 Tone on, #4 Tone 1-6, #8 Bright, #10 Gain, #11 EQ on, #12 Bass, #13 Low switch (-1/0/1),
// #14 Mids, #15 1-2-3 switch, #16 Treble, #17 High switch, #20 Master, #24 Compressor on, #25 Comp, #26 Hard/Soft,
// #27 comp Gain, #29 Graphic (1) / Parametric (0), #30 additional EQ on, #31 Pre/Post, #32-#38 graphic bands dB,
// #39-#41 LoMid gain/Hz/Q, #42-#44 HiMid gain/Hz/Q, #46 Model, #47 Amp (1 Modern, 3 Classic, 5 Flip Top), #48 Cabinet,
// #49 Mic, #50 / #52 mic position, #63 Blend (0 amp - 100 D.I.), #64 Output dB
json bassAmpFx(const PatchPlugin &p, std::vector<std::string> &notes) {
    auto v = [&](size_t n, double def = 0) { return n < p.params.size() && p.params[n] < 1e29f ? (double)p.params[n] : def; };
    const int ampId = (int)std::lround(v(47, 3)), cab = (int)std::lround(v(48, 4));
    struct BassAmp { double hp, g0, span, pw; };
    const BassAmp a = ampId == 1 ? BassAmp{40, -3, 27, 10} : ampId == 5 ? BassAmp{40, -9, 24, 10} : BassAmp{35, -6, 26, 12};
    json amp = json::array();
    if (v(8) >= 0.5) addFx(amp, eqOf({bandOf("highshelf", 2000, 4, 0.7071)}));                       // Bright
    addFx(amp, eqOf({bandOf("highpass", a.hp, 0, 0.7071)}));
    addSat(amp, a.g0 + a.span * v(10, 5) / 10, 1, true, true);
    json pre = json::array(), extra = json::array(), comp = json::array();
    if (v(11, 1) >= 0.5) {
        const int low = (int)std::lround(v(13)), mid = std::clamp((int)std::lround(v(15, 1)), 0, 2);
        addFx(pre, eqOf({bandOf("lowshelf", low < 0 ? 60 : low > 0 ? 150 : 100, 10 * (v(12, 5) - 5) / 5, 0.7071),
                         bandOf("peak", mid == 0 ? 250 : mid == 1 ? 500 : 1000, 10 * (v(14, 5) - 5) / 5, 0.8),
                         bandOf("highshelf", v(17) >= 0.5 ? 5000 : 3000, 10 * (v(16, 5) - 5) / 5, 0.7071)}));
    }
    if (v(30) >= 0.5) {
        if (v(29, 1) >= 0.5) {   // the graphic EQ's bands aren't documented: octaves from 50 Hz (a guess)
            std::vector<json> bs;
            for (int i = 0; i < 7; ++i) bs.push_back(bandOf("peak", 50.0 * (1 << i), v(32 + (size_t)i), 1.4));
            addFx(extra, eqOf(bs));
        } else addFx(extra, eqOf({bandOf("peak", v(40, 800), v(39), std::max(0.1, v(41, 1))), bandOf("peak", v(43, 2500), v(42), std::max(0.1, v(44, 1)))}));
    }
    if (v(24) >= 0.5) {   // its compressor always runs auto gain
        const double T = -3 * v(25, 5), R = 2 + 0.4 * v(25, 5);
        const bool hard = v(26) < 0.5;
        comp.push_back({{"type", "compressor"}, {"threshold", r2(T)}, {"ratio", r2(R * (hard ? 1.5 : 1))}, {"attack", hard ? 3 : 25},
                        {"release", 150}, {"knee", 6}, {"makeup", r2(-T * (1 - 1 / R) * 0.35 + (v(27, 5) - 5))}});
    }
    for (auto *part : {&pre, v(31) >= 0.5 ? &comp : &extra, v(31) >= 0.5 ? &extra : &comp})
        for (auto &e : *part) amp.push_back(e);
    if (cab != 7 && v(20, 5) > 5) addSat(amp, a.pw * (v(20, 5) - 5) / 5, 1, true, true);   // power amp (not at PreAmp Out)
    // #48: 0 Modern 3 Way, 1 Modern 15", 2 Modern 10", 3 Modern 6", 4 Classic 8 x 10", 5 Flip Top 1 x 15", 6 Direct (PowerAmp
    // Out), 7 Direct (PreAmp Out); its mic #49: 1 Condenser 87, 3 Dynamic 20, 4 Dynamic 421
    struct BassCab { double hp, hpQ, lp; std::vector<Peak> peaks; };
    static const BassCab cabs[] = {{40, 0.8, 8000, {{3000, 1, 1}}}, {40, 0.9, 2500, {{80, 2, 1}}}, {60, 0.8, 4000, {{1000, 2, 1}}},
                                   {120, 0.8, 6000, {{2000, 2, 1}}}, {45, 1.2, 4500, {{100, 2, 1}, {500, -2, 1}}}, {45, 1.1, 2800, {{90, 3, 1}}},
                                   {0, 0, 0, {}}, {0, 0, 0, {}}};
    const BassCab &c = cabs[std::clamp(cab, 0, 7)];
    std::vector<json> cb;
    cabMicInto(cb, c.hp, c.hpQ, c.lp, 24, c.peaks, (int)std::lround(v(49, 1)), std::clamp(v(50), 0.0, 1.0), std::clamp(v(52, 1), 0.0, 1.0), 120);
    addFx(amp, eqOf(cb));
    amp = matched(amp);
    // the D.I. box: its Tone curves 1-6 as Apple describes them, HF Cut
    json di = json::array();
    if (v(3) >= 0.5) {
        const int t = std::clamp((int)std::lround(v(4, 1)), 1, 6);
        std::vector<json> bs;
        if (t == 1) bs = {bandOf("peak", 800, -6, 0.35)};
        else if (t == 2) bs = {bandOf("peak", 800, -24, 0.5)};
        else if (t == 3) bs = {bandOf("peak", 1000, -3, 0.4)};
        else if (t == 4) bs = {bandOf("peak", 250, 1.5, 0.5), bandOf("peak", 8000, -3, 0.8), bandOf("highshelf", 10000, 3, 0.7)};
        else bs = {bandOf("highpass", 60, 0, 0.5), bandOf("lowshelf", 300, -3, 0.5), bandOf("peak", 900, 3, 0.7)};
        if (t == 6) bs.push_back(bandOf("lowpass", 12000, 0, 0.7071));
        addFx(di, eqOf(bs));
    }
    if (v(2) >= 0.5) addFx(di, eqOf({bandOf("lowpass", 8000, 0, 0.7071)}));
    // Blend #63: 0 = the amp alone, 100 = the D.I. alone, between them both in parallel (the D.I. at its Boost)
    const double wdi = std::clamp(v(63) / 100, 0.0, 1.0), ga = 1 - wdi, gd = wdi * std::pow(10.0, v(1) / 20);
    json fx = json::array();
    if (gd <= 1e-6) fx = amp;
    else if (ga <= 1e-6) { fx = di; addFx(fx, gainOf(v(1))); }
    else {
        const double w = ga / (ga + gd);
        for (auto &e : blendChain(di, 1 - w)) fx.push_back(e);
        for (auto &e : blendChain(amp, w)) fx.push_back(e);
        addFx(fx, gainOf(toDb(ga + gd)));
        notes.push_back("Bass Amp Designer: its amp and D.I. channels play in parallel; blended as one chain (an approximation)");
    }
    // Output, less the trim the presets set against Gain and Master (5.8 - 2.05 Gain - 0.58 Master over 60 settings)
    addFx(fx, gainOf(v(64) - (5.8 - 2.05 * v(10, 5) - 0.58 * v(20, 5))));
    return fx;
}

// ------------------------------------------------------------------------------------------------------ Pedalboard
// A stompbox's #0 On, then its parameters as its CSParameterOrder.plist numbers them; synced Rate / Time are
// fractions of a whole note. Distortion pedals: a level-matched tanh stage and their Level relative to where their
// factory presets sit (the median over the stompbox presets; the pedals keep their level as the drive turns).
json stompFxRaw(const std::string &name, const std::map<int, double> &s, std::vector<std::string> &notes);
json stompFx(const std::string &name, const std::map<int, double> &s, std::vector<std::string> &notes) {
    static const std::set<std::string> levelMatched = {"Vintage Drive", "Grinder", "Grit", "Fuzz Machine", "Happy Face Fuzz", "Candy Fuzz",
        "OctaFuzz", "Monster Fuzz", "Rawk! Distortion", "Double Dragon", "Tube Burner", "Classic Wah", "Modern Wah", "Auto-Funk", "Spin Box"};
    json fx = stompFxRaw(name, s, notes);
    return levelMatched.count(name) ? matched(fx) : fx;
}
json stompFxRaw(const std::string &name, const std::map<int, double> &s, std::vector<std::string> &notes) {
    auto v = [&](int n, double def = 0) { auto it = s.find(n); return it == s.end() ? def : it->second; };
    static const std::map<std::string, double> level0 = {{"Vintage Drive", -1}, {"Grinder", -5}, {"Fuzz Machine", 2}, {"OctaFuzz", 0},
        {"Happy Face Fuzz", -9.6}, {"Monster Fuzz", -4}, {"Candy Fuzz", -11.8}, {"Double Dragon", 0}, {"Rawk! Distortion", 5},
        {"Tube Burner", -6}, {"Grit", -14.3}};
    auto lvl = [&](int n) { return gainOf(v(n) - (level0.count(name) ? level0.at(name) : 0.0)); };
    auto squash = [&](double T, double R, double attack) {   // a pedal's internal compressor, its reduction half made up
        return json{{"type", "compressor"}, {"threshold", r2(T)}, {"ratio", R}, {"attack", attack}, {"release", 150}, {"knee", 6},
                    {"makeup", r2(-T * (1 - 1 / R) * 0.5)}};
    };
    auto hz = [&](int nSync, int nRate) {
        if (v(nSync) >= 0.5) notes.push_back(name + ": its tempo-synced rate plays at 120 BPM");
        return hzAt120(v(nSync) >= 0.5, v(nRate, 1));
    };
    auto delayOf = [&](bool sync, double t) {   // a pedal's delay: one line, its echoes in the middle
        json d = {{"type", "delay"}, {"pingpong", false}};
        if (sync) d["time"] = r4(4 * t); else d["ms"] = r2(std::max(1.0, t));
        return d;
    };
    json fx = json::array();
    if (name == "Vintage Drive") {   // Drive, Tone (Hz: a high cut), Level, Fat
        addFx(fx, eqOf({bandOf("highpass", v(4) >= 0.5 ? 80 : 200, 0, 0.5)}));
        addSat(fx, v(1, 30));
        addFx(fx, eqOf({bandOf("lowpass", v(2, 2000), 0, 0.5)}));
        addFx(fx, lvl(3));
    } else if (name == "Grinder") {   // Grind, Filter (Hz; harsher higher: a high-pass before), Level, Full/Scoop
        addFx(fx, eqOf({bandOf("highpass", v(2, 460), 0, 0.5)}));
        addSat(fx, v(1, 39));
        addFx(fx, eqOf({bandOf("peak", 700, v(4) >= 0.5 ? -8 : -3, 0.8), bandOf("lowpass", 5000, 0, 0.7071)}));
        addFx(fx, lvl(3));
    } else if (name == "Grit") {   // Distortion, Filter (Hz: a high cut), Volume
        addSat(fx, v(1, 15));
        addFx(fx, eqOf({bandOf("lowpass", std::min(19000.0, v(2, 5900)), 0, 0.7071)}));
        addFx(fx, lvl(3));
    } else if (name == "Fuzz Machine") {   // Fuzz, Tone 0-1 (more treble, fewer lows), Level
        const double t = std::clamp(v(2, 0.7), 0.0, 1.0);
        addFx(fx, eqOf({bandOf("highpass", 60 + 200 * t, 0, 0.7071)}));
        addSat(fx, 20 + 0.5 * v(1, 45));
        addFx(fx, eqOf({bandOf("lowpass", 1500 * std::pow(2.0, 2.5 * t), 0, 0.7071)}));
        addFx(fx, lvl(3));
    } else if (name == "Happy Face Fuzz") {   // Fuzz, Volume
        addFx(fx, eqOf({bandOf("highpass", 60, 0, 0.7071)}));
        addSat(fx, 18 + v(1, 30));
        addFx(fx, eqOf({bandOf("lowpass", 5000, 0, 0.7071)}));
        addFx(fx, lvl(2));
    } else if (name == "Candy Fuzz") {   // Drive, Level
        addFx(fx, eqOf({bandOf("highpass", 150, 0, 0.7071)}));
        addSat(fx, 18 + v(1, 6));
        addFx(fx, eqOf({bandOf("highshelf", 3000, 3, 0.7071)}));
        addFx(fx, lvl(2));
    } else if (name == "OctaFuzz") {   // Fuzz, Level, Tone 0-100 (a high-pass)
        notes.push_back("OctaFuzz: its octave-up is not played (a plain fuzz)");
        addSat(fx, 18 + v(1, 28));
        addFx(fx, eqOf({bandOf("highpass", 80 * std::pow(2.0, v(3, 46) / 25), 0, 0.7071)}));
        addFx(fx, lvl(2));
    } else if (name == "Monster Fuzz") {   // Roar, Growl, Tone 0-100, Texture, Grain, Level
        addSat(fx, v(1, 37) + v(2, 15));
        addFx(fx, eqOf({bandOf("peak", 1000, -6, 0.7), bandOf("lowpass", 1500 * std::pow(2.0, v(3, 35) / 30), 0, 0.7071)}));
        addFx(fx, lvl(6));
    } else if (name == "Rawk! Distortion") {   // Crunch, Tone 0-100 (brighter), Level
        double t = v(2, 38);
        if (t > 100) t = std::clamp(100 * (1 - std::log(t / 100) / std::log(20.0)), 0.0, 100.0);   // newer presets: 100-2000, read inverted
        addFx(fx, eqOf({bandOf("highpass", 100, 0, 0.7071)}));
        addSat(fx, v(1, 32));
        addFx(fx, eqOf({bandOf("lowpass", 1500 * std::pow(2.0, t / 30), 0, 0.7071)}));
        addFx(fx, lvl(3));
    } else if (name == "Double Dragon") {   // Drive, Tone 0-100, Level, Input dB, Squash 0-100, Contour, Mix %, Bright (1) / Fat
        if (v(5) > 0) fx.push_back(squash(-0.3 * v(5), 4, 5));
        addSat(fx, v(1, 19) + v(4), std::min(1.0, v(7, 100) / 100));   // Input drives harder (the stage is level-compensated)
        addFx(fx, eqOf({bandOf("lowpass", 1000 * std::pow(2.0, v(2, 74) / 25), 0, 0.7071),
                        v(8) >= 0.5 ? bandOf("highshelf", 3000, 3, 0.7071) : bandOf("lowshelf", 150, 3, 0.7071)}));
        addFx(fx, lvl(3));
    } else if (name == "Tube Burner") {   // Drive, Low, Mid Freq, Mid Gain, High, Tone (Hz), Bias, Squash, Fat, Output
        if (v(8) > 0) fx.push_back(squash(-0.3 * v(8), 3, 10));
        addSat(fx, v(1, 22));
        addFx(fx, eqOf({bandOf("lowshelf", 100, v(2), 0.7071), bandOf("peak", v(3, 800), v(4), 0.8), bandOf("highshelf", 3000, v(5), 0.7071),
                        bandOf("lowpass", std::min(19000.0, v(6, 4700)), 0, 0.7071), v(9) >= 0.5 ? bandOf("lowshelf", 120, 3, 0.7071) : json()}));
        addFx(fx, lvl(10));
    } else if (name == "Hi-Drive") {   // Level dB, Treble (0: only the highs boosted) / Full: a boost into what follows
        const double boost = v(1, 10);
        addFx(fx, v(2) < 0.5 ? eqOf({bandOf("highshelf", 1200, boost, 0.7071)}) : gainOf(boost));
        addSat(fx, 3, 1, false);
    } else if (name == "Squash Compressor") {   // Sustain (threshold dB), Level (dB), Attack
        fx.push_back({{"type", "compressor"}, {"threshold", r2(v(1, -20))}, {"ratio", 4}, {"attack", v(3) >= 0.5 ? 2 : 20}, {"release", 200},
                      {"knee", 6}, {"makeup", r2(v(2))}});
    } else if (name == "Graphic EQ") {   // 0.1-6.4 kHz bands (dB), Level
        std::vector<json> bs;
        for (int i = 0; i < 7; ++i) bs.push_back(bandOf("peak", 100.0 * (1 << i), v(1 + i), 1.4));
        addFx(fx, eqOf(bs));
        addFx(fx, gainOf(v(8)));
    } else if (name == "Classic Wah" || name == "Modern Wah") {   // Pedal Position 0-1; Modern: Mode (4 = Volume), Q -100..100
        const double pos = std::clamp(v(1, 0.5), 0.0, 1.0);
        if (name == "Modern Wah" && std::lround(v(2)) == 4) addFx(fx, gainOf(toDb(std::max(0.001, pos))));
        else fx.push_back({{"type", "filter"}, {"mode", "bandpass"}, {"cutoff", r2(350 * std::pow(2.0, 2.6 * pos))},
                           {"resonance", name == "Classic Wah" ? 4.0 : r2(4 * std::pow(2.0, 1.5 * v(3) / 100))}});
    } else if (name == "Auto-Funk") {   // Cutoff, Sensitivity, BP/LP (0 = BP), Up/Down (1 / -1), Hi/Lo resonance
        const double lo = 150 * std::pow(2.0, v(1, 30) / 40);
        json a = {{"type", "autowah"}, {"min", r2(lo)}, {"max", r2(lo * 5)}, {"resonance", v(5) >= 0.5 ? 6 : 2.5},
                  {"sensitivity", r2(std::clamp(20 + v(2, -20), -12.0, 12.0))}, {"mode", v(3) >= 0.5 ? "lowpass" : "bandpass"}};
        if (v(4, 1) < 0) std::swap(a["min"], a["max"]);   // sweeping down
        fx.push_back(a);
    } else if (name == "Blue Echo") {   // Sync, Time, Repeats, Mix, Tone Cut (0 Lo, 1 Hi, 2 Off), Mute
        if (v(4) <= 0) return fx;
        const int tc = (int)std::lround(v(5, 2));
        json d = delayOf(v(1) >= 0.5, v(2, 0.25));
        d["feedback"] = r4(std::min(0.95, v(3, 50) / 100 * 0.85));
        d["highpass"] = tc == 1 ? 500 : 20;
        d["lowpass"] = tc == 0 ? 2500 : 20000;
        d["pingpong"] = false;
        d["mix"] = r4(std::min(1.0, v(4) / 100));
        fx.push_back(d);
    } else if (name == "Tru-Tape Delay") {   // Sync, Time, Feedback, Mix, Hi Cut, Lo Cut, Dirt, Flutter, Norm/Reverse
        if (v(4) <= 0) return fx;
        json d = delayOf(v(1) >= 0.5, v(2, 0.25));
        d["feedback"] = r4(std::min(0.97, v(3, 50) / 100));
        d["highpass"] = r2(std::max(20.0, v(6, 20)));
        d["lowpass"] = r2(std::min(20000.0, v(5, 20000)));
        d["pingpong"] = false;
        d["mix"] = r4(std::min(1.0, v(4) / 100));
        json loop = json::array();
        if (v(7) > 0) addSat(loop, 0.12 * v(7));
        if (v(8) > 0) loop.push_back({{"type", "vibrato"}, {"rate", 6}, {"depth", r2(0.1 * v(8))}});
        if (!loop.empty()) d["loopFx"] = loop;
        fx.push_back(d);
        if (v(9) >= 0.5) notes.push_back("Tru-Tape Delay: its reverse playback plays forwards");
    } else if (name == "Tie Dye Delay") {   // Sync, Time, Feedback, Mix, Tone, Bright/Dark, Listen: a reverse delay
        if (v(4) <= 0) return fx;
        json d = delayOf(v(1) >= 0.5, v(2, 0.25));
        d["feedback"] = r4(std::min(0.95, v(3, 50) / 100));
        d["highpass"] = 100;
        d["lowpass"] = r2(1000 * std::pow(2.0, v(5, 50) / 25) * (v(6) >= 0.5 ? 1.5 : 1));
        d["pingpong"] = false;
        d["mix"] = r4(std::min(1.0, v(4) / 100));
        fx.push_back(d);
        notes.push_back("Tie Dye Delay: its reversed repeats play forwards");
    } else if (name == "Spring Box") {   // Style (Boutique, Simple, Vintage, Bright, Resonant), Time (short/medium/long), Tone, Mix
        if (v(4) <= 0) return fx;
        static const double lps[] = {5000, 4000, 6000, 5500, 4000}, decays[] = {1.2, 2.0, 3.0};
        fx.push_back(springOrReverb(true, lps[((int)std::lround(v(1)) % 5 + 5) % 5] * std::pow(2.0, v(3) / 100), decays[((int)std::lround(v(2, 1)) % 3 + 3) % 3],
                                    std::min(1.0, v(4) / 100)));
    } else if (name == "Phase Tripper") {   // Sync, Rate, Feedback, Depth
        fx.push_back({{"type", "phaser"}, {"rate", rateOf(v(1) >= 0.5, v(2, 1))}, {"floor", 250}, {"ceiling", r2(250 * std::pow(2.0, 1 + 3 * v(4, 67) / 100))},
                      {"stages", 4}, {"feedback", r4(std::min(0.9, v(3, 30) / 100 * 0.8))}, {"spread", 0}, {"mix", 0.5}});
    } else if (name == "Phaze 2") {   // two phasers: the one the Mix favours plays
        const int b = v(14, 50) < 50 ? 2 : 8;
        notes.push_back("Phaze 2: its two phasers play as one");
        fx.push_back({{"type", "phaser"}, {"rate", rateOf(v(1) >= 0.5, v(b, 0.5))}, {"floor", r2(std::max(20.0, v(b + 3, 200)))},
                      {"ceiling", r2(std::max(v(b + 3, 200) * 1.5, v(b + 4, 4000)))}, {"stages", std::clamp(2 * (int)std::lround(v(b + 1, 6) / 2), 2, 24)},
                      {"feedback", r4(std::clamp(v(b + 2) / 100, -0.9, 0.9))}, {"spread", 0.25}, {"mix", 0.5}});
    } else if (name == "Roto Phase") {   // Sync, Rate, Intensity, Vintage/Modern
        fx.push_back({{"type", "phaser"}, {"rate", rateOf(v(1) >= 0.5, v(2, 1))}, {"floor", 200}, {"ceiling", r2(200 * std::pow(2.0, 1 + 4 * v(3, 50) / 100))},
                      {"stages", 6}, {"feedback", 0.3}, {"spread", 0.25}, {"mix", 0.5}});
    } else if (name == "Retro Chorus") {   // Sync, Rate, Depth
        fx.push_back({{"type", "chorus"}, {"rate", hz(1, 2)}, {"depth", r2(0.5 + 0.05 * v(3, 50))}, {"delay", 8}, {"mix", 0.5}});
    } else if (name == "Heavenly Chorus") {   // Sync, Rate, Depth, Feedback, Density (dry/effect), Bright
        fx.push_back({{"type", "chorus"}, {"rate", hz(1, 2)}, {"depth", r2(0.06 * v(3, 50) + 0.5)}, {"delay", 12},
                      {"mix", r4(std::clamp(v(5, 50) / 100, 0.1, 0.9))}});
        if (v(6) >= 0.5) addFx(fx, eqOf({bandOf("highshelf", 4000, 2, 0.7071)}));
    } else if (name == "Robo Flanger") {   // Sync, Rate, Depth, Feedback, Manual
        if (v(4) > 0) notes.push_back("Robo Flanger: played as a short chorus, without its feedback");
        fx.push_back({{"type", "chorus"}, {"rate", hz(1, 2)}, {"depth", r2(0.2 + 0.04 * v(3, 30))}, {"delay", r2(0.5 + 0.08 * v(5))}, {"mix", 0.5}});
    } else if (name == "Flange Factory") {   // Sync, Rate, Depth, Reso, Mix, Wave, Shape, Curve, Manual, Low, High
        if (v(5) <= 0) return fx;
        if (v(4) > 0) notes.push_back("Flange Factory: played as a short chorus, without its resonance");
        fx.push_back({{"type", "chorus"}, {"rate", hz(1, 2)}, {"depth", r2(0.2 + 0.04 * v(3, 10))}, {"delay", r2(0.5 + 0.1 * v(9))},
                      {"mix", r4(std::min(1.0, v(5) / 100))}});
    } else if (name == "The Vibe") {   // Sync, Rate, Depth, Type V1 V2 V3 C1 C2 C3
        const int t = ((int)std::lround(v(4)) % 6 + 6) % 6;
        static const double vib[] = {10, 20, 35}, cho[] = {1, 2, 3.5};
        if (t < 3) fx.push_back({{"type", "vibrato"}, {"rate", rateOf(v(1) >= 0.5, v(2, 1))}, {"depth", r2(vib[t] * v(3, 100) / 100)}});
        else fx.push_back({{"type", "chorus"}, {"rate", hz(1, 2)}, {"depth", r2(cho[t - 3] * v(3, 100) / 100)}, {"delay", 2}, {"mix", 0.5}});
    } else if (name == "Spin Box") {   // Speed (0 Slow, 1 Brake, 2 Fast), Response, Drive, Cabinet, Fast Rate, Bright
        const int sp = (int)std::lround(v(1));
        if (sp == 1) notes.push_back("Spin Box: its brake (rotors stopped) plays as slow");
        if (v(3) > 1) addSat(fx, v(3));
        const double fast = std::max(1.0, v(5, 6.7));
        fx.push_back({{"type", "rotary"}, {"speed", sp == 2 ? 1 : 0}, {"hornFast", r2(fast)}, {"drumFast", r2(fast * 0.87)}});
        if (v(6) >= 0.5) addFx(fx, eqOf({bandOf("highshelf", 3000, 3, 0.7071)}));
    } else if (name == "Trem-O-Tone") {   // Sync, Rate, Depth %, Level
        fx.push_back({{"type", "tremolo"}, {"rate", rateOf(v(1) >= 0.5, v(2, 1))}, {"depth", r4(std::min(1.0, v(3, 50) / 100))}, {"shape", "sine"}});
        addFx(fx, gainOf(v(4)));
    } else if (name == "Total Tremolo") {   // Sync, Rate, Depth, Wave, Smoothing, Volume, Speed Up, Slow Down, Speed (1/2x, 1x, 2x)
        const long sp = std::lround(v(9, 1));
        const double mult = sp == 0 ? 0.5 : sp == 2 ? 2.0 : 1.0;
        const json rate = v(1) >= 0.5 ? rateOf(true, v(2, 0.25) / mult) : json(r4(v(2, 4) * mult));
        fx.push_back({{"type", "tremolo"}, {"rate", rate}, {"depth", r4(std::min(1.0, v(3, 50) / 100))},
                      {"shape", v(5) >= 40 ? "sine" : v(4) < 16 ? "square" : "triangle"}});
        addFx(fx, gainOf(v(6)));
    } else if (name == "Roswell Ringer") notes.push_back("Roswell Ringer: its ring modulation is not played");
    else if (name == "Dr. Octave") notes.push_back("Dr. Octave: its octaves below are not played (the direct signal plays)");
    else if (name == "Wham") notes.push_back("Wham: its pitch shift is not played (the direct signal plays)");
    else notes.push_back(name + ": not played");
    return fx;
}

// Pedalboard: a TSPP block of 2001 (or, in newer presets, 3401) values and a chunk per stompbox. #1001 + 20k = the
// stompbox in chain slot k (k = 0-13, -1 empty), #1002 + 20k = its bus inside a split (0 = A, 1 = B). Each "SBox" chunk
// (tag stored reversed) holds the stompbox's index, category, type and its own TSPP block (value 1 = #0 On).
json pedalboardFx(const PatchPlugin &p, std::vector<std::string> &notes) {
    static const char *types[] = {"Phase Tripper", "Vintage Drive", "Grinder", "Fuzz Machine", "Retro Chorus", "Robo Flanger", "The Vibe",
                                  "Auto-Funk", "Blue Echo", "Squash Compressor", "Splitter", "Mixer", "OctaFuzz", "Happy Face Fuzz",
                                  "Monster Fuzz", "Candy Fuzz", "Double Dragon", "Rawk! Distortion", "Hi-Drive", "Spin Box", "Roto Phase",
                                  "Heavenly Chorus", "Trem-O-Tone", "Phaze 2", "Roswell Ringer", "Total Tremolo", "Classic Wah", "Modern Wah",
                                  "Tru-Tape Delay", "Spring Box", "Phase Tripper", "Flange Factory", "Tube Burner", "Tie Dye Delay",
                                  "Dr. Octave", "Graphic EQ", "Wham", "Grit"};
    struct Box { std::string name; std::map<int, double> v; bool on = false; };
    std::map<uint32_t, Box> boxes;
    for (size_t i = 24 + 4 * p.values.size(); i + 8 <= p.block.size();) {
        const uint32_t n = le32(&p.block[i + 4]);
        if (n < 8 || i + n > p.block.size()) break;
        if (!std::memcmp(&p.block[i], "xoBS", 4) && n >= 8 + 16 + 24) {   // "SBox"
            const uint8_t *c = &p.block[i + 8], *t = c + 16;
            const uint32_t idx = le32(c), type = le32(c + 8), count = le32(t + 8);
            if (!std::memcmp(t + 12, "GAMETSPP", 8) && 8 + 16 + 24 + 4 * (size_t)count <= n) {
                Box b;
                b.name = type < sizeof types / sizeof *types ? types[type] : "stompbox " + std::to_string(type);
                for (uint32_t k = 1; k < count; ++k) {   // value 0 is reserved: #n = value n + 1
                    float f;
                    std::memcpy(&f, t + 24 + 4 * (size_t)k, 4);
                    if (std::isfinite(f) && std::fabs(f) < 1e29f) b.v[(int)k - 1] = f;
                }
                b.on = b.v.count(0) && b.v[0] >= 0.5;
                boxes[idx] = b;
            }
        }
        i += n;
    }
    auto v = [&](size_t n) { return n < p.params.size() && std::fabs(p.params[n]) < 1e29f ? (double)p.params[n] : -1.0; };
    struct Slot { const Box *box; int bus; };
    std::vector<Slot> chain;
    for (size_t k = 0; k < 14; ++k) {
        const double j = v(1001 + 20 * k);
        if (j < 0 || !boxes.count((uint32_t)std::lround(j))) continue;
        chain.push_back({&boxes[(uint32_t)std::lround(j)], (int)std::lround(std::max(0.0, v(1002 + 20 * k)))});
    }
    std::set<std::string> off;
    auto run = [&](const std::vector<const Box *> &bs) {
        json out = json::array();
        for (auto *b : bs) {
            if (!b->on) { off.insert(b->name); continue; }
            for (auto &e : stompFx(b->name, b->v, notes)) out.push_back(e);
        }
        return out;
    };
    json fx = json::array();
    for (size_t i = 0; i < chain.size();) {
        const Box &b = *chain[i].box;
        if (b.name == "Mixer") { ++i; continue; }
        if (b.name != "Splitter") { for (auto &e : run({&b})) fx.push_back(e); ++i; continue; }
        // a split: bus A (lower line) and bus B to the Mixer; Mixer #1 A / Mix / B, #2 Mix 0-100 (0 = A alone: a guess)
        size_t j = i + 1;
        while (j < chain.size() && chain[j].box->name != "Mixer") ++j;
        std::vector<const Box *> a, bb;
        for (size_t k = i + 1; k < j; ++k)
            if (chain[k].box->name != "Splitter") (chain[k].bus ? bb : a).push_back(chain[k].box);
        const json A = run(a), B = run(bb);
        double wA = 0.5, wB = 0.5;
        if (j < chain.size()) {
            const auto &m = chain[j].box->v;
            const long sw = m.count(1) ? std::lround(m.at(1)) : 1;
            const double mixB = std::clamp((m.count(2) ? m.at(2) : 50.0) / 100, 0.0, 1.0);
            wA = sw == 0 ? 1 : sw == 2 ? 0 : 1 - mixB;
            wB = sw == 0 ? 0 : sw == 2 ? 1 : mixB;
            if ((m.count(3) ? std::fabs(m.at(3)) : 0) + (m.count(4) ? std::fabs(m.at(4)) : 0) > 1) notes.push_back("Pedalboard: the Mixer's bus pans are not played");
        }
        if (b.v.count(1) && b.v.at(1) >= 0.5)   // Freq: below the crossover on bus A, above it on bus B
            fx.push_back({{"type", "multiband"}, {"crossovers", {r2(std::max(20.0, b.v.count(2) ? b.v.at(2) : 490.0))}},
                          {"bands", {{{"fx", A}, {"gain", wA > 0 ? r2(toDb(2 * wA)) : -96.0}}, {{"fx", B}, {"gain", wB > 0 ? r2(toDb(2 * wB)) : -96.0}}}}});
        else if (A.empty() && B.empty()) addFx(fx, gainOf(toDb(wA + wB)));
        else if (A.empty() || B.empty()) {
            const json &one = A.empty() ? B : A;
            const double w = wA + wB > 0 ? (A.empty() ? wB : wA) / (wA + wB) : 0;
            for (auto &e : blendChain(one, w)) fx.push_back(e);
            addFx(fx, gainOf(toDb(wA + wB)));
        } else {
            for (auto &e : wA >= wB ? A : B) fx.push_back(e);
            addFx(fx, gainOf(toDb(2 * std::max(wA, wB))));
            notes.push_back(std::string("Pedalboard: bus ") + (wA >= wB ? "B" : "A") + " of a split is left out (the louder bus plays)");
        }
        i = j + 1;
    }
    if (!off.empty()) {
        std::string names;
        for (auto &n : off) names += (names.empty() ? "" : ", ") + n;
        notes.push_back("Pedalboard: " + names + " switched off in the patch (a Smart Control turns it on), left out");
    }
    return fx;
}


// a chunk after a settings block's values (its tag stored byte-reversed, a u32 size counting its 8-byte header)
bool blockChunk(const PatchPlugin &p, const char *tag, const uint8_t *&data, size_t &size) {
    for (size_t at = 24 + 4 * p.values.size(); at + 8 <= p.block.size();) {
        const uint32_t n = le32(&p.block[at + 4]);
        if (n < 8 || at + n > p.block.size()) break;
        if (!std::memcmp(&p.block[at], tag, 4)) { data = &p.block[at + 8]; size = n - 8; return true; }
        at += n;
    }
    return false;
}

std::string pcName(int k) {
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((k % 12) + 12) % 12];
}
std::string keyName(int k) { return pcName(k) + std::to_string(k / 12 - 1); }   // 0-127, C4 = 60

// Apple's name for the MIDI effect a "midiFx" type re-creates
std::string appleMidiFxName(const std::string &type) {
    return type == "arp" ? "Arpeggiator" : type == "chord" ? "Chord Trigger" : type == "transpose" ? "Transposer" : type == "repeat" ? "Note Repeater" : type;
}

// a script that only passes its events on (tracing them or not), as Scripter's starting script does
bool passThroughScript(const std::string &js) {
    std::string code;
    for (size_t i = 0; i < js.size(); ++i) {
        if (!js.compare(i, 2, "//")) { while (i < js.size() && js[i] != '\n') ++i; continue; }
        if (!js.compare(i, 2, "/*")) { const size_t e = js.find("*/", i + 2); if (e == std::string::npos) break; i = e + 1; continue; }
        if (!std::isspace((unsigned char)js[i])) code += js[i];
    }
    return code.empty() || code == "functionHandleMIDI(event){event.send();}" || code == "functionHandleMIDI(event){event.trace();event.send();}";
}

// the MIDI effects of one channel as a "midiFx" chain; a Single mode Chord Trigger on a drum kit is GarageBand's
// default left in the patch (Blue Ridge): every hit would bring three other drums
json channelMidiFx(const PatchChannel &c, std::vector<std::string> &notes) {
    json chain = json::array();
    const bool kit = c.instrument == "Drum Kit" || c.instrument == "Ultrabeat" || c.instrument == "Drum Machin";
    for (auto &m : c.midiChain) {
        json e = midiEffectSettings(m, notes);
        if (e.is_null()) continue;
        if (kit && e["type"] == "chord" && e.contains("intervals")) {
            notes.push_back("Chord Trigger: in Single mode on a drum kit (its default chord, left in the patch), skipped: it would add other drums to every hit");
            continue;
        }
        chain.push_back(e);
    }
    return chain;
}
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
            // #3 feedback %, #4 high cut, #5 low cut (12 dB/oct, on every echo: the first one too), #6 sync, #7 note (1/x
            // of a whole note), #19 dry %, #20 wet %, #22 time ms (unsynced; the legacy 23-value layout has coarse #1 + fine
            // #2 instead). Dry and Wet on a sine taper: a one-note bounce through GarageBand's Echo (wet 30 %) put the first
            // echo 7.4 dB under the note with its filters once, feedback 0.59 (0.61 saved), corners within 20 % of the saved
            const bool legacy = p.params.size() < 25;
            const double kHalfPi = 1.5707963267948966;
            const double dry = std::sin(kHalfPi * std::clamp((legacy ? 100 : v(19, 100)) / 100, 0.0, 1.0));
            const double wet = std::sin(kHalfPi * std::clamp((legacy ? v(0, 30) : v(20)) / 100, 0.0, 1.0));
            if (wet <= 0) { notes.push_back("Tape Delay: Wet is 0 as saved (GarageBand's Delay knob raises it), left out"); continue; }
            json d = {{"type", "delay"}, {"feedback", r4(std::min(v(3) / 100, 0.97))}, {"highpass", r2(v(5, 20))}, {"lowpass", r2(v(4, 20000))},
                      {"mix", r4(wet / (dry + wet))}, {"filterEchoes", true}, {"pingpong", false}};   // its echoes stay in the middle (the bounce)
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
        } else if (p.name == "St-Delay") {   // Stereo Delay: each side its own time and feedback, with crossfeed; ping-pong when the
                                             // crossfeed bounces one input between the sides
            const double mix = (v(0) + v(1)) / 200;
            if (mix <= 0) { notes.push_back("Stereo Delay: Mix 0 as saved (a Smart Control knob raises it), left out"); continue; }
            json d = {{"type", "delay"}};
            const bool sync = v(10) != 0;
            auto setTime = [&](json &o, double note, double groove, double ms) {   // #11/#13 note value, #12/#14 groove, #2/#3 ms
                if (sync && note > 0) o["time"] = r4(4 / note * (1 + groove / 100));
                else o["ms"] = r2(std::max(1.0, ms));
            };
            setTime(d, v(11), v(12), v(2));
            double fb = (v(4) + v(5)) / 200;
            const double xf = std::max(v(6), v(7)) / 100;
            const int inL = (int)std::lround(v(15, 1)), inR = (int)std::lround(v(16, 2));   // 0 off, 1 left, 2 right, 3 L+R, 4 L-R
            const bool pingpong = xf >= 0.5 && (inL == 0 || inR == 0 || (inL == 3 && inR == 3));
            const bool apart = sync ? std::fabs(v(11) - v(13)) > 1e-3 || std::fabs(v(12) - v(14)) > 1e-3 : std::fabs(v(2) - v(3)) > 1;
            if (pingpong) fb = std::max({v(4), v(5), std::sqrt(std::max(0.0, v(6) * v(7)))}) / 100;
            else if (apart || std::fabs(v(4) - v(5)) > 1 || xf > 0.005) {
                fb = v(4) / 100;
                json right = json::object();
                setTime(right, v(13), v(14), v(3));
                right["feedback"] = r4(std::min(v(5) / 100, 0.97));
                d["right"] = right;
                if (xf > 0.005) d["crossfeed"] = json::array({r4(std::min(v(6) / 100, 0.97)), r4(std::min(v(7) / 100, 0.97))});
            }
            d["feedback"] = r4(std::min(fb, 0.97));
            d["highpass"] = r2(v(23) > 0 ? v(23) : v(9) > 0 ? v(9) : 20);
            d["lowpass"] = r2(v(22) > 0 ? v(22) : v(8) > 0 ? v(8) : 20000);
            if (!d.contains("right")) d["pingpong"] = pingpong;
            d["mix"] = r4(mix);
            add(d);
            if (pingpong && apart) notes.push_back("Stereo Delay: ping-pong at the left side's time (its two sides' times differ)");
            if (!pingpong && (inL >= 3 || inR >= 3)) notes.push_back("Stereo Delay: its L+R or L-R inputs play as each side's own input");
        } else if (p.name == "Delay D") {   // Delay Designer: values[0..7] = sync, grid, swing, feedback on, feedback tap, feedback dB, dry dB,
                                            // wet dB; then chunks "TapA".."TapZ" of 20 floats (ms, steps, level dB, mute, pan, ..., HP, LP, filter on)
            auto val = [&](size_t i, double def = 0) { return i < p.values.size() ? (double)p.values[i] : def; };
            struct Tap { double ms, steps, level, pan, hp, lp; bool mute, filter, pitch; };
            std::vector<Tap> taps;
            for (size_t i = 24 + 4 * p.values.size(); i + 8 <= p.block.size();) {
                const uint32_t n = le32(&p.block[i + 4]);
                if (n < 8 || i + n > p.block.size()) break;
                const bool tap = p.block[i + 3] == 'T' && p.block[i + 2] == 'a' && p.block[i + 1] == 'p';   // "TapX", stored reversed
                if (tap && n >= 8 + 80) {
                    float f[20];
                    std::memcpy(f, &p.block[i + 8], 80);
                    // 0 ms, 1 grid steps, 2 level dB, 3 mute, 4 pan -100..100, 13 high-pass, 14 low-pass, 15 filter on, 19 pitch on
                    taps.push_back({f[0], f[1], f[2], f[4], f[13], f[14], f[3] != 0, f[15] != 0, f[19] != 0});
                }
                i += n;
            }
            const double dry = dbLin(val(6, -6));
            double wet = dbLin(val(7, -12));
            const bool fbOn = val(3) != 0;
            const size_t idx = (size_t)std::max(0.0, val(4));
            // every tap that isn't muted, at its time, level, pan and filters; the feedback tap (muted or not) feeds back
            json list = json::array();
            int fbIndex = -1;
            bool pitched = false;
            for (size_t k = 0; k < taps.size(); ++k) {
                const Tap &x = taps[k];
                const bool feeds = fbOn && k == idx;
                if (x.mute && !feeds) continue;
                json o = json::object();
                if (val(0) != 0 && x.steps > 0 && val(1) > 0) o["time"] = r4(x.steps * val(1) * 4);
                else o["ms"] = r2(std::max(1.0, x.ms));
                o["level"] = x.mute ? -120.0 : r2(std::clamp(x.level, -120.0, 24.0));
                if (std::fabs(x.pan) > 0.5) o["pan"] = r4(std::clamp(x.pan / 100, -1.0, 1.0));
                if (x.filter) { o["highpass"] = r2(x.hp); o["lowpass"] = r2(x.lp); }
                if (feeds) fbIndex = (int)list.size();
                pitched = pitched || (x.pitch && !x.mute);
                list.push_back(o);
            }
            if (list.empty() || wet <= 0) { notes.push_back("Delay Designer: no tap or Wet off as saved, left out"); continue; }
            json d = {{"type", "delay"}};
            const double fbAmount = fbIndex >= 0 ? r4(std::min(dbLin(val(5, -100)), 0.97)) : 0.0;
            if (list.size() == 1 && !list[0].contains("pan")) {   // one tap in the middle: a plain delay, both sides kept apart
                const json &o = list[0];
                if (o.contains("time")) d["time"] = o["time"]; else d["ms"] = o["ms"];
                d["pingpong"] = false;
                d["highpass"] = o.value("highpass", 20.0);
                d["lowpass"] = o.value("lowpass", 20000.0);
                d["feedback"] = fbAmount;
                if (std::fabs(o.value("level", 0.0)) >= 0.01) wet *= dbLin(o.value("level", 0.0));
            } else {
                d["taps"] = list;
                d["feedback"] = fbAmount;
                if (fbIndex > 0) d["feedbackTap"] = fbIndex;
            }
            addMixGain(d, dry, wet);
            if (pitched) notes.push_back("Delay Designer: its taps' pitch shifting is not played");
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
        } else if (p.name == "Amp") {          // Amp Designer
            for (auto &e : ampDesignerFx(p, notes)) add(e);
        } else if (p.name == "Bass Amp") {     // Bass Amp Designer
            for (auto &e : bassAmpFx(p, notes)) add(e);
        } else if (p.name == "Pedalboard") {
            for (auto &e : pedalboardFx(p, notes)) add(e);
        } else if (p.id == 273) {              // one stompbox as its own plug-in ("Spring Box"): #0 On, then its own parameters
            std::map<int, double> s;
            for (size_t i = 0; i < p.params.size(); ++i) if (std::fabs(p.params[i]) < 1e29f) s[(int)i] = p.params[i];
            if (s.count(0) && s[0] < 0.5) notes.push_back(p.name + ": switched off in the patch, left out");
            else for (auto &e : stompFx(p.name, s, notes)) add(e);
        } else if (p.name == "Echo") {   // #16 note value (dotted, straight, triplet for 1/2, 1/4, 1/8, 1/16 from 0, read from its
                                         // presets' names), #17 repeat %, #18 color -100..100, #19 dry %, #20 wet %
            const double dry = v(19) / 100, wet = v(20, 100) / 100;
            if (wet <= 0) { notes.push_back("Echo: Wet 0 as saved, left out"); continue; }
            const int k = std::clamp((int)std::lround(v(16, 4)), 0, 11);
            const double beats = 2.0 / std::pow(2.0, k / 3) * (k % 3 == 0 ? 1.5 : k % 3 == 2 ? 2.0 / 3 : 1.0);
            // Color as the echoes' low-pass, 6 kHz at 0 and an octave per 50 (a guess); a 100 Hz high-pass
            json d = {{"type", "delay"}, {"time", r4(beats)}, {"feedback", r4(std::clamp(v(17, 30) / 100, 0.0, 0.95))}, {"pingpong", false},
                      {"highpass", 100}, {"lowpass", r2(std::min(20000.0, 6000 * std::pow(2.0, v(18) / 50)))}};
            addMixGain(d, dry, wet);
            notes.push_back("Echo: its Color plays as a low-pass on the echoes (a guess)");
        } else if (p.name == "Multipr") {   // Multipressor: #0 bands (2-4, the top ones play: 2 bands are bands 3 and 4), crossovers #24
                                            // (1/2), #13 (2/3), #2 (3/4); band b's threshold, ratio, make-up, attack, release at
                                            // #40/#29/#18/#7 + 0..4; bypass #52..#49; #46 master gain dB, #48 auto gain
            const int n = (int)std::lround(std::clamp(v(0, 4), 2.0, 4.0));
            const int base[4] = {40, 29, 18, 7}, bypass[4] = {52, 51, 50, 49};
            const double xo[3] = {v(24, 160), v(13, 1100), v(2, 7500)};
            json xs = json::array(), bands = json::array();
            for (int b = 4 - n; b < 4; ++b) {
                if (b > 4 - n) xs.push_back(r2(xo[b - 1]));
                json band = json::object();
                const int q = base[b];
                if (v(bypass[b]) < 0.5 && v(q + 1, 1) > 1.001)
                    band["fx"] = json::array({{{"type", "compressor"}, {"threshold", r2(v(q))}, {"ratio", r4(v(q + 1))}, {"makeup", r2(v(q + 2))},
                                               {"attack", r2(std::max(0.1, v(q + 3, 20)))}, {"release", r2(std::max(1.0, v(q + 4, 250)))}}});
                else if (v(q + 2) != 0) band["gain"] = r2(v(q + 2));
                bands.push_back(band);
            }
            bool ascending = true;
            for (size_t i = 1; i < xs.size(); ++i) ascending &= xs[i].get<double>() > xs[i - 1].get<double>();
            if (!ascending) { notes.push_back("Multipressor: its crossovers don't rise, left out"); continue; }
            add({{"type", "multiband"}, {"crossovers", xs}, {"bands", bands}});
            if (v(46) != 0) add({{"type", "gain"}, {"db", r2(v(46))}});
            if (v(48) >= 0.5) notes.push_back("Multipressor: its auto gain is not played (make-up as saved)");
            if (v(1) > 0.5) notes.push_back("Multipressor: its lookahead is not played");
        } else if (p.name == "DirMix") {   // Direction Mixer: #0 input (0 L/R, 1 M/S), #1 direction (degrees), #2 spread (1 = as it is)
            const double spread = v(2, 1), dir = v(1);
            if (std::fabs(spread - 1) > 0.01) add({{"type", "width"}, {"amount", r4(std::clamp(spread, 0.0, 2.0))}});
            if (std::fabs(dir) > 0.5) {   // the image turned toward one side: a pan (90 degrees = all the way; a guess)
                add({{"type", "pan"}, {"position", r4(std::clamp(dir / 90, -1.0, 1.0))}});
                notes.push_back("Direction Mixer: its direction plays as a pan");
            }
            if (v(0) >= 0.5) notes.push_back("Direction Mixer: its M/S input plays as left and right");
        } else if (p.name == "Distortion") {   // #0 drive dB, #1 tone Hz (a low-pass after it: a guess), #2 output dB
            if (v(0) > 0) add({{"type", "saturate"}, {"drive", r2(v(0))}});
            if (v(1, 20000) < 19000) add({{"type", "eq"}, {"bands", json::array({{{"type", "lowpass"}, {"freq", r2(std::max(200.0, v(1)))}, {"q", 0.7071}}})}});
            if (v(2) != 0) add({{"type", "gain"}, {"db", r2(v(2))}});
        } else if (p.name == "Dist II") {   // Distortion II: #0 pre gain dB, #1 drive 0-1, #2 tone dB (a high shelf: a guess), #3 type
            const double drive = std::max(0.0, v(0)) + 24 * std::clamp(v(1, 0.5), 0.0, 1.0);
            if (drive >= 0.5) add({{"type", "saturate"}, {"drive", r2(drive)}, {"match", true}});
            else notes.push_back("Distortion II: Pre Gain and Drive 0 as saved (a Smart Control knob raises them)");
            if (std::fabs(v(2)) > 0.1) add({{"type", "eq"}, {"bands", json::array({{{"type", "highshelf"}, {"freq", 2000.0}, {"gain", r2(std::clamp(v(2), -24.0, 12.0))}}})}});
            notes.push_back("Distortion II: drive as tanh saturation, level-matched, tone as a high shelf, its three types alike (guesses)");
        } else if (p.name == "Exciter") {   // #0 frequency Hz, #1 color, #2 harmonics %, #3 original signal on
            if (v(2) > 0) {
                add({{"type", "eq"}, {"bands", json::array({{{"type", "highshelf"}, {"freq", r2(std::clamp(v(0, 4000), 500.0, 15000.0))},
                                                            {"gain", r2(std::min(9.0, 6 * v(2) / 100))}}})}});
                notes.push_back("Exciter: its harmonics play as a high shelf (a guess)");
            }
        } else if (p.name == "Microphaser") {   // #0 rate Hz, #3 feedback %, #4/#5 sweep floor/ceiling Hz, #6 stages (Phaser's layout), #15 intensity %
            if (v(15) > 0.5) {
                add({{"type", "phaser"}, {"rate", r4(std::max(0.02, v(0, 0.5)))}, {"floor", r2(std::max(20.0, v(4, 200)))},
                     {"ceiling", r2(std::max(v(4, 200) * 1.05, v(5, 4000)))}, {"stages", (int)std::lround(std::clamp(v(6, 4), 2.0, 24.0))},
                     {"feedback", r4(std::clamp(v(3) / 100, -0.95, 0.95))}, {"mix", r4(0.5 * std::min(1.0, v(15) / 100))}});
                notes.push_back("Microphaser: its Intensity plays as the phaser's mix (a guess)");
            } else notes.push_back("Microphaser: Intensity 0 as saved (a Smart Control knob raises it), left out");
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
        std::vector<std::string> left;
        const json midi = channelMidiFx(*inst, left);
        std::string names;
        for (auto &m : midi) names += (names.empty() ? "" : ", ") + appleMidiFxName(m.value("type", ""));
        if (!names.empty()) notes.push_back("MIDI effects (" + names + "): they play as the track's \"midiFx\" (\"midiFx\": false plays the notes as written)");
        notes.insert(notes.end(), left.begin(), left.end());
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
            p.db = 40 * std::log10(v * 127 / 90);   // sendVolume is the send knob's position / 127, on the fader's law (90 = 0 dB)
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
                    for (auto &x : recs) {
                        p.arpeggiator |= x.midiFx && x.name == "Arpeggiator" && !x.bypassed;
                        if (x.midiFx && !x.bypassed) p.midiEffects.push_back(x.name);
                    }
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

const std::vector<EffectPatch> &logicEffectPatches() {
    static std::vector<EffectPatch> patches;
    static std::once_flag once;
    std::call_once(once, [] {
        std::set<std::string> roots;
        for (auto &r : logicPatchRoots()) {
            const fs::path base = fs::path(r).parent_path();
            for (const char *kind : {"Audio", "Aux", "Output"}) {
                const fs::path root = base / kind;
                std::error_code ec;
                if (!fs::is_directory(root, ec) || !roots.insert(root.string()).second) continue;
                for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_directory(ec) || lower(it->path().extension().string()) != ".patch") continue;
                    it.disable_recursion_pending();
                    EffectPatch p;
                    p.name = it->path().stem().u8string();
                    p.path = it->path().string();
                    p.category = (fs::path(kind) / it->path().parent_path().lexically_relative(root)).lexically_normal().generic_u8string();
                    if (!p.category.empty() && p.category.back() == '/') p.category.pop_back();
                    if (p.category.size() > 2 && p.category.compare(p.category.size() - 2, 2, "/.") == 0) p.category.resize(p.category.size() - 2);
                    patches.push_back(p);
                }
            }
        }
    });
    return patches;
}

const EffectPatch *logicEffectPatchNamed(const std::string &name) {
    std::string q = lower(name);
    if (q.size() > 6 && q.compare(q.size() - 6, 6, ".patch") == 0) q.resize(q.size() - 6);
    for (auto &p : logicEffectPatches()) if (lower(p.name) == q) return &p;
    for (auto &p : logicEffectPatches()) {   // "Output/Pop", "Clean Guitar/Echo Studio"
        const std::string full = lower(p.category + "/" + p.name);
        if (q.find('/') != std::string::npos && full.size() >= q.size() && full.compare(full.size() - q.size(), q.size(), q) == 0 &&
            (full.size() == q.size() || full[full.size() - q.size() - 1] == '/'))
            return &p;
    }
    return nullptr;
}

const LogicPatch *logicPatchNamed(const std::string &name) {
    std::string q = lower(name);
    if (q.size() > 6 && q.compare(q.size() - 6, 6, ".patch") == 0) q.resize(q.size() - 6);
    for (auto &p : logicPatches()) if (lower(p.name) == q) return &p;
    return nullptr;
}

std::vector<SmartMapping> smartControls(const std::string &patchDir) {
    std::vector<SmartMapping> out;
    const auto files = channelFiles(patchDir);
    std::vector<uint8_t> d;
    if (files.empty() || !readWhole(files[0], d)) return out;
    std::map<int, std::string> labels;   // "Smart Knob 3" -> knob 2 (in its own archive, beside the mappings')
    for (size_t s = 20; s + 8 <= d.size(); ++s) {
        if (std::memcmp(&d[s], "bplist00", 8)) continue;
        const uint32_t len = le32(&d[s - 4]);
        json a;
        if (len < 40 || s + len > d.size() || !parseKeyedArchive(&d[s], len, a) || !a.is_object()) continue;
        const json &top = a.contains("dictionary") && a["dictionary"].is_object() ? a["dictionary"] : a;
        if (top.contains("kLgControlLabelLookupKey") && top["kLgControlLabelLookupKey"].is_object())
            for (auto &[k, v] : top["kLgControlLabelLookupKey"].items()) {
                const size_t p = k.find("Smart Knob ");
                if (p != std::string::npos && v.is_object() && v.contains("customLabel") && v["customLabel"].is_string())
                    labels[std::atoi(k.c_str() + p + 11) - 1] = v["customLabel"].get<std::string>();
            }
        if (!a.contains("dictionary") || !a["dictionary"].is_object() || !out.empty()) continue;
        for (auto &[k, list] : a["dictionary"].items()) {
            if (k.empty() || !std::isdigit((unsigned char)k[0]) || !list.is_array()) continue;
            for (auto &m : list) {
                if (!m.is_object() || !m.contains("parameterIndex_1") || !m["parameterIndex_1"].is_number()) continue;
                SmartMapping sm;
                sm.knob = std::atoi(k.c_str());
                sm.param = m["parameterIndex_1"].get<int>();
                sm.slot = m.contains("slot") && m["slot"].is_number() ? m["slot"].get<int>() : 0;
                sm.send = m.contains("isSendMuteMapping") || !m.contains("slot");
                if (m.contains("rangeLow") && m["rangeLow"].is_number()) sm.low = m["rangeLow"].get<double>();
                if (m.contains("rangeHigh") && m["rangeHigh"].is_number()) sm.high = m["rangeHigh"].get<double>();
                sm.flipped = m.value("rangeIsFlipped", false);
                if (m.contains("scalingGraph") && m["scalingGraph"].is_array())
                    for (auto &g : m["scalingGraph"]) if (g.is_object()) sm.graph.push_back({g.value("x", 0.0), g.value("y", 0.0)});
                out.push_back(sm);
            }
        }
    }
    for (auto &sm : out) if (auto it = labels.find(sm.knob); it != labels.end()) sm.label = it->second;
    return out;
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
        // the audio effects and the MIDI effects each in their slot order (records aren't stored in it)
        std::vector<const Record *> fx, mfx;
        for (auto &x : recs) {
            if (x.name.empty() || &x == r) continue;
            c.effects.push_back(x.name);
            (x.midiFx ? mfx : fx).push_back(&x);
        }
        std::stable_sort(mfx.begin(), mfx.end(), [](const Record *a, const Record *b) { return a->order < b->order; });
        for (auto *x : mfx) {
            c.midiEffects.push_back(x->name);
            c.midiChain.push_back(settingsOf(d, x->at, std::min(d.size(), x->at + x->size), x->name));
            c.midiChain.back().bypassed = x->bypassed;
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
        const uint8_t *g = nullptr;
        size_t gsize = 0;
        if (blockChunk(p, "DCGU", g, gsize)) parseBinaryPlist(g, gsize, grid);
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

json midiEffectSettings(const PatchPlugin &p, std::vector<std::string> &notes) {
    auto v = [&](size_t n, double def = 0) { return n < p.params.size() && std::fabs(p.params[n]) < 1e29f ? (double)p.params[n] : def; };
    if (p.bypassed) return nullptr;
    if (p.id == 300) {   // the Arpeggiator
        json a = arpeggiatorSettings(p, notes);
        if (!a.is_null()) a["type"] = "arp";
        return a;
    }
    if (p.id == 308) {   // Chord Trigger
        // the chord map: n, n intervals (the Single chord, from the trigger key), m, then m x (key, n, n intervals from it)
        std::vector<int32_t> raw;
        const uint8_t *m = nullptr;
        size_t ms = 0;
        if (blockChunk(p, "\0\0\0\0", m, ms)) for (size_t i = 0; i + 4 <= ms; i += 4) raw.push_back((int32_t)le32(m + i));
        size_t at = 0;
        bool whole = true;
        auto list = [&](int32_t n) {
            json out = json::array();
            if (n < 0 || n > 128 || at + (size_t)n > raw.size()) { whole = false; return out; }
            for (int32_t i = 0; i < n; ++i) out.push_back(raw[at++]);
            return out;
        };
        json single = at < raw.size() ? list(raw[at++]) : json::array(), chords = json::object();
        for (int32_t k = 0, count = at < raw.size() ? raw[at++] : 0; k < count && whole; ++k) {
            if (at + 2 > raw.size()) { whole = false; break; }
            const int key = raw[at++];
            json c = list(raw[at++]);
            if (whole && key >= 0 && key <= 127) chords[keyName(key)] = c;
        }
        if (!whole) notes.push_back("Chord Trigger: its chord map couldn't be read whole");
        // a strip saved by an older Chord Trigger keeps Learn Remote in #8 (its step table: #7 9 steps, #8 89)
        const bool older = p.steps.size() > 8 && p.steps[7] == 8 && p.steps[8] == 88;
        json c = {{"type", "chord"}};
        if (v(0) >= 0.5) c["chords"] = chords;
        else c["intervals"] = single;
        c["range"] = {(int)std::lround(std::clamp(v(1, 21), 0.0, 127.0)), (int)std::lround(std::clamp(v(2, 108), 0.0, 127.0))};
        if (const int t = older ? 0 : (int)std::lround(std::clamp(v(8), -48.0, 48.0)); t != 0) c["transpose"] = t;
        if (older && v(7, 20) != 20) notes.push_back("Chord Trigger: an older version's setting #7 (" + std::to_string(std::lround(v(7))) + ") is read as no transposition");
        if (v(older ? 8 : 7, 20) > 20.5) notes.push_back("Chord Trigger: its Learn Remote key (picking chords live) is left out");
        return c;
    }
    if (p.id == 290) {   // Transposer: #0 semitones, #2 root, #4 scale menu, #5-#16 the scale's notes C to B
        json scale = json::array();
        for (int i = 0; i < 12; ++i) if (v(5 + (size_t)i, 1) >= 0.5) scale.push_back(i);
        const int semis = (int)std::lround(std::clamp(v(0), -48.0, 48.0));
        const bool chromatic = scale.empty() || scale.size() == 12;
        if (semis == 0 && chromatic) return nullptr;
        json t = {{"type", "transpose"}, {"semitones", semis}};
        if (!chromatic) {
            t["scale"] = scale;
            if (std::lround(v(2)) % 12 != 0) notes.push_back("Transposer: its scale's notes are read as written (its root, " + pcName((int)std::lround(v(2))) + ", left out: unverified)");
        }
        return t;
    }
    if (p.id == 303) {   // Note Repeater
        json r = {{"type", "repeat"}};
        const double d = v(2, 0.125);
        if (v(1, 1) >= 0.5) {   // synced: a fraction of a whole note (0.0625 = 1/16)
            std::string name;
            for (int den = 1; den <= 128 && name.empty(); den *= 2) {
                if (std::fabs(d - 1.0 / den) < 1e-7) name = "1/" + std::to_string(den);
                else if (std::fabs(d - 1.5 / den) < 1e-7) name = "1/" + std::to_string(den) + "D";
                else if (std::fabs(d - 2.0 / 3 / den) < 1e-7) name = "1/" + std::to_string(den) + "T";
            }
            if (!name.empty()) r["time"] = name;
            else r["time"] = r4(d * 4);
        } else {
            r["ms"] = r2(d);
            notes.push_back("Note Repeater: its unsynced delay is read as milliseconds (unverified)");
        }
        r["repeats"] = (int)std::lround(std::clamp(v(3, 3), 0.0, 99.0));
        if (const int t = (int)std::lround(std::clamp(v(4), -48.0, 48.0)); t != 0) r["transpose"] = t;
        if (const double ramp = std::clamp(v(5, 100), 1.0, 200.0); ramp != 100) r["ramp"] = r4(ramp / 100);
        if (v(0, 1) < 0.5) r["thru"] = false;
        const int lo = (int)std::lround(std::clamp(v(6), 0.0, 127.0)), hi = (int)std::lround(std::clamp(v(7, 127), 0.0, 127.0));
        if (lo != 0 || hi != 127) r["range"] = {lo, hi};
        return r;
    }
    if (p.id == 312 || p.name == "ScriptInst") return nullptr;   // GarageBand's built-in instrument scripts: the notes play as written
    if (p.id == 302) {   // Scripter: the script is a binary property list holding its text
        const uint8_t *d = nullptr;
        size_t n = 0;
        json script;
        if (blockChunk(p, "TSCS", d, n) && parseBinaryPlist(d, n, script) && script.is_string() && passThroughScript(script.get<std::string>())) return nullptr;
        notes.push_back("Scripter: its script isn't run (the notes play as written)");
        return nullptr;
    }
    static const std::map<uint32_t, std::string> names = {{307, "Velocity Processor"}, {304, "Randomizer"}, {305, "Modifier"}};
    notes.push_back((names.count(p.id) ? names.at(p.id) : p.name) + ": a MIDI effect, not played (the notes play as written)");
    return nullptr;
}

json patchMidiFx(const std::vector<PatchChannel> &chans, std::vector<std::string> &notes, const std::string &name) {
    json chain = json::array();
    for (auto &c : chans) {   // GarageBand's patches keep their MIDI effects on one channel
        chain = channelMidiFx(c, notes);
        if (!chain.empty()) break;
    }
    for (auto &e : chain) if (e["type"] == "arp") return chain;
    for (auto &c : chans)   // else Alchemy's own arpeggiator, inside the instrument (after the MIDI effects)
        if (c.instrument == "Alchemy" && !c.alchemy.empty()) {
            AlchemyPatch a = alchemyPatch(c.alchemy, name);
            if (a.arp.is_null()) continue;
            json e = a.arp;
            e["type"] = "arp";
            chain.push_back(e);
            notes.insert(notes.end(), a.arpNotes.begin(), a.arpNotes.end());
            break;
        }
    return chain;
}

bool appleMidiFx(const std::string &name, json &chain, std::vector<std::string> &notes, std::string &err) {
    std::error_code ec;
    std::string dir = name, shown = fs::u8path(name).stem().u8string();   // a patch folder, or a patch by name
    if (!fs::is_directory(fs::u8path(name), ec)) {
        const LogicPatch *lp = logicPatchNamed(name);
        if (!lp) { err = "no GarageBand or Logic patch '" + name + "'"; return false; }
        dir = lp->path, shown = lp->name;
    }
    std::vector<PatchChannel> chans;
    if (!readPatchChannels(dir, chans, err)) return false;
    chain = patchMidiFx(chans, notes, shown);
    return true;
}

const std::vector<std::pair<std::string, std::string>> &midiEffectPresets(const std::string &plugin) {
    static std::map<std::string, std::vector<std::pair<std::string, std::string>>> lists;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    auto it = lists.find(plugin);
    if (it != lists.end()) return it->second;
    auto &list = lists[plugin];
    std::set<std::string> seen;
    for (auto &root : pluginSettingsRoots()) {
        const fs::path dir = fs::path(root) / plugin;
        std::error_code ec;
        for (auto e = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && e != fs::recursive_directory_iterator(); e.increment(ec)) {
            if (!e->is_regular_file(ec) || lower(e->path().extension().string()) != ".pst") continue;
            const std::string rel = (e->path().parent_path().lexically_relative(dir) / e->path().stem()).generic_u8string();
            const std::string name = rel.rfind("./", 0) == 0 ? rel.substr(2) : rel;
            if (seen.insert(lower(name)).second) list.push_back({name, e->path().string()});
        }
    }
    std::sort(list.begin(), list.end(), [](auto &a, auto &b) { return lower(a.first) < lower(b.first); });
    return list;
}

bool appleMidiFxPreset(const std::string &type, const std::string &name, json &fx, std::vector<std::string> &notes, std::string &err) {
    const std::string plugin = appleMidiFxName(type);
    const auto &list = midiEffectPresets(plugin);
    const std::string q = lower(name);
    std::vector<const std::pair<std::string, std::string> *> hits;
    for (auto &x : list) if (lower(x.first) == q) { hits = {&x}; break; }
    if (hits.empty())
        for (auto &x : list) {
            const size_t slash = x.first.rfind('/');
            if (lower(slash == std::string::npos ? x.first : x.first.substr(slash + 1)) == q) hits.push_back(&x);
        }
    if (hits.empty()) { err = "no " + plugin + " preset '" + name + "' (`wavelength presets " + type + "` lists them)"; return false; }
    if (hits.size() > 1) {
        err = "'" + name + "' names several " + plugin + " presets:";
        for (auto *h : hits) err += " '" + h->first + "'";
        return false;
    }
    std::vector<uint8_t> d;
    if (!readWhole(hits[0]->second, d)) { err = "can't read " + hits[0]->second; return false; }
    const PatchPlugin p = settingsOf(d, 0, d.size(), plugin);
    static const std::map<std::string, uint32_t> ids = {{"chord", 308}, {"transpose", 290}, {"repeat", 303}};
    if (!ids.count(type) || p.id != ids.at(type)) { err = hits[0]->second + " isn't " + plugin + " settings"; return false; }
    fx = midiEffectSettings(p, notes);
    if (fx.is_null()) fx = {{"type", type}};   // a Transposer preset that changes nothing
    return true;
}

} // namespace wl
