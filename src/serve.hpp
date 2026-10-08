#pragma once
// `wavelength serve`: a local web UI for reviewing songs. Reads song folders (job.json, report.json,
// stems, review.json), renders previews of bars and tracks in child processes (never a plugin in
// this process), and takes review comments for the agent. The music changes only through edits.json
// (note edits) and the Sounds page (sounds.json, brief.md, new tracks and the tempo in job.json).
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
// stdin lines {"on": 60, "vel": 0.8}, {"off": 60}, {"allOff": true}, {"stop": true}, {"param": id, "value": plain}.
// {"editor": true/false} opens or closes the plugin's own window; `statusPath`, when not empty, keeps what it changed.
int liveWorker(const std::string &jobPath, const std::string &track, const std::string &statusPath, std::FILE *out);
// `wavelength __knobs <job.json> <track>` (internal, started by serve): the track's instrument loaded (preset and
// state, without its "params") for the Sounds page's knobs. A JSON hello line lists its parameters with the
// preset's values; then each stdin line asks one thing and gets one line back:
//   {"resolve": {"Filter Cutoff": "917 Hz", ...}}  -> {"values": [{"key", "id", "value", "display"}], "errors": [...]}
//   {"text": [[id, value], ...]}                    -> {"text": ["917 Hz", ...]}
//   {"parse": id, "text": "917 Hz"}                 -> {"value", "display"} or {"error"}
//   {"store": [[id, value], ...]}                   -> {"store": [...]}: the plugin's text where it reads back as the
//                                                      same value (so sounds.json stays readable), else the number
int knobsWorker(const std::string &jobPath, const std::string &track, std::FILE *out);

} // namespace wl
