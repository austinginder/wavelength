#pragma once
#include <string>
#include <vector>

namespace wl {

// Stereo float buffer: one vector per channel.
struct Audio {
    std::vector<float> left, right;
    size_t frames() const { return left.size(); }
    void resize(size_t n) { left.assign(n, 0.f); right.assign(n, 0.f); }
};

struct Levels {
    double peakDb, rmsDb, activeRmsDb;   // activeRmsDb ignores near-silent samples
    bool silent;
};

// bits: 32 = IEEE float (keeps overs above 0 dBFS), 24 or 16 = PCM (dithered 16, clipped at full scale)
// leadFrames: silent frames written before the audio (a lead-in for streaming uploads)
bool writeWav(const std::string &path, const Audio &a, int sampleRate, std::string &err, int bits = 32, size_t leadFrames = 0);
Levels measure(const Audio &a);
// Read a PCM (8/16/24/32-bit) or float (32/64-bit) WAV; mono files are duplicated to both channels.
bool readWav(const std::string &path, Audio &out, int &sampleRate, std::string &err);

} // namespace wl
