#pragma once
// `wavelength compat`: a compatibility sweep over every installed plugin. Each plugin is tested in
// its own worker process (a crash or hang costs one plugin, not the sweep): it opens, lists its
// parameters and presets, renders (a chord for instruments, noise through effects), loads a few
// presets spread over its list (does each change parameters or sound?) and saves and reloads its
// state. Results are cached per plugin (bundle date + engine version), so a rerun tests only what
// changed; --rebuild tests everything again.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct CompatOptions {
    std::vector<std::string> plugins;   // specs; empty = every installed plugin
    std::string format;                 // "clap", "vst3", "vst2", "au"; empty = all
    int jobs = 1;                       // plugins tested at once (each loads one plugin)
    int presets = 3;                    // presets loaded per plugin
    int timeoutSec = 120;               // no progress for this long = hung
    bool rebuild = false, verbose = false, progress = true;
    std::string report;                 // write a Markdown report here
};

// Runs the sweep; `result` gets {"summary", "plugins": [...]} (every tested or cached plugin).
int runCompat(const CompatOptions &opt, nlohmann::json &result, std::string &err);
// The worker: tests one plugin, appending one JSON line per step to `resultsFile`.
int compatWorker(const std::string &spec, const std::string &resultsFile, int presets);
// Where results are cached
std::string compatCachePath();

} // namespace wl
