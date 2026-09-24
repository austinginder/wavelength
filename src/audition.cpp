#include "audition.hpp"

#include "analyze.hpp"
#include "catalog.hpp"
#include "engine.hpp"
#include "preset_files.hpp"
#include "state_file.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace wl {

namespace {

std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

std::string indexPath(const PluginInfo &info) {
    const char *h = getenv("HOME");
    std::string id;
    for (char c : info.id) id += std::isalnum((unsigned char)c) || c == '.' || c == '-' ? c : '_';
    return std::string(h ? h : "") + "/Library/Caches/wavelength/audition/" + info.format + "-" + id + ".json";
}

// audition: C4 from 0.5 s for 1 s, 3 s in all
constexpr double kNoteOn = 0.5, kNoteLen = 1.0, kLength = 3.0;
constexpr int kKey = 60;

std::vector<std::string> tagsFor(const json &r) {
    std::vector<std::string> t;
    if (r.value("silent", false)) return {"silent"};
    if (r.value("selfPlaying", false)) t.push_back("self-playing");
    const json p = r.value("pitch", json::object());
    const double conf = p.value("confidence", 0.0);
    if (conf >= 0.5 && p.value("octaveOffset", 0) != 0) {
        const int o = p.value("octaveOffset", 0);
        t.push_back(std::string("octave ") + (o > 0 ? "+" : "") + std::to_string(o));
    }
    if (conf < 0.3) t.push_back("unpitched");
    const double c = r.value("centroidHz", 0.0);
    if (c < 400) t.push_back("dark"); else if (c < 1200) t.push_back("warm"); else if (c > 3000) t.push_back("bright");
    const json b = r.value("bandsDb", json::object());
    if (b.value("sub", -120.0) > -6) t.push_back("sub");
    else if (b.value("bass", -120.0) > -4) t.push_back("bassy");
    if (b.value("air", -120.0) > -15 || b.value("presence", -120.0) > -8) t.push_back("airy");
    const double attack = r.value("attackMs", 0.0), sustain = r.value("sustainDb", 0.0), decay = r.value("decayMs", 0.0);
    const bool rhythmic = r.value("onsets", 0) >= 3;
    if (rhythmic) t.push_back("rhythmic");
    else if (attack > 150) t.push_back("slow attack");
    if (sustain < -20) t.push_back(decay > 0 && decay < 400 ? "pluck" : "short");
    else if (sustain > -6) t.push_back("sustained");
    if (r.value("releaseMs", 0.0) > 1000) t.push_back("long release");
    const double w = r.value("width", 0.0);
    if (w > 0.5) t.push_back("wide"); else if (w < 0.05) t.push_back("mono");
    return t;
}

json measure(const Audio &a, int sr, bool selfPlaying) {
    const Analysis all = analyzeAudio(a, sr);
    json r = {{"silent", all.silent}};
    if (all.silent) return r;
    const Analysis note = analyzeAudio(a, sr, kNoteOn, kNoteOn + kNoteLen);   // the held note
    const int offset = note.pitchKey >= 0 ? note.pitchKey - kKey : 0;
    r["pitch"] = {{"note", keyName(note.pitchKey)}, {"offset", offset}, {"octaveOffset", (int)std::lround(offset / 12.0)},
                  {"cents", std::lround(note.pitchCents)}, {"confidence", std::round(note.pitchConfidence * 100) / 100}};
    r["lufs"] = std::round(all.lufs * 10) / 10;
    r["peakDb"] = std::round(all.peakDb * 10) / 10;
    r["centroidHz"] = std::lround(note.centroidHz);
    r["bandsDb"] = {{"sub", std::round(note.bandsDb[0] * 10) / 10}, {"bass", std::round(note.bandsDb[1] * 10) / 10},
                    {"lowMid", std::round(note.bandsDb[2] * 10) / 10}, {"highMid", std::round(note.bandsDb[3] * 10) / 10},
                    {"presence", std::round(note.bandsDb[4] * 10) / 10}, {"air", std::round(note.bandsDb[5] * 10) / 10}};
    r["attackMs"] = std::lround(note.attackMs);
    r["decayMs"] = std::lround(note.decayMs);
    r["sustainDb"] = std::round(note.sustainDb * 10) / 10;
    // release: from note-off until 20 dB below the note's loudest moment (10 ms RMS frames)
    {
        const size_t hop = (size_t)(0.01 * sr), on = (size_t)(kNoteOn * sr), off = (size_t)((kNoteOn + kNoteLen) * sr);
        auto rms = [&](size_t at) {
            double s = 0;
            for (size_t i = at; i < at + hop && i < a.frames(); ++i) s += 0.5 * ((double)a.left[i] * a.left[i] + (double)a.right[i] * a.right[i]);
            return std::sqrt(s / (double)hop);
        };
        double peak = 0;
        for (size_t i = on; i + hop <= off; i += hop) peak = std::max(peak, rms(i));
        size_t i = off;
        while (i + hop <= a.frames() && rms(i) > peak * 0.1) i += hop;
        r["releaseMs"] = std::lround((double)(i - off) / sr * 1000);
    }
    r["width"] = std::round(note.width * 100) / 100;
    r["selfPlaying"] = selfPlaying;
    r["onsets"] = note.onsets.size();   // several onsets from one held note: an arp, sequence or rhythmic patch
    return r;
}

} // namespace

