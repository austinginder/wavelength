#pragma once
// builtin:synth: a virtual-analog polysynth inside Wavelength, so melodic parts render on a machine
// without any plugins (a fresh laptop, a CI runner, an agent's cloud container).
//
//   "plugin": "builtin:synth", "preset": "BA Acid"      a named patch (`wavelength presets builtin:synth`)
//   "synth": {...}                                      the patch itself, merged over the preset
//   "params": {"cutoff": "800 Hz"}                      overrides by name (`wavelength params builtin:synth`)
//   "automation": {"params": {"cutoff": [[0, 200], [16, 4000]]}}   the same names over time
//
// Oscillators are band-limited (polyBLEP saw and pulse), with unison, FM on sine oscillators, a sub
// and noise, a resonant 12/24 dB state-variable filter with its own envelope, LFOs, glide and mono
// legato. Deterministic: the same job renders the same samples every time.
#include "job.hpp"
#include "wav.hpp"

#include <string>
#include <vector>

namespace wl {

struct SynthPatchInfo {
    std::string name, category, description;
};
std::vector<SynthPatchInfo> synthPatches();
// One of the synth's own patches (exact name), not a GarageBand one it re-creates.
bool isBuiltinSynthPatch(const std::string &name);

struct SynthParamInfo {
    std::string name, unit, description;
    double min, max, def;
    bool exp;   // automation curves interpolate exponentially (frequencies)
};
const std::vector<SynthParamInfo> &synthParams();
bool isSynthExpParam(const std::string &name);

bool renderSynth(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err);

} // namespace wl
