// Wavelength, a headless music engine for AI agents.
//
//   wavelength plugins [--rescan] [--json]
//   wavelength presets <plugin> [--search TEXT] [--json]
//   wavelength samples [--search TEXT] [--kit NAME] [--json]
//   wavelength params <plugin> [--state FILE] [--format F] [--all] [--json]
//   wavelength render <job.json> [--out DIR] [--json] [--verbose]
//   wavelength state save <plugin> --out FILE.clap-preset [--state FILE] [--set "Name=value"]...
//   wavelength import <project.dawproject> [--out DIR] [--bitwig FILE.bwproject | none] [--json]
//   wavelength lint <job.json> [--tracks "A,B,C"] [--low "B"] [--crossings] [--json]
//   wavelength lint <job.json> --harmony [--key K] [--ignore "A,B"] [--chords] [--max-bars N] [--json]
//   wavelength version
#include "analyze.hpp"
#include "harmony.hpp"
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
#include "bitwig.hpp"
#include "dawproject.hpp"
#include "midi_file.hpp"
#include "vst2_plugin.hpp"
#include "vst3_plugin.hpp"
#include "job.hpp"
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
  wavelength analyze <file.wav | render-dir> [--start S] [--end S] [--song-time] [--grid BPM [--div 4]] [--every S] [--peaks [--top N]] [--json]
      Measure what can't be heard: pitch, brightness, spectral balance, stereo width,
      onsets and envelope of a WAV (or a window of it). A render folder analyzes its mix,
      every stem and every marker section. --start/--end are seconds into the file; with
      --song-time they are song time (a render's lead-in is added from its report.json).
      --grid lists each onset's beat and its timing offset from the nearest 1/div-beat step.
      --every S prints the loudness of every S-second window (file time, or song time from the
      first beat with --song-time), labelled with the render's sections: the song's contour at a
      glance, dropouts and drops included. --peaks lists the strongest spectral peaks of the
      window (Hz, note and cents, level; 12 or --top N) and their spacing: where a comb,
      resonator or flanger sits. A render folder also analyzes bus stems.
  wavelength params <plugin> [--preset NAME] [--state FILE] [--format F] [--all] [--set "Name=v"]... [--map "Name" [--steps N]] [--json]
      Show a plugin's parameters, optionally after loading a state/preset.
      Hidden and read-only parameters are omitted unless --all is given. --set "Name=0.55"
      prints the plugin's display text for a plain value (or the value it reads for display
      text, "Name=800 Hz", or a note name); --map "Name" tabulates value -> display across the
      range (21 rows, or --steps N). Neither changes anything.
  wavelength render <job.json> [--out DIR] [--stems float|24|16|none] [--jobs N] [--tracks "A,B"] [--level-from report.json] [--json] [--verbose]
      Render a job to DIR/stems/*.wav and DIR/mix.wav (default DIR: ./out). Plugin tracks render
      in worker processes, N at once (default: half the cores, up to 4; --jobs 0 = one process);
      a worker whose plugin crashes is started again (job "retries", default 2); a track that
      still fails is left out of the mix and listed in "failedTracks", and the render then
      reports "ok": false and exits 1 (mix.wav and report.json are still written).
      --tracks renders only the named tracks (and, muted, any track that keys their
      sidechains), with the song's buses and master, to check a part without the whole song.
      --level-from out/report.json keeps the master at that render's gains (its loudness-target
      and normalize gain) instead of targeting again: a --tracks render then plays each part at
      the level it has in the full mix.
  wavelength master <mix.wav> --chain <chain.json | job.json> [--loudness LUFS] [--lead-in S] [--input-lead-in S] [--out DIR] [--json]
      Put a finished mix through a master chain (effects list, master object or a song's job:
      its master, markers and tempo; a file, or JSON inline) without re-rendering; reports
      loudness before and after. A mix with a lead-in (read from the render's report.json, or
      --input-lead-in) is lined up with the markers and keeps its lead-in unless --lead-in.
  wavelength state save <plugin> --out FILE [--state FILE] [--set "Name=value"]...
      Load an optional starting state, apply parameter values, save a preset
      (.clap-preset for CLAP plugins, .vstpreset for VST3).
  wavelength import <project.dawproject> [--out DIR] [--bitwig FILE.bwproject | none] [--json]
      Turn a DAWproject export (Bitwig, Studio One, Cubase...) into a job: arrangement notes,
      tracks with their plugins and saved states, volume, pan, mute, sends, groups, tempo,
      markers, volume/pan automation. Lists what the file can't carry (a DAW's own devices).
      `render project.dawproject` imports into <out>/import and renders in one go.
  wavelength import <song.mid> [--out DIR] [--instrument PLUGIN] [--json]
      Turn a Standard MIDI File into a job: tempo map, time signature, markers, one track per
      MIDI track and channel (notes, sustain pedal, volume/pan/expression, other CCs, pitch
      bend). Channel 10 plays builtin:drums; other channels a General MIDI-family sound from
      the sample library, or PLUGIN for all of them. `render song.mid` imports and renders.
  wavelength export <job.json> [--out song.mid] [--json]
      Write the job's parts as a MIDI file (type 1): tempo map, time signature, markers, and a
      track per job track with its notes, CC, pitch bend and pressure automation.
  wavelength lint <job.json> [--tracks "Soprano,Alto,Bass"] [--low "Bass"] [--split "Organ=4"]
                  [--from BAR] [--to BAR] [--section NAME] [--crossings] [--json]
      Voice-leading check between melodic tracks (one voice each: its top note, its lowest for
      --low tracks, or N voices top to bottom for --split chord tracks): parallel fifths and
      octaves in similar motion, and with --crossings a voice below the next one in order (high
      to low). Each problem shows both chords' notes and where they are; --from/--to/--section
      limit the report to a bar range or a marker's section.
  wavelength lint <job.json> --harmony [--key "D minor"] [--ignore "SFX,Ping"] [--chords] [--max-bars 2]
                  [--from BAR] [--to BAR] [--section NAME] [--json]
      Harmony check on the notes: the key of every stretch of bars (the job's "keys", --key, or
      detected), a chord chart with --chords, one- or two-bar chords outside the key that go
      straight back (heard as a key change), clashes (a minor 2nd/9th held a beat, one note outside
      the key) and in-key rubs grouped per pair of tracks.
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
    static const std::vector<std::string> flags = {"--json", "--rescan", "--verbose", "--all", "--roundrobin", "--rebuild", "--retag", "--song-time", "--crossings", "--harmony", "--chords", "--peaks"};
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
        std::fprintf(OUT, "%-7s %-32.32s %-30.30s %-22.22s %s\n", p.format.c_str(), p.name.c_str(), p.id.c_str(), p.vendor.c_str(),
                     (std::string(instrument ? "instrument" : "effect") + (p.arch.empty() ? "" : "  (" + p.arch + ", Rosetta)") +
                      (blocked.contains(p.id) ? "  BLOCKED" : "")).c_str());
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
        if (!readWav(path, audio, sr, err)) return false;
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
        if (report.is_object() && report.contains("tracks"))
            for (auto &t : report["tracks"]) {
                const std::string f = t.value("file", "");
                if (f.empty() || !fs::exists(f, ec)) continue;
                json s;
                if (one(f, start, end, false, s)) { s["track"] = t.value("name", ""); result["stems"].push_back(s); }
            }
        if (report.is_object() && report.contains("buses"))   // bus stems ("stem": true on the bus)
            for (auto &b : report["buses"]) {
                const std::string f = b.value("file", "");
                if (f.empty() || !fs::exists(f, ec)) continue;
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
        if (!readWav(file, audio, sr, err)) return fail(a, err);
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
// ---- import -------------------------------------------------------------------------
int cmdImport(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength import <project.dawproject | song.mid | project.bwproject> [--out DIR]");
    const std::string src = a.positional[1];
    if (fs::path(src).extension() == ".bwproject") {   // Bitwig's own format: list what it holds
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
        std::fprintf(OUT, "(notes and clips aren't read from .bwproject files: export a DAWproject and import that)\n");
        return 0;
    }
    const std::string outDir = a.get("--out", fs::path(src).stem().string());
    std::string ext = fs::path(src).extension().string();
    for (auto &ch : ext) ch = (char)std::tolower((unsigned char)ch);
    if (ext == ".mid" || ext == ".midi" || ext == ".smf" || ext == ".kar" || ext == ".rmi") {
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
    DawprojectImport r;
    std::string err;
    if (!importDawproject(src, outDir, r, err, a.get("--bitwig", ""))) return fail(a, err);
    const std::string jobPath = (fs::path(outDir) / "job.json").string();
    if (a.has("--json")) {
        emit(json{{"ok", true}, {"job", jobPath}, {"application", r.application}, {"tracks", r.tracks}, {"buses", r.buses},
                  {"notes", r.noteCount}, {"plugins", r.plugins}, {"bitwig", r.bitwig}, {"left out", r.notes}}.dump(2, ' ', false, json::error_handler_t::replace));
        return 0;
    }
    std::fprintf(OUT, "imported %s%s: %zu tracks, %zu buses, %zu notes, %zu plugins -> %s\n", src.c_str(),
                 r.application.empty() ? "" : (" (" + r.application + ")").c_str(), r.tracks, r.buses, r.noteCount, r.plugins, jobPath.c_str());
    if (!r.bitwig.empty()) std::fprintf(OUT, "  Bitwig's own devices from %s\n", r.bitwig.c_str());
    for (auto &n : r.notes) std::fprintf(OUT, "  ! %s\n", n.c_str());
    return 0;
}

int cmdExport(const Args &a) {
    if (a.positional.size() < 2) return fail(a, "usage: wavelength export <job.json> [--out song.mid]");
    const std::string src = a.positional[1];
    std::ifstream in(src);
    if (!in) return fail(a, "cannot read " + src);
    json j;
    try { in >> j; } catch (const std::exception &e) { return fail(a, std::string("job is not valid JSON: ") + e.what()); }
    Job job;
    std::string err;
    if (!parseJob(j, fs::path(src).parent_path().string(), job, err)) return fail(a, err);
    std::string out = a.get("--out", (fs::path(src).parent_path() / (fs::path(src).stem().string() + ".mid")).string());
    if (fs::path(out).extension() != ".mid" && fs::path(out).extension() != ".midi") return fail(a, "export writes MIDI files: give --out a .mid name");
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
    if (const std::string ext = fs::path(path).extension().string(); ext == ".mid" || ext == ".midi") {   // import, then render
        const std::string importDir = (fs::path(a.get("--out", "out")) / "import").string();
        MidiImport m;
        std::string err;
        if (!importMidiFile(path, importDir, a.get("--instrument", ""), m, err)) return fail(a, err);
        for (auto &n : m.notes) std::fprintf(stderr, "import: %s\n", n.c_str());
        path = (fs::path(importDir) / "job.json").string();
    }
    if (fs::path(path).extension() == ".dawproject") {   // import next to the output, then render that job
        const std::string importDir = (fs::path(a.get("--out", "out")) / "import").string();
        DawprojectImport r;
        std::string err;
        if (!importDawproject(path, importDir, r, err, a.get("--bitwig", ""))) return fail(a, err);
        for (auto &n : r.notes) std::fprintf(stderr, "import: %s\n", n.c_str());
        path = (fs::path(importDir) / "job.json").string();
    }
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
            if (!want.count(n)) { c["mute"] = true; c["stem"] = false; }   // renders only to key an effect: no mix, no stem file
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
    if (a.has("--level-from")) {   // play at the level of an earlier render (a full mix): its master gains, not a new target
        std::ifstream rin(a.get("--level-from"));
        json rep;
        try { rin >> rep; } catch (...) { return fail(a, "--level-from: cannot read report " + a.get("--level-from")); }
        if (!rep.contains("mix") || !rep["mix"].is_object()) return fail(a, "--level-from: " + a.get("--level-from") + " is not a render report");
        job.levelFixed = true;
        job.fixedLoudnessGainDb = rep["mix"].value("loudnessGainDb", 0.0);
        job.fixedNormalizeGainDb = rep["mix"].value("normalizeGainDb", 0.0);
    }
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
        buses.push_back({{"name", b.name}, {"fx", b.fx}, {"file", b.file}, {"lufs", r1(b.lufs)}, {"sections", labelled(b.sectionLufs)},
                         {"sectionLufs", bare(b.sectionLufs)}, {"levels", levelsJson(b.levels)}});
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
                            {"masterFx", r.masterFx}, {"normalizeGainDb", r1(r.normalizeGainDb)}, {"loudnessGainDb", r1(r.loudnessGainDb)}}},
                   {"sections", sections}, {"tracks", tracks}, {"buses", buses}, {"warnings", r.warnings},
                   {"dropouts", dropouts}, {"failedTracks", r.failedTracks}};
    if (!only.empty()) report["onlyTracks"] = only;
    if (!complete) report["error"] = incomplete;
    std::ofstream(fs::path(outDir) / "report.json") << report.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    if (a.has("--json")) { emit(report.dump(2, ' ', false, json::error_handler_t::replace)); return complete ? 0 : 1; }
    for (auto &t : r.tracks) {
        std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  %s\n", t.name.c_str(), t.pluginName.c_str(), t.levels.peakDb,
                    t.lufs, t.file.c_str());
        for (auto &w : t.warnings) std::fprintf(OUT, "    ! %s\n", w.c_str());
    }
    for (auto &b : r.buses)
        std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  %s\n", ("bus: " + b.name).c_str(), "", b.levels.peakDb, b.lufs, b.file.c_str());
    std::fprintf(OUT, "%-24s %-20s peak %6.1f dB  %6.1f LUFS  LRA %.1f LU  true peak %.1f dBTP  %s\n", "MIX", "", r.mix.peakDb, r.mixLufs,
                 r.mixLra, r.truePeakDb, r.mixFile.c_str());
    for (auto &sec : r.sections) std::fprintf(OUT, "    section %-18s %6.1f LUFS  (%.1f–%.1f s)\n", sec.name.c_str(), sec.lufs, sec.start, sec.end);
    for (auto &w : r.warnings) std::fprintf(OUT, "    ! %s\n", w.c_str());
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
    if (a.positional.empty() || a.positional[0] == "help" || a.has("--help")) { std::fputs(kUsage, OUT); return a.positional.empty() ? 1 : 0; }
    const std::string cmd = a.positional[0];
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
    if ((cmd == "params" || cmd == "presets" || cmd == "audition" || cmd == "state") && !std::getenv("WAVELENGTH_ARCH_REEXEC")) {
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
            execv(cargs[0], cargs.data());
            return fail(a, "could not run " + info.name + " under Rosetta");
        }
    }
#endif
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
            {"samples", {"--search", "--kit", "--roundrobin", "--json"}},
            {"analyze", {"--start", "--end", "--song-time", "--grid", "--div", "--every", "--peaks", "--top", "--json"}},
            {"audition", {"--jobs", "--limit", "--rebuild", "--retag", "--json", "--verbose"}},
            {"render", {"--out", "--stems", "--jobs", "--tracks", "--level-from", "--json", "--verbose", "--bitwig", "--instrument"}},
            {"master", {"--chain", "--loudness", "--lead-in", "--input-lead-in", "--out", "--json", "--verbose"}},
            {"state", {"--out", "--preset", "--state", "--format", "--json", "--verbose"}},
            {"import", {"--out", "--json", "--bitwig", "--instrument"}},
            {"export", {"--out", "--json"}},
            {"lint", {"--tracks", "--low", "--split", "--from", "--to", "--section", "--crossings", "--json", "--harmony", "--key", "--ignore", "--chords", "--max-bars"}},
            {"version", {"--json"}}};
        auto it = known.find(cmd);
        if (it != known.end())
            for (auto &[k, v] : a.opts)
                if (!it->second.count(k)) {
                    std::string list;
                    for (auto &o : it->second) list += (list.empty() ? "" : " ") + o;
                    return fail(a, "unknown option " + k + " for `" + cmd + "` (it takes: " + list + (cmd == "state" ? " --set" : "") + ")");
                }
    }
    // after the unknown-option check, so a stray `--typo` at the end is named as unknown, not as missing a value
    if (!a.missing.empty()) return fail(a, a.missing.front() + " needs a value");
    try {
        if (cmd == "plugins") return cmdPlugins(a);
        if (cmd == "params") return cmdParams(a);
        if (cmd == "presets") return cmdPresets(a);
        if (cmd == "samples") return cmdSamples(a);
        if (cmd == "analyze") return cmdAnalyze(a);
        if (cmd == "audition") return cmdAudition(a);
        if (cmd == "render") return cmdRender(a);
        if (cmd == "import") return cmdImport(a);
        if (cmd == "export") return cmdExport(a);
        if (cmd == "master") return cmdMaster(a);
        if (cmd == "state") return cmdState(a);
        if (cmd == "lint") return cmdLint(a);
        if (cmd == "version") { std::fprintf(OUT, "wavelength %s\n", WAVELENGTH_VERSION); return 0; }
    } catch (const std::exception &e) {
        return fail(a, e.what());
    }
    return fail(a, "unknown command '" + cmd + "' (run `wavelength help`)");
}
