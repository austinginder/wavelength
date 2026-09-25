#include "analyze.hpp"

#include "dsp.hpp"
#include "effects.hpp"
#include "loudness.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

namespace wl {

namespace {

// in-place radix-2 FFT, n a power of two
void fft(std::vector<std::complex<double>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * dsp::kPi / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
    return v[v.size() / 2];
}

// YIN fundamental of one frame (0 = unvoiced); cmnd, when given, receives the cumulative mean
// normalized difference (index = lag in samples) for correctSubharmonic
double yin(const float *x, size_t n, double sr, double fmin, double fmax, std::vector<double> *cmnd = nullptr) {
    const size_t tauMin = (size_t)(sr / fmax), tauMax = std::min(n / 2, (size_t)(sr / fmin));
    if (tauMax <= tauMin + 2) return 0;
    std::vector<double> d(tauMax + 1, 0);
    const size_t w = n - tauMax;
    for (size_t tau = 1; tau <= tauMax; ++tau) {
        double s = 0;
        for (size_t i = 0; i < w; ++i) { const double e = x[i] - x[i + tau]; s += e * e; }
        d[tau] = s;
    }
    double run = 0;   // cumulative mean normalized difference
    std::vector<double> c(tauMax + 1, 1);
    for (size_t tau = 1; tau <= tauMax; ++tau) { run += d[tau]; c[tau] = run > 0 ? d[tau] * (double)tau / run : 1; }
    if (cmnd) *cmnd = c;
    for (size_t tau = tauMin; tau < tauMax; ++tau) {
        if (c[tau] < 0.15) {
            while (tau + 1 < tauMax && c[tau + 1] < c[tau]) ++tau;
            // parabolic interpolation around the dip
            const double a = c[tau - 1], b = c[tau], cc = c[tau + 1], den = a - 2 * b + cc;
            const double t = den != 0 ? (double)tau + 0.5 * (a - cc) / den : (double)tau;
            return sr / t;
        }
    }
    return 0;
}

// Energy near frequency f in a Hann-windowed frame (Goertzel), the best of f and f +-1 %
// so a note with vibrato still lands on its harmonics.
double energyAt(const float *x, size_t n, double sr, double f) {
    double best = 0;
    for (double d : {0.99, 1.0, 1.01}) {
        const double w = 2 * M_PI * f * d / sr, cw = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (size_t i = 0; i < n; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)(n - 1));
            const double s0 = x[i] * hann + cw * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        best = std::max(best, s1 * s1 + s2 * s2 - cw * s1 * s2);
    }
    return best;
}

// Lowest normalized difference within 3 % of a lag.
double dipNear(const std::vector<double> &c, double lag) {
    double best = 1e9;
    for (size_t t = (size_t)std::max(1.0, lag * 0.97); t <= (size_t)(lag * 1.03) + 1 && t < c.size(); ++t) best = std::min(best, c[t]);
    return best;
}

// YIN can lock onto a multiple of the period when a waveform repeats exactly only every few
// cycles (a chip oscillator whose edges fall on the sample grid in a 3-cycle pattern read a B5
// as E4). The fundamental is k * f0 when both hold:
//  - nearly all the energy sits on every k-th harmonic of f0 (the rest 7 dB or more under it;
//    a real square wave at f0 puts 9 dB more off them than on them), and
//  - the signal is nearly as periodic at 1/k of the period as at the period YIN chose. A real
//    low note whose k-th harmonic dominates (a cello's C2 is mostly its 3rd harmonic) is 14-100x
//    more periodic at its true period; the chip B5 was only 3.5x.
double correctSubharmonic(const float *x, size_t n, double sr, double f0, double fmax, const std::vector<double> &c) {
    const double top = std::min(sr * 0.45, 16000.0), period = sr / f0, atF0 = std::max(dipNear(c, period), 1e-4);
    for (int k = 5; k >= 2; --k) {
        if (f0 * k > fmax || dipNear(c, period / k) > 6 * atF0) continue;
        double onK = 0, offK = 0;
        for (int h = 1; h <= 12 && h * f0 < top; ++h) (h % k ? offK : onK) += energyAt(x, n, sr, h * f0);
        if (onK > 0 && offK < 0.2 * onK) return f0 * k;
    }
    return f0;
}

} // namespace

std::string keyName(int key) {
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (key < 0) return "";
    return std::string(names[key % 12]) + std::to_string(key / 12 - 1);
}

