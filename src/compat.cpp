#include "compat.hpp"

#include "analyze.hpp"
#include "audition.hpp"
#include "catalog.hpp"
#include "engine.hpp"
#include "platform.hpp"
#include "presets.hpp"
#include "state_file.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace wl {

namespace {

constexpr double kLength = 3.0, kOn = 0.5, kLen = 1.0;
constexpr int kTestVersion = 5;   // bump when the tests change: cached results of older tests are run again

double since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }

bool isInstrument(const PluginInfo &p) { return std::find(p.features.begin(), p.features.end(), "instrument") != p.features.end(); }

// one render of the test: a C2-C5 chord for instruments (or the same notes one after another: some
// instruments give keys other jobs, Microtonic's Audio Unit mutes on C3), a noise burst through effects
struct Take { bool ok = false, silent = true, garbage = false; int oneSided = 0; double lufs = -120, peakDb = -120, centroid = 0, ms = 0, midDb = -120, sideDb = -120; double bands[6] = {}; std::string error; Audio audio; };

Take renderTest(OpenedPlugin &p, bool instrument, double warmup, const Audio &noise, bool arpeggio = false) {
    Take t;
    Job job;
    job.sampleRate = 48000;
    job.blockSize = 512;
    job.warmup = warmup;
    std::vector<Note> notes;
    int n = 0;
    for (int key : {36, 48, 60, 72}) {
        if (arpeggio) notes.push_back({kOn + 0.4 * n++, 0.35, key, 0, 0.8, {}, {}});
        else notes.push_back({kOn, kLen, key, 0, 0.8, {}, {}});
    }
    const auto events = scheduleNotes(notes, job.sampleRate);
    t.audio.resize((size_t)(kLength * job.sampleRate));
    std::vector<std::string> warnings;
    const auto t0 = std::chrono::steady_clock::now();
    t.ok = p.plugin->render(job, events, p.initial, {}, instrument ? nullptr : &noise, t.audio, warnings, t.error);
    t.ms = since(t0);
    if (!t.ok) return t;
    std::vector<std::string> g;
    muteGarbage(t.audio, job.sampleRate, p.name, g);
    t.garbage = !g.empty();
    t.oneSided = instrument ? oneSidedChannel(t.audio) : 0;
    const Analysis a = analyzeAudio(t.audio, job.sampleRate);
    t.silent = a.silent;
    t.lufs = a.lufs;
    t.peakDb = a.peakDb;
    t.centroid = a.centroidHz;
    for (int i = 0; i < 6; ++i) t.bands[i] = a.bandsDb[i];
    double mid = 0, side = 0;   // the stereo image: a panner's preset may change nothing else
    for (size_t i = 0; i < t.audio.frames(); ++i) {
        const double m = 0.5 * (t.audio.left[i] + t.audio.right[i]), d = 0.5 * (t.audio.left[i] - t.audio.right[i]);
        mid += m * m;
        side += d * d;
    }
    const double frames = (double)std::max<size_t>(1, t.audio.frames());
    t.midDb = 10 * std::log10(mid / frames + 1e-12);
    t.sideDb = 10 * std::log10(side / frames + 1e-12);
    return t;
}

// did a preset change the sound? level, brightness or band balance, beyond render-to-render noise
bool soundsDifferent(const Take &a, const Take &b) {
    if (a.silent != b.silent) return true;
    if (a.silent) return false;
    if (std::fabs(a.lufs - b.lufs) > 0.5) return true;
    if (a.centroid > 0 && b.centroid > 0 && std::fabs(std::log(a.centroid / b.centroid)) > 0.05) return true;
    for (int i = 0; i < 6; ++i) if (a.bands[i] > -60 && std::fabs(a.bands[i] - b.bands[i]) > 1.5) return true;
    if (std::max(a.sideDb, b.sideDb) > -80 && std::fabs((a.sideDb - a.midDb) - (b.sideDb - b.midDb)) > 3) return true;   // wider, narrower
    return false;
}

