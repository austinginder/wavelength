#pragma once
// Built-in instruments, used as `"plugin": "builtin:drums"` or `"plugin": "builtin:fx"`.
//
//  builtin:drums  synthesized kit on the General MIDI map: 35/36 kick, 37 rim, 38/40 snare,
//                 42/44 closed hat, 46 open hat, 49/57 crash, 51 ride, 41/43 low tom,
//                 45/47 mid tom, 48/50 high tom
//  builtin:sampler  sample libraries (Bitwig .multisample, WAV drum kits); see sampler.hpp
//  builtin:fx     cinematic effects: 48 (C3) impact, 50 (D3) riser lasting the note's length,
//                 52 (E3) reverse swell ending when the note ends, 53 (F3) sub drop
#include "job.hpp"
#include "wav.hpp"

#include <map>
#include <string>
#include <vector>

namespace wl {

bool isBuiltin(const std::string &plugin);
// `rendered`: builtin:audio "render" clips' captured audio, by clip index (see clips.hpp)
bool renderBuiltin(const std::string &plugin, const Job &job, const Track &track, Audio &out,
                   std::vector<std::string> &warnings, std::string &err, const std::map<size_t, Audio> *rendered = nullptr);

} // namespace wl
