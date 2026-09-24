#include "engine.hpp"

#include "catalog.hpp"
#include "state_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

    // a preset or state the plugin silently ignores leaves every parameter where it was
    auto snapshot = [&] {
        std::vector<double> v;
        for (const auto &p : out.plugin->params()) v.push_back(p.value);
        return v;
    };
    auto checkChanged = [&](const std::vector<double> &before, const std::string &what) {
        const auto after = snapshot();
        if (before.empty() || before.size() != after.size()) return;
        for (size_t i = 0; i < before.size(); ++i) if (std::fabs(before[i] - after[i]) > 1e-7) return;
        out.warnings.push_back(what + " changed none of " + info.name + "'s " + std::to_string(before.size()) +
                               " parameters: it was either already loaded or ignored (wrong format for this plugin?)");
    };
    if (!setup.preset.empty()) {
        const auto before = snapshot();
        if (!out.plugin->loadPreset(setup.preset, out.preset, err)) { err = context + ": " + err; return false; }
        out.plugin->pump(100);
        checkChanged(before, "preset '" + out.preset + "'");
    }
    if (!setup.stateFile.empty()) {
        StateFile sf;
        if (!readStateFile(setup.stateFile, setup.stateFormat, sf, err)) { err = context + ": " + err; return false; }
        out.stateFormat = sf.format;
        const auto before = snapshot();
        if (!out.plugin->loadState(sf, err)) { err = context + ": " + err; return false; }
        out.plugin->pump(50);
        checkChanged(before, "state " + sf.format + " file");
    }
    auto lookup = [&](const std::string &key, ParamInfo &pi) -> bool {
        if (out.plugin->findParam(key, pi)) return true;
        err = context + ": no parameter '" + key + "' on " + info.name + " (run `wavelength params \"" + info.name + "\"`)";
        return false;
    };
    for (const auto &p : setup.params) {
        ParamInfo pi;
        if (!lookup(p.key, pi)) return false;
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

bool runPlugin(const Job &job, OpenedPlugin &p, const std::vector<TimedEvent> &events, const Audio *input, Audio &out,
               std::string &err) {
    return p.plugin->render(job, events, p.initial, p.autos, input, out, p.warnings, err);
}

} // namespace wl
