#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

namespace wl {

namespace {
void u32(std::ofstream &o, uint32_t v) { o.put(char(v)); o.put(char(v >> 8)); o.put(char(v >> 16)); o.put(char(v >> 24)); }
void u16(std::ofstream &o, uint16_t v) { o.put(char(v)); o.put(char(v >> 8)); }
double db(double lin) { return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0; }
} // namespace

bool writeWav(const std::string &path, const Audio &a, int sampleRate, std::string &err, int bits) {
    std::ofstream o(path, std::ios::binary);
    if (!o) { err = "cannot write " + path; return false; }
    if (bits != 16 && bits != 24) bits = 32;
    const uint32_t bps = bits / 8, frames = (uint32_t)a.frames(), channels = 2, bytes = frames * channels * bps;
    const bool isFloat = bits == 32;
    o.write("RIFF", 4); u32(o, 4 + (8 + 18) + (isFloat ? 8 + 4 : 0) + (8 + bytes)); o.write("WAVE", 4);
    o.write("fmt ", 4); u32(o, 18); u16(o, isFloat ? 3 /* IEEE float */ : 1 /* PCM */); u16(o, channels); u32(o, sampleRate);
    u32(o, sampleRate * channels * bps); u16(o, (uint16_t)(channels * bps)); u16(o, (uint16_t)bits); u16(o, 0);
    if (isFloat) { o.write("fact", 4); u32(o, 4); u32(o, frames); }
    o.write("data", 4); u32(o, bytes);
    std::vector<char> buf;
    buf.reserve((size_t)bytes);
    uint32_t rng = 22222;
    auto tpdf = [&] {   // triangular dither, +-1 LSB
        rng = rng * 1664525u + 1013904223u; const double r1 = (rng >> 8) / 16777216.0;
        rng = rng * 1664525u + 1013904223u; const double r2 = (rng >> 8) / 16777216.0;
        return r1 - r2;
    };
    for (uint32_t i = 0; i < frames; ++i)
        for (float s : {a.left[i], a.right[i]}) {
            if (isFloat) { const char *p = reinterpret_cast<const char *>(&s); buf.insert(buf.end(), p, p + 4); continue; }
            const double full = bits == 16 ? 32767.0 : 8388607.0;
            double v = s * full + (bits == 16 ? tpdf() : 0.0);
            const int32_t q = (int32_t)std::lround(std::clamp(v, -full - 1, full));
            buf.push_back((char)(q & 0xff)); buf.push_back((char)((q >> 8) & 0xff));
            if (bits == 24) buf.push_back((char)((q >> 16) & 0xff));
        }
    o.write(buf.data(), (std::streamsize)buf.size());
    if (!o) { err = "write failed for " + path + " (disk full?)"; return false; }
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
