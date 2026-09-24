#pragma once
// A hosted plugin, independent of its format (CLAP or VST3). The engine, effects and CLI
// talk to this interface only; clap_plugin.* and vst3_plugin.* implement it.
#include "automation.hpp"
#include "job.hpp"
#include "state_file.hpp"
#include "wav.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wl {

using ParamId = uint32_t;

struct ParamInfo {
    ParamId id;
    void *cookie;          // CLAP: must be passed back in every event for this parameter
    std::string name, module;
    double min, max, def, value;   // plain values in the plugin's own range
    bool stepped, readonly, hidden;
    std::string display;   // plugin's own text for the current value
};

struct ParamValue {
    ParamId id;
    void *cookie;
    double value;          // plain
};

struct TimedEvent {
    int64_t frame;
    bool on;
    int key, channel;
    double velocity;
};

struct AutoParam {
    ParamId id;
    void *cookie;
    std::string name;
    Envelope env;          // plain values over seconds
};

struct PluginInfo;

class Plugin {
public:
    virtual ~Plugin() = default;

    virtual const std::string &id() const = 0;
    virtual const std::string &name() const = 0;
    virtual const char *format() const = 0;            // "clap" or "vst3"

    // State: `sf` is a parsed state file (see state_file.hpp); each format accepts the
    // containers that make sense for it and explains the rest.
    virtual bool loadState(const StateFile &sf, std::string &err) = 0;
    // Save the current state as this format's preset file (.clap-preset / .vstpreset).
    virtual bool saveStateFile(const std::string &path, size_t &bytes, std::string &err) = 0;

    virtual std::vector<ParamInfo> params() const = 0;
    bool findParam(const std::string &key, ParamInfo &out) const;   // "#id", name, "Module/Name"
    // Apply plain values on the main thread (plugin inactive).
    virtual bool setParams(const std::vector<ParamValue> &values, std::string &err) = 0;
    // Run a few silent blocks so plugins that only commit through process() take the values.
    virtual bool commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) = 0;

    // Render the whole timeline into `out` (already sized). `input` feeds effects.
    virtual bool render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                        const std::vector<AutoParam> &autos, const Audio *input, Audio &out,
                        std::vector<std::string> &warnings, std::string &err) = 0;

    // Service the main thread (run loop, plugin callbacks) for `ms` milliseconds.
    virtual void pump(double ms) = 0;

    bool verbose = false;
    double warmup = -1;    // seconds to settle after activation; < 0 = the job's "warmup"
};

// Creates the right implementation for a resolved plugin.
std::unique_ptr<Plugin> createPlugin(const PluginInfo &info, std::string &err);

} // namespace wl
