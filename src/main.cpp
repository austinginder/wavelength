// Wavelength, a headless music engine for AI agents.
//
//   wavelength plugins [--rescan] [--json]
//   wavelength presets <plugin> [--search TEXT] [--json]
//   wavelength samples [--search TEXT] [--kit NAME] [--json]
//   wavelength params <plugin> [--state FILE] [--format F] [--all] [--json]
//   wavelength render <job.json> [--out DIR] [--json] [--verbose]
//   wavelength state save <plugin> --out FILE.clap-preset [--state FILE] [--set "Name=value"]...
//   wavelength import <project.dawproject> [--out DIR] [--bitwig FILE.bwproject | none] [--json]
//   wavelength import <project.bwproject> [--out DIR] [--list] [--json]
//   wavelength lint <job.json> [--tracks "A,B,C"] [--low "B"] [--crossings] [--json]
//   wavelength lint <job.json> --harmony [--key K] [--ignore "A,B"] [--chords] [--max-bars N] [--json]
//   wavelength save | history | undo | redo | restore | diff | comments [song] (song_cli.cpp)
//   wavelength pack | unpack | validate (song_cli.cpp, package.cpp)
//   wavelength serve [SONGS_DIR] [--port 7400] [--host 127.0.0.1] [--open] [--ui DIR]
//   wavelength version [--check] | upgrade [--check] [--force] (self_upgrade.cpp)
#include "analyze.hpp"
#include "audio_file.hpp"
#include "clips.hpp"
#include "apple_loops.hpp"
#include "arp.hpp"
#include "logic_patches.hpp"
#include "harmony.hpp"
#include "serve.hpp"
#include "help.hpp"
#include "self_upgrade.hpp"
#include "term.hpp"
#include "sf2.hpp"
#include "audition.hpp"
#include "catalog.hpp"
#include "compat.hpp"
#include "instance.hpp"
#include "platform.hpp"
#include "plugin.hpp"
#include "engine.hpp"
#include "effects.hpp"
#include "loudness.hpp"
#include "presets.hpp"
#include "preset_files.hpp"
#include "sampler.hpp"
#include "synth.hpp"
#include "bitwig.hpp"
#include "dawproject.hpp"
#include "midi_file.hpp"
#include "musicxml.hpp"
#include "vst2_plugin.hpp"
#include "vst3_plugin.hpp"
#include "job.hpp"
#include "card.hpp"
#include "stage.hpp"
#include "picture.hpp"
#include "docs.hpp"
#include "fallback.hpp"
#include "kit.hpp"
#include "history.hpp"
#include "package.hpp"
#include "song.hpp"
#include "song_cli.hpp"
#include "mcp.hpp"
#include "render.hpp"
#include "state_file.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace wl;

namespace {

// Plugins print debug text to stdout. Wavelength's own output goes to a private copy of
// the original stdout, and fd 1 is redirected to stderr before any plugin loads.
FILE *OUT = stdout;
void emit(const std::string &s) { std::fputs(s.c_str(), OUT); std::fputc('\n', OUT); std::fflush(OUT); }


struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;
    std::vector<std::string> sets;
    std::vector<std::string> missing;   // options given without their value
    bool has(const std::string &k) const { return opts.count(k) > 0; }
    std::string get(const std::string &k, const std::string &d = "") const { auto it = opts.find(k); return it == opts.end() ? d : it->second; }
};

Args parse(int argc, char **argv) {
    static const std::vector<std::string> flags = {"--json", "--rescan", "--verbose", "--all", "--roundrobin", "--rebuild", "--retag", "--song-time", "--crossings", "--harmony", "--chords", "--peaks", "--open", "--install-soundfont", "--force", "--no-print", "--png", "--no-png", "--loop", "--keep", "--fallbacks", "--cache", "--check", "--list", "--help", "--apply", "--dry-run", "--midi"};
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            if (std::find(flags.begin(), flags.end(), s) != flags.end()) a.opts[s] = "1";
            else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                if (s == "--set") a.sets.push_back(argv[++i]);
                else a.opts[s] = argv[++i];
            } else { a.opts[s] = ""; a.missing.push_back(s); }   // "--end --json": --end has no value
        } else a.positional.push_back(s);
    }
    return a;
}

// a column on a terminal: cut to `w` characters and padded, so styling it keeps the table aligned
std::string col(const std::string &s, size_t w) {
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (((unsigned char)s[i] & 0xC0) != 0x80 && ++chars > w) break;
        out += s[i];
    }
    return term::pad(out, w);
}

// ---- render, on a terminal: a table, sections, warnings wrapped to the window, a closing line ----
std::string mmss(double s) {
    char b[24];
    std::snprintf(b, sizeof b, "%d:%02d", (int)s / 60, (int)s % 60);
    return b;
}

void printWrapped(const std::string &lead, const std::string &text, size_t indent) {
    const size_t room = (size_t)std::max(40, term::width() - 1) - indent;
    std::string line, word;
    bool first = true;
    auto flush = [&] { std::fprintf(OUT, "%s%s\n", first ? lead.c_str() : std::string(indent, ' ').c_str(), line.c_str()); line.clear(); first = false; };
    std::istringstream words(text);
    while (words >> word) {
        if (!line.empty() && term::visibleWidth(line) + 1 + term::visibleWidth(word) > room) flush();
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty() || first) flush();
}

// a path as short as it can be said: relative to the current folder, else from ~
std::string shortPath(const std::string &p) {
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::u8path(p), ec).lexically_normal();
    const fs::path rel = abs.lexically_relative(fs::current_path(ec));
    if (!rel.empty() && *rel.begin() != "..") return rel.generic_u8string();
    const fs::path home = platform::homeDir(), fromHome = abs.lexically_relative(home);
    if (!fromHome.empty() && *fromHome.begin() != "..") return "~/" + fromHome.generic_u8string();
    return p;
}

int printRenderSummary(const RenderResult &r, int songRev, bool complete, const std::string &incomplete) {
    const term::Style &st = term::out();
    size_t nameW = 5, plugW = 10;
    for (auto &t : r.tracks) { nameW = std::max(nameW, term::visibleWidth(t.name)); plugW = std::max(plugW, term::visibleWidth(t.pluginName)); }
    for (auto &b : r.buses) nameW = std::max(nameW, term::visibleWidth(b.name) + 4);
    nameW = std::min<size_t>(nameW, 28);
    plugW = std::min<size_t>(plugW, 26);
    auto cut = [](const std::string &s, size_t w) { return term::visibleWidth(s) <= w ? s : s.substr(0, w - 1) + "\u2026"; };
    auto num = [](double v, const char *unit) { char b[32]; std::snprintf(b, sizeof b, "%6.1f %s", v, unit); return std::string(b); };
    std::fprintf(OUT, "\n  %s  %s  %s  %s\n", term::pad(st.dim("Track"), nameW).c_str(), term::pad(st.dim("Sound"), plugW).c_str(),
                 term::pad(st.dim("   Peak"), 9).c_str(), st.dim("  Loudness").c_str());
    for (auto &t : r.tracks) {
        const bool silent = t.lufs < -70;
        std::fprintf(OUT, "  %s  %s  %s  %s\n", term::pad(silent ? st.red(cut(t.name, nameW)) : cut(t.name, nameW), nameW).c_str(),
                     term::pad(st.dim(cut(t.pluginName, plugW)), plugW).c_str(), num(t.levels.peakDb, "dB").c_str(),
                     silent ? st.red("  silent").c_str() : num(t.lufs, "LUFS").c_str());
        for (auto &w : t.warnings) printWrapped("    " + st.warn() + " ", w, 6);
    }
    for (auto &b : r.buses)
        std::fprintf(OUT, "  %s  %s  %s  %s\n", term::pad(st.dim("bus ") + cut(b.name, nameW - 4), nameW).c_str(), std::string(plugW, ' ').c_str(),
                     num(b.levels.peakDb, "dB").c_str(), num(b.lufs, "LUFS").c_str());
    std::string rule;
    for (size_t i = 0; i < nameW + plugW + 29; ++i) rule += "\u2500";
    std::fprintf(OUT, "  %s\n", st.dim(rule).c_str());
    const std::string tp = num(r.truePeakDb, "dBTP");
    std::fprintf(OUT, "  %s  %s  %s  %s\n", term::pad(st.bold("Mix"), nameW).c_str(), term::pad(st.dim("LRA " + num(r.mixLra, "LU").substr(1)), plugW).c_str(),
                 r.truePeakDb > 0 ? st.red(tp).c_str() : r.truePeakDb > -1 ? st.yellow(tp).c_str() : tp.c_str(), st.bold(num(r.mixLufs, "LUFS")).c_str());
    for (auto &d : r.deliveries)
        std::fprintf(OUT, "  %s  %s  %s  %s   %s\n", term::pad(d.spec.format, nameW).c_str(), std::string(plugW, ' ').c_str(), num(d.truePeakDb, "dBTP").c_str(),
                     num(d.lufs, "LUFS").c_str(), st.dim(shortPath(d.file)).c_str());
    if (!r.sections.empty()) {
        std::fprintf(OUT, "\n  %s\n", st.bold("Sections").c_str());
        size_t secW = 8;
        for (auto &sec : r.sections) secW = std::max(secW, term::visibleWidth(sec.name));
        for (auto &sec : r.sections)
            std::fprintf(OUT, "  %s  %s  %s\n", term::pad(sec.name, std::min<size_t>(secW, 28)).c_str(), num(sec.lufs, "LUFS").c_str(),
                         st.dim(mmss(sec.start) + "-" + mmss(sec.end)).c_str());
    }
    if (!r.warnings.empty()) {
        std::fprintf(OUT, "\n");
        for (auto &w : r.warnings) printWrapped("  " + st.warn() + " ", w, 4);
    }
    std::fprintf(OUT, "\n");
    if (!complete) {
        printWrapped(st.fail() + " ", incomplete, 2);
        return 1;
    }
    std::string done = st.ok() + " " + st.bold("Rendered " + mmss(r.seconds)) + " in " + num(r.renderSeconds, "s").substr(num(r.renderSeconds, "s").find_first_not_of(' ')) + "  " + st.arrow() + " " + shortPath(r.mixFile);
    std::fprintf(OUT, "%s\n", done.c_str());
    std::string extra;
    if (!r.pictureFile.empty()) extra += "picture " + shortPath(r.pictureFile);
    if (songRev) extra += (extra.empty() ? "" : "  " + st.dot() + "  ") + "song revision " + std::to_string(songRev);
    if (!extra.empty()) std::fprintf(OUT, "  %s\n", st.dim(extra).c_str());
    return 0;
}

int fail(const Args &a, const std::string &msg) {
    if (a.has("--json")) emit(json{{"ok", false}, {"error", msg}}.dump(2, ' ', false, json::error_handler_t::replace));
    else if (term::err().on) std::fprintf(stderr, "%s %s %s\n", term::err().fail().c_str(), term::err().red(term::err().bold("error:")).c_str(), msg.c_str());
    else std::fprintf(stderr, "error: %s\n", msg.c_str());
    return 1;
}

json levelsJson(const Levels &l) {
    auto r = [](double v) { return std::round(v * 10) / 10; };
    return {{"peakDb", r(l.peakDb)}, {"rmsDb", r(l.rmsDb)}, {"activeRmsDb", r(l.activeRmsDb)}, {"silent", l.silent}};
}

