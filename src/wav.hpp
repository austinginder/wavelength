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

bool writeWav(const std::string &path, const Audio &a, int sampleRate, std::string &err);  // 32-bit float
Levels measure(const Audio &a);

} // namespace wl