bool sameAudio(const Audio &a, const Audio &b) {
    if (a.frames() != b.frames()) return false;
    double diff = 0, sig = 0;
    for (size_t i = 0; i < a.frames(); ++i) {
        const double dl = a.left[i] - b.left[i], dr = a.right[i] - b.right[i];
        diff += dl * dl + dr * dr;
        sig += (double)b.left[i] * b.left[i] + (double)b.right[i] * b.right[i];
    }
    return sig > 0 && diff < sig * 1e-8;   // within -80 dB
}

std::string key(const PluginInfo &p) { return p.format + ":" + p.id; }

long long bundleTime(const PluginInfo &p) { return p.bundlePath.empty() ? 0 : bundleStamp(p.bundlePath); }

// The spec a worker resolves: the format and id, through the catalog (which knows an Intel-only
// plugin's architecture, so the worker runs itself under Rosetta for it)
std::string specOf(const PluginInfo &p) { return p.format + ":" + p.id; }

// Turn a worker's step lines into a record with a status and its issues
// A Maize Sampler player (HERA-ONE, Subs) keeps its sounds in an .instruments folder its settings name:
// when that folder is gone the plugin plays nothing. Returns the missing folder, or "".
std::string missingMaizeContent(const PluginInfo &info) {
    const char *appdata = std::getenv("APPDATA");
    for (const fs::path &settings : {platform::homeDir() / "Library/Application Support/Maize Sampler Player/pluginsettings.xml",
                                     fs::path(appdata ? appdata : "") / "Maize Sampler Player/pluginsettings.xml"}) {
        std::ifstream in(settings);
        if (!in) continue;
        const std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = "name=\"" + info.vendor + ":" + info.name + ".instruments\" val=\"";
        std::string lower = xml, lkey = key;
        for (auto *t : {&lower, &lkey}) std::transform(t->begin(), t->end(), t->begin(), ::tolower);
        const size_t at = lower.find(lkey);
        if (at == std::string::npos) continue;
        std::string path = xml.substr(at + key.size(), xml.find('"', at + key.size()) - (at + key.size()));
        for (size_t i; (i = path.find("&amp;")) != std::string::npos;) path.replace(i, 5, "&");
        std::error_code ec;
        if (!path.empty() && !fs::exists(path, ec)) return path;
    }
    return "";
}