Analysis analyzeAudio(const Audio &in, int sampleRate, double start, double end) {
    Analysis r;
    const double sr = sampleRate;
    const size_t a0 = std::min(in.frames(), (size_t)std::max(0.0, start * sr));
    const size_t a1 = end > 0 ? std::min(in.frames(), (size_t)(end * sr)) : in.frames();
    Audio a;
    a.left.assign(in.left.begin() + (long)a0, in.left.begin() + (long)std::max(a0, a1));
    a.right.assign(in.right.begin() + (long)a0, in.right.begin() + (long)std::max(a0, a1));
    const size_t n = a.frames();
    r.seconds = n / sr;
    if (n == 0) return r;
    const Levels lv = measure(a);
    r.peakDb = lv.peakDb;
    r.rmsDb = lv.rmsDb;
    r.silent = lv.silent;
    r.truePeakDb = truePeakDb(a);
    r.lufs = integratedLufs(a, sampleRate);
    r.lra = loudnessRange(a, sampleRate);
    if (r.silent) return r;

    std::vector<float> mono(n);
    for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (a.left[i] + a.right[i]);

    // envelope: RMS in 5 ms hops
    const size_t hop = std::max<size_t>(1, (size_t)(0.005 * sr));
    std::vector<double> env;
    for (size_t i = 0; i < n; i += hop) {
        double s = 0;
        const size_t e = std::min(n, i + hop);
        for (size_t k = i; k < e; ++k) s += (double)mono[k] * mono[k];
        env.push_back(std::sqrt(s / (double)(e - i)));
    }
    const size_t pk = (size_t)(std::max_element(env.begin(), env.end()) - env.begin());
    const double peak = env[pk], floorLin = dsp::dbToLin(-60);
    size_t first = env.size(), last = 0;
    std::vector<double> active;
    for (size_t i = 0; i < env.size(); ++i)
        if (env[i] > floorLin) { first = std::min(first, i); last = i; active.push_back(env[i]); }
    r.activeSeconds = (double)active.size() * hop / sr;
    if (first < env.size()) { r.firstSoundSeconds = start + (double)first * hop / sr; r.lastSoundSeconds = start + (double)(last + 1) * hop / sr; }
    // attack: from first reaching 10% of the peak to first reaching 90% of it
    size_t i10 = 0;
    while (i10 < pk && env[i10] < 0.1 * peak) ++i10;
    size_t i90 = i10;
    while (i90 < pk && env[i90] < 0.9 * peak) ++i90;
    r.attackMs = (double)(i90 - i10) * hop / sr * 1000;
    size_t id = pk;
    while (id < env.size() && env[id] > peak * 0.1) ++id;
    r.decayMs = id < env.size() ? (double)(id - pk) * hop / sr * 1000 : 0;
    r.sustainDb = dsp::linToDb(median(active) / peak);

    // stereo image
    double mm = 0, ss = 0, lr = 0, ll = 0, rr = 0;
    for (size_t i = 0; i < n; ++i) {
        const double l = a.left[i], rt = a.right[i], m = 0.5 * (l + rt), s = 0.5 * (l - rt);
        mm += m * m; ss += s * s; lr += l * rt; ll += l * l; rr += rt * rt;
    }
    r.width = mm > 0 ? std::sqrt(ss / mm) : 0;
    r.correlation = ll > 0 && rr > 0 ? lr / std::sqrt(ll * rr) : 1;

    // spectrum and onsets from 2048-sample frames (hop 512)
    const size_t N = 2048, H = 512;
    std::vector<double> power(N / 2 + 1, 0), prevMag(N / 2 + 1, 0), flux;
    std::vector<std::complex<double>> buf(N);
    std::vector<double> win(N);
    for (size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2 * dsp::kPi * (double)i / (double)(N - 1));
    size_t frames = 0;
    for (size_t p = 0; p + N <= n || (p == 0 && n > 0); p += H) {
        double e = 0;
        for (size_t i = 0; i < N; ++i) {
            const double v = p + i < n ? mono[p + i] : 0.0;
            buf[i] = v * win[i];
            e += v * v;
        }
        fft(buf);
        double f = 0;
        for (size_t k = 0; k <= N / 2; ++k) {
            const double m = std::abs(buf[k]);
            if (e / N > floorLin * floorLin) power[k] += m * m;
            f += std::max(0.0, m - prevMag[k]);
            prevMag[k] = m;
        }
        flux.push_back(f);
        if (e / N > floorLin * floorLin) ++frames;
        if (p + N >= n) break;
    }
    if (frames) {
        double tot = 0, wsum = 0;
        for (size_t k = 1; k <= N / 2; ++k) { tot += power[k]; wsum += power[k] * k * sr / N; }
        r.centroidHz = tot > 0 ? wsum / tot : 0;
        double acc = 0;
        for (size_t k = 1; k <= N / 2; ++k) { acc += power[k]; if (acc >= 0.85 * tot) { r.rolloffHz = (double)k * sr / N; break; } }
        const double edges[7] = {0, 60, 250, 2000, 6000, 12000, 1e9};
        for (int b = 0; b < 6; ++b) {
            double e = 0;
            for (size_t k = 1; k <= N / 2; ++k) { const double hz = (double)k * sr / N; if (hz >= edges[b] && hz < edges[b + 1]) e += power[k]; }
            r.bandsDb[b] = dsp::linToDb(std::sqrt(e / std::max(1e-30, tot)));
        }
    }
    // onsets: spectral-flux peaks above a moving median threshold, at least 50 ms apart
    if (flux.size() > 3) {
        const size_t W = 8;
        double lastOn = -1;
        const double mx = *std::max_element(flux.begin(), flux.end());
        for (size_t i = 0; i + 1 < flux.size(); ++i) {   // i = 0: a sound that starts with the file
            std::vector<double> local(flux.begin() + (long)(i > W ? i - W : 0), flux.begin() + (long)std::min(flux.size(), i + W + 1));
            const double thr = median(local) * 1.5 + mx * 0.05;
            if (flux[i] > thr && (i == 0 || flux[i] >= flux[i - 1]) && flux[i] > flux[i + 1]) {
                if (i == 0 && start > 0) continue;   // a window cut into a sound: not an onset
                // flux peaks when the attack reaches the middle of the Hann window: frame start + N/2 (+ H/4 measured on
                // a click track) is the attack, within one hop (±5 ms)
                const double t = i == 0 ? start : start + (double)(i * H + N / 2 + H / 4) / sr;
                if (lastOn < 0 || t - lastOn > 0.05) { r.onsets.push_back(t); lastOn = t; }
            }
        }
        if (r.onsets.size() > 2000) r.onsets.resize(2000);
    }

    // pitch: YIN over active 4096-sample frames (fundamentals 30 Hz - 4 kHz)
    std::vector<double> f0s;
    size_t voicedTried = 0;
    const size_t P = n >= 4096 ? 4096 : 2048;   // a short window (one slap-bass note) still gets a reading, down to ~50 Hz
    const size_t stride = std::max<size_t>(P / 2, (n / 60 / (P / 2)) * (P / 2));   // at most ~60 frames: YIN is costly
    for (size_t p = 0; p + P <= n; p += stride) {
        double e = 0;
        for (size_t i = 0; i < P; ++i) e += (double)mono[p + i] * mono[p + i];
        if (std::sqrt(e / P) < peak * 0.1) continue;
        ++voicedTried;
        std::vector<double> cmnd;
        const double f = yin(&mono[p], P, sr, P == 4096 ? 30 : 50, 4000, &cmnd);
        if (f > 0) f0s.push_back(correctSubharmonic(&mono[p], P, sr, f, 4000, cmnd));
        if (voicedTried >= 400) break;
    }
    if (!f0s.empty()) {
        r.pitchHz = median(f0s);
        r.pitchConfidence = (double)f0s.size() / (double)voicedTried;
        const double k = 69 + 12 * std::log2(r.pitchHz / 440.0);
        r.pitchKey = (int)std::lround(k);
        r.pitchCents = (k - r.pitchKey) * 100;
    }
    return r;
}

