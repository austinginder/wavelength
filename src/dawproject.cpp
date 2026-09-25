#include "dawproject.hpp"

#include "bitwig.hpp"
#include "catalog.hpp"
#include "sampler.hpp"
#include "vst2_abi.hpp"
#include "xml.hpp"
#include "zip.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

double linToDb(double v) { return v > 1e-6 ? 20.0 * std::log10(v) : -120.0; }
double r3(double v) { return std::round(v * 1000) / 1000; }
double r6(double v) { return std::round(v * 1e6) / 1e6; }

struct Ctx {
    Zip zip;
    std::string outDir;
    DawprojectImport *res;
    std::set<std::string> written;   // plugin states already extracted
    double bpm = 120;
    std::map<std::string, std::string> busByChannel;    // channel id -> bus name (effect returns, groups)
    std::map<std::string, std::string> trackByChannel;  // channel id -> track name
    std::map<std::string, std::string> channelOfTrack;  // track id -> channel id
    // parameter id -> (track/bus name, "volume" | "pan" | "param:#<plugin id>" (instrument, normalized) |
    // "plain:#<id>" (instrument, plain units) | "fx" (a parameter of an effect: not imported yet))
    std::map<std::string, std::pair<std::string, std::string>> paramTarget;
    std::set<std::string> names;
    bitwig::Project bw;            // the Bitwig project behind the export, when found
    bool haveBw = false;
    Zip bwZip;
};

// Bitwig's Drum Machine hands each pad's chain the note C3, whatever the pad's key
constexpr int kPadNote = 60;

std::string writeState(Ctx &c, const std::string &file, const std::vector<uint8_t> &data) {
    const std::string rel = "plugins/" + file;
    if (c.written.count(rel)) return rel;
    std::error_code ec;
    fs::create_directories(fs::path(c.outDir) / "plugins", ec);
    std::ofstream o(fs::path(c.outDir) / rel, std::ios::binary);
    o.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
    c.written.insert(rel);
    return rel;
}

// the component state inside a .vstpreset (VST2 builds of a plugin take the same chunk)
bool vstpresetComponent(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
    auto le32 = [&](size_t at) { return (uint32_t)in[at] | (uint32_t)in[at + 1] << 8 | (uint32_t)in[at + 2] << 16 | (uint32_t)in[at + 3] << 24; };
    auto le64 = [&](size_t at) { return (uint64_t)le32(at) | (uint64_t)le32(at + 4) << 32; };
    if (in.size() < 48 || std::memcmp(in.data(), "VST3", 4) != 0) return false;
    const uint64_t list = le64(40);
    if (list + 8 > in.size() || std::memcmp(&in[list], "List", 4) != 0) return false;
    const uint32_t n = le32(list + 4);
    for (uint32_t k = 0; k < n && list + 8 + 20 * (k + 1) <= in.size(); ++k) {
        const size_t e = list + 8 + 20 * k;
        if (std::memcmp(&in[e], "Comp", 4) != 0) continue;
        const uint64_t off = le64(e + 4), size = le64(e + 12);
        if (off + size > in.size()) return false;
        out.assign(in.begin() + (long)off, in.begin() + (long)(off + size));
        return true;
    }
    return false;
}

// VST2 builds that save parameter lists, not chunks: their VST3 state decoded into parameters
bool vst3StateToVst2Params(const std::string &vst2Id, const std::vector<uint8_t> &comp, json &params) {
    auto f32 = [&](size_t at) { uint32_t b = (uint32_t)comp[at] | (uint32_t)comp[at + 1] << 8 | (uint32_t)comp[at + 2] << 16 | (uint32_t)comp[at + 3] << 24; float f; std::memcpy(&f, &b, 4); return (double)f; };
    if (vst2Id == "LdMx" && comp.size() >= 12) {   // LoudMax: Thresh, Output (floats), Fader Link, ISP Detection (16-bit flags)
        params = {{"Thresh", r3(f32(0))}, {"Output", r3(f32(4))}, {"Fader Link", comp[8] ? 1 : 0}, {"ISP Detection", comp[10] ? 1 : 0}};
        return true;
    }
    return false;
}

// VST3 builds that render silence offline, where the VST2 build of the same plugin plays
const std::set<std::string> kSilentVst3 = {"Komplete Kontrol"};

std::string uniqueName(Ctx &c, std::string n) {
    if (n.empty()) n = "Track";
    std::string cand = n;
    for (int k = 2; c.names.count(cand); ++k) cand = n + " " + std::to_string(k);
    c.names.insert(cand);
    return cand;
}

// copy a plugin state out of the archive; returns the job-relative path ("" on failure)
std::string extractState(Ctx &c, const std::string &archivePath) {
    if (archivePath.empty()) return "";
    const std::string rel = "plugins/" + fs::path(archivePath).filename().string();
    if (c.written.count(rel)) return rel;
    std::vector<uint8_t> data;
    std::string err;
    if (!c.zip.read(archivePath, data, err)) { c.res->notes.push_back("plugin state " + archivePath + " is missing from the file: " + err); return ""; }
    std::error_code ec;
    fs::create_directories(fs::path(c.outDir) / "plugins", ec);
    std::ofstream o(fs::path(c.outDir) / rel, std::ios::binary);
    o.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
    c.written.insert(rel);
    return rel;
}

double paramValue(const xml::Node *dev, const char *tag, double def) {
    const xml::Node *p = dev ? dev->child(tag) : nullptr;
    return p ? p->num("value", def) : def;
}

