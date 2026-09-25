#pragma once
// Effects: built-in DSP (deterministic, always available) and CLAP effect plugins.
// Every effect processes a whole stereo timeline in place. See docs/effects.md.
#include "job.hpp"
#include "wav.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wl {

struct FxContext {
    const Job &job;
    bool verbose;
    // a track's audio (after its effects, before its fader) for effects keyed by it ("sidechain")
    std::function<const Audio *(const std::string &track)> sidechain;
};

class Effect {
public:
    virtual ~Effect() = default;
    virtual bool process(Audio &io, const FxContext &ctx, std::string &err) = 0;
    std::string label;                  // "reverb", "compressor", "Vital", …
    uint32_t latencySamples = 0;        // plugin effects: reported processing delay (already compensated)
    std::vector<std::string> warnings;
    // curves that start late, as (beat of the first point, warning): kept only when the track sounds before
    // that beat (buildChain decides; a curve on a track that is silent until then holds nothing audible)
    std::vector<std::pair<double, std::string>> lateCurves;
};

// Builds one effect from its JSON description. `context` prefixes error messages.
std::unique_ptr<Effect> makeEffect(const nlohmann::json &j, const Job &job, const std::string &context, std::string &err);
std::vector<std::string> builtinEffectTypes();

// Highest reconstructed (inter-sample, 4x oversampled) peak in dBTP.
double truePeakDb(const Audio &a);

} // namespace wl
