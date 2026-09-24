#include "wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
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

bool readWav(const std::string &path, Audio &out, int &sampleRate, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot read " + path; return false; }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t i) { return (uint32_t)(d[i] | d[i + 1] << 8); };
    auto u32 = [&](size_t i) { return (uint32_t)d[i] | (uint32_t)d[i + 1] << 8 | (uint32_t)d[i + 2] << 16 | (uint32_t)d[i + 3] << 24; };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) { err = path + " is not a WAV file"; return false; }
    uint32_t format = 0, channels = 0, bits = 0;
    size_t data = 0, len = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t n = u32(p + 4);
        if (!std::memcmp(&d[p], "fmt ", 4) && p + 24 <= d.size()) {
            format = u16(p + 8); channels = u16(p + 10); sampleRate = (int)u32(p + 12); bits = u16(p + 22);
            if (format == 0xFFFE && p + 34 <= d.size()) format = u16(p + 32);
        } else if (!std::memcmp(&d[p], "data", 4)) { data = p + 8; len = std::min<size_t>(n, d.size() - p - 8); }
        p += 8 + n + (n & 1);
    }
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32), flt = format == 3 && (bits == 32 || bits == 64);
    if (!data || !channels || !(pcm || flt)) { err = path + ": unsupported WAV encoding"; return false; }
    const size_t bps = bits / 8, frames = len / (bps * channels);
    out.resize(frames);
    auto get = [&](size_t i) -> float {
        const uint8_t *q = &d[i];
        if (flt) { if (bits == 32) { float f; std::memcpy(&f, q, 4); return f; } double v; std::memcpy(&v, q, 8); return (float)v; }
        switch (bits) {
        case 8: return (q[0] - 128) / 128.f;
        case 16: return (int16_t)(q[0] | q[1] << 8) / 32768.f;
        case 24: return (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.f;
        default: return (float)((int32_t)(q[0] | q[1] << 8 | q[2] << 16 | (uint32_t)q[3] << 24) / 2147483648.0);
        }
    };
    for (size_t f = 0; f < frames; ++f) {
        const size_t at = data + f * bps * channels;
        out.left[f] = get(at);
        out.right[f] = channels > 1 ? get(at + bps) : out.left[f];
    }
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