std::vector<PresetInfo> listPresets(const PluginInfo &info, bool rescanNks, std::string &err) {
    std::vector<PresetInfo> presets;
    if (info.format == "vst3") {   // factory programs from the plugin's program list
        auto plugin = createPlugin(info, err);
        if (!plugin) return presets;
        for (auto &n : plugin->programs()) { PresetInfo p; p.name = n; p.category = "Programs"; presets.push_back(p); }
    } else {
        std::string discoverErr;
        discoverPresets(info.bundlePath, info.id, presets, discoverErr);
    }
    if (rescanNks) nksPresets(info, true);
    std::set<std::string> listed;   // a preset file with a program's name is the same sound: list it once
    for (auto &p : presets) listed.insert(lower(p.name));
    for (auto &p : filePresets(info))
        if (listed.insert(lower(p.name)).second) presets.push_back(p);
    return presets;
}

json auditionIndex(const PluginInfo &info) {
    std::ifstream in(indexPath(info));
    if (!in) return json::object();
    const json j = json::parse(in, nullptr, false);
    return j.is_object() && j.contains("presets") ? j["presets"] : json::object();
}

int auditionWorker(const std::string &plugin, const std::string &batchFile, const std::string &resultsFile) {
    std::ifstream bin(batchFile);
    const json batch = json::parse(bin, nullptr, false);
    std::ofstream out(resultsFile, std::ios::app);
    if (!batch.is_array() || !out) return 2;
    PluginSetup setup;
    setup.spec = plugin;
    OpenedPlugin p;
    std::string err;
    if (!openPlugin(setup, "audition", p, err)) {
        for (auto &b : batch) out << json{{"name", b["name"]}, {"error", err}}.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
        return 1;
    }
    PluginInfo info;
    resolvePlugin(plugin, info, err);
    Job job;
    job.sampleRate = 48000;
    job.blockSize = 512;
    job.warmup = 0.4;
    const std::vector<TimedEvent> events = scheduleNotes({{kNoteOn, kNoteLen, kKey, 0, 0.8, {}}}, job.sampleRate);
    for (auto &b : batch) {
        const std::string name = b.value("name", ""), location = b.value("location", "");
        json rec;
        std::string e, loaded, fmt;
        bool ok;
        if (!location.empty()) {   // a preset file or bank entry: load exactly that one
            StateFile sf;
            ok = readStateFile(location, "auto", sf, e) && loadStateInto(*p.plugin, sf, e);
        } else ok = loadPresetByName(*p.plugin, info, name, loaded, fmt, e);
        if (ok) {
            p.plugin->pump(100);
            // 1.5 s with no notes: lets the previous preset's tail die away, and a patch still
            // sounding (not decaying) at the end plays by itself (latched arp, drone, sequencer)
            Audio quiet;
            quiet.resize((size_t)(1.5 * job.sampleRate));
            std::vector<std::string> warnings;
            ok = p.plugin->render(job, {}, {}, {}, nullptr, quiet, warnings, e);
            bool selfPlaying = false;
            if (ok) {
                const Analysis early = analyzeAudio(quiet, job.sampleRate, 0.25, 0.75), late = analyzeAudio(quiet, job.sampleRate, 1.0, 1.5);
                selfPlaying = !late.silent && late.peakDb > -50 && late.rmsDb > early.rmsDb - 6;
            }
            Audio audio;
            audio.resize((size_t)(kLength * job.sampleRate));
            if (ok) ok = p.plugin->render(job, events, {}, {}, nullptr, audio, warnings, e);
            if (ok) rec = measure(audio, job.sampleRate, selfPlaying);
        }
        if (!ok) rec = {{"error", e}};
        rec["name"] = name;
        out << rec.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
        out.flush();
    }
    return 0;
}

