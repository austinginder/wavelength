#include "engine.hpp"

#include "catalog.hpp"
#include "state_file.hpp"

#include <algorithm>
#include <cmath>

namespace wl {

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

    if (!setup.preset.empty()) {
        if (!out.plugin->loadPreset(setup.preset, out.preset, err)) { err = context + ": " + err; return false; }
        out.plugin->pump(100);
    }
    if (!setup.stateFile.empty()) {
        StateFile sf;
        if (!readStateFile(setup.stateFile, setup.stateFormat, sf, err)) { err = context + ": " + err; return false; }
        out.stateFormat = sf.format;
        if (!out.plugin->loadState(sf, err)) { err = context + ": " + err; return false; }
        out.plugin->pump(50);
    }
    auto lookup = [&](const std::string &key, ParamInfo &pi) -> bool {
        if (out.plugin->findParam(key, pi)) return true;
        err = context + ": no parameter '" + key + "' on " + info.name + " (run `wavelength params \"" + info.name + "\"`)";
        return false;
    };
    for (const auto &p : setup.params) {
        ParamInfo pi;
        if (!lookup(p.key, pi)) return false;
        const double v = std::clamp(p.value, std::min(pi.min, pi.max), std::max(pi.min, pi.max));
        if (v != p.value) out.warnings.push_back("parameter '" + pi.name + "' clamped to " + std::to_string(v));
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

bool runPlugin(const Job &job, OpenedPlugin &p, const std::vector<TimedEvent> &events, const Audio *input, Audio &out,
               std::string &err) {
    return p.plugin->render(job, events, p.initial, p.autos, input, out, p.warnings, err);
}

} // namespace wl
