#pragma once
// Running plugins (CLAP or VST3) over a whole timeline: instruments (notes in, audio out) and
// effects (audio in, audio out), with parameter automation.
#include "automation.hpp"
#include "plugin.hpp"
#include "state_file.hpp"
#include "job.hpp"
#include "wav.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wl {

struct PluginSetup {
    std::string spec, stateFile, stateFormat, preset;
    std::vector<ParamSetting> params;                         // set once
    std::vector<std::pair<std::string, Envelope>> automation; // name → curve
    bool verbose = false;
    double warmup = -1;
};

struct OpenedPlugin {
    std::unique_ptr<Plugin> plugin;
    std::string id, name, format, stateFormat, preset;
    std::vector<ParamValue> initial;
    std::vector<AutoParam> autos;
    std::vector<std::string> warnings;
};

// Load a state file into a plugin, running the file's transform on the plugin's current state
// first when the format needs it (e.g. a DX7 voice patched into Dexed's state).
bool loadStateInto(Plugin &plugin, StateFile &sf, std::string &err);

// A preset by name: the plugin's own library (CLAP preset discovery, VST3 program list), then
// preset files for it (preset folders, DX7 cartridges, NKS).
bool loadPresetByName(Plugin &plugin, const PluginInfo &info, const std::string &query, std::string &loadedName,
                      std::string &stateFormat, std::string &err);

// Resolve, create, load state, resolve and apply parameters. `context` prefixes errors.
bool openPlugin(const PluginSetup &setup, const std::string &context, OpenedPlugin &out, std::string &err);

// Note events for a track, sorted, note-offs first on ties.
std::vector<TimedEvent> scheduleNotes(const std::vector<Note> &notes, int sampleRate);
// Adds the track's MIDI CC / pitch bend / pressure automation as events (sampled every 64
// frames, only when the quantized value changes) and re-sorts.
void scheduleControllers(const Track &track, int sampleRate, double seconds, std::vector<TimedEvent> &events);

// Activate, render out.frames() frames (with a discarded pre-roll), deactivate.
// `input` (optional) feeds the plugin's first audio input port.
bool runPlugin(const Job &job, OpenedPlugin &p, const std::vector<TimedEvent> &events, const Audio *input, Audio &out,
               std::string &err);

} // namespace wl
