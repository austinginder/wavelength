#include "loudness.hpp"

#include "dsp.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace wl {

namespace {
// K-weighting coefficients derived for any sample rate (the BS.1770 filters are specified
// at 48 kHz; these are the analogue prototypes re-sampled with the bilinear transform).
void kWeighting(double sr, dsp::Biquad &shelf, dsp::Biquad &hp) {
    {   // stage 1: high shelf, +4 dB above ~1.7 kHz
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(dsp::kPi * f0 / sr), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf.setRaw((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                     2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
    }
    {   // stage 2: RLB high-pass at ~38 Hz
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(dsp::kPi * f0 / sr), a0 = 1.0 + K / Q + K * K;
        hp.setRaw(1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
    }
}
} // namespace

double integratedLufs(const Audio &a, int sampleRate, size_t from, size_t to) {
    to = std::min(to, a.frames());
    if (to <= from) return -120;
    const size_t n = to - from, block = (size_t)(0.4 * sampleRate), hop = (size_t)(0.1 * sampleRate);
    if (n < block) return -120;

    // K-weighted squared signal, summed over channels
    std::vector<double> sq(n, 0.0);
    for (int ch = 0; ch < 2; ++ch) {
        dsp::Biquad s, h;
        kWeighting(sampleRate, s, h);
        const auto &x = ch ? a.right : a.left;
        for (size_t i = 0; i < n; ++i) { const double y = h.process(s.process(x[from + i])); sq[i] += y * y; }
    }
    // prefix sums for fast block means
    std::vector<double> pre(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + sq[i];

    std::vector<double> z;   // mean power of each 400 ms block
    for (size_t s = 0; s + block <= n; s += hop) z.push_back((pre[s + block] - pre[s]) / block);
    auto lufs = [](double p) { return -0.691 + 10.0 * std::log10(std::max(p, 1e-20)); };

    double sum = 0; size_t cnt = 0;
    for (double p : z) if (lufs(p) > -70.0) { sum += p; ++cnt; }
    if (!cnt) return -120;
    const double relGate = lufs(sum / cnt) - 10.0;
    sum = 0; cnt = 0;
    for (double p : z) if (lufs(p) > -70.0 && lufs(p) > relGate) { sum += p; ++cnt; }
    return cnt ? lufs(sum / cnt) : -120;
}

} // namespace wl
