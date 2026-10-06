#include "render.hpp"
#include "track_cache.hpp"

#include "builtins.hpp"
#include "picture.hpp"
#include "song.hpp"
#include "clips.hpp"
#include "dsp.hpp"
#include "effects.hpp"
#include "engine.hpp"
#include "loudness.hpp"
#include "catalog.hpp"
#include "platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>
#include <map>
#include <thread>
#include <fstream>

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

// Mean square per 50 ms of a stereo signal: where something sounds.
std::vector<float> levelTimeline(const Audio &a, int sampleRate) {
    const size_t hop = std::max<size_t>(1, (size_t)(0.05 * sampleRate));
    std::vector<float> out((a.frames() + hop - 1) / hop, 0.f);
    for (size_t i = 0; i < a.frames(); ++i) out[i / hop] += a.left[i] * a.left[i] + a.right[i] * a.right[i];
    for (auto &v : out) v /= (float)(2 * hop);
    return out;
}

// What each automated setting did: the beats where it leaves its resting value (the value it holds longest),
// its range, and warnings for curves nobody hears: every point outside the parameter's range, or values that
// differ from the resting one only where nothing sounds through the effect (in or out: a wet tail counts).
void reportCurves(const Job &job, const std::string &where, const std::vector<Effect::Curve> &curves, const std::vector<float> &in,
                  const std::vector<float> &outLv, size_t frames, nlohmann::json &into, std::vector<std::string> &warnings) {
    const double sr = job.sampleRate, seconds = (double)frames / sr, step = 0.01;
    const double audible = 1e-7;   // -70 dBFS
    auto sounds = [&](double t) {
        const size_t k = (size_t)(t / 0.05);
        return (k < in.size() && in[k] > audible) || (k < outLv.size() && outLv[k] > audible);
    };
    auto fmt = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.4g", v); return std::string(b); };
    for (const auto &c : curves) {
        const bool clamps = c.lo > -1e299;
        auto val = [&](double t) { const double v = c.env.at(t); return clamps ? std::clamp(v, c.lo, c.hi) : v; };
        if (clamps) {   // written values that differ but all clamp to one value: the curve holds still
            std::set<double> written, held;
            for (auto &[t, v] : c.env.corners()) { written.insert(v); held.insert(std::clamp(v, c.lo, c.hi)); }
            if (written.size() > 1 && held.size() == 1)
                warnings.push_back(where + ": every point of the '" + c.param + "' curve lies outside its range [" + fmt(c.lo) + " .. " + fmt(c.hi) +
                                   "], so it holds at " + fmt(*held.begin()) + " (use the plugin's own values, display text, or \"scale\": \"normalized\")");
        }
        std::vector<double> v;
        for (double t = 0; t < seconds; t += step) v.push_back(val(t));
        if (v.empty()) continue;
        const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
        const double lo = *mn, hi = *mx, eps = std::max(1e-9, 1e-4 * (hi - lo));
        if (hi - lo <= 1e-9 * std::max(1.0, std::fabs(hi))) continue;   // a steady value: nothing to report
        std::map<long long, std::pair<size_t, double>> held;   // the value held longest (quantized to eps): count, a value
        for (double x : v) { auto &h = held[std::llround(x / eps)]; if (!h.first++) h.second = x; }
        double rest = v.front();
        size_t best = 0;
        for (auto &[k, h] : held) if (h.first > best) { best = h.first; rest = h.second; }
        nlohmann::json ranges = nlohmann::json::array();
        double a0 = -1, a1 = -1;
        bool heard = false;
        size_t count = 0;
        auto close = [&] {
            if (a0 < 0) return;
            ++count;
            if (ranges.size() < 24) ranges.push_back({std::round(job.tempo.secToBeat(a0) * 100) / 100, std::round(job.tempo.secToBeat(a1) * 100) / 100});
            a0 = -1;
        };
        const double beat = 60.0 / job.tempo.bpmAtBeat(0);
        for (size_t i = 0; i < v.size(); ++i) {
            const double t = i * step;
            if (std::fabs(v[i] - rest) <= eps * 1.5) continue;
            if (sounds(t)) heard = true;
            if (a0 >= 0 && t - a1 > beat) close();   // gaps under a beat merge
            if (a0 < 0) a0 = t;
            a1 = t + step;
        }
        close();
        nlohmann::json e = {{"where", where}, {"param", c.param}, {"min", std::stod(fmt(lo))}, {"max", std::stod(fmt(hi))}, {"rest", std::stod(fmt(rest))},
                            {"activeBeats", ranges}};
        if (count > ranges.size()) e["moreRanges"] = count - ranges.size();
        if (!heard && count) {
            e["heard"] = false;
            warnings.push_back(where + ": the '" + c.param + "' curve leaves its resting value (" + fmt(rest) + ") only where nothing sounds through it"
                               " (from beat " + ranges[0][0].dump() + "): that automation is never heard");
        }
        into.push_back(e);
    }
}

// `firstSoundBeat`: when the chain's input first sounds (a track's first note); a curve that starts later
// but only after that sound holds an inaudible value, so its late-start warning is dropped
bool buildChain(const nlohmann::json &list, const Job &job, const std::string &context, Chain &chain, std::string &err,
                double firstSoundBeat = -1) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].is_object() && list[i].value("bypass", false)) continue;
        auto fx = makeEffect(list[i], job, context + " fx[" + std::to_string(i) + "]", err);
        if (!fx) return false;
        fx->index = (int)i;
        for (auto &[beat, w] : fx->lateCurves) if (beat > firstSoundBeat + 1e-6) fx->warnings.push_back(w);
        fx->lateCurves.clear();
        chain.push_back(std::move(fx));
    }
    return true;
}

bool runChain(Chain &chain, Audio &a, const FxContext &ctx, std::vector<std::string> &labels, std::vector<std::string> &warnings,
              const std::string &context, std::string &err, nlohmann::json *automation = nullptr) {
    for (auto &fx : chain) {
        std::vector<float> in;
        if (fx->automated && automation) in = levelTimeline(a, ctx.job.sampleRate);
        if (!fx->process(a, ctx, err)) { err = context + ": " + err; return false; }
        muteGarbage(a, ctx.job.sampleRate, context + " " + fx->label, warnings);
        labels.push_back(fx->label);
        for (auto &w : fx->warnings) warnings.push_back(w);
        if (fx->automated && automation)
            reportCurves(ctx.job, "fx[" + std::to_string(fx->index) + "] " + fx->label, fx->curves, in, levelTimeline(a, ctx.job.sampleRate), a.frames(),
                         *automation, warnings);
    }
    return true;
}

// Instrument stage: a CLAP plugin or a built-in synth.
bool renderInstrument(const Job &job, const Track &track, Audio &audio, TrackResult &tr, bool verbose, std::string &err,
                      const std::map<size_t, Audio> *rendered = nullptr) {
    tr.notes = track.notes.size();
    for (auto &w : track.warnings) tr.warnings.push_back(w);
    if (isBuiltin(track.plugin)) {
        tr.plugin = tr.pluginName = track.plugin;
        if (track.plugin == "builtin:synth") {   // presets, params and their automation are the synth's own
            if (!track.stateFile.empty()) tr.warnings.push_back("builtin:synth ignores \"state\"; set the patch with \"preset\" and \"synth\"");
        } else if (!track.stateFile.empty() || !track.preset.empty() || !track.params.empty() || !track.paramAutomation.empty())
            tr.warnings.push_back("built-in instruments ignore state, params and parameter automation");
        if (!renderBuiltin(track.plugin, job, track, audio, tr.warnings, err, rendered)) return false;
        muteGarbage(audio, job.sampleRate, track.plugin, tr.warnings);
        return true;
    }
    PluginSetup setup;
    setup.spec = track.plugin;
    setup.stateFile = track.stateFile;
    setup.stateFormat = track.stateFormat;
    setup.params = track.params;
    setup.automation = track.paramAutomation;
    setup.verbose = verbose;
    setup.warmup = track.warmup;
    setup.realtime = track.realtime;
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
    if (!p.autos.empty()) {   // instrument parameter curves: heard where the instrument sounds
        std::vector<Effect::Curve> curves;
        for (const auto &au : p.autos) {
            ParamInfo pi;
            Effect::Curve cv{au.name, au.env};
            if (p.plugin->findParam("#" + std::to_string(au.id), pi)) { cv.lo = std::min(pi.min, pi.max); cv.hi = std::max(pi.min, pi.max); }
            curves.push_back(cv);
        }
        reportCurves(job, "instrument " + p.name, curves, {}, levelTimeline(audio, job.sampleRate), audio.frames(), tr.automation, tr.warnings);
    }
    return true;
}


// Instrument + effect chain for track `i` (what a worker process renders).
bool renderTrackAudio(const Job &job, size_t i, Chain &chain, const FxContext &ctx, Audio &audio, TrackResult &tr, bool verbose,
                      std::string &err, const std::map<size_t, Audio> *rendered = nullptr) {
    const Track &track = job.tracks[i];
    if (!renderInstrument(job, track, audio, tr, verbose, err, rendered)) return false;
    if (!runChain(chain, audio, ctx, tr.fx, tr.warnings, "track '" + track.name + "'", err, &tr.automation)) return false;
    for (auto &fx : chain) tr.latencySamples += fx->latencySamples;
    return true;
}

