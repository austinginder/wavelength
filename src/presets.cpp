#include "presets.hpp"

#include "bundle.hpp"

#include <clap/clap.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace wl {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

struct Location { uint32_t kind; std::string name, path; };

struct IndexerState {
    std::vector<Location> locations;
    std::vector<std::string> extensions;
};

struct ReceiverState {
    std::vector<PresetInfo> *out;
    std::string pluginId, location;
    uint32_t kind;
    std::vector<std::string> ids;   // plugin ids of the current preset
    bool open = false;

    void flush() {
        if (!open) return;
        open = false;
        auto &p = out->back();
        const bool matches = ids.empty() || std::find(ids.begin(), ids.end(), pluginId) != ids.end();
        if (!matches) out->pop_back();
        (void)p;
    }
};

ReceiverState *rs(const clap_preset_discovery_metadata_receiver_t *r) { return static_cast<ReceiverState *>(r->receiver_data); }

// category from the load key or file path: "patches/Factory Basses/BA x.synthpatch" -> "Factory Basses"
std::string categoryOf(const std::string &key) {
    const fs::path p(key);
    const std::string parent = p.parent_path().filename().string();
    return parent;
}

void collectFiles(const std::string &dir, const std::vector<std::string> &exts, std::vector<std::string> &files) {
    std::error_code ec;
    if (fs::is_regular_file(dir, ec)) { files.push_back(dir); return; }
    if (!fs::is_directory(dir, ec)) return;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        const bool ok = exts.empty() || std::any_of(exts.begin(), exts.end(), [&](const std::string &e) {
            return lower(ext) == lower(e.empty() || e[0] == '.' ? e : "." + e);
        });
        if (ok) files.push_back(it->path().string());
    }
    std::sort(files.begin(), files.end());
}

} // namespace

