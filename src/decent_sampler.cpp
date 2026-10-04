#include "decent_sampler.hpp"

#include "xml.hpp"
#include "zip.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string upper(std::string s) {
    for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool isTrue(const std::string &v) { const std::string t = lower(trim(v)); return t == "true" || t == "1" || t == "yes" || t == "on"; }
bool isFalse(const std::string &v) { const std::string t = lower(trim(v)); return t == "false" || t == "0" || t == "no" || t == "off"; }
double toNum(const std::string &v, double def) {
    const std::string t = trim(v);
    if (t.empty()) return def;
    char *end = nullptr;
    const double x = std::strtod(t.c_str(), &end);
    return end == t.c_str() || !std::isfinite(x) ? def : x;
}
std::string fmt(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}
double linDb(double lin) { return lin > 1e-8 ? 20 * std::log10(lin) : -200; }
// A DecentSampler volume: "-3dB" (decibels) or a linear factor ("0.5")
double volumeDb(const std::string &v, double defDb = 0) {
    std::string t = lower(trim(v));
    if (t.empty()) return defDb;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "db") == 0) {
        t = trim(t.substr(0, t.size() - 2));
        if (t == "-inf") return -200;
        return toNum(t, defDb);
    }
    return linDb(toNum(t, 1));
}
// A level in dB: "-6", "-6dB"
double dbValue(const std::string &v, double def) {
    std::string t = lower(trim(v));
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "db") == 0) t = t.substr(0, t.size() - 2);
    return toNum(t, def);
}
std::vector<std::string> splitOn(const std::string &v, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = v.find(sep, a);
        const std::string t = trim(v.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (!t.empty()) out.push_back(t);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}
std::vector<std::string> splitList(const std::string &v) { return splitOn(v, ','); }

// An element's attributes by lower-case name; when a name repeats (19 of the presets in one library do), the last wins
using Attrs = std::map<std::string, std::string>;
Attrs attrsOf(const xml::Node &n) {
    Attrs a;
    for (auto &[k, v] : n.attrs) a[lower(trim(k))] = v;
    return a;
}
const std::string *attr(const Attrs &a, const char *k) {
    auto it = a.find(k);
    return it == a.end() ? nullptr : &it->second;
}
std::string get(const Attrs &a, const char *k, const std::string &def = "") { auto *v = attr(a, k); return v ? *v : def; }
double num(const Attrs &a, const char *k, double def) { auto *v = attr(a, k); return v ? toNum(*v, def) : def; }
std::string tagOf(const xml::Node &n) { return lower(n.tag); }

bool readText(const fs::path &p, std::string &out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// `path` as it is on disk, matching each part without case when the exact spelling is missing (presets written on
// Windows spell "Samples\Kick.WAV" for "samples/kick.wav"); "" when nothing matches
std::string onDisk(const fs::path &p) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return p.string();
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
    return fs::is_regular_file(cur, ec) ? cur.string() : "";
}

struct Group {
    Attrs a;
    std::vector<Attrs> samples, oscillators, fx;
};

struct Preset {
    Attrs root, groups;                  // <DecentSampler>, <groups> (the instrument level)
    std::vector<Group> group;
    std::vector<Attrs> fx;               // the instrument's <effects>
    std::map<std::string, Attrs> tags;   // <tags><tag name=...>
    std::vector<std::string> notes;
    std::set<std::string> skippedParams;  // bound parameters that play no part here
    bool velAmp = false;                 // <midi><velocity> drives the instrument volume
    std::set<size_t> velAmpGroups;       // ... or these groups' volume
    std::set<size_t> velFilterFx;        // instrument filters whose frequency follows velocity
    std::set<std::pair<size_t, size_t>> velFilterGroupFx;   // (group, effect) the same for group effects
    void note(const std::string &n) { if (std::find(notes.begin(), notes.end(), n) == notes.end()) notes.push_back(n); }
};

bool isFilter(const std::string &type) {
    return type == "lowpass" || type == "lowpass_4pl" || type == "lowpass_1pl" || type == "highpass" || type == "hipass" || type == "bandpass";
}

// The effect attribute a binding parameter sets, for the effect types that have it ("" = that type has none)
std::string fxAttr(const std::string &param, const std::string &type) {
    const bool filter = isFilter(type), eq = type == "peak" || type == "notch" || type == "low_shelf" || type == "high_shelf";
    if (param == "ENABLED") return "enabled";
    if (param == "FX_FILTER_FREQUENCY" && (filter || eq)) return "frequency";
    if (param == "FX_FILTER_RESONANCE" && filter) return "resonance";
    if (param == "FX_FILTER_Q" && eq) return "q";
    if (param == "FX_FILTER_GAIN" && eq) return "gain";
    if (param == "FX_REVERB_WET_LEVEL" && type == "reverb") return "wetlevel";
    if (param == "FX_REVERB_ROOM_SIZE" && type == "reverb") return "roomsize";
    if (param == "FX_REVERB_DAMPING" && type == "reverb") return "damping";
    if (param == "FX_MIX" && (type == "chorus" || type == "phaser" || type == "convolution" || type == "pitch_shift")) return "mix";
    if (param == "FX_MOD_DEPTH" && (type == "chorus" || type == "phaser")) return "moddepth";
    if (param == "FX_MOD_RATE" && (type == "chorus" || type == "phaser")) return "modrate";
    if (param == "FX_CENTER_FREQUENCY" && type == "phaser") return "centerfrequency";
    if (param == "FX_FEEDBACK" && (type == "phaser" || type == "delay")) return "feedback";
    if (param == "FX_FEEDBACK_CUTOFF" && type == "delay") return "feedbackcutoff";
    if (param == "FX_DELAY_TIME" && type == "delay") return "delaytime";
    if (param == "FX_DELAY_TIME_FORMAT" && type == "delay") return "delaytimeformat";
    if (param == "FX_STEREO_OFFSET" && type == "delay") return "stereooffset";
    if (param == "FX_WET_LEVEL" && type == "delay") return "wetlevel";
    if (param == "LEVEL" && type == "gain") return "level";
    if (param == "FX_IR_FILE" && type == "convolution") return "irfile";
    if (param == "FX_PITCH_SHIFT" && type == "pitch_shift") return "pitchshift";
    return "";
}

// The group (and instrument) attribute an amp / general binding parameter sets
std::string levelAttr(const std::string &param, bool instrument) {
    static const std::map<std::string, std::string> common = {
        {"AMP_VOLUME", "modvolume"}, {"ENV_ATTACK", "attack"}, {"ENV_DECAY", "decay"}, {"ENV_SUSTAIN", "sustain"},
        {"ENV_RELEASE", "release"}, {"ENV_ATTACK_CURVE", "attackcurve"}, {"ENV_DECAY_CURVE", "decaycurve"},
        {"ENV_RELEASE_CURVE", "releasecurve"}, {"AMP_VEL_TRACK", "ampveltrack"}, {"GLIDE_TIME", "glidetime"},
        {"GLIDE_MODE", "glidemode"}, {"LO_NOTE", "lonote"}, {"HI_NOTE", "hinote"}, {"ROOT_NOTE", "rootnote"},
        {"SAMPLE_START", "start"}, {"SAMPLE_END", "end"}, {"LOOP_START", "loopstart"}, {"LOOP_END", "loopend"},
        {"PITCH_KEY_TRACK", "pitchkeytrack"}, {"SILENCING_MODE", "silencingmode"}, {"SILENCING_DECAY", "silencingdecay"}};
    auto it = common.find(param);
    if (it != common.end()) return it->second;
    if (instrument) {
        if (param == "GLOBAL_TUNING") return "globaltuning";
        if (param == "PAN" || param == "GLOBAL_PAN") return "globalpan";
        return "";
    }
    if (param == "ENABLED") return "enabled";
    if (param == "AMP_ENV_ENABLED") return "ampenvenabled";
    if (param == "GROUP_TUNING") return "grouptuning";
    if (param == "PAN") return "pan";
    if (param == "GROUP_PAN") return "grouppan";
    if (param == "LO_VEL") return "lovel";
    if (param == "HI_VEL") return "hivel";
    return "";
}

// What a binding passes on for a control at value v (its range [lo, hi]): the factor, then the translation
std::string translate(const Attrs &b, double v, double lo, double hi) {
    const std::string mode = lower(trim(get(b, "translation", "linear")));
    const double factor = num(b, "factor", 1);
    if (mode == "fixed_value") return get(b, "translationvalue");
    const double x = v * factor;
    if (mode == "table") {   // "in,out;in,out;...", straight lines between the points, flat beyond the ends
        std::vector<std::pair<double, double>> pts;
        for (const std::string &pair : splitOn(get(b, "translationtable"), ';')) {
            const size_t c = pair.find(',');
            if (c != std::string::npos) pts.push_back({toNum(pair.substr(0, c), 0), toNum(pair.substr(c + 1), 0)});
        }
        if (pts.empty()) return fmt(x);
        std::stable_sort(pts.begin(), pts.end(), [](auto &p, auto &q) { return p.first < q.first; });
        if (x <= pts.front().first) return fmt(pts.front().second);
        for (size_t i = 1; i < pts.size(); ++i)
            if (x <= pts[i].first) {
                const double span = pts[i].first - pts[i - 1].first;
                return fmt(span > 0 ? pts[i - 1].second + (pts[i].second - pts[i - 1].second) * (x - pts[i - 1].first) / span : pts[i].second);
            }
        return fmt(pts.back().second);
    }
    // linear: the control's range onto [translationOutputMin, translationOutputMax]; without them the value as it is
    if (!attr(b, "translationoutputmin") && !attr(b, "translationoutputmax")) return fmt(x);
    double t = hi > lo ? (v - lo) / (hi - lo) : 0;
    t = std::clamp(t, 0.0, 1.0);
    if (isTrue(get(b, "translationreversed"))) t = 1 - t;
    const double a = num(b, "translationoutputmin", 0), z = num(b, "translationoutputmax", 1);
    return fmt(a + t * (z - a));
}

std::vector<size_t> indexes(const Attrs &b, std::initializer_list<const char *> keys, size_t count) {
    for (const char *k : keys)
        if (auto *v = attr(b, k)) {
            const double i = toNum(*v, -1);
            if (i >= 0 && (size_t)i < count) return {(size_t)i};
            return {};
        }
    return {};
}
bool hasTag(const Attrs &a, const std::string &wanted) {
    for (auto &t : splitList(get(a, "tags"))) if (t == wanted) return true;
    return false;
}

// A binding fired at load: its target takes the translated value
void applyBinding(Preset &p, const Attrs &b, double v, double lo, double hi) {
    if (attr(b, "enabled") && isFalse(get(b, "enabled"))) return;
    if (attr(b, "triggeronload") && isFalse(get(b, "triggeronload"))) return;
    const std::string type = lower(trim(get(b, "type"))), level = lower(trim(get(b, "level"))), param = upper(trim(get(b, "parameter")));
    const std::string val = translate(b, v, lo, hi);
    if (type == "effect") {
        if (level == "group") {   // an effect inside a group: the group by groupIndex/position, the effect by effectIndex
            for (size_t g : indexes(b, {"groupindex", "position"}, p.group.size())) {
                auto &fx = p.group[g].fx;
                const std::vector<size_t> e = attr(b, "effectindex") ? indexes(b, {"effectindex"}, fx.size()) : std::vector<size_t>(fx.empty() ? 0 : 1, 0);
                for (size_t i : e) {
                    const std::string a = fxAttr(param, lower(get(fx[i], "type")));
                    if (a.empty()) p.skippedParams.insert(param + " on a " + lower(get(fx[i], "type")));
                    else fx[i][a] = val;
                }
            }
            return;
        }
        std::vector<size_t> targets = indexes(b, {"effectindex", "position"}, p.fx.size());
        const std::string tags = get(b, "effecttags", get(b, "tags"));
        if (targets.empty() && !tags.empty())
            for (size_t i = 0; i < p.fx.size(); ++i)
                for (auto &t : splitList(tags)) if (hasTag(p.fx[i], t)) { targets.push_back(i); break; }
        for (size_t i : targets) {
            const std::string a = fxAttr(param, lower(get(p.fx[i], "type")));
            if (a.empty()) p.skippedParams.insert(param + " on a " + lower(get(p.fx[i], "type")));
            else p.fx[i][a] = val;
        }
        return;
    }
    if (level == "instrument" && (type == "amp" || type == "general" || type.empty())) {
        const std::string a = levelAttr(param, true);
        if (a.empty()) p.skippedParams.insert(param);
        else p.groups[a] = val;
        return;
    }
    if (level == "group" && (type == "amp" || type == "general" || type.empty())) {
        std::vector<size_t> targets = indexes(b, {"groupindex", "position"}, p.group.size());
        const std::string tags = get(b, "grouptags", get(b, "tags"));
        if (!attr(b, "groupindex") && !attr(b, "position") && !tags.empty())
            for (size_t i = 0; i < p.group.size(); ++i)
                for (auto &t : splitList(tags)) if (hasTag(p.group[i].a, t)) { targets.push_back(i); break; }
        const std::string a = levelAttr(param, false);
        if (a.empty()) { p.skippedParams.insert(param); return; }
        for (size_t g : targets) p.group[g].a[a] = val;
        return;
    }
    if (level == "tag") {
        const std::vector<std::string> names = splitList(get(b, "identifier", get(b, "tags")));
        for (auto &n : names) {
            if (param == "AMP_VOLUME" || param == "TAG_VOLUME") p.tags[n]["volume"] = val;
            else if (param == "ENABLED" || param == "TAG_ENABLED") p.tags[n]["enabled"] = val;
            else p.skippedParams.insert(param);
        }
        return;
    }
    if (level == "ui" || type == "control" || type == "keyboard_color" || type == "button_state_binding") return;   // the interface itself
    p.skippedParams.insert(param.empty() ? type : param);
}

// The bindings of a control at its saved value: a knob or slider's `value`, a menu's selected option (1-based), a
// button's or multi-state control's state (0-based)
void applyControls(Preset &p, const xml::Node &n) {
    for (auto &c : n.children) {
        const std::string t = tagOf(*c);
        const Attrs a = attrsOf(*c);
        if (t == "labeled-knob" || t == "control" || t == "knob" || t == "slider" || t == "labeled-slider") {
            const double lo = num(a, "minvalue", 0), hi = num(a, "maxvalue", 1);
            double v = num(a, "value", 0);
            if (hi > lo) v = std::clamp(v, lo, hi);
            const std::string vt = lower(get(a, "valuetype", get(a, "type")));
            std::vector<const xml::Node *> states;
            for (auto &s : c->children) if (tagOf(*s) == "state") states.push_back(s.get());
            if (vt == "multi_state" && !states.empty()) {
                const size_t i = (size_t)std::clamp((long)std::lround(v), 0L, (long)states.size() - 1);
                for (auto &b : states[i]->children) if (tagOf(*b) == "binding") applyBinding(p, attrsOf(*b), v, lo, hi);
            }
            for (auto &b : c->children) if (tagOf(*b) == "binding") applyBinding(p, attrsOf(*b), v, lo, hi);
        } else if (t == "menu") {
            std::vector<const xml::Node *> options;
            for (auto &o : c->children) if (tagOf(*o) == "option") options.push_back(o.get());
            const long v = std::lround(num(a, "value", 0));
            if (v >= 1 && v <= (long)options.size())
                for (auto &b : options[(size_t)v - 1]->children)
                    if (tagOf(*b) == "binding") applyBinding(p, attrsOf(*b), (double)v, 1, (double)options.size());
        } else if (t == "button") {
            std::vector<const xml::Node *> states;
            for (auto &s : c->children) if (tagOf(*s) == "state") states.push_back(s.get());
            const long v = std::lround(num(a, "value", 0));
            if (v >= 0 && v < (long)states.size())
                for (auto &b : states[(size_t)v]->children)
                    if (tagOf(*b) == "binding") applyBinding(p, attrsOf(*b), (double)v, 0, (double)std::max<size_t>(1, states.size() - 1));
        } else if (t != "binding" && t != "option" && t != "state") applyControls(p, *c);   // tabs and other containers
    }
}

// <midi>: velocity bindings to the amplitude and to a filter play; controller and note bindings don't
void readMidi(Preset &p, const xml::Node &midi) {
    size_t cc = 0, notes = 0;
    for (auto &c : midi.children) {
        const std::string t = tagOf(*c);
        if (t == "cc") { for (auto &b : c->children) cc += tagOf(*b) == "binding"; }
        else if (t == "note") { for (auto &b : c->children) notes += tagOf(*b) == "binding"; }
        else if (t == "velocity")
            for (auto &bn : c->children) {
                if (tagOf(*bn) != "binding") continue;
                const Attrs b = attrsOf(*bn);
                const std::string param = upper(trim(get(b, "parameter"))), level = lower(trim(get(b, "level")));
                if (param == "AMP_VOLUME" && level == "instrument") p.velAmp = true;
                else if (param == "AMP_VOLUME" && level == "group") { for (size_t g : indexes(b, {"groupindex", "position"}, p.group.size())) p.velAmpGroups.insert(g); }
                else if (param == "FX_FILTER_FREQUENCY" && level == "group") {
                    for (size_t g : indexes(b, {"groupindex", "position"}, p.group.size())) {
                        const std::vector<size_t> e = attr(b, "effectindex") ? indexes(b, {"effectindex"}, p.group[g].fx.size()) : std::vector<size_t>(p.group[g].fx.empty() ? 0 : 1, 0);
                        for (size_t i : e) p.velFilterGroupFx.insert({g, i});
                    }
                } else if (param == "FX_FILTER_FREQUENCY") { for (size_t i : indexes(b, {"effectindex", "position"}, p.fx.size())) p.velFilterFx.insert(i); }
                else p.note("a velocity binding to " + param + " is left out");
            }
    }
    if (cc) p.note(std::to_string(cc) + " MIDI controller binding(s) left out (controllers rest where the controls are)");
    if (notes) p.note(std::to_string(notes) + " MIDI note binding(s) (keyswitches) left out");
}

// DecentSampler's envelope curves run from -100 (logarithmic) to 100 (exponential)
double curveOr(const std::string *v, double def) { return v ? std::clamp(toNum(*v, def), -100.0, 100.0) : def; }

struct Source {   // where the preset and its samples are: a folder on disk or a zip
    fs::path dir;                    // the preset's folder (disk), or its folder inside the zip ("" = the zip's root)
    std::unique_ptr<Zip> zip;
    std::map<std::string, std::string> entries;   // zip: lower-case entry name -> entry name
};

// A sample's path: on disk an absolute path (matched without case), in a zip its entry name; "" when it isn't there
std::string resolveSample(const Source &src, const std::string &raw, const std::string &samplePath) {
    std::string p = raw;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.empty()) return "";
    if (src.zip) {
        const std::string e = (src.dir / fs::u8path(p)).lexically_normal().generic_u8string();
        auto it = src.entries.find(lower(e));
        if (it != src.entries.end()) return it->second;
        it = src.entries.find(lower(fs::u8path(p).lexically_normal().generic_u8string()));
        return it == src.entries.end() ? "" : it->second;
    }
    const fs::path rel = fs::u8path(p);
    std::vector<fs::path> tries;
    if (rel.is_absolute()) tries.push_back(rel);
    else tries.push_back(src.dir / rel);
    if (!samplePath.empty()) {   // a preset DecentSampler saved names the library it came from
        std::string sp = samplePath;
        std::replace(sp.begin(), sp.end(), '\\', '/');
        if (fs::u8path(sp).is_absolute()) tries.push_back(fs::u8path(sp) / rel);
    }
    const bool windows = p.size() > 2 && p[1] == ':' && p[2] == '/';
    if (rel.is_absolute() || windows) {   // an absolute path from another computer: the file by name beside the preset
        tries.push_back(src.dir / rel.filename());
        tries.push_back(src.dir / "Samples" / rel.filename());
    }
    for (auto &t : tries) {
        const std::string f = onDisk(t.lexically_normal());
        if (!f.empty()) return f;
    }
    return "";
}

} // namespace

