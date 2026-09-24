#pragma once
// Finds CLAP plugins on disk. Scanning means loading every bundle, which is slow, so
// descriptors are cached (keyed by bundle path + modification time) in
// ~/Library/Caches/wavelength/plugins.json.
#include "bundle.hpp"

#include <string>
#include <vector>

namespace wl {

std::vector<std::string> clapSearchPaths();
// All plugins, from cache where possible. `rescan` ignores the cache.
std::vector<PluginInfo> scanPlugins(bool rescan, std::vector<std::string> &warnings);
// Resolve "vendor.id", a plugin name, a path to a .clap bundle, or "path.clap#id".
bool resolvePlugin(const std::string &spec, PluginInfo &out, std::string &err);

} // namespace wl
