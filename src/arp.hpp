#pragma once
// A track's "arp": its held notes played as an arpeggio, before anything else reads them (any instrument).
//
//   "arp": {"rate": "1/16", "order": "up", "octaves": 1, "gate": 0.8}
//   "arp": "Classic Analog Arp"                    a GarageBand patch's Arpeggiator, or an Arpeggiator preset
//   "arp": {"preset": "Rolling 8ths", "octaves": 2}   the same, with settings changed
//
// Modelled on Logic's and GarageBand's Arpeggiator. A phrase starts when a note goes down with none held and ends
// when the last is released (so back-to-back chords restart it, overlapping ones carry on). Its steps fall on the
// song's grid every `rate` (a note up to a tenth of a step late still starts on that step). At each step the notes
// held there are ordered by `order` and `variation` and spread over `octaves` (or `inversions`), and the next one
// in that cycle plays for `gate` of a step. `steps` is a rhythm grid: a velocity per step, rests, longer steps
// (ties) and chord steps, repeating from the start of each phrase. Orders: up, down, updown, downup, outsidein,
// played (the order the notes were struck), random (seeded: deterministic), chord (every held note each step).
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

// The arpeggiated notes ({beat, dur, key, vel, ...the held note's other fields}) of a track's written notes; err
// says what's wrong with `arp`. Keys may be note names or numbers, as in a job.
bool arpeggiate(const nlohmann::json &notes, const nlohmann::json &arp, nlohmann::json &out, std::string &err);
// `arp` with a patch or preset it names resolved into its settings (the other keys on top); notes = what's left out.
bool resolveArp(const nlohmann::json &arp, nlohmann::json &out, std::vector<std::string> &notes, std::string &err);
// A few words on an arp's settings: "1/16 up over 2 octaves, a 16-step grid".
std::string arpSummary(const nlohmann::json &arp);

} // namespace wl
