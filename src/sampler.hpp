#pragma once
// builtin:sampler: plays sample libraries without a plugin.
//
//  multisample  a Bitwig .multisample (zip: multisample.xml + samples; the open format Bitwig's
//               Sampler uses for its piano, organ, guitar, bass and keys libraries) or a folder
//               holding multisample.xml. Key and velocity zones, velocity crossfades, round
//               robins, select ranges, sustain loops with crossfade, key tracking, reverse.
//  soundfont    a preset of a SoundFont (.sf2, or .sf3 with Ogg Vorbis samples) by bank/program or name:
//               zones, envelope, filter, loops and exclusive classes from the file.
//  sfz          an SFZ instrument (sfz.hpp): regions with key/velocity ranges, crossfades, round
//               robins, keyswitches, loops (the file's own too), amp envelope, choke groups,
//               and the *sine/*saw/*square/*triangle/*noise generators.
//  kit          a folder of one-shot samples (any format audio_file.hpp reads), mapped to General MIDI keys from the file names
//               (kick 36, snare 38, clap 39, closed hat 42, open hat 46, crash 49, ...), or an
//               explicit {"key": "file"} map.
//  sample       one sample played chromatically from a root key.
//
// Names are searched in $WAVELENGTH_SAMPLES_PATH and the Bitwig Studio package folders.
#include "job.hpp"
#include "wav.hpp"

#include <string>
#include <vector>

namespace wl {

bool renderSampler(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err);

struct SampleLibraryEntry {
    std::string kind;       // "multisample", "sfz", "soundfont", "kit" or "loops"
    std::string name, path, category;
    size_t count = 0;       // zones (multisample), regions (sfz), presets (soundfont) or sample files (kit)
};

// Every multisample and kit folder under the sample roots (cached per process).
const std::vector<SampleLibraryEntry> &sampleLibrary();
// A sample file by path (absolute, relative to baseDir, or relative to a sample root); "" if missing.
std::string resolveSampleFile(const std::string &name, const std::string &baseDir);
std::vector<std::string> sampleRoots();
// A library entry of `kind` by path or name (as the sampler finds "multisample", "sfz", "soundfont")
bool findSampleEntry(const std::string &kind, const std::string &query, const std::string &baseDir, std::string &path, std::string &err);
// Where `samples --install-soundfont` puts SoundFonts (a sample root when it exists)
std::string soundFontDir();
// The General MIDI SoundFont to fall back on: $WAVELENGTH_SOUNDFONT, else the installed one with the
// most presets that covers all 128 programs; "" when there is none
std::string defaultSoundFont();

// The General MIDI map a kit folder gets: key -> file path. Files that are takes of the same sound
// ("Snare 01", "Snare 02") share its key as round robins when `roundRobin`, else they (and
// unrecognised files) take free keys from 60 and are listed in `unmapped` / `extraTakes`.
bool kitMap(const std::string &nameOrPath, const std::string &baseDir, std::vector<std::pair<int, std::string>> &map,
            std::vector<std::string> &unmapped, std::string &resolved, std::string &err, bool roundRobin = false,
            std::vector<std::string> *extraTakes = nullptr);

} // namespace wl
