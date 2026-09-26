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

// Why a track is not melodic material ("" = it is): drums, effects, audio, kits, "harmony": false, or a
// builtin:sampler one-shot with no pitch of its own. Lint and chord detection leave these out.
std::string unpitchedReason(const Track &t, const std::string &baseDir);

// A chord symbol's tones as semitones above its root: "C#m", "Bb7", "F#m7b5", "Gsus4", "Dmaj7", "A5",
// "E/G#" (the bass after the slash is ignored). -1 = the chord has no such tone.
struct ChordTones { int root = -1, third = 4, fifth = 7, seventh = -1; };
bool parseChordName(const std::string &s, ChordTones &c, std::string &err);

// The chords of a job's melodic tracks as lint --harmony reads them: one per bar, or per half bar when
// the halves differ, as (beat, chord symbol). Bars without harmony are left out.
std::vector<std::pair<double, std::string>> detectChords(const Job &job);

} // namespace wl