int runAudition(const PluginInfo &info, int jobs, int limit, bool rebuild, bool verbose, std::string &err, std::string &summary) {
    auto presets = listPresets(info, false, err);
    if (presets.empty()) { if (err.empty()) err = info.name + " has no presets to audition"; return 1; }
    json index = rebuild ? json::object() : auditionIndex(info);
    std::vector<json> todo;
    for (auto &p : presets) {
        if (index.contains(p.name)) continue;
        todo.push_back({{"name", p.name}, {"location", p.stateFile ? p.location : ""}});
        if (limit > 0 && (int)todo.size() >= limit) break;
    }
    const size_t already = index.size();
    const auto t0 = std::chrono::steady_clock::now();
    const std::string self = selfExecutable(), spec = info.bundlePath + "#" + info.id;
    const fs::path tmp = fs::temp_directory_path() / ("wavelength-audition-" + std::to_string(getpid()));
    fs::create_directories(tmp);

    struct Worker { pid_t pid = 0; std::vector<json> batch; std::string results; size_t done = 0; std::chrono::steady_clock::time_point last; int n = 0; };
    std::vector<Worker> workers;
    size_t crashed = 0, timedOut = 0, serial = 0;
    auto spawn = [&](std::vector<json> batch) {
        if (batch.empty()) return;
        Worker w;
        w.batch = std::move(batch);
        w.n = (int)serial++;
        const std::string bf = (tmp / ("batch" + std::to_string(w.n) + ".json")).string();
        w.results = (tmp / ("results" + std::to_string(w.n) + ".jsonl")).string();
        std::ofstream(bf) << json(w.batch).dump();
        std::ofstream(w.results).close();
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        if (!verbose) posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        std::vector<std::string> args = {self, "__audition", spec, bf, w.results};
        std::vector<char *> argv;
        for (auto &s : args) argv.push_back(s.data());
        argv.push_back(nullptr);
        if (posix_spawn(&w.pid, self.c_str(), &fa, nullptr, argv.data(), environ) != 0) w.pid = 0;
        posix_spawn_file_actions_destroy(&fa);
        w.last = std::chrono::steady_clock::now();
        workers.push_back(std::move(w));
    };
    auto readResults = [&](Worker &w) {
        std::ifstream in(w.results);
        std::string line;
        size_t n = 0;
        while (std::getline(in, line)) {
            const json r = json::parse(line, nullptr, false);
            if (!r.is_object() || !r.contains("name")) continue;
            ++n;
            json rec = r;
            rec.erase("name");
            if (!rec.contains("error")) rec["tags"] = tagsFor(rec);
            index[r["name"].get<std::string>()] = rec;
        }
        return n;
    };
    // round-robin batches so each worker gets a spread of categories
    jobs = std::max(1, std::min<int>(jobs, (int)todo.size()));
    std::vector<std::vector<json>> batches((size_t)jobs);
    for (size_t i = 0; i < todo.size(); ++i) batches[i % (size_t)jobs].push_back(todo[i]);
    for (auto &b : batches) spawn(b);

    const double hangSeconds = 120;
    while (!workers.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        for (size_t i = 0; i < workers.size();) {
            Worker &w = workers[i];
            const size_t n = readResults(w);
            if (n > w.done) { w.done = n; w.last = std::chrono::steady_clock::now(); }
            int status = 0;
            const bool exited = w.pid == 0 || waitpid(w.pid, &status, WNOHANG) == w.pid;
            const bool hung = !exited && std::chrono::duration<double>(std::chrono::steady_clock::now() - w.last).count() > hangSeconds;
            if (hung) { kill(w.pid, SIGKILL); waitpid(w.pid, &status, 0); }
            if (!exited && !hung) { ++i; continue; }
            w.done = readResults(w);
            std::vector<json> rest(w.batch.begin() + (long)std::min(w.done, w.batch.size()), w.batch.end());
            if (!rest.empty()) {   // the preset it stopped on crashed or hung: record it, go on with the rest
                const std::string why = hung ? "timed out (no progress for 120 s)" : "crashed the plugin";
                (hung ? timedOut : crashed)++;
                index[rest.front()["name"].get<std::string>()] = {{"error", why}};
                rest.erase(rest.begin());
            }
            Worker done = std::move(w);
            workers.erase(workers.begin() + (long)i);
            spawn(std::move(rest));
            if (verbose) std::fprintf(stderr, "audition: %zu of %zu presets measured\n", index.size() - already, todo.size());
        }
    }
    fs::create_directories(fs::path(indexPath(info)).parent_path());
    std::ofstream(indexPath(info)) << json{{"plugin", info.name}, {"id", info.id}, {"note", "C4, 1 s at 0.5 s, velocity 0.8"},
                                            {"presets", index}}.dump(1, ' ', false, json::error_handler_t::replace);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    size_t failed = 0;
    for (auto &[k, v] : index.items()) failed += v.contains("error");
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    summary = std::to_string(todo.size()) + " presets auditioned in " + std::to_string((int)secs) + " s (" + std::to_string(crashed) +
              " crashed, " + std::to_string(timedOut) + " timed out); index holds " + std::to_string(index.size()) + " of " +
              std::to_string(presets.size()) + " (" + std::to_string(failed) + " failed): " + indexPath(info);
    return 0;
}

} // namespace wl
