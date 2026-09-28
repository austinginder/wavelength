#pragma once
// wavelength purge: gives back the disk renders take while every song stays playable. In each render
// folder (a folder whose report.json a render wrote: out/, a shootout's out-bass/, serve's previews) it
// deletes mix.wav, the stems and lossless deliveries, and empties the preview cache. A render with no
// MP3 gets mix.mp3 first. Reports and pictures stay (the review page shows them); the song's sources
// never go: files its job or manifest names, render/ (kept renders), media/, anything not written by a
// render. A render changed in the last few minutes may still be running and is left alone. Outputs of
// `wavelength master` (finished masters) are kept unless Options::masters.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl::purge {

struct Options {
    bool dryRun = false;
    bool masters = false;         // also `wavelength master` outputs (finished masters are kept by default)
    double minAgeSeconds = 300;   // renders written more recently than this are skipped
};

// Purges every render folder under `folders` (default "."). `r`: {"ok", "dryRun", "renders": [{"dir",
// "removed": [..], "bytes", "previews", "master", "madeMp3", "replacedMp3", "keptMix", "error"}], "skipped":
// [{"dir", "why"}], "freedBytes", "mp3sMade", "mastersKept": {"count", "bytes"}, "otherWav": {"files",
// "bytes", "folders": [{"dir", "bytes"}]}}.
bool run(const std::vector<std::string> &folders, const Options &opt, nlohmann::json &r, std::string &err);

// "1.4 GB", "85.0 MB", "512 KB"
std::string bytesText(double bytes);

} // namespace wl::purge
