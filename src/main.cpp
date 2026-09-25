// Wavelength, a headless music engine for AI agents.
//
//   wavelength plugins [--rescan] [--json]
//   wavelength presets <plugin> [--search TEXT] [--json]
//   wavelength samples [--search TEXT] [--kit NAME] [--json]
//   wavelength params <plugin> [--state FILE] [--format F] [--all] [--json]
//   wavelength render <job.json> [--out DIR] [--json] [--verbose]
//   wavelength state save <plugin> --out FILE.clap-preset [--state FILE] [--set "Name=value"]...
//   wavelength version
#include "analyze.hpp"
#include "audition.hpp"
#include "catalog.hpp"
#include "instance.hpp"
#include "platform.hpp"
#include "plugin.hpp"
#include "engine.hpp"
#include "effects.hpp"
#include "loudness.hpp"
#include "presets.hpp"
#include "preset_files.hpp"
#include "sampler.hpp"
#include "vst3_plugin.hpp"
#include "job.hpp"
#include "render.hpp"
#include "state_file.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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

const char *kUsage = R"(Wavelength, a headless music engine for AI agents (https://wavelength.run)

Usage:
  wavelength plugins [--rescan] [--json] | plugins --block <plugin> [--reason TEXT] | --unblock <plugin>
      List installed CLAP and VST3 plugins and the built-in instruments (cached; --rescan
      reloads every bundle). A blocked plugin (one that opens a licence window on every load,
      or crashes) is never loaded: render, params, presets and audition refuse it by name.
  wavelength presets <plugin> [--search TEXT] [--rescan] [--json]
      List a plugin's presets to use as "preset": CLAP preset discovery, VST3 program lists,
      preset files in its preset folders, NKS presets, DX7 cartridges, bank entries.
  wavelength samples [--search TEXT] [--kit NAME] [--json]
      List sample libraries for builtin:sampler (Bitwig multisamples and drum kit folders);
      --kit shows the General MIDI key each of a kit's files is mapped to.
  wavelength audition <plugin> [--jobs 4] [--limit N] [--rebuild] [--json] | audition --retag
      Render every preset once (C4, 1 s) in worker processes and index how it sounds: octave
      offset, loudness, brightness, band balance, envelope, width. `presets` then shows tags
      (dark, bright, sub, pluck, slow attack, wide, self-playing, octave -1...) you can search.
  wavelength analyze <file.wav | render-dir> [--start S] [--end S] [--song-time] [--grid BPM [--div 4]] [--json]
      Measure what can't be heard: pitch, brightness, spectral balance, stereo width,
      onsets and envelope of a WAV (or a window of it). A render folder analyzes its mix,
      every stem and every marker section. --start/--end are seconds into the file; with
      --song-time they are song time (a render's lead-in is added from its report.json).
      --grid lists each onset's beat and its timing offset from the nearest 1/div-beat step.
  wavelength params <plugin> [--preset NAME] [--state FILE] [--format F] [--all] [--json]
      Show a plugin's parameters, optionally after loading a state/preset.
      Hidden and read-only parameters are omitted unless --all is given.
  wavelength render <job.json> [--out DIR] [--stems float|24|16|none] [--jobs N] [--tracks "A,B"] [--json] [--verbose]
      Render a job to DIR/stems/*.wav and DIR/mix.wav (default DIR: ./out). Plugin tracks render
      in worker processes, N at once (default: half the cores, up to 4; --jobs 0 = one process);
      a worker whose plugin crashes is started again (job "retries", default 2); a track that
      still fails is left out of the mix and listed in "failedTracks", and the render then
      reports "ok": false and exits 1 (mix.wav and report.json are still written).
      --tracks renders only the named tracks (and, muted, any track that keys their
      sidechains), with the song's buses and master, to check a part without the whole song.
  wavelength master <mix.wav> --chain <chain.json | job.json> [--loudness LUFS] [--lead-in S] [--input-lead-in S] [--out DIR] [--json]
      Put a finished mix through a master chain (effects list, master object or a song's job:
      its master, markers and tempo; a file, or JSON inline) without re-rendering; reports
      loudness before and after. A mix with a lead-in (read from the render's report.json, or
      --input-lead-in) is lined up with the markers and keeps its lead-in unless --lead-in.
  wavelength state save <plugin> --out FILE [--state FILE] [--set "Name=value"]...
      Load an optional starting state, apply parameter values, save a preset
      (.clap-preset for CLAP plugins, .vstpreset for VST3).
  wavelength version

<plugin> is a plugin id, a plugin name (Apricot, "BBC Symphony Orchestra"), or a path to a
.clap/.vst3 bundle. Prefix with vst3: or clap: when a name exists in both formats.
State formats: auto (default), clap-preset, vstpreset, nksf, fxp, serum, juce-valuetree (.odin), h2p,
dx7 (<cartridge>.syx#<voice>), synplant, cherry, ngrr, microtonic, soundbox, decentsampler,
juce-string (.vital), raw.
Exit status is non-zero on any error; with --json, errors are {"ok":false,"error":...}.
)";

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;
    std::vector<std::string> sets;
    std::vector<std::string> missing;   // options given without their value
    bool has(const std::string &k) const { return opts.count(k) > 0; }
    std::string get(const std::string &k, const std::string &d = "") const { auto it = opts.find(k); return it == opts.end() ? d : it->second; }
};

Args parse(int argc, char **argv) {
    static const std::vector<std::string> flags = {"--json", "--rescan", "--verbose", "--all", "--roundrobin", "--rebuild", "--retag", "--song-time"};
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

int fail(const Args &a, const std::string &msg) {
    if (a.has("--json")) emit(json{{"ok", false}, {"error", msg}}.dump(2, ' ', false, json::error_handler_t::replace));
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
    all.push_back(builtin("builtin:drums", "Drums (built-in)", "GM kit: 36 kick, 38 snare, 37 rim, 42/46 hats, 49 crash, 51 ride, 41/45/48 toms", {"instrument", "drum"}));
    all.push_back(builtin("builtin:sampler", "Sampler (built-in)", "Bitwig .multisample instruments, WAV drum kits and single samples (see `wavelength samples`)", {"instrument", "sampler"}));
    all.push_back(builtin("builtin:audio", "Audio clips (built-in)", "WAV files placed in beats, tempo-fitted with pitch-preserving stretch, transposed, reversed, trimmed", {"instrument", "audio"}));
    all.push_back(builtin("builtin:fx", "FX (built-in)", "48 impact, 50 riser (note length), 52 reverse swell (ends with the note), 53 sub drop", {"instrument"}));
    if (a.has("--json")) {
        json list = json::array();
        for (auto &p : all)
            list.push_back({{"id", p.id}, {"name", p.name}, {"vendor", p.vendor}, {"version", p.version}, {"format", p.format},
                            {"features", p.features}, {"bundle", p.bundlePath}, {"blocked", blocked.contains(p.id)}});
        emit(json{{"ok", true}, {"plugins", list}, {"warnings", warnings}}.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    for (auto &p : all) {
        bool instrument = std::find(p.features.begin(), p.features.end(), "instrument") != p.features.end();
        std::fprintf(OUT, "%-7s %-32.32s %-30.30s %-22.22s %s\n", p.format.c_str(), p.name.c_str(), p.id.c_str(), p.vendor.c_str(),
                     (std::string(instrument ? "instrument" : "effect") + (blocked.contains(p.id) ? "  BLOCKED" : "")).c_str());
    }
    for (auto &w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    std::fprintf(OUT, "\n%zu plugins (CLAP, VST3 and built-in). Use \"vst3:Name\" or \"clap:Name\" when a name exists in both formats.\n", all.size());
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
            std::fprintf(OUT, "%-24.24s %s%s%s%s\n", p.category.c_str(), p.name.c_str(), p.description.empty() ? "" : "   (",
                         p.description.empty() ? "" : (p.description.substr(0, 90) + ")").c_str(), tags.empty() ? "" : ("   [" + tags + "]").c_str());
        }
    }
    if (a.has("--json")) emit(json{{"ok", true}, {"plugin", info.id}, {"presets", list}}.dump(2, ' ', false, json::error_handler_t::replace));
    else std::fprintf(OUT, "\n%zu of %zu presets (%s). Use them in a job as \"preset\": \"<name>\".%s\n", shown, presets.size(), info.name.c_str(),
                      audition.empty() ? " Run `wavelength audition` to tag them by sound." : "");
    return 0;
}

// ---- samples ---------------------------------------------------------------------------
int cmdSamples(const Args &a) {
    std::string err;
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
    const auto &lib = sampleLibrary();
    for (const auto &e : lib) {
        std::string hay = e.kind + " " + e.category + " " + e.name;
        std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
        if (!q.empty() && hay.find(q) == std::string::npos) continue;
        ++shown;
        if (a.has("--json")) list.push_back({{"kind", e.kind}, {"name", e.name}, {"category", e.category}, {"count", e.count}, {"path", e.path}});
        else std::fprintf(OUT, "%-12s %-22.22s %-44.44s %4zu %s\n", e.kind.c_str(), e.category.c_str(), e.name.c_str(), e.count,
                          e.kind == "kit" ? "wavs" : "zones");
    }
    if (a.has("--json")) emit(json{{"ok", true}, {"roots", sampleRoots()}, {"samples", list}}.dump(2, ' ', false, json::error_handler_t::replace));
    else std::fprintf(OUT, "\n%zu of %zu libraries. Use as \"plugin\": \"builtin:sampler\" with \"sampler\": {\"multisample\": \"<name>\"} or {\"kit\": \"<name>\"}.\n",
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

// ---- analyze ---------------------------------------------------------------------------
int cmdAnalyze(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength analyze <file.wav | render-dir> [--start S] [--end S] [--song-time]");
    const std::string target = a.positional[1];
    std::string err;
    double start = std::atof(a.get("--start", "0").c_str()), end = std::atof(a.get("--end", "0").c_str());
    // --start/--end are file times; a render's files begin with its lead-in (the report next to them says how long)
    double leadIn = 0;
    {
        std::error_code ec;
        const fs::path base = fs::is_directory(target, ec) ? fs::path(target) : fs::absolute(target).parent_path();
        for (const fs::path &dir : {base, base.parent_path()}) {
            std::ifstream rin(dir / "report.json");
            const json rep = rin ? json::parse(rin, nullptr, false) : json();
            if (rep.is_object() && rep.contains("leadIn")) { leadIn = rep.value("leadIn", 0.0); break; }
        }
    }
    std::string windowNote;
    if (a.has("--song-time")) { if (start > 0 || end > 0) { start += leadIn; if (end > 0) end += leadIn; } }
    else if (leadIn > 0 && (start > 0 || end > 0)) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "--start/--end are file times and this render begins with %.1f s of lead-in; add --song-time to measure song time", leadIn);
        windowNote = buf;
    }
    auto one = [&](const std::string &path, double s, double e, bool onsets, json &out) {
        Audio audio;
        int sr = 0;
        if (!readWav(path, audio, sr, err)) return false;
        out = analysisToJson(analyzeAudio(audio, sr, s, e), onsets);
        out["file"] = path;
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
        if (report.is_object() && report.contains("tracks"))
            for (auto &t : report["tracks"]) {
                const std::string f = t.value("file", "");
                if (f.empty() || !fs::exists(f, ec)) continue;
                json s;
                if (one(f, start, end, false, s)) { s["track"] = t.value("name", ""); result["stems"].push_back(s); }
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
    // --grid BPM [--div 4]: how far each onset sits from the nearest grid step (song time, constant tempo)
    json &main = result.contains("mix") ? result["mix"] : result;
    if (a.has("--grid") && main.contains("onsets")) {
        const double bpm = std::atof(a.get("--grid").c_str()), div = std::max(1, std::atoi(a.get("--div", "4").c_str()));
        if (bpm <= 0) return fail(a, "--grid needs the song's tempo in BPM");
        json list = json::array();
        double sumAbs = 0;
        for (auto &o : main["onsets"]) {
            const double t = o.get<double>() - leadIn, beat = t * bpm / 60.0, step = std::round(beat * div) / div;
            const double offMs = (beat - step) * 60000.0 / bpm;
            sumAbs += std::fabs(offMs);
            list.push_back({{"time", std::round(t * 1000) / 1000}, {"beat", std::round(step * 1000) / 1000}, {"offsetMs", std::round(offMs * 10) / 10}});
        }
        result["grid"] = {{"bpm", bpm}, {"div", div}, {"onsets", list},
                          {"meanAbsOffsetMs", list.empty() ? 0.0 : std::round(sumAbs / list.size() * 10) / 10}};
    }
    if (a.has("--json")) { emit(result.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
    if (result.contains("grid")) {
        const auto &g = result["grid"];
        std::fprintf(OUT, "grid %g BPM, 1/%d beat: mean offset %.1f ms (+ = late)\n", g["bpm"].get<double>(), g["div"].get<int>(), g["meanAbsOffsetMs"].get<double>());
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
    if (result.contains("mix")) {
        line("mix", result["mix"]);
        for (auto &s : result["stems"]) line(s["track"].get<std::string>(), s);
        for (auto &s : result["sections"]) line("section " + s["section"].get<std::string>(), s);
    } else line(fs::path(target).filename().string(), result);
    return 0;
}

// ---- params ----------------------------------------------------------------------------
int cmdParams(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength params <plugin>");
    PluginInfo info;
    std::string err;
    auto inst = openForInspection(a, a.positional[1], info, err);
    if (!inst) return fail(a, err);
    auto params = inst->params();
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
    if (!readWav(input, in, sr, err)) return fail(a, err);
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
    json master, markers = json::array(), tempo = 120;
    if (chain.is_array()) master = {{"fx", chain}};
    else if (chain.is_object() && (chain.contains("tracks") || chain.contains("master"))) {   // a song's job (or its master part): master, markers, tempo
        master = chain.value("master", json::object());
        markers = chain.value("markers", json::array());
        if (chain.contains("tempo")) tempo = chain["tempo"];
    } else if (chain.is_object()) master = chain;
    else return fail(a, "the chain must be an effect list, a master object ({\"fx\": [...], \"loudness\": -14}) or a job");
    if (a.has("--loudness")) master["loudness"] = std::atof(a.get("--loudness").c_str());
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
                          {"markers", markers}, {"master", master},
                          {"tracks", json::array({{{"name", "Mix"}, {"plugin", "builtin:audio"}, {"clips", json::array({clip})}}})}};
    const std::string base = inlineChain ? fs::current_path().string() : fs::absolute(chainArg).parent_path().string();
    Job job;
    if (!parseJob(jobJson, base, job, err, false)) return fail(a, err);
    const std::string outDir = a.get("--out", (fs::path(input).parent_path() / "mastered").string());
    RenderResult r;
    bool ok = false;
    try { ok = renderJob(job, outDir, a.has("--verbose"), r, err); }
    catch (const std::exception &e) { err = std::string("master failed: ") + e.what(); }
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
                               {"loudnessGainDb", r1(r.loudnessGainDb)}, {"levels", levelsJson(r.mix)}}},
                   {"masterFx", r.masterFx}, {"sections", sections}, {"warnings", r.warnings},
                   {"renderSeconds", std::round(r.renderSeconds * 100) / 100}};
    std::ofstream(fs::path(outDir) / "report.json") << report.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    if (a.has("--json")) { emit(report.dump(2, ' ', false, json::error_handler_t::replace)); return 0; }
    std::fprintf(OUT, "input   %6.1f LUFS  %5.1f dBTP  %s\n", report["input"]["lufs"].get<double>(), report["input"]["truePeakDb"].get<double>(), input.c_str());
    std::fprintf(OUT, "output  %6.1f LUFS  %5.1f dBTP  %s\n", r.mixLufs, r.truePeakDb, r.mixFile.c_str());
    for (auto &sec : sections)
        std::fprintf(OUT, "  %-20s %6.1f -> %6.1f LUFS\n", sec["name"].get<std::string>().c_str(), sec["inputLufs"].get<double>(), sec["lufs"].get<double>());
    for (auto &w : r.warnings) std::fprintf(OUT, "warning: %s\n", w.c_str());
    return 0;
}

// ---- render ----------------------------------------------------------------------------
int cmdRender(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength render <job.json>");
    const std::string path = a.positional[1];
    std::ifstream in(path);
    if (!in) return fail(a, "cannot read " + path);
    json j;
    try { in >> j; } catch (const std::exception &e) { return fail(a, std::string("job is not valid JSON: ") + e.what()); }
    // --tracks "Lead,Bass": render only those (plus, muted, the tracks that key their sidechains).
    // Workers re-read the job by track index, so the subset goes to a file next to the job.
    std::string subsetPath;
    std::vector<std::string> only;
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
        for (auto &t : j["tracks"]) {
            const std::string n = t.value("name", "");
            if (!need.count(n)) continue;
            json c = t;
            if (!want.count(n)) c["mute"] = true;   // renders only to key an effect
            kept.push_back(c);
        }
        j["tracks"] = kept;
        subsetPath = (fs::absolute(path).parent_path() / (".wavelength-tracks-" + std::to_string(platform::processId()) + ".json")).string();
        std::ofstream(subsetPath) << j.dump();
    }
    struct RemoveSubset { std::string p; ~RemoveSubset() { std::error_code ec; if (!p.empty()) fs::remove(p, ec); } } removeSubset{subsetPath};
    Job job;
    std::string err;
    std::string base = fs::absolute(path).parent_path().string();
    if (!parseJob(j, base, job, err)) return fail(a, err);
    job.sourcePath = subsetPath.empty() ? fs::absolute(path).string() : subsetPath;
    if (a.has("--jobs")) job.parallel = std::atoi(a.get("--jobs").c_str());
    if (a.has("--stems")) {
        const std::string s = a.get("--stems");
        job.stemBits = s == "none" ? 0 : s == "16" ? 16 : s == "24" ? 24 : s == "float" || s == "32" ? 32 : -1;
        if (job.stemBits < 0) return fail(a, "--stems must be float, 24, 16 or none");
    }
    RenderResult r;
    std::string outDir = a.get("--out", "out");
    bool ok = false;
    try { ok = renderJob(job, outDir, a.has("--verbose"), r, err); }
    catch (const std::exception &e) { err = std::string("render failed: ") + e.what(); }
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
                          {"stateFormat", t.stateFormat}, {"fx", t.fx}, {"lufs", r1(t.lufs)}, {"postFaderPeakDb", r1(t.postPeakDb)},
                          {"renderSeconds", std::round(t.seconds * 100) / 100},
                          {"latencyCompensatedMs", std::round(t.latencySamples * 1000.0 / r.sampleRate * 100) / 100},
                          {"sections", labelled(t.sectionLufs)}, {"sectionLufs", bare(t.sectionLufs)},
                          {"levels", levelsJson(t.levels)}, {"warnings", t.warnings}});
    json buses = json::array();
    for (auto &b : r.buses)
        buses.push_back({{"name", b.name}, {"fx", b.fx}, {"lufs", r1(b.lufs)}, {"sections", labelled(b.sectionLufs)},
                         {"sectionLufs", bare(b.sectionLufs)}, {"levels", levelsJson(b.levels)}});
    json sections = json::array();
    for (auto &sec : r.sections)
        sections.push_back({{"name", sec.name}, {"start", std::round(sec.start * 100) / 100}, {"end", std::round(sec.end * 100) / 100}, {"lufs", r1(sec.lufs)}});
    const bool complete = r.failedTracks.empty();
    std::string incomplete;
    if (!complete) {
        for (auto &n : r.failedTracks) incomplete += (incomplete.empty() ? "'" : ", '") + n + "'";
        incomplete = std::to_string(r.failedTracks.size()) + (r.failedTracks.size() == 1 ? " track" : " tracks") + " failed (" + incomplete +
                     "): mix.wav and this report are missing them, so levels, ducking and loudness are wrong; render again";
    }
    json report = {{"ok", complete}, {"sampleRate", r.sampleRate}, {"seconds", std::round(r.seconds * 100) / 100},
                   {"renderSeconds", std::round(r.renderSeconds * 100) / 100}, {"leadIn", r.leadIn}, {"defaultsApplied", job.appliedDefaults},
                   {"mix", {{"file", r.mixFile}, {"lufs", r1(r.mixLufs)}, {"lra", r1(r.mixLra)}, {"truePeakDb", r1(r.truePeakDb)}, {"levels", levelsJson(r.mix)},
                            {"masterFx", r.masterFx}, {"normalizeGainDb", r1(r.normalizeGainDb)}, {"loudnessGainDb", r1(r.loudnessGainDb)}}},
                   {"sections", sections}, {"tracks", tracks}, {"buses", buses}, {"warnings", r.warnings},
                   {"failedTracks", r.failedTracks}};
    if (!only.empty()) report["onlyTracks"] = only;
    if (!complete) report["error"] = incomplete;
    std::ofstream(fs::path(outDir) / "report.json") << report.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    if (a.has("--json")) { emit(report.dump(2, ' ', false, json::error_handler_t::replace)); return complete ? 0 : 1; }
    for (auto &t : r.tracks) {
        std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  %s\n", t.name.c_str(), t.pluginName.c_str(), t.levels.peakDb,
                    t.lufs, t.file.c_str());
        for (auto &w : t.warnings) std::fprintf(OUT, "    ! %s\n", w.c_str());
    }
    for (auto &b : r.buses) std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS\n", ("bus: " + b.name).c_str(), "", b.levels.peakDb, b.lufs);
    std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  LRA %.1f LU  true peak %.1f dBTP  %s\n", "MIX", "", r.mix.peakDb, r.mixLufs,
                 r.mixLra, r.truePeakDb, r.mixFile.c_str());
    for (auto &sec : r.sections) std::fprintf(OUT, "    section %-18s %6.1f LUFS  (%.1f–%.1f s)\n", sec.name.c_str(), sec.lufs, sec.start, sec.end);
    for (auto &w : r.warnings) std::fprintf(OUT, "    ! %s\n", w.c_str());
    std::fprintf(OUT, "%.2f s of audio rendered in %.2f s\n", r.seconds, r.renderSeconds);
    if (!complete) { std::fprintf(stderr, "error: %s\n", incomplete.c_str()); return 1; }
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
    if (a.positional.empty() || a.positional[0] == "help" || a.has("--help")) { std::fputs(kUsage, OUT); return a.positional.empty() ? 1 : 0; }
    const std::string cmd = a.positional[0];
    if (!a.missing.empty()) return fail(a, a.missing.front() + " needs a value");
    if (cmd == "__track" && a.positional.size() > 3)
        return renderTrackWorker(a.positional[1], std::stoul(a.positional[2]), a.positional[3],
                                 std::vector<std::string>(a.positional.begin() + 4, a.positional.end()));
    if (cmd == "__audition" && a.positional.size() > 3) return auditionWorker(a.positional[1], a.positional[2], a.positional[3]);
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
    try {
        if (cmd == "plugins") return cmdPlugins(a);
        if (cmd == "params") return cmdParams(a);
        if (cmd == "presets") return cmdPresets(a);
        if (cmd == "samples") return cmdSamples(a);
        if (cmd == "analyze") return cmdAnalyze(a);
        if (cmd == "audition") return cmdAudition(a);
        if (cmd == "render") return cmdRender(a);
        if (cmd == "master") return cmdMaster(a);
        if (cmd == "state") return cmdState(a);
        if (cmd == "version") { std::fprintf(OUT, "wavelength %s\n", WAVELENGTH_VERSION); return 0; }
    } catch (const std::exception &e) {
        return fail(a, e.what());
    }
    return fail(a, "unknown command '" + cmd + "' (run `wavelength help`)");
}
