#pragma once
// `wavelength serve`: a local web UI for reviewing songs. Reads song folders (job.json, report.json,
// stems, review.json), renders previews of bars and tracks in child processes (never a plugin in
// this process), and takes review comments for the agent. Read-only on the music.
#include <string>

namespace wl {

struct ServeOptions {
    std::string root = ".";        // folder of song folders
    std::string host = "127.0.0.1";
    int port = 7400;
    std::string uiDir;             // serve the UI from this folder (development) instead of the built-in copy
    bool open = false;             // open the browser
};

int serve(const ServeOptions &o);

} // namespace wl
