#pragma once
// The audition index: every preset of a plugin rendered once (C4 for one second) and measured,
// so an agent can choose sounds by what they sound like and knows each preset's octave offset.
// Stored in ~/Library/Caches/wavelength/audition/<plugin id>.json, keyed by preset name.
#include "bundle.hpp"
#include "presets.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wl {

// Every preset Wavelength can load for the plugin: CLAP preset discovery or VST3 programs, then
// preset files, cartridge voices, NKS and bank entries (names listed once).
std::vector<PresetInfo> listPresets(const PluginInfo &info, bool rescanNks, std::string &err);

// The audition index for a plugin ({} when it has none). Keys are preset names; values carry the
// measurements, "tags" and, when rendering failed, "error".
nlohmann::json auditionIndex(const PluginInfo &info);

// `wavelength audition <plugin>`: render the presets missing from the index in worker processes
// (one plugin instance per worker, crashes and hangs are recorded and skipped), then save.
int runAudition(const PluginInfo &info, int jobs, int limit, bool rebuild, bool verbose, std::string &err,
                std::string &summary);

// Worker entry point (`wavelength __audition <plugin> <batch.json> <results.jsonl>`).
int auditionWorker(const std::string &plugin, const std::string &batchFile, const std::string &resultsFile);

} // namespace wl