// ---- plugins ---------------------------------------------------------------------------
int cmdPlugins(const Args &a) {
    for (const char *flag : {"--block", "--unblock"}) {
        if (!a.has(flag)) continue;
        const bool block = std::string(flag) == "--block";
        PluginInfo info;
        std::string err;
        if (!resolvePlugin(a.get(flag), info, err, true)) return fail(a, err);
        if (!setPluginBlocked(info, block, a.get("--reason"), err)) return fail(a, err);
        if (a.has("--json")) emit(json{{"ok", true}, {"plugin", info.id}, {"name", info.name}, {"blocked", block}}.dump(2));
        else std::fprintf(OUT, "%s %s (%s %s)\n", block ? "blocked" : "unblocked", info.name.c_str(), info.format.c_str(), info.id.c_str());
        return 0;
    }
    const json blocked = blockedPlugins();
    std::vector<std::string> warnings;
    auto all = scanPlugins(a.has("--rescan"), warnings);
    auto builtin = [](const char *id, const char *name, const char *desc, std::vector<std::string> features) {
        PluginInfo p;
        p.id = id; p.name = name; p.vendor = "Wavelength"; p.version = WAVELENGTH_VERSION; p.description = desc;
        p.format = "builtin"; p.features = std::move(features);
        return p;
    };
    all.push_back(builtin("builtin:synth", "Synth (built-in)", "Virtual-analog polysynth: named patches for bass, leads, pads, plucks, keys, brass (`wavelength presets builtin:synth`); needs no plugin", {"instrument", "synthesizer"}));
    all.push_back(builtin("builtin:drums", "Drums (built-in)", "GM kit: 36 kick, 38 snare, 37 rim, 42/46 hats, 49 crash, 51 ride, 41/45/48 toms", {"instrument", "drum"}));
    all.push_back(builtin("builtin:sampler", "Sampler (built-in)", "Bitwig .multisample instruments, WAV drum kits and single samples (see `wavelength samples`)", {"instrument", "sampler"}));
    all.push_back(builtin("builtin:audio", "Audio clips (built-in)", "WAV files placed in beats, tempo-fitted with pitch-preserving stretch, transposed, reversed, trimmed", {"instrument", "audio"}));
    all.push_back(builtin("builtin:fx", "FX (built-in)", "48 impact, 50 riser (note length), 52 reverse swell (ends with the note), 53 sub drop, 55/57 Shepard rise/fall (note length)", {"instrument"}));
    all.push_back(builtin("builtin:shepard", "Shepard-Risset glissando (built-in)", "Endless rising or falling glissando for each note's length: rate, direction, centre, width", {"instrument"}));
    if (a.has("--json")) {
        json list = json::array();
        for (auto &p : all)
            list.push_back({{"id", p.id}, {"name", p.name}, {"vendor", p.vendor}, {"version", p.version}, {"format", p.format},
                            {"features", p.features}, {"bundle", p.bundlePath}, {"blocked", blocked.contains(p.id)},
                            {"arch", p.arch.empty() ? platform::hostArch() : p.arch}});
        emit(json{{"ok", true}, {"plugins", list}, {"warnings", warnings}}.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    for (auto &p : all) {
        bool instrument = std::find(p.features.begin(), p.features.end(), "instrument") != p.features.end();
        const term::Style &st = term::out();
        if (st.on) {
            const std::string f = col(p.format, 7);
            const std::string format = p.format == "clap" ? st.cyan(f) : p.format == "vst3" ? st.blue(f) : p.format == "vst2" ? st.yellow(f) : st.green(f);
            std::fprintf(OUT, "%s %s %s %s %s%s%s\n", format.c_str(), st.bold(col(p.name, 32)).c_str(), st.dim(col(p.id, 30)).c_str(),
                         st.dim(col(p.vendor, 22)).c_str(), instrument ? "instrument" : st.dim("effect").c_str(),
                         p.arch.empty() ? "" : st.yellow("  (" + p.arch + ", Rosetta)").c_str(), blocked.contains(p.id) ? st.red("  BLOCKED").c_str() : "");
            continue;
        }
        std::fprintf(OUT, "%-7s %-32.32s %-30.30s %-22.22s %s\n", p.format.c_str(), p.name.c_str(), p.id.c_str(), p.vendor.c_str(),
                     (std::string(instrument ? "instrument" : "effect") + (p.arch.empty() ? "" : "  (" + p.arch + ", Rosetta)") +
                      (blocked.contains(p.id) ? "  BLOCKED" : "")).c_str());
    }
    for (auto &w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    if (term::out().on)
        std::fprintf(OUT, "\n%s %s\n", term::out().bold(std::to_string(all.size()) + " plugins").c_str(),
                     term::out().dim("(CLAP, VST3, VST2 and built-in). Use \"clap:Name\", \"vst3:Name\" or \"vst2:Name\" when a name exists in more than one format.").c_str());
    else std::fprintf(OUT, "\n%zu plugins (CLAP, VST3, VST2 and built-in). Use \"clap:Name\", \"vst3:Name\" or \"vst2:Name\" when a name exists in more than one format.\n", all.size());
    return 0;
}

// load a plugin (+ optional state) for inspection on the main thread
std::unique_ptr<Plugin> openForInspection(const Args &a, const std::string &spec, PluginInfo &info, std::string &err) {
    if (!resolvePlugin(spec, info, err)) return nullptr;
    auto plugin = createPlugin(info, err);
    if (!plugin) return nullptr;
    plugin->verbose = a.has("--verbose");
    if (a.has("--preset")) {
        std::string loaded, fmt;
        if (!loadPresetByName(*plugin, info, a.get("--preset"), loaded, fmt, err)) return nullptr;
    }
    if (a.has("--state")) {
        StateFile sf;
        if (!readStateFile(a.get("--state"), a.get("--format", "auto"), sf, err)) return nullptr;
        if (!loadStateInto(*plugin, sf, err)) return nullptr;
    }
    plugin->pump(info.format == "vst3" ? 500 : 150);
    if (info.format == "vst3") {   // some plugins (Surge XT) only publish real values after processing a few blocks
        auto ps = plugin->params();
        if (!ps.empty()) {
            std::string e2;
            plugin->commitParams({{ps.front().id, ps.front().cookie, ps.front().value}}, 48000, 512, e2);
        }
    }
    return plugin;
}

// ---- presets ---------------------------------------------------------------------------
int cmdPresets(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength presets <plugin> [--search TEXT]");
    if (a.positional[1] == "arp") {   // GarageBand's and Logic's Arpeggiator presets, and the patches that arpeggiate
        std::string q = a.get("--search");
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        json list = json::array();
        auto add = [&](const std::string &name, const std::string &kind, const json &arp) {
            std::string hay = kind + " " + name + " " + arpSummary(arp);
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (!q.empty() && hay.find(q) == std::string::npos) return;
            if (a.has("--json")) list.push_back({{"name", name}, {"kind", kind}, {"arp", arp}});
            else std::fprintf(OUT, "%-7s %-30s %s\n", kind.c_str(), name.c_str(), arpSummary(arp).c_str());
        };
        std::vector<std::string> ignored;
        std::string e2;
        for (auto &[name, path] : arpeggiatorPresets()) {
            json arp;
            if (appleArpeggiator(name, true, arp, ignored, e2)) add(name, "preset", arp);
        }
        for (auto &p : logicPatches()) {
            json arp;
            if (p.arpeggiator && appleArpeggiator(p.path, false, arp, ignored, e2)) add(p.name, "patch", arp);
        }
        if (a.has("--json")) emit(json{{"ok", true}, {"plugin", "arp"}, {"presets", list}}.dump(2));
        else std::fprintf(OUT, "\nOn any track: \"arp\": \"<name>\" (or {\"preset\": \"<name>\"} / {\"patch\": \"<name>\"} with settings on top).\n"
                               "A patch's own Arpeggiator plays by itself when the track uses the patch.\n");
        return 0;
    }
    if (a.positional[1] == "builtin:synth") {   // the synth's own patches
        std::string q = a.get("--search");
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        json list = json::array();
        const auto patches = synthPatches();
        size_t shown = 0;
        for (const auto &p : patches) {
            std::string hay = p.category + " " + p.name + " " + p.description;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (!q.empty() && hay.find(q) == std::string::npos) continue;
            ++shown;
            if (a.has("--json")) list.push_back({{"name", p.name}, {"category", p.category}, {"description", p.description}});
            else if (term::out().on) std::fprintf(OUT, "%s %s %s\n", term::out().dim(col(p.category, 8)).c_str(), term::out().bold(col(p.name, 20)).c_str(), p.description.c_str());
            else std::fprintf(OUT, "%-8s %-20s %s\n", p.category.c_str(), p.name.c_str(), p.description.c_str());
        }
        if (a.has("--json")) emit(json{{"ok", true}, {"plugin", "builtin:synth"}, {"presets", list}}.dump(2));
        else std::fprintf(OUT, "\n%zu of %zu patches. Use them as \"preset\": \"<name>\"; a \"synth\" object on the track changes any part of the patch.\n", shown, patches.size());
        return 0;
    }
    PluginInfo info;
    std::string err;
    if (!resolvePlugin(a.positional[1], info, err)) return fail(a, err);
    std::vector<PresetInfo> presets = listPresets(info, a.has("--rescan"), err);
    // sounds from the audition index: tags join the searchable text, measurements the JSON
    const json audition = auditionIndex(info);
    for (auto &p : presets)
        if (audition.contains(p.name) && audition[p.name].contains("tags"))
            for (auto &t : audition[p.name]["tags"]) p.features.push_back(t.get<std::string>());
    if (presets.empty()) return fail(a, info.name + " has no presets Wavelength can find (no preset discovery, program list or preset folder); load a state file");
    std::string q = a.get("--search");
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    auto matches = [&](const PresetInfo &p) {
        if (q.empty()) return true;
        std::string hay = p.category + " " + p.name + " " + p.description;
        for (auto &f : p.features) hay += " " + f;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        return hay.find(q) != std::string::npos;
    };
    json list = json::array();
    size_t shown = 0;
    for (const auto &p : presets) {
        if (!matches(p)) continue;
        ++shown;
        if (a.has("--json")) {
            json item = {{"name", p.name}, {"category", p.category}, {"description", p.description}, {"creator", p.creator}, {"features", p.features}};
            if (audition.contains(p.name)) item["audition"] = audition[p.name];
            list.push_back(item);
        } else {
            std::string tags;
            if (audition.contains(p.name) && audition[p.name].contains("tags"))
                for (auto &t : audition[p.name]["tags"]) tags += (tags.empty() ? "" : ", ") + t.get<std::string>();
            if (term::out().on) {
                const term::Style &st = term::out();
                std::fprintf(OUT, "%s %s%s%s\n", st.dim(col(p.category, 24)).c_str(), st.bold(p.name).c_str(),
                             p.description.empty() ? "" : st.dim("   " + p.description.substr(0, 90)).c_str(), tags.empty() ? "" : ("   " + st.cyan(tags)).c_str());
            } else
            std::fprintf(OUT, "%-24.24s %s%s%s%s\n", p.category.c_str(), p.name.c_str(), p.description.empty() ? "" : "   (",
                         p.description.empty() ? "" : (p.description.substr(0, 90) + ")").c_str(), tags.empty() ? "" : ("   [" + tags + "]").c_str());
        }
    }
    if (a.has("--json")) emit(json{{"ok", true}, {"plugin", info.id}, {"presets", list}}.dump(2, ' ', false, json::error_handler_t::replace));
    else std::fprintf(OUT, "\n%zu of %zu presets (%s). Use them in a job as \"preset\": \"<name>\".%s\n", shown, presets.size(), info.name.c_str(),
                      audition.empty() ? " Run `wavelength audition` to tag them by sound." : "");
    return 0;
}

// ---- loops -----------------------------------------------------------------------------
json appleLoopJson(const AppleLoop &l) {
    json j = {{"name", l.name}, {"file", "lib:Apple Loops/" + fs::u8path(l.path).filename().u8string()}, {"folder", l.folder},
              {"category", l.category}, {"subcategory", l.subcategory}, {"genre", l.genre}, {"descriptors", l.descriptors},
              {"beats", l.beats}, {"bpm", std::round(l.bpm * 100) / 100}, {"seconds", std::round(l.seconds * 1000) / 1000},
              {"timeSignature", l.timeSignature}, {"notes", l.midi}};
    if (!l.key.empty()) { j["key"] = l.key; j["scale"] = l.scale; }
    return j;
}

int cmdLoops(const Args &a) {
    std::string err;
    int tonic = -1;
    bool minor = false;
    if (a.has("--key") && !parseKeyName(a.get("--key"), tonic, minor, err)) return fail(a, "--key: " + err);
    if (a.has("--notes")) {   // the notes inside a software-instrument loop, in beats, ready for a track
        const std::string q = a.get("--notes");
        const AppleLoop *l = findAppleLoop(q);
        if (!l && fs::exists(fs::u8path(q))) l = appleLoopAt(q);
        if (!l) return fail(a, "no Apple Loop named '" + q + "' (run `wavelength loops --search <text>`)");
        MidiImport m;
        if (!importMidiFile(l->path, "", "", m, err)) return fail(a, err);
        int shift = 0;
        if (tonic >= 0) appleLoopShift(*l, tonic, minor, shift);
        json notes = json::array();
        for (auto &t : m.job["tracks"])
            for (auto n : t["notes"]) {
                if (shift && n.contains("key") && n["key"].is_number()) n["key"] = n["key"].get<int>() + shift;
                notes.push_back(n);
            }
        std::sort(notes.begin(), notes.end(), [](const json &x, const json &y) { return x["beat"].get<double>() < y["beat"].get<double>(); });
        // a Drummer loop's notes are a slice of a longer performance, starting a whole number of loops in: from beat 0
        if (!notes.empty() && l->beats > 0) {
            const double first = notes.front()["beat"].get<double>(), shift = std::floor((first + 0.25) / l->beats) * l->beats;   // a flam a 16th early still counts
            if (shift > 0) for (auto &n : notes) n["beat"] = std::max(0.0, std::round((n["beat"].get<double>() - shift) * 1e6) / 1e6);
        }
        if (a.has("--json")) { emit(json{{"ok", true}, {"loop", appleLoopJson(*l)}, {"shift", shift}, {"notes", notes}}.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
        double span = 0;
        for (auto &n : notes) span = std::max(span, n["beat"].get<double>() + n.value("dur", 0.0));
        const int beats = std::max(l->beats, (int)std::ceil(span - 1e-6));   // a Drummer loop's notes run past its audio
        std::fprintf(OUT, "%s: %zu notes over %d beats (%s%s%s)%s\n", l->name.c_str(), notes.size(), beats, l->key.empty() ? "no key" : l->key.c_str(),
                     l->scale.empty() ? "" : " ", l->scale.c_str(), shift ? (", moved " + std::string(shift > 0 ? "+" : "") + std::to_string(shift) + " to " + a.get("--key")).c_str() : "");
        std::string lines;
        for (size_t i = 0; i < notes.size(); ++i) lines += (i ? ",\n " : "[") + notes[i].dump();
        std::fprintf(OUT, "%s]\n", lines.empty() ? "[" : lines.c_str());
        return 0;
    }
    std::vector<std::string> words;
    {
        std::string q = a.get("--search"), w;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        for (char c : q + " ") { if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); } else w += c; }
    }
    const auto &loops = appleLoops();
    json list = json::array();
    size_t shown = 0;
    for (auto &l : loops) {
        if (a.has("--midi") && !l.midi) continue;
        std::string hay = l.name + " " + l.folder + " " + l.category + " " + l.subcategory + " " + l.genre + " " + l.key + " " + l.scale;
        for (auto &d : l.descriptors) hay += " " + d;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (!std::all_of(words.begin(), words.end(), [&](const std::string &w) { return hay.find(w) != std::string::npos; })) continue;
        ++shown;
        int shift = 0;
        const bool shifted = tonic >= 0 && appleLoopShift(l, tonic, minor, shift);
        if (a.has("--json")) {
            json j = appleLoopJson(l);
            if (shifted) j["shift"] = shift;
            list.push_back(j);
            continue;
        }
        const std::string key = l.key.empty() ? "-" : l.key + (l.scale == "minor" ? "m" : l.scale == "major" ? "" : " " + l.scale);
        const std::string bpm = l.beats > 0 && l.bpm > 0 ? (std::to_string((int)std::lround(l.bpm)) + " bpm") : "one-shot";
        const std::string extra = (l.midi ? "notes" : "") + std::string(shifted ? (std::string(l.midi ? ", " : "") + (shift > 0 ? "+" : "") + std::to_string(shift)) : "");
        const std::string cat = l.category + (l.subcategory.empty() ? "" : "/" + l.subcategory);
        if (term::out().on) {
            const term::Style &st = term::out();
            std::fprintf(OUT, "%s %s %s %s %s %s\n", st.bold(col(l.name, 34)).c_str(), st.dim(col(cat, 24)).c_str(), st.cyan(col(key, 9)).c_str(),
                         col(bpm, 9).c_str(), st.dim(col(std::to_string(l.beats) + " beats", 9)).c_str(), extra.c_str());
        } else std::fprintf(OUT, "%-34.34s %-24.24s %-9s %-9s %3d beats %s\n", l.name.c_str(), cat.c_str(), key.c_str(), bpm.c_str(), l.beats, extra.c_str());
    }
    if (a.has("--json")) { emit(json{{"ok", true}, {"roots", appleLoopRoots()}, {"loops", list}}.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
    if (loops.empty()) { std::fprintf(OUT, "No Apple Loops installed (GarageBand or Logic Pro installs them in /Library/Audio/Apple Loops).\n"); return 0; }
    std::fprintf(OUT, "\n%zu of %zu Apple Loops. Play one on a builtin:audio track: {\"file\": \"lib:Apple Loops/<name>.caf\", \"beat\": 0, \"repeat\": 4, \"key\": \"song\"}\n"
                      "(it follows the song's tempo by itself). Loops marked \"notes\" carry their notes: `wavelength loops --notes <name>`.%s\n",
                 shown, loops.size(), tonic >= 0 ? " The last column is the shift into --key." : "");
    return 0;
}

// ---- samples ---------------------------------------------------------------------------
int cmdSamples(const Args &a) {
    std::string err;
    if (a.has("--install-soundfont")) {   // MuseScore General (MIT, 40 MB): the General MIDI fallback for imports
        std::string path;
        if (!installSoundFont(a.has("--force"), path, err)) return fail(a, err);
        if (a.has("--json")) emit(json{{"ok", true}, {"soundfont", path}}.dump(2));
        else std::fprintf(OUT, "%s\nMIDI and MusicXML imports now fall back on it; use it in a job as \"sampler\": {\"soundfont\": \"MuseScore_General\", \"program\": 0}.\n", path.c_str());
        return 0;
    }
    if (a.has("--soundfont")) {   // a SoundFont's presets
        std::string path;
        if (!findSampleEntry("soundfont", a.get("--soundfont"), fs::current_path().string(), path, err)) return fail(a, err);
        SoundFont sf;
        if (!sf.open(path, err)) return fail(a, err);
        std::string q = a.get("--search");
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        json list = json::array();
        for (auto &p : sf.presets()) {
            std::string n = p.name;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (!q.empty() && n.find(q) == std::string::npos) continue;
            if (a.has("--json")) list.push_back({{"bank", p.bank}, {"program", p.program}, {"name", p.name}});
            else std::fprintf(OUT, "%3d:%-3d  %s\n", p.bank, p.program, p.name.c_str());
        }
        if (a.has("--json")) emit(json{{"ok", true}, {"soundfont", path}, {"presets", list}}.dump(2, ' ', false, json::error_handler_t::replace));
        else std::fprintf(OUT, "\n%s\nUse as \"sampler\": {\"soundfont\": \"%s\", \"program\": N} (\"bank\": 128 for drum kits) or \"preset\": \"<name>\".\n",
                          path.c_str(), fs::path(path).stem().string().c_str());
        return 0;
    }
    if (a.has("--patch")) {   // a GarageBand or Logic patch: its channels, what plays, its sends
        const json d = describePatch(a.get("--patch"), fs::current_path().string(), err);
        if (d.is_null()) return fail(a, err);
        if (a.has("--json")) { emit(json{{"ok", true}, {"patch", d}}.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
        std::fprintf(OUT, "%s\n%s\n", d["name"].get<std::string>().c_str(), d["path"].get<std::string>().c_str());
        for (auto &c : d["channels"]) {
            std::string what = c["instrument"].get<std::string>().empty() ? "(no instrument)" : c["instrument"].get<std::string>();
            if (c.contains("source")) what += ", " + c["source"].get<std::string>();
            if (c.contains("samples")) what += ", " + std::to_string(c["samples"]["installed"].get<size_t>()) + " of " + std::to_string(c["samples"]["total"].get<size_t>()) + " samples installed";
            std::string fx;
            for (auto &e : c["effects"]) fx += (fx.empty() ? "" : ", ") + e.get<std::string>();
            std::fprintf(OUT, "  %-14s %s%s\n", c["file"].get<std::string>().c_str(), what.c_str(), fx.empty() ? "" : ("\n                 effects: " + fx).c_str());
        }
        if (d.contains("kit")) std::fprintf(OUT, "  plays its kit \"%s\" (Ultrabeat's synthesis left out)\n", d["kit"].get<std::string>().c_str());
        for (auto &s : d["sends"])
            std::fprintf(OUT, "  send %-34s %6.1f dB%s\n", s["aux"].get<std::string>().c_str(), s["db"].get<double>(),
                         s.contains("ir") ? ("   room: \"" + s["ir"].get<std::string>() + "\"").c_str() : "");
        if (!d["effects"].empty()) std::fprintf(OUT, "  effects played as: %s\n", d["effects"].dump().c_str());
        for (auto &n : d["effectNotes"]) std::fprintf(OUT, "  ! %s\n", n.get<std::string>().c_str());
        if (d.contains("arp")) std::fprintf(OUT, "  arpeggiator: %s\n    played as \"arp\": %s\n", arpSummary(d["arp"]).c_str(), d["arp"].dump().c_str());
        if (!d["plays"].get<bool>()) { std::fprintf(OUT, "\nDoesn't play here: %s.\n", d["why"].get<std::string>().c_str()); return 0; }
        if (d.contains("synth")) {   // a synth patch, re-created on builtin:synth
            for (auto &n : d["synth"]["notes"]) std::fprintf(OUT, "  ~ %s\n", n.get<std::string>().c_str());
            std::fprintf(OUT, "\nPlay it: \"plugin\": \"builtin:synth\", \"preset\": \"%s\": %s re-created on the built-in synth, an approximation.\n",
                         d["name"].get<std::string>().c_str(), d["synth"]["instrument"].get<std::string>().c_str());
            return 0;
        }
        std::fprintf(OUT, "\nPlay it: \"plugin\": \"builtin:sampler\", \"sampler\": {\"patch\": \"%s\"} (\"effects\": false for the dry instrument).\n",
                     d["name"].get<std::string>().c_str());
        if (!d["sends"].empty())
            std::fprintf(OUT, "For its sends, a bus per room: {\"name\": \"Hall\", \"fx\": [{\"type\": \"convolve\", \"ir\": \"<room>\", \"mix\": 1}]} and \"sends\": {\"Hall\": <dB>}.\n");
        return 0;
    }
    if (a.has("--kit")) {
        std::vector<std::pair<int, std::string>> map;
        std::vector<std::string> unmapped;
        std::string dir;
        std::vector<std::string> extras;
        if (!kitMap(a.get("--kit"), fs::current_path().string(), map, unmapped, dir, err, a.has("--roundrobin"), &extras)) return fail(a, err);
        json list = json::array();
        for (auto &[k, f] : map) {
            const bool extra = std::find(extras.begin(), extras.end(), f) != extras.end();
            const bool guessed = !extra && std::find(unmapped.begin(), unmapped.end(), f) != unmapped.end();
            const std::string file = fs::path(f).filename().string();
            if (a.has("--json")) list.push_back({{"key", k}, {"file", file}, {"recognised", !guessed}, {"extraTake", extra}});
            else std::fprintf(OUT, "%3d  %s%s\n", k, file.c_str(), extra ? "   (another take: --roundrobin stacks takes on one key)" : guessed ? "   (unrecognised name: next free key)" : "");
        }
        if (a.has("--json")) emit(json{{"ok", true}, {"kit", dir}, {"map", list}}.dump(2, ' ', false, json::error_handler_t::replace));
        else std::fprintf(OUT, "\n%s\nUse as \"sampler\": {\"kit\": \"%s\"}; override keys with \"map\": {\"36\": \"<file>\"}.\n",
                          dir.c_str(), fs::path(dir).filename().string().c_str());
        return 0;
    }
    std::string q = a.get("--search");
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    json list = json::array();
    size_t shown = 0;
    std::vector<SampleLibraryEntry> lib = sampleLibrary();
    lib.insert(lib.end(), patchLibrary().begin(), patchLibrary().end());
    lib.insert(lib.end(), impulseLibrary().begin(), impulseLibrary().end());
    std::stable_sort(lib.begin(), lib.end(), [](const SampleLibraryEntry &x, const SampleLibraryEntry &y) { return x.kind < y.kind; });
    for (const auto &e : lib) {
        std::string hay = e.kind + " " + e.category + " " + e.name;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (!q.empty() && hay.find(q) == std::string::npos) continue;
        ++shown;
        if (a.has("--json")) list.push_back({{"kind", e.kind}, {"name", e.name}, {"category", e.category}, {"count", e.count}, {"path", e.path}});
        else if (term::out().on) {
            const term::Style &st = term::out();
            const std::string unit = e.kind == "kit" || e.kind == "loops" ? "files" : e.kind == "sfz" ? "regions" : e.kind == "soundfont" ? "presets" : e.kind == "patch" ? "samples" : e.kind == "ir" ? "ms" : "zones";
            std::fprintf(OUT, "%s %s %s %s\n", st.cyan(col(e.kind, 12)).c_str(), st.dim(col(e.category, 22)).c_str(), st.bold(col(e.name, 44)).c_str(),
                         st.dim(std::to_string(e.count) + " " + unit).c_str());
        }
        else std::fprintf(OUT, "%-12s %-22.22s %-44.44s %4zu %s\n", e.kind.c_str(), e.category.c_str(), e.name.c_str(), e.count,
                          e.kind == "kit" || e.kind == "loops" ? "files" : e.kind == "sfz" ? "regions" : e.kind == "soundfont" ? "presets" : e.kind == "patch" ? "samples" : e.kind == "ir" ? "ms" : "zones");
    }
    if (a.has("--json")) emit(json{{"ok", true}, {"roots", sampleRoots()}, {"samples", list}}.dump(2, ' ', false, json::error_handler_t::replace));
    else std::fprintf(OUT, "\n%zu of %zu libraries. Use as \"plugin\": \"builtin:sampler\" with \"sampler\": {\"<kind>\": \"<name>\"} (multisample, sfz, exs, patch, kit), or {\"soundfont\": \"<name>\", \"program\": N}; an ir in a {\"type\": \"convolve\", \"ir\": \"<name>\"} effect.\n",
                      shown, lib.size());
    return 0;
}

// ---- audition --------------------------------------------------------------------------
int cmdAudition(const Args &a) {
    std::string err, summary;
    if (a.has("--retag")) { retagAuditions(summary); std::fprintf(OUT, "%s\n", summary.c_str()); return 0; }
    if (a.positional.size() < 2) return fail(a, "usage: wavelength audition <plugin> [--jobs N] [--limit N] [--rebuild] | audition --retag");
    PluginInfo info;
    if (!resolvePlugin(a.positional[1], info, err)) return fail(a, err);
    const int jobs = std::atoi(a.get("--jobs", "4").c_str()), limit = std::atoi(a.get("--limit", "0").c_str());
    if (runAudition(info, jobs, limit, a.has("--rebuild"), a.has("--verbose"), err, summary)) return fail(a, err);
    if (a.has("--json")) emit(json{{"ok", true}, {"plugin", info.id}, {"summary", summary}}.dump(2, ' ', false, json::error_handler_t::replace));
    else std::fprintf(OUT, "%s\n", summary.c_str());
    return 0;
}

// ---- compat ----------------------------------------------------------------------------
int cmdCompat(const Args &a) {
    CompatOptions o;
    o.plugins.assign(a.positional.begin() + 1, a.positional.end());
    o.format = a.get("--format");
    if (!o.format.empty() && o.format != "clap" && o.format != "vst3" && o.format != "vst2" && o.format != "au")
        return fail(a, "--format is clap, vst3, vst2 or au");
    o.jobs = std::atoi(a.get("--jobs", "1").c_str());
    o.presets = std::atoi(a.get("--presets", "3").c_str());
    o.timeoutSec = std::max(10, std::atoi(a.get("--timeout", "120").c_str()));
    o.rebuild = a.has("--rebuild");
    o.verbose = a.has("--verbose");
    o.report = a.get("--report");
    json result;
    std::string err;
    if (runCompat(o, result, err)) return fail(a, err);
    if (a.has("--json")) { emit(result.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
    std::fprintf(OUT, "%s\n", result["summary"].get<std::string>().c_str());
    for (auto &r : result["plugins"]) {
        const std::string status = r.value("status", "");
        if (status != "fail" && status != "warn") continue;
        std::string issues;
        for (const char *k : {"fails", "warnings"}) for (auto &x : r[k]) issues += (issues.empty() ? "" : "; ") + x.get<std::string>();
        std::fprintf(OUT, "  %-4s %-5s %s: %s\n", status.c_str(), r.value("format", std::string("")).c_str(), r.value("name", std::string("")).c_str(), issues.c_str());
    }
    if (result.contains("report")) std::fprintf(OUT, "report: %s\n", result["report"].get<std::string>().c_str());
    return 0;
}

// ---- analyze ---------------------------------------------------------------------------
int cmdAnalyze(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength analyze <file.wav | render-dir> [--start S] [--end S] [--song-time]");
    const std::string target = a.positional[1];
    std::string err;
    double start = std::atof(a.get("--start", "0").c_str()), end = std::atof(a.get("--end", "0").c_str());
    // --start/--end are file times; a render's files begin with its lead-in (the report next to them says how long)
    double leadIn = 0;
    json ownReport;   // the report that wrote the analyzed file, if any (lead-in, sections)
    {
        std::error_code ec;
        const fs::path base = fs::is_directory(target, ec) ? fs::path(target) : fs::absolute(target).parent_path();
        // only a report that wrote this file (a render's mix or stem, or a master's output) knows its lead-in
        const bool isDir = fs::is_directory(target, ec);
        const std::string name = fs::path(target).filename().string();
        for (const fs::path &dir : {base, base.parent_path()}) {
            std::ifstream rin(dir / "report.json");
            const json rep = rin ? json::parse(rin, nullptr, false) : json();
            if (!rep.is_object() || !rep.contains("leadIn")) continue;
            bool mine = isDir && dir == base;
            auto named = [&](const json &o) { return o.is_object() && fs::path(o.value("file", "")).filename().string() == name; };
            if (rep.contains("mix") && named(rep["mix"])) mine = true;
            if (rep.contains("output") && named(rep["output"])) mine = true;
            for (auto &t : rep.value("tracks", json::array())) if (named(t)) mine = true;
            for (auto &b : rep.value("buses", json::array())) if (named(b)) mine = true;
            if (mine) { leadIn = rep.value("leadIn", 0.0); ownReport = rep; break; }
        }
    }
    std::string windowNote;
    if (a.has("--song-time")) { if (start > 0 || end > 0) { start += leadIn; if (end > 0) end += leadIn; } }
    else if (leadIn > 0 && (start > 0 || end > 0)) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "--start/--end are file times and this render begins with %.1f s of lead-in; add --song-time to measure song time", leadIn);
        windowNote = buf;
    }
    const bool peaks = a.has("--peaks");
    const size_t top = (size_t)std::clamp(std::atoi(a.get("--top", "12").c_str()), 1, 100);
    auto one = [&](const std::string &path, double s, double e, bool onsets, json &out) {
        Audio audio;
        int sr = 0;
        if (!readAudio(path, audio, sr, err)) return false;
        out = analysisToJson(analyzeAudio(audio, sr, s, e), onsets);
        out["file"] = path;
        if (peaks) {   // --peaks: where the partials sit (combs, resonators, flangers, chords)
            double binHz = 0;
            const auto p = spectralPeaks(audio, sr, s, e, top, binHz);
            out["spectrum"]["peaks"] = peaksToJson(p, binHz);
        }
        return true;
    };
    json result;
    std::error_code ec;
    if (fs::is_directory(target, ec)) {   // a render folder: mix, stems, sections
        const fs::path dir(target);
        std::ifstream rin(dir / "report.json");
        const json report = rin ? json::parse(rin, nullptr, false) : json();
        json mix;
        if (!one((dir / "mix.wav").string(), start, end, true, mix)) return fail(a, err);
        result = {{"ok", true}, {"mix", mix}, {"stems", json::array()}, {"sections", json::array()}};
        // the report names stems as the render was told (relative to where it ran): find them in this folder too
        auto locate = [&](const std::string &f) {
            if (f.empty() || fs::exists(f, ec)) return f;
            for (const fs::path &c : {dir / "stems" / fs::path(f).filename(), dir / fs::path(f).filename()})
                if (fs::exists(c, ec)) return c.string();
            return std::string();
        };
        if (report.is_object() && report.contains("tracks"))
            for (auto &t : report["tracks"]) {
                const std::string f = locate(t.value("file", ""));
                if (f.empty()) continue;
                json s;
                if (one(f, start, end, false, s)) { s["track"] = t.value("name", ""); result["stems"].push_back(s); }
            }
        if (report.is_object() && report.contains("buses"))   // bus stems ("stem": true on the bus)
            for (auto &b : report["buses"]) {
                const std::string f = locate(b.value("file", ""));
                if (f.empty()) continue;
                json s;
                if (one(f, start, end, false, s)) { s["bus"] = b.value("name", ""); result["stems"].push_back(s); }
            }
        if (report.is_object() && report.contains("sections"))
            for (auto &sec : report["sections"]) {
                json s;
                if (one((dir / "mix.wav").string(), sec.value("start", 0.0), sec.value("end", 0.0), false, s)) {
                    s["section"] = sec.value("name", "");
                    result["sections"].push_back(s);
                }
            }
    } else {
        json one1;
        if (!one(target, start, end, true, one1)) return fail(a, err);
        result = one1;
        result["ok"] = true;
    }
    if (!windowNote.empty()) result["note"] = windowNote;
    // --every S: loudness over time (one value per S seconds), labelled with the render's sections
    if (a.has("--every")) {
        const double every = std::atof(a.get("--every").c_str());
        if (every < 0.1) return fail(a, "--every needs a window in seconds (0.1 or more)");
        const std::string file = fs::is_directory(target) ? (fs::path(target) / "mix.wav").string() : target;
        Audio audio;
        int sr = 0;
        if (!readAudio(file, audio, sr, err)) return fail(a, err);
        // in song time the windows start on the first beat (after the lead-in), so they line up with bars
        const double first = a.has("--song-time") && start <= 0 ? leadIn : start;
        const size_t from = (size_t)(std::max(0.0, first) * sr), to = end > 0 ? (size_t)(end * sr) : audio.frames();
        const auto tl = loudnessTimeline(audio, sr, every, every, from, to);
        json list = json::array();
        for (size_t i = 0; i < tl.size(); ++i) {
            const double t = (double)from / sr + i * every;
            json o = {{"time", std::round(t * 100) / 100}, {"songTime", std::round((t - leadIn) * 100) / 100}, {"lufs", std::round(tl[i] * 10) / 10}};
            if (ownReport.contains("sections"))
                for (auto &s : ownReport["sections"])
                    if (t >= s.value("start", 0.0) - 1e-6 && t < s.value("end", 0.0)) o["section"] = s.value("name", "");
            list.push_back(o);
        }
        result["timeline"] = {{"every", every}, {"windows", list}};
    }
    // --grid BPM [--div 4]: how far each onset sits from the nearest grid step (song time, constant tempo)
    json &main = result.contains("mix") ? result["mix"] : result;
    if (a.has("--grid") && main.contains("onsets")) {
        const double bpm = std::atof(a.get("--grid").c_str()), div = std::max(1, std::atoi(a.get("--div", "4").c_str()));
        if (bpm <= 0) return fail(a, "--grid needs the song's tempo in BPM");
        json list = json::array();
        double sumAbs = 0, sum = 0;
        for (auto &o : main["onsets"]) {
            const double t = o.get<double>() - leadIn, beat = t * bpm / 60.0, step = std::round(beat * div) / div;
            const double offMs = (beat - step) * 60000.0 / bpm;
            sumAbs += std::fabs(offMs);
            sum += offMs;
            list.push_back({{"time", std::round(t * 1000) / 1000}, {"beat", std::round(step * 1000) / 1000}, {"offsetMs", std::round(offMs * 10) / 10}});
        }
        result["grid"] = {{"bpm", bpm}, {"div", div}, {"onsets", list},
                          {"meanOffsetMs", list.empty() ? 0.0 : std::round(sum / list.size() * 10) / 10},
                          {"meanAbsOffsetMs", list.empty() ? 0.0 : std::round(sumAbs / list.size() * 10) / 10}};
    }
    if (a.has("--json")) { emit(result.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
    if (result.contains("timeline")) {
        for (auto &w : result["timeline"]["windows"]) {
            const double t = w[a.has("--song-time") ? "songTime" : "time"].get<double>();
            std::fprintf(OUT, "  %2d:%04.1f  %6.1f LUFS  %s\n", (int)(t / 60), std::fmod(t, 60.0), w["lufs"].get<double>(),
                         w.value("section", std::string()).c_str());
        }
        return 0;
    }
    if (result.contains("grid")) {
        const auto &g = result["grid"];
        std::fprintf(OUT, "grid %g BPM, 1/%d beat: mean offset %+.1f ms (+ = late), mean distance %.1f ms\n", g["bpm"].get<double>(),
                     g["div"].get<int>(), g["meanOffsetMs"].get<double>(), g["meanAbsOffsetMs"].get<double>());
        size_t k = 0;
        for (auto &o : g["onsets"]) {
            if (k++ == 64) { std::fprintf(OUT, "  ... (%zu onsets; --json lists all)\n", g["onsets"].size()); break; }
            std::fprintf(OUT, "  %8.3f s  beat %8.3f  %+6.1f ms\n", o["time"].get<double>(), o["beat"].get<double>(), o["offsetMs"].get<double>());
        }
    }
    if (!windowNote.empty()) std::fprintf(OUT, "note: %s\n", windowNote.c_str());
    auto line = [&](const std::string &label, const json &x) {
        const auto &p = x["pitch"], &s = x["spectrum"], &e = x["envelope"];
        std::fprintf(OUT, "%-22.22s %6.1f LUFS  pitch %-4s %+3d c (%.0f%%)  centroid %5d Hz  width %.2f  attack %4d ms  sustain %6.1f dB  onsets %zu\n",
                     label.c_str(), x["lufs"].get<double>(), p["note"].get<std::string>().c_str(), p["cents"].get<int>(),
                     p["confidence"].get<double>() * 100, s["centroidHz"].get<int>(), x["stereo"]["width"].get<double>(),
                     e["attackMs"].get<int>(), e["sustainDb"].get<double>(), x["onsetCount"].get<size_t>());
    };
    auto peakLines = [&](const json &x) {
        if (!x.contains("spectrum") || !x["spectrum"].contains("peaks")) return;
        const auto &p = x["spectrum"]["peaks"];
        char spacing[120] = "";
        if (p.contains("spacingHz"))
            std::snprintf(spacing, sizeof spacing, ", %d of them multiples of %.1f Hz (%s %+d c)", p["onSpacing"].get<int>(), p["spacingHz"].get<double>(),
                          p["spacingNote"].get<std::string>().c_str(), p["spacingCents"].get<int>());
        std::fprintf(OUT, "    peaks (%.1f Hz resolution)%s\n", p["resolutionHz"].get<double>(), spacing);
        for (auto &k : p["peaks"])
            std::fprintf(OUT, "      %8.1f Hz  %-4s %+3d c  %6.1f dB\n", k["hz"].get<double>(), k["note"].get<std::string>().c_str(),
                         k["cents"].get<int>(), k["levelDb"].get<double>());
    };
    if (result.contains("mix")) {
        line("mix", result["mix"]);
        peakLines(result["mix"]);
        for (auto &s : result["stems"]) {
            line(s.contains("bus") ? "bus " + s["bus"].get<std::string>() : s["track"].get<std::string>(), s);
            peakLines(s);
        }
        for (auto &s : result["sections"]) line("section " + s["section"].get<std::string>(), s);
    } else {
        line(fs::path(target).filename().string(), result);
        peakLines(result);
    }
    return 0;
}

// ---- params ----------------------------------------------------------------------------
int cmdParams(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength params <plugin>");
    if (a.positional[1] == "builtin:synth") {   // names for "params" and "automation.params"
        json list = json::array();
        for (const auto &p : synthParams()) {
            if (a.has("--json"))
                list.push_back({{"name", p.name}, {"unit", p.unit}, {"min", p.min}, {"max", p.max}, {"default", p.def}, {"description", p.description},
                                {"curve", p.exp ? "exp" : "linear"}});
            else std::fprintf(OUT, "%-10s %8g .. %-8g %-6s %s\n", p.name.c_str(), p.min, p.max, p.unit.c_str(), p.description.c_str());
        }
        if (a.has("--json")) emit(json{{"ok", true}, {"plugin", "builtin:synth"}, {"format", "builtin"}, {"params", list}}.dump(2));
        else std::fprintf(OUT, "\nSet them with \"params\" (numbers or text like \"800 Hz\") and move them with automation.params; the patch itself is the track's \"synth\" object (docs/job-format.md).\n");
        return 0;
    }
    PluginInfo info;
    std::string err;
    auto inst = openForInspection(a, a.positional[1], info, err);
    if (!inst) return fail(a, err);
    auto params = inst->params();
    // --set "Name=0.55" (or "Name=800 Hz"): what a value means, in the plugin's own display text;
    // --map "Name" [--steps N]: value -> display across the whole range. Nothing is changed.
    if (!a.sets.empty() || a.has("--map")) {
        auto display = [&](const ParamInfo &p, double v) {
            std::string t;
            return inst->textForValue(p.id, v, t) ? t : std::string("?");
        };
        auto norm = [](const ParamInfo &p, double v) { return p.max != p.min ? (v - p.min) / (p.max - p.min) : 0.0; };
        json out = {{"ok", true}, {"plugin", info.id}, {"format", info.format}};
        if (!a.sets.empty()) out["set"] = json::array();
        for (const auto &s : a.sets) {
            const size_t eq = s.find('=');
            if (eq == std::string::npos) return fail(a, "--set takes \"Name=value\"");
            ParamInfo p;
            if (!inst->findParam(s.substr(0, eq), p)) return fail(a, "no parameter '" + s.substr(0, eq) + "' on " + info.name);
            const std::string rhs = s.substr(eq + 1);
            char *end = nullptr;
            double v = std::strtod(rhs.c_str(), &end);
            const bool number = end != rhs.c_str() && *end == 0;
            if (!number) {   // display text or a note name: what plain value the plugin reads it as
                double hz;
                bool ok = false;
                if (Envelope::noteHz(rhs, hz)) {
                    char buf[40];
                    std::snprintf(buf, sizeof buf, "%.2f Hz", hz);
                    ok = inst->valueFromText(p.id, buf, v);
                }
                if (!ok && !inst->valueFromText(p.id, rhs, v)) return fail(a, info.name + " could not read '" + rhs + "' as a value of '" + p.name + "'");
            }
            const std::string d = display(p, v);
            out["set"].push_back({{"name", p.name}, {"id", p.id}, {"input", rhs}, {"value", v}, {"normalized", norm(p, v)}, {"display", d}});
            if (!a.has("--json"))
                std::fprintf(OUT, "%-32s %-12s -> value %.6g (normalized %.4f)  display '%s'\n", p.name.c_str(), rhs.c_str(), v, norm(p, v), d.c_str());
        }
        if (a.has("--map")) {
            ParamInfo p;
            if (!inst->findParam(a.get("--map"), p)) return fail(a, "no parameter '" + a.get("--map") + "' on " + info.name);
            const int steps = std::clamp(std::atoi(a.get("--steps", p.stepped && p.max - p.min <= 64 ? std::to_string((int)(p.max - p.min)) : "20").c_str()), 1, 1000);
            json rows = json::array();
            if (!a.has("--json")) std::fprintf(OUT, "%s (%s): value -> display\n", p.name.c_str(), info.name.c_str());
            for (int i = 0; i <= steps; ++i) {
                const double v = p.min + (p.max - p.min) * i / steps;
                const std::string d = display(p, v);
                rows.push_back({{"value", v}, {"normalized", norm(p, v)}, {"display", d}});
                if (!a.has("--json")) std::fprintf(OUT, "  %10.5g  (%.3f)  %s\n", v, norm(p, v), d.c_str());
            }
            out["map"] = {{"name", p.name}, {"id", p.id}, {"rows", rows}};
        }
        if (a.has("--json")) emit(out.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    json list = json::array();
    size_t shown = 0, midiHidden = 0;
    for (auto &p : params) {
        // JUCE plugins expose ~2000 "MIDI CC 0|1" placeholder parameters: use "cc"/"pitchbend" automation instead
        const bool midiPlaceholder = p.name.rfind("MIDI CC ", 0) == 0 || p.name == "MIDI";
        if (midiPlaceholder) ++midiHidden;
        if (!a.has("--all") && (p.hidden || p.readonly || midiPlaceholder)) continue;
        ++shown;
        if (a.has("--json"))
            list.push_back({{"id", p.id}, {"name", p.name}, {"module", p.module}, {"min", p.min}, {"max", p.max},
                            {"default", p.def}, {"value", p.value}, {"display", p.display}, {"stepped", p.stepped}});
        else
            std::fprintf(OUT, "#%-6u %-40s %10.4g  [%g .. %g]  %s\n", p.id,
                        (p.module.empty() ? p.name : p.module + "/" + p.name).c_str(), p.value, p.min, p.max, p.display.c_str());
    }
    if (a.has("--json")) emit(json{{"ok", true}, {"plugin", info.id}, {"format", info.format}, {"params", list}}.dump(2, ' ', false, json::error_handler_t::replace));
    else std::fprintf(OUT, "\n%zu of %zu parameters (%s)%s\n", shown, params.size(), info.name.c_str(),
                      midiHidden && !a.has("--all") ? (", " + std::to_string(midiHidden) + " MIDI controller placeholders hidden: use \"cc\" / \"pitchbend\" automation").c_str() : "");
    return 0;
}

// ---- master ----------------------------------------------------------------------------
// A finished mix through a master chain, without re-rendering the song: the file plays on a
// builtin:audio track and the chain runs as the job's master (loudness target included).
int cmdMaster(const Args &a) {
    if (a.positional.size() < 2 || !a.has("--chain"))
        return fail(a, "usage: wavelength master <mix.wav> --chain <chain.json | job.json> [--loudness LUFS] [--lead-in S] [--out DIR]");
    const std::string input = fs::absolute(a.positional[1]).string();
    Audio in;
    int sr = 0;
    std::string err;
    if (!readAudio(input, in, sr, err)) return fail(a, err);
    const std::string chainArg = a.get("--chain");
    const bool inlineChain = !chainArg.empty() && (chainArg[0] == '{' || chainArg[0] == '[');   // JSON on the command line
    json chain;
    try {
        if (inlineChain) chain = json::parse(chainArg);
        else {
            std::ifstream cf(chainArg);
            if (!cf) return fail(a, "cannot read " + chainArg);
            cf >> chain;
        }
    } catch (const std::exception &e) { return fail(a, std::string("chain is not valid JSON: ") + e.what()); }
    json master, markers = json::array(), tempo = 120, deliver = json::array();
    if (chain.is_array()) master = {{"fx", chain}};
    else if (chain.is_object() && (chain.contains("tracks") || chain.contains("master"))) {   // a song's job (or its master part): master, markers, tempo
        master = chain.value("master", json::object());
        markers = chain.value("markers", json::array());
        if (chain.contains("tempo")) tempo = chain["tempo"];
        if (chain.contains("deliver")) deliver = chain["deliver"];
    } else if (chain.is_object()) master = chain;
    else return fail(a, "the chain must be an effect list, a master object ({\"fx\": [...], \"loudness\": -14}) or a job");
    if (a.has("--loudness")) master["loudness"] = std::atof(a.get("--loudness").c_str());
    if (a.has("--deliver")) {
        deliver = json::array();
        std::stringstream list(a.get("--deliver"));
        for (std::string item; std::getline(list, item, ',');) if (!item.empty() && item != "none") deliver.push_back(item);
    }
    if (!master.is_object() || ((!master.contains("fx") || master["fx"].empty()) && !master.contains("loudness"))) {
        std::string keys;
        if (chain.is_object()) for (auto &[k, v] : chain.items()) keys += (keys.empty() ? "" : ", ") + k;
        return fail(a, "the chain has no effects and no loudness target (found keys: " + (keys.empty() ? std::string("none") : keys) +
                        "); pass an effect list, {\"fx\": [...]}, {\"master\": {\"fx\": [...]}} or a job");
    }
    // a mix rendered with a lead-in starts with that much silence: skip it so markers line up, and keep
    // it on the output unless --lead-in says otherwise (the render's report next to the mix says how long)
    double inputLeadIn = 0;
    if (a.has("--input-lead-in")) inputLeadIn = std::atof(a.get("--input-lead-in").c_str());
    else {
        std::ifstream rin(fs::path(input).parent_path() / "report.json");
        const json rep = rin ? json::parse(rin, nullptr, false) : json();
        if (rep.is_object() && rep.contains("mix") && fs::path(rep["mix"].value("file", "")).filename() == fs::path(input).filename())
            inputLeadIn = rep.value("leadIn", 0.0);
    }
    inputLeadIn = std::clamp(inputLeadIn, 0.0, std::max(0.0, (double)in.frames() / sr - 1));
    const double leadIn = a.has("--lead-in") ? std::atof(a.get("--lead-in").c_str()) : inputLeadIn;
    const double seconds = (double)in.frames() / sr - inputLeadIn;
    json clip = {{"file", input}, {"beat", 0}};
    if (inputLeadIn > 0) { clip["start"] = inputLeadIn; clip["fadeIn"] = 0; }
    const json jobJson = {{"sampleRate", sr}, {"tempo", tempo}, {"tail", 0}, {"length", seconds}, {"stems", "none"}, {"leadIn", leadIn},
                          {"markers", markers}, {"master", master}, {"deliver", deliver},
                          {"tracks", json::array({{{"name", "Mix"}, {"plugin", "builtin:audio"}, {"clips", json::array({clip})}}})}};
    const std::string base = inlineChain ? fs::current_path().string() : fs::absolute(chainArg).parent_path().string();
    Job job;
    if (!parseJob(jobJson, base, job, err, false)) return fail(a, err);
    const std::string outDir = a.get("--out", (fs::path(input).parent_path() / "mastered").string());
    RenderResult r;
    bool ok = false;
    {
        term::Progress bar("Mastering " + fs::path(input).filename().string());
        setRenderProgress([&](size_t done, size_t total, const std::string &now) { bar.update(done, total, now); });
        try { ok = renderJob(job, outDir, a.has("--verbose"), r, err); }
        catch (const std::exception &e) { err = std::string("master failed: ") + e.what(); }
        setRenderProgress(nullptr);
    }
    if (!ok) return fail(a, err);
    std::error_code ec;
    fs::remove_all(fs::path(outDir) / "stems", ec);
    auto r1 = [](double v) { return std::round(v * 10) / 10; };
    json sections = json::array();
    for (auto &sec : r.sections)
        sections.push_back({{"name", sec.name}, {"start", std::round(sec.start * 100) / 100},
                            {"inputLufs", r1(integratedLufs(in, sr, (size_t)((sec.start - leadIn + inputLeadIn) * sr), (size_t)((sec.end - leadIn + inputLeadIn) * sr)))}, {"lufs", r1(sec.lufs)}});
    json report = {{"ok", true}, {"inputLeadIn", inputLeadIn}, {"leadIn", leadIn},
                   {"input", {{"file", input}, {"lufs", r1(integratedLufs(in, sr))}, {"lra", r1(loudnessRange(in, sr))}, {"truePeakDb", r1(truePeakDb(in))}}},
                   {"output", {{"file", r.mixFile}, {"lufs", r1(r.mixLufs)}, {"lra", r1(r.mixLra)}, {"truePeakDb", r1(r.truePeakDb)},
                               {"loudnessGainDb", r1(r.loudnessGainDb)}, {"levels", levelsJson(r.mix)}, {"deliveries", deliveriesJson(r.deliveries)}}},
                   {"masterFx", r.masterFx}, {"sections", sections}, {"warnings", r.warnings},
                   {"renderSeconds", std::round(r.renderSeconds * 100) / 100}};
    std::ofstream(fs::path(outDir) / "report.json") << report.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    if (a.has("--json")) { emit(report.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
    std::fprintf(OUT, "input   %6.1f LUFS  %5.1f dBTP  %s\n", report["input"]["lufs"].get<double>(), report["input"]["truePeakDb"].get<double>(), input.c_str());
    std::fprintf(OUT, "output  %6.1f LUFS  %5.1f dBTP  %s\n", r.mixLufs, r.truePeakDb, r.mixFile.c_str());
    for (auto &d : r.deliveries) std::fprintf(OUT, "        %6.1f LUFS  %5.1f dBTP  %s\n", d.lufs, d.truePeakDb, d.file.c_str());
    for (auto &sec : sections)
        std::fprintf(OUT, "  %-20s %6.1f -> %6.1f LUFS\n", sec["name"].get<std::string>().c_str(), sec["inputLufs"].get<double>(), sec["lufs"].get<double>());
    for (auto &w : r.warnings) std::fprintf(OUT, "warning: %s\n", w.c_str());
    return 0;
}

// ---- render ----------------------------------------------------------------------------
// ---- import -------------------------------------------------------------------------
int cmdImport(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength import <project.dawproject | song.mid | loop.caf | score.musicxml | score.mxl | project.bwproject> [--out DIR]");
    const std::string src = a.positional[1];
    if (fs::path(src).extension() == ".bwproject" && a.has("--list")) {   // Bitwig's own format: list its tracks and devices
        bitwig::Project p;
        std::string err;
        if (!bitwig::load(src, p, err)) return fail(a, err);
        std::function<json(const std::vector<bitwig::Device> &)> devs = [&](const std::vector<bitwig::Device> &ds) {
            json out = json::array();
            for (auto &d : ds) {
                json j = {{"name", d.name}, {"kind", d.kind}};
                if (!d.pluginId.empty()) j["id"] = d.pluginId;
                if (!d.state.empty()) j["state"] = d.state;
                if (!d.sample.empty()) j["sample"] = {{"file", d.sample}, {"root", d.sampleRoot}};
                if (!d.enabled) j["enabled"] = false;
                if (!d.params.empty()) j["params"] = d.params;
                for (auto &c : d.chains) j["chains"][c.first] = devs(c.second);
                for (auto &pd : d.pads) j["pads"].push_back({{"key", pd.key}, {"volume", pd.volume}, {"pan", pd.pan}, {"mute", pd.mute}, {"devices", devs(pd.devices)}});
                out.push_back(j);
            }
            return out;
        };
        auto trackJson = [&](const bitwig::Track &t) { return json{{"name", t.name}, {"devices", devs(t.devices)}}; };
        json tj = json::array(), ej = json::array();
        for (auto &t : p.tracks) tj.push_back(trackJson(t));
        for (auto &t : p.effects) ej.push_back(trackJson(t));
        json out = {{"ok", true}, {"format", p.format}, {"tracks", tj}, {"effectTracks", ej}};
        if (p.hasMaster) out["master"] = trackJson(p.master);
        if (a.has("--json")) { emit(out.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
        std::function<void(const json &, int)> show = [&](const json &ds, int ind) {
            for (auto &d : ds) {
                std::fprintf(OUT, "%*s%s (%s)%s\n", ind, "", d["name"].get<std::string>().c_str(), d["kind"].get<std::string>().c_str(), d.contains("state") ? " +state" : "");
                if (d.contains("chains")) for (auto &c : d["chains"].items()) { std::fprintf(OUT, "%*s[%s]\n", ind + 2, "", c.key().c_str()); show(c.value(), ind + 4); }
                if (d.contains("pads")) for (auto &pd : d["pads"]) { std::fprintf(OUT, "%*spad %d\n", ind + 2, "", pd["key"].get<int>()); show(pd["devices"], ind + 4); }
            }
        };
        int k = 1;
        for (auto &t : tj) { std::fprintf(OUT, "track %d %s\n", k++, t["name"].get<std::string>().c_str()); show(t["devices"], 2); }
        for (auto &t : ej) { std::fprintf(OUT, "effect track %s\n", t["name"].get<std::string>().c_str()); show(t["devices"], 2); }
        if (out.contains("master")) { std::fprintf(OUT, "master\n"); show(out["master"]["devices"], 2); }
        return 0;
    }
    const std::string outDir = a.get("--out", fs::path(src).stem().string());
    std::string ext = fs::path(src).extension().string();
    for (auto &ch : ext) ch = (char)std::tolower((unsigned char)ch);
    if (ext == ".mid" || ext == ".midi" || ext == ".smf" || ext == ".kar" || ext == ".rmi" || ext == ".caf") {   // .caf: a software-instrument Apple Loop
        MidiImport m;
        std::string err;
        if (!importMidiFile(src, outDir, a.get("--instrument", ""), m, err)) return fail(a, err);
        const std::string jobPath = (fs::path(outDir) / "job.json").string();
        if (a.has("--json")) {
            emit(json{{"ok", true}, {"job", jobPath}, {"format", m.format}, {"tracks", m.tracks}, {"notes", m.noteCount}, {"left out", m.notes}}.dump(2, ' ', false, json::error_handler_t::replace));
            return 0;
        }
        std::fprintf(OUT, "imported %s (MIDI type %d): %zu tracks, %zu notes -> %s\n", src.c_str(), m.format, m.tracks, m.noteCount, jobPath.c_str());
        for (auto &n : m.notes) std::fprintf(OUT, "  ! %s\n", n.c_str());
        return 0;
    }
    if (ext == ".musicxml" || ext == ".mxl" || ext == ".xml") {
        MusicXmlImport m;
        std::string err;
        if (!importMusicXml(src, outDir, a.get("--instrument", ""), m, err)) return fail(a, err);
        const std::string jobPath = (fs::path(outDir) / "job.json").string();
        if (a.has("--json")) {
            emit(json{{"ok", true}, {"job", jobPath}, {"tracks", m.tracks}, {"notes", m.noteCount}, {"measures", m.measures},
                      {"playedMeasures", m.playedMeasures}, {"left out", m.notes}}.dump(2, ' ', false, json::error_handler_t::replace));
            return 0;
        }
        std::fprintf(OUT, "imported %s (MusicXML): %zu tracks, %zu notes, %zu measures (%zu played with repeats) -> %s\n", src.c_str(), m.tracks,
                     m.noteCount, m.measures, m.playedMeasures, jobPath.c_str());
        for (auto &n : m.notes) std::fprintf(OUT, "  ! %s\n", n.c_str());
        return 0;
    }
    DawprojectImport r;
    std::string err;
    const bool bwproject = ext == ".bwproject";   // Bitwig's own project, no export needed
    if (bwproject ? !importBitwig(src, outDir, r, err) : !importDawproject(src, outDir, r, err, a.get("--bitwig", ""))) return fail(a, err);
    const std::string jobPath = (fs::path(outDir) / "job.json").string();
    if (a.has("--json")) {
        emit(json{{"ok", true}, {"job", jobPath}, {"application", r.application}, {"tracks", r.tracks}, {"buses", r.buses},
                  {"notes", r.noteCount}, {"plugins", r.plugins}, {"bitwig", r.bitwig}, {"left out", r.notes}}.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    std::fprintf(OUT, "imported %s%s: %zu tracks, %zu buses, %zu notes, %zu plugins -> %s\n", src.c_str(),
                 r.application.empty() ? "" : (" (" + r.application + ")").c_str(), r.tracks, r.buses, r.noteCount, r.plugins, jobPath.c_str());
    if (!r.bitwig.empty() && !bwproject) std::fprintf(OUT, "  Bitwig's own devices from %s\n", r.bitwig.c_str());
    for (auto &n : r.notes) std::fprintf(OUT, "  ! %s\n", n.c_str());
    return 0;
}

int cmdExport(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength export <job.json> [--out song.mid | song.dawproject]");
    const std::string src = a.positional[1];
    std::ifstream in(src);
    if (!in) return fail(a, "cannot read " + src);
    json j;
    try { in >> j; } catch (const std::exception &e) { return fail(a, std::string("job is not valid JSON: ") + e.what()); }
    Job job;
    std::string err;
    if (!parseJob(j, fs::path(src).parent_path().string(), job, err)) return fail(a, err);
    std::string out = a.get("--out", (fs::path(src).parent_path() / (fs::path(src).stem().string() + ".mid")).string());
    if (fs::path(out).extension() == ".dawproject") {
        DawprojectExport d;
        if (!exportDawproject(job, j, src, out, d, err, !a.has("--no-print"))) return fail(a, err);
        if (a.has("--json")) {
            emit(json{{"ok", true}, {"file", out}, {"tracks", d.tracks}, {"buses", d.buses}, {"plugins", d.plugins}, {"notes", d.noteCount},
                      {"left out", d.notes}}.dump(2, ' ', false, json::error_handler_t::replace));
            return 0;
        }
        std::fprintf(OUT, "exported %zu tracks, %zu buses, %zu plugin states, %zu notes -> %s\n", d.tracks, d.buses, d.plugins, d.noteCount, out.c_str());
        for (auto &x : d.notes) std::fprintf(OUT, "  ! %s\n", x.c_str());
        return 0;
    }
    if (fs::path(out).extension() != ".mid" && fs::path(out).extension() != ".midi") return fail(a, "export writes MIDI (.mid) or DAWproject (.dawproject) files: give --out one of those names");
    std::vector<std::string> notes;
    if (!exportMidiFile(job, j, out, notes, err)) return fail(a, err);
    size_t n = 0, tracks = 0;
    for (auto &t : job.tracks) if (!t.notes.empty()) { n += t.notes.size(); ++tracks; }
    if (a.has("--json")) {
        emit(json{{"ok", true}, {"file", out}, {"tracks", tracks}, {"notes", n}, {"left out", notes}}.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    std::fprintf(OUT, "exported %zu tracks, %zu notes -> %s\n", tracks, n, out.c_str());
    for (auto &x : notes) std::fprintf(OUT, "  ! %s\n", x.c_str());
    return 0;
}

int cmdRender(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength render <job.json | project.dawproject>");
    std::string path = a.positional[1];
    if (package::isPackage(path)) {   // a .wavelength song: unpacked once into the cache, rendered from there
        std::string err;
        const std::string dir = package::cached(path, err);
        if (dir.empty()) return fail(a, err);
        Song song;
        if (!openSong(dir, song, err)) return fail(a, err);
        path = song.jobPath().string();
        // a package is untrusted: its job reads only files of the song and writes only into the output folder
        nlohmann::json pj;
        if (!parseJsonStrict(readText(path), pj, err)) return fail(a, "the song's job " + err);
        std::string outside, why;
        for (auto &[where, ref] : jobFileRefs(pj))
            if (!checkSongPath(ref, why) || fs::is_symlink(fs::path(dir) / fs::u8path(ref))) outside += "\n  " + where + ": " + ref;
        for (auto &[where, ref] : jobOutputRefs(pj))
            if (!checkSongPath(ref, why)) outside += "\n  " + where + ": " + ref;
        if (!outside.empty()) return fail(a, "not rendering this package: its job reads or writes outside the song" + outside);
    }
    if (const std::string ext = fs::path(path).extension().string(); ext == ".mid" || ext == ".midi") {   // import, then render
        const std::string importDir = (fs::path(a.get("--out", "out")) / "import").string();
        MidiImport m;
        std::string err;
        if (!importMidiFile(path, importDir, a.get("--instrument", ""), m, err)) return fail(a, err);
        for (auto &n : m.notes) std::fprintf(stderr, "import: %s\n", n.c_str());
        path = (fs::path(importDir) / "job.json").string();
    }
    if (const std::string ext = fs::path(path).extension().string(); ext == ".musicxml" || ext == ".mxl") {   // import, then render
        const std::string importDir = (fs::path(a.get("--out", "out")) / "import").string();
        MusicXmlImport m;
        std::string err;
        if (!importMusicXml(path, importDir, a.get("--instrument", ""), m, err)) return fail(a, err);
        for (auto &n : m.notes) std::fprintf(stderr, "import: %s\n", n.c_str());
        path = (fs::path(importDir) / "job.json").string();
    }
    if (fs::path(path).extension() == ".dawproject" || fs::path(path).extension() == ".bwproject") {   // import next to the output, then render that job
        const std::string importDir = (fs::path(a.get("--out", "out")) / "import").string();
        DawprojectImport r;
        std::string err;
        const bool ok = fs::path(path).extension() == ".bwproject" ? importBitwig(path, importDir, r, err)
                                                                    : importDawproject(path, importDir, r, err, a.get("--bitwig", ""));
        if (!ok) return fail(a, err);
        for (auto &n : r.notes) std::fprintf(stderr, "import: %s\n", n.c_str());
        path = (fs::path(importDir) / "job.json").string();
    }
    std::ifstream in(path);
    if (!in) return fail(a, "cannot read " + path);
    json j;
    try { in >> j; } catch (const std::exception &e) { return fail(a, std::string("job is not valid JSON: ") + e.what()); }
    // tracks whose plugin or sample library isn't on this computer play their "fallback"
    std::vector<std::string> fallbackNotes;
    if (a.has("--fallbacks") && a.has("--keep")) return fail(a, "--keep keeps the song's own render, not one with --fallbacks");
    const int swapped = applyFallbacks(j, fs::absolute(path).parent_path().string(), fallbackNotes, a.has("--fallbacks"));
    // --mix '{"Bass": {"gain": -3, "pan": 0.2, "mute": true}}': mixer settings for this render only (the job file
    // stays as it is). With --cache nothing renders again for them.
    bool mixed = false;
    if (a.has("--mix")) {
        std::string text = a.get("--mix");
        if (!text.empty() && text[0] == '@') {
            std::ifstream mf(text.substr(1));
            if (!mf) return fail(a, "--mix: cannot read " + text.substr(1));
            text.assign(std::istreambuf_iterator<char>(mf), std::istreambuf_iterator<char>());
        }
        const json m = json::parse(text, nullptr, false);
        if (!m.is_object()) return fail(a, "--mix takes a JSON object: {\"<track>\": {\"gain\": dB, \"pan\": -1..1, \"mute\": true}}");
        if (!j.contains("tracks") || !j["tracks"].is_array()) return fail(a, "the job has no tracks");
        for (auto &[name, set] : m.items()) {
            json *t = nullptr;
            for (auto &x : j["tracks"]) if (x.value("name", "") == name) t = &x;
            if (!t) return fail(a, "--mix: no track named '" + name + "'");
            if (!set.is_object()) return fail(a, "--mix: '" + name + "' needs an object ({\"gain\": -3})");
            for (auto &[k, v] : set.items()) {
                if ((k == "gain" || k == "pan") && v.is_number()) (*t)[k] = k == "pan" ? std::clamp(v.get<double>(), -1.0, 1.0) : v.get<double>();
                else if (k == "mute" && v.is_boolean()) (*t)[k] = v;
                else return fail(a, "--mix: '" + name + "': \"" + k + "\" must be gain (dB), pan (-1..1) or mute (true/false)");
            }
        }
        mixed = true;
    }
    // --tracks "Lead,Bass": render only those (plus, muted, the tracks that key their sidechains).
    // Workers re-read the job by track index, so the subset goes to a file next to the job.
    std::string subsetPath;
    std::vector<std::string> only;
    std::vector<int> stemNumbers;
    if (a.has("--tracks")) {
        std::stringstream ss(a.get("--tracks"));
        for (std::string n; std::getline(ss, n, ',');) {
            n.erase(0, n.find_first_not_of(' '));
            n.erase(n.find_last_not_of(' ') + 1);
            if (!n.empty()) only.push_back(n);
        }
        if (!j.contains("tracks") || !j["tracks"].is_array()) return fail(a, "the job has no tracks");
        std::set<std::string> want(only.begin(), only.end()), have;
        for (auto &t : j["tracks"]) have.insert(t.value("name", ""));
        for (auto &n : only) if (!have.count(n)) return fail(a, "--tracks: no track named '" + n + "'");
        std::set<std::string> need = want;
        auto keys = [&](const json &fx) {
            if (fx.is_array())   // "sidechain" keys from a track's audio, "trigger" (duck, gate) from its notes
                for (auto &e : fx)
                    for (const char *k : {"sidechain", "trigger"})
                        if (e.is_object() && e.contains(k) && e[k].is_string()) need.insert(e[k].get<std::string>());
        };
        for (auto &b : j.value("buses", json::array())) keys(b.value("fx", json::array()));
        if (j.contains("master") && j["master"].is_object()) keys(j["master"].value("fx", json::array()));
        for (size_t before = 0; before != need.size();) {   // sidechain sources of sources
            before = need.size();
            for (auto &t : j["tracks"]) if (need.count(t.value("name", ""))) keys(t.value("fx", json::array()));
        }
        json kept = json::array();
        size_t position = 0;
        for (auto &t : j["tracks"]) {
            const std::string n = t.value("name", "");
            ++position;
            if (!need.count(n)) continue;
            stemNumbers.push_back((int)position);   // stems keep the full render's file names
            json c = t;
            if (!want.count(n)) { c["mute"] = true; c["stem"] = false; }   // renders only to key an effect: no mix, no stem file
            kept.push_back(c);
        }
        j["tracks"] = kept;
    }
    // --from/--to BAR: render only those bars (to exclusive), after --preroll bars (default 2) that are
    // rendered and then cut, so reverbs, delays, compressors and held notes are already going
    if (a.has("--from") || a.has("--to")) {
        const double bpb = j.contains("timeSignature") ? j["timeSignature"][0].get<double>() * 4.0 / j["timeSignature"][1].get<double>() : 4.0;
        const double from = a.has("--from") ? std::atof(a.get("--from").c_str()) : 1, pre = a.has("--preroll") ? std::atof(a.get("--preroll").c_str()) : 2;
        if (!a.has("--to")) return fail(a, "--from needs --to BAR (the bar where the render stops, not included)");
        const double to = std::atof(a.get("--to").c_str());
        if (from < 1 || to <= from) return fail(a, "--from/--to are bars: --from 41 --to 45 renders bars 41-44");
        j["window"] = {{"from", (from - 1) * bpb}, {"to", (to - 1) * bpb}, {"preroll", std::max(0.0, pre) * bpb}};
        if (a.has("--loop")) { j["window"]["loop"] = true; j["window"]["preroll"] = 0; }   // the bars alone, their tail folded back in
    } else if (a.has("--loop")) return fail(a, "--loop needs --from BAR --to BAR (the loop's bars, to exclusive)");
    if (!only.empty() || j.contains("window") || swapped > 0 || mixed) {   // workers re-read the job by track index: the changed job goes to a file next to it
        subsetPath = (fs::absolute(path).parent_path() / (".wavelength-tracks-" + std::to_string(platform::processId()) + ".json")).string();
        std::ofstream(subsetPath) << j.dump();
    }
    struct RemoveSubset { std::string p; ~RemoveSubset() { std::error_code ec; if (!p.empty()) fs::remove(p, ec); } } removeSubset{subsetPath};
    Job job;
    std::string err;
    std::string base = fs::absolute(path).parent_path().string();
    if (!parseJob(j, base, job, err)) return fail(a, err);
    job.sourcePath = subsetPath.empty() ? fs::absolute(path).string() : subsetPath;
    for (size_t k = 0; k < stemNumbers.size() && k < job.tracks.size(); ++k) job.tracks[k].stemNumber = stemNumbers[k];
    if (a.has("--jobs")) job.parallel = std::atoi(a.get("--jobs").c_str());
    job.trackCache = a.has("--cache");
    if (a.has("--level-from")) {   // play at the level of an earlier render (a full mix): its master gains, not a new target
        std::ifstream rin(a.get("--level-from"));
        json rep;
        try { rin >> rep; } catch (...) { return fail(a, "--level-from: cannot read report " + a.get("--level-from")); }
        if (!rep.contains("mix") || !rep["mix"].is_object()) return fail(a, "--level-from: " + a.get("--level-from") + " is not a render report");
        job.levelFixed = true;
        job.fixedLoudnessGainDb = rep["mix"].value("loudnessGainDb", 0.0);
        job.fixedNormalizeGainDb = rep["mix"].value("normalizeGainDb", 0.0);
    }
    if (a.has("--deliver")) {   // "mp3,flac:16" replaces the job's own; "none" turns it off
        job.deliver.clear();
        std::stringstream list(a.get("--deliver"));
        for (std::string item; std::getline(list, item, ',');) {
            if (item.empty() || item == "none") continue;
            DeliverySpec spec;
            if (!parseDeliverySpec(item, spec, err)) return fail(a, "--deliver: " + err);
            job.deliver.push_back(spec);
        }
    } else if (!only.empty() || job.window.on) job.deliver.clear();   // a partial render is not a delivery
    if (a.has("--stems")) {
        const std::string s = a.get("--stems");
        job.stemBits = s == "none" ? 0 : s == "16" ? 16 : s == "24" ? 24 : s == "float" || s == "32" ? 32 : -1;
        if (job.stemBits < 0) return fail(a, "--stems must be float, 24, 16 or none");
    }
    if (a.has("--png")) job.picture = true;
    if (a.has("--no-png")) job.picture = false;   // a job with "picture": true, rendered where no one looks at it (serve's live loop)
    Song keepSong;
    if (a.has("--keep")) {   // the render goes with the song: it needs a song, the whole song and an MP3
        if (!songOfJob(path, keepSong)) return fail(a, "--keep needs a song (its job next to wavelength.json: `wavelength save` makes one)");
        if (!only.empty() || job.window.on) return fail(a, "--keep keeps a whole render, not --tracks or --from/--to");
        if (std::none_of(job.deliver.begin(), job.deliver.end(), [](const DeliverySpec &d) { return d.format == "mp3"; })) {
            DeliverySpec mp3;
            if (!parseDeliverySpec("mp3", mp3, err)) return fail(a, err);
            job.deliver.push_back(mp3);
        }
    }
    RenderResult r;
    std::string outDir = a.get("--out", "out");
    bool ok = false;
    {   // a progress line on a terminal (never in a pipe or with --json)
        term::Progress bar(std::string(job.window.on ? "Previewing " : "Rendering ") + titleOfJob(fs::absolute(path)));
        setRenderProgress([&](size_t done, size_t total, const std::string &now) { bar.update(done, total, now); });
        try { ok = renderJob(job, outDir, a.has("--verbose"), r, err); }
        catch (const std::exception &e) { err = std::string("render failed: ") + e.what(); }
        setRenderProgress(nullptr);
    }
    if (!ok)   // a track with fallbacks that can't play any of them: name what was tried
        for (auto &n : fallbackNotes)
            if (n.find("none of its fallbacks") != std::string::npos) err += "; " + n;
    if (!ok) {   // a failed render leaves a failed report, never the previous render's
        std::error_code ec;
        if (fs::is_directory(outDir, ec)) std::ofstream(fs::path(outDir) / "report.json") << json{{"ok", false}, {"error", err}}.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
        return fail(a, err);
    }

    auto r1 = [](double v) { return std::round(v * 10) / 10; };
    // per-section loudness, labelled ({"name", "lufs"}) and as a bare list in "sections" order
    auto labelled = [&](const std::vector<double> &v) {
        json o = json::array();
        for (size_t i = 0; i < v.size() && i < r.sections.size(); ++i) o.push_back({{"name", r.sections[i].name}, {"lufs", r1(v[i])}});
        return o;
    };
    auto bare = [&](const std::vector<double> &v) { json o = json::array(); for (double x : v) o.push_back(r1(x)); return o; };
    json tracks = json::array();
    for (auto &t : r.tracks)
        tracks.push_back({{"name", t.name}, {"plugin", t.plugin}, {"pluginName", t.pluginName}, {"file", t.file},
                          {"preset", t.preset}, {"notes", t.notes}, {"paramsApplied", t.paramsApplied}, {"automatedParams", t.automated},
                          {"stateFormat", t.stateFormat}, {"fx", t.fx}, {"lufs", r1(t.lufs)}, {"postFaderLufs", r1(t.postLufs)}, {"postFaderPeakDb", r1(t.postPeakDb)},
                          {"lowShare", std::round(lowShare(t) * 100) / 100},
                          {"renderSeconds", std::round(t.seconds * 100) / 100},
                          {"latencyCompensatedMs", std::round(t.latencySamples * 1000.0 / r.sampleRate * 100) / 100},
                          {"sections", labelled(t.sectionLufs)}, {"sectionLufs", bare(t.sectionLufs)},
                          {"levels", levelsJson(t.levels)}, {"automation", t.automation}, {"warnings", t.warnings}});
    for (size_t i = 0; i < r.tracks.size(); ++i) if (r.tracks[i].cached) tracks[i]["cached"] = true;
    json buses = json::array();
    for (auto &b : r.buses)
        buses.push_back({{"name", b.name}, {"fx", b.fx}, {"file", b.file}, {"lufs", r1(b.lufs)}, {"sections", labelled(b.sectionLufs)},
                         {"sectionLufs", bare(b.sectionLufs)}, {"levels", levelsJson(b.levels)}, {"automation", b.automation}});
    json sections = json::array();
    for (size_t m = 0; m < r.sections.size(); ++m) {
        const auto &sec = r.sections[m];
        json o = {{"name", sec.name}, {"start", std::round(sec.start * 100) / 100}, {"end", std::round(sec.end * 100) / 100}, {"lufs", r1(sec.lufs)},
                  {"preMasterLufs", r1(sec.preMasterLufs)}};
        if (!sec.checks) o["checks"] = false;
        if (m > 0 && sec.tailBefore > -60 && sec.head > -60) {   // the boundary as heard: last 2 bars before it, first 4 after it
            o["transition"] = {{"lastBarsBefore", r1(sec.tailBefore)}, {"firstBars", r1(sec.head)}, {"jump", r1(sec.head - sec.tailBefore)}};
            if (sec.skippedSilence)   // the bars before the boundary were a near-silence: measured against the music before it
                o["transition"]["skippedSilence"] = {{"bars", {(int)std::floor(sec.silenceFrom / 4) + 1, (int)std::floor(sec.at / 4)}}, {"lufs", r1(sec.silenceLufs)},
                                                     {"lastBarsFrom", (int)std::floor(sec.tailFrom / 4) + 1}};
        }
        if (m > 0 && sec.lufs > -60 && r.sections[m - 1].lufs > -60) o["change"] = r1(sec.lufs - r.sections[m - 1].lufs);   // dB over the previous section
        sections.push_back(o);
    }
    json dropouts = json::array();
    for (auto &d : r.dropouts)
        dropouts.push_back({{"start", std::round((d.start + r.leadIn) * 100) / 100}, {"end", std::round((d.end + r.leadIn) * 100) / 100},
                            {"lufs", r1(d.lufs)}, {"musicLufs", r1(d.around)}, {"bars", {std::floor(d.startBar), std::floor(d.endBar)}},
                            {"intended", d.intended}});
    r.warnings.insert(r.warnings.begin(), fallbackNotes.begin(), fallbackNotes.end());   // a stand-in sound changes the song: say so first
    const bool complete = r.failedTracks.empty();
    std::string incomplete;
    if (!complete) {
        for (auto &n : r.failedTracks) incomplete += (incomplete.empty() ? "'" : ", '") + n + "'";
        incomplete = std::to_string(r.failedTracks.size()) + (r.failedTracks.size() == 1 ? " track" : " tracks") + " failed (" + incomplete +
                     "): mix.wav and this report are missing them, so levels, ducking and loudness are wrong; render again";
    }
    json report = {{"ok", complete}, {"sampleRate", r.sampleRate}, {"seconds", std::round(r.seconds * 100) / 100},
                   {"duration", std::round((r.seconds + r.leadIn) * 100) / 100},   // the written file, lead-in included
                   {"renderSeconds", std::round(r.renderSeconds * 100) / 100}, {"leadIn", r.leadIn}, {"defaultsApplied", job.appliedDefaults},
                   {"mix", {{"file", r.mixFile}, {"lufs", r1(r.mixLufs)}, {"lra", r1(r.mixLra)}, {"truePeakDb", r1(r.truePeakDb)}, {"levels", levelsJson(r.mix)},
                            {"masterFx", r.masterFx}, {"masterAutomation", r.masterAutomation}, {"normalizeGainDb", r1(r.normalizeGainDb)}, {"loudnessGainDb", r1(r.loudnessGainDb)}}},
                   {"sections", sections}, {"tracks", tracks}, {"buses", buses}, {"warnings", r.warnings},
                   {"dropouts", dropouts}, {"failedTracks", r.failedTracks}};
    if (!fallbackNotes.empty()) report["fallbacks"] = fallbackNotes;
    if (!r.deliveries.empty()) report["mix"]["deliveries"] = deliveriesJson(r.deliveries);
    if (!r.pictureFile.empty()) report["picture"] = {{"file", r.pictureFile}, {"width", r.pictureWidth}, {"height", r.pictureHeight}};
    if (!only.empty()) report["onlyTracks"] = only;
    if (job.window.on) {   // the files hold bars from..to only; songStart = where that is in the song (seconds)
        const double bpb = job.tsigNum * 4.0 / job.tsigDen;
        report["window"] = {{"fromBar", r1(job.window.fromBeat / bpb + 1)}, {"toBar", r1(job.window.toBeat / bpb + 1)},
                            {"fromBeat", job.window.fromBeat}, {"toBeat", job.window.toBeat}, {"songStart", std::round(job.window.songStartSec * 1e6) / 1e6},
                            {"seconds", std::round((job.window.loop ? job.window.loopSec : job.length - job.window.trimSec) * 1000) / 1000}};
        if (job.window.loop)
            report["window"]["loop"] = {{"seconds", std::round(job.window.loopSec * 1e6) / 1e6},
                                        {"frames", (long long)std::llround(job.window.loopSec * job.sampleRate)},
                                        {"tailFolded", std::round((job.length - job.window.loopSec) * 1000) / 1000}};
    }
    if (!complete) report["error"] = incomplete;
    std::ofstream(fs::path(outDir) / "report.json") << report.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    int songRev = 0;   // a full render of a song is a revision (docs/song-format.md): the report names it
    if (complete && only.empty() && !job.window.on && !a.has("--fallbacks")) {   // stand-ins on purpose are not the song
        std::string herr;
        songRev = history::recordRender(path, outDir, report, herr);
        if (!herr.empty()) std::fprintf(stderr, "warning: song history: %s\n", herr.c_str());
        if (a.has("--keep") && songRev > 0 && !package::keepRender(keepSong, outDir, report, songRev, herr))
            std::fprintf(stderr, "warning: --keep: %s\n", herr.c_str());
    }
    if (a.has("--json")) { emit(report.dump(2, ' ', false, json::error_handler_t::replace)); return complete ? 0 : 1; }
    if (term::out().on) return printRenderSummary(r, songRev, complete, incomplete);
    for (auto &t : r.tracks) {
        std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  %s\n", t.name.c_str(), t.pluginName.c_str(), t.levels.peakDb,
                    t.lufs, t.file.c_str());
        for (auto &w : t.warnings) std::fprintf(OUT, "    ! %s\n", w.c_str());
    }
    for (auto &b : r.buses)
        std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  %s\n", ("bus: " + b.name).c_str(), "", b.levels.peakDb, b.lufs, b.file.c_str());
    std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  LRA %.1f LU  true peak %.1f dBTP  %s\n", "MIX", "", r.mix.peakDb, r.mixLufs,
                 r.mixLra, r.truePeakDb, r.mixFile.c_str());
    for (auto &d : r.deliveries)
        std::fprintf(OUT, "%-24s %-20s %6.1f LUFS  true peak %.1f dBTP (%+.1f)  %s\n", ("  " + d.spec.format).c_str(), "", d.lufs, d.truePeakDb,
                     d.overshootDb, d.file.c_str());
    for (auto &sec : r.sections) std::fprintf(OUT, "    section %-18s %6.1f LUFS  (%.1f–%.1f s)\n", sec.name.c_str(), sec.lufs, sec.start, sec.end);
    for (auto &w : r.warnings) std::fprintf(OUT, "    ! %s\n", w.c_str());
    if (!r.pictureFile.empty()) std::fprintf(OUT, "picture %s\n", r.pictureFile.c_str());
    if (songRev) std::fprintf(OUT, "song revision %d\n", songRev);
    std::fprintf(OUT, "%.2f s of audio rendered in %.2f s\n", r.seconds, r.renderSeconds);
    if (!complete) { std::fprintf(stderr, "error: %s\n", incomplete.c_str()); return 1; }
    return 0;
}

// ---- lint --harmony: keys, chords, one-bar excursions and clashes ---------------------------
int lintHarmony(const Args &a, const Job &job, const std::vector<size_t> &tracks, double fromBeat, double toBeat, const json &skipped) {
    HarmonyOptions o;
    o.tracks = tracks;
    o.keys = job.keys;
    o.fromBeat = fromBeat;
    o.toBeat = toBeat;
    if (a.has("--key")) {   // one key for the whole song, instead of the job's "keys" or detection
        KeyMark k{0, 0, false, -1, true};
        std::string err;
        if (!parseKeyName(a.get("--key"), k.tonic, k.minor, err, &k.mode)) return fail(a, "--key: " + err);
        o.keys = {k};
    }
    if (a.has("--max-bars")) o.maxExcursionBars = std::max(1, std::atoi(a.get("--max-bars").c_str()));
    const json r = analyzeHarmony(job, o);
    json names = json::array();
    for (size_t i : tracks) names.push_back(job.tracks[i].name);
    if (a.has("--json")) {
        json out = {{"ok", true}, {"tracks", names}, {"skipped", skipped}};
        out.update(r);
        if (!a.has("--chords")) out.erase("bars");
        emit(out.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    std::fprintf(OUT, "tracks: %s\n", names.dump().c_str());
    if (!skipped.empty()) {
        std::string sk;
        for (auto &x : skipped) sk += (sk.empty() ? "" : ", ") + x["track"].get<std::string>() + " (" + x["reason"].get<std::string>() + ")";
        std::fprintf(OUT, "skipped: %s\n", sk.c_str());
    }
    for (auto &k : r["keys"])
        std::fprintf(OUT, "key: %-9s bars %d-%d (%s%s)\n", k["key"].get<std::string>().c_str(), k["from"].get<int>(), k["to"].get<int>(),
                     k["source"].get<std::string>().c_str(), k.contains("checks") ? ", checks off" : "");
    if (a.has("--chords")) {   // a chord chart, 8 bars a line; "F>E" = the chord changes halfway through the bar
        std::string line;
        int n = 0;
        for (auto &b : r["bars"]) {
            if (n % 8 == 0) { if (!line.empty()) std::fprintf(OUT, "%s\n", line.c_str()); char h[16]; std::snprintf(h, sizeof h, "  %4d:", b["bar"].get<int>()); line = h; }
            std::string c = b.contains("halves") ? b["halves"][0].get<std::string>() + ">" + b["halves"][1].get<std::string>() : b["chord"].get<std::string>();
            if (b.contains("outside")) c += "!";
            char cell[24];
            std::snprintf(cell, sizeof cell, " %-9s", c.c_str());
            line += cell;
            ++n;
        }
        if (!line.empty()) std::fprintf(OUT, "%s\n", line.c_str());
    }
    for (auto &p : r["problems"])
        std::fprintf(OUT, "  %-14s %-20s %s\n", p["kind"].get<std::string>().c_str(), p["at"].get<std::string>().c_str(), p["detail"].get<std::string>().c_str());
    for (auto &p : r["info"])
        std::fprintf(OUT, "  (ok) %-19s %-20s %s\n", p["kind"].get<std::string>().c_str(), p["at"].get<std::string>().c_str(), p["detail"].get<std::string>().c_str());
    for (auto &rb : r["rubs"]) {   // in-key semitone rubs, one line per pair of tracks
        std::string bars;
        size_t shown = 0;
        for (auto &b : rb["bars"]) { if (shown++ == 8) { bars += ", ..."; break; } bars += (bars.empty() ? "" : ", ") + std::to_string(b.get<int>()); }
        std::fprintf(OUT, "  rub %-28s %3dx  bars %s  e.g. %s\n", (rb["tracks"][0].get<std::string>() + " / " + rb["tracks"][1].get<std::string>()).c_str(),
                     rb["count"].get<int>(), bars.c_str(), rb["example"].get<std::string>().c_str());
    }
    std::fprintf(OUT, "%zu problem%s\n", r["problems"].size(), r["problems"].size() == 1 ? "" : "s");
    return 0;
}

// ---- timeline: bars and markers -> song time, from the tempo map ------------------------------
// wavelength kit [install [names...] | remove NAME]: free instruments into Wavelength's own folder
int cmdKit(const Args &a) {
    const std::string sub = a.positional.size() > 1 ? a.positional[1] : "";
    std::string err;
    if (sub.empty() || sub == "list") {
        const json list = kitList();
        if (a.has("--json")) { emit(json{{"ok", true}, {"folder", kitDir().string()}, {"kit", list}}.dump(2)); return 0; }
        for (auto &e : list) {
            const term::Style &st = term::out();
            const std::string status = e["status"].get<std::string>();
            if (st.on) {
                const bool installed = status.rfind("installed", 0) == 0;
                std::fprintf(OUT, "%s %s %s %s %s  %s %s\n            %s\n", st.accent(col(e["name"].get<std::string>(), 10)).c_str(),
                             st.bold(col(e["title"].get<std::string>(), 18)).c_str(), st.dim(col(e["version"].get<std::string>(), 7)).c_str(),
                             st.dim(col(e["license"].get<std::string>(), 17)).c_str(), st.dim(col(std::to_string(e["downloadMB"].get<int>()) + " MB", 7)).c_str(),
                             installed ? st.ok().c_str() : st.dim("\u25cb").c_str(), installed ? st.green(status).c_str() : status.c_str(),
                             st.dim(e["about"].get<std::string>()).c_str());
                continue;
            }
            std::fprintf(OUT, "%-10s %-18s %-7s %-17s %4d MB  %s\n            %s\n", e["name"].get<std::string>().c_str(), e["title"].get<std::string>().c_str(),
                         e["version"].get<std::string>().c_str(), e["license"].get<std::string>().c_str(), e["downloadMB"].get<int>(),
                         status.c_str(), e["about"].get<std::string>().c_str());
        }
        std::fprintf(OUT, "\nwavelength kit install [names] downloads them from their own releases into %s (never the system's plugin folders).\n",
                     kitDir().string().c_str());
        return 0;
    }
    if (sub == "install") {
        json r;
        if (!kitInstall(std::vector<std::string>(a.positional.begin() + 2, a.positional.end()), a.has("--force"), r, err)) return fail(a, err);
        if (a.has("--json")) { r["ok"] = true; emit(r.dump(2)); return 0; }
        for (auto &i : r["installed"]) std::fprintf(OUT, "installed %s %s in %s\n", i["name"].get<std::string>().c_str(), i["version"].get<std::string>().c_str(), i["folder"].get<std::string>().c_str());
        for (auto &s : r["skipped"]) std::fprintf(OUT, "skipped   %s: %s\n", s["name"].get<std::string>().c_str(), s["why"].get<std::string>().c_str());
        for (auto &p : r["problems"]) std::fprintf(OUT, "! %s does not load: %s\n", p["plugin"].get<std::string>().c_str(), p["error"].get<std::string>().c_str());
        if (r.contains("fix")) std::fprintf(OUT, "  fix: %s\n", r["fix"].get<std::string>().c_str());
        return 0;
    }
    if (sub == "remove") {
        if (a.positional.size() < 3) return fail(a, "usage: wavelength kit remove <name>");
        if (!kitRemove(a.positional[2], err)) return fail(a, err);
        if (a.has("--json")) emit(json{{"ok", true}, {"removed", a.positional[2]}}.dump(2));
        else std::fprintf(OUT, "removed %s\n", a.positional[2].c_str());
        return 0;
    }
    return fail(a, "usage: wavelength kit [list | install [names...] [--force] | remove <name>]");
}

// wavelength docs [name] [--section TEXT]: the agent docs built into this binary
int cmdDocs(const Args &a) {
    if (a.positional.size() < 2) {
        const auto docs = listDocs();
        if (a.has("--json")) {
            json list = json::array();
            for (auto &d : docs) list.push_back({{"name", d.name}, {"title", d.title}, {"bytes", d.size}, {"sections", d.headings}});
            emit(json{{"ok", true}, {"docs", list}}.dump(2));
            return 0;
        }
        for (auto &d : docs) {
            std::fprintf(OUT, "%-11s %s (%zu KB)\n", d.name.c_str(), d.title.c_str(), (d.size + 512) / 1024);
            std::string heads;
            for (auto &h : d.headings) heads += (heads.empty() ? "" : " | ") + h;
            std::fprintf(OUT, "            %s\n", heads.c_str());
        }
        std::fprintf(OUT, "\nwavelength docs agents (the operating guide; read it in full first), docs job-format, docs effects, docs song-format; --section TEXT prints one section.\n");
        return 0;
    }
    std::string text, err;
    if (!docText(a.positional[1], a.get("--section"), text, err)) return fail(a, err);
    if (a.has("--json")) emit(json{{"ok", true}, {"doc", a.positional[1]}, {"text", text}}.dump(2));
    else std::fputs(text.c_str(), OUT);
    return 0;
}

// wavelength picture job.json: the arrangement as an image before rendering (render --png draws the full one)
int cmdPicture(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength picture <job.json> [--out FILE.png] [--width PX] [--json]");
    const std::string path = a.positional[1];
    std::ifstream in(path);
    if (!in) return fail(a, "cannot read " + path);
    json j;
    try { in >> j; } catch (const std::exception &e) { return fail(a, std::string("job is not valid JSON: ") + e.what()); }
    Job job;
    std::string err;
    if (!parseJob(j, fs::absolute(path).parent_path().string(), job, err)) return fail(a, err);
    double end = 0;
    for (const auto &t : job.tracks) for (const auto &n : t.notes) end = std::max(end, n.start + n.length);
    for (const auto &t : job.tracks) if (!t.clips.empty()) end = std::max(end, clipsEndSeconds(job, t));
    Picture pic;
    const fs::path src = fs::absolute(path);
    pic.title = titleOfJob(src);
    pic.width = std::atoi(a.get("--width", std::to_string(job.pictureWidth)).c_str());
    pic.seconds = job.length > 0 ? job.length : end + job.tail;
    const std::string out = a.get("--out", (src.parent_path() / "arrangement.png").string());
    int h = 0;
    if (!writePicture(out, job, pic, h, err)) return fail(a, err);
    const int w = std::clamp(pic.width, 800, 3200);
    if (a.has("--json")) emit(json{{"ok", true}, {"file", out}, {"width", w}, {"height", h}}.dump(2));
    else std::fprintf(OUT, "%s (%d x %d)\n", out.c_str(), w, h);
    return 0;
}

int cmdStage(const Args &a) {
    const char *usage = "usage: wavelength stage <job.json> [--targets FILE] [--report FILE] [--out DIR] [--write FILE] [--apply] [--dry-run] [--jobs N] [--json]";
    if (a.positional.size() < 2) return fail(a, usage);
    StageOptions o;
    o.job = a.positional[1];
    o.targets = a.get("--targets");
    o.report = a.get("--report");
    o.out = a.get("--out");
    o.write = a.get("--write");
    o.apply = a.has("--apply");
    o.dryRun = a.has("--dry-run");
    o.verbose = a.has("--verbose");
    if (a.has("--jobs")) o.jobs = std::atoi(a.get("--jobs").c_str());
    json rep;
    std::string err;
    if (!stageGains(o, rep, err)) return fail(a, err);
    if (a.has("--json")) { emit(rep.dump(2)); return 0; }
    std::fprintf(OUT, "Faders from %s (targets: role defaults%s)\n", rep["report"].get<std::string>().c_str(),
                 rep["targets"].empty() ? "" : (" + " + rep["targets"].back().get<std::string>()).c_str());
    for (auto &t : rep["tracks"])
        std::fprintf(OUT, "  %-16s %-11s stem %6.1f LUFS  target %6.1f  gain %+5.1f (was %+.1f)%s\n", t["name"].get<std::string>().c_str(),
                     t["role"].get<std::string>().c_str(), t["lufs"].get<double>(), t["target"].get<double>(), t["gain"].get<double>(),
                     t["was"].get<double>(), t.contains("cappedByPeak") ? "  (peak cap)" : "");
    for (auto &b : rep["buses"])
        std::fprintf(OUT, "  bus %-12s             stem %6.1f LUFS  target %6.1f  gain %+5.1f (was %+.1f)\n", b["name"].get<std::string>().c_str(),
                     b["lufs"].get<double>(), b["target"].get<double>(), b["gain"].get<double>(), b["was"].get<double>());
    if (rep["gainsFile"].is_string()) std::fprintf(OUT, "Wrote %s%s\n", rep["gainsFile"].get<std::string>().c_str(), rep["applied"].get<bool>() ? " and set the gains in the job" : "");
    else std::fprintf(OUT, "Dry run: nothing written\n");
    for (auto &w : rep["warnings"]) std::fprintf(OUT, "warning: %s\n", w.get<std::string>().c_str());
    return 0;
}

int cmdCard(const Args &a) {
    const char *usage = "usage: wavelength card <plugin> [<preset>...] [+ <plugin> [<preset>...]]... [--state FILE] [--out FILE.png] [--width PX] [--jobs N] [--probe DIR] [--json]";
    if (a.positional.size() < 2) return fail(a, usage);
    CardOptions o;
    std::string plugin;
    bool wantPlugin = true, hadPreset = false;
    auto flush = [&] { if (!plugin.empty() && !hadPreset) o.items.push_back({plugin, "", ""}); };
    for (size_t i = 1; i < a.positional.size(); ++i) {
        const std::string &s = a.positional[i];
        if (s == "+") { flush(); wantPlugin = true; continue; }
        if (wantPlugin) { plugin = s; wantPlugin = false; hadPreset = false; continue; }
        o.items.push_back({plugin, s, ""});
        hadPreset = true;
    }
    flush();
    if (o.items.empty()) return fail(a, usage);
    if (a.has("--state")) {
        if (o.items.size() != 1 || !o.items[0].preset.empty()) return fail(a, "--state draws one plugin: wavelength card <plugin> --state FILE");
        o.items[0].state = a.get("--state");
    }
    o.out = a.get("--out", o.items.size() == 1 ? "card.png" : "cards.png");
    o.width = std::atoi(a.get("--width", "1400").c_str());
    if (a.has("--jobs")) o.jobs = std::atoi(a.get("--jobs").c_str());
    o.keep = a.get("--probe");
    o.verbose = a.has("--verbose");
    json rep;
    std::string err;
    if (!makeCards(o, rep, err)) return fail(a, err);
    if (a.has("--json")) { emit(rep.dump(2)); return 0; }
    std::fprintf(OUT, "%s (%d x %d)\n", rep["file"].get<std::string>().c_str(), rep["width"].get<int>(), rep["height"].get<int>());
    for (auto &p : rep["patches"]) {
        std::string name = p.value("preset", p.value("state", std::string("default")));
        std::fprintf(OUT, "  %s / %s: ", p.value("name", std::string()).c_str(), name.c_str());
        if (!p.value("ok", false)) { std::fprintf(OUT, "failed: %s\n", p.value("error", std::string()).c_str()); continue; }
        const json &h = p["held"];
        std::fprintf(OUT, "C4 sounds %s, %.1f LUFS, attack %.0f ms, tail %.2f s, width %.2f, brightness %.0f Hz\n",
                     p["sounds"].is_string() ? p["sounds"].get<std::string>().c_str() : "no pitch", h.value("lufs", -120.0),
                     p.value("attackMs", 0.0), p.value("tailMs", 0.0) / 1000, h["stereo"].value("width", 0.0), h["spectrum"].value("centroidHz", 0.0));
        for (auto &f : p["flags"]) std::fprintf(OUT, "      %s\n", f.get<std::string>().c_str());
    }
    return 0;
}

int cmdTimeline(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength timeline <job.json> [--every BARS] [--json]");
    const std::string path = a.positional[1];
    std::ifstream in(path);
    if (!in) return fail(a, "cannot read " + path);
    json j;
    try { in >> j; } catch (const std::exception &e) { return fail(a, std::string("job is not valid JSON: ") + e.what()); }
    Job job;
    std::string err;
    if (!parseJob(j, fs::absolute(path).parent_path().string(), job, err)) return fail(a, err);
    const int every = std::max(1, std::atoi(a.get("--every", "8").c_str()));
    const double bpb = job.tsigNum * 4.0 / job.tsigDen;
    double end = 0;   // the render's length: the last note or clip plus the tail, or "length"
    for (const auto &t : job.tracks) for (const auto &n : t.notes) end = std::max(end, n.start + n.length);
    for (const auto &t : job.tracks) if (!t.clips.empty()) end = std::max(end, clipsEndSeconds(job, t));
    const double lastSound = end, songEnd = job.length > 0 ? job.length : end + job.tail;
    auto mmss = [](double s) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%d:%05.2f", (int)(s / 60), std::fmod(s, 60.0));
        return std::string(buf);
    };
    auto row = [&](double beat, const std::string &kind, const std::string &name) {
        const double sec = job.tempo.beatToSec(beat), bar = std::floor(beat / bpb + 1e-9);
        return json{{"kind", kind}, {"name", name}, {"bar", (int)bar + 1}, {"beatInBar", std::round((beat - bar * bpb + 1) * 1000) / 1000},
                    {"beat", std::round(beat * 1000) / 1000}, {"seconds", std::round(sec * 1000) / 1000},
                    {"fileSeconds", std::round((sec + job.leadIn) * 1000) / 1000}, {"time", mmss(sec)},
                    {"bpm", std::round(job.tempo.bpmAtBeat(beat) * 100) / 100}};
    };
    std::vector<json> rows;
    const double lastBeat = job.tempo.secToBeat(songEnd);
    for (double b = 0; b <= lastBeat + 1e-9; b += every * bpb) rows.push_back(row(b, "bar", ""));
    for (const auto &m : job.markers) rows.push_back(row(m.beat, "marker", m.name));
    {
        json r = row(job.tempo.secToBeat(lastSound), "last sound", "");
        rows.push_back(r);
        r = row(lastBeat, "end", "");
        r["seconds"] = std::round(songEnd * 1000) / 1000;   // exact, not through the beat round trip
        rows.push_back(r);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const json &x, const json &y) { return x["beat"].get<double>() < y["beat"].get<double>(); });
    if (a.has("--json")) {
        json out = {{"ok", true}, {"leadIn", job.leadIn}, {"beatsPerBar", bpb}, {"seconds", std::round(songEnd * 1000) / 1000},
                    {"duration", std::round((songEnd + job.leadIn) * 1000) / 1000}, {"rows", rows}};
        emit(out.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    std::fprintf(OUT, "%-10s %-24s %6s %8s %9s  %10s %7s\n", "", "", "bar|beat", "beat", "song time", "file time", "bpm");
    for (auto &r : rows) {
        const std::string label = r["kind"] == "marker" ? r["name"].get<std::string>() : r["kind"] == "bar" ? "" : r["kind"].get<std::string>();
        char bar[24];
        std::snprintf(bar, sizeof bar, "%d|%g", r["bar"].get<int>(), r["beatInBar"].get<double>());
        std::fprintf(OUT, "%-10s %-24.24s %6s %8g %9s  %10s %7g\n", r["kind"] == "marker" ? "marker" : "", label.c_str(), bar,
                     r["beat"].get<double>(), r["time"].get<std::string>().c_str(), mmss(r["fileSeconds"].get<double>()).c_str(), r["bpm"].get<double>());
    }
    std::fprintf(OUT, "song %s (%s with the %.1f s lead-in)\n", mmss(songEnd).c_str(), mmss(songEnd + job.leadIn).c_str(), job.leadIn);
    return 0;
}

// ---- lint: voice leading between melodic tracks --------------------------------------------
// Each track is one voice (its highest sounding note; "--low" names tracks read by their lowest,
// for basses). At every onset where two voices both move, a perfect fifth or octave (or unison)
// before and after, in similar motion, is a parallel. With --crossings the --tracks order is
// high to low and a lower voice above a higher one is reported.
int cmdLint(const Args &a) {
    if (a.positional.size() < 2)
        return fail(a, "usage: wavelength lint <job.json> [--tracks \"Soprano,Alto,Bass\"] [--low \"Bass\"] [--split \"Organ=4\"] [--from BAR] [--to BAR] [--section NAME] [--crossings] | --harmony [--key \"D minor\"] [--ignore \"SFX\"] [--chords]");
    const std::string path = a.positional[1];
    std::ifstream in(path);
    if (!in) return fail(a, "cannot read " + path);
    json j;
    try { in >> j; } catch (const std::exception &e) { return fail(a, std::string("job is not valid JSON: ") + e.what()); }
    Job job;
    std::string err;
    if (!parseJob(j, fs::absolute(path).parent_path().string(), job, err)) return fail(a, err);
    auto split = [](const std::string &s) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        for (std::string n; std::getline(ss, n, ',');) {
            n.erase(0, n.find_first_not_of(' '));
            n.erase(n.find_last_not_of(' ') + 1);
            if (!n.empty()) out.push_back(n);
        }
        return out;
    };
    const auto low = split(a.get("--low"));
    std::map<std::string, int> splits;   // "Organ=4": the chord track read as 4 voices, top to bottom
    for (auto &s : split(a.get("--split"))) {
        const size_t eq = s.find('=');
        if (eq == std::string::npos || std::atoi(s.c_str() + eq + 1) < 2) return fail(a, "--split takes \"Track=N\" (N voices, 2 or more)");
        splits[s.substr(0, eq)] = std::atoi(s.c_str() + eq + 1);
    }
    // a voice: a track's top note, its lowest (--low), or the rank-th note from the top of a split chord track
    struct Voice { const Track *t; std::string name; int rank; bool lowest; };
    std::vector<Voice> voices;
    json skipped = json::array();   // tracks left out, with the reason
    auto addTrack = [&](const Track &t) {
        auto sp = splits.find(t.name);
        if (sp != splits.end()) for (int r = 0; r < sp->second; ++r) voices.push_back({&t, t.name + "." + std::to_string(r + 1), r, false});
        else voices.push_back({&t, t.name, -1, std::find(low.begin(), low.end(), t.name) != low.end()});
    };
    if (a.has("--tracks")) {
        for (auto &n : split(a.get("--tracks"))) {
            auto it = std::find_if(job.tracks.begin(), job.tracks.end(), [&](const Track &t) { return t.name == n; });
            if (it == job.tracks.end()) return fail(a, "--tracks: no track named '" + n + "'");
            addTrack(*it);
        }
    } else
        for (auto &t : job.tracks) {   // melodic tracks: not drums, kits, effects or unpitched samples
            if (t.notes.empty()) continue;
            const std::string why = unpitchedReason(t, job.baseDir);
            if (why.empty()) addTrack(t);
            else skipped.push_back({{"track", t.name}, {"reason", why}});
        }
    for (auto &sp : splits) {
        const std::string n = sp.first;
        if (std::none_of(voices.begin(), voices.end(), [&](const Voice &v) { return v.t->name == n; })) return fail(a, "--split: no track named '" + n + "' among the voices");
    }
    // range: --from/--to bars (from inclusive, to exclusive) or a section's markers
    double fromBeat = -1e18, toBeat = 1e18;
    if (a.has("--from")) fromBeat = (std::atof(a.get("--from").c_str()) - 1) * 4;
    if (a.has("--to")) toBeat = (std::atof(a.get("--to").c_str()) - 1) * 4;
    if (a.has("--section")) {
        const std::string want = a.get("--section");
        size_t m = 0;
        for (; m < job.markers.size() && job.markers[m].name != want; ++m) {}
        if (m == job.markers.size()) return fail(a, "--section: no marker named '" + want + "'");
        fromBeat = job.markers[m].beat;
        toBeat = m + 1 < job.markers.size() ? job.markers[m + 1].beat : 1e18;
    }
    if (a.has("--harmony")) return lintHarmony(a, job, [&] {
        std::vector<size_t> idx;
        const auto ignore = split(a.get("--ignore"));
        for (auto &v : voices) {
            const size_t i = (size_t)(v.t - job.tracks.data());
            if (std::find(idx.begin(), idx.end(), i) == idx.end() && std::find(ignore.begin(), ignore.end(), v.t->name) == ignore.end()) idx.push_back(i);
        }
        return idx;
    }(), fromBeat, toBeat, skipped);
    auto noteAt = [&](const Voice &v, double t) {
        std::vector<int> keys;
        for (auto &n : v.t->notes)
            if (n.start <= t + 1e-6 && t < n.start + n.length - 1e-6) keys.push_back(n.key);
        if (keys.empty()) return -1;
        std::sort(keys.begin(), keys.end(), std::greater<int>());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        if (v.rank >= 0) return v.rank < (int)keys.size() ? keys[(size_t)v.rank] : -1;
        return v.lowest ? keys.back() : keys.front();
    };
    auto starts = [&](const Voice &v) {
        std::set<double> s;
        for (auto &n : v.t->notes) s.insert(std::round(n.start * 1e4) / 1e4);
        return s;
    };
    auto where = [&](double t) {
        const double beat = std::round(job.tempo.secToBeat(t) * 1000) / 1000;   // 71.99999 is bar 19 beat 1, not bar 18 beat 5
        char buf[48];
        std::snprintf(buf, sizeof buf, "bar %d beat %.2f", (int)std::floor(beat / 4) + 1, beat - 4 * std::floor(beat / 4) + 1);
        return std::string(buf);
    };
    auto inRange = [&](double t) { const double b = job.tempo.secToBeat(t); return b >= fromBeat - 1e-6 && b < toBeat - 1e-6; };
    json problems = json::array();
    std::map<std::string, int> counts;
    std::map<std::pair<size_t, size_t>, int> moves, par8, par5;
    struct Found { size_t x, y; int ic; double t0, t1; int a0, a1, b0, b1; };
    std::vector<Found> found;
    for (size_t x = 0; x < voices.size(); ++x)
        for (size_t y = x + 1; y < voices.size(); ++y) {
            if (voices[x].t == voices[y].t && voices[x].rank < 0) continue;
            // this pair's own verticalities: where either of the two starts a note (other voices don't split them)
            std::set<double> s = starts(voices[x]);
            for (double t : starts(voices[y])) s.insert(t);
            const std::vector<double> on(s.begin(), s.end());
            for (size_t k = 1; k < on.size(); ++k) {
                const double t0 = on[k - 1], t1 = on[k];
                const int a0 = noteAt(voices[x], t0), a1 = noteAt(voices[x], t1), b0 = noteAt(voices[y], t0), b1 = noteAt(voices[y], t1);
                if (a0 < 0 || a1 < 0 || b0 < 0 || b1 < 0 || a0 == a1 || b0 == b1) continue;
                const int i0 = std::abs(a0 - b0) % 12, i1 = std::abs(a1 - b1) % 12;
                const bool similar = (a1 - a0 > 0) == (b1 - b0 > 0);
                ++moves[{x, y}];
                if (similar && i0 == i1 && (i0 == 0 || i0 == 7)) {
                    ++(i0 == 7 ? par5 : par8)[{x, y}];
                    found.push_back({x, y, i0, t0, t1, a0, a1, b0, b1});
                }
            }
        }
    if (a.has("--crossings")) {
        std::set<double> all;
        for (auto &v : voices) for (double t : starts(v)) all.insert(t);
        for (double t : all) {
            if (!inRange(t)) continue;
            for (size_t x = 0; x + 1 < voices.size(); ++x) {
                const int hi = noteAt(voices[x], t), lo = noteAt(voices[x + 1], t);
                if (hi >= 0 && lo >= 0 && lo > hi) {
                    problems.push_back({{"kind", "voice crossing"}, {"voices", {voices[x].name, voices[x + 1].name}}, {"at", where(t)},
                                        {"time", std::round(t * 1000) / 1000}, {"notes", json::array({keyName(hi), keyName(lo)})}});
                    ++counts["voice crossing"];
                }
            }
        }
    }
    json doublings = json::array();
    auto doubling = [&](size_t x, size_t y, int ic) {
        const int m = moves[{x, y}], p = ic == 7 ? par5[{x, y}] : par8[{x, y}];
        return m >= 4 && p * 2 > m;   // parallel on most of their shared moves: written as a doubling
    };
    for (auto &[pair, m] : moves)
        for (int ic : {0, 7})
            if (doubling(pair.first, pair.second, ic))
                doublings.push_back({{"voices", {voices[pair.first].name, voices[pair.second].name}}, {"interval", ic ? "fifths" : "octaves"}});
    for (auto &f : found) {
        if (doubling(f.x, f.y, f.ic) || !inRange(f.t1)) continue;
        const std::string kind = f.ic == 7 ? "parallel fifths" : "parallel octaves";
        problems.push_back({{"kind", kind}, {"voices", {voices[f.x].name, voices[f.y].name}}, {"at", where(f.t1)}, {"from", where(f.t0)},
                            {"time", std::round(f.t1 * 1000) / 1000},
                            {"notes", json::array({json::array({keyName(f.a0), keyName(f.b0)}), json::array({keyName(f.a1), keyName(f.b1)})})}});
        ++counts[kind];
    }
    std::stable_sort(problems.begin(), problems.end(), [](const json &p, const json &q) { return p["time"].get<double>() < q["time"].get<double>(); });
    json names = json::array();
    for (auto &v : voices) names.push_back(v.name);
    if (a.has("--json")) {
        emit(json{{"ok", true}, {"voices", names}, {"skipped", skipped}, {"counts", counts}, {"doublings", doublings}, {"problems", problems}}.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    std::fprintf(OUT, "voices: %s\n", names.dump().c_str());
    if (!skipped.empty()) {
        std::string sk;
        for (auto &x : skipped) sk += (sk.empty() ? "" : ", ") + x["track"].get<std::string>() + " (" + x["reason"].get<std::string>() + ")";
        std::fprintf(OUT, "skipped: %s\n", sk.c_str());
    }
    for (auto &d : doublings)
        std::fprintf(OUT, "  doubling (ignored): %s / %s in %s\n", d["voices"][0].get<std::string>().c_str(), d["voices"][1].get<std::string>().c_str(),
                     d["interval"].get<std::string>().c_str());
    for (auto &p : problems) {
        std::string detail;
        if (p.contains("from"))   // "G4/C4 (bar 3 beat 1.00) -> A4/D4"
            detail = p["notes"][0][0].get<std::string>() + "/" + p["notes"][0][1].get<std::string>() + " (" + p["from"].get<std::string>() + ") -> " +
                     p["notes"][1][0].get<std::string>() + "/" + p["notes"][1][1].get<std::string>();
        else detail = p["notes"][0].get<std::string>() + " under " + p["notes"][1].get<std::string>();
        std::fprintf(OUT, "  %-17s %-26s %-20s %s\n", p["kind"].get<std::string>().c_str(),
                     (p["voices"][0].get<std::string>() + " / " + p["voices"][1].get<std::string>()).c_str(), p["at"].get<std::string>().c_str(), detail.c_str());
    }
    std::fprintf(OUT, "%zu problem%s\n", problems.size(), problems.size() == 1 ? "" : "s");
    return 0;
}

// ---- state save ------------------------------------------------------------------------
int cmdState(const Args &a) {
    if (a.positional.size() < 3 || a.positional[1] != "save") return fail(a, "usage: wavelength state save <plugin> --out FILE");
    if (!a.has("--out")) return fail(a, "state save needs --out FILE (.clap-preset for CLAP, .vstpreset for VST3)");
    PluginInfo info;
    std::string err;
    auto inst = openForInspection(a, a.positional[2], info, err);
    if (!inst) return fail(a, err);
    std::vector<ParamValue> values;
    for (const auto &s : a.sets) {
        auto eq = s.rfind('=');
        if (eq == std::string::npos) return fail(a, "--set expects \"Name=value\", got '" + s + "'");
        ParamInfo pi;
        if (!inst->findParam(s.substr(0, eq), pi)) return fail(a, "no parameter '" + s.substr(0, eq) + "' on " + info.name);
        values.push_back({pi.id, pi.cookie, std::clamp(std::stod(s.substr(eq + 1)), std::min(pi.min, pi.max), std::max(pi.min, pi.max))});
    }
    if (!inst->setParams(values, err)) return fail(a, err);
    if (!values.empty() && !inst->commitParams(values, 48000, 512, err)) return fail(a, err);
    inst->pump(100);
    size_t bytes = 0;
    if (!inst->saveStateFile(a.get("--out"), bytes, err)) return fail(a, err);
    if (a.has("--json")) emit(json{{"ok", true}, {"plugin", info.id}, {"format", info.format}, {"file", a.get("--out")}, {"bytes", bytes}}.dump(2, ' ', false, json::error_handler_t::replace));
    else std::fprintf(OUT, "saved %zu bytes of %s state to %s\n", bytes, info.name.c_str(), a.get("--out").c_str());
    return 0;
}

} // namespace

int run(int argc, char **argv);

// Plugins (JUCE ones especially) often crash in their static destructors when a host process
// exits. Everything useful is written by then, so leave without running them.
int main(int argc, char **argv) {
    platform::init(argc, argv);
    const int code = run(argc, argv);
    std::fflush(OUT);
    std::fflush(stderr);
    platform::quickExit(code);
}

int run(int argc, char **argv) {
    OUT = platform::takeStdout();
    Args a = parse(argc, argv);
    term::init(OUT, a.has("--json"));
    // help: the short list, `help <command>`, `<command> --help` or -h, `help all`
    const bool dashH = std::find(a.positional.begin(), a.positional.end(), "-h") != a.positional.end();
    if (a.positional.empty() || a.positional[0] == "help" || a.positional[0] == "-h" || a.has("--help") || dashH) {
        std::string topic;
        if (!a.positional.empty() && a.positional[0] == "help") topic = a.positional.size() > 1 ? a.positional[1] : "";
        else if (!a.positional.empty() && a.positional[0] != "-h") topic = a.positional[0];
        if (a.has("--all")) topic = "all";
        const int code = printHelp(OUT, topic, WAVELENGTH_VERSION);
        return a.positional.empty() ? 1 : code;
    }
    const std::string cmd = a.positional[0];
    if (isSongCommand(cmd)) return runSongCommand(argc, argv, OUT);   // save, history, undo... (song_cli.cpp)
    if (cmd == "__save-state" && a.positional.size() > 3) return saveStateWorker(a.positional[1], a.positional[2], a.positional[3], OUT);
    if (cmd == "__play" && a.positional.size() > 2) return playWorker(a.positional[1], a.positional[2], OUT);   // internal: serve's live notes
    if (cmd == "__live" && a.positional.size() > 2) return liveWorker(a.positional[1], a.positional[2], OUT);   // internal: serve's live playing
    if (cmd == "__track" && a.positional.size() > 3)
        return renderTrackWorker(a.positional[1], std::stoul(a.positional[2]), a.positional[3],
                                 std::vector<std::string>(a.positional.begin() + 4, a.positional.end()));
    if (cmd == "__audition" && a.positional.size() > 3) return auditionWorker(a.positional[1], a.positional[2], a.positional[3]);
    if (cmd == "__scan-vst2" && a.positional.size() > 1) {   // internal: run by `plugins` in a child process
        std::vector<PluginInfo> plugins;
        std::string err;
        json out = {{"ok", scanVst2Bundle(a.positional[1], plugins, err)}, {"plugins", json::array()}};
        for (const auto &p : plugins) out["plugins"].push_back(pluginToJson(p));
        if (!err.empty()) out["error"] = err;
        emit(out.dump());
        std::fflush(OUT);
        platform::quickExit(0);
    }
#if defined(__APPLE__)
    // commands that load one plugin in this process: an Intel-only plugin needs the whole command under
    // Rosetta, so run this same command again as x86_64 (a universal build has both)
    if ((cmd == "params" || cmd == "presets" || cmd == "audition" || cmd == "state" || cmd == "__compat") && !std::getenv("WAVELENGTH_ARCH_REEXEC")) {
        const size_t at = cmd == "state" ? 2 : 1;
        PluginInfo info;
        std::string e;
        if (a.positional.size() > at && resolvePlugin(a.positional[at], info, e) && !info.arch.empty() && info.arch != platform::hostArch()) {
            std::vector<std::string> args;
            if (!platform::archPrefix(info.arch, args, e)) return fail(a, info.name + ": " + e);
            args.push_back(platform::selfExecutable());
            for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
            std::vector<char *> cargs;
            for (auto &s : args) cargs.push_back(s.data());
            cargs.push_back(nullptr);
            setenv("WAVELENGTH_ARCH_REEXEC", "1", 1);
            std::fflush(nullptr);
            dup2(fileno(OUT), STDOUT_FILENO);   // give the real stdout back: fd 1 points at stderr since takeStdout()
            execv(cargs[0], cargs.data());
            return fail(a, "could not run " + info.name + " under Rosetta");
        }
    }
#endif
    // internal: one plugin of `compat`, after the Rosetta re-exec above (an Intel-only plugin's worker runs as x86_64)
    if (cmd == "__compat" && a.positional.size() > 3)
        return compatWorker(a.positional[1], a.positional[2], std::atoi(a.positional[3].c_str()),
                            a.positional.size() > 5 ? std::atoi(a.positional[4].c_str()) : -1, a.positional.size() > 5 ? a.positional[5] : "");
    if (cmd == "__scan-vst3" && a.positional.size() > 1) {   // internal: run by `plugins` in a child process
        std::vector<PluginInfo> plugins;
        std::string err;
        json out = {{"ok", scanVst3Bundle(a.positional[1], plugins, err)}, {"plugins", json::array()}};
        for (const auto &p : plugins) out["plugins"].push_back(pluginToJson(p));
        if (!err.empty()) out["error"] = err;
        emit(out.dump());
        std::fflush(OUT);
        platform::quickExit(0);   // skip plugin static destructors, which some plugins crash in
    }
    {   // an option a command doesn't know is an error: a typo (or a newer flag on an older binary) must not be ignored
        static const std::map<std::string, std::set<std::string>> known = {
            {"plugins", {"--rescan", "--json", "--block", "--unblock", "--reason"}},
            {"params", {"--preset", "--state", "--format", "--all", "--map", "--steps", "--json", "--verbose"}},
            {"presets", {"--search", "--rescan", "--json"}},
            {"samples", {"--search", "--kit", "--roundrobin", "--soundfont", "--install-soundfont", "--force", "--patch", "--json"}},
            {"loops", {"--search", "--key", "--midi", "--notes", "--json"}},
            {"analyze", {"--start", "--end", "--song-time", "--grid", "--div", "--every", "--peaks", "--top", "--json"}},
            {"audition", {"--jobs", "--limit", "--rebuild", "--retag", "--json", "--verbose"}},
            {"compat", {"--format", "--jobs", "--presets", "--timeout", "--report", "--rebuild", "--json", "--verbose"}},
            {"render", {"--out", "--stems", "--deliver", "--jobs", "--tracks", "--level-from", "--from", "--to", "--preroll", "--json", "--verbose", "--bitwig", "--instrument", "--png", "--no-png", "--loop", "--keep", "--fallbacks", "--cache", "--mix"}},
            {"master", {"--chain", "--loudness", "--lead-in", "--input-lead-in", "--out", "--deliver", "--json", "--verbose"}},
            {"state", {"--out", "--preset", "--state", "--format", "--json", "--verbose"}},
            {"import", {"--out", "--json", "--bitwig", "--instrument", "--list"}},
            {"export", {"--out", "--json", "--no-print"}},
            {"serve", {"--port", "--host", "--open", "--ui"}},
            {"lint", {"--tracks", "--low", "--split", "--from", "--to", "--section", "--crossings", "--json", "--harmony", "--key", "--ignore", "--chords", "--max-bars"}},
            {"timeline", {"--every", "--json"}},
            {"picture", {"--out", "--width", "--json"}},
            {"card", {"--out", "--width", "--state", "--jobs", "--probe", "--json", "--verbose"}},
            {"stage", {"--targets", "--report", "--out", "--write", "--apply", "--dry-run", "--jobs", "--json", "--verbose"}},
            {"docs", {"--section", "--json"}},
            {"kit", {"--force", "--json"}},
            {"mcp", {}},
            {"version", {"--json", "--check"}},
            {"upgrade", {"--json", "--check", "--force"}}};
        auto it = known.find(cmd);
        if (it != known.end())
            for (auto &[k, v] : a.opts)
                if (!it->second.count(k)) {
                    std::string list;
                    for (auto &o : it->second) list += (list.empty() ? "" : " ") + o;
                    return fail(a, "unknown option " + k + " for `" + cmd + "` (it takes: " + list + (cmd == "state" || cmd == "params" ? " --set" : "") + ")");
                }
    }
    // after the unknown-option check, so a stray `--typo` at the end is named as unknown, not as missing a value
    if (!a.missing.empty()) return fail(a, a.missing.front() + " needs a value");
    try {
        if (cmd == "plugins") return cmdPlugins(a);
        if (cmd == "params") return cmdParams(a);
        if (cmd == "presets") return cmdPresets(a);
        if (cmd == "samples") return cmdSamples(a);
        if (cmd == "loops") return cmdLoops(a);
        if (cmd == "analyze") return cmdAnalyze(a);
        if (cmd == "audition") return cmdAudition(a);
        if (cmd == "compat") return cmdCompat(a);
        if (cmd == "render") return cmdRender(a);
        if (cmd == "import") return cmdImport(a);
        if (cmd == "export") return cmdExport(a);
        if (cmd == "master") return cmdMaster(a);
        if (cmd == "state") return cmdState(a);
        if (cmd == "lint") return cmdLint(a);
        if (cmd == "serve") {
            ServeOptions o;
            if (a.positional.size() > 1) o.root = a.positional[1];
            if (a.has("--port")) o.port = std::atoi(a.get("--port").c_str());
            if (a.has("--host")) o.host = a.get("--host");
            if (a.has("--ui")) o.uiDir = a.get("--ui");
            o.open = a.has("--open");
            if (o.port <= 0 || o.port > 65535) return fail(a, "--port must be 1-65535");
            return serve(o);
        }
        if (cmd == "timeline") return cmdTimeline(a);
        if (cmd == "picture") return cmdPicture(a);
        if (cmd == "card") return cmdCard(a);
        if (cmd == "stage") return cmdStage(a);
        if (cmd == "docs") return cmdDocs(a);
        if (cmd == "kit") return cmdKit(a);
        if (cmd == "mcp") return runMcp(OUT);
        if (cmd == "version" || cmd == "upgrade") {
            json info = {{"ok", true}, {"name", "wavelength"}, {"version", WAVELENGTH_VERSION}, {"songFormat", kSongFormatVersion}};
            if (cmd == "version" && !a.has("--check")) {
                if (a.has("--json")) emit(info.dump(2));
                else std::fprintf(OUT, "wavelength %s\n", WAVELENGTH_VERSION);
                return 0;
            }
            UpgradeOptions opt;
            opt.check = cmd == "version" || a.has("--check");
            opt.force = a.has("--force");
            json r;
            std::string err;
            if (!selfUpgrade(opt, r, err)) return fail(a, err);
            if (a.has("--json")) { for (auto &[k, v] : r.items()) info[k] = v; emit(info.dump(2)); return 0; }
            const term::Style &st = term::out();
            if (opt.check && st.on)
                std::fprintf(OUT, "%s wavelength %s %s the latest release is %s%s\n", r["upgradeAvailable"].get<bool>() ? st.yellow("\u2191").c_str() : st.ok().c_str(),
                             st.bold(WAVELENGTH_VERSION).c_str(), st.dot().c_str(), st.bold(r["latest"].get<std::string>()).c_str(),
                             r["upgradeAvailable"].get<bool>() ? (": " + st.cyan("wavelength upgrade") + " installs it").c_str() : st.dim(" (up to date)").c_str());
            else if (opt.check)
                std::fprintf(OUT, "wavelength %s; the latest release is %s%s\n", WAVELENGTH_VERSION, r["latest"].get<std::string>().c_str(),
                             r["upgradeAvailable"].get<bool>() ? ": `wavelength upgrade` installs it" : " (up to date)");
            else
                for (auto &n : r["notes"]) std::fprintf(OUT, "%s%s\n", st.on ? (st.ok() + " ").c_str() : "", n.get<std::string>().c_str());
            return 0;
        }
    } catch (const std::exception &e) {
        return fail(a, e.what());
    }
    return fail(a, "unknown command '" + cmd + "' (run `wavelength help`)");
}
