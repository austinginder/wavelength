#pragma once
// Harmony check for `wavelength lint --harmony`: the key in force at every bar (declared in the
// job's "keys", given with --key, or detected per section), the chord of every bar, and the
// places a listener hears as wrong: a bar or two of a chord outside the key that goes straight
// back (a one-bar "key change"), and sustained minor-second clashes between parts.
#include "job.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

// "D minor", "Dm", "F# major", "Bb", "c#m", "C phrygian": tonic pitch class 0-11, minor-type or not,
// and (optional) the mode: 0 major, 1 minor, 2 dorian, 3 phrygian, 4 lydian, 5 mixolydian.
bool parseKeyName(const std::string &s, int &tonic, bool &minor, std::string &err, int *mode = nullptr);
std::string keyLabel(int tonic, bool minor, int mode = -1);

struct HarmonyOptions {
    std::vector<size_t> tracks;     // tracks to read (indices into job.tracks)
    std::vector<KeyMark> keys;      // declared keys (from the job or --key); empty = detect
    int maxExcursionBars = 2;       // an outside run this short that goes back is reported
    double fromBeat = -1e18, toBeat = 1e18;   // report range
};

// {"keys": [{from, to, key, source}], "bars": [{bar, chord, key, outside: [...]}],
//  "problems": [{kind, bars, at, key, chord, notes, detail}], "info": [...]}
nlohmann::json analyzeHarmony(const Job &job, const HarmonyOptions &o);

} // namespace wl
