// Sonic Charge Microtonic kits (.mtpreset) and drums (.mtdrum). The text presets hold display
// values ("84.3Hz", "1/16"); the plugin's own text parser turns them into parameter values, which
// go into its binary state (a one-program FXB bank) or, for a single drum, onto one channel.
#include "microtonic.hpp"

#include "plugin.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace wl {

namespace {

struct Node {
    std::string value;
    std::vector<std::string> list;
    std::map<std::string, std::unique_ptr<Node>> kids;
    const Node *get(const std::string &k) const { auto it = kids.find(k); return it == kids.end() ? nullptr : it->second.get(); }
    std::string str(const std::string &k, const std::string &def = "") const { const Node *n = get(k); return n ? n->value : def; }
};

std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "Key: value" with {} nesting (V3) or "Key=value" / "Key = value" (V1/V2)
bool parseText(const std::string &text, Node &root, std::string &header) {
    std::vector<Node *> stack = {&root};
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        const std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty()) continue;
        if (line == "}") { if (stack.size() > 1) stack.pop_back(); continue; }
        const size_t sep = line.find_first_of(":=");
        if (sep == std::string::npos) continue;
        const std::string k = trim(line.substr(0, sep));
        std::string v = trim(line.substr(sep + 1));
        auto node = std::make_unique<Node>();
        Node *raw = node.get();
        stack.back()->kids[k] = std::move(node);
        if (v == "{") { stack.push_back(raw); continue; }
        if (v.size() > 1 && v.front() == '{' && v.back() == '}') {
            std::string inner = v.substr(1, v.size() - 2);
            for (size_t a = 0, b; a <= inner.size(); a = b + 1) {
                b = inner.find(',', a);
                if (b == std::string::npos) b = inner.size();
                raw->list.push_back(trim(inner.substr(a, b - a)));
            }
        } else if (v.size() > 1 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        raw->value = v;
    }
    if (root.kids.size() != 1) return false;
    header = root.kids.begin()->first;
    return true;
}

const char *kDrumKeys[25] = {"OscWave", "OscFreq", "OscAtk", "OscDcy", "ModMode", "ModRate", "ModAmt", "NFilMod", "NFilFrq",
                             "NFilQ", "NStereo", "NEnvMod", "NEnvAtk", "NEnvDcy", "Mix", "DistAmt", "EQFreq", "EQGain",
                             "Level", "Pan", "Output", "Choke", "OscVel", "NVel", "ModVel"};
const std::map<std::string, std::vector<std::string>> kEnums = {
    {"OscWave", {"Sine", "Triangle", "Saw"}}, {"ModMode", {"Decay", "Sine", "Noise"}}, {"NFilMod", {"LP", "BP", "HP"}},
    {"NStereo", {"Off", "On"}}, {"NEnvMod", {"Exp", "Linear", "Mod"}}, {"Output", {"A", "B"}}, {"Choke", {"Off", "On"}}};
std::string missing(const std::string &k) { return k == "OscAtk" ? "0 ms" : k == "Choke" ? "Off" : k == "Output" ? "A" : ""; }

void be32(std::vector<uint8_t> &o, uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i))); }
void bef(std::vector<uint8_t> &o, float f) { uint32_t u; std::memcpy(&u, &f, 4); be32(o, u); }
void lp(std::vector<uint8_t> &o, const std::string &s) { be32(o, (uint32_t)s.size()); o.insert(o.end(), s.begin(), s.end()); }
uint32_t rbe32(const std::vector<uint8_t> &d, size_t i) { return (uint32_t)d[i] << 24 | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | d[i + 3]; }

// normalized value of a parameter from its display text, read by the plugin
bool norm(Plugin &plugin, const std::string &param, const std::string &text, float &out, std::string &err) {
    ParamInfo pi;
    double plain = 0;
    if (!plugin.findParam(param, pi)) { err = "Microtonic has no parameter " + param; return false; }
    if (!plugin.valueFromText(pi.id, text, plain)) { err = "Microtonic could not read '" + text + "' for " + param; return false; }
    out = (float)((plain - pi.min) / std::max(1e-12, pi.max - pi.min));
    return true;
}

