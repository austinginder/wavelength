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
//  exs          a Logic / GarageBand Sampler instrument (.exs, EXS24): zones, groups (velocity layers,
//               key ranges, controller-enabled groups at rest, articulations as keyswitches from MIDI 0),
//               loops, the instrument's level, tuning and amplitude envelope; samples found where Logic
//               keeps them, consolidated CAF samples read by range.
//  patch        a GarageBand / Logic patch (logic_patches.hpp) whose channels play Sampler, EXS24 or Drum
//               Kit Designer: the instruments of those channels, merged (the patch's effects are not played).
//  kit          a folder of one-shot samples (any format audio_file.hpp reads), mapped to General MIDI keys from the file names
//               (kick 36, snare 38, clap 39, closed hat 42, open hat 46, crash 49, ...), or an
//               explicit {"key": "file"} map.
//  sample       one sample played chromatically from a root key.
//
// Names are searched in $WAVELENGTH_SAMPLES_PATH, the Bitwig Studio package folders and Serum 2's Multisamples.
#include "job.hpp"
#include "wav.hpp"

#include <string>
#include <vector>

namespace wl {

bool renderSampler(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err);

struct SampleLibraryEntry {
    std::string kind;       // "multisample", "sfz", "soundfont", "exs", "patch", "kit", "loops" or "ir"
    std::string name, path, category;
    size_t count = 0;       // zones (multisample), regions (sfz), presets (soundfont), installed samples (patch) or sample files (kit)
};

// Every multisample and kit folder under the sample roots (cached per process).
const std::vector<SampleLibraryEntry> &sampleLibrary();
// The GarageBand and Logic patches the sampler plays ("patch"), with how many of their samples are
// installed: only patches with some (cached per process, built on first use).
const std::vector<SampleLibraryEntry> &patchLibrary();
// Impulse responses for the convolve effect ("ir"): Logic's and GarageBand's Space Designer rooms (.SDIR, AIFF
// inside) and any audio file under /Library/Audio/Impulse Responses, ~/Library/Audio/Impulse Responses and
// $WAVELENGTH_IR_PATH; count = length in ms (cached per process, built on first use).
const std::vector<SampleLibraryEntry> &impulseLibrary();
// An impulse response by path (relative to baseDir), lib: name or library name; "" with err when none.
std::string findImpulseResponse(const std::string &query, const std::string &baseDir, std::string &err);
// A sample file by path (absolute, relative to baseDir, or relative to a sample root); "" if missing.
std::string resolveSampleFile(const std::string &name, const std::string &baseDir);
std::vector<std::string> sampleRoots();
// "lib:<library>/<file>": a file of an installed sample library, never one of the song's: a kit or loop
// folder by name ("lib:Legend 909/Kick Legend 909 01.wav", or "lib:Classic Drum Machines/Legend 909/...")
// or a path under a sample root ("lib:Bitwig/Anti-Loops/Genys/Kick.wav"); "" when it isn't installed.
std::string resolveLibraryFile(const std::string &ref);
// The shortest "lib:" name of an installed library file (absolute path); "" when it is in no library.
std::string libraryRef(const std::string &file);
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