json summarize(const PluginInfo &info, const std::vector<json> &steps, const std::string &ended) {
    json rec = {{"name", info.name}, {"format", info.format}, {"id", info.id}, {"vendor", info.vendor}, {"version", info.version},
                {"arch", info.arch}, {"instrument", isInstrument(info)}};
    std::vector<std::string> fails, warns, notes;
    std::string last = "start";
    bool presetSounds = false;   // a preset rendered sound (a sample player silent until one is loaded)
    for (const auto &s : steps) if (s.value("step", "") == "preset" && s.value("lufs", -120.0) > -70) presetSounds = true;
    for (const auto &s : steps) {
        const std::string step = s.value("step", "");
        last = step;
        if (step == "open") {
            rec["openMs"] = s.value("ms", 0.0);
            rec["params"] = s.value("params", 0);
            if (!s.value("ok", false)) fails.push_back("does not open: " + s.value("error", std::string("?")));
            else if (s.value("ms", 0.0) > 15000) warns.push_back("takes " + std::to_string((int)(s.value("ms", 0.0) / 1000)) + " s to open");
        } else if (step == "presets") {
            rec["presets"] = s.value("count", 0);
            rec["presetSources"] = {{"programs", s.value("programs", 0)}, {"files", s.value("files", 0)}, {"other", s.value("other", 0)}};
            if (s.contains("error") && !s["error"].get<std::string>().empty() && s.value("count", 0) == 0) notes.push_back("no presets found");
        } else if (step == "render") {
            rec["render"] = s;
            rec["render"].erase("step");
            if (!s.value("ok", false)) fails.push_back("render fails: " + s.value("error", std::string("?")));
            else if (s.value("garbage", false)) fails.push_back("outputs garbage (NaN or far over 0 dBFS)");
            else if (s.value("silent", false)) {
                const bool inst = rec["instrument"].get<bool>(), empty = rec.value("presets", 0) == 0;
                if (!inst) (presetSounds ? notes : warns).push_back(presetSounds ? "silent at its defaults; its presets pass audio" : "outputs silence from a noise input");
                else if (empty) {
                    const std::string gone = missingMaizeContent(info);
                    warns.push_back(gone.empty() ? "renders silence at its defaults and lists no presets (a sample player waiting for an instrument?)"
                                                 : "renders silence: its sounds are missing (Maize Sampler's settings point at " + gone + ", which is gone)");
                }
                else if (presetSounds) notes.push_back("silent at its defaults; its presets play");
                else fails.push_back("renders silence (a C2-C5 chord, a 5 s warm-up, the notes one at a time), and so do the presets tried");
            }
            else if (s.value("passthrough", false)) notes.push_back("passes audio through unchanged at its defaults");
            if (!s.value("oneSided", std::string()).empty())
                notes.push_back("sounds only in the " + s.value("oneSided", std::string()) +
                                " channel of its stereo output (a mono voice in one side); renders copy it across");
            if (s.value("arpeggio", false)) notes.push_back("silent with the chord, plays its notes one at a time (a key in C2-C5 has another job)");
            else if (s.value("warmedUp", false)) notes.push_back("silent until given a 5 s warm-up (samples load after activation)");
            if (s.value("realtime", 0.0) > 1.0) warns.push_back("renders slower than real time (" + std::to_string(s.value("realtime", 0.0)).substr(0, 4) + "x)");
        } else if (step == "preset") {
            rec["presetTests"].push_back(s);
            rec["presetTests"].back().erase("step");
            const std::string n = s.value("name", std::string("?"));
            if (!s.value("ok", false)) warns.push_back("preset '" + n + "' does not load: " + s.value("error", std::string("?")));
            else if (!s.value("paramsChanged", 0) && !s.value("soundChanged", false)) {
                std::string l = n;
                std::transform(l.begin(), l.end(), l.begin(), ::tolower);
                const bool initial = l.find("init") != std::string::npos || l.find("default") != std::string::npos || l == "basic" || l == "empty";
                const bool program = s.value("source", "") == "program";   // often the program the plugin starts with
                const bool tool = std::find(info.features.begin(), info.features.end(), "tools") != info.features.end();   // analyzers: views, not sound
                (initial || program || tool ? notes : warns).push_back("preset '" + n + "' changes neither parameters nor sound" +
                    (initial ? " (an init preset)" : program ? " (a program: perhaps the one it starts with)" : tool ? " (a tool: its presets may set only its display)" : ""));
            }
        } else if (step == "state") {
            rec["state"] = s;
            rec["state"].erase("step");
            if (!s.value("saved", false)) warns.push_back("cannot save its state: " + s.value("error", std::string("?")));
            else if (!s.value("reloaded", false)) warns.push_back("does not reload its own saved state: " + s.value("error", std::string("?")));
        }
    }
    if (!ended.empty()) fails.push_back(ended + (last == "done" ? "" : " (during " + (last == "start" ? std::string("loading") : "the step after '" + last + "'") + ")"));
    rec["status"] = !fails.empty() ? "fail" : !warns.empty() ? "warn" : "ok";
    rec["fails"] = fails;
    rec["warnings"] = warns;
    rec["notes"] = notes;
    return rec;
}

