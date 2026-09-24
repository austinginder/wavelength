#pragma once
// Preset files on disk for a plugin, found in the standard preset folders
// (/Library/Audio/Presets/<vendor>/<plugin>, ~/Library/Audio/Presets/...) and a few known vendor
// locations, in any state format Wavelength reads (.vstpreset, .fxp, .SerumPreset, .odin, .h2p,
// .vital, .nksf). This gives "preset by name" to plugins without CLAP preset discovery or a VST3
// program list: Serum 2, Odin2, Zebra2, Surge XT, OB-Xf, ...
#include "catalog.hpp"
#include "presets.hpp"

#include <string>
#include <vector>

namespace wl {

// PresetInfo.location = the file; category = its folder; kind = file.
std::vector<PresetInfo> filePresets(const PluginInfo &plugin);

} // namespace wl
