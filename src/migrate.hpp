#pragma once
// `wavelength migrate <song>`: brings a song folder made before the song format (docs/song-format.md) up
// to it. Writes the manifest (from the folder, its site.json and the options), makes every file the job
// uses part of the song or names it (paths inside the song become relative, scratch files in out/ move
// into media/, library samples become "lib:" names, preset files become preset names, anything else is
// copied into media/), brings review.json to the current shape, keeps the last render when it matches
// the job, and saves a revision. Reports what it could not fix.
#include <nlohmann/json.hpp>

#include <string>

namespace wl::migrate {

struct Options {
    std::string license, author;   // SPDX expression; a person to credit as producer
    bool dryRun = false;           // report what would change, write nothing
    bool copyOutside = true;       // copy files from outside the song into media/ (else report them)
};

// {"ok", "changes": [...], "problems": [...], "notes": [...], "revision"}
bool run(const std::string &songDir, const Options &opt, nlohmann::json &result, std::string &err);

} // namespace wl::migrate
