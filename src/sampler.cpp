#include "sampler.hpp"

#include "audio_file.hpp"
#include "dsp.hpp"
#include "platform.hpp"
#include "sf2.hpp"
#include "sfz.hpp"
#include "zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// `path` as it exists on disk, comparing each part without case when the exact spelling is missing (SFZ
// libraries made on Windows spell "Samples/KICK.wav" for "samples/Kick.wav"); "" when nothing matches
std::string existingIgnoringCase(const std::string &path) {
    std::error_code ec;
    const fs::path p(path);
    if (fs::exists(p, ec)) return path;
    fs::path cur = p.root_path();
    for (const auto &part : p.relative_path()) {
        fs::path next = cur / part;
        if (!fs::exists(next, ec)) {
            const std::string want = lower(part.string());
            bool found = false;
            for (auto &e : fs::directory_iterator(cur.empty() ? fs::path(".") : cur, ec))
                if (lower(e.path().filename().string()) == want) { next = e.path(); found = true; break; }
            if (!found) return "";
        }
        cur = next;
    }
    return cur.string();
}

bool readFile(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char *>(out.data()), (std::streamsize)out.size());
    return (bool)f;
}

using SampleData = DecodedAudio;

// ---- tiny XML reader for multisample.xml ----------------------------------------------------
std::string unescape(std::string v) {
    static const std::pair<const char *, const char *> ents[] = {{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (auto &[from, to] : ents)
        for (size_t p; (p = v.find(from)) != std::string::npos;) v.replace(p, strlen(from), to);
    return v;
}

struct Tag { std::string name; std::map<std::string, std::string> attrs; bool closing = false, selfClosing = false; };

std::vector<Tag> tags(const std::string &xml) {
    std::vector<Tag> out;
    for (size_t p = 0; (p = xml.find('<', p)) != std::string::npos;) {
        const size_t end = xml.find('>', p);
        if (end == std::string::npos) break;
        std::string body = xml.substr(p + 1, end - p - 1);
        p = end + 1;
        if (body.empty() || body[0] == '?' || body[0] == '!') continue;
        Tag t;
        if (body[0] == '/') { t.closing = true; body.erase(0, 1); }
        if (!body.empty() && body.back() == '/') { t.selfClosing = true; body.pop_back(); }
        size_t i = 0;
        while (i < body.size() && !isspace((unsigned char)body[i])) ++i;
        t.name = body.substr(0, i);
        while (i < body.size()) {
            while (i < body.size() && isspace((unsigned char)body[i])) ++i;
            const size_t eq = body.find('=', i);
            if (eq == std::string::npos) break;
            std::string key = body.substr(i, eq - i);
            while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
            size_t q = eq + 1;
            while (q < body.size() && isspace((unsigned char)body[q])) ++q;
            if (q >= body.size()) break;
            const char quote = body[q];
            const size_t close = body.find(quote, q + 1);
            if (close == std::string::npos) break;
            t.attrs[key] = unescape(body.substr(q + 1, close - q - 1));
            i = close + 1;
        }
        out.push_back(std::move(t));
    }
    return out;
}

double num(const std::map<std::string, std::string> &a, const char *k, double def) {
    auto it = a.find(k);
    if (it == a.end() || it->second.empty()) return def;
    if (it->second == "true") return 1;
    if (it->second == "false") return 0;
    return std::atof(it->second.c_str());
}

// ---- zones --------------------------------------------------------------------------------
struct Zone {
    std::string file;
    int keyLow = 0, keyHigh = 127, root = 60;
    double keyTrack = 1, tune = 0, gainDb = 0;
    int velLow = 0, velHigh = 127, velLowFade = 0, velHighFade = 0;
    int selLow = 0, selHigh = 127;
    double start = 0, stop = -1;             // frames; stop < 0 = end of file
    enum Loop { Off, Always, Sustain } loop = Off;
    double loopStart = 0, loopStop = 0, loopFade = 0;
    bool reverse = false, roundRobin = false;
    double pan = 0;                          // kit map entries only
    double startSec = 0;                     // extra start offset (sampler "start"), seconds
    double lengthSec = 0;                    // sampler "length": play at most this much from the start (0 = to the end)
    // SFZ regions
    bool loopFromFile = false;               // loop on the file's own loop points (WAV smpl) when it has them
    bool oneShot = false;                    // plays to the end whatever the note length
    double velTrack = 1;                     // amp_veltrack / 100
    double attack = -1, hold = 0, decay = 0, sustain = 1, release = -1;   // ampeg_*; < 0 = the sampler's
    int sw = -1;                             // keyswitch that selects this zone (sw_last), -1 = always
    int seq = 0;                             // round-robin order (seq_position, lorand)
    int group = 0, offBy = 0;                // choke groups
    int filter = 0;                          // 0 off, 1 low-pass, 2 high-pass, 3 band-pass (SFZ fil_type, SF2 initialFilterFc)
    double cutoff = 0, resonanceDb = 0;      // Hz at velocity 0 and the key centre, dB
    double filVelCents = 0, filKeyCents = 0; // cutoff shift at velocity 127, per key from filKeyCenter
    int filKeyCenter = 60;
    double delay = 0;                        // seconds before the zone starts (ampeg_delay, SF2 delayVolEnv)
    bool releaseTrigger = false;             // SFZ trigger=release: plays when the note ends
    double rtDecay = 0;                      // dB lower per second the note was held (release triggers)
    std::shared_ptr<const Sf2Zone> sf2;      // SoundFont zones: per-note modulators and the modulation envelope
};

bool parseMultisample(const std::string &xml, std::vector<Zone> &zones, std::string &err) {
    Zone *z = nullptr;
    for (const auto &t : tags(xml)) {
        if (t.name == "sample" && !t.closing) {
            zones.emplace_back();
            z = &zones.back();
            auto it = t.attrs.find("file");
            if (it == t.attrs.end()) { err = "multisample.xml: <sample> without file"; return false; }
            z->file = it->second;
            z->gainDb = num(t.attrs, "gain", 0);
            z->tune = num(t.attrs, "tune", 0);
            z->start = num(t.attrs, "sample-start", 0);
            z->stop = num(t.attrs, "sample-stop", -1);
            z->reverse = num(t.attrs, "reverse", 0) > 0;
            auto zl = t.attrs.find("zone-logic");
            z->roundRobin = zl != t.attrs.end() && zl->second == "round-robin";
            if (t.selfClosing) z = nullptr;
        } else if (t.name == "sample" && t.closing) {
            z = nullptr;
        } else if (z && t.name == "key") {
            z->root = (int)num(t.attrs, "root", 60);
            z->keyLow = (int)num(t.attrs, "low", 0);
            z->keyHigh = (int)num(t.attrs, "high", 127);
            z->keyTrack = num(t.attrs, "track", 1);
            z->tune += num(t.attrs, "tune", 0);
        } else if (z && t.name == "velocity") {
            z->velLow = (int)num(t.attrs, "low", 0);
            z->velHigh = (int)num(t.attrs, "high", 127);
            z->velLowFade = (int)num(t.attrs, "low-fade", 0);
            z->velHighFade = (int)num(t.attrs, "high-fade", 0);
        } else if (z && t.name == "select") {
            z->selLow = (int)num(t.attrs, "low", 0);
            z->selHigh = (int)num(t.attrs, "high", 127);
        } else if (z && t.name == "loop") {
            auto m = t.attrs.find("mode");
            const std::string mode = m == t.attrs.end() ? "off" : m->second;
            z->loop = mode == "loop" ? Zone::Always : mode == "sustain" ? Zone::Sustain : Zone::Off;
            z->loopStart = num(t.attrs, "start", 0);
            z->loopStop = num(t.attrs, "stop", 0);
            z->loopFade = std::clamp(num(t.attrs, "fade", 0), 0.0, 1.0);
        }
    }
    if (zones.empty()) { err = "multisample.xml has no samples"; return false; }
    return true;
}

// ---- SFZ ----------------------------------------------------------------------------------
// regions -> zones. Opcodes the sampler can't play are named once in a warning.
bool sfzZones(const SfzFile &sfz, std::vector<Zone> &zones, int &swLow, int &swHigh, int &swDefault, bool &notePolyOne,
              std::vector<std::string> &warnings, std::string &err) {
    static const std::set<std::string> handled = {
        "sample", "lokey", "hikey", "pitch_keycenter", "pitch_keytrack", "lovel", "hivel", "tune", "transpose",
        "volume", "amplitude", "pan", "offset", "end", "loop_mode", "loop_start", "loop_end", "trigger",
        "xfin_lovel", "xfin_hivel", "xfout_lovel", "xfout_hivel", "amp_veltrack", "ampeg_attack", "ampeg_hold",
        "ampeg_decay", "ampeg_sustain", "ampeg_release", "seq_length", "seq_position", "lorand", "hirand",
        "sw_lokey", "sw_hikey", "sw_last", "sw_default", "group", "off_by", "direction", "note_polyphony",
        "group_volume", "master_volume", "global_volume", "cutoff", "resonance", "fil_type", "fil_veltrack", "fil_keytrack",
        "fil_keycenter", "rt_decay", "ampeg_delay", "delay",
        // no effect on the sound
        "lochan", "hichan", "off_mode", "sw_label", "region_label", "group_label", "master_label", "global_label",
        "polyphony", "note_selfmask", "sw_vel", "xf_velcurve"};
    std::map<std::string, int> ignored;
    int ccRegions = 0;
    bool ccMod = false;
    swLow = 128; swHigh = -1; swDefault = -1;
    notePolyOne = false;
    auto ccValue = [&](int n) { auto it = sfz.cc.find(n); return it == sfz.cc.end() ? 0.0 : it->second; };
    for (const auto &r : sfz.regions) {
        // regions for other controller states (sustain pedal down, a mic position off) or played by a controller
        bool ccOut = false;
        for (auto &[k, v] : r.op) {
            if (k.rfind("locc", 0) == 0 && ccValue(std::atoi(k.c_str() + 4)) < std::atof(v.c_str())) ccOut = true;
            if (k.rfind("hicc", 0) == 0 && ccValue(std::atoi(k.c_str() + 4)) > std::atof(v.c_str())) ccOut = true;
            if (k.rfind("on_locc", 0) == 0 || k.rfind("on_hicc", 0) == 0 || k.rfind("start_locc", 0) == 0 || k.rfind("start_hicc", 0) == 0) ccOut = true;
        }
        if (ccOut) { ++ccRegions; continue; }
        for (auto &[k, v] : r.op) {
            if (handled.count(k) || k.rfind("amp_velcurve_", 0) == 0 || k.find("label") != std::string::npos ||
                k.rfind("locc", 0) == 0 || k.rfind("hicc", 0) == 0 || k.rfind("set_", 0) == 0) continue;
            const size_t cc = k.find("cc");   // amplitude_oncc7, ampeg_releasecc64, pan_curvecc10: controller modulation
            if (cc != std::string::npos && cc + 2 < k.size() && std::isdigit((unsigned char)k[cc + 2])) ccMod = true;
            else ++ignored[k];
        }
        if (r.has("note_polyphony") && r.num("note_polyphony", 0) <= 1) notePolyOne = true;
        const std::string trig = r.get("trigger", "attack");
        if (r.num("end", 0) < 0) continue;   // end=-1: a disabled region
        std::string file = r.get("sample");
        if (file.empty()) continue;
        std::replace(file.begin(), file.end(), '\\', '/');
        Zone z;
        z.file = file[0] == '*' ? file : (fs::path(sfz.dir) / file).lexically_normal().string();
        const int off = sfz.noteOffset;
        z.keyLow = std::clamp(r.key("lokey", 0) + off, 0, 127);
        z.keyHigh = std::clamp(r.key("hikey", 127) + off, 0, 127);
        z.root = r.key("pitch_keycenter", 60) + off;
        if (file[0] == '*') z.file = lower(file) + "#" + std::to_string(z.root);   // a table per root key
        z.keyTrack = r.num("pitch_keytrack", 100) / 100;
        z.tune = r.num("tune", 0) / 100 + r.num("transpose", 0);
        z.gainDb = r.num("volume", 0) + r.num("group_volume", 0) + r.num("master_volume", 0) + r.num("global_volume", 0) +
                   (r.has("amplitude") ? 20 * std::log10(std::max(1e-4, r.num("amplitude", 100) / 100)) : 0);
        z.pan = std::clamp(r.num("pan", 0) / 100, -1.0, 1.0);
        z.velLow = (int)r.num("lovel", 0); z.velHigh = (int)r.num("hivel", 127);
        if (r.has("xfin_hivel")) {   // crossfade in: silent at xfin_lovel, full at xfin_hivel
            z.velLow = std::max(z.velLow, (int)r.num("xfin_lovel", 0));
            z.velLowFade = std::max(0, (int)r.num("xfin_hivel", 0) - z.velLow);
        }
        if (r.has("xfout_lovel")) {
            z.velHigh = std::min(z.velHigh, (int)r.num("xfout_hivel", 127));
            z.velHighFade = std::max(0, z.velHigh - (int)r.num("xfout_lovel", 127));
        }
        z.start = r.num("offset", 0);
        if (r.has("end")) z.stop = r.num("end", 0) + 1;
        z.reverse = r.get("direction") == "reverse";
        const std::string mode = r.get("loop_mode");
        const bool hasPoints = r.has("loop_end");
        if (mode == "one_shot") z.oneShot = true;
        if (file[0] == '*') { z.loop = Zone::Always; z.loopFromFile = true; }   // a generator is one cycle: it always loops
        else if (mode == "loop_continuous" || mode == "loop_sustain" || (mode.empty() && hasPoints)) {
            z.loop = mode == "loop_sustain" ? Zone::Sustain : Zone::Always;
            if (hasPoints) { z.loopStart = r.num("loop_start", 0); z.loopStop = r.num("loop_end", 0) + 1; }
            else z.loopFromFile = true;
        } else if (mode.empty()) {   // SFZ: loops that the file defines play unless told otherwise
            z.loop = Zone::Always;
            z.loopFromFile = true;
        }
        z.velTrack = r.num("amp_veltrack", 100) / 100;
        if (r.has("ampeg_attack")) z.attack = r.num("ampeg_attack", 0);
        if (r.has("ampeg_release")) z.release = r.num("ampeg_release", 0);
        z.hold = r.num("ampeg_hold", 0);
        z.decay = r.num("ampeg_decay", 0);
        z.sustain = std::clamp(r.num("ampeg_sustain", 100) / 100, 0.0, 1.0);
        if (r.num("seq_length", 1) > 1) { z.roundRobin = true; z.seq = (int)r.num("seq_position", 1); }
        else if (r.has("lorand") || r.has("hirand")) { z.roundRobin = true; z.seq = (int)std::lround(r.num("lorand", 0) * 1000); }
        if (r.has("sw_last")) z.sw = r.key("sw_last", -1) + off;
        if (r.has("sw_lokey")) { swLow = std::min(swLow, r.key("sw_lokey", 0) + off); swHigh = std::max(swHigh, r.key("sw_hikey", 127) + off); }
        if (r.has("sw_default")) swDefault = r.key("sw_default", -1) + off;
        z.group = (int)r.num("group", 0);
        z.offBy = (int)r.num("off_by", 0);
        if (r.has("cutoff")) {
            const std::string ft = r.get("fil_type", "lpf_2p");
            z.filter = ft.rfind("hpf", 0) == 0 ? 2 : ft.rfind("bpf", 0) == 0 ? 3 : ft.rfind("lpf", 0) == 0 ? 1 : 0;
            if (!z.filter) ++ignored["fil_type=" + ft];
            z.cutoff = r.num("cutoff", 0);
            z.resonanceDb = r.num("resonance", 0);
            z.filVelCents = r.num("fil_veltrack", 0);
            z.filKeyCents = r.num("fil_keytrack", 0);
            z.filKeyCenter = r.key("fil_keycenter", 60);
        }
        z.delay = r.num("delay", 0) + r.num("ampeg_delay", 0);
        z.releaseTrigger = trig == "release" || trig == "release_key";
        z.rtDecay = r.num("rt_decay", 0);
        zones.push_back(z);
    }
    if (swHigh < 0) {   // sw_last without a declared range: the keyswitches are the sw_last keys
        for (auto &z : zones) if (z.sw >= 0) { swLow = std::min(swLow, z.sw); swHigh = std::max(swHigh, z.sw); }
    }
    std::stable_sort(zones.begin(), zones.end(), [](const Zone &a, const Zone &b) { return a.seq < b.seq; });
    if (ccRegions) warnings.push_back("sfz: " + std::to_string(ccRegions) + " region(s) for other controller states left out (e.g. pedal down)");
    if (!ignored.empty() || ccMod) {
        std::string names;
        size_t n = 0;
        for (auto &[k, c] : ignored) if (n++ < 12) names += (names.empty() ? "" : ", ") + k;
        if (ignored.size() > 12) names += ", ...";
        if (ccMod) names += std::string(names.empty() ? "" : "; ") + "controller modulation (*cc* opcodes)";
        warnings.push_back("sfz: opcodes the sampler doesn't play: " + names);
    }
    if (zones.empty()) { err = "the SFZ has no playable regions"; return false; }
    return true;
}

// SoundFont zones -> sampler zones ("sf2#<sample index>" files)
void sf2Zones(const std::vector<Sf2Zone> &in, std::vector<Zone> &zones) {
    for (const auto &q : in) {
        Zone z;
        z.file = "sf2#" + std::to_string(q.sample);
        z.keyLow = q.keyLow; z.keyHigh = q.keyHigh; z.velLow = q.velLow; z.velHigh = q.velHigh;
        z.root = q.root; z.tune = q.tune; z.keyTrack = q.keyTrack; z.pan = q.pan;   // level: per voice (attenuation + modulators)
        z.start = q.start; z.stop = q.stop;
        if (q.loopMode) {
            z.loop = q.loopMode == 3 ? Zone::Sustain : Zone::Always;
            z.loopStart = q.loopStart; z.loopStop = q.loopStop;
        }
        z.velTrack = 0;   // velocity reaches the level through the zone's modulators (per voice)
        z.delay = q.delay; z.attack = q.attack; z.hold = q.hold; z.decay = q.decay; z.sustain = q.sustain; z.release = q.release;
        z.filter = 1;     // opened or bypassed per voice
        z.sf2 = std::make_shared<Sf2Zone>(q);
        z.group = z.offBy = q.exclusiveClass;   // a hi-hat's closed and open zones cut each other
        zones.push_back(z);
    }
}

// *sine, *saw, *square, *triangle, *noise, *silence: SFZ's built-in sources, as a looped table
bool generatorSample(const std::string &spec, SampleData &d, std::string &err) {   // "*sine#60"
    const size_t hash = spec.find('#');
    const std::string name = spec.substr(0, hash), g = lower(name.substr(1));
    const int root = hash == std::string::npos ? 60 : std::atoi(spec.c_str() + hash + 1);
    const size_t n = 2048;
    d.l.assign(g == "noise" ? 96000 : n, 0.f);
    d.r.clear();
    d.rate = g == "noise" ? 48000 : n * 440.0 * std::pow(2.0, (root - 69) / 12.0);   // one cycle at the root key
    uint32_t rng = 12345;
    for (size_t i = 0; i < d.l.size(); ++i) {
        const double ph = (double)i / n;
        float v = 0;
        if (g == "sine") v = (float)std::sin(2 * M_PI * ph);
        else if (g == "saw") v = (float)(2 * ph - 1);
        else if (g == "square") v = ph < 0.5 ? 1.f : -1.f;
        else if (g == "triangle" || g == "tri") v = (float)(ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph);
        else if (g == "noise") { rng = rng * 1664525u + 1013904223u; v = (float)((rng >> 8) / 8388608.0 - 1.0); }
        else if (g != "silence") { err = "unknown SFZ generator '" + name + "'"; return false; }
        d.l[i] = v * 0.5f;
    }
    d.loopStart = 0;
    d.loopEnd = (double)d.l.size();
    return true;
}

// ---- library index ------------------------------------------------------------------------
std::string home() { return platform::homeDir().string(); }

int gmKeyFor(const std::string &file, std::set<int> &taken, int *primary = nullptr) {
    const std::string n = lower(fs::path(file).stem().string());
    std::vector<std::string> tok;
    std::string cur;
    for (char c : n) { if (isalnum((unsigned char)c)) cur += c; else { if (!cur.empty()) tok.push_back(cur); cur.clear(); } }
    if (!cur.empty()) tok.push_back(cur);
    // loops and phrases are not drum hits: "133bpm", "loop", "beat", "fill"
    for (auto &t : tok) {
        const size_t d = t.find_first_not_of("0123456789");
        if (d != std::string::npos && d > 1 && t.substr(d) == "bpm") return -1;
        if (t == "bpm" || t == "loop" || t == "loops" || t == "fill" || t == "groove") return -1;
    }
    auto has = [&](std::initializer_list<const char *> words) {
        for (const char *w : words) {
            for (size_t i = 0; i < tok.size(); ++i) {
                const bool match = strlen(w) <= 3 ? tok[i] == w : tok[i].find(w) != std::string::npos;
                if (match && !(i > 0 && (tok[i - 1] == "no" || tok[i - 1] == "without"))) return true;   // "No Snare"
            }
            if (strlen(w) > 3 && strchr(w, ' ') && n.find(w) != std::string::npos) return true;
        }
        return false;
    };
    auto pick = [&](std::initializer_list<int> keys) {
        if (primary) *primary = *keys.begin();   // the role's main key, for round-robin variants
        for (int k : keys) if (!taken.count(k)) { taken.insert(k); return k; }
        return -1;
    };
    const bool hat = has({"hat", "hh", "hihat", "chh", "ohh", "oh", "ch"});
    if (hat && has({"open", "ohh", "oh"})) return pick({46});
    if (hat && has({"pedal", "foot"})) return pick({44});
    if (hat) return pick({42, 44});
    if (has({"crash"})) return pick({49, 57});
    if (has({"ride"})) return has({"bell"}) ? pick({53}) : pick({51, 59});
    if (has({"china"})) return pick({52});
    if (has({"splash"})) return pick({55});
    if (has({"cymbal", "cym"})) return pick({49, 57, 55});
    if (has({"kick", "bd", "bassdrum", "bass drum", "kck"})) return pick({36, 35});
    if (has({"rim", "rimshot", "sidestick", "side stick", "rs"})) return pick({37});
    if (has({"clap", "cp", "clp"})) return pick({39});
    if (has({"snare", "sd", "snr"})) return pick({38, 40});
    if (has({"tom"})) {
        if (has({"floor", "low", "lo", "lt"})) return pick({45, 41, 43});
        if (has({"high", "hi", "ht"})) return pick({50, 48});
        for (size_t i = 0; i + 1 < tok.size(); ++i)   // "Tom 1" is the smallest drum, "Tom 3" the floor tom
            if (tok[i].find("tom") != std::string::npos && tok[i + 1].size() == 1 && isdigit((unsigned char)tok[i + 1][0])) {
                switch (tok[i + 1][0]) {
                case '1': return pick({50, 48});
                case '2': return pick({47, 48});
                case '3': return pick({45, 43});
                default: return pick({43, 41});
                }
            }
        return pick({47, 48, 45, 50});
    }
    if (has({"cowbell", "cow"})) return pick({56});
    if (has({"tambourine", "tamb", "tambo"})) return pick({54});
    if (has({"shaker", "maraca", "maracas"})) return pick({70, 82});
    if (has({"conga"})) return has({"high", "hi"}) ? pick({62, 63}) : pick({63, 64, 62});
    if (has({"bongo"})) return has({"high", "hi"}) ? pick({60, 61}) : pick({61, 60});
    if (has({"clave", "claves"})) return pick({75});
    if (has({"block", "wood"})) return pick({76, 77});
    if (has({"agogo"})) return pick({67, 68});
    if (has({"cabasa"})) return pick({69});
    if (has({"timbale"})) return pick({65, 66});
    if (has({"guiro"})) return pick({73, 74});
    if (has({"whistle"})) return pick({71, 72});
    if (has({"triangle"})) return pick({81, 80});
    return -1;
}

bool looksLikeLoop(const std::string &file) {
    const std::string n = lower(fs::path(file).stem().string());
    if (n.find("loop") != std::string::npos) return true;
    for (size_t p = n.find("bpm"); p != std::string::npos; p = n.find("bpm", p + 1))
        if (p > 0 && isdigit((unsigned char)n[p - 1])) return true;
    return false;
}

bool isWav(const fs::path &p) { return isAudioFileName(p.string()); }

std::vector<std::string> wavsIn(const std::string &dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && isWav(e.path())) out.push_back(e.path().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool looksLikeKit(const std::vector<std::string> &wavs) {
    if (wavs.size() < 4 || wavs.size() > 400) return false;
    std::set<int> taken;
    int roles = 0;
    for (auto &w : wavs) if (gmKeyFor(w, taken) >= 0) ++roles;
    return taken.count(36) || taken.count(35) ? roles >= 3 : false;
}

std::string multisampleCategory(const std::string &xml) {
    const auto a = xml.find("<category>"), b = xml.find("</category>");
    return a != std::string::npos && b > a ? xml.substr(a + 10, b - a - 10) : "";
}

std::string resolveIn(const std::string &name, const std::string &baseDir) {
    fs::path p(name);
    if (p.is_absolute()) return fs::exists(p) ? p.string() : "";
    if (!baseDir.empty() && fs::exists(fs::path(baseDir) / p)) return (fs::path(baseDir) / p).string();
    for (auto &root : sampleRoots()) if (fs::exists(fs::path(root) / p)) return (fs::path(root) / p).string();
    return "";
}

// find a library entry by exact name, then by path suffix, then by unique substring
bool findEntry(const std::string &kind, const std::string &query, std::string &path, std::string &err) {
    const auto &lib = sampleLibrary();
    const std::string q = lower(query);
    std::vector<const SampleLibraryEntry *> exact;
    for (auto &e : lib) if (e.kind == kind && lower(e.name) == q) exact.push_back(&e);
    if (exact.size() == 1) { path = exact[0]->path; return true; }
    if (exact.size() > 1) {
        err = "'" + query + "' names " + std::to_string(exact.size()) + " " + kind + "s; add the folder above it:";
        for (auto *e : exact) err += " \"" + fs::path(e->path).parent_path().filename().string() + "/" + e->name + "\"";
        return false;
    }
    for (auto &e : lib) {
        const std::string lp = lower(e.path);
        if (e.kind == kind && lp.size() >= q.size() && lp.compare(lp.size() - q.size(), q.size(), q) == 0) { path = e.path; return true; }
    }
    std::vector<const SampleLibraryEntry *> hits;
    std::set<std::string> names;
    for (auto &e : lib) if (e.kind == kind && lower(e.name).find(q) != std::string::npos && names.insert(e.name).second) hits.push_back(&e);
    if (hits.size() == 1) { path = hits[0]->path; return true; }
    err = hits.empty() ? "no " + kind + " named '" + query + "'" : "'" + query + "' matches " + std::to_string(hits.size()) + " " + kind + "s";
    if (!hits.empty()) {
        err += ": ";
        for (size_t i = 0; i < hits.size() && i < 6; ++i) err += (i ? ", " : "") + hits[i]->name;
        if (hits.size() > 6) err += ", ...";
    }
    err += " (run `wavelength samples --search <text>`)";
    return false;
}

// ---- playback -----------------------------------------------------------------------------
struct Voice {
    const Zone *zone;
    std::shared_ptr<SampleData> data;
    size_t startFrame;
    double noteLen;     // seconds until note-off (inf for one-shots)
    double cutAt;       // seconds until a choke or the next mono note cuts the voice (inf = never)
    double amp;
    std::function<double(double)> key;   // sounding key (fractional, incl. glide and bend) at t seconds
    double semisOffset = 0;              // zone tune + transpose
    double cutoffHz = 0;                 // the zone's filter for this note (0 = off)
    double resonanceDb = 0;
    double modEnvCents = 0;              // SoundFont: how far the modulation envelope opens the filter
    // SoundFont per-note modulators on envelope times (timecents: x 2^(tc/1200)), pan and level
    double tcAttack = 0, tcHold = 0, tcDecay = 0, tcRelease = 0, tcModAttack = 0, tcModHold = 0, tcModDecay = 0, tcModRelease = 0;
    double panOffset = 0;
};

inline float cubic(const std::vector<float> &x, double pos) {
    const long i = (long)pos;
    const float f = (float)(pos - i);
    const long n = (long)x.size();
    auto at = [&](long k) { return x[(size_t)std::clamp(k, 0L, n - 1)]; };
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void play(const Voice &v, Audio &out, double sr, double attack, double release) {
    const Zone &z = *v.zone;
    const SampleData &s = *v.data;
    // the region played, in the file's own time: [start, start + length), reversed as a whole with "reverse"
    const double fileStop = z.stop > 0 ? std::min<double>(z.stop, (double)s.frames()) : (double)s.frames();
    const double from = std::min(fileStop, z.start + z.startSec * s.rate);
    const double stop = z.lengthSec > 0 ? std::min(fileStop, from + z.lengthSec * s.rate) : fileStop;
    double loopStart = z.loopStart, loopStop = z.loopStop;
    if (z.loopFromFile && s.loopEnd > s.loopStart && s.loopStart >= 0) { loopStart = s.loopStart; loopStop = s.loopEnd; }
    const double loopLen = loopStop - loopStart;
    const bool canLoop = z.loop != Zone::Off && loopLen > 16 && loopStop <= stop && !z.reverse;
    const double fadeLen = canLoop ? std::min(z.loopFade * loopLen, loopStart) : 0;
    dsp::Biquad fl, fr;
    const bool filtered = z.filter && v.cutoffHz > 0;
    const auto ftype = z.filter == 2 ? dsp::Biquad::HighPass : z.filter == 3 ? dsp::Biquad::BandPass : dsp::Biquad::LowPass;
    const double fq = 0.7071 * std::pow(10.0, v.resonanceDb / 20);
    // a SoundFont filter lowers the level by half its resonance (SoundFont 2.01, as FluidSynth does)
    const double filterGain = filtered && z.sf2 ? 1 / std::sqrt(fq / 0.7071) : 1.0;
    const double pan = std::clamp(z.pan + v.panOffset, -1.0, 1.0);
    // SoundFont zones pan at constant power, unity in the centre (a hard-panned stereo pair gets +3 dB a side, as in FluidSynth)
    const double panL = std::sqrt(2.0) * std::cos((pan + 1) * M_PI / 4), panR = std::sqrt(2.0) * std::sin((pan + 1) * M_PI / 4);
    auto tc = [](double seconds, double cents) { return cents ? seconds * std::pow(2.0, cents / 1200) : seconds; };
    if (filtered) {
        fl.set(ftype, v.cutoffHz, fq, 0, sr);
        fr = fl;
    }
    if (z.attack >= 0) attack = z.attack;
    if (z.release >= 0) release = z.release;
    double hold = z.hold, decay = z.decay, modHold = 0, modDecay = 0;
    double modAttack = 0, modRelease = 0;
    if (z.sf2) {   // SoundFont keynumTo* generators (hold and decay depend on the key) and per-note time modulators
        const int key = (int)std::lround(v.key(0));
        hold = tc(Sf2Zone::keyScaled(z.hold, z.sf2->keyToHold, key), v.tcHold);
        decay = tc(Sf2Zone::keyScaled(z.decay, z.sf2->keyToDecay, key), v.tcDecay);
        modHold = tc(Sf2Zone::keyScaled(z.sf2->modHold, z.sf2->keyToModHold, key), v.tcModHold);
        modDecay = tc(Sf2Zone::keyScaled(z.sf2->modDecay, z.sf2->keyToModDecay, key), v.tcModDecay);
        modAttack = tc(z.sf2->modAttack, v.tcModAttack);
        modRelease = tc(z.sf2->modRelease, v.tcModRelease);
        attack = tc(attack, v.tcAttack);
        release = tc(release, v.tcRelease);
    }
    // SoundFont modulation envelope (delay, attack, hold, linear decay to sustain, release) moving the cutoff
    const Sf2Zone *me = v.modEnvCents != 0 && z.sf2 ? z.sf2.get() : nullptr;
    double modLevel = 0, modAtOff = -1;
    auto modEnv = [&](double t) {
        const double a0 = me->modDelay, a1 = a0 + modAttack, h1 = a1 + modHold, d1 = h1 + modDecay;
        double m;
        if (t < a0) m = 0;
        else if (t < a1) m = (t - a0) / std::max(1e-6, modAttack);
        else if (t < h1) m = 1;
        else if (t < d1) m = 1 - (1 - me->modSustain) * (t - h1) / std::max(1e-6, modDecay);
        else m = me->modSustain;
        return m;
    };
    const double decayFrom = attack + hold;
    const double susDb = z.sustain > 0 ? std::max(-96.0, 20 * std::log10(z.sustain)) : -96.0;
    const bool stereo = !s.r.empty();
    double pos = from;
    const double chokeFade = 0.004;
    const double rateRatio = s.rate / sr;
    double ratio = 1;
    for (size_t i = 0;; ++i) {
        if (i % 16 == 0) ratio = std::pow(2.0, (z.keyTrack * (v.key(i / sr) - z.root) + v.semisOffset) / 12) * rateRatio;
        const size_t idx = v.startFrame + i;
        if (idx >= out.frames()) break;
        const double t = i / sr;
        double env = attack > 0 ? std::min(1.0, t / attack) : 1.0;
        if (z.sf2) {   // SoundFont volume envelope: decay and release fall linearly in dB, 96 dB over their times
            const double td = std::min(t, v.noteLen);   // decay stops at note-off, release starts from there
            double db = 0;
            if (td > decayFrom) db = decay > 0 ? std::max(susDb, -96 * (td - decayFrom) / decay) : susDb;
            if (t > v.noteLen) db -= release > 0 ? 96 * (t - v.noteLen) / release : 96;
            if (db <= -96) break;
            env *= std::pow(10.0, db / 20);
        } else {
            if (z.sustain < 1 && t > decayFrom)   // ampeg_decay: falls toward ampeg_sustain (-60 dB over the decay time)
                env *= decay > 0 ? z.sustain + (1 - z.sustain) * std::exp(-6.9 * (t - decayFrom) / decay) : z.sustain;
            if (env < 1e-4 && z.sustain <= 0 && t > decayFrom) break;
            if (t > v.noteLen) {
                if (release <= 0) break;
                const double r = 1.0 - (t - v.noteLen) / release;
                if (r <= 0) break;
                env *= r * r;
            }
        }
        if (t > v.cutAt) {
            const double c = 1.0 - (t - v.cutAt) / chokeFade;
            if (c <= 0) break;
            env *= c;
        }
        const bool looping = canLoop && (z.loop == Zone::Always || t <= v.noteLen);
        if (looping) while (pos >= loopStop) pos -= loopLen;
        if (pos >= stop) break;
        const double rp = z.reverse ? stop - 1 - (pos - from) : pos;
        if (rp < 0) break;
        float l = cubic(s.l, rp), r = stereo ? cubic(s.r, rp) : l;
        if (looping && fadeLen > 0 && pos >= loopStop - fadeLen) {
            const float w = (float)((pos - (loopStop - fadeLen)) / fadeLen);
            const double alt = pos - loopLen;
            l = l * (1 - w) + cubic(s.l, alt) * w;
            r = r * (1 - w) + (stereo ? cubic(s.r, alt) : cubic(s.l, alt)) * w;
        }
        if (filtered) {
            if (me && i % 16 == 0) {
                if (t <= v.noteLen) modLevel = modEnv(t);
                else {
                    if (modAtOff < 0) modAtOff = modEnv(v.noteLen);
                    modLevel = modRelease > 0 ? std::max(0.0, modAtOff * (1 - (t - v.noteLen) / modRelease)) : 0;
                }
                const double hz = v.cutoffHz * std::pow(2.0, v.modEnvCents * modLevel / 1200);
                fl.set(ftype, hz, fq, 0, sr);
                fr.b0 = fl.b0; fr.b1 = fl.b1; fr.b2 = fl.b2; fr.a1 = fl.a1; fr.a2 = fl.a2;
            }
            l = (float)fl.process(l); r = (float)fr.process(r);
        }
        const double g = v.amp * env * filterGain;
        const double pl = z.sf2 ? panL : pan > 0 ? 1 - pan : 1, pr = z.sf2 ? panR : pan < 0 ? 1 + pan : 1;
        out.left[idx] += (float)(l * g * pl);
        out.right[idx] += (float)(r * g * pr);
        pos += ratio;
    }
}

} // namespace

std::string resolveSampleFile(const std::string &name, const std::string &baseDir) { return resolveIn(name, baseDir); }

bool findSampleEntry(const std::string &kind, const std::string &query, const std::string &baseDir, std::string &path, std::string &err) {
    path = resolveIn(query, baseDir);
    return !path.empty() || findEntry(kind, query, path, err);
}

std::string soundFontDir() { return (platform::dataDir() / "soundfonts").string(); }

std::string defaultSoundFont() {
    if (const char *env = std::getenv("WAVELENGTH_SOUNDFONT")) { std::error_code ec; if (fs::exists(env, ec)) return env; }
    const SampleLibraryEntry *best = nullptr;
    for (auto &e : sampleLibrary())
        if (e.kind == "soundfont" && e.category == "General MIDI" && (!best || e.count > best->count)) best = &e;
    return best ? best->path : "";
}

std::vector<std::string> sampleRoots() {
    std::vector<std::string> roots = platform::envPathList("WAVELENGTH_SAMPLES_PATH");
    std::error_code sec;
    if (fs::is_directory(soundFontDir(), sec)) roots.push_back(soundFontDir());   // `samples --install-soundfont` puts them here
    // Bitwig Studio's installed sound content, newest package format first
#if defined(__APPLE__)
    const fs::path bitwig = fs::path(home()) / "Library/Application Support/Bitwig/Bitwig Studio/installed-packages";
#elif defined(_WIN32)
    const fs::path bitwig = fs::path(getenv("LOCALAPPDATA") ? getenv("LOCALAPPDATA") : "") / "Bitwig Studio/installed-packages";
#else
    const fs::path bitwig = fs::path(home()) / ".BitwigStudio/installed-packages";
#endif
    std::error_code ec;
    std::vector<std::string> versions;
    for (auto &e : fs::directory_iterator(bitwig, ec)) if (e.is_directory(ec)) versions.push_back(e.path().string());
    std::sort(versions.rbegin(), versions.rend());
    roots.insert(roots.end(), versions.begin(), versions.end());
    const fs::path userLib = fs::path(home()) / "Documents/Bitwig Studio/Library";
    if (fs::exists(userLib, ec)) roots.push_back(userLib.string());
    return roots;
}

const std::vector<SampleLibraryEntry> &sampleLibrary() {
    static std::vector<SampleLibraryEntry> lib;
    static std::once_flag once;
    std::call_once(once, [] {
        std::set<std::string> seen;   // the same package appears under several format versions
        for (auto &root : sampleRoots()) {
            std::error_code ec;
            std::map<std::string, std::vector<std::string>> dirWavs;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const auto &p = it->path();
                const std::string ext = lower(p.extension().string());
                if (ext == ".multisample") {
                    const std::string name = p.stem().string();
                    if (!seen.insert("m:" + name).second) continue;
                    SampleLibraryEntry e{"multisample", name, p.string(), "", 0};
                    Zip z;
                    std::string err;
                    std::vector<uint8_t> x;
                    if (z.open(p.string(), err) && z.read("multisample.xml", x, err)) {
                        const std::string xml(x.begin(), x.end());
                        e.category = multisampleCategory(xml);
                        for (size_t q = 0; (q = xml.find("<sample ", q)) != std::string::npos; ++q) ++e.count;
                    }
                    lib.push_back(e);
                } else if (ext == ".sf2" || ext == ".sf3") {
                    SoundFont sf;
                    std::string e2;
                    if (!sf.open(p.string(), e2)) continue;
                    const bool gm = std::count_if(sf.presets().begin(), sf.presets().end(), [](const Sf2Preset &q) { return q.bank == 0; }) >= 128;
                    lib.push_back({"soundfont", p.stem().string(), p.string(), gm ? "General MIDI" : "", sf.presets().size()});
                } else if (ext == ".sfz") {
                    std::vector<uint8_t> x;
                    if (!readFile(p.string(), x)) continue;
                    const std::string text(x.begin(), x.end());
                    SampleLibraryEntry e{"sfz", p.stem().string(), p.string(), p.parent_path().filename().string(), 0};
                    for (size_t q = 0; (q = text.find("<region>", q)) != std::string::npos; ++q) ++e.count;
                    lib.push_back(e);
                } else if (isAudioFileName(p.string())) {
                    dirWavs[p.parent_path().string()].push_back(p.string());
                }
            }
            for (auto &[dir, wavs] : dirWavs) {
                std::sort(wavs.begin(), wavs.end());
                size_t loops = 0;
                for (auto &w : wavs) loops += looksLikeLoop(w);
                const bool kit = looksLikeKit(wavs), loopDir = wavs.size() >= 2 && loops * 2 >= wavs.size();
                if (!kit && !loopDir) continue;
                const fs::path d(dir);
                const std::string name = d.filename().string();
                if (!seen.insert("k:" + d.parent_path().filename().string() + "/" + name).second) continue;
                lib.push_back({kit ? "kit" : "loops", name, dir, d.parent_path().filename().string(), wavs.size()});
            }
        }
        std::sort(lib.begin(), lib.end(), [](auto &a, auto &b) { return a.kind != b.kind ? a.kind < b.kind : lower(a.name) < lower(b.name); });
    });
    return lib;
}

// "Snare 01.wav" and "Snare 02.wav" are takes of one sound: the name without its trailing number
std::string takeGroup(const std::string &file) {
    std::string n = fs::path(file).stem().string();
    while (!n.empty() && (isdigit((unsigned char)n.back()) || n.back() == ' ' || n.back() == '_' || n.back() == '-')) n.pop_back();
    return lower(n);
}

bool kitMap(const std::string &nameOrPath, const std::string &baseDir, std::vector<std::pair<int, std::string>> &map,
            std::vector<std::string> &unmapped, std::string &resolved, std::string &err, bool roundRobin,
            std::vector<std::string> *extraTakes) {
    resolved = resolveIn(nameOrPath, baseDir);
    if (resolved.empty() || !fs::is_directory(resolved)) {
        std::string err2;
        if (!findEntry("kit", nameOrPath, resolved, err) && !findEntry("loops", nameOrPath, resolved, err2)) return false;
        err.clear();
    }
    std::set<int> taken;
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;   // in file order
    for (auto &w : wavsIn(resolved)) {
        const std::string g = takeGroup(w);
        auto it = std::find_if(groups.begin(), groups.end(), [&](auto &x) { return x.first == g; });
        if (it == groups.end()) groups.push_back({g, {w}}); else it->second.push_back(w);
    }
    std::vector<std::string> extras;
    for (auto &[g, files] : groups) {
        int primary = -1;
        const int k = gmKeyFor(files.front(), taken, &primary);
        if (k < 0) {   // a known drum whose keys are all used is another take, not an unknown sound
            (primary >= 0 ? extras : unmapped).insert((primary >= 0 ? extras : unmapped).end(), files.begin(), files.end());
            continue;
        }
        map.push_back({k, files.front()});
        for (size_t i = 1; i < files.size(); ++i) {
            if (roundRobin) { map.push_back({k, files[i]}); continue; }   // takes cycle on the same key
            const int alt = gmKeyFor(files[i], taken);                     // else the drum's alternate key (35, 40, 57, ...)
            if (alt >= 0) map.push_back({alt, files[i]}); else extras.push_back(files[i]);
        }
    }
    unmapped.insert(unmapped.end(), extras.begin(), extras.end());
    if (extraTakes) *extraTakes = extras;
    // anything unrecognised goes on the free keys from 60 up
    int next = 60;
    for (auto &w : unmapped) {
        while (taken.count(next) && next < 127) ++next;
        taken.insert(next);
        map.push_back({next, w});
    }
    // round robin stacks every take on the drum's main key, which leaves the General MIDI alternate
    // keys empty: a GM part (43 floor tom, 57 crash 2) would go silent. Alias them to their drum.
    if (roundRobin) {
        static const std::vector<std::pair<int, std::vector<int>>> aliases = {
            {35, {36}}, {40, {38}}, {41, {43, 45}}, {43, {41, 45}}, {48, {47, 50}}, {57, {49}}, {59, {51}}, {52, {49}}, {55, {49}}};
        std::set<int> have;
        for (auto &m : map) have.insert(m.first);
        const auto original = map;
        for (auto &[alt, mains] : aliases) {
            if (have.count(alt)) continue;
            for (int main : mains) {
                if (!have.count(main)) continue;
                for (auto &m : original) if (m.first == main) map.push_back({alt, m.second});
                break;
            }
        }
    }
    std::sort(map.begin(), map.end());
    if (map.empty()) { err = resolved + " has no WAV files"; return false; }
    return true;
}

bool renderSampler(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err) {
    const json &cfg = track.sampler;
    if (!cfg.is_object()) { err = "track '" + track.name + "': builtin:sampler needs a \"sampler\" object (multisample, sfz, kit or sample)"; return false; }
    static const std::set<std::string> known = {"multisample", "kit", "map", "sample", "root", "attack", "release", "oneShot",
                                                "select", "transpose", "velocity", "choke", "gain", "mono", "glide",
                                                "retrigger", "bpm", "reverse", "start", "length", "slices", "variants", "sfz",
                                                "soundfont", "program", "bank", "preset"};
    for (auto &[k, v] : cfg.items()) if (!known.count(k)) warnings.push_back("sampler: unknown setting '" + k + "'");

    std::vector<Zone> zones;
    std::unique_ptr<Zip> zip;
    std::string source;
    bool isKit = false, isSfz = false;
    int swLow = 128, swHigh = -1, swDefault = -1;
    bool notePolyOne = false;
    const bool sfzAsMultisample = cfg.contains("multisample") && cfg["multisample"].is_string() &&
                                  lower(fs::path(cfg["multisample"].get<std::string>()).extension().string()) == ".sfz";
    std::shared_ptr<SoundFont> soundfont;
    bool isSf2 = false;
    if (cfg.contains("soundfont")) {
        isSf2 = true;
        const std::string q = cfg["soundfont"].get<std::string>();
        std::string path = resolveIn(q, job.baseDir);
        if (path.empty() && !findEntry("soundfont", q, path, err)) return false;
        soundfont = std::make_shared<SoundFont>();
        if (!soundfont->open(path, err)) return false;
        const Sf2Preset *pr = nullptr;
        if (cfg.contains("preset")) {
            pr = soundfont->findByName(cfg["preset"].get<std::string>());
            if (!pr) { err = fs::path(path).filename().string() + ": no single preset named '" + cfg["preset"].get<std::string>() + "' (wavelength samples --soundfont <name> lists them)"; return false; }
        } else {
            pr = soundfont->find(cfg.value("bank", 0), cfg.value("program", 0));
            if (!pr) { err = fs::path(path).filename().string() + ": no preset for bank " + std::to_string(cfg.value("bank", 0)) + " program " + std::to_string(cfg.value("program", 0)); return false; }
        }
        std::vector<Sf2Zone> sz;
        if (!soundfont->zones(*pr, sz, err)) return false;
        sf2Zones(sz, zones);
        source = path;
    } else if (cfg.contains("sfz") || sfzAsMultisample) {
        isSfz = true;
        const std::string q = cfg.contains("sfz") ? cfg["sfz"].get<std::string>() : cfg["multisample"].get<std::string>();
        std::string path = resolveIn(q, job.baseDir);
        if (path.empty() && !findEntry("sfz", q, path, err)) return false;
        SfzFile sfz;
        if (!parseSfz(path, sfz, err)) return false;
        if (!sfzZones(sfz, zones, swLow, swHigh, swDefault, notePolyOne, warnings, err)) { err = fs::path(path).filename().string() + ": " + err; return false; }
        source = sfz.dir;
    } else if (cfg.contains("multisample")) {
        const std::string q = cfg["multisample"].get<std::string>();
        std::string path = resolveIn(q, job.baseDir);
        if (path.empty() && !findEntry("multisample", q, path, err)) return false;
        std::string xml;
        if (fs::is_directory(path)) {
            std::vector<uint8_t> x;
            if (!readFile((fs::path(path) / "multisample.xml").string(), x)) { err = path + " has no multisample.xml"; return false; }
            xml.assign(x.begin(), x.end());
            source = path;
        } else {
            zip = std::make_unique<Zip>();
            std::vector<uint8_t> x;
            if (!zip->open(path, err) || !zip->read("multisample.xml", x, err)) return false;
            xml.assign(x.begin(), x.end());
            source = path;
        }
        if (!parseMultisample(xml, zones, err)) { err = fs::path(path).filename().string() + ": " + err; return false; }
    } else if (cfg.contains("kit") || cfg.contains("map")) {
        isKit = true;
        std::vector<std::pair<int, std::string>> map;
        std::string dir;
        if (cfg.contains("kit") && cfg["kit"].is_string()) {
            std::vector<std::string> unmapped;
            if (!kitMap(cfg["kit"].get<std::string>(), job.baseDir, map, unmapped, dir, err, cfg.value("variants", std::string("keys")) == "roundrobin"))
                return false;
        }
        json explicitMap = cfg.contains("kit") && cfg["kit"].is_object() ? cfg["kit"] : cfg.value("map", json::object());
        struct KeyOpts { double gain = 0, pan = 0, tune = 0; };
        std::map<int, KeyOpts> opts;
        for (auto &[k, v] : explicitMap.items()) {
            const int key = std::atoi(k.c_str());
            if (v.is_object() && !v.contains("file")) {   // settings only, for the kit's own sample on this key
                opts[key] = {v.value("gain", 0.0), v.value("pan", 0.0), v.value("tune", 0.0)};
                continue;
            }
            std::string file = v.is_object() ? v.at("file").get<std::string>() : v.get<std::string>();
            if (v.is_object()) opts[key] = {v.value("gain", 0.0), v.value("pan", 0.0), v.value("tune", 0.0)};
            std::string path = !dir.empty() && fs::exists(fs::path(dir) / file) ? (fs::path(dir) / file).string() : resolveIn(file, job.baseDir);
            if (path.empty()) { err = "sampler: cannot find kit sample '" + file + "'"; return false; }
            map.erase(std::remove_if(map.begin(), map.end(), [&](auto &m) { return m.first == key; }), map.end());
            map.push_back({key, path});
        }
        std::map<int, int> perKey;
        for (auto &[key, file] : map) ++perKey[key];
        for (auto &[key, file] : map) {
            Zone z;
            z.file = file; z.keyLow = z.keyHigh = z.root = key; z.keyTrack = 0;
            z.roundRobin = perKey[key] > 1;
            if (opts.count(key)) { z.gainDb = opts[key].gain; z.pan = std::clamp(opts[key].pan, -1.0, 1.0); z.tune = opts[key].tune; }
            zones.push_back(z);
        }
        source = dir;
    } else if (cfg.contains("sample")) {
        const std::string f = cfg["sample"].get<std::string>();
        Zone z;
        z.file = resolveIn(f, job.baseDir);
        if (z.file.empty()) { err = "sampler: cannot find sample '" + f + "'"; return false; }
        z.root = cfg.value("root", 60);
        const int slices = cfg.value("slices", 0);
        if (slices > 1) {   // slice mode: key root + i plays the i-th equal slice, unpitched
            std::vector<uint8_t> bytes;
            SampleData d;
            std::string e2;
            if (!readFile(z.file, bytes) || !decodeAudio(bytes.data(), bytes.size(), d, e2)) { err = "sampler: cannot read " + z.file + " " + e2; return false; }
            const double len = (double)d.frames() / slices;
            for (int i = 0; i < slices && z.root + i <= 127; ++i) {
                Zone sl = z;
                sl.keyLow = sl.keyHigh = z.root + i;
                sl.root = z.root + i;
                sl.keyTrack = 0;
                sl.start = i * len; sl.stop = (i + 1) * len;
                zones.push_back(sl);
            }
        } else zones.push_back(z);
    } else {
        err = "track '" + track.name + "': the sampler needs \"multisample\", \"sfz\", \"kit\"/\"map\" or \"sample\"";
        return false;
    }

    const double sr = job.sampleRate;
    const bool allOneShot = isSfz && std::all_of(zones.begin(), zones.end(), [](const Zone &z) { return z.oneShot || z.releaseTrigger; });
    const bool oneShot = cfg.value("oneShot", isKit || allOneShot);
    const double attack = cfg.value("attack", isKit ? 0.0 : 0.002);
    const double release = cfg.value("release", isKit ? 0.05 : 0.25);
    const int select = std::clamp(cfg.value("select", 0), 0, 127);
    const double transpose = cfg.value("transpose", 0.0);
    const double velSens = std::clamp(cfg.value("velocity", 1.0), 0.0, 1.0);
    const double gainDb = cfg.value("gain", 0.0);
    std::vector<std::set<int>> chokes;
    if (cfg.contains("choke")) for (auto &g : cfg["choke"]) chokes.push_back(g.get<std::set<int>>());
    else if (isKit) chokes.push_back({42, 44, 46});   // closed and pedal hats cut the open hat
    else if (isSfz || isSf2) {   // SFZ group / off_by, SF2 exclusiveClass: a note in group G cuts what plays in regions with off_by=G
        std::map<int, std::set<int>> groups;
        for (auto &z : zones)
            if (z.offBy)
                for (auto &y : zones)
                    if (y.group == z.offBy)
                        for (int k = y.keyLow; k <= y.keyHigh; ++k) {
                            groups[z.offBy].insert(k);
                            for (int j = z.keyLow; j <= z.keyHigh; ++j) groups[z.offBy].insert(j);
                        }
        for (auto &[g, keys] : groups) chokes.push_back(keys);
    }
    const bool chokesCut = oneShot || ((isSfz || isSf2) && !chokes.empty());

    std::map<std::string, std::shared_ptr<SampleData>> cache;
    auto load = [&](const std::string &file, std::shared_ptr<SampleData> &outData) -> bool {
        auto it = cache.find(file);
        if (it != cache.end()) { outData = it->second; return true; }
        std::vector<uint8_t> bytes;
        std::string e2;
        if (soundfont && file.rfind("sf2#", 0) == 0) {
            auto d = std::make_shared<SampleData>();
            if (!soundfont->sampleData(std::atoi(file.c_str() + 4), *d, err)) return false;
            cache[file] = outData = d;
            return true;
        }
        if (isSfz && !file.empty() && file[0] == '*') {
            auto d = std::make_shared<SampleData>();
            if (!generatorSample(file, *d, err)) return false;
            cache[file] = outData = d;
            return true;
        }
        if (zip) { if (!zip->read(file, bytes, e2)) { err = e2; return false; } }
        else {
            const std::string p = fs::path(file).is_absolute() ? file : (fs::path(source) / file).string();
            const std::string found = existingIgnoringCase(p);
            if (found.empty()) { err = "sample not found: " + p + (isSfz ? " (named by the SFZ's sample= opcode; checked without case too)" : ""); return false; }
            if (!readFile(found, bytes)) { err = "cannot read " + found; return false; }
        }
        auto d = std::make_shared<SampleData>();
        if (!decodeAudio(bytes.data(), bytes.size(), *d, e2)) { err = fs::path(file).filename().string() + ": " + e2; return false; }
        cache[file] = outData = d;
        return true;
    };

    const bool mono = cfg.value("mono", false);
    const double glide = std::max(0.0, cfg.value("glide", 0.0));
    const bool retriggerCut = cfg.value("retrigger", std::string(notePolyOne ? "cut" : "overlap")) == "cut";   // SFZ note_polyphony=1
    const double loopBpm = cfg.value("bpm", 0.0);   // the sample's own tempo: resampled to the song tempo
    const bool reverseAll = cfg.value("reverse", false);
    const double startSec = std::max(0.0, cfg.value("start", 0.0));   // skip into every sample (seconds)
    const double lengthSec = std::max(0.0, cfg.value("length", 0.0));  // and play at most this much of it
    if (reverseAll || startSec > 0 || lengthSec > 0)
        for (auto &z : zones) { if (reverseAll) z.reverse = !z.reverse; z.startSec = startSec; z.lengthSec = lengthSec; }

    // notes in time order; in mono mode overlapping notes form one legato voice that glides
    std::vector<size_t> idx(track.notes.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return track.notes[a].start < track.notes[b].start; });
    struct Phrase { std::vector<const Note *> notes; double end; };
    std::vector<Phrase> phrases;
    for (size_t i : idx) {
        const Note &n = track.notes[i];
        if (mono && !phrases.empty() && n.start < phrases.back().end - 1e-6) {
            phrases.back().notes.push_back(&n);
            phrases.back().end = n.start + n.length;   // the newest note owns the voice
        } else phrases.push_back({{&n}, n.start + n.length});
    }
    auto bendAt = [](const Note &n, double t) {   // t seconds after the note start
        if (n.bend.empty()) return 0.0;
        if (t <= n.bend.front().first) return n.bend.front().second;
        for (size_t i = 1; i < n.bend.size(); ++i)
            if (t < n.bend[i].first) {
                const auto &a = n.bend[i - 1], &b = n.bend[i];
                return a.second + (b.second - a.second) * (t - a.first) / std::max(1e-9, b.first - a.first);
            }
        return n.bend.back().second;
    };

    const bool hasRelease = std::any_of(zones.begin(), zones.end(), [](const Zone &z) { return z.releaseTrigger; });
    std::map<int, size_t> rr;   // round-robin position per key (release triggers from 1000)
    std::map<int, int> missed;  // key -> notes with no zone
    // SFZ keyswitches: a note in the switch range picks the regions with that sw_last and makes no sound
    int curSw = swDefault >= 0 ? swDefault : (swHigh >= 0 ? swLow : -1);
    for (size_t pi = 0; pi < phrases.size(); ++pi) {
        const auto &ph = phrases[pi];
        const Note &n = *ph.notes.front();
        if (swHigh >= 0 && n.key >= swLow && n.key <= swHigh) { curSw = n.key; continue; }
        const int vel127 = std::clamp((int)std::lround(n.velocity * 127), 1, 127);
        auto velOk = [&](const Zone &z) { return vel127 >= z.velLow && vel127 <= z.velHigh; };
        auto selOk = [&](const Zone &z) { return select >= z.selLow && select <= z.selHigh && (z.sw < 0 || z.sw == curSw); };
        std::vector<const Zone *> hit;
        for (auto &z : zones) if (!z.releaseTrigger && n.key >= z.keyLow && n.key <= z.keyHigh && velOk(z) && selOk(z)) hit.push_back(&z);
        if (hit.empty() && !isKit && !isSfz && !isSf2) {
            // no zone covers this key: stretch the zones with the nearest root (velocity first, then any)
            for (int pass = 0; pass < 2 && hit.empty(); ++pass) {
                int best = 1000;
                for (auto &z : zones) if (!z.releaseTrigger && (pass || velOk(z)) && selOk(z)) best = std::min(best, std::abs(z.root - n.key));
                for (auto &z : zones) if (!z.releaseTrigger && (pass || velOk(z)) && selOk(z) && std::abs(z.root - n.key) == best) hit.push_back(&z);
            }
        }
        if (hit.empty()) { missed[n.key] += (int)ph.notes.size(); continue; }
        std::vector<const Zone *> play1, robin;
        for (auto *z : hit) (z->roundRobin ? robin : play1).push_back(z);
        if (!robin.empty()) play1.push_back(robin[rr[n.key]++ % robin.size()]);

        double cutAt = INFINITY;
        auto cutBy = [&](const Note &m) { if (m.start > n.start + 1e-6 && m.start - n.start < cutAt) cutAt = m.start - n.start; };
        if (mono && pi + 1 < phrases.size()) cutBy(*phrases[pi + 1].notes.front());
        for (const auto &m : track.notes) {
            if (retriggerCut && m.key == n.key) cutBy(m);
            if (chokesCut)
                for (auto &grp : chokes)
                    if (grp.count(n.key) && grp.count(m.key) && (m.key != n.key || grp.size() == 1)) cutBy(m);
        }
        // sounding key over time: glides between the phrase's notes, plus each note's bend
        std::vector<std::pair<double, int>> steps;   // (seconds after phrase start, key)
        for (auto *m : ph.notes) steps.push_back({m->start - n.start, m->key});
        const std::vector<const Note *> notes = ph.notes;
        const double phraseStart = n.start;
        auto keyAt = [steps, notes, glide, phraseStart, bendAt](double t) {
            size_t i = 0;
            while (i + 1 < steps.size() && steps[i + 1].first <= t) ++i;
            double k = steps[i].second;
            if (i > 0 && glide > 0 && t - steps[i].first < glide)
                k = steps[i - 1].second + (steps[i].second - steps[i - 1].second) * (t - steps[i].first) / glide;
            return k + bendAt(*notes[i], t - (notes[i]->start - phraseStart));
        };
        double tempoSemis = 0;
        if (loopBpm > 0) tempoSemis = 12 * std::log2(job.tempo.bpmAtBeat(job.tempo.secToBeat(n.start)) / loopBpm);
        const double velDb = velSens * 24 * std::log10(std::max(n.velocity, 0.01));   // scaled per zone by amp_veltrack
        // a voice for zone z from `from` seconds (the note start, or its end for a release trigger)
        auto voice = [&](const Zone *z, double from, double noteLen, double extraDb) -> bool {
            Voice v;
            v.zone = z;
            if (!load(z->file, v.data)) return false;
            double w = 1;
            if (z->velLowFade > 0 && vel127 < z->velLow + z->velLowFade) w *= (vel127 - z->velLow + 1.0) / (z->velLowFade + 1.0);
            if (z->velHighFade > 0 && vel127 > z->velHigh - z->velHighFade) w *= (z->velHigh - vel127 + 1.0) / (z->velHighFade + 1.0);
            v.amp = w * std::pow(10.0, (z->gainDb + gainDb + velDb * z->velTrack + extraDb) / 20);
            v.key = keyAt;
            v.semisOffset = z->tune + transpose + tempoSemis;
            v.startFrame = (size_t)std::llround((from + z->delay) * sr);
            v.noteLen = noteLen - z->delay;
            v.cutAt = cutAt - (from - n.start) - z->delay;
            if (z->sf2) {   // SoundFont: modulators for this key and velocity
                const Sf2Zone &q = *z->sf2;
                const double vel = vel127 / 127.0;
                const double cb = std::clamp(q.attenuationCb + q.modulate(Sf2Zone::kAttenuation, n.key, vel), 0.0, 1440.0);   // never a boost
                v.amp *= std::pow(10.0, -cb / 200);
                const double cents = q.fcCents + q.modulate(Sf2Zone::kFilterFc, n.key, vel);
                v.modEnvCents = q.modEnvToFc + q.modulate(Sf2Zone::kModEnvToFc, n.key, vel);
                v.resonanceDb = std::clamp((q.qCb + q.modulate(Sf2Zone::kFilterQ, n.key, vel)) / 10, 0.0, 96.0);
                // the filter runs when it closes, or resonates (FluidSynth always filters; open and flat it passes everything)
                if (cents < 13500 || cents + v.modEnvCents < 13500 || v.resonanceDb > 0) v.cutoffHz = 8.176 * std::pow(2.0, std::min(cents, 13500.0) / 1200);
                else v.modEnvCents = 0;
                v.panOffset = q.modulate(Sf2Zone::kPan, n.key, vel) / 500;
                v.semisOffset += q.modulate(Sf2Zone::kCoarseTune, n.key, vel) + q.modulate(Sf2Zone::kFineTune, n.key, vel) / 100;
                v.tcAttack = q.modulate(Sf2Zone::kAttackVol, n.key, vel);
                v.tcHold = q.modulate(Sf2Zone::kHoldVol, n.key, vel);
                v.tcDecay = q.modulate(Sf2Zone::kDecayVol, n.key, vel);
                v.tcRelease = q.modulate(Sf2Zone::kReleaseVol, n.key, vel);
                v.tcModAttack = q.modulate(Sf2Zone::kAttackMod, n.key, vel);
                v.tcModHold = q.modulate(Sf2Zone::kHoldMod, n.key, vel);
                v.tcModDecay = q.modulate(Sf2Zone::kDecayMod, n.key, vel);
                v.tcModRelease = q.modulate(Sf2Zone::kReleaseMod, n.key, vel);
            } else if (z->filter) {
                v.cutoffHz = z->cutoff * std::pow(2.0, (z->filVelCents * vel127 / 127.0 + z->filKeyCents * (n.key - z->filKeyCenter)) / 1200);
                v.resonanceDb = z->resonanceDb;
            }
            if (v.noteLen <= 0 && !std::isinf(noteLen)) return true;
            play(v, out, sr, attack, release);
            return true;
        };
        for (auto *z : play1)
            if (!voice(z, n.start, oneShot || z->oneShot ? INFINITY : ph.end - n.start, 0)) return false;
        if (hasRelease) {   // SFZ release triggers: key-up sounds when the note ends, quieter the longer it was held
            const double end = ph.end, held = ph.end - n.start;
            std::vector<const Zone *> rel;
            for (auto &z : zones) if (z.releaseTrigger && n.key >= z.keyLow && n.key <= z.keyHigh && velOk(z) && selOk(z)) rel.push_back(&z);
            std::vector<const Zone *> relOne, relRobin;
            for (auto *z : rel) (z->roundRobin ? relRobin : relOne).push_back(z);
            if (!relRobin.empty()) relOne.push_back(relRobin[rr[1000 + n.key]++ % relRobin.size()]);
            for (auto *z : relOne)
                if (!voice(z, end, INFINITY, -z->rtDecay * held)) return false;
        }
    }
    size_t silent = 0;
    std::string keys;
    for (auto &[k, c] : missed) { silent += c; keys += (keys.empty() ? "" : ", ") + std::to_string(k); }
    if (silent) warnings.push_back(std::to_string(silent) + " note(s) matched no sample zone (keys " + keys + ")" +
                                   (isKit ? "; run `wavelength samples --kit <name>` for the key map" : ""));
    return true;
}

} // namespace wl