size_t decentSampleCount(const std::string &text, std::vector<std::string> *sampleDirs) {
    size_t n = 0;
    std::set<std::string> dirs;
    for (size_t q = 0; (q = text.find("<sample", q)) != std::string::npos; ++q) {
        const char c = q + 7 < text.size() ? text[q + 7] : ' ';
        if (!std::isspace((unsigned char)c) && c != '/' && c != '>') continue;   // <samples>, <sampleX>
        ++n;
        if (!sampleDirs) continue;
        const size_t end = text.find('>', q);
        const std::string tag = text.substr(q, end == std::string::npos ? std::string::npos : end - q);
        for (size_t a = 0; (a = tag.find("path", a)) != std::string::npos; a += 4) {
            if (a > 0 && !std::isspace((unsigned char)tag[a - 1])) continue;
            size_t e = a + 4;
            while (e < tag.size() && std::isspace((unsigned char)tag[e])) ++e;
            if (e >= tag.size() || tag[e] != '=') continue;
            ++e;
            while (e < tag.size() && std::isspace((unsigned char)tag[e])) ++e;
            if (e >= tag.size() || (tag[e] != '"' && tag[e] != '\'')) continue;
            const size_t close = tag.find(tag[e], e + 1);
            if (close == std::string::npos) break;
            std::string v = tag.substr(e + 1, close - e - 1);
            std::replace(v.begin(), v.end(), '\\', '/');
            const size_t slash = v.rfind('/');
            dirs.insert(slash == std::string::npos ? "" : v.substr(0, slash));
            break;
        }
    }
    if (sampleDirs) sampleDirs->assign(dirs.begin(), dirs.end());
    return n;
}

