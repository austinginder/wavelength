#pragma once
// Finds CLAP and VST3 plugins on disk. Descriptors are cached (keyed by bundle path +
// modification time) in plugins.json in platform::cacheDir(). VST3 bundles are scanned
// in child processes, because loading them runs plugin code that may crash or hang.
#include "bundle.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

std::vector<std::string> clapSearchPaths();
std::vector<std::string> vst3SearchPaths();
std::vector<std::string> vst2SearchPaths();
// All plugins, from cache where possible. `rescan` ignores the cache.
std::vector<PluginInfo> scanPlugins(bool rescan, std::vector<std::string> &warnings);
// Resolve an id, a plugin name, a path to a .clap/.vst3 bundle, or "path#id". Prefix with
// "clap:" or "vst3:" to pick a format; plain names prefer CLAP when both exist.
// A blocked plugin fails to resolve unless `allowBlocked` (used by `plugins --block/--unblock`).
bool resolvePlugin(const std::string &spec, PluginInfo &out, std::string &err, bool allowBlocked = false);

// Plugins the user has blocked (blocked.json in platform::dataDir(), keyed by plugin id): an
// unlicensed plugin that opens a registration window on every load, one that crashes. A blocked
// plugin fails to resolve, so render, params, presets and audition never load it.
nlohmann::json blockedPlugins();
bool setPluginBlocked(const PluginInfo &p, bool blocked, const std::string &reason, std::string &err);

nlohmann::json pluginToJson(const PluginInfo &p);
PluginInfo pluginFromJson(const nlohmann::json &j);

} // namespace wl