// the 25 values of one drum on channel `ch`: modes first, so the other values are read in their context
bool drumValues(Plugin &plugin, const Node &d, int ch, std::vector<float> &v, std::string &err) {
    for (auto &[k, names] : kEnums) {
        const std::string text = d.str(k, missing(k));
        size_t idx = 0;
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == text) idx = i;
        ParamInfo pi;
        if (plugin.findParam(k + std::to_string(ch), pi))
            plugin.setControllerValue(pi.id, pi.min + (pi.max - pi.min) * (double)idx / (double)(names.size() - 1));
    }
    v.clear();
    for (const char *k : kDrumKeys) {
        float f;
        if (!norm(plugin, k + std::to_string(ch), d.str(k, missing(k)), f, err)) return false;
        v.push_back(f);
    }
    return true;
}

uint16_t stepBits(const std::string &s) {
    uint16_t b = 0;
    int i = 0;
    for (char c : s) {
        if (c == ' ') continue;
        if (i >= 16) break;
        if (c == '#') b |= (uint16_t)(0x8000 >> i);
        ++i;
    }
    return b;
}

// bytes after the programs of the plugin's own bank (MIDI note map + an opaque blob), kept as is
bool bankTail(const std::vector<uint8_t> &comp, std::vector<uint8_t> &tail) {
    const size_t base = 170;
    if (comp.size() < base + 8) return false;
    const uint32_t count = rbe32(comp, base + 4);
    size_t o = base + 8;
    auto need = [&](size_t n) { return o + n <= comp.size(); };
    for (uint32_t p = 0; p < count; ++p) {
        if (!need(8)) return false;
        o += 4; o += 4 + rbe32(comp, o);
        o += 6;
        if (!need(4)) return false;
        o += 4 + rbe32(comp, o);
        o += 56 + 11 + 12 * 53 + 4;
        for (int d = 0; d < 8; ++d) {
            if (!need(4)) return false;
            o += 4 + rbe32(comp, o); o += 1;
            if (!need(4)) return false;
            o += 4 + 2 * (size_t)rbe32(comp, o) + 200;
        }
    }
    if (o > comp.size()) return false;
    tail.assign(comp.begin() + (long)o, comp.end());
    return true;
}

} // namespace

bool isMicrotonicText(const std::vector<uint8_t> &d) {
    std::string head(d.begin(), d.begin() + (long)std::min<size_t>(d.size(), 24));
    for (auto &c : head) c = (char)std::tolower((unsigned char)c);
    return head.rfind("microtonicpresetv", 0) == 0 || head.rfind("microtonicdrumpatchv", 0) == 0;
}

