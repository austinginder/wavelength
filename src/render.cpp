#include "render.hpp"

#include "builtins.hpp"
#include "clips.hpp"
#include "dsp.hpp"
#include "effects.hpp"
#include "engine.hpp"
#include "loudness.hpp"
#include "catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <unistd.h>
#include <thread>
#include <sys/wait.h>
#include <spawn.h>
#include <fstream>
#include <fcntl.h>
#include <csignal>

namespace fs = std::filesystem;
extern char **environ;

namespace wl {

namespace {

std::string slug(const std::string &s) {
    std::string out;
    for (char c : s) out += std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '-';
    while (out.find("--") != std::string::npos) out.replace(out.find("--"), 2, "-");
    return out.empty() ? "track" : out;
}

using Chain = std::vector<std::unique_ptr<Effect>>;

bool buildChain(const nlohmann::json &list, const Job &job, const std::string &context, Chain &chain, std::string &err) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].is_object() && list[i].value("bypass", false)) continue;
        auto fx = makeEffect(list[i], job, context + " fx[" + std::to_string(i) + "]", err);
        if (!fx) return false;
        chain.push_back(std::move(fx));
    }
    return true;
}

bool runChain(Chain &chain, Audio &a, const FxContext &ctx, std::vector<std::string> &labels, std::vector<std::string> &warnings,
              const std::string &context, std::string &err) {
    for (auto &fx : chain) {
        if (!fx->process(a, ctx, err)) { err = context + ": " + err; return false; }
        labels.push_back(fx->label);
        for (auto &w : fx->warnings) warnings.push_back(w);
    }
    return true;
}

// Instrument stage: a CLAP plugin or a built-in synth.
bool renderInstrument(const Job &job, const Track &track, Audio &audio, TrackResult &tr, bool verbose, std::string &err) {
    tr.notes = track.notes.size();
    for (auto &w : track.warnings) tr.warnings.push_back(w);
    if (isBuiltin(track.plugin)) {
        tr.plugin = tr.pluginName = track.plugin;
        if (!track.stateFile.empty() || !track.preset.empty() || !track.params.empty() || !track.paramAutomation.empty())
            tr.warnings.push_back("built-in instruments ignore state, params and parameter automation");
        return renderBuiltin(track.plugin, job, track, audio, tr.warnings, err);
    }
    PluginSetup setup;
    setup.spec = track.plugin;
    setup.stateFile = track.stateFile;
    setup.stateFormat = track.stateFormat;
    setup.params = track.params;
    setup.automation = track.paramAutomation;
    setup.verbose = verbose;
    setup.warmup = track.warmup;
    setup.preset = track.preset;
    OpenedPlugin p;
    if (!openPlugin(setup, track.name, p, err)) return false;
    tr.plugin = p.id;
    tr.pluginName = p.name;
    tr.stateFormat = p.stateFormat;
    tr.preset = p.preset;
    tr.paramsApplied = track.params.size();
    tr.automated = p.autos.size();
    auto events = scheduleNotes(track.notes, job.sampleRate);
    scheduleControllers(track, job.sampleRate, (double)audio.frames() / job.sampleRate, events);
    if (!runPlugin(job, p, events, nullptr, audio, err)) { err = track.name + ": " + err; return false; }
    tr.latencySamples += p.plugin->latencySamples;
    for (auto &w : p.warnings) tr.warnings.push_back(w);
    return true;
}


// Instrument + effect chain for track `i` (what a worker process renders).
bool renderTrackAudio(const Job &job, size_t i, Chain &chain, const FxContext &ctx, Audio &audio, TrackResult &tr, bool verbose,
                      std::string &err) {
    const Track &track = job.tracks[i];
    if (!renderInstrument(job, track, audio, tr, verbose, err)) return false;
    if (!runChain(chain, audio, ctx, tr.fx, tr.warnings, "track '" + track.name + "'", err)) return false;
    for (auto &fx : chain) tr.latencySamples += fx->latencySamples;
    return true;
}

nlohmann::json trackToJson(const TrackResult &t) {
    return {{"ok", true}, {"plugin", t.plugin}, {"pluginName", t.pluginName}, {"stateFormat", t.stateFormat}, {"preset", t.preset},
            {"notes", t.notes}, {"paramsApplied", t.paramsApplied}, {"automated", t.automated}, {"fx", t.fx},
            {"warnings", t.warnings}, {"latencySamples", t.latencySamples}};
}

