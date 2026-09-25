#include "engine.hpp"

#include "catalog.hpp"
#include "preset_files.hpp"
#include "state_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace wl {

bool loadStateInto(Plugin &plugin, StateFile &sf, std::string &err) {
    if (sf.transform) {
        std::vector<uint8_t> current;
        if (!plugin.getState(current, err) || !sf.transform(plugin, current, sf.state, err)) return false;
        sf.transform = nullptr;
        if (sf.state.empty()) return true;   // the transform set parameters only (a Microtonic drum)
    }
    return plugin.loadState(sf, err);
}

bool loadPresetByName(Plugin &plugin, const PluginInfo &info, const std::string &query, std::string &loadedName,
                      std::string &stateFormat, std::string &err, std::vector<std::string> *warnings) {
    std::string pluginErr;
    if (plugin.loadPreset(query, loadedName, pluginErr)) return true;
    // not in the plugin's own library: a preset file in its preset folders, a cartridge voice, NKS
    auto files = filePresets(info);
    PresetInfo hit;
    std::string fileErr, q = query, suffix;
    const size_t hash = query.rfind('#');   // "AC BD Back#3": a Microtonic drum on channel 3
    if (hash != std::string::npos && hash + 1 < query.size() && std::isdigit((unsigned char)query[hash + 1])) {
        q = query.substr(0, hash);
        suffix = query.substr(hash);
    }
    if (!findPreset(files, q, hit, fileErr)) {   // maybe new NKS files: rebuild that index once
        nksPresets(info, true);
        files = filePresets(info);
    }
    if (files.empty() || !findPreset(files, q, hit, fileErr)) { err = files.empty() ? pluginErr : fileErr; return false; }
    if (!suffix.empty()) hit.location += suffix;
    StateFile sf;
    if (!readStateFile(hit.location, "auto", sf, err) || !loadStateInto(plugin, sf, err)) return false;
    if (warnings) for (auto &w : sf.warnings) warnings->push_back("preset '" + hit.name + "': " + w);
    loadedName = hit.name;
    stateFormat = sf.format;
    return true;
}

bool openPlugin(const PluginSetup &setup, const std::string &context, OpenedPlugin &out, std::string &err) {
    PluginInfo info;
    if (!resolvePlugin(setup.spec, info, err)) { err = context + ": " + err; return false; }
    out.id = info.id;
    out.name = info.name;
    out.format = info.format;
    out.plugin = createPlugin(info, err);
    if (!out.plugin) { err = context + ": " + err; return false; }
    out.plugin->verbose = setup.verbose;
    out.plugin->warmup = setup.warmup;
    if (setup.warmup < 0) {   // plugins known to load patches or samples asynchronously after activation
        std::string n;
        for (unsigned char c : info.name) if (std::isalnum(c)) n += (char)std::tolower(c);
        if (n == "voltagemodular") out.plugin->warmup = 5;
        else if (n == "bbcsymphonyorchestra") out.plugin->warmup = 6;
        else if (n == "analoglabv") out.plugin->warmup = 15;   // sampled engines stream after activation
        else if (n == "decentsampler") out.plugin->warmup = 3;
    }

    // a preset or state the plugin silently ignores leaves every parameter where it was
    auto snapshot = [&] {
        std::vector<double> v;
        for (const auto &p : out.plugin->params()) v.push_back(p.value);
        return v;
    };
    auto checkChanged = [&](const std::vector<double> &before, const std::string &what) {
        // plugins whose state selects a program without moving any parameter (AAS Player, DecentSampler,
        // SynthMaster, BBC Symphony Orchestra's NKS instruments) would always trip this check
        if (out.stateFormat == "aas" || out.stateFormat == "decentsampler" || info.name.rfind("SynthMaster", 0) == 0 ||
            info.name == "BBC Symphony Orchestra") return;
        const auto after = snapshot();
        if (before.empty() || before.size() != after.size()) return;
        for (size_t i = 0; i < before.size(); ++i) if (std::fabs(before[i] - after[i]) > 1e-7) return;
        out.warnings.push_back(what + " changed none of " + info.name + "'s " + std::to_string(before.size()) +
                               " parameters: it was either already loaded or ignored (wrong format for this plugin?)");
    };
    if (!setup.preset.empty()) {
        const auto before = snapshot();
        if (!loadPresetByName(*out.plugin, info, setup.preset, out.preset, out.stateFormat, err, &out.warnings)) { err = context + ": " + err; return false; }
        out.plugin->pump(100);
        checkChanged(before, "preset '" + out.preset + "'");
    }
    if (!setup.stateFile.empty()) {
        StateFile sf;
        if (!readStateFile(setup.stateFile, setup.stateFormat, sf, err)) { err = context + ": " + err; return false; }
        out.stateFormat = sf.format;
        const auto before = snapshot();
        if (!loadStateInto(*out.plugin, sf, err)) { err = context + ": " + err; return false; }
        for (auto &w : sf.warnings) out.warnings.push_back(w);
        out.plugin->pump(50);
        checkChanged(before, "state " + sf.format + " file");
    }
    auto lookup = [&](const std::string &key, ParamInfo &pi) -> bool {
        if (out.plugin->findParam(key, pi)) return true;
        err = context + ": no parameter '" + key + "' on " + info.name + " (run `wavelength params \"" + info.name + "\"`)";
        return false;
    };
    for (auto p : setup.params) {
        ParamInfo pi;
        if (!lookup(p.key, pi)) return false;
        if (!p.text.empty() && !out.plugin->valueFromText(pi.id, p.text, p.value)) {
            err = context + ": " + info.name + " could not read '" + p.text + "' as a value of '" + pi.name + "' (now showing '" +
                  pi.display + "'); use a number in [" + std::to_string(pi.min) + " .. " + std::to_string(pi.max) + "] or its own text format";
            return false;
        }
        const double lo = std::min(pi.min, pi.max), hi = std::max(pi.min, pi.max);
        const double v = std::clamp(p.value, lo, hi);
        if (std::fabs(v - p.value) > 1e-6 * std::max(1.0, hi - lo)) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "parameter '%s' = %g is outside [%g .. %g]; clamped to %g", pi.name.c_str(), p.value, lo, hi, v);
            out.warnings.push_back(buf);
        }
        out.initial.push_back({pi.id, pi.cookie, v});
    }
    for (const auto &[key, env] : setup.automation) {
        ParamInfo pi;
        if (!lookup(key, pi)) return false;
        out.autos.push_back({pi.id, pi.cookie, pi.name, env});
        out.initial.push_back({pi.id, pi.cookie, std::clamp(env.at(0), std::min(pi.min, pi.max), std::max(pi.min, pi.max))});
    }
    if (!out.plugin->setParams(out.initial, err)) { err = context + ": " + err; return false; }
    return true;
}

