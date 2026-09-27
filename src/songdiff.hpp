#pragma once
// Two versions of a job compared as music (`wavelength diff`, docs/song-format.md): which tracks, bars,
// sounds and settings changed. Comments use it to tell whether what they point at changed since.
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace wl {

struct PartDiff {
    std::string id, name, kind;          // kind: track, bus
    std::string status;                  // added, removed, changed
    std::vector<std::string> changes;    // one line each: "gain -6 -> -4 dB", "fx[1] filter: cutoff automation (bars 9-16)"
    std::vector<std::pair<int, int>> bars;   // bars (1-based, inclusive) where its notes or curves changed; whole = the part as a whole
    bool whole = false;                  // a change that sounds everywhere (plugin, preset, gain...)
};

struct SongDiff {
    std::vector<std::string> song;       // tempo, time signature, markers, keys, master
    std::vector<PartDiff> parts;
    bool empty() const { return song.empty() && parts.empty(); }
};

// `baseDir` resolves the jobs' relative paths while parsing them (nothing is loaded).
bool diffJobs(const nlohmann::json &a, const nlohmann::json &b, const std::string &baseDir, SongDiff &d, std::string &err);
nlohmann::json diffToJson(const SongDiff &d);
std::string diffToText(const SongDiff &d);
// Did bars [bar0, bar1] of these parts (IDs or names; empty = any part) change? `why` lists what did.
bool diffTouches(const SongDiff &d, const std::vector<std::string> &parts, int bar0, int bar1, std::vector<std::string> &why);

} // namespace wl