void trackFromJson(const nlohmann::json &j, TrackResult &t) {
    t.plugin = j.value("plugin", ""); t.pluginName = j.value("pluginName", ""); t.stateFormat = j.value("stateFormat", "");
    t.preset = j.value("preset", ""); t.notes = j.value("notes", (size_t)0); t.paramsApplied = j.value("paramsApplied", (size_t)0);
    t.automated = j.value("automated", (size_t)0); t.fx = j.value("fx", std::vector<std::string>());
    t.warnings = j.value("warnings", std::vector<std::string>()); t.latencySamples = j.value("latencySamples", 0u);
}

} // namespace

int renderTrackWorker(const std::string &jobPath, size_t index, const std::string &prefix) {
    auto fail = [&](const std::string &e) { std::ofstream(prefix + ".json") << nlohmann::json{{"ok", false}, {"error", e}}.dump(); return 1; };
    std::ifstream in(jobPath);
    const nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
    Job job;
    std::string err;
    if (j.is_discarded() || !parseJob(j, fs::absolute(jobPath).parent_path().string(), job, err)) return fail("cannot read job: " + err);
    if (index >= job.tracks.size()) return fail("no track " + std::to_string(index));
    double end = 0;
    for (const auto &t : job.tracks) for (const auto &n : t.notes) end = std::max(end, n.start + n.length);
    for (const auto &t : job.tracks) if (!t.clips.empty()) end = std::max(end, clipsEndSeconds(job, t));
    const double seconds = job.length > 0 ? job.length : end + job.tail;
    const size_t frames = (size_t)std::ceil(seconds * job.sampleRate);
    Chain chain;
    if (!buildChain(job.tracks[index].fx, job, "track '" + job.tracks[index].name + "'", chain, err)) return fail(err);
    const FxContext ctx{job, false};
    Audio audio;
    audio.resize(frames);
    TrackResult tr;
    tr.name = job.tracks[index].name;
    if (!renderTrackAudio(job, index, chain, ctx, audio, tr, false, err)) return fail(err);
    std::ofstream pcm(prefix + ".pcm", std::ios::binary);
    pcm.write(reinterpret_cast<const char *>(audio.left.data()), (std::streamsize)(frames * sizeof(float)));
    pcm.write(reinterpret_cast<const char *>(audio.right.data()), (std::streamsize)(frames * sizeof(float)));
    pcm.close();
    if (!pcm) return fail("cannot write track audio (disk full?)");
    std::ofstream(prefix + ".json") << trackToJson(tr).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return 0;
}


