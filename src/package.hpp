#pragma once
// .wavelength packages (docs/song-format.md, section 5): a song folder packed into a ZIP whose first entry
// is `mimetype`, and back. Unpacking checks every entry before writing anything; validate checks a song
// folder or a package against the spec.
#include "song.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace wl::package {

using json = nlohmann::json;

struct PackOptions {
    bool history = true, render = true, review = true;
};

// Packs the song into `out` (default "<slug>.wavelength" in the current folder). Writes the manifest's
// `requires`, `generator`, `updated` and file hashes back into the song first. `result` lists the entries.
bool pack(Song &song, std::string out, const PackOptions &opt, json &result, std::string &err);
// Unpacks into `outDir` (default: the song's slug next to the package); refuses anything section 5 forbids.
bool unpack(const std::string &file, std::string outDir, bool force, json &result, std::string &err);
// A song folder or a package against the spec: {"ok", "problems": [{severity, path, message}]}.
json validate(const std::string &target);
// A package unpacked once into the cache (by its hash), for render and serve; "" on error.
std::string cached(const std::string &file, std::string &err);
bool isPackage(const std::string &path);

} // namespace wl::package
