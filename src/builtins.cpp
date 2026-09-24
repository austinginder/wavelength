#include "builtins.hpp"

#include "dsp.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace wl {

namespace {

using dsp::Biquad;
constexpr double TAU = 2 * dsp::kPi;

struct Voice {
    Audio &out;
    double sr;
    size_t start;
    // add a mono sample at time t (seconds after the hit) with a pan (-1..1)
    inline void add(size_t i, double v, double pan = 0) {
        const size_t idx = start + i;
        if (idx >= out.frames()) return;
        out.left[idx] += (float)(v * std::min(1.0, 1.0 - pan));
        out.right[idx] += (float)(v * std::min(1.0, 1.0 + pan));
    }
    size_t len(double secs) const { return (size_t)(secs * sr); }
};

// exponential frequency sweep from f0 to f1 over `dur` seconds, then held
inline double sweep(double t, double f0, double f1, double dur) { return t < dur ? f0 * std::pow(f1 / f0, t / dur) : f1; }

// ---- drums -------------------------------------------------------------------------------
void kick(Voice v, double vel, dsp::Noise &nz) {
    double ph = 0;
    Biquad hp; hp.set(Biquad::HighPass, 2500, 0.7071, 0, v.sr);
    const double norm = 1.0 / std::tanh(1.6);
    for (size_t i = 0, n = v.len(0.9); i < n; ++i) {
        const double t = i / v.sr;
        ph += TAU * sweep(t, 170, 43, 0.1) / v.sr;
        const double body = std::tanh(std::sin(ph) * 1.6) * norm * 0.95 * vel * std::exp(-std::max(0.0, t - 0.01) / 0.16);
        const double click = t < 0.05 ? hp.process(nz.next()) * 0.25 * vel * std::exp(-t / 0.004) : 0;
        v.add(i, body + click);
    }
}
void snare(Voice v, double vel, dsp::Noise &nz) {
    Biquad bp; bp.set(Biquad::BandPass, 1900, 0.7, 0, v.sr);
    double ph = 0;
    for (size_t i = 0, n = v.len(0.6); i < n; ++i) {
        const double t = i / v.sr;
        ph += TAU * sweep(t, 190, 160, 0.05) / v.sr;
        const double tri = 2.0 / dsp::kPi * std::asin(std::sin(ph));
        v.add(i, bp.process(nz.next()) * 0.75 * vel * std::exp(-t / 0.075) + tri * 0.45 * vel * std::exp(-t / 0.05));
    }
}
void rim(Voice v, double vel, dsp::Noise &nz) {
    Biquad bp; bp.set(Biquad::BandPass, 3400, 2, 0, v.sr);
    for (size_t i = 0, n = v.len(0.15); i < n; ++i) v.add(i, bp.process(nz.next()) * 0.6 * vel * std::exp(-(i / v.sr) / 0.018));
}
void hat(Voice v, double vel, dsp::Noise &nz, double tau) {
    Biquad hp; hp.set(Biquad::HighPass, 7600, 0.7071, 0, v.sr);
    for (size_t i = 0, n = v.len(tau * 8); i < n; ++i) v.add(i, hp.process(nz.next()) * 0.3 * vel * std::exp(-(i / v.sr) / tau), 0.25);
}
void cymbal(Voice v, double vel, dsp::Noise &nzl, dsp::Noise &nzr, double tau, double level) {
    Biquad hl, hr, pl, pr;
    hl.set(Biquad::HighPass, 3600, 0.7071, 0, v.sr); hr = hl;
    pl.set(Biquad::Peak, 6200, 1, 5, v.sr); pr = pl;
    for (size_t i = 0, n = v.len(tau * 4.5); i < n; ++i) {
        const double e = level * vel * std::exp(-(i / v.sr) / tau);
        const size_t idx = v.start + i;
        if (idx >= v.out.frames()) break;
        v.out.left[idx] += (float)(pl.process(hl.process(nzl.next())) * e);
        v.out.right[idx] += (float)(pr.process(hr.process(nzr.next())) * e);
    }
}
void tom(Voice v, double vel, double f) {
    double ph = 0;
    for (size_t i = 0, n = v.len(1.4); i < n; ++i) {
        const double t = i / v.sr;
        ph += TAU * sweep(t, f * 1.5, f, 0.08) / v.sr;
        v.add(i, std::sin(ph) * 0.85 * vel * std::exp(-t / 0.22));
    }
}

// ---- fx ----------------------------------------------------------------------------------
void impact(Voice v, double vel, dsp::Noise &nz) {
    double ph = 0;
    Biquad lp;
    for (size_t i = 0, n = v.len(5); i < n; ++i) {
        const double t = i / v.sr;
        ph += TAU * sweep(t, 82, 27, 1.5) / v.sr;
        double s = std::sin(ph) * 1.1 * vel * std::exp(-std::max(0.0, t - 0.02) / 0.95);
        if (t < 2.5) {
            if (i % 32 == 0) lp.set(Biquad::LowPass, 300 + 2300 * std::exp(-t / 0.3), 0.7071, 0, v.sr);
            s += lp.process(nz.next()) * 0.7 * vel * std::exp(-t / 0.32);
        }
        v.add(i, s);
    }
}
void riser(Voice v, double vel, double dur, dsp::Noise &nzl, dsp::Noise &nzr) {
    Biquad bl, br, slp;
    slp.set(Biquad::LowPass, 2800, 0.7071, 0, v.sr);
    double ph = 0;
    const size_t n = v.len(dur);
    for (size_t i = 0; i < n; ++i) {
        const double t = i / v.sr, p = t / dur;
        if (i % 32 == 0) { bl.set(Biquad::BandPass, 250 * std::pow(9000.0 / 250, p), 3, 0, v.sr); br = bl; }
        const double g = 0.0001 * std::pow(0.55 * vel / 0.0001, p);
        ph += TAU * (110 * std::pow(8.0, p)) / v.sr;
        const double saw = 2 * (ph / TAU - std::floor(ph / TAU + 0.5));
        const double s = slp.process(saw) * 0.07 * vel * p * p;
        const size_t idx = v.start + i;
        if (idx >= v.out.frames()) break;
        v.out.left[idx] += (float)(bl.process(nzl.next()) * g + s);
        v.out.right[idx] += (float)(br.process(nzr.next()) * g + s);
    }
}
void reverseSwell(Voice v, double vel, double dur, dsp::Noise &nzl, dsp::Noise &nzr) {
    Biquad hl, hr;
    hl.set(Biquad::HighPass, 2200, 0.7071, 0, v.sr); hr = hl;
    const size_t n = v.len(dur);
    for (size_t i = 0; i < n; ++i) {
        const double e = std::pow((double)i / n, 3.2) * 0.55 * vel;
        const size_t idx = v.start + i;
        if (idx >= v.out.frames()) break;
        v.out.left[idx] += (float)(hl.process(nzl.next()) * e);
        v.out.right[idx] += (float)(hr.process(nzr.next()) * e);
    }
}
void subDrop(Voice v, double vel) {
    double ph = 0;
    for (size_t i = 0, n = v.len(2.5); i < n; ++i) {
        const double t = i / v.sr;
        ph += TAU * sweep(t, 62, 30, 1.2) / v.sr;
        v.add(i, std::sin(ph) * 0.9 * vel * std::exp(-t / 0.7));
    }
}

} // namespace

