#pragma once
// DAWproject import (the open project format Bitwig, Studio One and others export): the arrangement,
// tracks, notes, plugin states, mixer (volume, pan, mute, sends, groups), tempo and markers become a
// Wavelength job. What the file can't carry (a DAW's own devices, their samples) is listed, not dropped
// silently.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct DawprojectImport {
    nlohmann::json job;                 // the job, with state paths relative to the output folder
    std::vector<std::string> notes;     // what was substituted or left out, one line each
    size_t tracks = 0, buses = 0, noteCount = 0, plugins = 0;
    std::string application;            // the exporting DAW ("Bitwig Studio 5.3")
    std::string bitwig;                 // the Bitwig project read for Bitwig's own devices ("" when none)
};

// Reads `path` (.dawproject), writes `outDir`/job.json and the plugin states under `outDir`/plugins/.
// `bitwig`: the .bwproject behind a Bitwig export ("" = look for it next to the file and in
// ~/Documents/Bitwig Studio/Projects/<name>/, "none" = don't).
bool importDawproject(const std::string &path, const std::string &outDir, DawprojectImport &out, std::string &err,
                      const std::string &bitwig = "");

} // namespace wl