std::string markdownReport(const json &result) {
    std::string md = "# Wavelength compatibility sweep\n\n" + result.value("summary", std::string("")) + "\n\n";
    auto section = [&](const std::string &status, const std::string &title) {
        std::vector<const json *> rows;
        for (auto &r : result["plugins"]) if (r.value("status", "") == status) rows.push_back(&r);
        if (rows.empty()) return;
        md += "## " + title + " (" + std::to_string(rows.size()) + ")\n\n| Plugin | Format | Presets | Issues |\n|---|---|---|---|\n";
        for (auto *r : rows) {
            std::string issues;
            for (const char *k : {"fails", "warnings"})
                for (auto &x : (*r)[k]) issues += (issues.empty() ? "" : "; ") + x.get<std::string>();
            if (issues.empty()) for (auto &x : (*r)["notes"]) issues += (issues.empty() ? "" : "; ") + x.get<std::string>();
            for (auto &c : issues) if (c == '|' || c == '\n') c = ' ';
            md += "| " + r->value("name", std::string("?")) + " | " + r->value("format", std::string("?")) + " | " + std::to_string(r->value("presets", 0)) + " | " + issues + " |\n";
        }
        md += "\n";
    };
    section("fail", "Failing");
    section("warn", "Working, with warnings");
    section("blocked", "Blocked");
    section("ok", "Working");
    return md;
}

} // namespace

std::string compatCachePath() { return (platform::cacheDir() / "compat.json").string(); }