bool isBuiltin(const std::string &plugin) { return plugin.rfind("builtin:", 0) == 0; }

bool renderBuiltin(const std::string &plugin, const Job &job, const Track &track, Audio &out,
                   std::vector<std::string> &warnings, std::string &err) {
    const std::string kind = plugin.substr(8);
    if (kind != "drums" && kind != "fx") { err = "unknown built-in instrument '" + plugin + "' (use builtin:drums or builtin:fx)"; return false; }
    const double sr = job.sampleRate;
    dsp::Noise nz(12345), nzl(777), nzr(4242);
    std::map<int, int> unmapped;
    for (const auto &n : track.notes) {
        Voice v{out, sr, (size_t)std::llround(n.start * sr)};
        const double vel = n.velocity;
        if (kind == "drums") {
            switch (n.key) {
            case 35: case 36: kick(v, vel, nz); break;
            case 37: rim(v, vel, nz); break;
            case 38: case 40: snare(v, vel, nz); break;
            case 42: case 44: hat(v, vel, nz, 0.022); break;
            case 46: hat(v, vel, nz, 0.17); break;
            case 49: case 57: cymbal(v, vel, nzl, nzr, 0.9, 0.42); break;
            case 51: case 59: cymbal(v, vel, nzl, nzr, 0.45, 0.22); break;
            case 41: case 43: tom(v, vel, 82); break;
            case 45: case 47: tom(v, vel, 118); break;
            case 48: case 50: tom(v, vel, 160); break;
            default: unmapped[n.key]++;
            }
        } else {
            switch (n.key) {
            case 48: impact(v, vel, nz); break;
            case 50: riser(v, vel, n.length, nzl, nzr); break;
            case 52: reverseSwell(v, vel, n.length, nzl, nzr); break;
            case 53: subDrop(v, vel); break;
            default: unmapped[n.key]++;
            }
        }
    }
    for (auto &[k, c] : unmapped) warnings.push_back(std::to_string(c) + " note(s) on key " + std::to_string(k) + " have no " + kind + " sound");
    return true;
}

} // namespace wl
