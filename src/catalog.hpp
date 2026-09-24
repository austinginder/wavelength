#pragma once
// Finds CLAP and VST3 plugins on disk. Descriptors are cached (keyed by bundle path +
// modification time) in ~/Library/Caches/wavelength/plugins.json. VST3 bundles are scanned
// in child processes, because loading them runs plugin code that may crash or hang.
#include "bundle.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

std::vector<std::string> clapSearchPaths();
std::vector<std::string> vst3SearchPaths();
// All plugins, from cache where possible. `rescan` ignores the cache.
std::vector<PluginInfo> scanPlugins(bool rescan, std::vector<std::string> &warnings);
// Resolve an id, a plugin name, a path to a .clap/.vst3 bundle, or "path#id". Prefix with
// "clap:" or "vst3:" to pick a format; plain names prefer CLAP when both exist.
bool resolvePlugin(const std::string &spec, PluginInfo &out, std::string &err);

nlohmann::json pluginToJson(const PluginInfo &p);
PluginInfo pluginFromJson(const nlohmann::json &j);

} // namespace wl
