#pragma once
// builtin:audio: audio files placed on the timeline in beats, with pitch-preserving time stretch
// (Signalsmith Stretch), transposition, reverse, trimming and fades.
//
//   {"name": "Break", "plugin": "builtin:audio", "clips": [
//     {"file": "break.wav", "beat": 0, "bpm": 133},                 // stretched to the song tempo, pitch kept
//     {"file": "swell.wav", "endAt": 64, "reverse": true},          // ends exactly on beat 64
//     {"file": "vox.wav", "beat": 32, "pitch": -2, "start": 1.5, "length": 2, "fadeOut": 50},
//     {"file": {"render": [64, 65], "tail": 3, "fx": [{"type": "reverb", "mix": 0.6}]}, "reverse": true, "endAt": 64}]}
#include "job.hpp"
#include "wav.hpp"

#include <map>
#include <string>
#include <vector>

namespace wl {

// A clip whose "file" is the song's own audio: {"render": [fromBeat, toBeat], "tracks": [...], "tail": s, "fx": [...]}.
struct ClipRender {
    size_t clip;                      // index in the track's clips
    double fromBeat, toBeat;
    std::vector<std::string> tracks;  // empty = every track that renders no audio of its own this way
};
bool clipRenders(const Track &track, std::vector<ClipRender> &out, std::string &err);

// When the last clip of an audio track ends (seconds), for the render length.
double clipsEndSeconds(const Job &job, const Track &track);

// `rendered`: the captured audio for each "render" clip (by clip index), from the render graph.
bool renderClips(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err,
                 const std::map<size_t, Audio> *rendered = nullptr);

} // namespace wl
