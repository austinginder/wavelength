#pragma once
// render --cache: each track's audio after its effects and before its fader, kept between renders and
// reused when nothing that shapes it has changed. The key covers the track as parsed (notes after
// edits, sound, effects, instrument automation) without its mixer settings (gain, pan, mute, sends,
// rides), the job's shared settings (tempo, sample rate, window, length), the tracks it depends on
// (sidechain audio, duck/gate trigger notes), the size and date of every file it names, and the
// engine version. A mixer change re-renders nothing; a note change re-renders that track. Not
// noticed: a plugin update or a changed preset file named by "preset" (render without --cache).
// Entries live in platform::cacheDir()/tracks, oldest dropped past a size cap.
#include "job.hpp"
#include "wav.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace wl::trackcache {

// One key per track ("" = not cached: a track that renders other tracks into a clip, or depends on one).
// deps: tracks whose audio each track needs (sidechain sources, rendered-clip sources); `uncached`
// tracks never are.
std::vector<std::string> keys(const Job &job, const std::vector<std::set<size_t>> &deps, const std::set<size_t> &uncached,
                              size_t frames);

// A cached track: its audio (exactly `frames` long) and the worker result JSON it came with.
bool load(const std::string &key, size_t frames, Audio &audio, nlohmann::json &result);
void store(const std::string &key, const Audio &audio, const nlohmann::json &result);
// the worker's own files (<prefix>.pcm, <prefix>.json), moved into the cache
void storeFiles(const std::string &key, const std::string &prefix);

// Drops the least recently used entries past the cap ($WAVELENGTH_TRACK_CACHE_GB, default 4).
void prune();

} // namespace wl::trackcache
