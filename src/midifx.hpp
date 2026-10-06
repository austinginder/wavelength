#pragma once
// A track's "midiFx": its notes through a chain of MIDI effects, in order, before anything else reads them (any
// instrument). Modelled on Logic's and GarageBand's MIDI effects:
//
//   {"type": "chord", "intervals": [0, 4, 7]}                         a chord built on every note (Chord Trigger, Single)
//   {"type": "chord", "chords": {"C3": [0, 4, 7], "D3": [0, 3, 7]}}  a chord per key, other keys in range silent (Multi)
//   {"type": "transpose", "semitones": 12, "scale": "A minor"}        moved, then onto the nearest scale note (Transposer)
//   {"type": "repeat", "time": "1/16", "repeats": 3, "ramp": 0.8}     echoes of every note (Note Repeater)
//   {"type": "arp", "rate": "1/16", "order": "up"}                    an arpeggio (arp.hpp)
//
// chord and repeat act on the keys in their "range" (the others pass). Every type also takes a preset of the effect
// it re-creates ("preset") or a GarageBand or Logic patch's ("patch") by name, with settings on top. Two notes that
// come out on one key at one moment sound once (the louder, then the longer); a repeat that strikes a key still
// sounding ends the earlier note there, as MIDI does.
#include <nlohmann/json.hpp>

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace wl {

// A scale as its pitch classes (C = 0): a key ("A minor", "D dorian", "C minor pentatonic") or a list of notes ([0, 3, 7] or
// ["C", "Eb", "G"]); all twelve (chromatic) leaves it empty.
bool parseScale(const nlohmann::json &s, std::set<int> &scale, std::string &err);

// beats from `beat` to `seconds` later on the song's tempo map (for a repeat's "ms"); unset = 120 BPM
using SecondsToBeats = std::function<double(double beat, double seconds)>;

// `fx` (a track's "midiFx" list) with the presets and patches it names resolved into settings and checked; notes =
// what is left out.
bool resolveMidiFx(const nlohmann::json &fx, nlohmann::json &out, std::vector<std::string> &notes, std::string &err);
// A track's notes ({beat, dur, key, vel, ...}) through a resolved chain, in order; deterministic. Notes without a
// beat and key play as written. err says what's wrong with an effect.
bool runMidiFx(const nlohmann::json &notes, const nlohmann::json &chain, nlohmann::json &out, std::string &err,
               const SecondsToBeats &toBeats = nullptr);
// A few words on one effect ("3 repeats 1/16 apart, -5 semitones each"); `apple` puts the name of the Apple effect
// it re-creates first: "Note Repeater (3 repeats 1/16 apart)".
std::string midiFxSummary(const nlohmann::json &fx, bool apple = false);

} // namespace wl
