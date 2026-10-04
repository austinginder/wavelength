#include "logic_patches.hpp"

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

struct Record { size_t at, size; std::string name, preset; };

// The plugin records of a channel strip: where each payload is, its size and its plugin's name
bool records(const std::vector<uint8_t> &d, std::vector<Record> &out) {
    const uint8_t magic[4] = {'U', 'C', 'u', 'A'};
    const auto first = std::search(d.begin(), d.begin() + (long)std::min<size_t>(d.size(), 4096), magic, magic + 4);
    for (size_t pos = (size_t)(first - d.begin()); pos + 36 <= d.size() && !std::memcmp(&d[pos], "UCuA", 4);) {
        const uint32_t n = le32(&d[pos + 0x1c]);
        Record r{pos + 36, n, "", ""};
        const uint8_t *pl = &d[pos + 36];
        if (n >= 140 && pos + 36 + 140 <= d.size() && (!std::memcmp(pl + 132, "MELC", 4) || !std::memcmp(pl + 132, "GAME", 4))) {
            for (size_t k = 0; k < 12 && pl[120 + k]; ++k) r.name += (char)pl[120 + k];
            for (size_t k = 14; k < 120 && pl[k] >= 32 && pl[k] < 127; ++k) r.preset += (char)pl[k];
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
        for (uint32_t i = 1; i < count; ++i) {   // value 0 is reserved: params[n] = parameter #n
            const uint32_t bits = u32(s + 24 + 4 * (size_t)i);
            float f;
            std::memcpy(&f, &bits, 4);
            p.params.push_back(std::isfinite(f) ? f : 0.f);
        }
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
} // namespace

json patchEffects(const std::vector<PatchPlugin> &chain, std::vector<std::string> &notes) {
    json fx = json::array();
    for (auto &p : chain) {
        auto v = [&](size_t n, double def = 0) { return n < p.params.size() ? (double)p.params[n] : def; };
        if (p.params.empty()) { notes.push_back(p.name + ": no settings saved, left out"); continue; }
        if (p.name == "Channel EQ") {   // 8 bands of (on, Hz, gain dB or slope n = 6n dB/oct, Q) from #0; master gain #32
            json bands = json::array();
            if (v(0)) cutStages(bands, "highpass", v(1), (int)std::lround(v(2)) * 6, v(3));
            const char *kinds[6] = {"lowshelf", "peak", "peak", "peak", "peak", "highshelf"};
            for (int b = 1; b <= 6; ++b)
                if (v(4 * b) && v(4 * b + 2) != 0)
                    bands.push_back({{"type", kinds[b - 1]}, {"freq", r2(v(4 * b + 1))}, {"gain", r2(v(4 * b + 2))}, {"q", r4(v(4 * b + 3))}});
            if (v(28)) cutStages(bands, "lowpass", v(29), (int)std::lround(v(30)) * 6, v(31));
            if (!bands.empty()) fx.push_back({{"type", "eq"}, {"bands", bands}});
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
        } else {
            notes.push_back(p.name + ": GarageBand's own effect, not played");
        }
    }
    return fx;
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
        if (r) { c.instrument = r->name; c.preset = r->preset; }
        for (auto &x : recs) {
            if (x.name.empty() || &x == r) continue;
            c.effects.push_back(x.name);
            if (!r || x.at > r->at) c.chain.push_back(settingsOf(d, x.at, std::min(d.size(), x.at + x.size), x.name));
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

} // namespace wl
