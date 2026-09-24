#pragma once
// A loaded .clap bundle: the shared library, its clap_entry, and the plugin factory.
#include <clap/clap.h>
#include <memory>
#include <string>
#include <vector>

namespace wl {

struct PluginInfo {
    std::string id, name, vendor, version, description, bundlePath;
    std::vector<std::string> features;
};

class Bundle {
public:
    // Loads (or returns the already-loaded) bundle at `path`. Bundles stay loaded for the
    // life of the process: unloading plugin libraries is a common source of crashes.
    static std::shared_ptr<Bundle> open(const std::string &path, std::string &err);

    const clap_plugin_factory_t *factory() const { return factory_; }
    const std::string &path() const { return path_; }
    std::vector<PluginInfo> plugins() const;

private:
    std::string path_;
    const clap_plugin_entry_t *entry_ = nullptr;
    const clap_plugin_factory_t *factory_ = nullptr;
};

} // namespace wl