std::vector<std::string> decentLibraryPresets(const std::string &zipPath, std::string &err) {
    Zip z;
    std::vector<std::string> out;
    if (!z.open(zipPath, err)) return out;
    for (auto &e : z.entries()) {
        const std::string n = e.name, base = fs::u8path(n).filename().u8string();
        if (lower(fs::u8path(n).extension().u8string()) != ".dspreset" || n.rfind("__MACOSX", 0) == 0 || base.rfind("._", 0) == 0) continue;
        out.push_back(n);
    }
    return out;
}

bool readDecentPreset(const std::string &spec, DecentPreset &out, std::string &err) {
    out = DecentPreset{};
    std::error_code ec;
    std::string path = spec, pick;
    const size_t hash = spec.rfind('#');
    if (hash != std::string::npos && !fs::exists(fs::u8path(spec), ec) && fs::exists(fs::u8path(spec.substr(0, hash)), ec)) {
        path = spec.substr(0, hash);
        pick = spec.substr(hash + 1);
    }
    auto choose = [&](const std::vector<std::string> &names, const std::string &where, std::string &chosen) {
        if (names.empty()) { err = where + " holds no .dspreset"; return false; }
        if (!pick.empty()) {
            for (auto &n : names) {
                const std::string stem = fs::u8path(n).stem().u8string(), file = fs::u8path(n).filename().u8string();
                if (n == pick || lower(file) == lower(pick) || lower(stem) == lower(pick)) { chosen = n; return true; }
            }
            err = where + " has no preset named '" + pick + "'";
        } else if (names.size() == 1) { chosen = names[0]; return true; }
        else err = where + " holds " + std::to_string(names.size()) + " presets: name one as \"" + fs::u8path(where).filename().u8string() + "#<preset>\"";
        err += " (";
        for (size_t i = 0; i < names.size() && i < 8; ++i) err += (i ? ", " : "") + fs::u8path(names[i]).stem().u8string();
        err += names.size() > 8 ? ", ...)" : ")";
        return false;
    };
    Source src;
    std::string text;
    if (fs::is_directory(fs::u8path(path), ec)) {   // a .dsbundle: the presets in it (in its top folder first)
        std::vector<std::string> names;
        for (auto &e : fs::directory_iterator(fs::u8path(path), ec))
            if (e.is_regular_file(ec) && lower(e.path().extension().u8string()) == ".dspreset" && e.path().filename().u8string().rfind("._", 0) != 0)
                names.push_back(e.path().u8string());
        if (names.empty())
            for (auto it = fs::recursive_directory_iterator(fs::u8path(path), fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && lower(it->path().extension().u8string()) == ".dspreset" && it->path().filename().u8string().rfind("._", 0) != 0)
                    names.push_back(it->path().u8string());
            }
        std::sort(names.begin(), names.end());
        std::string chosen;
        if (!choose(names, path, chosen)) return false;
        path = chosen;
    }
    std::string head;
    { std::ifstream in(fs::u8path(path), std::ios::binary); char b[4] = {0, 0, 0, 0}; in.read(b, 4); head.assign(b, (size_t)in.gcount()); }
    const bool zipped = head.size() == 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4;
    if (zipped || lower(fs::u8path(path).extension().u8string()) == ".dslibrary") {   // a .dslibrary
        src.zip = std::make_unique<Zip>();
        if (!src.zip->open(path, err)) return false;
        for (auto &e : src.zip->entries()) src.entries.emplace(lower(e.name), e.name);
        std::string entry;
        if (!choose(decentLibraryPresets(path, err), path, entry)) return false;
        std::vector<uint8_t> bytes;
        if (!src.zip->read(entry, bytes, err)) return false;
        text.assign(bytes.begin(), bytes.end());
        src.dir = fs::u8path(entry).parent_path();
        out.zip = path;
        out.name = fs::u8path(entry).stem().u8string();
    } else {
        if (!readText(fs::u8path(path), text)) { err = "cannot read " + path; return false; }
        src.dir = fs::absolute(fs::u8path(path), ec).parent_path();
        out.name = fs::u8path(path).stem().u8string();
    }
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.erase(0, 3);
    std::string perr;
    auto doc = xml::parse(text, perr);
    if (!doc && perr.find("is not closed") != std::string::npos) {   // a file cut short: close what is open
        const std::string fixed = text + "</group></groups></DecentSampler>";
        doc = xml::parse(fixed, perr);
    }
    if (!doc) { err = out.name + ".dspreset: not readable XML (" + perr + ")"; return false; }
    if (tagOf(*doc) != "decentsampler") { err = out.name + ".dspreset: not a DecentSampler preset (its root is <" + doc->tag + ">)"; return false; }

    Preset p;
    p.root = attrsOf(*doc);
    const xml::Node *ui = nullptr, *midi = nullptr;
    bool haveFx = false, haveGroups = false;
    for (auto &c : doc->children) {
        const std::string t = tagOf(*c);
        if (t == "groups") {
            for (auto &[k, v] : attrsOf(*c)) if (!haveGroups || !p.groups.count(k)) p.groups[k] = v;
            haveGroups = true;
            Group implicit;
            for (auto &g : c->children) {
                const std::string gt = tagOf(*g);
                if (gt == "sample") { implicit.samples.push_back(attrsOf(*g)); continue; }   // a sample outside any group
                if (gt != "group") continue;
                Group grp;
                grp.a = attrsOf(*g);
                for (auto &s : g->children) {
                    const std::string st = tagOf(*s);
                    if (st == "sample") grp.samples.push_back(attrsOf(*s));
                    else if (st == "oscillator") grp.oscillators.push_back(attrsOf(*s));
                    else if (st == "effects") for (auto &e : s->children) if (tagOf(*e) == "effect") grp.fx.push_back(attrsOf(*e));
                }
                p.group.push_back(std::move(grp));
            }
            if (!implicit.samples.empty()) p.group.push_back(std::move(implicit));
        } else if (t == "effects") {
            if (haveFx) { p.note("a second <effects> list left out"); continue; }
            haveFx = true;
            for (auto &e : c->children) if (tagOf(*e) == "effect") p.fx.push_back(attrsOf(*e));
        } else if (t == "tags") {
            for (auto &tg : c->children)
                if (tagOf(*tg) == "tag") { const Attrs a = attrsOf(*tg); if (!get(a, "name").empty()) p.tags[trim(get(a, "name"))] = a; }
        } else if (t == "ui") ui = c.get();
        else if (t == "midi") midi = c.get();
        else if (t == "modulators") { if (!c->children.empty()) p.note("modulators (LFOs, envelopes) left out"); }
        else if (t == "buses") { if (!c->children.empty()) p.note("buses left out: everything plays on the main output"); }
        else if (t == "notesequences" || t == "arpeggiator") { if (!c->children.empty() || isTrue(get(attrsOf(*c), "enabled"))) p.note("<" + c->tag + "> left out"); }
    }
    if (ui) applyControls(p, *ui);
    if (midi) readMidi(p, *midi);
    if (!p.skippedParams.empty()) {
        std::string l;
        for (auto &s : p.skippedParams) l += (l.empty() ? "" : ", ") + s;
        p.note("control bindings without a counterpart left out: " + l);
    }
    const std::string samplePath = get(p.root, "samplepath");
    if (!samplePath.empty()) p.note("a preset DecentSampler saved (its library was at " + samplePath + "): its samples are read from beside it");

    // ---- the effects chain; a filter whose frequency follows velocity becomes a per-voice filter instead
    struct RegionFilter { std::string type; double freq = 0, q = 0.7; bool vel = false; };
    std::unique_ptr<RegionFilter> instFilter;
    for (size_t i = 0; i < p.fx.size(); ++i) {
        const Attrs &e = p.fx[i];
        const std::string type = lower(trim(get(e, "type")));
        if (attr(e, "enabled") && isFalse(get(e, "enabled"))) continue;
        if (isFilter(type)) {
            const double f = std::max(0.0, num(e, "frequency", 22000));
            const double q = std::clamp(num(e, "resonance", 0.7), 0.05, 18.0);
            const bool low = type.rfind("lowpass", 0) == 0, high = type == "highpass" || type == "hipass";
            if (p.velFilterFx.count(i) && !instFilter) {
                instFilter = std::make_unique<RegionFilter>(RegionFilter{low ? "lpf_2p" : high ? "hpf_2p" : "bpf_2p", f, type == "lowpass_1pl" ? 0.5 : q, true});
                p.note("its " + type + " filter opens with velocity, per voice, from " + fmt(std::round(f)) + " Hz to 22 kHz (an approximation of its velocity binding)");
                continue;
            }
            if ((low && f >= 20000) || (high && f <= 20)) continue;   // wide open: nothing to hear
            out.effects.push_back({{"type", "filter"}, {"mode", low ? "lowpass" : high ? "highpass" : "bandpass"}, {"cutoff", std::max(5.0, f)},
                                   {"resonance", type == "lowpass_1pl" ? 0.5 : q}});
            if (type == "lowpass_1pl") p.note("the one-pole low-pass plays as a gentle two-pole one");
        } else if (type == "peak" || type == "notch" || type == "low_shelf" || type == "high_shelf") {
            const bool dbUnit = lower(get(e, "gainunit")) == "decibels";
            const double g = num(e, "gain", dbUnit ? 0 : 1);
            const double db = type == "notch" ? -30 : dbUnit ? g : linDb(std::max(g, 1e-4));
            if (std::fabs(db) < 0.05) continue;
            const double f = std::clamp(num(e, "frequency", type == "low_shelf" ? 200 : type == "high_shelf" ? 5000 : 10000), 20.0, 22000.0);
            out.effects.push_back({{"type", "eq"}, {"bands", json::array({{{"type", type == "low_shelf" ? "lowshelf" : type == "high_shelf" ? "highshelf" : "peak"},
                                                                          {"freq", f}, {"q", std::clamp(num(e, "q", 0.7), 0.05, 18.0)}, {"gain", db}}})}});
        } else if (type == "gain") {
            const double db = lower(get(e, "levelunit")) == "linear" ? linDb(num(e, "level", 0.5)) : dbValue(get(e, "level"), -6);
            if (std::fabs(db) >= 0.05) out.effects.push_back({{"type", "gain"}, {"db", std::max(-100.0, db)}});
        } else if (type == "reverb") {
            // Freeverb (JUCE's Reverb): the comb feedback 0.7 + 0.28 x room size sets the decay (RT60 within 0.1 s of
            // DecentSampler's for room sizes 0.3-1); the wet signal adds to the full dry one, 3.76 x wetLevel as loud as this
            // reverb's wet signal (both measured on noise bursts against DecentSampler 1.11)
            const double w = std::clamp(num(e, "wetlevel", 0), 0.0, 1.0);
            if (w <= 0) continue;
            const double room = std::clamp(num(e, "roomsize", 0.7), 0.0, 1.0), fb = 0.7 + 0.28 * room;
            const double rt60 = std::clamp(3 * 0.0306 / -std::log10(fb), 0.3, 20.0), g = 3.76 * w;
            out.effects.push_back({{"type", "reverb"}, {"decay", std::round(rt60 * 100) / 100}, {"size", room}, {"damping", std::clamp(num(e, "damping", 0.3), 0.0, 1.0)},
                                   {"predelay", 0}, {"highpass", 20}, {"mix", g / (1 + g)}});
            out.effects.push_back({{"type", "gain"}, {"db", linDb(1 + g)}});
        } else if (type == "delay") {
            const double w = std::clamp(num(e, "wetlevel", 0.5), 0.0, 1.0);
            if (w <= 0) continue;
            if (lower(get(e, "delaytimeformat")) == "musical_time") { p.note("a tempo-synced delay left out"); continue; }
            out.effects.push_back({{"type", "delay"}, {"ms", std::max(1.0, 1000 * num(e, "delaytime", 0.7))}, {"feedback", std::clamp(num(e, "feedback", 0.2), 0.0, 0.97)},
                                   {"pingpong", false}, {"highpass", 20}, {"lowpass", std::clamp(num(e, "feedbackcutoff", 22000), 20.0, 20000.0)}, {"mix", w / (1 + w)}});
            out.effects.push_back({{"type", "gain"}, {"db", linDb(1 + w)}});
            if (std::fabs(num(e, "stereooffset", 0)) > 1e-4) p.note("the delay's stereo offset left out (both sides at its time)");
        } else if (type == "chorus") {   // JUCE's Chorus: 7 ms +- 10 ms x depth
            const double mix = std::clamp(num(e, "mix", 0.5), 0.0, 1.0), depth = std::clamp(num(e, "moddepth", 0.2), 0.0, 1.0);
            if (mix <= 0) continue;
            out.effects.push_back({{"type", "chorus"}, {"rate", std::max(0.01, num(e, "modrate", 0.2))}, {"depth", std::max(0.1, 20 * depth)},
                                   {"delay", std::max(1.0, 7 - 10 * depth)}, {"mix", mix}});
        } else if (type == "phaser") {   // JUCE's Phaser: the centre +- depth / 2 of the 20 Hz - 20 kHz log range
            const double mix = std::clamp(num(e, "mix", 0.5), 0.0, 1.0), depth = std::clamp(num(e, "moddepth", 0.2), 0.0, 1.0);
            if (mix <= 0) continue;
            const double c = std::clamp(num(e, "centerfrequency", 400), 20.0, 20000.0), oct = 4.98 * depth;
            const double lo = std::max(20.0, c * std::pow(2.0, -oct)), hi = std::max(lo * 1.02, std::min(20000.0, c * std::pow(2.0, oct)));
            out.effects.push_back({{"type", "phaser"}, {"rate", std::max(0.01, num(e, "modrate", 0.2))}, {"floor", lo}, {"ceiling", hi}, {"stages", 6},
                                   {"feedback", std::clamp(num(e, "feedback", 0.7), -0.95, 0.95)}, {"spread", 0}, {"mix", mix}});
        } else if (type == "convolution") {
            const double mix = std::clamp(num(e, "mix", 0.5), 0.0, 1.0);
            if (mix <= 0) continue;
            const std::string ir = src.zip ? "" : resolveSample(src, get(e, "irfile"), samplePath);
            if (ir.empty()) { p.note("the convolution's impulse response " + std::string(src.zip ? "is inside the library" : "isn't there") + ": left out"); continue; }
            out.effects.push_back({{"type", "convolve"}, {"ir", ir}, {"mix", mix}});
        } else if (!type.empty()) p.note("its " + type + " effect left out");
    }

    // ---- the regions
    // tags: a voice's are its group's and its own; a tag switched off silences it, tag volumes multiply
    auto tagGain = [&](const std::vector<std::string> &tags, bool &on) {
        double db = 0;
        on = true;
        for (auto &t : tags) {
            auto it = p.tags.find(t);
            if (it == p.tags.end()) continue;
            if (attr(it->second, "enabled") && isFalse(get(it->second, "enabled"))) on = false;
            if (attr(it->second, "volume")) db += volumeDb(get(it->second, "volume"));
            if (std::fabs(num(it->second, "pan", 0)) > 0.5) p.note("tag pans left out");
            if (attr(it->second, "polyphony") && num(it->second, "polyphony", -1) > 0) p.note("tag polyphony left out");
        }
        return db;
    };
    std::map<std::string, int> chokeId;   // tags that silence others -> SFZ group numbers
    for (auto &t : splitList(get(p.groups, "silencedbytags"))) chokeId.emplace(t, 0);
    for (auto &g : p.group) {
        for (auto &t : splitList(get(g.a, "silencedbytags"))) chokeId.emplace(t, 0);
        for (auto &s : g.samples) for (auto &t : splitList(get(s, "silencedbytags"))) chokeId.emplace(t, 0);
    }
    { int n = 0; for (auto &[t, id] : chokeId) id = ++n; }
    std::set<std::string> missing;
    std::vector<std::map<std::string, std::string>> regions;
    int maxSeq = 0;
    size_t disabledGroups = 0, silentGroups = 0;
    for (size_t gi = 0; gi < p.group.size(); ++gi) {
        const Group &g = p.group[gi];
        out.total += g.samples.size();
        if (attr(g.a, "enabled") && isFalse(get(g.a, "enabled"))) { ++disabledGroups; continue; }
        // the group's own effects: its first filter plays per voice, gains add, the rest are left out
        std::unique_ptr<RegionFilter> gFilter;
        double gFxDb = 0;
        for (size_t ei = 0; ei < g.fx.size(); ++ei) {
            const Attrs &e = g.fx[ei];
            const std::string type = lower(trim(get(e, "type")));
            if (attr(e, "enabled") && isFalse(get(e, "enabled"))) continue;
            if (isFilter(type)) {
                const double f = std::max(0.0, num(e, "frequency", 22000));
                const bool low = type.rfind("lowpass", 0) == 0, high = type == "highpass" || type == "hipass", vel = p.velFilterGroupFx.count({gi, ei}) > 0;
                if (!vel && ((low && f >= 20000) || (high && f <= 20))) continue;
                if (gFilter) { p.note("a group's second filter left out"); continue; }
                gFilter = std::make_unique<RegionFilter>(RegionFilter{low ? "lpf_2p" : high ? "hpf_2p" : "bpf_2p", f, type == "lowpass_1pl" ? 0.5 : std::clamp(num(e, "resonance", 0.7), 0.05, 18.0), vel});
                if (vel) p.note("a group's " + type + " filter opens with velocity from " + fmt(std::round(f)) + " Hz to 22 kHz (an approximation of its velocity binding)");
            } else if (type == "gain") {
                gFxDb += lower(get(e, "levelunit")) == "linear" ? linDb(num(e, "level", 0.5)) : dbValue(get(e, "level"), -6);
            } else if (!type.empty()) p.note("group " + type + " effects left out");
        }
        const RegionFilter *filter = gFilter ? gFilter.get() : instFilter.get();
        if (gFilter && instFilter) p.note("a group's own filter replaces the instrument's velocity filter for its samples");
        const std::vector<std::string> groupTags = splitList(get(g.a, "tags"));
        const double groupDb = volumeDb(get(g.a, "groupvolume", get(g.a, "volume"))) + volumeDb(get(g.a, "modvolume"), 0) +
                               volumeDb(get(p.groups, "globalvolume", get(p.groups, "volume"))) + volumeDb(get(p.groups, "modvolume"), 0) + gFxDb;
        if (groupDb < -150) { ++silentGroups; continue; }
        // one voice: a <sample>, or an <oscillator> as an SFZ generator
        auto voice = [&](const Attrs &s, const std::string &file) {
            auto inh = [&](const char *k) -> const std::string * {   // the sample's own, else its group's, else the instrument's
                if (auto *v = attr(s, k)) return v;
                if (auto *v = attr(g.a, k)) return v;
                return attr(p.groups, k);
            };
            auto inhNum = [&](const char *k, double def) { auto *v = inh(k); return v ? toNum(*v, def) : def; };
            std::vector<std::string> tags = groupTags;
            for (auto &t : splitList(get(s, "tags"))) if (std::find(tags.begin(), tags.end(), t) == tags.end()) tags.push_back(t);
            bool on = true;
            const double tDb = tagGain(tags, on);
            const double db = groupDb + tDb + volumeDb(get(s, "volume"));
            if (!on || db < -150) return;
            std::map<std::string, std::string> r;
            r["sample"] = file;
            auto key = [&](const char *k, int def) {
                auto *v = inh(k);
                if (!v) return def;
                const std::string t = trim(*v);
                const int n = !t.empty() && (std::isdigit((unsigned char)t[0]) || t[0] == '-') ? (int)std::lround(toNum(t, def)) : sfzNoteNumber(t);
                return n < 0 && t != "-1" ? def : n;
            };
            r["lokey"] = std::to_string(std::clamp(key("lonote", 0), 0, 127));
            r["hikey"] = std::to_string(std::clamp(key("hinote", 127), 0, 127));
            r["pitch_keycenter"] = std::to_string(key("rootnote", 60));
            r["lovel"] = std::to_string(std::clamp((int)std::lround(inhNum("lovel", 0)), 0, 127));
            r["hivel"] = std::to_string(std::clamp((int)std::lround(inhNum("hivel", 127)), 0, 127));
            const std::string *tun = attr(s, "tuning") ? attr(s, "tuning") : attr(s, "tunning") ? attr(s, "tunning") : inh("tuning");
            const double semis = (tun ? toNum(*tun, 0) : 0) + num(g.a, "grouptuning", 0) + num(p.groups, "globaltuning", 0);
            if (std::fabs(semis) > 1e-6) r["tune"] = fmt(100 * semis);
            if (std::fabs(db) > 1e-6) r["volume"] = fmt(db);
            const double pan = std::clamp(inhNum("pan", 0) + num(g.a, "grouppan", 0) + num(p.groups, "globalpan", 0), -100.0, 100.0);
            if (std::fabs(pan) > 1e-6) r["pan"] = fmt(pan);
            if (file[0] != '*') {
                if (inh("start") && inhNum("start", 0) > 0) r["offset"] = fmt(std::floor(inhNum("start", 0)));
                if (inh("end") && inhNum("end", 0) > 0) r["end"] = fmt(std::floor(inhNum("end", 0)));
            }
            // ampEnvEnabled=false: a one-shot, to the end of the file whatever the note length
            const bool oneShot = inh("ampenvenabled") && isFalse(*inh("ampenvenabled"));
            const std::string *le = inh("loopenabled");
            if (file[0] != '*') {   // loops: when the preset says nothing, the file's own (as SFZ and DecentSampler do)
                const double ls = std::max(0.0, inhNum("loopstart", 0)), lend = inhNum("loopend", -1);
                if (le && isFalse(*le)) r["loop_mode"] = oneShot ? "one_shot" : "no_loop";
                else if (oneShot && !le) r["loop_mode"] = "one_shot";
                else {
                    if (le && isTrue(*le)) r["loop_mode"] = "loop_continuous";
                    if (inh("loopend") && lend > ls) { r["loop_start"] = fmt(std::floor(ls)); r["loop_end"] = fmt(std::floor(lend)); }
                    const double xf = inhNum("loopcrossfade", 0);
                    if (xf > 0) r["wl_loop_crossfade_frames"] = fmt(std::floor(xf));
                }
            }
            if (!oneShot) {   // the amplitude envelope (DecentSampler's defaults: no attack or decay, full sustain, 0.5 s release)
                r["ampeg_attack"] = fmt(std::max(0.0, inhNum("attack", 0)));
                r["ampeg_decay"] = fmt(std::max(0.0, inhNum("decay", 0)));
                r["ampeg_sustain"] = fmt(100 * std::clamp(inhNum("sustain", 1), 0.0, 1.0));
                r["ampeg_release"] = fmt(std::max(0.0, inhNum("release", 0.5)));
                r["wl_attack_curve"] = fmt(curveOr(inh("attackcurve"), -100));
                r["wl_decay_curve"] = fmt(curveOr(inh("decaycurve"), 100));
                r["wl_release_curve"] = fmt(curveOr(inh("releasecurve"), 100));
            }
            // velocity: gain 1 - t + t x velocity / 127, times velocity / 127 again when a velocity binding drives the volume
            const double vt = std::clamp(inhNum("ampveltrack", 1), 0.0, 1.0);
            const bool velVol = p.velAmp || p.velAmpGroups.count(gi);
            if (velVol)
                for (int v = 0; v <= 127; v += v == 120 ? 7 : 8) r["amp_velcurve_" + std::to_string(v)] = fmt((1 - vt + vt * v / 127.0) * v / 127.0);
            else if (vt <= 0) r["amp_veltrack"] = "0";
            else { r["amp_velcurve_0"] = fmt(1 - vt); r["amp_velcurve_127"] = "1"; }
            if (inh("pitchkeytrack")) r["pitch_keytrack"] = fmt(100 * std::clamp(inhNum("pitchkeytrack", 1), 0.0, 1.0));
            // round robins: every sample at the chosen position plays; with no seqMode ("always") they all play
            const std::string mode = lower(trim(inh("seqmode") ? *inh("seqmode") : ""));
            if (mode == "round_robin" || mode == "random" || mode == "true_random") {
                const int pos = std::max(1, (int)std::lround(inhNum("seqposition", 1)));
                r["seq_position"] = std::to_string(pos);
                maxSeq = std::max(maxSeq, pos);
                if (mode != "round_robin") p.note("random round robins cycle in order");
            }
            const std::string trig = lower(trim(inh("trigger") ? *inh("trigger") : "attack"));
            if (trig == "release") {
                r["trigger"] = "release";
                if (auto *d = inh("releasetriggerdecay")) {
                    const std::string t = lower(trim(*d));
                    const double dbs = t.find("db") != std::string::npos ? std::fabs(toNum(t.substr(0, t.find("db")), 0)) : -linDb(std::clamp(toNum(t, 1), 1e-4, 1.0));
                    if (dbs > 0) r["rt_decay"] = fmt(dbs);
                }
            } else if (trig == "first" || trig == "legato") p.note("samples triggered on '" + trig + "' play on every note");
            // choke groups: a voice whose silencedByTags names a tag stops when a voice with that tag starts
            if (auto *v = inh("silencedbytags")) { const auto l = splitList(*v); if (!l.empty()) r["off_by"] = std::to_string(chokeId[l[0]]); }
            for (auto &t : tags) if (chokeId.count(t)) { r["group"] = std::to_string(chokeId[t]); break; }
            if (r.count("off_by") && lower(trim(inh("silencingmode") ? *inh("silencingmode") : "fast")) == "normal") p.note("voices silenced by tags stop quickly instead of releasing");
            // controllers at rest: samples for a controller range (pedal down) stay out, as in SFZ; loCC64 -> locc64,
            // onLoCC64 (played by the controller) -> on_locc64
            auto ccKey = [](const std::string &k, const char *prefix) {
                const size_t n = std::strlen(prefix);
                return k.size() > n && k.compare(0, n, prefix) == 0 && std::all_of(k.begin() + (long)n, k.end(), [](char c) { return std::isdigit((unsigned char)c); });
            };
            for (const Attrs *a : {(const Attrs *)&p.groups, &g.a, &s})
                for (auto &[k, v] : *a) {
                    if (ccKey(k, "locc") || ccKey(k, "hicc")) r[k] = trim(v);
                    else if (ccKey(k, "onlocc") || ccKey(k, "onhicc")) r["on_" + k.substr(2)] = trim(v);
                }
            if (auto *d = inh("delay")) {
                const std::string unit = lower(trim(inh("delayunit") ? *inh("delayunit") : "seconds"));
                if (unit == "seconds") { if (toNum(*d, 0) > 0) r["delay"] = fmt(toNum(*d, 0)); }
                else if (toNum(*d, 0) > 0) p.note("sample delays in " + unit + " left out");
            }
            if (inh("glidetime") && inhNum("glidetime", 0) > 0) p.note("glide left out");
            if (filter) {
                r["fil_type"] = filter->type;
                r["cutoff"] = fmt(std::max(filter->vel ? 20.0 : 5.0, filter->freq));
                r["resonance"] = fmt(20 * std::log10(filter->q / 0.7071));
                if (filter->vel) r["fil_veltrack"] = fmt(std::max(0.0, 1200 * std::log2(22000 / std::max(20.0, filter->freq))));
            }
            for (int o = 1; o <= 16; ++o)   // outputs other than the main one
                if (auto *t = attr(s, ("output" + std::to_string(o) + "target").c_str()); t && lower(*t) != "main_output" && lower(*t) != "no_output") { p.note("auxiliary outputs and buses left out"); break; }
            regions.push_back(std::move(r));
        };
        for (auto &s : g.samples) {
            const std::string raw = get(s, "path");
            const std::string file = resolveSample(src, raw, samplePath);
            if (file.empty()) { if (!raw.empty()) missing.insert(raw); continue; }
            voice(s, file);
        }
        for (auto &o : g.oscillators) {
            const std::string w = lower(trim(get(o, "waveform", get(o, "shape", get(g.a, "waveform", "sine")))));
            const std::string gen = w == "white_noise" ? "noise" : w;
            if (gen != "sine" && gen != "saw" && gen != "square" && gen != "triangle" && gen != "noise") { p.note("its " + w + " oscillator left out"); continue; }
            Attrs s = o;
            s.erase("tags");
            if (!attr(s, "rootnote") && !attr(g.a, "rootnote")) s["rootnote"] = "60";
            voice(s, "*" + gen);
        }
    }
    if (maxSeq > 1)
        for (auto &r : regions) if (r.count("seq_position")) r["seq_length"] = std::to_string(maxSeq);
    for (auto &r : regions) if (r.count("seq_position") && maxSeq <= 1) r.erase("seq_position");
    if (!missing.empty())
        p.note(std::to_string(missing.size()) + " sample file(s) not found, left out (" + *missing.begin() + (missing.size() > 1 ? ", ..." : "") + ")");
    if (disabledGroups) p.note(std::to_string(disabledGroups) + " group(s) switched off at load (another menu option, button state or articulation) left out");
    for (auto &r : regions) {
        SfzRegion rg;
        rg.op = std::move(r);
        out.sfz.regions.push_back(std::move(rg));
    }
    out.sfz.dir = "";
    out.output = json::array({{{"type", "compressor"}, {"threshold", -6}, {"ratio", 4}, {"attack", 2}, {"release", 200}, {"knee", 0}, {"intended", true}},
                              {{"type", "limiter"}, {"ceiling", 0}, {"release", 100}, {"lookahead", 0.5}, {"truePeak", false}}});
    out.samples = out.sfz.regions.size();
    out.notes = p.notes;
    if (out.sfz.regions.empty()) {
        err = out.name + ".dspreset: no sample plays";
        if (!missing.empty()) err += " (" + std::to_string(missing.size()) + " sample file(s) missing, e.g. " + *missing.begin() + ")";
        else if (disabledGroups || silentGroups) err += " (every group is switched off or silent at load)";
        else if (out.total == 0) err += " (it has no <sample>)";
        return false;
    }
    return true;
}

} // namespace wl