nlohmann::json analysisToJson(const Analysis &x, bool withOnsets) {
    auto r1 = [](double v) { return std::round(v * 10) / 10; };
    nlohmann::json j = {
        {"seconds", r1(x.seconds)}, {"silent", x.silent}, {"lufs", r1(x.lufs)}, {"peakDb", r1(x.peakDb)},
        {"truePeakDb", r1(x.truePeakDb)}, {"lra", r1(x.lra)}, {"rmsDb", r1(x.rmsDb)},
        {"pitch", {{"hz", r1(x.pitchHz)}, {"note", keyName(x.pitchKey)}, {"key", x.pitchKey}, {"cents", std::lround(x.pitchCents)},
                   {"confidence", std::round(x.pitchConfidence * 100) / 100}}},
        {"spectrum", {{"centroidHz", std::lround(x.centroidHz)}, {"rolloffHz", std::lround(x.rolloffHz)},
                      {"bandsDb", {{"sub", r1(x.bandsDb[0])}, {"bass", r1(x.bandsDb[1])}, {"lowMid", r1(x.bandsDb[2])},
                                   {"highMid", r1(x.bandsDb[3])}, {"presence", r1(x.bandsDb[4])}, {"air", r1(x.bandsDb[5])}}}}},
        {"stereo", {{"width", std::round(x.width * 100) / 100}, {"correlation", std::round(x.correlation * 100) / 100}}},
        {"envelope", {{"attackMs", std::lround(x.attackMs)}, {"decayMs", std::lround(x.decayMs)}, {"sustainDb", r1(x.sustainDb)},
                      {"activeSeconds", r1(x.activeSeconds)}, {"firstSound", std::round(x.firstSoundSeconds * 1000) / 1000},
                      {"lastSound", std::round(x.lastSoundSeconds * 1000) / 1000}}},
        {"onsetCount", x.onsets.size()}};
    if (withOnsets) {
        nlohmann::json o = nlohmann::json::array();
        for (double t : x.onsets) o.push_back(std::round(t * 1000) / 1000);
        j["onsets"] = o;
    }
    return j;
}

} // namespace wl
