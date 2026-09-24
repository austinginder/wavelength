#pragma once
// Running plugins (CLAP or VST3) over a whole timeline: instruments (notes in, audio out) and
// effects (audio in, audio out), with parameter automation.
#include "automation.hpp"
#include "plugin.hpp"
#include "job.hpp"
#include "wav.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wl {

struct PluginSetup {
    std::string spec, stateFile, stateFormat;
    std::vector<ParamSetting> params;                         // set once
    std::vector<std::pair<std::string, Envelope>> automation; // name → curve
    bool verbose = false;
    double warmup = -1;
};

struct OpenedPlugin {
    std::unique_ptr<Plugin> plugin;
    std::string id, name, format, stateFormat;
    std::vector<ParamValue> initial;
    std::vector<AutoParam> autos;
    std::vector<std::string> warnings;
};

// Resolve, create, load state, resolve and apply parameters. `context` prefixes errors.
bool openPlugin(const PluginSetup &setup, const std::string &context, OpenedPlugin &out, std::string &err);

// Note events for a track, sorted, note-offs first on ties.
std::vector<TimedEvent> scheduleNotes(const std::vector<Note> &notes, int sampleRate);

// Activate, render out.frames() frames (with a discarded pre-roll), deactivate.
// `input` (optional) feeds the plugin's first audio input port.
bool runPlugin(const Job &job, OpenedPlugin &p, const std::vector<TimedEvent> &events, const Audio *input, Audio &out,
               std::string &err);

} // namespace wl
