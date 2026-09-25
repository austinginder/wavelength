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

namespace {
// Prefix sums of the K-weighted power (summed over channels) of frames [from, to).
std::vector<double> weightedPower(const Audio &a, int sampleRate, size_t from, size_t n) {
    std::vector<double> sq(n, 0.0);
    for (int ch = 0; ch < 2; ++ch) {
        dsp::Biquad s, h;
        kWeighting(sampleRate, s, h);
        const auto &x = ch ? a.right : a.left;
        for (size_t i = 0; i < n; ++i) { const double y = h.process(s.process(x[from + i])); sq[i] += y * y; }
    }
    std::vector<double> pre(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + sq[i];
    return pre;
}
double lufs(double p) { return -0.691 + 10.0 * std::log10(std::max(p, 1e-20)); }
} // namespace

double integratedLufs(const Audio &a, int sampleRate, size_t from, size_t to) {
    to = std::min(to, a.frames());
    if (to <= from) return -120;
    const size_t n = to - from, block = (size_t)(0.4 * sampleRate), hop = (size_t)(0.1 * sampleRate);
    if (n < block) {   // shorter than one gating block (a 0.2 s analyze window, a one-hit section): its plain K-weighted loudness
        if (n < (size_t)(0.01 * sampleRate)) return -120;
        const std::vector<double> pre = weightedPower(a, sampleRate, from, n);
        const double l = lufs(pre[n] / n);
        return l > -70.0 ? l : -120;
    }
    const std::vector<double> pre = weightedPower(a, sampleRate, from, n);

    std::vector<double> z;   // mean power of each 400 ms block
    for (size_t s = 0; s + block <= n; s += hop) z.push_back((pre[s + block] - pre[s]) / block);

    double sum = 0; size_t cnt = 0;
    for (double p : z) if (lufs(p) > -70.0) { sum += p; ++cnt; }
    if (!cnt) return -120;
    const double relGate = lufs(sum / cnt) - 10.0;
    sum = 0; cnt = 0;
    for (double p : z) if (lufs(p) > -70.0 && lufs(p) > relGate) { sum += p; ++cnt; }
    return cnt ? lufs(sum / cnt) : -120;
}

double loudnessRange(const Audio &a, int sampleRate, size_t from, size_t to) {
    to = std::min(to, a.frames());
    if (to <= from) return 0;
    const size_t n = to - from, window = (size_t)(3.0 * sampleRate), hop = (size_t)(0.1 * sampleRate);
    if (n < window) return 0;
    const std::vector<double> pre = weightedPower(a, sampleRate, from, n);
    std::vector<double> z;   // short-term (3 s) mean powers above the absolute gate
    for (size_t s = 0; s + window <= n; s += hop) {
        const double p = (pre[s + window] - pre[s]) / window;
        if (lufs(p) > -70.0) z.push_back(p);
    }
    if (z.empty()) return 0;
    double sum = 0;
    for (double p : z) sum += p;
    const double relGate = lufs(sum / z.size()) - 20.0;
    std::vector<double> l;
    for (double p : z) if (lufs(p) > relGate) l.push_back(lufs(p));
    if (l.size() < 2) return 0;
    std::sort(l.begin(), l.end());
    auto pct = [&](double q) { return l[(size_t)std::lround(q * (l.size() - 1))]; };
    return pct(0.95) - pct(0.10);
}

} // namespace wl
