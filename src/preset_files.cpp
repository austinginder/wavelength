#include "preset_files.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace wl {

namespace {

std::string squash(std::string s) {   // "Serum 2" == "serum2", "Odin2" == "odin2"
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

const std::set<std::string> kExtensions = {".vstpreset", ".fxp", ".fxb", ".serumpreset", ".odin", ".h2p", ".vital", ".nksf"};

// a child folder of `dir` whose squashed name is one of `names`
std::vector<fs::path> childrenNamed(const fs::path &dir, const std::vector<std::string> &names) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string n = squash(e.path().filename().string());
        if (std::find(names.begin(), names.end(), n) != names.end()) out.push_back(e.path());
    }
    return out;
}

// a preset folder can hold other plugins' files (a Diva .h2p saved among Serum presets): keep a
// file only when its format can belong to this plugin
bool belongsTo(const fs::path &file, const std::string &ext, const PluginInfo &plugin) {
    const std::string p = squash(plugin.name);
    if (ext == ".serumpreset") return p.find("serum") != std::string::npos;
    if (ext == ".odin") return p.find("odin") != std::string::npos;
    if (ext != ".h2p" && ext != ".vstpreset") return true;
    std::ifstream in(file, std::ios::binary);
    std::string head(4096, '\0');
    in.read(&head[0], (std::streamsize)head.size());
    head.resize((size_t)in.gcount());
    if (ext == ".vstpreset")   // "VST3" + version + the 32-character class id of the plugin it belongs to
        return head.size() >= 40 && squash(head.substr(8, 32)) == squash(plugin.id);
    const size_t am = head.find("#AM=");   // u-he: the plugin the preset was saved by
    if (am == std::string::npos) return true;
    const size_t end = head.find_first_of("\r\n", am);
    return squash(head.substr(am + 4, end - am - 4)) == p;
}

} // namespace

std::vector<PresetInfo> filePresets(const PluginInfo &plugin) {
    const std::string home = getenv("HOME") ? getenv("HOME") : "";
    const std::string p = squash(plugin.name);
    const std::vector<std::string> names = {p, p + "presets", p + "patches"};
    std::vector<fs::path> dirs;
    // the VST3 convention: <Presets>/<Vendor>/<Plugin>/, plus <Presets>/<Plugin>/
    for (const auto &root : {fs::path("/Library/Audio/Presets"), fs::path(home) / "Library/Audio/Presets"}) {
        for (auto &d : childrenNamed(root, names)) dirs.push_back(d);
        std::error_code ec;
        for (auto &vendor : fs::directory_iterator(root, ec))
            if (vendor.is_directory(ec)) for (auto &d : childrenNamed(vendor.path(), names)) dirs.push_back(d);
    }
    // vendors that keep patches elsewhere
    if (p == "surgext")
        for (auto d : {"/Library/Application Support/Surge XT/patches_factory", "/Library/Application Support/Surge XT/patches_3rdparty"})
            dirs.push_back(d);
    if (p == "surgext") dirs.push_back(fs::path(home) / "Documents/Surge XT/Patches");
    if (p == "obxf") {
        dirs.push_back("/Library/Application Support/Surge Synth Team/OB-Xf/Patches");
        dirs.push_back(fs::path(home) / "Documents/Surge Synth Team/OB-Xf/Patches");
    }

    std::vector<PresetInfo> out;
    std::set<std::string> seen;
    for (const auto &dir : dirs) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (!kExtensions.count(ext) || !belongsTo(it->path(), ext, plugin)) continue;
            const std::string file = it->path().string();
            if (!seen.insert(file).second) continue;
            PresetInfo pi;
            pi.name = it->path().stem().string();
            pi.category = it->path().parent_path().filename().string();
            pi.location = file;
            pi.loadKey = file;
            pi.kind = 0;   // CLAP_PRESET_DISCOVERY_LOCATION_FILE
            out.push_back(pi);
        }
    }
    std::sort(out.begin(), out.end(), [](const PresetInfo &a, const PresetInfo &b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
    return out;
}

} // namespace wl
