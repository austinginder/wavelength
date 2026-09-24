#pragma once
// builtin:sampler: plays sample libraries without a plugin.
//
//  multisample  a Bitwig .multisample (zip: multisample.xml + WAVs; the open format Bitwig's
//               Sampler uses for its piano, organ, guitar, bass and keys libraries) or a folder
//               holding multisample.xml. Key and velocity zones, velocity crossfades, round
//               robins, select ranges, sustain loops with crossfade, key tracking, reverse.
//  kit          a folder of one-shot WAVs, mapped to General MIDI keys from the file names
//               (kick 36, snare 38, clap 39, closed hat 42, open hat 46, crash 49, ...), or an
//               explicit {"key": "file"} map.
//  sample       one WAV played chromatically from a root key.
//
// Names are searched in $WAVELENGTH_SAMPLES_PATH and the Bitwig Studio package folders.
#include "job.hpp"
#include "wav.hpp"

#include <string>
#include <vector>

namespace wl {

bool renderSampler(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err);

struct SampleLibraryEntry {
    std::string kind;       // "multisample" or "kit"
    std::string name, path, category;
    size_t count = 0;       // zones (multisample) or WAV files (kit)
};

// Every multisample and kit folder under the sample roots (cached per process).
const std::vector<SampleLibraryEntry> &sampleLibrary();
// A sample file by path (absolute, relative to baseDir, or relative to a sample root); "" if missing.
std::string resolveSampleFile(const std::string &name, const std::string &baseDir);
std::vector<std::string> sampleRoots();

// The General MIDI map a kit folder gets: key -> file path. Files that are takes of the same sound
// ("Snare 01", "Snare 02") share its key as round robins when `roundRobin`, else they (and
// unrecognised files) take free keys from 60 and are listed in `unmapped` / `extraTakes`.
bool kitMap(const std::string &nameOrPath, const std::string &baseDir, std::vector<std::pair<int, std::string>> &map,
            std::vector<std::string> &unmapped, std::string &resolved, std::string &err, bool roundRobin = false,
            std::vector<std::string> *extraTakes = nullptr);

} // namespace wl
