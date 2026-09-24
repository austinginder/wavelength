#pragma once
// builtin:audio: audio files placed on the timeline in beats, with pitch-preserving time stretch
// (Signalsmith Stretch), transposition, reverse, trimming and fades.
//
//   {"name": "Break", "plugin": "builtin:audio", "clips": [
//     {"file": "break.wav", "beat": 0, "bpm": 133},                 // stretched to the song tempo, pitch kept
//     {"file": "swell.wav", "endAt": 64, "reverse": true},          // ends exactly on beat 64
//     {"file": "vox.wav", "beat": 32, "pitch": -2, "start": 1.5, "length": 2, "fadeOut": 50}]}
#include "job.hpp"
#include "wav.hpp"

#include <string>
#include <vector>

namespace wl {

// When the last clip of an audio track ends (seconds), for the render length.
double clipsEndSeconds(const Job &job, const Track &track);

bool renderClips(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err);

} // namespace wl
