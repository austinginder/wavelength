#pragma once
// Gain staging: each track's fader from its stem loudness. One render (stems not written, the track cache
// on) measures every track before its fader; a fader is then target - stem LUFS, with the target from the
// track's role (a word of its name: Kick -12, Bass -15.5, Lead -16, Pad -22 ...) or from targets.json.
// The gains go to gains.json beside the job (what a song's generator reads), and with `apply` into the job.
#include <nlohmann/json.hpp>

#include <string>

namespace wl {

struct StageOptions {
    std::string job;        // the job file
    std::string targets;    // a targets file on top of the role defaults and targets.json beside the job
    std::string report;     // measure from this render's report instead of rendering
    std::string out;        // the staging render's folder (default: <job dir>/out/stage)
    std::string write;      // the gains file (default: <job dir>/gains.json)
    bool apply = false;     // also set the gains in the job file
    bool dryRun = false;    // write nothing
    bool verbose = false;
    int jobs = -1;          // render workers (-1: render's default)
};

// The role a track name says (role name and target LUFS); empty role when no word of the name matches.
struct Role { std::string name; double lufs = 0; };
Role roleOf(const std::string &trackName, const std::string &plugin = "", bool kit = false);

bool stageGains(const StageOptions &opt, nlohmann::json &result, std::string &err);

} // namespace wl
