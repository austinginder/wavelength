#pragma once
// `wavelength serve`: a local web UI for reviewing songs. Reads song folders (job.json, report.json,
// stems, review.json), renders previews of bars and tracks in child processes (never a plugin in
// this process), and takes review comments for the agent. Read-only on the music.
#include <cstdio>
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

// `wavelength __play <job.json> <track>` (internal, started by serve): loads the track's instrument once
// and keeps it loaded. Each JSON line on stdin, {"notes": [{"key", "vel", "start", "dur"}] (seconds),
// "seconds", "out": wav path}, renders those notes through it and answers one JSON line on `out`.
int playWorker(const std::string &jobPath, const std::string &track, std::FILE *out);
// `wavelength __live <job.json> <track>` (internal, started by serve): the track's instrument playing
// continuously (CLAP and VST3). A JSON hello line, then 16-bit stereo PCM paced to the clock on `out`;
// stdin lines {"on": 60, "vel": 0.8}, {"off": 60}, {"allOff": true}, {"stop": true}.
int liveWorker(const std::string &jobPath, const std::string &track, std::FILE *out);

} // namespace wl