nlohmann::json trackToJson(const TrackResult &t) {
    return {{"ok", true}, {"plugin", t.plugin}, {"pluginName", t.pluginName}, {"stateFormat", t.stateFormat}, {"preset", t.preset},
            {"notes", t.notes}, {"paramsApplied", t.paramsApplied}, {"automated", t.automated}, {"fx", t.fx},
            {"warnings", t.warnings}, {"latencySamples", t.latencySamples}, {"automation", t.automation}};
}

void trackFromJson(const nlohmann::json &j, TrackResult &t) {
    t.plugin = j.value("plugin", ""); t.pluginName = j.value("pluginName", ""); t.stateFormat = j.value("stateFormat", "");
    t.preset = j.value("preset", ""); t.notes = j.value("notes", (size_t)0); t.paramsApplied = j.value("paramsApplied", (size_t)0);
    t.automated = j.value("automated", (size_t)0); t.fx = j.value("fx", std::vector<std::string>());
    t.warnings = j.value("warnings", std::vector<std::string>()); t.latencySamples = j.value("latencySamples", 0u);
    t.automation = j.value("automation", nlohmann::json::array());
}

} // namespace

// the share of a track's post-fader energy below 120 Hz (0..1): the mix checks' mid and side low ends against the
// left/right sums (mid and side are half-sums, so their energies add up to half of left plus right)
double lowShare(const TrackResult &t) {
    const double all = t.sumLL + t.sumRR;
    if (all <= 0) return 0;
    double low = 0;
    for (size_t k = 0; k < t.lowMid.size(); ++k) low += t.lowMid[k] + (k < t.lowSide.size() ? t.lowSide[k] : 0);
    return std::min(1.0, 2 * low / all);
}

