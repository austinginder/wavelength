#pragma once
// A track's "arp": its held notes played as an arpeggio, before anything else reads them (any instrument).
//
//   "arp": {"rate": "1/16", "order": "up", "octaves": 1, "gate": 0.8}
//
// Steps fall on the song's grid every `rate` (a note value or beats). At each step the notes held there (start <= step
// < end) are ordered by `order` and spread over `octaves`, and the next one in that cycle plays for `gate` of a step
// with its own velocity. The cycle restarts when a new phrase begins (the held set was empty) and carries on when
// notes join or leave. Orders: up, down, updown and downup (the turning notes once), played (the order the notes
// were struck), random (seeded: deterministic), chord (every held note on each step).
#include <nlohmann/json.hpp>

#include <string>

namespace wl {

// The arpeggiated notes ({beat, dur, key, vel, ...the held note's other fields}) of a track's written notes; err
// says what's wrong with `arp`. Keys may be note names or numbers, as in a job.
bool arpeggiate(const nlohmann::json &notes, const nlohmann::json &arp, nlohmann::json &out, std::string &err);

} // namespace wl