int compatWorker(const std::string &spec, const std::string &resultsFile, int presetCount, int only, const std::string &baseFile) {
    std::ofstream out(resultsFile, std::ios::app);
    auto emit = [&](json j) { out << j.dump(-1, ' ', false, json::error_handler_t::replace) << "\n"; out.flush(); };
    PluginInfo info;
    std::string err;
    if (!resolvePlugin(spec, info, err, true)) { emit({{"step", "open"}, {"ok", false}, {"error", err}}); return 1; }
    const bool instrument = isInstrument(info);
    auto t0 = std::chrono::steady_clock::now();
    Audio noise;
    noise.resize((size_t)(kLength * 48000));
    uint32_t rng = 12345;
    for (size_t i = (size_t)(kOn * 48000); i < (size_t)((kOn + kLen) * 48000); ++i) {
        rng = rng * 1664525u + 1013904223u;
        noise.left[i] = noise.right[i] = (float)(((rng >> 8) / 8388608.0 - 1.0) * 0.1);
    }

    // defaults and the reference render: measured here, or (one preset per process) read from the full run
    std::vector<ParamInfo> params0;
    Take base;
    bool warmedUp = false, arpeggio = false;
    std::vector<PresetInfo> presets;
    std::vector<size_t> pick;
    if (only >= 0) {
        std::ifstream in(baseFile);
        std::string line;
        while (std::getline(in, line)) {
            const json j = json::parse(line, nullptr, false);
            if (!j.is_object()) continue;
            if (j.value("step", "") == "defaults")
                for (auto &v : j["values"]) { ParamInfo pi{}; pi.id = v[0]; pi.value = v[1]; pi.min = 0; pi.max = v[2]; params0.push_back(pi); }
            if (j.value("step", "") == "render") {
                base.ok = j.value("ok", false); base.silent = j.value("silent", true); base.lufs = j.value("lufs", -120.0);
                base.centroid = j.value("centroid", 0.0); base.midDb = j.value("midDb", -120.0); base.sideDb = j.value("sideDb", -120.0); warmedUp = j.value("warmedUp", false);
                arpeggio = j.value("arpeggio", false);
                if (j.contains("bands")) for (int b = 0; b < 6; ++b) base.bands[b] = j["bands"][b];
            }
        }
        std::string perr;
        presets = listPresets(info, false, perr);
        if ((size_t)only < presets.size()) pick.push_back((size_t)only);
    } else {
        PluginSetup setup;
        setup.spec = spec;
        OpenedPlugin p;
        const bool opened = openPlugin(setup, "compat", p, err);
        emit({{"step", "open"}, {"ok", opened}, {"ms", std::round(since(t0))}, {"error", err}, {"params", opened ? p.plugin->params().size() : 0}});
        if (!opened) return 1;
        params0 = p.plugin->params();
        json values = json::array();
        for (auto &x : params0) values.push_back({x.id, x.value, std::fabs(x.max - x.min)});
        emit({{"step", "defaults"}, {"values", values}});

        std::string perr;
        presets = listPresets(info, false, perr);
        size_t programs = 0, files = 0;
        for (auto &x : presets) { if (x.stateFile) ++files; else if (x.category == "Programs") ++programs; }   // the rest: the plugin's own discovery
        emit({{"step", "presets"}, {"count", presets.size()}, {"programs", programs}, {"files", files}, {"other", presets.size() - programs - files}, {"error", perr}});

        // the test render (instruments get a longer warm-up when silent: samples stream in after activation;
        // then the notes one at a time)
        double warm = 0.4;
        base = renderTest(p, instrument, warm, noise);
        if (base.ok && base.silent && instrument) {
            Take again = renderTest(p, instrument, 5.0, noise);
            if (again.ok && !again.silent) { base = std::move(again); warmedUp = true; warm = 5.0; }
        }
        if (base.ok && base.silent && instrument) {
            Take again = renderTest(p, instrument, 0.4, noise, true);
            if (again.ok && !again.silent) { base = std::move(again); arpeggio = true; }
        }
        json bands = json::array();
        for (double b : base.bands) bands.push_back(std::round(b * 10) / 10);
        json r = {{"step", "render"}, {"ok", base.ok}, {"error", base.error}, {"silent", base.silent}, {"garbage", base.garbage},
                  {"lufs", std::round(base.lufs * 10) / 10}, {"peakDb", std::round(base.peakDb * 10) / 10}, {"ms", std::round(base.ms)},
                  {"realtime", std::round(std::max(0.0, base.ms - warm * 1000) / 1000 / kLength * 100) / 100}, {"warmedUp", warmedUp},
                  {"arpeggio", arpeggio}, {"centroid", std::round(base.centroid)}, {"bands", bands},
                  {"midDb", std::round(base.midDb * 10) / 10}, {"sideDb", std::round(base.sideDb * 10) / 10},
                  {"oneSided", base.oneSided == 1 ? "left" : base.oneSided == 2 ? "right" : ""}};
        if (!instrument && base.ok) r["passthrough"] = sameAudio(base.audio, noise);
        emit(r);

        // state: save, load it back, save again
        {
            json rec = {{"step", "state"}};
            std::vector<uint8_t> a, b;
            std::string e;
            const bool saved = p.plugin->getState(a, e);
            rec["saved"] = saved;
            rec["bytes"] = a.size();
            if (saved) {
                StateFile sf;
                sf.state = a;
                sf.format = "raw";
                const bool reloaded = p.plugin->loadState(sf, e);
                rec["reloaded"] = reloaded;
                if (reloaded && p.plugin->getState(b, e)) rec["stable"] = a == b;
            }
            if (!e.empty()) rec["error"] = e;
            emit(rec);
        }
        // presets spread over the list, one of each source when there are several
        if (!presets.empty() && presetCount > 0) {
            auto add = [&](size_t i) { if (i < presets.size() && std::find(pick.begin(), pick.end(), i) == pick.end() && (int)pick.size() < presetCount) pick.push_back(i); };
            {   // a program from the middle of its list (the first is usually what the plugin starts with)
                std::vector<size_t> progs;
                for (size_t i = 0; i < presets.size(); ++i) if (!presets[i].stateFile) progs.push_back(i);
                if (!progs.empty()) add(progs[progs.size() / 2]);
            }
            for (size_t i = presets.size(); i-- > 0;) if (presets[i].stateFile) { add(i); break; }
            for (int k = 0; (int)pick.size() < presetCount && k < 8; ++k) add(presets.size() * (size_t)(k + 1) / 5);
        }
        json names = json::array();
        for (size_t i : pick) names.push_back(presets[i].name);
        emit({{"step", "picks"}, {"indices", pick}, {"names", names}});
    }
    // each preset loads into a fresh instance, as a render opens it (not over another state)
    for (size_t i : pick) {
        const PresetInfo &pr = presets[i];
        json rec = {{"step", "preset"}, {"index", i}, {"name", pr.name}, {"category", pr.category}, {"source", pr.stateFile ? "file" : "program"}};
        if (only >= 0) rec["isolated"] = true;
        PluginSetup ps;
        ps.spec = spec;
        if (pr.stateFile && !pr.location.empty()) ps.stateFile = pr.location;
        else ps.preset = pr.category.empty() || pr.category == "Programs" ? pr.name : pr.category + "/" + pr.name;
        OpenedPlugin q;
        std::string e;
        const bool ok = openPlugin(ps, "compat", q, e);
        rec["ok"] = ok;
        if (!ok) { rec["error"] = e; emit(rec); continue; }
        q.plugin->pump(100);
        const auto now = q.plugin->params();
        int changed = 0;
        for (const auto &a : params0)
            for (const auto &b : now)
                if (a.id == b.id) { if (std::fabs(a.value - b.value) > 1e-6 * std::max(1.0, std::fabs(a.max - a.min))) ++changed; break; }
        rec["paramsChanged"] = changed;
        Take t = renderTest(q, instrument, warmedUp ? 5.0 : 0.4, noise, arpeggio);
        rec["soundChanged"] = t.ok && soundsDifferent(t, base);
        if (t.ok) rec["lufs"] = std::round(t.lufs * 10) / 10;
        else rec["renderError"] = t.error;
        emit(rec);
    }
    emit({{"step", "done"}, {"ms", std::round(since(t0))}});
    std::fflush(nullptr);
    return 0;
}