// One device: a plugin (with state), a DAWproject standard effect, or a DAW's own device (noted, left out).
// Returns a json object: {"plugin": spec, "state": path} for instruments/plugin effects, or a built-in effect.
bool mapDevice(Ctx &c, const xml::Node &d, const std::string &where, json &out) {
    const std::string tag = d.tag, name = d.get("name", d.get("deviceName"));
    const xml::Node *en = d.child("Enabled");
    if (en && en->get("value", "true") == "false") { c.res->notes.push_back(where + ": device '" + name + "' is switched off in the DAW; left out"); return false; }
    std::string spec;
    if (tag == "Vst3Plugin") spec = "vst3:" + d.get("deviceID");
    else if (tag == "ClapPlugin") spec = "clap:" + d.get("deviceID");
    else if (tag == "Vst2Plugin") spec = "vst2:" + vst2::fourcc((int32_t)std::strtoll(d.get("deviceID").c_str(), nullptr, 10));
    else if (tag == "AuPlugin") { c.res->notes.push_back(where + ": Audio Unit '" + name + "' can't be hosted yet; left out"); return false; }
    if (!spec.empty()) {
        out = {{"plugin", spec}};
        if (const xml::Node *st = d.child("State")) {
            const std::string rel = extractState(c, st->get("path"));
            if (!rel.empty()) out["state"] = rel;
        }
        PluginInfo pi;
        std::string e;
        if (!resolvePlugin(spec, pi, e)) {   // fall back to the name when the id isn't installed in this format
            PluginInfo byName;
            if (resolvePlugin(name, byName, e)) {
                c.res->notes.push_back(where + ": " + spec + " is not installed; using " + byName.format + " '" + byName.name + "' by name (its state may not load)");
                out["plugin"] = byName.format + ":" + byName.id;
            } else c.res->notes.push_back(where + ": plugin '" + name + "' (" + spec + ") is not installed");
        }
        ++c.res->plugins;
        return true;
    }
    // DAWproject standard devices
    if (tag == "Equalizer") {
        json bands = json::array();
        for (const xml::Node *b : d.all("Band")) {
            const std::string type = b->get("type", "bell");
            std::string t = type == "highPass" ? "highpass" : type == "lowPass" ? "lowpass" : type == "lowShelf" ? "lowshelf" : type == "highShelf" ? "highshelf" : "peak";
            const xml::Node *on = b->child("Enabled");
            if (on && on->get("value", "true") == "false") continue;
            json bj = {{"type", t}, {"freq", r3(paramValue(b, "Freq", 1000))}};
            if (t == "peak" || t == "lowshelf" || t == "highshelf") bj["gain"] = r3(paramValue(b, "Gain", 0));
            if (b->child("Q")) bj["q"] = r3(paramValue(b, "Q", 0.707));
            bands.push_back(bj);
        }
        out = {{"type", "eq"}, {"bands", bands}};
        return true;
    }
    if (tag == "Compressor") {
        out = {{"type", "compressor"}, {"threshold", r3(paramValue(&d, "Threshold", -20))}, {"ratio", r3(paramValue(&d, "Ratio", 4))},
               {"attack", r3(paramValue(&d, "Attack", 0.01) * 1000)}, {"release", r3(paramValue(&d, "Release", 0.1) * 1000)},
               {"makeup", r3(paramValue(&d, "OutputGain", 0))}};
        return true;
    }
    if (tag == "Limiter") {
        out = {{"type", "limiter"}, {"ceiling", r3(paramValue(&d, "Threshold", -1))}, {"release", r3(paramValue(&d, "Release", 0.08) * 1000)}};
        return true;
    }
    if (tag == "NoiseGate") { c.res->notes.push_back(where + ": noise gate left out (no built-in gate by threshold yet)"); return false; }
    // a DAW's own device: DAWproject carries its name, not its settings
    if (d.get("deviceName") == "Drum Machine" && d.get("deviceRole") == "instrument") return false;   // the caller puts a kit in its place
    c.res->notes.push_back(where + ": " + d.get("deviceName", name) + " is a device of the exporting DAW; its settings aren't in the file, so it is left out");
    return false;
}

// ---- devices from the Bitwig project ---------------------------------------------------------

// a plugin device: spec + state from the project's own plugin-states
bool bwPlugin(Ctx &c, const bitwig::Device &d, const std::string &where, json &out) {
    std::string spec = d.kind + ":" + d.pluginId;
    std::vector<uint8_t> state;
    std::string e;
    if (!d.state.empty() && !c.bwZip.read(d.state, state, e)) {
        c.res->notes.push_back(where + ": " + d.name + "'s saved state is missing from the Bitwig project");
        state.clear();
    }
    PluginInfo pi;
    bool ok = resolvePlugin(spec, pi, e);
    if (ok && d.kind == "vst3" && kSilentVst3.count(pi.name)) {   // prefer the VST2 build
        PluginInfo v2;
        if (resolvePlugin("vst2:" + pi.name, v2, e)) {
            c.res->notes.push_back(where + ": " + pi.name + " renders silence as VST3 here; using its VST2 build with the same state");
            pi = v2;
            spec = "vst2:" + v2.id;
        }
    } else if (!ok) {
        PluginInfo byName;
        if (resolvePlugin(d.name, byName, e)) {
            c.res->notes.push_back(where + ": " + spec + " is not installed; using " + byName.format + " '" + byName.name + "' by name");
            pi = byName;
            spec = byName.format + ":" + byName.id;
            ok = true;
        } else c.res->notes.push_back(where + ": plugin '" + d.name + "' (" + spec + ") is not installed");
    }
    out = {{"plugin", spec}};
    if (!state.empty()) {
        std::string file = fs::path(d.state).filename().string();
        if (ok && pi.format == "vst2" && fs::path(file).extension() == ".vstpreset") {   // VST3 preset -> the VST2 chunk
            std::vector<uint8_t> chunk;
            json params;
            if (vstpresetComponent(state, chunk)) {
                if (vst3StateToVst2Params(pi.id, chunk, params)) { out["params"] = params; state.clear(); }
                else { state = chunk; file = fs::path(file).stem().string() + ".bin"; }
            }
        }
        if (!state.empty()) out["state"] = writeState(c, file, state);
    }
    ++c.res->plugins;
    return true;
}

double bwParam(const bitwig::Device &d, const std::string &id, double def) {
    auto it = d.params.find(id);
    return it == d.params.end() ? def : it->second;
}

void bwChain(Ctx &c, const std::vector<bitwig::Device> &devs, const std::string &where, json &fx);

