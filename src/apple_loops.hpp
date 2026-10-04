#pragma once
// Apple Loops: the loops GarageBand and Logic install in /Library/Audio/Apple Loops (and the user's own in
// ~/Library/Audio/Apple Loops). Each is a CAF whose 'uuid' chunk carries tags: beat count, key, scale,
// time signature, category, genre and descriptors. Software-instrument ("green") loops also carry their
// notes as a Standard MIDI File in a 'midi' chunk. The audio is AAC, decoded by the system (macOS).
#include <cstdint>
#include <string>
#include <vector>

namespace wl {

struct AppleLoop {
    std::string path, name, folder;  // name: the file name without ".caf"; folder: the folder it is in
    int beats = 0;                   // 0: a one-shot (an effect, a hit) with no tempo of its own
    double seconds = 0, bpm = 0;     // the audio's length, and the tempo it was recorded at (beats / length)
    std::string key, scale;          // "Bb" and "minor", "major", "both" or "neither"; both empty for drums
    std::string timeSignature, category, subcategory, genre;
    std::vector<std::string> descriptors;
    bool midi = false;               // its notes are inside (a software-instrument loop)
};

// The tags of one file; false when it isn't an Apple Loop (not a CAF, or a CAF without the tags).
bool readAppleLoop(const std::string &path, AppleLoop &out, std::string &err);
// The tags of a file, read once per process; nullptr when it isn't an Apple Loop.
const AppleLoop *appleLoopAt(const std::string &path);
// Where Apple Loops are installed (the folders that exist).
std::vector<std::string> appleLoopRoots();
// Every Apple Loop under the roots, sorted by name, the same file in two folders once (cached per process).
const std::vector<AppleLoop> &appleLoops();
// How many there are, without reading their tags (the folders only).
size_t appleLoopCount();
// An Apple Loop by file name, "Early Days Piano" or "Early Days Piano.caf", ignoring case; nullptr when none.
const AppleLoop *findAppleLoop(const std::string &name);
// The Standard MIDI File inside a CAF (a software-instrument loop); false when it has none.
bool cafMidi(const std::vector<uint8_t> &caf, std::vector<uint8_t> &smf, std::string &err);
// Semitones (-6..+5) that move `loop` into `key` ("D minor", "F#", "Bbm", "C dorian"): tonic to tonic, or to
// the relative tonic when one is minor and the other major, so the loop's notes stay in the key. False with
// err when the loop has no key (drums) or `key` doesn't parse.
bool appleLoopShift(const AppleLoop &loop, const std::string &key, int &semitones, std::string &err);
// The same from a tonic (0-11) and minor flag, as the job's "keys" give them.
bool appleLoopShift(const AppleLoop &loop, int tonic, bool minor, int &semitones);

} // namespace wl