bool renderJob(const Job &job, const std::string &outDir, bool verbose, RenderResult &result, std::string &err) {
    const auto t0 = std::chrono::steady_clock::now();
    double end = 0;
    for (const auto &t : job.tracks) for (const auto &n : t.notes) end = std::max(end, n.start + n.length);
    for (const auto &t : job.tracks) if (!t.clips.empty()) end = std::max(end, clipsEndSeconds(job, t));
    const double seconds = job.length > 0 ? job.length : end + job.tail;
    const size_t frames = (size_t)std::ceil(seconds * job.sampleRate);
    const double sr = job.sampleRate;
    result.sampleRate = job.sampleRate;
    result.seconds = seconds;

    // 0. build every effect chain first, so a typo fails in milliseconds, not after a long render
    std::vector<Chain> trackChains(job.tracks.size()), busChains(job.buses.size());
    Chain masterChain;
    for (size_t i = 0; i < job.tracks.size(); ++i)
        if (!buildChain(job.tracks[i].fx, job, "track '" + job.tracks[i].name + "'", trackChains[i], err)) return false;
    for (size_t i = 0; i < job.buses.size(); ++i)
        if (!buildChain(job.buses[i].fx, job, "bus '" + job.buses[i].name + "'", busChains[i], err)) return false;
    if (!buildChain(job.masterFx, job, "master", masterChain, err)) return false;

    std::error_code ec;
    fs::create_directories(fs::path(outDir) / "stems", ec);
    if (ec) { err = "cannot create " + outDir + ": " + ec.message(); return false; }
    // never leave a previous render's files next to this one's: a failed render must not look finished
    fs::remove(fs::path(outDir) / "report.json", ec);
    fs::remove(fs::path(outDir) / "mix.wav", ec);
    for (auto &e : fs::directory_iterator(fs::path(outDir) / "stems", ec))
        if (e.path().extension() == ".wav") fs::remove(e.path(), ec);
    {
        const double perFile = (double)frames * 2 * 4 + 64;
        const double need = perFile + (job.stemBits ? job.tracks.size() * (double)frames * 2 * (job.stemBits / 8) : 0);
        const auto space = fs::space(outDir, ec);
        if (!ec && (double)space.available < need * 1.05) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "not enough disk space in %s: this render writes %.0f MB, %.0f MB free (set \"stems\": \"none\" or \"16\" to write less)",
                          outDir.c_str(), need / 1e6, space.available / 1e6);
            err = buf;
            return false;
        }
    }

    const FxContext ctx{job, verbose};
    Audio mix;
    mix.resize(frames);
    std::vector<Audio> buses(job.buses.size());
    for (auto &b : buses) b.resize(frames);

    // 1. tracks: instrument → effects (in this process, or plugin tracks in worker processes, several
    //    at once) → stem; then fader → mix and sends → buses, as each track finishes
    std::vector<TrackResult> trackResults(job.tracks.size());
    std::vector<bool> trackDone(job.tracks.size(), false);
    auto mixTrack = [&](size_t i, Audio &audio, TrackResult &tr) -> bool {
        const Track &track = job.tracks[i];
        char prefix[8];
        std::snprintf(prefix, sizeof prefix, "%02zu-", i + 1);
        if (job.stemBits) {
            tr.file = (fs::path(outDir) / "stems" / (prefix + slug(track.name) + ".wav")).string();
            if (!writeWav(tr.file, audio, job.sampleRate, err, job.stemBits)) return false;
        }
        tr.levels = measure(audio);
        tr.lufs = integratedLufs(audio, job.sampleRate);
        if (tr.levels.silent && !track.notes.empty())
            tr.warnings.push_back("rendered silence: check the notes, the state/preset, and that the plugin is an instrument");

        if (!track.mute) {
            double angle = (track.pan + 1.0) * dsp::kPi / 4.0;
            double pl = std::cos(angle) * M_SQRT2, pr = std::sin(angle) * M_SQRT2;
            const bool panAuto = !track.panAutomation.empty();
            const bool automated = !track.gainAutomation.empty();
            struct Send { Audio *bus; double amt; const Envelope *env; };
            std::vector<Send> sends;
            for (const auto &[busName, db] : track.sends)
                for (size_t b = 0; b < job.buses.size(); ++b)
                    if (job.buses[b].name == busName) {
                        const Envelope *env = nullptr;
                        for (auto &[n, e] : track.sendAutomation) if (n == busName) env = &e;
                        sends.push_back({&buses[b], dsp::dbToLin(db), env});
                    }
            Audio *dest = &mix;   // "output": a group bus instead of the master
            for (size_t b = 0; b < job.buses.size(); ++b) if (job.buses[b].name == track.output) dest = &buses[b];
            double g = dsp::dbToLin(track.gainDb);
            Audio post;   // what this track adds to its output, for per-section loudness
            if (!job.markers.empty()) post.resize(frames);
            for (size_t f = 0; f < frames; ++f) {
                if (automated && f % 32 == 0) g = dsp::dbToLin(track.gainDb + track.gainAutomation.at(f / sr));
                if (f % 32 == 0) for (auto &s : sends) if (s.env) s.amt = dsp::dbToLin(s.env->at(f / sr));
                if (panAuto && f % 32 == 0) {
                    angle = (std::clamp(track.panAutomation.at(f / sr), -1.0, 1.0) + 1.0) * dsp::kPi / 4.0;
                    pl = std::cos(angle) * M_SQRT2; pr = std::sin(angle) * M_SQRT2;
                }
                const float l = (float)(audio.left[f] * g * pl), r = (float)(audio.right[f] * g * pr);
                dest->left[f] += l; dest->right[f] += r;
                if (!post.left.empty()) { post.left[f] = l; post.right[f] = r; }
                for (auto &s : sends) { s.bus->left[f] += (float)(l * s.amt); s.bus->right[f] += (float)(r * s.amt); }
            }
            for (size_t m = 0; m < job.markers.size(); ++m) {
                const double a0 = job.markers[m].sec, b0 = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
                tr.sectionLufs.push_back(integratedLufs(post, job.sampleRate, (size_t)(a0 * sr), (size_t)(b0 * sr)));
            }
        }
        return true;
    };

    int parallel = job.parallel;
    if (parallel < 0) parallel = (int)std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    const bool isolate = parallel > 0 && !job.sourcePath.empty();
    std::vector<size_t> remote;   // plugin tracks for worker processes
    for (size_t i = 0; i < job.tracks.size(); ++i) {
        const Track &track = job.tracks[i];
        if (isolate && !isBuiltin(track.plugin)) { remote.push_back(i); continue; }
        TrackResult &tr = trackResults[i];
        tr.name = track.name;
        Audio audio;
        audio.resize(frames);
        const auto tt = std::chrono::steady_clock::now();
        if (verbose) std::fprintf(stderr, "rendering %s (%s)...\n", track.name.c_str(), track.plugin.c_str());
        if (!renderTrackAudio(job, i, trackChains[i], ctx, audio, tr, verbose, err)) return false;
        if (!mixTrack(i, audio, tr)) return false;
        tr.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - tt).count();
        trackDone[i] = true;
    }
    if (!remote.empty()) {
        const fs::path tmp = fs::temp_directory_path() / ("wavelength-render-" + std::to_string(getpid()));
        fs::create_directories(tmp, ec);
        struct Running { size_t index; pid_t pid; std::chrono::steady_clock::time_point started; };
        std::vector<Running> running;
        size_t next = 0;
        const double limit = 300 + 10 * seconds;   // a track that takes longer than this is hung
        const std::string self = selfExecutable();
        auto startTrack = [&](size_t i) {
            const std::string prefix = (tmp / std::to_string(i)).string();
            std::vector<std::string> args = {self, "__track", job.sourcePath, std::to_string(i), prefix};
            std::vector<char *> argv;
            for (auto &x : args) argv.push_back(x.data());
            argv.push_back(nullptr);
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
            if (!verbose) posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
            pid_t pid = 0;
            if (posix_spawn(&pid, self.c_str(), &fa, nullptr, argv.data(), environ) != 0) pid = 0;
            posix_spawn_file_actions_destroy(&fa);
            if (verbose) std::fprintf(stderr, "rendering %s (%s) in worker %d...\n", job.tracks[i].name.c_str(), job.tracks[i].plugin.c_str(), (int)pid);
            running.push_back({i, pid, std::chrono::steady_clock::now()});
        };
        bool failed = false;
        while ((next < remote.size() || !running.empty()) && !failed) {
            while (next < remote.size() && running.size() < (size_t)parallel) startTrack(remote[next++]);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            for (size_t r = 0; r < running.size();) {
                Running &run = running[r];
                int status = 0;
                const bool exited = run.pid == 0 || waitpid(run.pid, &status, WNOHANG) == run.pid;
                const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - run.started).count();
                const bool hung = !exited && took > limit;
                if (hung) { kill(run.pid, SIGKILL); waitpid(run.pid, &status, 0); }
                if (!exited && !hung) { ++r; continue; }
                const size_t i = run.index;
                const std::string prefix = (tmp / std::to_string(i)).string();
                TrackResult &tr = trackResults[i];
                tr.name = job.tracks[i].name;
                std::ifstream jin(prefix + ".json");
                const nlohmann::json res = jin ? nlohmann::json::parse(jin, nullptr, false) : nlohmann::json();
                if (res.is_object() && res.value("ok", false)) {
                    trackFromJson(res, tr);
                    Audio audio;
                    audio.resize(frames);
                    std::ifstream pin(prefix + ".pcm", std::ios::binary);
                    pin.read(reinterpret_cast<char *>(audio.left.data()), (std::streamsize)(frames * sizeof(float)));
                    pin.read(reinterpret_cast<char *>(audio.right.data()), (std::streamsize)(frames * sizeof(float)));
                    if (!pin) { err = "track '" + tr.name + "': worker output is incomplete"; failed = true; }
                    else if (!mixTrack(i, audio, tr)) failed = true;
                    tr.seconds = took;
                } else if (res.is_object() && res.contains("error")) {   // a job mistake (unknown preset, bad state): as before, the render fails
                    err = res["error"].get<std::string>();
                    failed = true;
                } else {   // the plugin crashed or hung: the song renders without this track
                    tr.plugin = tr.pluginName = job.tracks[i].plugin;
                    const std::string why = hung ? "hung (killed after " + std::to_string((int)limit) + " s)" : "crashed while rendering";
                    tr.warnings.push_back("track failed: " + job.tracks[i].plugin + " " + why + "; the mix is rendered without it");
                    tr.lufs = -120;
                    tr.levels = Levels{-240, -240, -240, true};
                    result.failedTracks.push_back(tr.name);
                    result.warnings.push_back("track '" + tr.name + "' failed (" + why + ") and is missing from the mix");
                    tr.seconds = took;
                }
                trackDone[i] = true;
                std::error_code rec;
                fs::remove(prefix + ".pcm", rec);
                fs::remove(prefix + ".json", rec);
                running.erase(running.begin() + (long)r);
            }
        }
        for (auto &run : running) if (run.pid) { kill(run.pid, SIGKILL); waitpid(run.pid, nullptr, 0); }
        fs::remove_all(tmp, ec);
        if (failed) return false;
    }
    for (auto &tr : trackResults) result.tracks.push_back(std::move(tr));

    // 2. buses (reverbs, delays, groups) run after every bus that feeds them, then return into
    //    their output bus or the mix
    std::vector<size_t> order;
    {
        std::vector<int> state(job.buses.size(), 0);
        std::function<void(size_t)> visit = [&](size_t b) {
            if (state[b]) return;
            state[b] = 1;
            for (size_t s = 0; s < job.buses.size(); ++s) if (job.buses[s].output == job.buses[b].name) visit(s);
            order.push_back(b);
        };
        for (size_t b = 0; b < job.buses.size(); ++b) visit(b);
    }
    std::vector<BusResult> busResults(job.buses.size());
    for (size_t b : order) {
        BusResult &br = busResults[b];
        br.name = job.buses[b].name;
        std::vector<std::string> warnings;
        if (!runChain(busChains[b], buses[b], ctx, br.fx, warnings, "bus '" + br.name + "'", err)) return false;
        for (auto &w : warnings) result.warnings.push_back("bus '" + br.name + "': " + w);
        Audio *dest = &mix;
        for (size_t o = 0; o < job.buses.size(); ++o) if (job.buses[o].name == job.buses[b].output) dest = &buses[o];
        const auto &env = job.buses[b].gainAutomation;
        float g = (float)dsp::dbToLin(job.buses[b].gainDb);
        for (size_t f = 0; f < frames; ++f) {
            if (!env.empty() && f % 32 == 0) g = (float)dsp::dbToLin(job.buses[b].gainDb + env.at(f / sr));
            buses[b].left[f] *= g; buses[b].right[f] *= g;
            dest->left[f] += buses[b].left[f]; dest->right[f] += buses[b].right[f];
        }
        br.levels = measure(buses[b]);
        br.lufs = integratedLufs(buses[b], job.sampleRate);
    }
    for (auto &br : busResults) result.buses.push_back(std::move(br));
    buses.clear();

    // 3. master chain, then optional peak normalisation
    if (job.masterGainDb != 0 || !job.masterGainAutomation.empty()) {
        float g = (float)dsp::dbToLin(job.masterGainDb);
        for (size_t f = 0; f < frames; ++f) {
            if (!job.masterGainAutomation.empty() && f % 32 == 0) g = (float)dsp::dbToLin(job.masterGainDb + job.masterGainAutomation.at(f / sr));
            mix.left[f] *= g; mix.right[f] *= g;
        }
    }
    {
        std::vector<std::string> warnings;
        if (!runChain(masterChain, mix, ctx, result.masterFx, warnings, "master", err)) return false;
        for (auto &w : warnings) result.warnings.push_back("master: " + w);
    }
    const Levels before = measure(mix);
    if (job.hasNormalize && !before.silent) {
        const double gainDb = job.normalizeDb - before.peakDb;
        const float g = (float)dsp::dbToLin(gainDb);
        for (size_t f = 0; f < frames; ++f) { mix.left[f] *= g; mix.right[f] *= g; }
        result.normalizeGainDb = gainDb;
    }
    result.mixFile = (fs::path(outDir) / "mix.wav").string();
    if (!writeWav(result.mixFile, mix, job.sampleRate, err)) return false;
    result.mix = measure(mix);
    result.truePeakDb = truePeakDb(mix);
    result.mixLufs = integratedLufs(mix, job.sampleRate);
    for (size_t m = 0; m < job.markers.size(); ++m) {
        const double a = job.markers[m].sec, b = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
        result.sections.push_back({job.markers[m].name, a, b, integratedLufs(mix, job.sampleRate, (size_t)(a * sr), (size_t)(b * sr))});
    }
    if (result.mix.peakDb > 0.0)
        result.warnings.push_back("mix peaks above 0 dBFS: add a limiter to \"master\", lower track gains, or set \"normalize\"");
    result.renderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace wl