int runCompat(const CompatOptions &opt, json &result, std::string &err) {
    std::vector<std::string> warnings;
    std::vector<PluginInfo> all = scanPlugins(false, warnings), todo;
    const json blocked = blockedPlugins();
    if (!opt.plugins.empty()) {
        for (auto &s : opt.plugins) {
            PluginInfo p;
            if (!resolvePlugin(s, p, err, true)) return 1;
            todo.push_back(p);
        }
    } else
        for (auto &p : all) if (opt.format.empty() || p.format == opt.format) todo.push_back(p);

    json cache;
    {
        std::ifstream in(compatCachePath());
        cache = in ? json::parse(in, nullptr, false) : json::object();
        if (!cache.is_object() || !cache.contains("plugins")) cache = {{"plugins", json::object()}};
    }
    const fs::path tmp = fs::temp_directory_path() / ("wavelength-compat-" + std::to_string(platform::processId()));
    fs::create_directories(tmp);
    const std::string self = platform::selfExecutable();
    struct Worker {
        PluginInfo info;
        platform::Process proc;
        std::string results, base;         // this run's step file; the full run's (isolated presets read its defaults)
        size_t lines = 0;
        std::chrono::steady_clock::time_point last, start;
        std::vector<json> steps;           // every run's steps so far
        std::vector<size_t> isolate;       // presets still to test, one process each
        int current = -1;                  // the preset this run tests (-1 = the full run)
        std::string ended;                 // how the full run ended, when it did not finish
        std::vector<std::string> extra;    // findings from the isolated runs
    };
    std::vector<Worker> running;
    size_t next = 0, finished = 0, tested = 0, reused = 0;
    std::vector<std::string> keys;
    auto save = [&] {
        std::string werr;
        platform::writeFileAtomic(compatCachePath(), cache.dump(1, ' ', false, json::error_handler_t::replace), werr);
    };
    auto readSteps = [](const std::string &file) {
        std::vector<json> steps;
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) { const json j = json::parse(line, nullptr, false); if (j.is_object()) steps.push_back(j); }
        return steps;
    };
    auto startNext = [&]() -> bool {
        while (next < todo.size()) {
            const PluginInfo &p = todo[next++];
            const std::string k = key(p);
            keys.push_back(k);
            if (blocked.contains(p.id)) {
                cache["plugins"][k] = {{"name", p.name}, {"format", p.format}, {"id", p.id}, {"status", "blocked"},
                                       {"fails", json::array()}, {"warnings", json::array()}, {"notes", json::array({"blocked: " + blocked[p.id].value("reason", std::string("by the user"))})}};
                ++finished;
                continue;
            }
            const json &old = cache["plugins"].contains(k) ? cache["plugins"][k] : json();
            if (!opt.rebuild && old.is_object() && old.value("test", 0) == kTestVersion && old.value("engine", "") == WAVELENGTH_VERSION && old.value("bundleTime", 0LL) == bundleTime(p) &&
                old.value("pluginVersion", "") == p.version) { ++reused; ++finished; continue; }
            Worker w;
            w.info = p;
            w.results = w.base = (tmp / ("r" + std::to_string(next) + ".jsonl")).string();
            std::ofstream(w.results).close();
            platform::spawn({self, "__compat", specOf(p), w.results, std::to_string(opt.presets)}, w.proc, false, !opt.verbose);
            w.last = w.start = std::chrono::steady_clock::now();
            running.push_back(std::move(w));
            return true;
        }
        return false;
    };
    const int jobs = std::clamp(opt.jobs, 1, 4);
    while ((int)running.size() < jobs && startNext()) {}
    while (!running.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (size_t i = 0; i < running.size();) {
            Worker &w = running[i];
            const auto steps = readSteps(w.results);
            if (steps.size() > w.lines) { w.lines = steps.size(); w.last = std::chrono::steady_clock::now(); }
            std::string crash, ended;
            bool exited = platform::finished(w.proc, crash);
            const bool hung = !exited && since(w.last) > opt.timeoutSec * 1000.0;
            const bool window = !exited && !hung && platform::hasOnscreenWindow(w.proc.id);
            if (hung || window) { platform::kill(w.proc); exited = true; }
            if (!exited) { ++i; continue; }
            const auto final = readSteps(w.results);
            const bool done = !final.empty() && final.back().value("step", "") == "done";
            if (window) ended = "opened a window (a licence or registration dialog?)";
            else if (hung) ended = "hung (no progress for " + std::to_string(opt.timeoutSec) + " s)";
            else if (!crash.empty()) ended = "crashed (" + crash + ")";
            else if (!done && !(final.size() == 1 && !final[0].value("ok", true))) ended = "ended without finishing";
            if (w.current < 0) {   // the full run
                for (auto &x : final) if (x.value("step", "") != "done") w.steps.push_back(x);
                // it got through the plugin's own tests and died among the presets: test the rest one process each
                size_t picked = 0, tested = 0;
                std::vector<size_t> picks;
                for (auto &x : final) {
                    if (x.value("step", "") == "picks") for (auto &k : x["indices"]) picks.push_back(k.get<size_t>());
                    if (x.value("step", "") == "preset") ++tested;
                }
                picked = picks.size();
                if (!ended.empty() && !window && picked > tested) {
                    w.isolate.assign(picks.begin() + (long)tested, picks.end());
                    w.extra.push_back("__second_instance");   // resolved once the first isolated preset runs
                    ended.clear();
                } else w.ended = ended;
            } else {   // one preset in its own process
                bool reported = false;
                for (auto &x : final) if (x.value("step", "") == "preset") { w.steps.push_back(x); reported = true; }
                std::string name = "#" + std::to_string(w.current);
                for (auto &x : w.steps) if (x.value("step", "") == "picks")
                    for (size_t k = 0; k < x["indices"].size(); ++k) if (x["indices"][k].get<int>() == w.current) name = x["names"][k].get<std::string>();
                const bool first = !w.extra.empty() && w.extra.back() == "__second_instance";
                if (first) w.extra.pop_back();
                if (!reported && !ended.empty()) w.extra.push_back("preset '" + name + "' " + ended);
                else if (first) w.extra.push_back("crashes when a second instance opens in the same process (its presets were tested one process each)");
            }
            if (!w.isolate.empty()) {   // next isolated preset in the same slot
                w.current = (int)w.isolate.front();
                w.isolate.erase(w.isolate.begin());
                w.results = w.base + "." + std::to_string(w.current);
                std::ofstream(w.results).close();
                platform::spawn({self, "__compat", specOf(w.info), w.results, std::to_string(opt.presets), std::to_string(w.current), w.base}, w.proc, false, !opt.verbose);
                w.lines = 0;
                w.last = std::chrono::steady_clock::now();
                ++i;
                continue;
            }
            ended = w.ended;
            std::vector<json> all = w.steps;
            if (w.current < 0) all = final;   // the full run finished normally
            all.push_back({{"step", "done"}});
            json rec = summarize(w.info, all, ended);
            for (auto &x : w.extra) rec["warnings"].push_back(x);
            if (rec["status"] == "ok" && !w.extra.empty()) rec["status"] = "warn";
            rec["engine"] = WAVELENGTH_VERSION;
            rec["test"] = kTestVersion;
            rec["bundleTime"] = bundleTime(w.info);
            rec["pluginVersion"] = w.info.version;
            rec["seconds"] = std::round(since(w.start) / 100) / 10;
            cache["plugins"][key(w.info)] = rec;
            ++finished;
            ++tested;
            save();
            if (opt.progress) {
                std::string issues;
                for (const char *k : {"fails", "warnings"}) for (auto &x : rec[k]) issues += (issues.empty() ? "" : "; ") + x.get<std::string>();
                std::fprintf(stderr, "[%zu/%zu] %-5s %-4s %s (%.0f s)%s%s\n", finished, todo.size(), rec["status"].get<std::string>().c_str(),
                             w.info.format.c_str(), w.info.name.c_str(), rec["seconds"].get<double>(), issues.empty() ? "" : ": ", issues.substr(0, 200).c_str());
            }
            std::error_code ec;
            fs::remove(w.results, ec);
            running.erase(running.begin() + (long)i);
            startNext();
        }
        while ((int)running.size() < jobs && startNext()) {}
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
    save();

    std::map<std::string, int> counts;
    json list = json::array();
    for (auto &k : keys)
        if (cache["plugins"].contains(k)) { list.push_back(cache["plugins"][k]); ++counts[cache["plugins"][k].value("status", "?")]; }
    std::stable_sort(list.begin(), list.end(), [](const json &a, const json &b) {
        auto rank = [](const std::string &s) { return s == "fail" ? 0 : s == "warn" ? 1 : s == "blocked" ? 2 : 3; };
        return rank(a.value("status", "")) < rank(b.value("status", ""));
    });
    const std::string summary = std::to_string(list.size()) + " plugins: " + std::to_string(counts["ok"]) + " work, " + std::to_string(counts["warn"]) +
                                " work with warnings, " + std::to_string(counts["fail"]) + " fail, " + std::to_string(counts["blocked"]) + " blocked (" +
                                std::to_string(tested) + " tested now, " + std::to_string(reused) + " from the cache: " + compatCachePath() + ")";
    result = {{"ok", true}, {"summary", summary}, {"plugins", list}};
    if (!opt.report.empty()) {
        std::string werr;
        if (!platform::writeFileAtomic(opt.report, markdownReport(result), werr)) { err = werr; return 1; }
        result["report"] = opt.report;
    }
    return 0;
}

} // namespace wl