bool discoverPresets(const std::string &bundlePath, const std::string &pluginId, std::vector<PresetInfo> &out, std::string &err) {
    auto bundle = Bundle::open(bundlePath, err);
    if (!bundle) return false;
    auto *factory = static_cast<const clap_preset_discovery_factory_t *>(bundle->getFactory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory) factory = static_cast<const clap_preset_discovery_factory_t *>(bundle->getFactory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
    if (!factory) { err = "this plugin does not publish presets through CLAP preset discovery"; return false; }

    for (uint32_t i = 0, n = factory->count(factory); i < n; ++i) {
        const auto *desc = factory->get_descriptor(factory, i);
        if (!desc) continue;
        IndexerState state;
        clap_preset_discovery_indexer_t indexer{
            CLAP_VERSION, "Wavelength", "wavelength.run", "https://wavelength.run", WAVELENGTH_VERSION, &state,
            [](const clap_preset_discovery_indexer_t *ix, const clap_preset_discovery_filetype_t *ft) {
                if (ft && ft->file_extension) static_cast<IndexerState *>(ix->indexer_data)->extensions.push_back(ft->file_extension);
                return true;
            },
            [](const clap_preset_discovery_indexer_t *ix, const clap_preset_discovery_location_t *loc) {
                if (loc) static_cast<IndexerState *>(ix->indexer_data)->locations.push_back(
                    {loc->kind, loc->name ? loc->name : "", loc->location ? loc->location : ""});
                return true;
            },
            [](const clap_preset_discovery_indexer_t *, const clap_preset_discovery_soundpack_t *) { return true; },
            [](const clap_preset_discovery_indexer_t *, const char *) -> const void * { return nullptr; },
        };
        const auto *provider = factory->create(factory, &indexer, desc->id);
        if (!provider) continue;
        if (!provider->init(provider)) { provider->destroy(provider); continue; }

        ReceiverState r{&out, pluginId, "", 0, {}, false};
        clap_preset_discovery_metadata_receiver_t receiver{
            &r,
            [](const clap_preset_discovery_metadata_receiver_t *, int32_t, const char *) {},
            [](const clap_preset_discovery_metadata_receiver_t *rv, const char *name, const char *loadKey) {
                auto *s = rs(rv);
                s->flush();
                PresetInfo p;
                p.name = name ? name : "";
                p.loadKey = loadKey ? loadKey : "";
                p.location = s->location;
                p.kind = s->kind;
                p.category = categoryOf(!p.loadKey.empty() ? p.loadKey : s->location);
                if (p.name.empty()) p.name = fs::path(s->location).stem().string();
                s->out->push_back(std::move(p));
                s->ids.clear();
                s->open = true;
                return true;
            },
            [](const clap_preset_discovery_metadata_receiver_t *rv, const clap_universal_plugin_id_t *id) {
                if (id && id->id && (!id->abi || std::string(id->abi) == "clap")) rs(rv)->ids.push_back(id->id);
            },
            [](const clap_preset_discovery_metadata_receiver_t *, const char *) {},
            [](const clap_preset_discovery_metadata_receiver_t *, uint32_t) {},
            [](const clap_preset_discovery_metadata_receiver_t *rv, const char *c) {
                if (c && rs(rv)->open) rs(rv)->out->back().creator = c;
            },
            [](const clap_preset_discovery_metadata_receiver_t *rv, const char *d) {
                if (d && rs(rv)->open) rs(rv)->out->back().description = d;
            },
            [](const clap_preset_discovery_metadata_receiver_t *, clap_timestamp, clap_timestamp) {},
            [](const clap_preset_discovery_metadata_receiver_t *rv, const char *f) {
                if (f && rs(rv)->open) rs(rv)->out->back().features.push_back(f);
            },
            [](const clap_preset_discovery_metadata_receiver_t *, const char *, const char *) {},
        };
        for (const auto &loc : state.locations) {
            if (loc.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
                r.location = ""; r.kind = loc.kind;
                provider->get_metadata(provider, loc.kind, nullptr, &receiver);
                r.flush();
            } else {
                std::vector<std::string> files;
                collectFiles(loc.path, state.extensions, files);
                for (const auto &f : files) {
                    r.location = f; r.kind = loc.kind;
                    provider->get_metadata(provider, loc.kind, f.c_str(), &receiver);
                    r.flush();
                }
            }
        }
        provider->destroy(provider);
    }
    // the same preset can be reported twice (e.g. factory bank and a copy on disk)
    std::stable_sort(out.begin(), out.end(), [](const PresetInfo &a, const PresetInfo &b) {
        return a.category != b.category ? a.category < b.category : lower(a.name) < lower(b.name);
    });
    out.erase(std::unique(out.begin(), out.end(), [](const PresetInfo &a, const PresetInfo &b) {
        return a.name == b.name && a.category == b.category && a.kind == b.kind;
    }), out.end());
    return true;
}

bool findPreset(const std::vector<PresetInfo> &presets, const std::string &query, PresetInfo &out, std::string &err) {
    const std::string q = lower(query);
    for (const auto &p : presets) if (p.name == query) { out = p; return true; }
    for (const auto &p : presets) if (lower(p.name) == q) { out = p; return true; }
    for (const auto &p : presets) if (lower(p.category + "/" + p.name) == q || lower(p.loadKey) == q) { out = p; return true; }
    std::vector<const PresetInfo *> hits;
    for (const auto &p : presets) if (lower(p.name).find(q) != std::string::npos) hits.push_back(&p);
    if (hits.size() == 1) { out = *hits[0]; return true; }
    err = hits.empty() ? "no preset named '" + query + "'" : "'" + query + "' matches " + std::to_string(hits.size()) + " presets";
    if (!hits.empty()) {
        err += ": ";
        for (size_t i = 0; i < hits.size() && i < 6; ++i) err += (i ? ", " : "") + hits[i]->name;
        if (hits.size() > 6) err += ", ...";
    }
    err += " (run `wavelength presets <plugin> --search <text>`)";
    return false;
}

} // namespace wl
