#pragma once
#include "job.hpp"
#include "wav.hpp"

#include <string>
#include <vector>

namespace wl {

struct TrackResult {
    std::string name, plugin, pluginName, file, stateFormat, preset;
    size_t notes = 0, paramsApplied = 0, automated = 0;
    std::vector<std::string> fx;
    Levels levels{};
    double lufs = -120;
    std::vector<std::string> warnings;
};

struct BusResult {
    std::string name;
    std::vector<std::string> fx;
    Levels levels{};
    double lufs = -120;
};

struct SectionResult {
    std::string name;
    double start, end, lufs;
};

struct RenderResult {
    int sampleRate = 0;
    double seconds = 0, renderSeconds = 0;
    std::vector<TrackResult> tracks;
    std::vector<BusResult> buses;
    std::vector<std::string> masterFx;
    std::string mixFile;
    Levels mix{};
    double mixLufs = -120, normalizeGainDb = 0;
    std::vector<SectionResult> sections;
    std::vector<std::string> warnings;
};

// Renders every track (instrument → effects) to its own stem, applies faders and sends,
// processes buses and the master chain, and writes mix.wav under outDir.
bool renderJob(const Job &job, const std::string &outDir, bool verbose, RenderResult &result, std::string &err);

} // namespace wl
