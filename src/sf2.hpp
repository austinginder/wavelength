#pragma once
// SoundFont 2 (.sf2) and its Ogg Vorbis compressed form (.sf3, MuseScore's): presets by bank and
// program, resolved into zones (instrument generators with the preset's added on top, ranges
// intersected) for builtin:sampler. Samples are read on demand.
#include "audio_file.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace wl {

struct Sf2Preset {
    std::string name;
    int bank = 0, program = 0;
};

// A modulator whose value depends on the note (velocity or key number): evaluated per note.
struct Sf2Mod { uint16_t src, dest; int16_t amount; uint16_t amtSrc; };

// One playable zone, generators resolved to units the sampler uses.
struct Sf2Zone {
    int sample = -1;
    int keyLow = 0, keyHigh = 127, velLow = 0, velHigh = 127;
    int root = 60;
    double tune = 0;             // semitones (coarse + fine + the sample's pitch correction)
    double keyTrack = 1;         // scaleTuning / 100
    double attenuationCb = 0;    // centibels: 0.4 x initialAttenuation + controller modulators at rest (per-note ones add to it)
    double pan = 0;              // -1..1
    double start = 0, stop = -1; // frames in the sample (stop exclusive, -1 = end)
    int loopMode = 0;            // 0 none, 1 continuous, 3 until release
    double loopStart = 0, loopStop = 0;
    double delay = 0, attack = 0, hold = 0, decay = 0, sustain = 1, release = 0;   // seconds, sustain linear
    double fcCents = 13500, qCb = 0;        // filter cutoff (absolute cents) and resonance with controllers at rest
    double modEnvToFc = 0;                  // cents the modulation envelope opens the filter by
    double modDelay = 0, modAttack = 0, modHold = 0, modDecay = 0, modSustain = 1, modRelease = 0;   // seconds, level 0-1
    int exclusiveClass = 0;
    // keynumTo*: timecents added per key below 60 (hold and decay get shorter up the keyboard)
    double keyToHold = 0, keyToDecay = 0, keyToModHold = 0, keyToModDecay = 0;
    // a time scaled for a key: seconds * 2^((60 - key) * centsPerKey / 1200)
    static double keyScaled(double seconds, double centsPerKey, int key) { return seconds * std::pow(2.0, (60 - key) * centsPerKey / 1200); }
    std::vector<Sf2Mod> perNote;            // velocity/key modulators (filter, level, pan, tuning, envelope times)
    // their sum for one destination generator at a key (0-127) and velocity (0-1), in the generator's units
    double modulate(uint16_t dest, int key, double vel) const;
    enum : uint16_t { kFilterFc = 8, kFilterQ = 9, kModEnvToFc = 11, kPan = 17, kAttackMod = 26, kHoldMod = 27, kDecayMod = 28,
                      kReleaseMod = 30, kAttackVol = 34, kHoldVol = 35, kDecayVol = 36, kReleaseVol = 38, kAttenuation = 48,
                      kCoarseTune = 51, kFineTune = 52 };
};

class SoundFont {
public:
    bool open(const std::string &path, std::string &err);
    const std::vector<Sf2Preset> &presets() const { return presets_; }
    // the preset for bank/program (bank 128 = percussion); falls back to bank 0, then any bank
    const Sf2Preset *find(int bank, int program) const;
    const Sf2Preset *findByName(const std::string &name) const;   // exact, then unique substring (case-insensitive)
    bool zones(const Sf2Preset &p, std::vector<Sf2Zone> &out, std::string &err) const;
    bool sampleData(int index, DecodedAudio &out, std::string &err) const;
    std::string sampleName(int index) const;
    bool compressed() const { return compressed_; }

private:
    struct Gen { uint16_t oper; uint16_t amount; };
    struct Mod { uint16_t src, dest; int16_t amount; uint16_t amtSrc, trans; };
    struct Bag { uint16_t gen, mod; };
    struct Hdr { std::string name; uint16_t preset, bank, bag; };
    struct Inst { std::string name; uint16_t bag; };
    struct Shdr { std::string name; uint32_t start, end, loopStart, loopEnd, rate; uint8_t key; int8_t corr; uint16_t link, type; };
    std::string path_;
    std::vector<Hdr> phdr_;
    std::vector<Bag> pbag_, ibag_;
    std::vector<Gen> pgen_, igen_;
    std::vector<Mod> pmod_, imod_;
    std::vector<Inst> inst_;
    std::vector<Shdr> shdr_;
    uint64_t smplOffset_ = 0, smplSize_ = 0, sm24Offset_ = 0, sm24Size_ = 0;
    bool compressed_ = false;
    std::vector<Sf2Preset> presets_;
};

} // namespace wl