bool microtonicKitState(Plugin &plugin, const std::vector<uint8_t> &comp, const std::vector<uint8_t> &preset,
                        const std::string &name, std::vector<uint8_t> &out, std::string &err) {
    static const uint8_t magic[4] = {0x59, 0xa2, 0xcd, 0x18};
    if (comp.size() < 180 || std::memcmp(comp.data(), magic, 4) || std::memcmp(comp.data() + 10, "CcnK", 4)) {
        err = "a Microtonic kit loads into Microtonic only";
        return false;
    }
    Node root;
    std::string header;
    if (!parseText(std::string(preset.begin(), preset.end()), root, header) || header.rfind("MicrotonicPresetV", 0) != 0) {
        err = "not a Microtonic kit (.mtpreset)";
        return false;
    }
    const Node &body = *root.kids.begin()->second;
    const Node *drums = body.get("DrumPatches"), *patterns = body.get("Patterns");
    if (!drums || !patterns) { err = "kit has no DrumPatches / Patterns"; return false; }
    std::vector<uint8_t> tail;
    if (!bankTail(comp, tail)) { err = "unexpected Microtonic state layout"; return false; }

    std::vector<uint8_t> prog;
    be32(prog, 0x5c652a78);
    lp(prog, name);
    prog.push_back(1);
    bef(prog, (float)std::atof(body.str("Tempo", "120").c_str()));
    prog.push_back(0);
    lp(prog, "");
    // globals: Pattern, PlayStop, StepRate, Swing, FillRate, MastVol, Mute1-8. The kit waits for notes
    // (PlayStop off, no mutes); set "PlayStop" in params to let its own patterns play.
    float g[5];
    const char *gk[5] = {"Pattern", "StepRate", "Swing", "FillRate", "MastVol"};
    for (int i = 0; i < 5; ++i) if (!norm(plugin, gk[i], body.str(gk[i]), g[i], err)) return false;
    bef(prog, g[0]); bef(prog, 0.f); bef(prog, g[1]); bef(prog, g[2]); bef(prog, g[3]); bef(prog, g[4]);
    for (int i = 0; i < 8; ++i) bef(prog, 0.f);
    for (char c = 'a'; c <= 'k'; ++c) { const Node *p = patterns->get(std::string(1, c)); prog.push_back(p && p->str("Chained") == "true"); }
    for (char c = 'a'; c <= 'l'; ++c) {
        const Node *p = patterns->get(std::string(1, c));
        prog.insert(prog.end(), {0xed, 0xda, 0x50, 0x19});
        prog.push_back((uint8_t)(p ? std::atoi(p->str("Length", "16").c_str()) : 16));
        for (int ch = 1; ch <= 8; ++ch) {
            const Node *t = p ? p->get(std::to_string(ch)) : nullptr;
            for (const char *k : {"Triggers", "Accents", "Fills"}) {
                const uint16_t b = t ? stepBits(t->str(k)) : 0;
                prog.push_back((uint8_t)(b >> 8)); prog.push_back((uint8_t)b);
            }
        }
    }
    bef(prog, 0.5f);   // morph
    for (int ch = 1; ch <= 8; ++ch) {
        const Node *d = drums->get(std::to_string(ch));
        if (!d) { err = "kit has no drum " + std::to_string(ch); return false; }
        std::vector<float> v;
        if (!drumValues(plugin, *d, ch, v, err)) return false;
        lp(prog, d->str("Name", "Drum " + std::to_string(ch)));
        prog.push_back(d->str("Modified") == "true");
        const std::string path = d->str("Path");
        be32(prog, (uint32_t)path.size());
        for (unsigned char c : path) { prog.push_back(0); prog.push_back(c); }   // UTF-16BE (paths are ASCII)
        for (float f : v) { bef(prog, f); bef(prog, f); }                        // morph A and B
    }
    std::vector<uint8_t> chunk;
    be32(chunk, 0xeddb81a6);
    be32(chunk, 1);
    chunk.insert(chunk.end(), prog.begin(), prog.end());
    chunk.insert(chunk.end(), tail.begin(), tail.end());
    std::vector<uint8_t> fxb = {'F', 'B', 'C', 'h'};
    be32(fxb, 1);
    fxb.insert(fxb.end(), comp.begin() + 26, comp.begin() + 30);   // fxID: NuMT or NuMm (Multi)
    be32(fxb, 1); be32(fxb, 1);
    fxb.insert(fxb.end(), 128, 0);
    be32(fxb, (uint32_t)chunk.size());
    fxb.insert(fxb.end(), chunk.begin(), chunk.end());
    std::vector<uint8_t> body2 = {'C', 'c', 'n', 'K'};
    be32(body2, (uint32_t)fxb.size());
    body2.insert(body2.end(), fxb.begin(), fxb.end());
    out.assign(comp.begin(), comp.begin() + 4);
    out.push_back(0); out.push_back(0);
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(body2.size() >> (8 * i)));
    out.insert(out.end(), body2.begin(), body2.end());
    return true;
}

bool microtonicDrumParams(Plugin &plugin, const std::vector<uint8_t> &drum, int channel, std::vector<ParamValue> &values,
                          std::string &err) {
    Node root;
    std::string header;
    const Node *d = nullptr;   // "MicroTonicDrumPatchV1={" / "...V2={" / "MicrotonicDrumPatchV3: {"
    if (parseText(std::string(drum.begin(), drum.end()), root, header) && root.kids.begin()->second->get("OscWave"))
        d = root.kids.begin()->second.get();
    if (!d) { err = "not a Microtonic drum (.mtdrum)"; return false; }
    if (channel < 1 || channel > 8) { err = "Microtonic drum channels are 1-8"; return false; }
    std::vector<float> v;
    if (!drumValues(plugin, *d, channel, v, err)) return false;
    for (int i = 0; i < 25; ++i) {
        ParamInfo pi;
        if (!plugin.findParam(kDrumKeys[i] + std::to_string(channel), pi)) continue;
        values.push_back({pi.id, pi.cookie, pi.min + (pi.max - pi.min) * v[(size_t)i]});
    }
    return true;
}

} // namespace wl
