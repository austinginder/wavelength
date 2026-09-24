#pragma once
// Effects: built-in DSP (deterministic, always available) and CLAP effect plugins.
// Every effect processes a whole stereo timeline in place. See docs/effects.md.
#include "job.hpp"
#include "wav.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wl {

struct FxContext {
    const Job &job;
    bool verbose;
};

class Effect {
public:
    virtual ~Effect() = default;
    virtual bool process(Audio &io, const FxContext &ctx, std::string &err) = 0;
    std::string label;                  // "reverb", "compressor", "Vital", …
    std::vector<std::string> warnings;
};

// Builds one effect from its JSON description. `context` prefixes error messages.
std::unique_ptr<Effect> makeEffect(const nlohmann::json &j, const Job &job, const std::string &context, std::string &err);
std::vector<std::string> builtinEffectTypes();

// Highest reconstructed (inter-sample, 4x oversampled) peak in dBTP.
double truePeakDb(const Audio &a);

} // namespace wl
