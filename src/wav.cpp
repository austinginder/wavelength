#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

namespace wl {

namespace {
void u32(std::ofstream &o, uint32_t v) { o.put(char(v)); o.put(char(v >> 8)); o.put(char(v >> 16)); o.put(char(v >> 24)); }
void u16(std::ofstream &o, uint16_t v) { o.put(char(v)); o.put(char(v >> 8)); }
double db(double lin) { return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0; }
} // namespace

bool writeWav(const std::string &path, const Audio &a, int sampleRate, std::string &err) {
    std::ofstream o(path, std::ios::binary);
    if (!o) { err = "cannot write " + path; return false; }
    const uint32_t frames = (uint32_t)a.frames(), channels = 2, bytes = frames * channels * 4;
    o.write("RIFF", 4); u32(o, 4 + (8 + 18) + (8 + 4) + (8 + bytes)); o.write("WAVE", 4);
    o.write("fmt ", 4); u32(o, 18); u16(o, 3 /* IEEE float */); u16(o, channels); u32(o, sampleRate);
    u32(o, sampleRate * channels * 4); u16(o, channels * 4); u16(o, 32); u16(o, 0);
    o.write("fact", 4); u32(o, 4); u32(o, frames);
    o.write("data", 4); u32(o, bytes);
    for (uint32_t i = 0; i < frames; ++i) {
        o.write(reinterpret_cast<const char *>(&a.left[i]), 4);
        o.write(reinterpret_cast<const char *>(&a.right[i]), 4);
    }
    if (!o) { err = "write failed for " + path; return false; }
    return true;
}

Levels measure(const Audio &a) {
    double peak = 0, sum = 0, activeSum = 0;
    size_t n = a.frames() * 2, active = 0;
    for (size_t i = 0; i < a.frames(); ++i) {
        for (float s : {a.left[i], a.right[i]}) {
            double v = std::fabs(s);
            peak = std::max(peak, v);
            sum += v * v;
            if (v > 1e-4) { activeSum += v * v; ++active; }
        }
    }
    Levels l;
    l.peakDb = db(peak);
    l.rmsDb = n ? db(std::sqrt(sum / n)) : -240.0;
    l.activeRmsDb = active ? db(std::sqrt(activeSum / active)) : -240.0;
    l.silent = peak < 1e-4;
    return l;
}

} // namespace wl
