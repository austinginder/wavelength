#pragma once
// Small DSP building blocks shared by the built-in effects and instruments.
#include <cmath>
#include <cstdint>
#include <vector>

namespace wl::dsp {

constexpr double kPi = 3.14159265358979323846;
inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double lin) { return lin > 1e-12 ? 20.0 * std::log10(lin) : -240.0; }

// RBJ cookbook biquad, transposed direct form II. One instance per channel.
struct Biquad {
    enum Type { LowPass, HighPass, BandPass, Peak, LowShelf, HighShelf };
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set(Type type, double freq, double q, double gainDb, double sr) {
        freq = std::fmin(std::fmax(freq, 5.0), sr * 0.49);
        q = std::fmax(q, 0.05);
        const double A = std::pow(10.0, gainDb / 40.0), w = 2 * kPi * freq / sr, c = std::cos(w), s = std::sin(w);
        const double alpha = s / (2 * q);
        double B0, B1, B2, A0, A1, A2;
        switch (type) {
        case LowPass:  B0 = (1 - c) / 2; B1 = 1 - c; B2 = (1 - c) / 2; A0 = 1 + alpha; A1 = -2 * c; A2 = 1 - alpha; break;
        case HighPass: B0 = (1 + c) / 2; B1 = -(1 + c); B2 = (1 + c) / 2; A0 = 1 + alpha; A1 = -2 * c; A2 = 1 - alpha; break;
        case BandPass: B0 = alpha; B1 = 0; B2 = -alpha; A0 = 1 + alpha; A1 = -2 * c; A2 = 1 - alpha; break;
        case Peak:     B0 = 1 + alpha * A; B1 = -2 * c; B2 = 1 - alpha * A; A0 = 1 + alpha / A; A1 = -2 * c; A2 = 1 - alpha / A; break;
        case LowShelf: {
            const double sq = 2 * std::sqrt(A) * alpha;
            B0 = A * ((A + 1) - (A - 1) * c + sq); B1 = 2 * A * ((A - 1) - (A + 1) * c); B2 = A * ((A + 1) - (A - 1) * c - sq);
            A0 = (A + 1) + (A - 1) * c + sq; A1 = -2 * ((A - 1) + (A + 1) * c); A2 = (A + 1) + (A - 1) * c - sq; break;
        }
        case HighShelf: default: {
            const double sq = 2 * std::sqrt(A) * alpha;
            B0 = A * ((A + 1) + (A - 1) * c + sq); B1 = -2 * A * ((A - 1) + (A + 1) * c); B2 = A * ((A + 1) + (A - 1) * c - sq);
            A0 = (A + 1) - (A - 1) * c + sq; A1 = 2 * ((A - 1) - (A + 1) * c); A2 = (A + 1) - (A - 1) * c - sq; break;
        }
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    // coefficients given directly (already normalised by a0)
    void setRaw(double nb0, double nb1, double nb2, double na1, double na2) { b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2; }

    inline double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct OnePoleLP {
    double a = 0, z = 0;
    void set(double freq, double sr) { a = 1.0 - std::exp(-2 * kPi * freq / sr); }
    inline double process(double x) { z += a * (x - z); return z; }
};

// Deterministic white noise (xorshift), so renders are bit-for-bit repeatable.
struct Noise {
    uint32_t s;
    explicit Noise(uint32_t seed = 22222) : s(seed ? seed : 1) {}
    inline double next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s / 4294967295.0) * 2.0 - 1.0;
    }
};

// Circular delay line with fractional (linear-interpolated) reads.
struct DelayLine {
    std::vector<float> buf;
    size_t w = 0;
    void resize(size_t n) { buf.assign(n + 2, 0.f); w = 0; }
    inline void push(double x) { buf[w] = (float)x; w = (w + 1) % buf.size(); }
    inline double tap(double delay) const {   // delay in samples, >= 1
        const double pos = (double)w - delay;
        double p = std::fmod(pos, (double)buf.size());
        if (p < 0) p += buf.size();
        const size_t i0 = (size_t)p, i1 = (i0 + 1) % buf.size();
        const double f = p - i0;
        return buf[i0] * (1 - f) + buf[i1] * f;
    }
};

} // namespace wl::dsp
