#pragma once
// CLAP preset discovery: lists a plugin's presets (its built-in banks and preset files)
// through the plugin's preset-discovery factory, and resolves a name to a loadable preset.
#include <cstdint>
#include <string>
#include <vector>

namespace wl {

struct PresetInfo {
    std::string name, category, loadKey, location, description, creator;
    uint32_t kind = 0;                     // CLAP_PRESET_DISCOVERY_LOCATION_FILE (0) or _PLUGIN (1)
    std::vector<std::string> features;
    bool stateFile = false;                // `location` loads as a Wavelength state file (not via the plugin's preset loader)
};

// All presets the plugin's providers report for `pluginId`.
bool discoverPresets(const std::string &bundlePath, const std::string &pluginId, std::vector<PresetInfo> &out, std::string &err);

// Match `query` against names: exact, case-insensitive, "Category/Name", load key, then a
// unique substring. On failure `err` lists close matches.
bool findPreset(const std::vector<PresetInfo> &presets, const std::string &query, PresetInfo &out, std::string &err);

} // namespace wl
