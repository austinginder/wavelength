#include "catalog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace wl {

namespace {

std::string home() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}

fs::path cacheFile() { return fs::path(home()) / "Library/Caches/wavelength/plugins.json"; }

long long mtimeOf(const fs::path &p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

json toJson(const PluginInfo &p) {
    return {{"id", p.id}, {"name", p.name}, {"vendor", p.vendor}, {"version", p.version},
            {"description", p.description}, {"bundle", p.bundlePath}, {"features", p.features}};
}
PluginInfo fromJson(const json &j) {
    PluginInfo p;
    p.id = j.value("id", "");
    p.name = j.value("name", "");
    p.vendor = j.value("vendor", "");
    p.version = j.value("version", "");
    p.description = j.value("description", "");
    p.bundlePath = j.value("bundle", "");
    p.features = j.value("features", std::vector<std::string>{});
    return p;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

std::vector<std::string> clapSearchPaths() {
    std::vector<std::string> paths;
    if (const char *extra = std::getenv("WAVELENGTH_CLAP_PATH")) {
        std::stringstream ss(extra);
        std::string item;
        while (std::getline(ss, item, ':')) if (!item.empty()) paths.push_back(item);
    }
#ifdef __APPLE__
    paths.push_back(home() + "/Library/Audio/Plug-Ins/CLAP");
    paths.push_back("/Library/Audio/Plug-Ins/CLAP");
#else
    paths.push_back(home() + "/.clap");
    paths.push_back("/usr/lib/clap");
#endif
    return paths;
}

std::vector<PluginInfo> scanPlugins(bool rescan, std::vector<std::string> &warnings) {
    json cache = json::object();
    if (!rescan) {
        std::ifstream in(cacheFile());
        if (in) { try { in >> cache; } catch (...) { cache = json::object(); } }
    }
    json fresh = {{"version", 1}, {"bundles", json::object()}};
    std::vector<PluginInfo> all;

    for (const auto &dir : clapSearchPaths()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> bundles;
        // bundles are directories: collect them without descending into their contents
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().extension() == ".clap") { bundles.push_back(it->path()); it.disable_recursion_pending(); }
        }
        std::sort(bundles.begin(), bundles.end());
        for (const auto &b : bundles) {
            std::string key = b.string();
            long long mt = mtimeOf(b);
            std::vector<PluginInfo> plugins;
            const json *cached = cache.contains("bundles") && cache["bundles"].contains(key) ? &cache["bundles"][key] : nullptr;
            if (cached && cached->value("mtime", 0LL) == mt) {
                for (auto &p : (*cached)["plugins"]) plugins.push_back(fromJson(p));
            } else {
                std::string err;
                auto bundle = Bundle::open(key, err);
                if (!bundle) { warnings.push_back(err); continue; }
                plugins = bundle->plugins();
            }
            json pj = json::array();
            for (auto &p : plugins) { pj.push_back(toJson(p)); all.push_back(p); }
            fresh["bundles"][key] = {{"mtime", mt}, {"plugins", pj}};
        }
    }
    std::error_code ec;
    fs::create_directories(cacheFile().parent_path(), ec);
    std::ofstream out(cacheFile());
    if (out) out << fresh.dump(2);
    return all;
}

bool resolvePlugin(const std::string &spec, PluginInfo &out, std::string &err) {
    // explicit bundle path, optionally "#plugin.id"
    std::string path = spec, wantId;
    if (auto hash = spec.find('#'); hash != std::string::npos) { path = spec.substr(0, hash); wantId = spec.substr(hash + 1); }
    if (path.size() > 5 && path.substr(path.size() - 5) == ".clap") {
        auto bundle = Bundle::open(fs::absolute(path).string(), err);
        if (!bundle) return false;
        auto plugins = bundle->plugins();
        for (auto &p : plugins)
            if (wantId.empty() || p.id == wantId) { out = p; return true; }
        err = "plugin '" + wantId + "' not found in " + path;
        return false;
    }
    std::vector<std::string> warnings;
    auto all = scanPlugins(false, warnings);
    for (auto &p : all) if (p.id == spec) { out = p; return true; }
    for (auto &p : all) if (lower(p.id) == lower(spec)) { out = p; return true; }
    for (auto &p : all) if (lower(p.name) == lower(spec)) { out = p; return true; }
    err = "no installed CLAP plugin matches '" + spec + "' (run `wavelength plugins`)";
    return false;
}

} // namespace wl
