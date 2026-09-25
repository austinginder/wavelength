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
    double lufs = -120, seconds = 0;   // seconds: wall time to render this track
    std::vector<double> sectionLufs;  // post-fader loudness in each marker section
    uint32_t latencySamples = 0;      // plugin processing delay removed from this track (instrument + effects)
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
    double mixLufs = -120, normalizeGainDb = 0, truePeakDb = -120;
    double loudnessGainDb = 0;   // gain into the master chain that met master.loudness
    std::vector<SectionResult> sections;
    std::vector<std::string> warnings;
    std::vector<std::string> failedTracks;   // tracks whose plugin crashed or hung; the song rendered without them
};

// Worker entry point: render track `index` of the job file (instrument + effects) into
// <prefix>.pcm (float left, then right) and <prefix>.json (its TrackResult). Exit code 0 on success.
int renderTrackWorker(const std::string &jobPath, size_t index, const std::string &prefix, const std::vector<std::string> &sidechains);

// Renders every track (instrument → effects) to its own stem, applies faders and sends,
// processes buses and the master chain, and writes mix.wav under outDir.
bool renderJob(const Job &job, const std::string &outDir, bool verbose, RenderResult &result, std::string &err);

} // namespace wl
