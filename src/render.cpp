#include "render.hpp"

#include "builtins.hpp"
#include "dsp.hpp"
#include "effects.hpp"
#include "engine.hpp"
#include "loudness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

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
    if (isBuiltin(track.plugin)) {
        tr.plugin = tr.pluginName = track.plugin;
        if (!track.stateFile.empty() || !track.params.empty() || !track.paramAutomation.empty())
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
    OpenedPlugin p;
    if (!openPlugin(setup, track.name, p, err)) return false;
    tr.plugin = p.id;
    tr.pluginName = p.name;
    tr.stateFormat = p.stateFormat;
    tr.paramsApplied = track.params.size();
    tr.automated = p.autos.size();
    const auto events = scheduleNotes(track.notes, job.sampleRate);
    if (!runPlugin(job, p, events, nullptr, audio, err)) { err = track.name + ": " + err; return false; }
    for (auto &w : p.warnings) tr.warnings.push_back(w);
    return true;
}

} // namespace

bool renderJob(const Job &job, const std::string &outDir, bool verbose, RenderResult &result, std::string &err) {
    const auto t0 = std::chrono::steady_clock::now();
    double end = 0;
    for (const auto &t : job.tracks) for (const auto &n : t.notes) end = std::max(end, n.start + n.length);
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

    const FxContext ctx{job, verbose};
    Audio mix;
    mix.resize(frames);
    std::vector<Audio> buses(job.buses.size());
    for (auto &b : buses) b.resize(frames);

    // 1. tracks: instrument → effects → stem; then fader → mix and sends → buses
    for (size_t i = 0; i < job.tracks.size(); ++i) {
        const Track &track = job.tracks[i];
        TrackResult tr;
        tr.name = track.name;
        Audio audio;
        audio.resize(frames);
        if (verbose) std::fprintf(stderr, "rendering %s (%s)...\n", track.name.c_str(), track.plugin.c_str());
        if (!renderInstrument(job, track, audio, tr, verbose, err)) return false;
        if (!runChain(trackChains[i], audio, ctx, tr.fx, tr.warnings, "track '" + track.name + "'", err)) return false;

        char prefix[8];
        std::snprintf(prefix, sizeof prefix, "%02zu-", i + 1);
        tr.file = (fs::path(outDir) / "stems" / (prefix + slug(track.name) + ".wav")).string();
        if (!writeWav(tr.file, audio, job.sampleRate, err)) return false;
        tr.levels = measure(audio);
        tr.lufs = integratedLufs(audio, job.sampleRate);
        if (tr.levels.silent && !track.notes.empty())
            tr.warnings.push_back("rendered silence: check the notes, the state/preset, and that the plugin is an instrument");

        if (!track.mute) {
            const double angle = (track.pan + 1.0) * dsp::kPi / 4.0;
            const double pl = std::cos(angle) * M_SQRT2, pr = std::sin(angle) * M_SQRT2;
            const bool automated = !track.gainAutomation.empty();
            std::vector<std::pair<Audio *, double>> sends;
            for (const auto &[busName, db] : track.sends)
                for (size_t b = 0; b < job.buses.size(); ++b)
                    if (job.buses[b].name == busName) sends.push_back({&buses[b], dsp::dbToLin(db)});
            double g = dsp::dbToLin(track.gainDb);
            for (size_t f = 0; f < frames; ++f) {
                if (automated && f % 32 == 0) g = dsp::dbToLin(track.gainDb + track.gainAutomation.at(f / sr));
                const float l = (float)(audio.left[f] * g * pl), r = (float)(audio.right[f] * g * pr);
                mix.left[f] += l; mix.right[f] += r;
                for (auto &[bus, amt] : sends) { bus->left[f] += (float)(l * amt); bus->right[f] += (float)(r * amt); }
            }
        }
        result.tracks.push_back(std::move(tr));
    }

    // 2. buses (typically 100%-wet reverbs and delays) return into the mix
    for (size_t b = 0; b < job.buses.size(); ++b) {
        BusResult br;
        br.name = job.buses[b].name;
        std::vector<std::string> warnings;
        if (!runChain(busChains[b], buses[b], ctx, br.fx, warnings, "bus '" + br.name + "'", err)) return false;
        for (auto &w : warnings) result.warnings.push_back("bus '" + br.name + "': " + w);
        const float g = (float)dsp::dbToLin(job.buses[b].gainDb);
        for (size_t f = 0; f < frames; ++f) { buses[b].left[f] *= g; buses[b].right[f] *= g; mix.left[f] += buses[b].left[f]; mix.right[f] += buses[b].right[f]; }
        br.levels = measure(buses[b]);
        br.lufs = integratedLufs(buses[b], job.sampleRate);
        result.buses.push_back(std::move(br));
    }
    buses.clear();

    // 3. master chain, then optional peak normalisation
    if (job.masterGainDb != 0) {
        const float g = (float)dsp::dbToLin(job.masterGainDb);
        for (size_t f = 0; f < frames; ++f) { mix.left[f] *= g; mix.right[f] *= g; }
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
