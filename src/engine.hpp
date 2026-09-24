#pragma once
// Running CLAP plugins over a whole timeline: instruments (notes in, audio out) and
// effects (audio in, audio out), with parameter automation.
#include "automation.hpp"
#include "instance.hpp"
#include "job.hpp"
#include "wav.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wl {

struct TimedEvent {
    int64_t frame;
    bool on;
    int key, channel;
    double velocity;
};

struct AutoParam {
    clap_id id;
    void *cookie;
    std::string name;
    Envelope env;   // plain values over seconds
};

struct PluginSetup {
    std::string spec, stateFile, stateFormat;
    std::vector<ParamSetting> params;                         // set once
    std::vector<std::pair<std::string, Envelope>> automation; // name → curve
    bool verbose = false;
};

struct OpenedPlugin {
    std::unique_ptr<Instance> inst;
    std::string id, name, stateFormat;
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