// Bitwig's own effects as built-in ones; appends to `fx`
void bwEffect(Ctx &c, const bitwig::Device &d, const std::string &where, json &fx) {
    if (!d.enabled) { c.res->notes.push_back(where + ": " + d.name + " is switched off in Bitwig; left out"); return; }
    if (d.kind != "native") { json m; if (bwPlugin(c, d, where, m)) fx.push_back(m); return; }
    const std::string &n = d.name;
    if (n == "Chain" || n == "FX Layer") {
        if (n == "FX Layer" || bwParam(d, "MIX", 1) < 0.999) c.res->notes.push_back(where + ": " + n + " is imported as a plain serial chain");
        for (auto &ch : d.chains) bwChain(c, ch.second, where + " » " + n, fx);
        if (const double g = bwParam(d, "GAIN", 0); std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        return;
    }
    if (n == "EQ+" || n == "EQ-5") {
        const int nb = n == "EQ+" ? 8 : 5;
        const double shift = bwParam(d, "GLOBAL_SHIFT", 0), amount = bwParam(d, "GLOBAL_AMOUNT", 1);
        json bands = json::array();
        bool approx = false;
        for (int b = 1; b <= nb; ++b) {
            const std::string k = std::to_string(b);
            if (bwParam(d, "ENABLE" + k, 1) == 0) continue;
            const int type = (int)bwParam(d, "TYPE" + k, 13);
            std::string t;
            double notch = 0;
            // the band types EQ+ and EQ-5 share (EQ+ adds steeper cuts and shelves)
            switch (type) {
            case 3: t = "peak"; break;
            case 4: t = "peak"; notch = -24; break;
            case 5: case 16: case 17: t = "lowshelf"; approx |= type != 5; break;
            case 6: case 15: t = "highshelf"; approx |= type != 6; break;
            case 1: case 10: t = "highpass"; approx = true; break;
            case 0: case 14: t = "lowpass"; approx = true; break;
            case 13: continue;                                    // off
            default: c.res->notes.push_back(where + ": " + n + " band " + k + " has a filter type Wavelength doesn't know (" + std::to_string(type) + "); left out"); continue;
            }
            json bj = {{"type", t}, {"freq", r3(bitwig::pitchToHz(bwParam(d, "FREQ" + k, 69) + shift))}, {"q", r3(std::pow(10.0, bwParam(d, "Q" + k, -0.15)))}};
            if (t == "peak" || t == "lowshelf" || t == "highshelf") {
                const double g = notch ? notch : bwParam(d, "GAIN" + k, 0) * amount;
                if (std::fabs(g) < 0.01) continue;
                bj["gain"] = r3(g);
            }
            bands.push_back(bj);
        }
        if (approx) c.res->notes.push_back(where + ": " + n + " cut and shelf slopes are approximated (Wavelength's are 12 dB/octave)");
        if (!bands.empty()) fx.push_back({{"type", "eq"}, {"bands", bands}});
        if (const double g = bwParam(d, "OUTPUT_GAIN", 0); std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        return;
    }
    if (n == "Filter") {
        const int type = (int)bwParam(d, "FILTER_TYPE", 0);
        const double pitch = bwParam(d, "CUTOFF", 135);
        std::string mode;
        switch (type) {
        case 0: case 8: mode = "lowpass"; break;
        case 1: case 11: mode = "highpass"; break;
        case 2: mode = "bandpass"; break;
        default: c.res->notes.push_back(where + ": Filter mode " + std::to_string(type) + " isn't known yet; left out"); return;
        }
        if (const double g = bwParam(d, "PRE_GAIN", 0); std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        const bool open = (mode == "lowpass" && pitch >= 130) || (mode == "highpass" && pitch <= 19);
        if (!open) {
            if (type == 8 || type == 11) c.res->notes.push_back(where + ": Filter slope approximated (Wavelength's filter is 12 dB/octave)");
            fx.push_back({{"type", "filter"}, {"mode", mode}, {"cutoff", r3(bitwig::pitchToHz(pitch))},
                          {"resonance", r3(0.707 * std::pow(16.0, std::clamp(bwParam(d, "RESONANCE", 0), 0.0, 1.0)))}});
        }
        if (const double g = bwParam(d, "POST_GAIN", 0); std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        return;
    }
    if (n == "Reverb") {
        const double mix = bwParam(d, "MIX", 0.5);
        if (mix < 0.005) return;   // dry: factory kits park their effects at 0% for the pad knobs
        fx.push_back({{"type", "reverb"}, {"decay", r3(std::clamp(std::pow(10.0, bwParam(d, "REVERB_TIME", 0.1)), 0.1, 30.0))},
                      {"size", r3(std::clamp(0.65 + bwParam(d, "ROOM_SIZE", 0) * 1.1, 0.05, 1.0))},
                      {"predelay", r3(bwParam(d, "PRE-DELAY", 0.004) * 1000)}, {"width", r3(std::clamp(bwParam(d, "WIDTH", 1), 0.0, 1.5))},
                      {"mix", r3(mix)}});
        return;
    }
    if (n == "Delay-2") {
        const double mix = bwParam(d, "MIX", 0.5);
        if (mix < 0.005) return;
        json dj = {{"type", "delay"}, {"mix", r3(mix)}};
        if (bwParam(d, "SYNCL", 1) != 0) dj["time"] = r3(bwParam(d, "LBEATTIME", 2) / 4.0);   // sixteenths
        else dj["ms"] = r3(bwParam(d, "LTIME", 0.25) * 1000);
        const double cross = (bwParam(d, "CROSSFEEDL", 0) + bwParam(d, "CROSSFEEDR", 0)) / 2;
        dj["feedback"] = r3(std::min(0.9, std::max({bwParam(d, "FEEDBACKL", 0.3), bwParam(d, "FEEDBACKR", 0.3), cross})));
        dj["pingpong"] = cross > 0.2;
        dj["highpass"] = r3(bitwig::pitchToHz(bwParam(d, "LOCUT", 45)));
        dj["lowpass"] = r3(bitwig::pitchToHz(bwParam(d, "HICUT", 117)));
        fx.push_back(dj);
        c.res->notes.push_back(where + ": Delay-2 is imported as one stereo delay at the left side's time");
        return;
    }
    if (n == "Distortion") {
        const double mix = bwParam(d, "MIX", 1);
        if (mix < 0.005) return;
        fx.push_back({{"type", "saturate"}, {"drive", r3(bwParam(d, "DRIVE", 20))}, {"mix", r3(mix)}});
        if (const double g = bwParam(d, "LEVEL", 0); std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        json cuts = json::array();
        if (const double lo = bwParam(d, "EQ_LOW_CUT", 12); lo > 13) cuts.push_back({{"type", "highpass"}, {"freq", r3(bitwig::pitchToHz(lo))}});
        if (const double hi = bwParam(d, "EQ_HIGH_CUT", 136); hi < 134) cuts.push_back({{"type", "lowpass"}, {"freq", r3(bitwig::pitchToHz(hi))}});
        if (!cuts.empty()) fx.push_back({{"type", "eq"}, {"bands", cuts}});
        return;
    }
    if (n == "Compressor") {
        if (const double g = bwParam(d, "INPUT", 0); std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        const double rx = std::clamp(bwParam(d, "RATIO", 0.5), 0.0, 0.99);
        fx.push_back({{"type", "compressor"}, {"threshold", r3(bwParam(d, "THRESHOLD", -18))}, {"ratio", r3(1.0 / (1.0 - rx))},
                      {"attack", r3(std::pow(10.0, bwParam(d, "ATTACK", -2)) * 1000)}, {"release", r3(std::pow(10.0, bwParam(d, "RELEASE", -1)) * 1000)},
                      {"makeup", r3(bwParam(d, "OUTPUT", 0))}});
        if (bwParam(d, "MAKEUP_GAIN", 0) != 0) c.res->notes.push_back(where + ": the Compressor's automatic makeup gain isn't modelled");
        return;
    }
    if (n == "Multiband FX-3" || n == "Multiband FX-2") {
        const bool three = n == "Multiband FX-3";
        json cross = json::array(), bands = json::array();
        if (three) { cross.push_back(r3(bitwig::pitchToHz(bwParam(d, "LOW_MID_SPLIT", 57)))); cross.push_back(r3(bitwig::pitchToHz(bwParam(d, "MID_HIGH_SPLIT", 100)))); }
        else cross.push_back(r3(bitwig::pitchToHz(bwParam(d, "SPLIT", 80))));
        for (const char *band : three ? std::vector<const char *>{"LOW", "MID", "HIGH"} : std::vector<const char *>{"LOW", "HIGH"}) {
            json bfx = json::array();
            auto it = d.chains.find(std::string(band) + "_CHAIN");
            if (it != d.chains.end()) bwChain(c, it->second, where + " » " + n + " " + band, bfx);
            json bj = {{"fx", bfx}};
            const double lvl = bwParam(d, band, 1);   // stored like a fader: amplitude^(1/3)
            if (std::fabs(lvl - 1) > 1e-3) bj["gain"] = lvl <= 1e-6 ? -60.0 : r3(60 * std::log10(lvl));
            bands.push_back(bj);
        }
        fx.push_back({{"type", "multiband"}, {"crossovers", cross}, {"bands", bands}});
        return;
    }
    if (n == "Peak Limiter") {
        if (const double g = bwParam(d, "GAIN", 0); std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        fx.push_back({{"type", "limiter"}, {"ceiling", r3(bwParam(d, "CEILING", -1))}});
        return;
    }
    if (n == "Tool") {
        const double vol = bwParam(d, "VOLUME", 1);
        const double g = bwParam(d, "AMPLITUDE", 0) + (vol <= 1e-6 ? -60.0 : 60 * std::log10(vol));
        if (std::fabs(g) > 0.01) fx.push_back({{"type", "gain"}, {"db", r3(g)}});
        if (const double p = bwParam(d, "PAN", 0); std::fabs(p) > 1e-3) fx.push_back({{"type", "pan"}, {"position", r3(p)}});
        return;
    }
    c.res->notes.push_back(where + ": Bitwig's " + n + " has no Wavelength equivalent yet; left out");
}

void bwChain(Ctx &c, const std::vector<bitwig::Device> &devs, const std::string &where, json &fx) {
    for (auto &d : devs) bwEffect(c, d, where, fx);
}

// a Sampler's sample on disk: Bitwig package paths ("Bitwig/Classic Drum Machines:14/samples/Legend
// 707/Kick.wav") live under installed-packages/<format>/<vendor>/<package>/
std::string bitwigSampleFile(const std::string &s) {
    std::error_code ec;
    if (s.empty()) return "";
    if (s[0] == '/' || (s.size() > 2 && s[1] == ':')) return fs::exists(s, ec) ? s : "";
    std::vector<std::string> parts;
    for (size_t a = 0;;) { const size_t b = s.find('/', a); parts.push_back(s.substr(a, b - a)); if (b == std::string::npos) break; a = b + 1; }
    if (parts.size() < 3) return "";
    const std::string pkg = parts[1].substr(0, parts[1].find(':'));
    fs::path inKind, rest;
    for (size_t k = 2; k < parts.size(); ++k) { inKind /= parts[k]; if (k > 2) rest /= parts[k]; }
    for (auto &root : sampleRoots())
        for (auto &r : {rest, inKind}) {
            const fs::path f = fs::path(root) / parts[0] / pkg / r;
            if (fs::exists(f, ec)) return f.string();
        }
    return "";
}

// an instrument device: a plugin, or Bitwig's Sampler playing one sample
bool bwInstrument(Ctx &c, const bitwig::Device &d, const std::string &where, bool drumPad, json &out) {
    if (d.kind != "native") return bwPlugin(c, d, where, out);
    if (d.name == "Sampler" && !d.multisample.empty()) {   // an installed .multisample of that name plays all its zones
        for (auto &e : sampleLibrary())
            if (e.kind == "multisample" && e.name == d.multisample) {
                out = {{"plugin", "builtin:sampler"}, {"sampler", {{"multisample", e.path}}}};
                return true;
            }
    }
    if (d.name == "Sampler" && !d.sample.empty()) {
        const std::string file = bitwigSampleFile(d.sample);
        if (file.empty()) { c.res->notes.push_back(where + ": the Sampler's sample '" + d.sample + "' isn't installed; left out"); return false; }
        json sm = {{"sample", file}, {"root", d.sampleRoot}};
        if (drumPad) sm["oneShot"] = true;
        out = {{"plugin", "builtin:sampler"}, {"sampler", sm}};
        return true;
    }
    return false;
}

std::string findBitwigProject(const std::string &dawproject) {
    const fs::path p(dawproject);
    const std::string stem = p.stem().string();
    std::vector<fs::path> cands = {p.parent_path() / (stem + ".bwproject")};
    if (const char *home = std::getenv("HOME")) {
        cands.push_back(fs::path(home) / "Documents" / "Bitwig Studio" / "Projects" / stem / (stem + ".bwproject"));
        cands.push_back(fs::path(home) / "Bitwig Studio" / "Projects" / stem / (stem + ".bwproject"));
    }
    std::error_code ec;
    for (auto &c : cands) if (fs::exists(c, ec)) return c.string();
    return "";
}

// Notes of a lane or clip, into song beats. `offset` = song beat of content time 0; notes outside
// [from, to) song beats are dropped and ones crossing `to` are shortened.
void collectNotes(Ctx &c, const xml::Node &n, double offset, double from, double to, json &notes) {
    for (auto &chp : n.children) {
        const xml::Node &ch = *chp;
        if (ch.tag == "Notes") {
            for (const xml::Node *note : ch.all("Note")) {
                const double t = offset + note->num("time"), d = note->num("duration");
                if (t < from - 1e-9 || t >= to - 1e-9) continue;
                const double dur = std::min(d, to - t);
                if (dur <= 0) continue;
                json nj = {{"beat", r3(t)}, {"dur", r3(dur)}, {"key", (int)note->num("key", 60)}, {"vel", r3(std::clamp(note->num("vel", 0.8), 0.0, 1.0))}};
                const int chn = (int)note->num("channel", 0);
                if (chn) nj["channel"] = chn;
                notes.push_back(nj);
                ++c.res->noteCount;
            }
        } else if (ch.tag == "Clips") {
            for (const xml::Node *clip : ch.all("Clip")) {
                const double t = offset + clip->num("time"), d = clip->num("duration");
                const double ps = clip->num("playStart", 0);
                const double cs = std::max(from, t), ce = std::min(to, t + d);
                if (ce <= cs) continue;
                const bool loops = clip->attr("loopEnd") != nullptr;
                const double ls = clip->num("loopStart", 0), le = clip->num("loopEnd", 0);
                if (!loops || le <= ls) { collectNotes(c, *clip, t - ps, cs, ce, notes); continue; }
                // looping content: play from playStart to loopEnd, then loopStart..loopEnd again until the clip ends
                double pos = ps, song = t;
                for (int guard = 0; song < t + d - 1e-9 && guard < 10000; ++guard) {
                    const double segEnd = pos < le ? le : pos + (le - ls);
                    const double len = segEnd - pos;
                    collectNotes(c, *clip, song - pos, std::max(cs, song), std::min(ce, song + len), notes);
                    song += len;
                    pos = ls;
                }
            }
        } else if (ch.tag == "Lanes") collectNotes(c, ch, offset, from, to, notes);
    }
}

// an audio file from the archive (or an external path) -> a job-relative path ("" on failure)
std::string audioFile(Ctx &c, const xml::Node &file, const std::string &where) {
    const std::string path = file.get("path");
    if (path.empty()) return "";
    std::string ext = fs::path(path).extension().string();
    for (auto &ch : ext) ch = (char)std::tolower((unsigned char)ch);
    if (ext != ".wav") { c.res->notes.push_back(where + ": audio file " + fs::path(path).filename().string() + " isn't WAV (only WAV clips play yet); left out"); return ""; }
    if (file.get("external") == "true") {
        std::error_code ec;
        if (fs::exists(path, ec)) return path;
        c.res->notes.push_back(where + ": audio file " + path + " is missing; left out");
        return "";
    }
    const std::string rel = "audio/" + fs::path(path).filename().string();
    if (c.written.count(rel)) return rel;
    std::vector<uint8_t> data;
    std::string err;
    if (!c.zip.read(path, data, err)) { c.res->notes.push_back(where + ": audio file " + path + " is missing from the file"); return ""; }
    std::error_code ec;
    fs::create_directories(fs::path(c.outDir) / "audio", ec);
    std::ofstream o(fs::path(c.outDir) / rel, std::ios::binary);
    o.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
    c.written.insert(rel);
    return rel;
}

// Audio clips of a lane or clip, into song beats (like collectNotes): `offset` = song beat of content
// time 0, clips cut to [from, to). Warped audio gets the file's tempo from its first and last warp
// markers (it then follows the song's tempo); unwarped audio plays at its own speed.
void collectAudio(Ctx &c, const xml::Node &n, double offset, double from, double to, const std::string &where, json &clips) {
    auto add = [&](const std::string &file, double song0, double song1, double startSec, double fileBpm, bool stretch) {
        if (song1 - song0 < 1e-6 || file.empty()) return;
        json cl = {{"file", file}, {"beat", r3(song0)}, {"start", r3(std::max(0.0, startSec))}};
        if (fileBpm > 0) { cl["bpm"] = r3(fileBpm); cl["beats"] = r3(song1 - song0); if (!stretch) cl["stretch"] = false; }
        else cl["length"] = r3((song1 - song0) * 60.0 / c.bpm);
        clips.push_back(cl);
    };
    for (auto &chp : n.children) {
        const xml::Node &ch = *chp;
        if (ch.tag == "Audio") {   // unwarped: content time is the file's own seconds
            const xml::Node *f = ch.child("File");
            if (!f) continue;
            const double s0 = std::max(from, offset), s1 = to;
            add(audioFile(c, *f, where), s0, s1, (s0 - offset) * 60.0 / c.bpm, 0, true);
        } else if (ch.tag == "Warps") {
            const xml::Node *au = ch.child("Audio");
            const xml::Node *f = au ? au->child("File") : nullptr;
            const auto warps = ch.all("Warp");
            if (!f) continue;
            const std::string file = audioFile(c, *f, where);
            const bool stretch = !au || au->get("algorithm") != "repitch";
            if (warps.size() < 2) { add(file, std::max(from, offset), to, (std::max(from, offset) - offset) * 60.0 / c.bpm, 0, stretch); continue; }
            const double t0 = warps.front()->num("time"), c0 = warps.front()->num("contentTime");
            const double t1 = warps.back()->num("time"), c1 = warps.back()->num("contentTime");
            if (t1 - t0 < 1e-9 || c1 - c0 < 1e-9) continue;
            if (warps.size() > 2) c.res->notes.push_back(where + ": a clip with " + std::to_string(warps.size()) + " warp markers plays at one tempo (its first to last marker)");
            const double secPerBeat = (c1 - c0) / (t1 - t0);
            const double x0 = std::max(from, offset) - offset;
            add(file, offset + x0, to, c0 + (x0 - t0) * secPerBeat, 60.0 / secPerBeat, stretch);
        } else if (ch.tag == "Clips") {
            for (const xml::Node *clip : ch.all("Clip")) {
                const double t = offset + clip->num("time"), d = clip->num("duration");
                const double ps = clip->num("playStart", 0);
                const double cs = std::max(from, t), ce = std::min(to, t + d);
                if (ce <= cs) continue;
                const size_t first = clips.size();
                const bool loops = clip->attr("loopEnd") != nullptr;
                const double ls = clip->num("loopStart", 0), le = clip->num("loopEnd", 0);
                if (!loops || le <= ls) collectAudio(c, *clip, t - ps, cs, ce, where, clips);
                else {
                    double pos = ps, song = t;
                    for (int guard = 0; song < t + d - 1e-9 && guard < 10000; ++guard) {
                        const double segEnd = pos < le ? le : pos + (le - ls);
                        const double len = segEnd - pos;
                        collectAudio(c, *clip, song - pos, std::max(cs, song), std::min(ce, song + len), where, clips);
                        song += len;
                        pos = ls;
                    }
                }
                if (clips.size() > first) {   // fades at the clip's edges
                    const double k = clip->get("fadeTimeUnit", "beats") == "seconds" ? 1000.0 : 60000.0 / c.bpm;
                    if (const double fi = clip->num("fadeInTime", 0); fi > 0) clips[first]["fadeIn"] = r3(fi * k);
                    if (const double fo = clip->num("fadeOutTime", 0); fo > 0) clips.back()["fadeOut"] = r3(fo * k);
                }
            }
        } else if (ch.tag == "Lanes") collectAudio(c, ch, offset, from, to, where, clips);
    }
}

// absolute volume points (dB) -> dB relative to a fader: gain automation adds to the fader
json relCurve(const json &curve, double fader) {
    json rel = json::array();
    for (auto &p : curve) { json q = p; q[1] = r3(p[1].get<double>() - fader); rel.push_back(q); }
    return rel;
}

// Automation: Points lanes whose target is a channel's Volume or Pan, or an instrument plugin's parameter.
// A point's interpolation describes the segment after it ("hold" = step to the next point).
void collectAutomation(Ctx &c, const xml::Node &lanes, std::map<std::string, json> &gainPts, std::map<std::string, json> &panPts,
                       std::map<std::string, json> &paramPts, size_t &unmapped, size_t &onEffects) {
    std::vector<const xml::Node *> pts;
    lanes.walk("Points", pts);
    for (const xml::Node *p : pts) {
        const xml::Node *target = p->child("Target");
        const std::string param = target ? target->get("parameter") : "";
        auto it = c.paramTarget.find(param);
        if (it == c.paramTarget.end()) { ++unmapped; continue; }
        const std::string &kind = it->second.second;
        if (kind == "fx") { ++onEffects; continue; }
        json curve = json::array();
        bool holdNext = false;
        for (const xml::Node *rp : p->all("RealPoint")) {
            const double v = rp->num("value");
            const double y = kind == "volume" ? r3(linToDb(v)) : kind == "pan" ? r3(v * 2 - 1) : r6(v);
            json pt = {r3(rp->num("time")), y};
            if (holdNext) pt.push_back("step");
            curve.push_back(pt);
            holdNext = rp->get("interpolation", "linear") == "hold";
        }
        if (curve.empty()) { ++unmapped; continue; }
        if (kind == "volume") gainPts[it->second.first] = curve;
        else if (kind == "pan") panPts[it->second.first] = curve;
        else {
            const bool norm = kind.rfind("param:", 0) == 0;
            json cj = {{"points", curve}};
            if (norm) cj["scale"] = "normalized";
            paramPts[it->second.first][kind.substr(kind.find(':') + 1)] = cj;
        }
    }
}

} // namespace

bool importDawproject(const std::string &path, const std::string &outDir, DawprojectImport &res, std::string &err, const std::string &bitwigPath) {
    Ctx c;
    c.outDir = outDir;
    c.res = &res;
    if (!c.zip.open(path, err)) return false;
    // the Bitwig project behind the export holds what DAWproject leaves out (Bitwig's own devices)
    const std::string bwPath = bitwigPath == "none" ? "" : bitwigPath.empty() ? findBitwigProject(path) : bitwigPath;
    if (!bwPath.empty()) {
        std::string e;
        if (bitwig::load(bwPath, c.bw, e) && c.bwZip.open(bwPath, e)) { c.haveBw = true; res.bitwig = bwPath; }
        else if (!bitwigPath.empty()) { err = e; return false; }
        else res.notes.push_back("found " + bwPath + " but couldn't read it (" + e + "); Bitwig's own devices are left out");
    }
    std::vector<uint8_t> xmlBytes;
    if (!c.zip.read("project.xml", xmlBytes, err)) { err = path + ": " + err; return false; }
    auto root = xml::parse(std::string(xmlBytes.begin(), xmlBytes.end()), err);
    if (!root) { err = path + ": project.xml: " + err; return false; }
    if (const xml::Node *app = root->child("Application")) res.application = app->get("name") + " " + app->get("version");

    json job = json::object();
    // transport
    if (const xml::Node *tr = root->child("Transport")) {
        if (const xml::Node *t = tr->child("Tempo")) c.bpm = t->num("value", 120);
        if (const xml::Node *ts = tr->child("TimeSignature")) job["timeSignature"] = {(int)ts->num("numerator", 4), (int)ts->num("denominator", 4)};
    }
    job["tempo"] = r3(c.bpm);
    const xml::Node *arrangement = nullptr;
    if (const xml::Node *st = root->child("Arrangement")) arrangement = st;
    if (!arrangement) { err = path + " has no arrangement (only launcher clips?)"; return false; }
    const xml::Node *arrLanes = arrangement->child("Lanes");
    if (arrLanes && arrLanes->get("timeUnit", "beats") != "beats") res.notes.push_back("arrangement is timed in seconds; times were read as beats (check the tempo)");
    if (const xml::Node *ta = arrangement->child("TempoAutomation")) {   // a tempo map
        json map = json::array();
        for (const xml::Node *p : ta->all("RealPoint"))
            map.push_back({{"beat", r3(p->num("time"))}, {"bpm", r3(p->num("value"))}, {"ramp", p->get("interpolation") == "linear"}});
        if (map.size() > 1) job["tempo"] = map;
    }

    // tracks, effect returns and groups: walk the structure (groups nest tracks)
    const xml::Node *structure = root->child("Structure");
    if (!structure) { err = path + " has no Structure"; return false; }
    struct TrackRef { const xml::Node *track, *channel; std::string name, role; bool group; };
    std::vector<TrackRef> refs;
    std::function<void(const xml::Node &)> walk = [&](const xml::Node &n) {
        for (const xml::Node *t : n.all("Track")) {
            const xml::Node *ch = t->child("Channel");
            const bool group = !t->all("Track").empty();
            refs.push_back({t, ch, "", ch ? ch->get("role", "regular") : "regular", group});
            if (group) walk(*t);
        }
    };
    walk(*structure);
    std::string masterChannel;
    for (auto &r : refs) {
        if (!r.channel) continue;
        if (r.role == "master") {
            masterChannel = r.channel->get("id");
            r.name = "Master";
            if (const xml::Node *pn = r.channel->child("Volume")) c.paramTarget[pn->get("id")] = {"Master", "volume"};
            continue;
        }
        r.name = uniqueName(c, r.track->get("name", "Track"));
        if (r.role == "effect" || r.group) c.busByChannel[r.channel->get("id")] = r.name;
        else c.trackByChannel[r.channel->get("id")] = r.name;
        c.channelOfTrack[r.track->get("id")] = r.channel->get("id");
        for (const char *p : {"Volume", "Pan"})
            if (const xml::Node *pn = r.channel->child(p)) c.paramTarget[pn->get("id")] = {r.name, p == std::string("Volume") ? "volume" : "pan"};
    }
    // plugin parameters that automation can target: the instrument's go to its track
    for (auto &r : refs) {
        if (!r.channel) continue;
        const xml::Node *devs = r.channel->child("Devices");
        if (!devs) continue;
        bool instrumentSeen = false;
        for (auto &dp : devs->children) {
            const bool instrument = !instrumentSeen && dp->get("deviceRole") == "instrument" && r.role == "regular" && !r.group;
            if (dp->get("deviceRole") == "instrument") instrumentSeen = true;
            const xml::Node *ps = dp->child("Parameters");
            if (!ps) continue;
            for (auto &pp : ps->children) {
                const std::string pid = pp->get("parameterID");
                if (pid.empty()) continue;
                if (!instrument) { c.paramTarget[pp->get("id")] = {r.name, "fx"}; continue; }
                c.paramTarget[pp->get("id")] = {r.name, (pp->get("unit", "normalized") == "normalized" ? "param:#" : "plain:#") + pid};
            }
        }
    }

    // arrangement notes per track, and volume/pan automation
    std::map<std::string, json> notesOf, clipsOf, gainPts, panPts, paramPts;
    size_t unmappedAuto = 0, fxAuto = 0;
    if (arrLanes)
        for (const xml::Node *ln : arrLanes->all("Lanes")) {
            const std::string trackId = ln->get("track");
            json notes = json::array(), audio = json::array();
            collectNotes(c, *ln, 0, -1e18, 1e18, notes);
            {
                auto ch = c.channelOfTrack.find(trackId);
                const std::string name = ch != c.channelOfTrack.end() && c.trackByChannel.count(ch->second) ? c.trackByChannel[ch->second] : "";
                collectAudio(c, *ln, 0, -1e18, 1e18, name.empty() ? "audio" : name, audio);
                if (!audio.empty()) {
                    if (!name.empty()) for (auto &a : audio) clipsOf[name].push_back(a);
                    else res.notes.push_back(std::to_string(audio.size()) + " audio clip(s) on a lane without a track were left out");
                }
            }
            if (!notes.empty()) {
                auto ch = c.channelOfTrack.find(trackId);
                const std::string name = ch != c.channelOfTrack.end() && c.trackByChannel.count(ch->second) ? c.trackByChannel[ch->second] : "";
                if (!name.empty()) for (auto &n : notes) notesOf[name].push_back(n);
                else res.notes.push_back(std::to_string(notes.size()) + " notes on a lane without an instrument track were left out");
            }
        }
    collectAutomation(c, *arrangement, gainPts, panPts, paramPts, unmappedAuto, fxAuto);
    if (unmappedAuto) res.notes.push_back(std::to_string(unmappedAuto) + " automation lane(s) on parameters the file doesn't describe were left out (a DAW's own devices)");
    if (fxAuto) res.notes.push_back(std::to_string(fxAuto) + " automation lane(s) on plugin effects were left out (instrument, volume and pan automation are imported)");
    {
        size_t launcher = 0;
        std::vector<const xml::Node *> slots;
        root->walk("ClipSlot", slots);
        for (auto *s : slots) if (s->child("Clip")) ++launcher;
        if (launcher) res.notes.push_back(std::to_string(launcher) + " clip-launcher clip(s) aren't rendered: only the arrangement plays");
    }

    // match the export's tracks to the Bitwig project's (both in arranger order)
    std::map<const xml::Node *, const bitwig::Track *> bwTrackOf;
    if (c.haveBw) {
        std::vector<const TrackRef *> regular, effects;
        for (auto &r : refs) {
            if (!r.channel || r.role == "master") continue;
            (r.role == "effect" ? effects : regular).push_back(&r);
        }
        auto match = [&](const std::vector<const TrackRef *> &mine, const std::vector<bitwig::Track> &theirs, const char *what) {
            if (mine.size() != theirs.size()) {
                res.notes.push_back(std::string("the Bitwig project has ") + std::to_string(theirs.size()) + " " + what + " and the export " + std::to_string(mine.size()) + "; their devices come from the export only");
                return;
            }
            for (size_t k = 0; k < mine.size(); ++k) bwTrackOf[mine[k]->track] = &theirs[k];
        };
        match(regular, c.bw.tracks, "tracks");
        match(effects, c.bw.effects, "effect tracks");
    }

    // build tracks and buses
    json tracks = json::array(), buses = json::array();
    json master = {{"gain", 0}, {"fx", json::array()}};
    for (auto &r : refs) {
        if (!r.channel) continue;
        const xml::Node &ch = *r.channel;
        const double vol = ch.child("Volume") ? ch.child("Volume")->num("value", 1) : 1;
        const double pan = ch.child("Pan") ? ch.child("Pan")->num("value", 0.5) * 2 - 1 : 0;
        const bool muted = ch.child("Mute") && ch.child("Mute")->get("value") == "true";
        json fx = json::array();
        json instrument;
        std::vector<const bitwig::Device::Pad *> pads;   // a Drum Machine's pads, from the Bitwig project
        const bitwig::Track *bt = nullptr;
        if (r.role == "master") bt = c.haveBw && c.bw.hasMaster ? &c.bw.master : nullptr;
        else if (bwTrackOf.count(r.track)) bt = bwTrackOf[r.track];
        if (bt) {
            // the export's device roles, device for device, when the two lists line up
            std::vector<std::string> roles;
            if (const xml::Node *devs = ch.child("Devices")) for (auto &dp : devs->children) roles.push_back(dp->get("deviceRole", "audioFX"));
            const bool aligned = roles.size() == bt->devices.size();
            const bool instrumentTrack = r.role != "master" && r.role != "effect" && !r.group;
            for (size_t k = 0; k < bt->devices.size(); ++k) {
                const bitwig::Device &d = bt->devices[k];
                const std::string role = aligned ? roles[k] : (k == 0 && instrumentTrack ? "instrument" : "audioFX");
                if (role == "noteFX") { res.notes.push_back(r.name + ": note effect '" + d.name + "' left out (note effects aren't imported)"); continue; }
                if (role == "instrument" && instrument.is_null() && pads.empty()) {
                    if (!d.pads.empty()) {   // the kit's own effects go on the drum bus
                        for (auto &p : d.pads) pads.push_back(&p);
                        if (auto g = d.chains.find("GLOBAL_EFFECT_CHAIN"); g != d.chains.end()) bwChain(c, g->second, r.name, fx);
                        continue;
                    }
                    json m;
                    if (bwInstrument(c, d, r.name, false, m)) {
                        instrument = m;
                        if (auto sfx = d.chains.find("FX"); d.kind == "native" && sfx != d.chains.end()) bwChain(c, sfx->second, r.name + " » " + d.name, fx);
                        continue;
                    }
                    res.notes.push_back(r.name + ": Bitwig's " + d.name + " instrument has no Wavelength equivalent yet; left out");
                    continue;
                }
                bwEffect(c, d, r.name, fx);
            }
        } else if (const xml::Node *devs = ch.child("Devices"))
            for (auto &dp : devs->children) {
                const std::string role = dp->get("deviceRole", "audioFX");
                if (role == "noteFX") { res.notes.push_back(r.name + ": note effect '" + dp->get("name") + "' left out (note effects aren't imported)"); continue; }
                json m;
                if (!mapDevice(c, *dp, r.name, m)) {
                    if (role == "instrument" && dp->get("deviceName") == "Drum Machine") {   // Bitwig's drum sampler: a GM kit stands in
                        instrument = {{"plugin", "builtin:drums"}};
                        res.notes.push_back(r.name + ": Bitwig Drum Machine samples aren't in the export; builtin:drums (General MIDI kit) stands in. Swap in a sampler kit that matches");
                    }
                    continue;
                }
                if (role == "instrument" && instrument.is_null()) instrument = m;
                else fx.push_back(m);
            }
        const std::string dest = ch.get("destination");
        std::string output;
        if (!dest.empty() && dest != masterChannel && c.busByChannel.count(dest)) output = c.busByChannel[dest];
        json sends = json::object();
        if (const xml::Node *ss = ch.child("Sends"))
            for (const xml::Node *s : ss->all("Send")) {
                const xml::Node *on = s->child("Enable"), *lv = s->child("Volume");
                const double v = lv ? lv->num("value", 0) : 0;
                if ((on && on->get("value") == "false") || v <= 1e-6 || !c.busByChannel.count(s->get("destination"))) continue;
                sends[c.busByChannel[s->get("destination")]] = r3(linToDb(v));
                if (s->get("type") == "pre") res.notes.push_back(r.name + ": a pre-fader send is imported as post-fader");
            }
        if (r.role == "master") {
            master["gain"] = r3(linToDb(vol));
            master["fx"] = fx;
            if (gainPts.count("Master")) master["automation"] = {{"gain", relCurve(gainPts["Master"], master["gain"].get<double>())}};
            continue;
        }
        if (r.role == "effect" || r.group) {
            json b = {{"name", r.name}, {"gain", r3(linToDb(vol))}, {"fx", fx}};
            if (!output.empty()) b["output"] = output;
            if (gainPts.count(r.name)) b["automation"] = {{"gain", relCurve(gainPts[r.name], b["gain"].get<double>())}};
            buses.push_back(b);
            ++res.buses;
            continue;
        }
        // audio clips: a builtin:audio track with this track's mixer settings (beside the notes, if any)
        auto mixerOf = [&](json &t) {
            t["gain"] = vol <= 1e-6 ? -60.0 : r3(linToDb(vol));
            if (std::fabs(pan) > 1e-3) t["pan"] = r3(pan);
            if (muted || vol <= 1e-6) t["mute"] = true;
            if (!fx.empty()) t["fx"] = fx;
            if (!output.empty()) t["output"] = output;
            if (!sends.empty()) t["sends"] = sends;
            json autom = json::object();
            if (gainPts.count(r.name)) autom["gain"] = relCurve(gainPts[r.name], t["gain"].get<double>());
            if (panPts.count(r.name)) autom["pan"] = panPts[r.name];
            if (!autom.empty()) t["automation"] = autom;
        };
        if (clipsOf.count(r.name)) {
            json t = {{"name", notesOf.count(r.name) ? uniqueName(c, r.name + " audio") : r.name}, {"plugin", "builtin:audio"}};
            mixerOf(t);
            t["clips"] = clipsOf[r.name];
            tracks.push_back(t);
            ++res.tracks;
        }
        if (!notesOf.count(r.name)) {
            if (!clipsOf.count(r.name)) res.notes.push_back(r.name + ": nothing in the arrangement; left out");
            continue;
        }
        if (!pads.empty()) {
            // a Drum Machine: one track per pad (its instrument gets C3, as in Bitwig) into a bus that
            // carries the drum track's fader, pan and effects
            const double busGain = vol <= 1e-6 ? -60.0 : r3(linToDb(vol));
            json b = {{"name", r.name}, {"gain", busGain}, {"fx", fx}};
            if (std::fabs(pan) > 1e-3) b["fx"].push_back({{"type", "pan"}, {"position", r3(pan)}});
            if (!output.empty()) b["output"] = output;
            if (gainPts.count(r.name)) b["automation"] = {{"gain", relCurve(gainPts[r.name], busGain)}};
            if (!sends.empty()) res.notes.push_back(r.name + ": sends from a Drum Machine track are left out");
            buses.push_back(b);
            ++res.buses;
            for (const bitwig::Device::Pad *pad : pads) {
                json padNotes = json::array();
                for (auto &nj : notesOf[r.name])
                    if (nj["key"].get<int>() == pad->key) { json x = nj; x["key"] = kPadNote; padNotes.push_back(x); }
                if (padNotes.empty()) continue;
                const std::string where = r.name + " pad " + std::to_string(pad->key);
                if (pad->mute) { res.notes.push_back(where + " is muted in Bitwig; left out"); continue; }
                json t;
                json padFx = json::array();
                for (auto &d : pad->devices) {
                    if (t.is_null()) {
                        if (bwInstrument(c, d, where, true, t)) {
                            const std::string label = d.name == "Sampler" ? fs::path(d.sample).stem().string() : d.name;
                            t["name"] = uniqueName(c, r.name + " " + std::to_string(pad->key) + " " + label);
                            if (auto sfx = d.chains.find("FX"); d.kind == "native" && sfx != d.chains.end()) bwChain(c, sfx->second, where + " » " + d.name, padFx);
                            continue;
                        }
                        if (d.kind == "native" && d.name != "Sampler") res.notes.push_back(where + ": Bitwig's " + d.name + " has no Wavelength equivalent yet; left out");
                        break;
                    }
                    bwEffect(c, d, where, padFx);
                }
                if (t.is_null()) continue;
                t["gain"] = pad->volume <= 1e-6 ? -60.0 : r3(60 * std::log10(pad->volume));   // pad faders store amplitude^(1/3)
                if (std::fabs(pad->pan) > 1e-3) t["pan"] = r3(pad->pan);
                if (muted) t["mute"] = true;
                if (!padFx.empty()) t["fx"] = padFx;
                t["output"] = r.name;
                t["notes"] = padNotes;
                tracks.push_back(t);
                ++res.tracks;
            }
            continue;
        }
        if (instrument.is_null()) { res.notes.push_back(r.name + ": no instrument the file can describe; left out"); continue; }
        json t = instrument;
        t["name"] = r.name;
        t["gain"] = vol <= 1e-6 ? -60.0 : r3(linToDb(vol));
        if (std::fabs(pan) > 1e-3) t["pan"] = r3(pan);
        if (muted || vol <= 1e-6) t["mute"] = true;
        if (!fx.empty()) t["fx"] = fx;
        if (!output.empty()) t["output"] = output;
        if (!sends.empty()) t["sends"] = sends;
        json autom = json::object();
        if (gainPts.count(r.name)) autom["gain"] = relCurve(gainPts[r.name], t["gain"].get<double>());
        if (panPts.count(r.name)) autom["pan"] = panPts[r.name];
        if (paramPts.count(r.name)) autom["params"] = paramPts[r.name];
        if (!autom.empty()) t["automation"] = autom;
        t["notes"] = notesOf[r.name];
        tracks.push_back(t);
        ++res.tracks;
    }
    job["tracks"] = tracks;
    if (!buses.empty()) job["buses"] = buses;
    job["master"] = master;
    // markers
    json markers = json::array();
    if (const xml::Node *ms = arrangement->child("Markers"))
        for (const xml::Node *m : ms->all("Marker")) markers.push_back({{"beat", r3(m->num("time"))}, {"name", m->get("name", "Marker")}});
    if (!markers.empty()) job["markers"] = markers;
    res.job = job;

    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::ofstream o(fs::path(outDir) / "job.json");
    o << job.dump(1) << "\n";
    if (!o) { err = "cannot write " + (fs::path(outDir) / "job.json").string(); return false; }
    return true;
}

} // namespace wl
