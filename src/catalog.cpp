#include "catalog.hpp"

#include "au_plugin.hpp"

#include "kit.hpp"
#include "platform.hpp"

#include "vst3_plugin.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace wl {

namespace {

#ifndef _WIN32
std::string home() { return platform::homeDir().string(); }
#endif

fs::path cacheFile() { return platform::cacheDir() / "plugins.json"; }

long long mtimeOf(const fs::path &p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::vector<std::string> envPaths(const char *var) { return platform::envPathList(var); }

// bundles are directories: collect them without descending into their contents (nor into
// another format's bundles: a .vst3 left in the VST folder is found, a .vst bundle's insides are skipped)
std::vector<fs::path> findBundles(const std::string &dir, const char *ext) {
    std::vector<fs::path> bundles;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return bundles;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string e = it->path().extension().string();
        if (e == ext) { bundles.push_back(it->path()); it.disable_recursion_pending(); }
        else if (e == ".vst" || e == ".vst3" || e == ".clap" || e == ".component") it.disable_recursion_pending();
    }
    std::sort(bundles.begin(), bundles.end());
    return bundles;
}

// Scan one VST3 bundle in a child process (`wavelength __scan-vst3 <bundle>`), so a plugin
// that crashes or hangs while loading can't take the scan down with it.
// The architecture a bundle must run as, when it has no code for this process's ("" = native).
std::string foreignArch(const std::string &bundle) {
    const auto archs = platform::binaryArchs(bundle);
    if (archs.empty() || std::find(archs.begin(), archs.end(), platform::hostArch()) != archs.end()) return "";
    return std::find(archs.begin(), archs.end(), "x86_64") != archs.end() ? "x86_64" : archs.front();
}

bool scanInChild(const char *command, const std::string &bundle, std::vector<PluginInfo> &out, std::string &err, int timeoutSec = 45) {
    // an Intel-only bundle is scanned (and later rendered) by this executable under Rosetta
    const std::string arch = foreignArch(bundle);
    std::vector<std::string> args;
    if (!platform::archPrefix(arch, args, err)) { err = bundle + ": " + err; return false; }
    for (const std::string &a : {platform::selfExecutable(), std::string(command), bundle}) args.push_back(a);
    platform::Process proc;
    if (!platform::spawn(args, proc, true, true)) {
        err = "could not start the scanner";
        return false;
    }
    std::string output, crash;
    const bool timedOut = !platform::readOutput(proc, output, timeoutSec);
    if (timedOut) platform::kill(proc);
    while (!platform::finished(proc, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (timedOut) { err = "timed out loading " + bundle; return false; }
    if (!crash.empty()) { err = "crashed while loading " + bundle + " (" + crash + ")"; return false; }
    if (output.find_first_not_of(" \t\r\n") == std::string::npos) {   // e.g. V-Pan calls exit() while its module loads
        err = "the plugin ended the scan process without a result while loading " + bundle + " (it may need its installer, a licence or an authorisation)";
        return false;
    }
    try {
        const json j = json::parse(output);
        if (!j.value("ok", false)) { err = j.value("error", "scan failed"); return false; }
        for (const auto &p : j["plugins"]) { out.push_back(pluginFromJson(p)); out.back().arch = arch; }
    } catch (...) {
        err = "unreadable scan output for " + bundle;
        return false;
    }
    return true;
}

} // namespace

json pluginToJson(const PluginInfo &p) {
    json j = {{"id", p.id}, {"name", p.name}, {"vendor", p.vendor}, {"version", p.version}, {"format", p.format},
              {"description", p.description}, {"bundle", p.bundlePath}, {"features", p.features}};
    if (!p.arch.empty()) j["arch"] = p.arch;
    return j;
}
PluginInfo pluginFromJson(const json &j) {
    PluginInfo p;
    p.id = j.value("id", "");
    p.name = j.value("name", "");
    p.vendor = j.value("vendor", "");
    p.version = j.value("version", "");
    p.format = j.value("format", "clap");
    p.description = j.value("description", "");
    p.bundlePath = j.value("bundle", "");
    p.features = j.value("features", std::vector<std::string>{});
    p.arch = j.value("arch", "");
    return p;
}

std::vector<std::string> clapSearchPaths() {
    auto paths = envPaths("WAVELENGTH_CLAP_PATH");
#ifdef __APPLE__
    paths.push_back(home() + "/Library/Audio/Plug-Ins/CLAP");
    paths.push_back("/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
    const char *common = std::getenv("COMMONPROGRAMFILES"), *local = std::getenv("LOCALAPPDATA");
    if (local) paths.push_back(std::string(local) + "\\Programs\\Common\\CLAP");
    if (common) paths.push_back(std::string(common) + "\\CLAP");
#else
    paths.push_back(home() + "/.clap");
    paths.push_back("/usr/local/lib/clap");
    paths.push_back("/usr/lib/clap");
#endif
    for (auto &k : kitPluginFolders()) paths.push_back(k);   // `wavelength kit`, after the system's own
    return paths;
}

std::vector<std::string> vst3SearchPaths() {
    auto paths = envPaths("WAVELENGTH_VST3_PATH");
#ifdef __APPLE__
    paths.push_back(home() + "/Library/Audio/Plug-Ins/VST3");
    paths.push_back("/Library/Audio/Plug-Ins/VST3");
#elif defined(_WIN32)
    const char *common = std::getenv("COMMONPROGRAMFILES"), *local = std::getenv("LOCALAPPDATA");
    if (local) paths.push_back(std::string(local) + "\\Programs\\Common\\VST3");
    if (common) paths.push_back(std::string(common) + "\\VST3");
#else
    paths.push_back(home() + "/.vst3");
    paths.push_back("/usr/local/lib/vst3");
    paths.push_back("/usr/lib/vst3");
#endif
    for (auto &k : kitPluginFolders()) paths.push_back(k);
    return paths;
}

std::vector<std::string> vst2SearchPaths() {
    auto paths = envPaths("WAVELENGTH_VST2_PATH");
#ifdef __APPLE__
    paths.push_back(home() + "/Library/Audio/Plug-Ins/VST");
    paths.push_back("/Library/Audio/Plug-Ins/VST");
#elif defined(_WIN32)
    const char *pf = std::getenv("PROGRAMFILES"), *common = std::getenv("COMMONPROGRAMFILES");
    if (pf) { paths.push_back(std::string(pf) + "\\VSTPlugins"); paths.push_back(std::string(pf) + "\\Steinberg\\VSTPlugins"); }
    if (common) paths.push_back(std::string(common) + "\\VST2");
#else
    paths.push_back(home() + "/.vst");
    paths.push_back("/usr/local/lib/vst");
    paths.push_back("/usr/lib/vst");
#endif
    return paths;
}

// The cache as another process last wrote it. Writes are atomic (a rename), so a file that won't
// parse is damaged or from an old build: try again briefly before giving up on it, because giving
// up means scanning every plugin.
json readCatalogCache() {
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::ifstream in(cacheFile());
        if (!in) return json::object();
        json j = json::parse(in, nullptr, false);
        if (j.is_object()) return j;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return json::object();
}

std::vector<PluginInfo> scanPlugins(bool rescan, std::vector<std::string> &warnings) {
    // every bundle on disk first, so the cache can be checked before anything is loaded
    std::vector<std::pair<std::string, long long>> clapBundles, childBundles;
    for (const auto &dir : clapSearchPaths())
        for (const auto &b : findBundles(dir, ".clap")) clapBundles.push_back({b.string(), mtimeOf(b)});
    // VST3 (and VST 2): loading a module runs plugin code, so unknown bundles are scanned in child processes
    std::vector<fs::path> childScanned;
    for (const auto &dir : vst3SearchPaths()) for (const auto &b : findBundles(dir, ".vst3")) childScanned.push_back(b);
    // VST 2: bundles on macOS, libraries elsewhere; scanned in child processes like VST3
#if defined(__APPLE__)
    const char *vst2Ext = ".vst";
#elif defined(_WIN32)
    const char *vst2Ext = ".dll";
#else
    const char *vst2Ext = ".so";
#endif
    for (const auto &dir : vst2SearchPaths()) for (const auto &b : findBundles(dir, vst2Ext)) childScanned.push_back(b);
    // VST3 bundles installed into a VST 2 folder by mistake (installers and people do it) still load
    for (const auto &dir : vst2SearchPaths()) for (const auto &b : findBundles(dir, ".vst3")) childScanned.push_back(b);
    std::sort(childScanned.begin(), childScanned.end());
    childScanned.erase(std::unique(childScanned.begin(), childScanned.end()), childScanned.end());
    for (const auto &b : childScanned) childBundles.push_back({b.string(), mtimeOf(b)});

    json cache = rescan ? json::object() : readCatalogCache();
    auto cached = [&](const std::string &key, long long mt) -> const json * {
        if (cache.value("version", 0) != 2 || !cache.contains("bundles") || !cache["bundles"].contains(key)) return nullptr;
        const json &c = cache["bundles"][key];
        return c.value("mtime", 0LL) == mt ? &c : nullptr;
    };
    auto anyMissing = [&] {
        for (const auto *list : {&clapBundles, &childBundles})
            for (const auto &[key, mt] : *list) if (!cached(key, mt)) return true;
        return false;
    };

    // Scanning is serialized across processes: many Wavelength commands started together (agents,
    // parallel renders) would otherwise each scan every plugin at once, 6 child processes apiece.
    // The one that gets the lock scans; the others wait and then find its results in the cache.
    platform::FileLock lock;
    if (rescan || anyMissing()) {
        std::string lockErr;
        if (!lock.acquire(platform::cacheDir() / "plugins.lock", 900, lockErr))
            warnings.push_back(lockErr + "; scanning without the lock");
        else if (!rescan) cache = readCatalogCache();
    }

    json fresh = {{"version", 2}, {"bundles", json::object()}};
    std::vector<PluginInfo> all;
    auto record = [&](const std::string &key, long long mt, const std::vector<PluginInfo> &plugins, const std::string &error) {
        json pj = json::array();
        for (const auto &p : plugins) { pj.push_back(pluginToJson(p)); all.push_back(p); }
        fresh["bundles"][key] = {{"mtime", mt}, {"plugins", pj}};
        if (!error.empty()) fresh["bundles"][key]["error"] = error;
    };

    // CLAP: loading a CLAP bundle only reads its descriptors, so scan in-process
    for (const auto &[key, mt] : clapBundles) {
        std::vector<PluginInfo> plugins;
        std::string error;
        if (const json *c = cached(key, mt)) {
            for (auto &p : (*c)["plugins"]) plugins.push_back(pluginFromJson(p));
            error = c->value("error", "");
        } else {
            auto bundle = Bundle::open(key, error);
            if (bundle) plugins = bundle->plugins();
            else warnings.push_back(error);
        }
        record(key, mt, plugins, error);
    }

    // child-scanned bundles: a few at a time, the result (including failures) is cached
    std::vector<std::pair<std::string, long long>> todo;
    for (const auto &[key, mt] : childBundles) {
        if (const json *c = cached(key, mt)) {
            std::vector<PluginInfo> plugins;
            for (auto &p : (*c)["plugins"]) plugins.push_back(pluginFromJson(p));
            record(key, mt, plugins, c->value("error", ""));
        } else todo.push_back({key, mt});
    }
    if (!todo.empty()) {
        std::mutex m;
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        for (int w = 0; w < 6; ++w)
            pool.emplace_back([&] {
                for (size_t i; (i = next++) < todo.size();) {
                    std::vector<PluginInfo> plugins;
                    std::string error;
                    if (!scanInChild(todo[i].first.size() > 5 && todo[i].first.substr(todo[i].first.size() - 5) == ".vst3" ? "__scan-vst3" : "__scan-vst2", todo[i].first, plugins, error)) plugins.clear();
                    std::lock_guard<std::mutex> lock(m);
                    if (!error.empty()) warnings.push_back(error);
                    record(todo[i].first, todo[i].second, plugins, error);
                }
            });
        for (auto &t : pool) t.join();
    }

    // Audio Units: the system's component registry lists them without running plugin code, so they
    // are read fresh every time (nothing to cache)
    for (auto &p : auPlugins()) all.push_back(p);
    std::sort(all.begin(), all.end(), [](const PluginInfo &a, const PluginInfo &b) {
        return lower(a.name) != lower(b.name) ? lower(a.name) < lower(b.name) : a.format < b.format;
    });
    // keep bundles another Wavelength build found and this one doesn't look for, while they exist,
    // so two versions sharing the cache don't rescan each other's bundles
    if (cache.value("version", 0) == 2 && cache.contains("bundles")) {
        std::error_code ec;
        for (const auto &[key, entry] : cache["bundles"].items())
            if (!fresh["bundles"].contains(key) && fs::exists(key, ec)) fresh["bundles"][key] = entry;
    }
    if (fresh != cache) {
        std::string err;
        if (!platform::writeFileAtomic(cacheFile(), fresh.dump(2), err)) warnings.push_back(err);
    }
    return all;
}

namespace {
fs::path blockedFile() { return platform::dataDir() / "blocked.json"; }
bool resolveAny(const std::string &specIn, PluginInfo &out, std::string &err);
} // namespace

nlohmann::json blockedPlugins() {
    std::ifstream in(blockedFile());
    nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json::object();
    return j.is_object() ? j : nlohmann::json::object();
}

bool setPluginBlocked(const PluginInfo &p, bool blocked, const std::string &reason, std::string &err) {
    nlohmann::json j = blockedPlugins();
    if (blocked) j[p.id] = {{"name", p.name}, {"format", p.format}, {"reason", reason}};
    else j.erase(p.id);
    std::error_code ec;
    fs::create_directories(blockedFile().parent_path(), ec);
    std::ofstream out(blockedFile());
    out << j.dump(2) << "\n";
    if (!out) { err = "cannot write " + blockedFile().string(); return false; }
    return true;
}

bool resolvePlugin(const std::string &spec, PluginInfo &out, std::string &err, bool allowBlocked) {
    if (!resolveAny(spec, out, err)) return false;
    if (allowBlocked) return true;
    const nlohmann::json blocked = blockedPlugins();
    if (!blocked.contains(out.id)) return true;
    const std::string reason = blocked[out.id].value("reason", "");
    err = out.name + " is blocked" + (reason.empty() ? "" : " (" + reason + ")") + ": pick another plugin, or run `wavelength plugins --unblock \"" +
          out.name + "\"` if it works now";
    return false;
}

namespace {
bool resolveAny(const std::string &specIn, PluginInfo &out, std::string &err) {
    // optional format prefix: "vst3:Vital", "clap:Vital", "vst2:Reaktor 6", "au:DLSMusicDevice"
    std::string spec = specIn, want;
    for (const char *f : {"vst3", "vst2", "clap", "au"})
        if (spec.rfind(std::string(f) + ":", 0) == 0) { want = f; spec = spec.substr(std::string(f).size() + 1); }

    // explicit bundle path, optionally "#plugin id"
    std::string path = spec, wantId;
    if (auto hash = spec.find('#'); hash != std::string::npos) { path = spec.substr(0, hash); wantId = spec.substr(hash + 1); }
    const bool isClap = path.size() > 5 && path.substr(path.size() - 5) == ".clap";
    const bool isVst3 = path.size() > 5 && path.substr(path.size() - 5) == ".vst3";
    const bool isVst2 = path.size() > 4 && path.substr(path.size() - 4) == ".vst";
    if (isClap || isVst3 || isVst2) {
        std::vector<PluginInfo> plugins;
        const std::string abs = fs::absolute(path).string();
        if (isClap) {
            auto bundle = Bundle::open(abs, err);
            if (!bundle) return false;
            plugins = bundle->plugins();
        } else if (isVst2) {
            if (!scanInChild("__scan-vst2", abs, plugins, err)) return false;
        } else if (!scanVst3Bundle(abs, plugins, err)) return false;
        for (auto &p : plugins)
            if (wantId.empty() || lower(p.id) == lower(wantId)) { out = p; return true; }
        err = "plugin '" + wantId + "' not found in " + path;
        return false;
    }

    std::vector<std::string> warnings;
    auto all = scanPlugins(false, warnings);
    // a name that exists in several formats resolves to CLAP, then VST3, then VST2, then Audio Unit, unless a prefix says otherwise
    auto rank = [](const PluginInfo &p) { return p.format == "clap" ? 0 : p.format == "vst3" ? 1 : p.format == "vst2" ? 2 : 3; };
    std::stable_sort(all.begin(), all.end(), [&](const PluginInfo &a, const PluginInfo &b) { return rank(a) < rank(b); });
    auto ok = [&](const PluginInfo &p) { return want.empty() || p.format == want; };
    for (auto &p : all) if (ok(p) && p.id == spec) { out = p; return true; }
    for (auto &p : all) if (ok(p) && lower(p.id) == lower(spec)) { out = p; return true; }
    for (auto &p : all) if (ok(p) && lower(p.name) == lower(spec)) { out = p; return true; }
    err = "no installed " + (want.empty() ? std::string("CLAP, VST3, VST2 or Audio Unit") : want) + " plugin matches '" + spec + "' (run `wavelength plugins`)";
    return false;
}
} // namespace

} // namespace wl
