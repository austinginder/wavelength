#pragma once
// `wavelength kit`: a starter set of free instruments for machines without plugins (a cloud container,
// CI, a fresh laptop): GPL synthesizers downloaded from their own releases and a General MIDI SoundFont.
// Everything goes into Wavelength's folder (<settings>/kit/<name>/), never into the system's plugin
// folders; the catalog scans each entry's plugins/ folder and the preset lists read its data.
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace wl {

std::filesystem::path kitDir();
// the plugins/ folder of every installed entry (the catalog scans them after the system folders)
std::vector<std::string> kitPluginFolders();
// every entry with its status on this computer
nlohmann::json kitList();
// Installs `names` (empty: every entry available here that isn't installed yet); progress goes to
// stderr. `result` lists what was installed, skipped (and why) and any plugin that won't load.
bool kitInstall(const std::vector<std::string> &names, bool force, nlohmann::json &result, std::string &err);
bool kitRemove(const std::string &name, std::string &err);
// MuseScore General (MIT) into the SoundFont folder: `samples --install-soundfont` and the kit's "soundfont".
bool installSoundFont(bool force, std::string &path, std::string &err);

} // namespace wl
