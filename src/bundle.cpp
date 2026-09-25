#include "bundle.hpp"

#include <map>

#include "platform.hpp"

namespace wl {

namespace {
std::map<std::string, std::shared_ptr<Bundle>> &loaded() {
    static std::map<std::string, std::shared_ptr<Bundle>> m;
    return m;
}

const clap_plugin_entry_t *loadEntry(const std::string &path, std::string &err) {
    return static_cast<const clap_plugin_entry_t *>(platform::loadLibrarySymbol(path, "clap_entry", err));
}
} // namespace

std::shared_ptr<Bundle> Bundle::open(const std::string &path, std::string &err) {
    if (auto it = loaded().find(path); it != loaded().end()) return it->second;

    const clap_plugin_entry_t *entry = loadEntry(path, err);
    if (!entry) return nullptr;
    if (!clap_version_is_compatible(entry->clap_version)) {
        err = "incompatible CLAP version in " + path;
        return nullptr;
    }
    if (!entry->init(path.c_str())) {
        err = "clap_entry.init failed for " + path;
        return nullptr;
    }
    auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        err = "no plugin factory in " + path;
        return nullptr;
    }
    auto b = std::shared_ptr<Bundle>(new Bundle());
    b->path_ = path;
    b->entry_ = entry;
    b->factory_ = factory;
    loaded()[path] = b;
    return b;
}

std::vector<PluginInfo> Bundle::plugins() const {
    std::vector<PluginInfo> out;
    uint32_t n = factory_->get_plugin_count(factory_);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_plugin_descriptor_t *d = factory_->get_plugin_descriptor(factory_, i);
        if (!d || !d->id) continue;
        PluginInfo info;
        info.id = d->id;
        info.name = d->name ? d->name : "";
        info.vendor = d->vendor ? d->vendor : "";
        info.version = d->version ? d->version : "";
        info.description = d->description ? d->description : "";
        info.bundlePath = path_;
        if (d->features)
            for (const char *const *f = d->features; *f; ++f) info.features.emplace_back(*f);
        out.push_back(std::move(info));
    }
    return out;
}

} // namespace wl
