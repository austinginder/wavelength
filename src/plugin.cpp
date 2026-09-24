#include "plugin.hpp"

#include "bundle.hpp"
#include "clap_plugin.hpp"
#include "vst3_plugin.hpp"

#include <algorithm>

namespace wl {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

bool Plugin::findParam(const std::string &key, ParamInfo &out) const {
    const auto all = params();
    if (!key.empty() && key[0] == '#') {
        const ParamId want = (ParamId)std::stoul(key.substr(1));
        for (const auto &p : all) if (p.id == want) { out = p; return true; }
        return false;
    }
    const std::string k = lower(key);
    for (const auto &p : all) if (lower(p.name) == k) { out = p; return true; }
    for (const auto &p : all) if (lower(p.module + "/" + p.name) == k) { out = p; return true; }
    return false;
}

std::unique_ptr<Plugin> createPlugin(const PluginInfo &info, std::string &err) {
    if (info.format == "vst3") return Vst3Plugin::create(info, err);
    return ClapPlugin::create(info, err);
}

} // namespace wl