std::vector<TimedEvent> scheduleNotes(const std::vector<Note> &notes, int sampleRate) {
    std::vector<TimedEvent> events;
    for (const auto &n : notes) {
        const int64_t on = (int64_t)std::llround(n.start * sampleRate);
        const int64_t off = std::max(on + 1, (int64_t)std::llround((n.start + n.length) * sampleRate));
        events.push_back({on, true, n.key, n.channel, n.velocity});
        events.push_back({off, false, n.key, n.channel, n.velocity});
    }
    std::stable_sort(events.begin(), events.end(), [](const TimedEvent &a, const TimedEvent &b) {
        return a.frame != b.frame ? a.frame < b.frame : (!a.on && b.on);
    });
    return events;
}

void scheduleControllers(const Track &track, int sampleRate, double seconds, std::vector<TimedEvent> &events) {
    const int64_t total = (int64_t)std::ceil(seconds * sampleRate);
    auto add = [&](const Envelope &env, TimedEvent::Kind kind, int number, double steps, auto toValue) {
        if (env.empty()) return;
        double last = NAN;
        for (int64_t f = 0; f < total; f += 64) {
            const double v = std::round(toValue(env.at((double)f / sampleRate)) * steps) / steps;
            if (v == last) continue;
            last = v;
            TimedEvent e{f, false, -1, 0, 0};
            e.kind = kind; e.number = number; e.value = v; e.range = track.bendRange;
            events.push_back(e);
        }
    };
    for (const auto &[num, env] : track.ccAutomation) add(env, TimedEvent::CC, num, 127, [](double v) { return std::clamp(v / 127.0, 0.0, 1.0); });
    add(track.pressureAutomation, TimedEvent::Pressure, 0, 127, [](double v) { return std::clamp(v / 127.0, 0.0, 1.0); });
    const double range = std::max(0.01, track.bendRange);
    add(track.bendAutomation, TimedEvent::PitchBend, 0, 8192, [range](double semis) { return std::clamp(semis / range, -1.0, 1.0); });
    // controllers before notes at the same frame, so a note starts already bent
    std::stable_sort(events.begin(), events.end(), [](const TimedEvent &a, const TimedEvent &b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        const int ra = a.kind != TimedEvent::Note ? 0 : a.on ? 2 : 1, rb = b.kind != TimedEvent::Note ? 0 : b.on ? 2 : 1;
        return ra < rb;
    });
}

namespace {
// Plugins occasionally emit a lone sample of garbage (DUNE 3 once wrote 9188.0, +79 dBFS, into a brass
// chord): it clicks, excites every reverb downstream, pumps the limiter and wrecks the track's loudness
// reading. Mute non-finite samples and anything above +30 dBFS, which no real signal reaches, and say where.
void muteGarbage(Audio &out, int sampleRate, const std::string &name, std::vector<std::string> &warnings) {
    constexpr float kMax = 31.6f;   // +30 dBFS
    size_t count = 0, first = 0;
    float worst = 0;
    bool nonFinite = false;
    for (size_t i = 0; i < out.frames(); ++i)
        for (float *s : {&out.left[i], &out.right[i]}) {
            const bool bad = !std::isfinite(*s);
            if (!bad && std::fabs(*s) <= kMax) continue;
            if (!count) first = i;
            ++count;
            if (bad) nonFinite = true;
            else worst = std::max(worst, std::fabs(*s));
            *s = 0;
        }
    if (!count) return;
    const double t = (double)first / sampleRate;
    char at[32];
    std::snprintf(at, sizeof at, "%d:%04.1f", (int)(t / 60), std::fmod(t, 60.0));
    std::string what = std::to_string(count) + (count == 1 ? " sample" : " samples");
    what += nonFinite ? " that were not numbers (NaN or infinity)" : "";
    if (worst > 0) what += (nonFinite ? " or" : "") + std::string(" up to ") + std::to_string((int)std::lround(20 * std::log10(worst))) + " dBFS";
    warnings.push_back(name + " output " + what + ", first at " + at + " (render time, before any lead-in); they were muted");
}
} // namespace

bool runPlugin(const Job &job, OpenedPlugin &p, const std::vector<TimedEvent> &events, const Audio *input, Audio &out,
               std::string &err) {
    if (!p.plugin->render(job, events, p.initial, p.autos, input, out, p.warnings, err)) return false;
    muteGarbage(out, job.sampleRate, p.name, p.warnings);
    return true;
}

} // namespace wl