int renderTrackWorker(const std::string &jobPath, size_t index, const std::string &prefix, const std::vector<std::string> &sidechains) {
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
    if (!buildChain(job.tracks[index].fx, job, "track '" + job.tracks[index].name + "'", chain, err, job.tracks[index].firstSoundBeat)) return fail(err);
    std::map<std::string, Audio> sc;   // "<track index>=<raw audio file>" from the parent
    for (auto &arg : sidechains) {
        const size_t eq = arg.find('=');
        const size_t k = (size_t)std::stoul(arg.substr(0, eq));
        if (k >= job.tracks.size()) continue;
        Audio &a = sc[job.tracks[k].name];
        a.resize(frames);
        std::ifstream f(arg.substr(eq + 1), std::ios::binary);
        f.read(reinterpret_cast<char *>(a.left.data()), (std::streamsize)(frames * sizeof(float)));
        f.read(reinterpret_cast<char *>(a.right.data()), (std::streamsize)(frames * sizeof(float)));
    }
    const FxContext ctx{job, false, [&](const std::string &name) -> const Audio * {
        auto it = sc.find(name);
        return it == sc.end() ? nullptr : &it->second;
    }};
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


namespace {

// render --loop: the render with what rings past the loop's end (reverbs, releases) added back onto its
// start, exactly one loop long, so it plays seamlessly on repeat
Audio foldLoop(const Audio &a, size_t loopFrames) {
    Audio out;
    out.resize(loopFrames);
    if (loopFrames == 0) return out;
    for (size_t i = 0; i < a.frames(); ++i) { out.left[i % loopFrames] += a.left[i]; out.right[i % loopFrames] += a.right[i]; }
    return out;
}

// "bars 17-48" / "bar 17" of a stretch of render seconds
std::string barsOf(const Job &job, double s0, double s1) {
    const int b0 = job.meter.barIndex(job.tempo.secToBeat(s0)) + 1, b1 = job.meter.barIndex(job.tempo.secToBeat(std::max(s0, s1 - 1e-6))) + 1;
    return b0 == b1 ? "bar " + std::to_string(b0) : "bars " + std::to_string(b0) + "-" + std::to_string(b1);
}

// a track that plays drums (a kick among them): kits, the built-in drums, names like Kick / Drums / BD
bool drumTrack(const Track &t) {
    std::string n = t.name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    if (t.plugin == "builtin:drums") return true;
    if (t.plugin == "builtin:sampler" && t.sampler.is_object() && t.sampler.contains("kit")) return true;
    for (const char *w : {"kick", "drum", "bd", "909", "808", "707", "kit", "beat"})
        if (n.find(w) != std::string::npos) return true;
    return false;
}

// a part that plays the bass line: named like one (bass, sub, reese) or written mostly below C3; not drums,
// effects or unpitched material
bool bassTrack(const Track &t) {
    if (t.notes.empty() || !t.harmony || drumTrack(t) || t.plugin == "builtin:fx" || t.plugin == "builtin:shepard") return false;
    std::string n = t.name;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    for (const char *w : {"bass", "sub", "reese"})
        if (n.find(w) != std::string::npos) return true;
    std::vector<int> keys;
    for (const auto &x : t.notes) keys.push_back(x.key);
    std::nth_element(keys.begin(), keys.begin() + keys.size() / 2, keys.end());
    return keys[keys.size() / 2] < 48;
}

// Mix checks on what each track sends to the mix (after its effects and fader): a kick buried under
// another part below 120 Hz, a low end spread wide in stereo, and left/right out of phase.
void mixChecks(const Job &job, const std::vector<TrackResult> &tracks, double seconds, std::vector<std::string> &warnings) {
    const double hop = 0.05;
    double mixLow = 0, mixAll = 0;
    for (size_t i = 0; i < tracks.size() && i < job.tracks.size(); ++i) {
        if (job.tracks[i].mute) continue;
        for (size_t k = 0; k < tracks[i].lowMid.size(); ++k) mixLow += tracks[i].lowMid[k] + tracks[i].lowSide[k];
        mixAll += tracks[i].sumLL + tracks[i].sumRR;
    }
    if (mixLow <= 0 || mixAll <= 0) return;
    auto sectionsOf = [&](const std::vector<size_t> &frames) {   // the marker sections those 50 ms frames fall in
        std::string out;
        std::vector<bool> seen(job.markers.size(), false);
        for (size_t k : frames)
            for (size_t m = 0; m < job.markers.size(); ++m) {
                const double a = job.markers[m].sec, b = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
                if (k * hop >= a && k * hop < b && !seen[m]) { seen[m] = true; out += (out.empty() ? "" : ", ") + job.markers[m].name; }
            }
        return out;
    };
    for (size_t i = 0; i < tracks.size() && i < job.tracks.size(); ++i) {
        const Track &t = job.tracks[i];
        const TrackResult &r = tracks[i];
        if (t.mute || r.lowMid.empty()) continue;
        double mid = 0, side = 0;
        for (size_t k = 0; k < r.lowMid.size(); ++k) { mid += r.lowMid[k]; side += r.lowSide[k]; }
        // a wide low end: a song with a real low end, the track carries a real share of it (and it is a real part of the
        // track, not a pad's leak through the filter), and much of it is side signal
        const double own = r.sumLL + r.sumRR;
        if (mixLow >= 0.02 * mixAll && (mid + side) >= 0.1 * mixLow && (mid + side) >= 0.05 * own && side >= 0.25 * mid) {
            const double db = 10 * std::log10(mid / std::max(side, 1e-30));
            char buf[460], rel[48];
            if (db >= 0) std::snprintf(rel, sizeof rel, "only %.1f dB under its mid", db);
            else std::snprintf(rel, sizeof rel, "%.1f dB over its mid", -db);
            std::snprintf(buf, sizeof buf, "track '%s' is wide below 120 Hz (its side signal is %s there, and it carries %.0f%% of the song's low end): "
                          "a wide low end smears and thins out in mono (clubs, phones, one speaker); keep it centred with "
                          "{\"type\": \"width\", \"monoBelow\": 120} at the end of its fx", t.name.c_str(), rel, 100 * (mid + side) / mixLow);
            warnings.push_back(buf);
        }
        // out of phase: left and right cancel when summed to mono
        const double corr = r.sumLR / std::sqrt(std::max(r.sumLL * r.sumRR, 1e-30));
        if ((r.sumLL + r.sumRR) >= 0.03 * mixAll && corr < -0.1) {
            char buf[360];
            std::snprintf(buf, sizeof buf, "track '%s' is out of phase (left/right correlation %.2f): it cancels when the song plays in mono; look for a "
                          "width amount above 1, a phase-inverting stereo effect or a sample whose channels are opposed", t.name.c_str(), corr);
            warnings.push_back(buf);
        }
    }
    // the kick buried: at a drum track's loudest low-end moments (its kick hits), another part is nearly as
    // loud below 120 Hz. Measured after effects, so a part that already ducks from the kick passes.
    for (size_t i = 0; i < tracks.size() && i < job.tracks.size(); ++i) {
        const Track &kt = job.tracks[i];
        const TrackResult &k = tracks[i];
        if (kt.mute || !drumTrack(kt) || k.lowMid.empty()) continue;
        std::vector<float> sounding;
        for (float e : k.lowMid) if (e > 1e-9) sounding.push_back(e);
        if (sounding.size() < 20) continue;
        std::sort(sounding.begin(), sounding.end());
        const float p50 = sounding[sounding.size() / 2], p90 = sounding[sounding.size() * 9 / 10];
        if (p90 < 4 * p50) continue;   // no punches in its low end (6 dB over its median): not a kick
        std::vector<size_t> hits;
        for (size_t f = 0; f < k.lowMid.size(); ++f) if (k.lowMid[f] >= p90) hits.push_back(f);
        double kSum = 0;
        for (size_t f : hits) kSum += k.lowMid[f];
        if (kSum < 0.03 * mixLow) continue;   // a kick too quiet to matter
        for (size_t j = 0; j < tracks.size() && j < job.tracks.size(); ++j) {
            const Track &bt = job.tracks[j];
            const TrackResult &b = tracks[j];
            if (j == i || bt.mute || drumTrack(bt) || b.lowMid.empty()) continue;
            double bSum = 0;
            std::vector<size_t> buried;
            for (size_t f : hits)
                if (f < b.lowMid.size()) {
                    bSum += b.lowMid[f];
                    if (b.lowMid[f] > 0.5 * k.lowMid[f]) buried.push_back(f);
                }
            const double margin = 10 * std::log10(kSum / std::max(bSum, 1e-30));
            if (margin >= 3 || buried.size() < hits.size() / 3) continue;
            const std::string where = job.markers.empty() ? barsOf(job, buried.front() * hop, (buried.back() + 1) * hop) : sectionsOf(buried);
            const bool kit = kt.plugin == "builtin:drums" || (kt.plugin == "builtin:sampler" && kt.sampler.is_object() && kt.sampler.contains("kit"));
            char buf[640], rel[48];
            if (margin >= 0) std::snprintf(rel, sizeof rel, "only %.1f dB under it", margin);
            else std::snprintf(rel, sizeof rel, "%.1f dB louder than it", -margin);
            std::snprintf(buf, sizeof buf, "the kick in '%s' has no room below 120 Hz: at its hits '%s' is %s there%s%s%s; duck it from the "
                          "kick ({\"type\": \"duck\", \"trigger\": \"%s\"%s} on '%s'), or give each its own range (cut '%s' below 50-60 Hz, or the kick's boom)",
                          kt.name.c_str(), bt.name.c_str(), rel, where.empty() ? "" : " (", where.c_str(), where.empty() ? "" : ")", kt.name.c_str(),
                          kit ? ", \"keys\": [35, 36]" : "", bt.name.c_str(), bt.name.c_str());
            warnings.push_back(buf);
        }
    }
    // a thin bass: the bass parts together (a mid-heavy bass with a sub under it passes) put little of their energy
    // below 120 Hz. Faders set by loudness can't see it: LUFS counts the mids, so such a bass reads as loud as a full
    // one and sounds weak. Measured on 16 trance basses: full ones 44-77%, one a listener heard as weak 3%.
    double bassLow = 0, bassAll = 0;
    std::vector<std::pair<double, std::string>> parts;
    for (size_t i = 0; i < tracks.size() && i < job.tracks.size(); ++i) {
        if (job.tracks[i].mute || !bassTrack(job.tracks[i])) continue;
        const double all = tracks[i].sumLL + tracks[i].sumRR;
        if (all <= 0) continue;
        const double share = lowShare(tracks[i]);
        bassLow += share * all; bassAll += all;
        parts.push_back({share, job.tracks[i].name});
    }
    if (!parts.empty() && bassAll >= 0.05 * mixAll && bassLow < 0.15 * bassAll) {
        std::sort(parts.begin(), parts.end());
        std::string names;
        for (size_t p = 0; p < parts.size(); ++p) names += (p ? (p + 1 == parts.size() ? "' and '" : "', '") : "'") + parts[p].second;
        names += "'";
        char buf[720];
        std::snprintf(buf, sizeof buf, "the bass is thin: %s %s only %.0f%% of %s energy below 120 Hz (full basses carry 40-75%%). "
                      "Loudness counts the mids, so a thin bass reads as loud as a full one and sounds weak. Layer a sub on the same notes "
                      "({\"plugin\": \"builtin:synth\", \"preset\": \"BA Sub\"}, low-passed near 160 Hz and ducked like the bass, in E1-D#2 when the "
                      "bass sits higher) or pick a bass with more low end; a low shelf only lifts what the patch already has",
                      names.c_str(), parts.size() == 1 ? "carries" : "carry", 100 * bassLow / bassAll, parts.size() == 1 ? "its" : "their");
        warnings.push_back(buf);
    }
}

// The same bars over and over: a stretch of 32 bars or more where every track plays the same 1-, 2-, 4- or
// 8-bar block again and nothing moves (no automation turns inside it). Listeners stop hearing it.
void repetitionChecks(const Job &job, std::vector<std::string> &warnings) {
    const MeterMap &M = job.meter;
    double lastBeat = 0;
    for (const auto &t : job.tracks)
        for (const auto &n : t.notes) lastBeat = std::max(lastBeat, job.tempo.secToBeat(n.start + n.length));
    const int bars = lastBeat > 1e-9 ? M.barIndex(lastBeat - 1e-9) + 1 : 0;
    const double endBeat = M.barToBeat(bars + 1);
    if (bars < 32) return;
    // each bar's content: every note that starts in it (track, key, position and length to 1/24 beat)
    std::vector<std::vector<long>> sig((size_t)bars);
    for (size_t ti = 0; ti < job.tracks.size(); ++ti) {
        if (job.tracks[ti].mute) continue;
        for (const auto &n : job.tracks[ti].notes) {
            const double b = job.tempo.secToBeat(n.start), e = job.tempo.secToBeat(n.start + n.length);
            const int bar = M.barIndex(b);
            if (bar < 0 || bar >= bars) continue;
            sig[(size_t)bar].push_back((((long)ti * 128 + n.key) * 4096 + std::lround((b - M.barToBeat(bar + 1)) * 24)) * 8192 + std::lround((e - b) * 24));
        }
    }
    for (auto &s : sig) std::sort(s.begin(), s.end());
    // bars where something is moving: any automation corner (track, bus, master) makes the stretch vary
    std::vector<bool> moving((size_t)bars + 1, false);
    auto markBeats = [&](double beat0, double beat1) {
        const int b0 = M.barIndex(beat0), b1 = M.barIndex(beat1);
        for (int b = std::max(0, b0); b <= std::min(bars, b1); ++b) moving[(size_t)b] = true;
    };
    auto mark = [&](const Envelope &e) {
        if (e.hasLfo()) { markBeats(0, endBeat); return; }   // an LFO moves all the time
        const auto &pts = e.corners();
        for (size_t k = 1; k < pts.size(); ++k)
            if (pts[k].second != pts[k - 1].second) markBeats(job.tempo.secToBeat(pts[k - 1].first), job.tempo.secToBeat(pts[k].first));
    };
    // effect settings that move: "automate" curves ([[beat, value], ...] or {"points": ..., "lfo": ...}) and
    // an effect's "lfo" block; curves written straight on a setting count too
    std::function<void(const nlohmann::json &)> markCurve = [&](const nlohmann::json &c) {
        if (c.is_object()) {
            if (c.contains("lfo")) markBeats(0, endBeat);
            if (c.contains("points")) markCurve(c["points"]);
            return;
        }
        if (!c.is_array()) return;
        for (size_t i = 1; i < c.size(); ++i) {
            const auto &a = c[i - 1], &b = c[i];
            const bool pa = a.is_array() && a.size() >= 2 && a[0].is_number(), pb = b.is_array() && b.size() >= 2 && b[0].is_number();
            if (pa && pb && a[1] != b[1]) markBeats(a[0].get<double>(), b[0].get<double>());
        }
    };
    std::function<void(const nlohmann::json &)> markFx = [&](const nlohmann::json &list) {
        for (const auto &fx : list) {
            if (!fx.is_object()) continue;
            for (auto &[k, v] : fx.items()) {
                if (k == "loopFx" || k == "bands") { if (v.is_array()) markFx(v); continue; }
                if (k == "automate" && v.is_object()) { for (auto &[pk, pv] : v.items()) markCurve(pv); continue; }
                if (k == "lfo" && v.is_object() && !v.empty()) { markBeats(0, endBeat); continue; }
                markCurve(v);
            }
        }
    };
    for (const auto &t : job.tracks) {
        for (const auto &c : t.gainAutomation.parts) mark(c);
        mark(t.panAutomation);
        for (const auto &[n, e] : t.paramAutomation) mark(e);
        for (const auto &[n, e] : t.ccAutomation) mark(e);
        for (const auto &[n, e] : t.sendAutomation) mark(e);
        mark(t.bendAutomation);
        markFx(t.fx);
    }
    for (const auto &b : job.buses) { for (const auto &c : b.gainAutomation.parts) mark(c); markFx(b.fx); }
    for (const auto &c : job.masterGainAutomation.parts) mark(c);
    markFx(job.masterFx);
    auto quiet = [&](int b) {   // inside a section marked "checks": false
        const double s = job.tempo.beatToSec(M.barToBeat(b + 1));
        for (size_t m = 0; m < job.markers.size(); ++m) {
            const double a = job.markers[m].sec, e = m + 1 < job.markers.size() ? job.markers[m + 1].sec : 1e18;
            if (s >= a && s < e) return !job.markers[m].checks;
        }
        return false;
    };
    std::vector<bool> reported((size_t)bars, false);
    for (int p : {1, 2, 4, 8}) {
        for (int b = p; b < bars;) {
            if (sig[(size_t)b].empty() || sig[(size_t)b] != sig[(size_t)(b - p)] || moving[(size_t)b] || quiet(b)) { ++b; continue; }
            int e = b;
            while (e < bars && !sig[(size_t)e].empty() && sig[(size_t)e] == sig[(size_t)(e - p)] && !moving[(size_t)e] && !quiet(e)) ++e;
            const int from = b - p, len = e - from;   // bars [from, e): the block and its repeats
            if (len >= 32 && !reported[(size_t)from]) {
                for (int x = from; x < e; ++x) reported[(size_t)x] = true;
                char buf[420];
                std::snprintf(buf, sizeof buf, "bars %d-%d repeat the same %d bar%s %d times with nothing added, taken away or moving: listeners stop hearing "
                              "it. Change something every 8 bars or so (a part in or out, a fill, a filter or level ride, a variation of the lead), or "
                              "shorten it (mark the section \"checks\": false if it is meant this way)",
                              from + 1, e, p, p == 1 ? "" : "s", len / p);
                warnings.push_back(buf);
            }
            b = e + 1;
        }
    }
}

} // namespace

namespace {
RenderProgress gProgress;
}

void setRenderProgress(RenderProgress progress) { gProgress = std::move(progress); }

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
    const size_t leadFrames = (size_t)std::llround(job.leadIn * job.sampleRate);
    const size_t trimFrames = job.window.on ? (size_t)std::llround(job.window.trimSec * job.sampleRate) : 0;   // render --from pre-roll
    const size_t loopFrames = job.window.loop ? (size_t)std::llround(job.window.loopSec * job.sampleRate) : 0;   // render --loop
    result.leadIn = job.leadIn;

    // 0. build every effect chain first, so a typo fails in milliseconds, not after a long render
    std::vector<Chain> trackChains(job.tracks.size()), busChains(job.buses.size());
    Chain masterChain;
    for (size_t i = 0; i < job.tracks.size(); ++i)
        if (!buildChain(job.tracks[i].fx, job, "track '" + job.tracks[i].name + "'", trackChains[i], err, job.tracks[i].firstSoundBeat)) return false;
    for (size_t i = 0; i < job.buses.size(); ++i)
        if (!buildChain(job.buses[i].fx, job, "bus '" + job.buses[i].name + "'", busChains[i], err, job.buses[i].firstSoundBeat)) return false;
    if (!buildChain(job.masterFx, job, "master", masterChain, err, job.masterFirstSoundBeat)) return false;
    for (const auto &b : job.buses)
        for (const auto &w : b.warnings) result.warnings.push_back("bus '" + b.name + "': " + w);
    for (const auto &w : job.masterWarnings) result.warnings.push_back("master: " + w);

    std::error_code ec;
    fs::create_directories(job.stemBits ? fs::path(outDir) / "stems" : fs::path(outDir), ec);   // no empty stems/ when stems are off
    if (ec) { err = "cannot create " + outDir + ": " + ec.message(); return false; }
    // never leave a previous render's files next to this one's: a failed render must not look finished
    fs::remove(fs::path(outDir) / "report.json", ec);
    fs::remove(fs::path(outDir) / "mix.wav", ec);
    for (auto &d : job.deliver) fs::remove(deliveryPath(d, outDir), ec);
    for (auto &e : fs::directory_iterator(fs::path(outDir) / "stems", ec))
        if (e.path().extension() == ".wav") fs::remove(e.path(), ec);
    {
        const double perFile = (double)frames * 2 * 4 + 64;
        const size_t stems = (size_t)std::count_if(job.tracks.begin(), job.tracks.end(), [](const Track &t) { return t.stem; }) +
                             (size_t)std::count_if(job.buses.begin(), job.buses.end(), [](const Bus &b) { return b.stem; });
        const double need = perFile + (job.stemBits ? stems * (double)frames * 2 * (job.stemBits / 8) : 0);
        const auto space = fs::space(outDir, ec);
        if (!ec && (double)space.available < need * 1.05) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "not enough disk space in %s: this render writes %.0f MB, %.0f MB free (set \"stems\": \"none\" or \"16\" to write less)",
                          outDir.c_str(), need / 1e6, space.available / 1e6);
            err = buf;
            return false;
        }
    }

    // sidechain sources: tracks whose audio keys an effect somewhere ("sidechain": "<track>")
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < job.tracks.size(); ++i) byName[job.tracks[i].name] = i;
    std::map<size_t, Audio> scAudio;
    std::vector<std::set<size_t>> deps(job.tracks.size());
    std::set<size_t> sources;
    auto keysOf = [&](const nlohmann::json &fxList, const std::string &where, std::set<size_t> &into) {
        for (auto &fx : fxList)
            if (fx.is_object() && fx.contains("sidechain") && fx["sidechain"].is_string()) {
                const std::string k = fx["sidechain"].get<std::string>();
                auto it = byName.find(k);
                if (it == byName.end()) { err = where + ": sidechain track '" + k + "' does not exist"; return false; }
                into.insert(it->second);
                sources.insert(it->second);
            }
        return true;
    };
    for (size_t i = 0; i < job.tracks.size(); ++i)
        if (!keysOf(job.tracks[i].fx, "track '" + job.tracks[i].name + "'", deps[i])) return false;
    {
        std::set<size_t> ignore;
        for (auto &b : job.buses) if (!keysOf(b.fx, "bus '" + b.name + "'", ignore)) return false;
        if (!keysOf(job.masterFx, "master", ignore)) return false;
    }
    // builtin:audio "render" clips: the song's own tracks over a beat range, captured after their faders
    // while they mix (before any bus). A track that renders others this way is never a source itself,
    // so it can't recurse; its sources render first (like sidechain sources).
    struct Capture { size_t track, clip, f0, f1; std::set<size_t> from; Audio audio; };
    std::vector<Capture> captures;
    {
        std::vector<std::vector<ClipRender>> reqs(job.tracks.size());
        std::set<size_t> renders;
        for (size_t i = 0; i < job.tracks.size(); ++i)
            if (job.tracks[i].plugin == "builtin:audio") {
                if (!clipRenders(job.tracks[i], reqs[i], err)) return false;
                if (!reqs[i].empty()) renders.insert(i);
            }
        for (size_t i = 0; i < job.tracks.size(); ++i)
            for (const auto &r : reqs[i]) {
                Capture c;
                c.track = i;
                c.clip = r.clip;
                c.f0 = std::min(frames, (size_t)std::llround(job.tempo.beatToSec(r.fromBeat) * sr));
                c.f1 = std::min(frames, (size_t)std::llround(job.tempo.beatToSec(r.toBeat) * sr));
                const std::string where = "track '" + job.tracks[i].name + "' clip " + std::to_string(r.clip + 1);
                if (r.tracks.empty()) {
                    for (size_t j = 0; j < job.tracks.size(); ++j) if (!renders.count(j)) c.from.insert(j);
                } else
                    for (const auto &name : r.tracks) {
                        auto it = byName.find(name);
                        if (it == byName.end()) { err = where + ": no track named '" + name + "' to render"; return false; }
                        if (renders.count(it->second)) {
                            err = where + ": track '" + name + "' plays rendered audio itself, so it can't be rendered into another clip";
                            return false;
                        }
                        c.from.insert(it->second);
                    }
                for (size_t j : c.from) deps[i].insert(j);
                c.audio.resize(c.f1 - c.f0);
                captures.push_back(std::move(c));
            }
    }
    {
        // no track may (indirectly) key itself
        std::vector<int> mark(job.tracks.size(), 0);
        std::function<bool(size_t)> acyclic = [&](size_t i) {
            if (mark[i] == 1) return false;
            if (mark[i] == 2) return true;
            mark[i] = 1;
            for (size_t d : deps[i]) if (!acyclic(d)) return false;
            mark[i] = 2;
            return true;
        };
        for (size_t i = 0; i < job.tracks.size(); ++i)
            if (!acyclic(i)) { err = "track '" + job.tracks[i].name + "': sidechain (or rendered-clip) sources form a loop"; return false; }
    }
    // render --cache: a key per track for what shapes its audio (a track that renders others into a clip isn't cached)
    std::vector<std::string> cacheKeys;
    if (job.trackCache) {
        std::set<size_t> uncached;
        for (auto &c : captures) uncached.insert(c.track);
        cacheKeys = trackcache::keys(job, deps, uncached, frames);
    }
    FxContext ctx{job, verbose, [&](const std::string &name) -> const Audio * {
        auto it = byName.find(name);
        if (it == byName.end()) return nullptr;
        auto a = scAudio.find(it->second);
        return a == scAudio.end() ? nullptr : &a->second;
    }};
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
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%02d-", track.stemNumber > 0 ? track.stemNumber : (int)i + 1);
        if (job.stemBits && track.stem) {
            tr.file = (fs::path(outDir) / "stems" / (prefix + slug(track.name) + ".wav")).string();
            if (job.window.loop) { if (!writeWav(tr.file, foldLoop(audio, loopFrames), job.sampleRate, err, job.stemBits, 0, 0, true)) return false; }
            else if (!writeWav(tr.file, audio, job.sampleRate, err, job.stemBits, leadFrames, trimFrames)) return false;
        }
        tr.levels = measure(audio);
        tr.lufs = integratedLufs(audio, job.sampleRate);
        if (tr.levels.silent && !track.notes.empty())
            tr.warnings.push_back("rendered silence: check the notes, the state/preset, and that the plugin is an instrument");
        if (tr.lufs > 6) {   // no instrument is this loud: garbage below the mute threshold or runaway feedback
            char msg[200];
            std::snprintf(msg, sizeof msg, "track '%s' reads %+.1f LUFS (peak %+.1f dBFS): broken output or runaway feedback, not a level to gain-stage from",
                          track.name.c_str(), tr.lufs, tr.levels.peakDb);
            tr.warnings.push_back(msg);
            result.warnings.push_back(msg);
        }

        if (!track.mute) {
            // constant power (+3 dB at the near side when hard over), or a balance: the near side stays and the far
            // side falls as (1 - |pan|)^2, GarageBand's and Logic's pan on a stereo track (measured on a bounce)
            auto panGains = [&](double pan, double &l, double &r) {
                pan = std::clamp(pan, -1.0, 1.0);
                if (track.balancePan) { l = pan > 0 ? (1 - pan) * (1 - pan) : 1.0; r = pan < 0 ? (1 + pan) * (1 + pan) : 1.0; return; }
                const double angle = (pan + 1.0) * dsp::kPi / 4.0;
                l = std::cos(angle) * M_SQRT2; r = std::sin(angle) * M_SQRT2;
            };
            double pl, pr;
            panGains(track.pan, pl, pr);
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
            std::vector<Capture *> caps;   // render clips that take this track
            for (auto &c : captures) if (c.from.count(i) && c.f1 > c.f0) caps.push_back(&c);
            const size_t capFade = (size_t)(0.005 * sr);   // 5 ms edges, so the cut doesn't click
            Audio *dest = &mix;   // "output": a group bus instead of the master
            for (size_t b = 0; b < job.buses.size(); ++b) if (job.buses[b].name == track.output) dest = &buses[b];
            double g = dsp::dbToLin(track.gainDb);
            Audio post;   // what this track adds to its output, for per-section loudness
            if (!job.markers.empty() || job.picture) post.resize(frames);
            float postPeak = 0;
            const size_t levelHop = (size_t)std::llround(0.05 * sr);   // the picture's level lane
            LoudnessMeter postMeter(job.sampleRate);   // post-fader loudness on every render, windows included
            if (job.picture) tr.levelTimeline.assign(frames / levelHop + 1, 0.f);
            // mix checks: the low end (below 120 Hz) of mid and side per 50 ms. 8-sample averages first, so the
            // 4th-order low-pass runs at an eighth of the rate (the average's nulls sit on the folding frequencies).
            const size_t dec = 8;
            dsp::Biquad lm1, lm2, ls1, ls2;
            lm1.set(dsp::Biquad::LowPass, 120, 0.7071, 0, sr / dec);
            lm2 = lm1; ls1 = lm1; ls2 = lm1;
            tr.lowMid.assign(frames / levelHop + 1, 0.f);
            tr.lowSide.assign(frames / levelHop + 1, 0.f);
            double accM = 0, accS = 0;
            for (size_t f = 0; f < frames; ++f) {
                if (automated && f % 32 == 0) g = dsp::dbToLin(track.gainDb + track.gainAutomation.at(f / sr));
                if (f % 32 == 0) for (auto &s : sends) if (s.env) s.amt = dsp::dbToLin(s.env->at(f / sr));
                if (panAuto && f % 32 == 0) panGains(track.panAutomation.at(f / sr), pl, pr);
                const float l = (float)(audio.left[f] * g * pl), r = (float)(audio.right[f] * g * pr);
                dest->left[f] += l; dest->right[f] += r;
                postPeak = std::max({postPeak, std::fabs(l), std::fabs(r)});
                if (!post.left.empty()) { post.left[f] = l; post.right[f] = r; }
                postMeter.add(l, r);
                if (!tr.levelTimeline.empty()) tr.levelTimeline[f / levelHop] += l * l + r * r;
                tr.sumLR += (double)l * r; tr.sumLL += (double)l * l; tr.sumRR += (double)r * r;
                accM += l + r; accS += l - r;
                if ((f + 1) % dec == 0) {
                    const double m = lm2.process(lm1.process(accM / (2.0 * dec))), s = ls2.process(ls1.process(accS / (2.0 * dec)));
                    tr.lowMid[f / levelHop] += (float)(m * m * dec);
                    tr.lowSide[f / levelHop] += (float)(s * s * dec);
                    accM = accS = 0;
                }
                for (auto &s : sends) { s.bus->left[f] += (float)(l * s.amt); s.bus->right[f] += (float)(r * s.amt); }
                for (auto *c : caps)
                    if (f >= c->f0 && f < c->f1) {
                        const double w = std::min({1.0, (double)(f - c->f0) / capFade, (double)(c->f1 - f) / capFade});
                        c->audio.left[f - c->f0] += (float)(l * w);
                        c->audio.right[f - c->f0] += (float)(r * w);
                    }
            }
            tr.postPeakDb = dsp::linToDb(postPeak);
            tr.postLufs = postMeter.integrated();
            for (auto &v : tr.levelTimeline) v /= (float)(2 * levelHop);
            for (size_t m = 0; m < job.markers.size(); ++m) {
                const double a0 = job.markers[m].sec, b0 = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
                tr.sectionLufs.push_back(integratedLufs(post, job.sampleRate, (size_t)(a0 * sr), (size_t)(b0 * sr)));
            }
        }
        return true;
    };

    int parallel = job.parallel;
    if (parallel < 0) {   // half the cores, up to 4, and fewer when other work (other renders) already keeps the cores busy
        const int cores = (int)std::max(1u, std::thread::hardware_concurrency());
        const double load = platform::loadAverage();
        const int idle = load < 0 ? cores : (int)std::floor(cores - load);
        parallel = std::clamp(std::min(cores / 2, idle), 1, 4);
        if (verbose) std::fprintf(stderr, "rendering %d plugin tracks at once (%d cores, load %.1f)\n", parallel, cores, load);
    }
    const bool isolate = parallel > 0 && !job.sourcePath.empty();
    const fs::path tmp = fs::temp_directory_path() / ("wavelength-render-" + std::to_string(platform::processId()));
    if (isolate) fs::create_directories(tmp, ec);
    auto progress = [&](const std::string &now) {
        if (!gProgress) return;
        gProgress((size_t)std::count(trackDone.begin(), trackDone.end(), true), job.tracks.size(), now);
    };
    auto finish = [&](size_t i, Audio &audio) {   // mix a finished track; keep it if it keys an effect
        if (sources.count(i)) scAudio[i] = audio;
        if (!mixTrack(i, audio, trackResults[i])) return false;
        trackDone[i] = true;
        return true;
    };
    struct Running { size_t index; platform::Process proc; std::chrono::steady_clock::time_point started; };
    std::vector<Running> running;
    std::vector<bool> started(job.tracks.size(), false);
    std::vector<int> crashes(job.tracks.size(), 0);
    auto lastWindowCheck = std::chrono::steady_clock::now();
    const double limit = 300 + 10 * seconds;   // a track that takes longer than this is hung
    const std::string self = isolate ? platform::selfExecutable() : "";
    std::string workerErr;   // a worker that can't start (an Intel-only plugin on a non-universal build)
    auto startTrack = [&](size_t i) {
        const std::string prefix = (tmp / std::to_string(i)).string();
        // an Intel-only plugin (instrument or effect on this track) runs in a worker under Rosetta
        std::string arch, archErr;
        {
            std::vector<std::string> specs = {job.tracks[i].plugin};
            for (auto &fxj : job.tracks[i].fx) if (fxj.is_object() && fxj.contains("plugin") && fxj["plugin"].is_string()) specs.push_back(fxj["plugin"]);
            for (auto &sp : specs) {
                PluginInfo pi;
                std::string e;
                if (!isBuiltin(sp) && resolvePlugin(sp, pi, e) && !pi.arch.empty() && pi.arch != platform::hostArch()) arch = pi.arch;
            }
        }
        std::vector<std::string> args;
        if (!platform::archPrefix(arch, args, archErr)) { workerErr = "track '" + job.tracks[i].name + "': " + archErr; return; }
        for (const std::string &x : {self, std::string("__track"), job.sourcePath, std::to_string(i), prefix}) args.push_back(x);
        for (size_t d : deps[i]) {   // sidechain sources as raw audio files
            const std::string f = (tmp / ("sc-" + std::to_string(d) + ".pcm")).string();
            if (!fs::exists(f)) {
                std::ofstream o(f, std::ios::binary);
                o.write(reinterpret_cast<const char *>(scAudio[d].left.data()), (std::streamsize)(frames * sizeof(float)));
                o.write(reinterpret_cast<const char *>(scAudio[d].right.data()), (std::streamsize)(frames * sizeof(float)));
            }
            args.push_back(std::to_string(d) + "=" + f);
        }
        platform::Process proc;
        platform::spawn(args, proc, false, !verbose);
        if (verbose) std::fprintf(stderr, "rendering %s (%s) in worker %d...\n", job.tracks[i].name.c_str(), job.tracks[i].plugin.c_str(), proc.id);
        running.push_back({i, proc, std::chrono::steady_clock::now()});
        started[i] = true;
        std::string names;
        for (auto &r : running) names += (names.empty() ? "" : ", ") + job.tracks[r.index].name;
        progress(names);
    };
    auto ready = [&](size_t i) { for (size_t d : deps[i]) if (!trackDone[d]) return false; return true; };
    bool failed = false;
    auto cleanup = [&] {
        for (auto &run : running) platform::kill(run.proc);
        if (isolate) fs::remove_all(tmp, ec);
    };
    while (!failed && std::find(trackDone.begin(), trackDone.end(), false) != trackDone.end()) {
        bool progressed = false;
        for (size_t i = 0; i < job.tracks.size() && !failed; ++i) {
            if (trackDone[i] || started[i] || !ready(i)) continue;
            const Track &track = job.tracks[i];
            if (!cacheKeys.empty() && !cacheKeys[i].empty()) {   // unchanged since an earlier render: its audio as it was
                const auto tt = std::chrono::steady_clock::now();
                nlohmann::json res;
                Audio audio;
                if (trackcache::load(cacheKeys[i], frames, audio, res)) {
                    started[i] = true;
                    progressed = true;
                    TrackResult &tr = trackResults[i];
                    tr.name = track.name;
                    trackFromJson(res, tr);
                    tr.cached = true;
                    if (!finish(i, audio)) { failed = true; break; }
                    tr.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - tt).count();
                    progress(track.name);
                    continue;
                }
            }
            if (isolate && !isBuiltin(track.plugin)) {   // plugin tracks: a worker process each
                if (running.size() < (size_t)parallel) {
                    startTrack(i);
                    if (!workerErr.empty()) { err = workerErr; failed = true; break; }
                    progressed = true;
                }
                continue;
            }
            started[i] = true;   // built-ins (and everything with --jobs 0): here
            progressed = true;
            TrackResult &tr = trackResults[i];
            tr.name = track.name;
            Audio audio;
            audio.resize(frames);
            const auto tt = std::chrono::steady_clock::now();
            if (verbose) std::fprintf(stderr, "rendering %s (%s)...\n", track.name.c_str(), track.plugin.c_str());
            progress(track.name);
            std::map<size_t, Audio> rendered;   // this track's render clips, by clip index
            for (auto &c : captures) if (c.track == i) rendered[c.clip] = std::move(c.audio);
            if (!renderTrackAudio(job, i, trackChains[i], ctx, audio, tr, verbose, err, &rendered)) { failed = true; break; }
            if (!cacheKeys.empty()) trackcache::store(cacheKeys[i], audio, trackToJson(tr));
            if (!finish(i, audio)) { failed = true; break; }
            tr.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - tt).count();
        }
        if (failed) break;
        if (running.empty()) {
            if (!progressed) { err = "internal: no track can start (sidechain dependencies)"; failed = true; }
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // a worker with a window on screen is waiting for someone to click a licence dialog: every 2 s, look
        const bool lookForWindows = std::chrono::steady_clock::now() - lastWindowCheck > std::chrono::seconds(2);
        if (lookForWindows) lastWindowCheck = std::chrono::steady_clock::now();
        for (size_t r = 0; r < running.size() && !failed;) {
            Running &run = running[r];
            std::string crash;
            const bool exited = platform::finished(run.proc, crash);
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - run.started).count();
            const bool hung = !exited && took > limit;
            const bool windowed = !exited && !hung && lookForWindows && platform::hasOnscreenWindow(run.proc.id);
            if (hung || windowed) platform::kill(run.proc);
            if (!exited && !hung && !windowed) { ++r; continue; }
            const size_t i = run.index;
            const std::string prefix = (tmp / std::to_string(i)).string();
            TrackResult &tr = trackResults[i];
            tr.name = job.tracks[i].name;
            std::ifstream jin(prefix + ".json");
            const nlohmann::json res = jin ? nlohmann::json::parse(jin, nullptr, false) : nlohmann::json();
            Audio audio;
            audio.resize(frames);
            if (res.is_object() && res.value("ok", false)) {
                const auto retried = tr.warnings;   // "rendered again" notes from earlier attempts
                trackFromJson(res, tr);
                tr.warnings.insert(tr.warnings.begin(), retried.begin(), retried.end());
                std::ifstream pin(prefix + ".pcm", std::ios::binary);
                pin.read(reinterpret_cast<char *>(audio.left.data()), (std::streamsize)(frames * sizeof(float)));
                pin.read(reinterpret_cast<char *>(audio.right.data()), (std::streamsize)(frames * sizeof(float)));
                if (!pin) { err = "track '" + tr.name + "': worker output is incomplete"; failed = true; }
                else if (!finish(i, audio)) failed = true;
                else if (!cacheKeys.empty()) { pin.close(); trackcache::storeFiles(cacheKeys[i], prefix); }
            } else if (res.is_object() && res.contains("error")) {   // a job mistake (unknown preset, bad state): the render fails
                err = res["error"].get<std::string>();
                failed = true;
            } else if (!hung && !windowed && crashes[i] < job.retries) {   // plugin crashes are mostly races (Altitude): render the track again
                ++crashes[i];
                const std::string why = "crashed while rendering" + (crash.empty() ? "" : " (" + crash + ")");
                tr.warnings.push_back(job.tracks[i].plugin + " " + why + "; rendered again (attempt " + std::to_string(crashes[i] + 1) + ")");
                if (verbose) std::fprintf(stderr, "%s: %s, starting it again\n", tr.name.c_str(), why.c_str());
                std::error_code rec;
                fs::remove(prefix + ".pcm", rec);
                fs::remove(prefix + ".json", rec);
                running.erase(running.begin() + (long)r);
                startTrack(i);
                continue;
            } else {   // the plugin crashed on every attempt or hung: the song renders without this track (silence keys its dependents)
                tr.plugin = tr.pluginName = job.tracks[i].plugin;
                std::string why = hung ? "hung (killed after " + std::to_string((int)limit) + " s)"
                                  : windowed ? "opened a window, most likely a licence or registration dialog (killed; `wavelength plugins --block` it if it keeps asking)"
                                  : "crashed while rendering" + (crash.empty() ? "" : " (" + crash + ")");
                if (crashes[i]) why += " on all " + std::to_string(crashes[i] + 1) + " attempts";
                tr.warnings.push_back("track failed: " + job.tracks[i].plugin + " " + why + "; the mix is rendered without it");
                tr.lufs = -120;
                tr.levels = Levels{-240, -240, -240, true};
                result.failedTracks.push_back(tr.name);
                result.warnings.push_back("track '" + tr.name + "' failed (" + why + ") and is missing from the mix");
                if (sources.count(i)) scAudio[i] = audio;
                trackDone[i] = true;
            }
            tr.seconds = took;
            std::error_code rec;
            fs::remove(prefix + ".pcm", rec);
            fs::remove(prefix + ".json", rec);
            running.erase(running.begin() + (long)r);
            std::string names;
            for (auto &x : running) names += (names.empty() ? "" : ", ") + job.tracks[x.index].name;
            progress(names);
        }
    }
    cleanup();
    if (failed) return false;
    if (job.trackCache) trackcache::prune();
    for (auto &tr : trackResults) result.tracks.push_back(std::move(tr));
    progress("buses and master");
    if (!job.window.on) {   // a window is too short to judge the mix or the arrangement
        mixChecks(job, result.tracks, seconds, result.warnings);
        repetitionChecks(job, result.warnings);
    }

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
        if (!runChain(busChains[b], buses[b], ctx, br.fx, warnings, "bus '" + br.name + "'", err, &br.automation)) return false;
        for (auto &w : warnings) result.warnings.push_back("bus '" + br.name + "': " + w);
        Audio *dest = &mix;
        for (size_t o = 0; o < job.buses.size(); ++o) if (job.buses[o].name == job.buses[b].output) dest = &buses[o];
        if (job.buses[b].stem && job.stemBits) {   // after its fx, before its fader: the same signal as its lufs
            br.file = (fs::path(outDir) / "stems" / ("bus-" + slug(br.name) + ".wav")).string();
            if (job.window.loop) { if (!writeWav(br.file, foldLoop(buses[b], loopFrames), job.sampleRate, err, job.stemBits, 0, 0, true)) return false; }
            else if (!writeWav(br.file, buses[b], job.sampleRate, err, job.stemBits, leadFrames, trimFrames)) return false;
        } else if (job.buses[b].stem) result.warnings.push_back("bus '" + br.name + "': \"stem\": true, but \"stems\" is \"none\": no stem written");
        br.levels = measure(buses[b]);   // before the fader, like a track's stem: `gain` = target - lufs
        br.lufs = integratedLufs(buses[b], job.sampleRate);
        const auto &env = job.buses[b].gainAutomation;
        float g = (float)dsp::dbToLin(job.buses[b].gainDb);
        // its pan after the fader, on the track's laws: constant power, or a balance (the far side falls as (1 - |pan|)^2)
        const double pan = job.buses[b].pan;
        float pl = 1, pr = 1;
        if (job.buses[b].balancePan) { pl = (float)(pan > 0 ? (1 - pan) * (1 - pan) : 1.0); pr = (float)(pan < 0 ? (1 + pan) * (1 + pan) : 1.0); }
        else if (pan != 0) { const double angle = (pan + 1.0) * dsp::kPi / 4.0; pl = (float)(std::cos(angle) * M_SQRT2); pr = (float)(std::sin(angle) * M_SQRT2); }
        for (size_t f = 0; f < frames; ++f) {
            if (!env.empty() && f % 32 == 0) g = (float)dsp::dbToLin(job.buses[b].gainDb + env.at(f / sr));
            buses[b].left[f] *= g * pl; buses[b].right[f] *= g * pr;
            dest->left[f] += buses[b].left[f]; dest->right[f] += buses[b].right[f];
        }
        for (size_t m = 0; m < job.markers.size(); ++m) {
            const double a0 = job.markers[m].sec, b0 = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
            br.sectionLufs.push_back(integratedLufs(buses[b], job.sampleRate, (size_t)(a0 * sr), (size_t)(b0 * sr)));
        }
    }
    for (auto &br : busResults) result.buses.push_back(std::move(br));
    buses.clear();

    // per-section loudness before the master: what track and bus faders and rides did, before the master
    // gain, rides, chain and loudness target move it (a limiter hands most of a ride back)
    std::vector<double> preMaster;
    for (size_t m = 0; m < job.markers.size(); ++m) {
        const double a = job.markers[m].sec, b = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
        preMaster.push_back(integratedLufs(mix, job.sampleRate, (size_t)(a * sr), (size_t)(b * sr)));
    }
    // 3. master chain, then optional peak normalisation
    if (job.masterGainDb != 0 || !job.masterGainAutomation.empty()) {
        float g = (float)dsp::dbToLin(job.masterGainDb);
        for (size_t f = 0; f < frames; ++f) {
            if (!job.masterGainAutomation.empty() && f % 32 == 0) g = (float)dsp::dbToLin(job.masterGainDb + job.masterGainAutomation.at(f / sr));
            mix.left[f] *= g; mix.right[f] *= g;
        }
    }
    if (!job.hasMasterLoudness) {
        std::vector<std::string> warnings;
        if (!runChain(masterChain, mix, ctx, result.masterFx, warnings, "master", err, &result.masterAutomation)) return false;
        for (auto &w : warnings) result.warnings.push_back("master: " + w);
    } else {
        // a loudness target: find the gain that lands the output on it. The gain goes in front of the
        // chain's last limiter (like a limiter's input gain), so EQ and glue compressors before it see
        // the mix as mixed and keep the section contrast; "loudnessGain": "start" puts it before
        // everything. Limiters compress, so the output moves less than the input; a few passes converge.
        size_t split = 0;
        auto typeAt = [&](size_t k) { return k < job.masterFx.size() && job.masterFx[k].is_object() ? job.masterFx[k].value("type", "") : std::string(); };
        if (job.loudnessGain != "start")
            for (size_t k = 0; k < job.masterFx.size(); ++k)
                if (typeAt(k) == "limiter") split = k;
        // "peak": also in front of the clips (and limiters) right before that limiter, so they see the loud signal
        if (job.loudnessGain == "peak" && typeAt(split) == "limiter")
            while (split > 0 && (typeAt(split - 1) == "clip" || typeAt(split - 1) == "limiter")) --split;
        const nlohmann::json head(job.masterFx.begin(), job.masterFx.begin() + (long)split),
                             tail(job.masterFx.begin() + (long)split, job.masterFx.end());
        std::vector<std::string> headLabels, headWarnings;
        nlohmann::json headAutomation = nlohmann::json::array(), automation;
        {
            Chain headChain;
            if (!buildChain(head, job, "master", headChain, err)) return false;
            if (!runChain(headChain, mix, ctx, headLabels, headWarnings, "master", err, &headAutomation)) return false;
        }
        const Audio pre = mix;
        double gainDb = 0, reached = -120;
        std::vector<std::string> labels, warnings;
        if (job.levelFixed) gainDb = job.fixedLoudnessGainDb;   // --level-from: the full mix's gain, no targeting
        for (int pass = 0; pass < 8; ++pass) {
            Chain chain;
            if (!buildChain(tail, job, "master", chain, err)) return false;
            mix = pre;
            const float g = (float)dsp::dbToLin(gainDb);
            for (size_t f = 0; f < frames; ++f) { mix.left[f] *= g; mix.right[f] *= g; }
            labels = headLabels; warnings = headWarnings; automation = headAutomation;
            for (auto &fx : chain) fx->index += (int)split;   // positions in the whole master list
            if (!runChain(chain, mix, ctx, labels, warnings, "master", err, &automation)) return false;
            reached = integratedLufs(mix, job.sampleRate);
            if (job.levelFixed) break;
            const double miss = job.masterLoudness - reached;
            if (std::fabs(miss) < 0.1 || reached < -69) break;
            gainDb = std::clamp(gainDb + miss, -40.0, 30.0);
        }
        result.masterFx = labels;
        result.masterAutomation = automation;
        for (auto &w : warnings) result.warnings.push_back("master: " + w);
        result.loudnessGainDb = gainDb;
        if (!job.levelFixed && std::fabs(job.masterLoudness - reached) >= 0.3) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "master: loudness target %.1f LUFS not reached (%.1f LUFS with %+.1f dB into the chain); the limiter or chain caps it",
                          job.masterLoudness, reached, gainDb);
            result.warnings.push_back(buf);
        }
        if (gainDb > 8) {   // the gain lands before the chain: every compressor threshold now sits that much lower in effect
            char buf[300];
            std::snprintf(buf, sizeof buf, "master: the loudness target adds %+.1f dB %s, so %s works %.0f dB harder than its settings "
                          "suggest; raise the track faders instead", gainDb, split ? "in front of the last limiter" : "before the master chain",
                          split ? "that limiter" : "every compressor and limiter in it", gainDb);
            result.warnings.push_back(buf);
        }
        if (job.hasNormalize) result.warnings.push_back("master: \"normalize\" after a loudness target changes the loudness again; use one of them");
    }
    // a hard-working master limiter: name the tracks whose peaks drive it (peak after the fader, and
    // how far the peaks stand above the track's loudness: a spiky kick or clap limits the whole song)
    if (std::any_of(result.warnings.begin(), result.warnings.end(), [](const std::string &w) { return w.find("master: limiter:") == 0; })) {
        std::vector<const TrackResult *> byPeak;
        for (auto &t : result.tracks) if (t.postPeakDb > -100) byPeak.push_back(&t);
        std::sort(byPeak.begin(), byPeak.end(), [](auto *a, auto *b) { return a->postPeakDb > b->postPeakDb; });
        std::string list;
        for (size_t k = 0; k < byPeak.size() && k < 3; ++k) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s'%s' peaks %.1f dBFS (%.0f dB above its loudness)", k ? ", " : "", byPeak[k]->name.c_str(),
                          byPeak[k]->postPeakDb, byPeak[k]->levels.peakDb - byPeak[k]->lufs);
            list += buf;
        }
        if (!list.empty())
            result.warnings.push_back("master: the loudest track peaks feeding the limiter: " + list +
                                      "; a limiter or saturate on a spiky track lets the song get loud with less master limiting");
    }
    const Levels before = measure(mix);
    if (job.hasNormalize && !before.silent) {
        const double gainDb = job.levelFixed ? job.fixedNormalizeGainDb : job.normalizeDb - before.peakDb;
        const float g = (float)dsp::dbToLin(gainDb);
        for (size_t f = 0; f < frames; ++f) { mix.left[f] *= g; mix.right[f] *= g; }
        result.normalizeGainDb = gainDb;
    }
    result.mixFile = (fs::path(outDir) / "mix.wav").string();
    if (job.window.loop) {   // from here on the song is the loop: files, measurements and the picture
        mix = foldLoop(mix, loopFrames);
        result.seconds = job.window.loopSec;
    }
    if (!writeWav(result.mixFile, mix, job.sampleRate, err, 32, leadFrames, trimFrames, job.window.loop)) return false;
    result.mix = measure(mix);
    result.truePeakDb = truePeakDb(mix);
    if (!job.deliver.empty() &&
        !writeDeliveries(job.deliver, mix, job.sampleRate, leadFrames, trimFrames, outDir, result.mixFile, result.truePeakDb, result.deliveries, result.warnings, err))
        return false;
    result.mixLufs = integratedLufs(mix, job.sampleRate);
    result.mixLra = loudnessRange(mix, job.sampleRate);
    for (size_t m = 0; m < job.markers.size(); ++m) {
        const double a = job.markers[m].sec, b = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
        result.sections.push_back({job.markers[m].name, a + job.leadIn, b + job.leadIn,   // times in the written file
                                   integratedLufs(mix, job.sampleRate, (size_t)(a * sr), (size_t)(b * sr)), preMaster[m], job.markers[m].checks});
    }
    if (result.mix.peakDb > 0.0)
        result.warnings.push_back("mix peaks above 0 dBFS: add a limiter to \"master\", lower track gains, or set \"normalize\"");

    // dropouts: the song seems to stop (15 dB under the last 8 s) for 0.75 s or more, then comes back
    // (within 6 dB of that level inside 3 s). A half-beat breath is shorter; an ending never comes back.
    {
        const double hop = 0.25;
        const std::vector<double> tl = loudnessTimeline(mix, job.sampleRate, 0.5, hop);
        auto fileTime = [&](double s) {
            char buf[16];
            const double t = s + job.leadIn;
            std::snprintf(buf, sizeof buf, "%d:%04.1f", (int)(t / 60), std::fmod(t, 60.0));
            return std::string(buf);
        };
        for (size_t i = (size_t)(2.0 / hop); i < tl.size();) {
            // reference: the upper quartile of the previous 8 s (the music, not its quiet moments)
            const size_t back = (size_t)(8.0 / hop);
            std::vector<double> prev(tl.begin() + (long)(i > back ? i - back : 0), tl.begin() + (long)i);
            std::sort(prev.begin(), prev.end());
            const double ref = prev.empty() ? -120 : prev[prev.size() * 3 / 4];
            if (ref < -40 || tl[i] > ref - 15) { ++i; continue; }
            size_t j = i;
            double sum = 0;
            while (j < tl.size() && tl[j] <= ref - 15) { sum += std::pow(10.0, tl[j] / 10); ++j; }
            const double len = (double)(j - i) * hop + 0.25;
            size_t k = j;
            while (k < tl.size() && k < j + (size_t)(3.0 / hop) && tl[k] < ref - 6) ++k;
            if (len >= 0.75 && k < tl.size() && k < j + (size_t)(3.0 / hop)) {
                const double s0 = i * hop + 0.25, s1 = j * hop + 0.25;   // window centres
                const double level = 10 * std::log10(std::max(sum / (double)(j - i), 1e-12));
                const double b0 = job.tempo.secToBeat(s0) / 4 + 1, b1 = job.tempo.secToBeat(s1) / 4 + 1;
                bool intended = false;   // inside (or right before) a section marked "checks": false
                for (size_t m = 0; m < job.markers.size(); ++m) {
                    const double ms = job.markers[m].sec, me = m + 1 < job.markers.size() ? job.markers[m + 1].sec : seconds;
                    if (!job.markers[m].checks && s1 >= ms && s0 < me) intended = true;
                }
                result.dropouts.push_back({s0, s1, level, ref, b0, b1, intended});
                if (intended) { i = std::max(j, i + 1); continue; }
                char buf[400];
                std::snprintf(buf, sizeof buf, "dropout: %.1f s at %.1f LUFS (bar %.0f-%.0f, %s in the file), %.0f dB under the music before it, "
                              "then it comes back: listeners hear the song stop. Keep the groove going or build into the hit (a half-beat breath is fine)",
                              s1 - s0, level, std::floor(b0), std::floor(b1), fileTime(s0).c_str(), ref - level);
                result.warnings.push_back(buf);
            }
            i = std::max(j, i + 1);
        }
    }
    // drops that don't land. A section is checked when it is named like a payoff (Drop, Chorus, Peak, Hook,
    // Final, Climax, Finale) or follows a section named like a build (Build, Rise, Pre..., Ramp, Climb,
    // Lead-in); an escalation (Climax, Peak, Finale after another payoff) must at least rise.
    auto has = [](std::string n, std::initializer_list<const char *> words, bool wholeWord) {
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        for (const char *w : words)
            for (size_t p = n.find(w); p != std::string::npos; p = n.find(w, p + 1)) {
                const size_t e = p + std::strlen(w);
                if (!wholeWord || ((p == 0 || !std::isalpha((unsigned char)n[p - 1])) && (e == n.size() || !std::isalpha((unsigned char)n[e]))))
                    return true;
            }
        return false;
    };
    auto buildLike = [&](const std::string &n) { return has(n, {"build", "rise", "riser", "pre", "ramp", "climb", "lead", "into", "up", "rebuild", "ignition"}, true)
                                                     || has(n, {"build", "rise"}, false); };
    auto payoff = [&](const std::string &n) {
        return !buildLike(n) && !has(n, {"end"}, true) && has(n, {"drop", "chorus", "peak", "climax", "finale", "hook", "final"}, false);
    };
    auto escalation = [&](const std::string &n) { return has(n, {"climax", "peak", "finale"}, false); };
    // every boundary: the last 2 bars before it against the first 4 after it (what a listener compares)
    for (size_t m = 1; m < result.sections.size() && m < job.markers.size(); ++m) {
        const double prevStart = job.markers[m - 1].beat, at = job.markers[m].beat;
        const double next = m + 1 < job.markers.size() ? job.markers[m + 1].beat : job.tempo.secToBeat(seconds);
        auto win = [&](double b0, double b1) {
            return integratedLufs(mix, job.sampleRate, (size_t)(job.tempo.beatToSec(b0) * sr), (size_t)(job.tempo.beatToSec(b1) * sr));
        };
        auto &s = result.sections[m];
        const double t0 = std::max(prevStart, at - 8);
        s.at = at;
        s.tailFrom = t0;
        s.tailBefore = win(t0, at);
        s.head = win(at, std::min(next, at + 16));
        // a near-silence right before the boundary (a power cut, a held breath): more than 15 dB under the 4 bars
        // before it, or under -40 LUFS. Compare the section with the last 2 bars of music before the silence.
        const double ref = t0 > 0 ? win(std::max(0.0, t0 - 16), t0) : -120;
        auto quiet = [&](double l) { return l < -40 || (ref > -60 && l < ref - 15); };
        if (quiet(s.tailBefore) && ref > -60) {
            for (double x = t0; x >= 8 && x > t0 - 128; x -= 4) {
                const double l = win(x - 8, x);
                if (quiet(l)) continue;
                s.skippedSilence = true;
                s.silenceLufs = s.tailBefore;
                s.silenceFrom = x;
                s.tailFrom = x - 8;
                s.tailBefore = l;
                break;
            }
        }
    }
    for (size_t m = 1; m < result.sections.size(); ++m) {
        const auto &a = result.sections[m - 1];
        auto &b = result.sections[m];
        if (!b.checks || (a.lufs < -60 && !b.skippedSilence) || b.lufs < -60) continue;
        double jump = b.lufs - a.lufs;
        double need = 0;
        if (buildLike(a.name) && !buildLike(b.name)) need = 2.0;          // whatever a build leads into must land
        else if (payoff(b.name) && !payoff(a.name)) need = 2.0;           // a payoff after a non-payoff
        else if (payoff(a.name) && escalation(b.name)) need = 0.5;        // Final Act -> Climax: at least rise
        // a drop is heard at its boundary: compare the build's last 2 bars with the drop's first 4
        if (need >= 2.0 && b.tailBefore > -60 && b.head > -60) jump = b.head - b.tailBefore;
        b.need = need;
        b.jump = jump;
        if (need == 0 || jump >= need) continue;
        char buf[480], against[160] = "";
        if (need >= 2 && b.skippedSilence)
            std::snprintf(against, sizeof against, " (its first 4 bars against the last 2 bars of music before the near-silence at bars %.0f-%.0f)",
                          std::floor(b.silenceFrom / 4) + 1, std::floor(b.at / 4));
        else if (need >= 2) std::snprintf(against, sizeof against, " (its first 4 bars against the last 2 before them)");
        std::snprintf(buf, sizeof buf, "section '%s' lands only %+.1f dB over '%s'%s: %s (mark the section \"checks\": false if it is meant this way)",
                      b.name.c_str(), jump, b.skippedSilence && need >= 2 ? "the music before it" : a.name.c_str(), against,
                      need >= 2 ? "empty the build (kick and bass out, high-pass sweep) rather than turning it down, and stack the downbeat; 3-5 dB reads as a drop"
                                : "an escalation should rise: add a layer, open filters, lift it a little");
        result.warnings.push_back(buf);
    }
    // every track warning also lands in the top-level list, named: an agent reading `warnings` must not
    // miss a curve that holds a lead 9 dB down because it was only in tracks[].warnings
    {
        const std::vector<std::string> own = result.warnings;
        for (const auto &t : result.tracks)
            for (const auto &w : t.warnings) {
                const std::string named = "track '" + t.name + "': " + w;
                const bool already = std::any_of(own.begin(), own.end(), [&](const std::string &o) { return o == w || o.find(w) != std::string::npos; });
                if (!already) result.warnings.push_back(named);
            }
    }
    if (job.picture) {   // song.png: sections, loudness, spectrum and a lane per track, for agents that can see
        Picture pic;
        const fs::path src(job.sourcePath);
        pic.title = src.empty() ? fs::path(outDir).filename().string() : titleOfJob(fs::absolute(src));
        pic.width = job.pictureWidth;
        pic.from = job.window.on ? job.window.trimSec : 0;
        pic.seconds = job.window.loop ? job.window.loopSec : seconds;
        pic.mix = &mix;
        pic.mixLufs = result.mixLufs;
        pic.lra = result.mixLra;
        pic.truePeak = result.truePeakDb;
        for (auto &s : result.sections) { pic.sectionLufs.push_back(s.lufs); pic.sectionChecks.push_back({s.need, s.jump}); }
        for (auto &d : result.dropouts) if (!d.intended) pic.dropouts.push_back({d.start, d.end});
        for (size_t i = 0; i < job.tracks.size() && i < result.tracks.size(); ++i) {
            const TrackResult &t = result.tracks[i];
            PictureTrack p;
            p.name = t.name;
            p.lufs = t.lufs;
            p.postLufs = t.postLufs;
            p.level = t.levelTimeline;
            p.failed = std::find(result.failedTracks.begin(), result.failedTracks.end(), t.name) != result.failedTracks.end();
            pic.tracks.push_back(std::move(p));
        }
        const std::string file = (fs::path(outDir) / "song.png").string();
        std::string perr;
        int h = 0;
        if (writePicture(file, job, pic, h, perr)) { result.pictureFile = file; result.pictureWidth = std::clamp(pic.width, 800, 3200); result.pictureHeight = h; }
        else result.warnings.push_back("picture: " + perr);
    }
    result.renderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace wl
