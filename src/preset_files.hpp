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

// PresetInfo.location = the file; category = its folder; kind = file. Includes Dexed's DX7
// cartridge voices ("<cart>.syx#<n>") and NKS presets (.nksf) that name this plugin.
std::vector<PresetInfo> filePresets(const PluginInfo &plugin);

// NKS presets for the plugin from the NKS index (~/Library/Caches/wavelength/nks.json, built by
// walking the usual NKS folders); `rescan` rebuilds it.
std::vector<PresetInfo> nksPresets(const PluginInfo &plugin, bool rescan);

} // namespace wl
