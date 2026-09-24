#include "effects.hpp"

#include "automation.hpp"
#include "dsp.hpp"
#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace wl {

namespace {

using dsp::Biquad;
using dsp::dbToLin;

// mix: 0 = dry only, 1 = wet only
inline float blend(float dry, double wet, double mix) { return (float)(dry * (1.0 - mix) + wet * mix); }

void checkKeys(const json &j, std::initializer_list<const char *> allowed, Effect &fx) {
    for (auto &[k, _] : j.items()) {
        if (k == "type" || k == "automate" || k == "lfo" || k == "bypass") continue;
        bool ok = false;
        for (auto *a : allowed) ok |= k == a;
        if (!ok) fx.warnings.push_back(fx.label + ": unknown setting '" + k + "' ignored");
    }
}

// ---------------------------------------------------------------------------------- gain
struct Gain : Effect {
    Envelope db;
    Gain(const json &j, const Job &job) { label = "gain"; db = param(j, "db", 0, job.tempo); checkKeys(j, {"db"}, *this); }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        for (size_t i = 0; i < a.frames(); ++i) {
            const float g = (float)dbToLin(db.at(i / sr));
            a.left[i] *= g; a.right[i] *= g;
        }
        return true;
    }
};

// ------------------------------------------------------------------------------------ eq
struct Eq : Effect {
    struct Band { Biquad::Type type; double freq, q, gain; };
    std::vector<Band> bands;
    Eq(const json &j, const Job &, std::string &err) {
        label = "eq";
        static const std::map<std::string, Biquad::Type> types = {
            {"highpass", Biquad::HighPass}, {"lowpass", Biquad::LowPass}, {"bandpass", Biquad::BandPass},
            {"peak", Biquad::Peak}, {"lowshelf", Biquad::LowShelf}, {"highshelf", Biquad::HighShelf}};
        for (auto &b : j.value("bands", json::array())) {
            auto t = types.find(b.value("type", "peak"));
            if (t == types.end()) { err = "eq band type must be one of highpass, lowpass, bandpass, peak, lowshelf, highshelf"; return; }
            bands.push_back({t->second, b.value("freq", 1000.0), b.value("q", 0.7071), b.value("gain", 0.0)});
        }
        checkKeys(j, {"bands"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        for (const auto &b : bands) {
            for (int ch = 0; ch < 2; ++ch) {
                Biquad f;
                f.set(b.type, b.freq, b.q, b.gain, c.job.sampleRate);
                auto &x = ch ? a.right : a.left;
                for (auto &s : x) s = (float)f.process(s);
            }
        }
        return true;
    }
};

// -------------------------------------------------------------------------------- filter
struct Filter : Effect {
    Biquad::Type type = Biquad::LowPass;
    Envelope cutoff, q, mix;
    Filter(const json &j, const Job &job, std::string &err) {
        label = "filter";
        const std::string mode = j.value("mode", "lowpass");
        if (mode == "lowpass") type = Biquad::LowPass;
        else if (mode == "highpass") type = Biquad::HighPass;
        else if (mode == "bandpass") type = Biquad::BandPass;
        else { err = "filter mode must be lowpass, highpass or bandpass"; return; }
        cutoff = param(j, "cutoff", 1000, job.tempo, true);
        q = param(j, "resonance", 0.7071, job.tempo);
        mix = param(j, "mix", 1, job.tempo);
        checkKeys(j, {"mode", "cutoff", "resonance", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        Biquad fl, fr;
        for (size_t i = 0; i < a.frames(); ++i) {
            if (i % 32 == 0) {
                const double t = i / sr;
                fl.set(type, cutoff.at(t), q.at(t), 0, sr);
                fr.b0 = fl.b0; fr.b1 = fl.b1; fr.b2 = fl.b2; fr.a1 = fl.a1; fr.a2 = fl.a2;
            }
            const double m = mix.constant() ? mix.at(0) : mix.at(i / sr);
            a.left[i] = blend(a.left[i], fl.process(a.left[i]), m);
            a.right[i] = blend(a.right[i], fr.process(a.right[i]), m);
        }
        return true;
    }
};

// --------------------------------------------------------------------------------- delay
struct Delay : Effect {
    double timeBeats, timeMs, feedback, hp, lp;
    bool pingpong;
    Envelope mix;
    Delay(const json &j, const Job &job) {
        label = "delay";
        timeBeats = j.value("time", 0.75);
        timeMs = j.value("ms", 0.0);
        feedback = std::clamp(j.value("feedback", 0.35), 0.0, 0.97);
        hp = j.value("highpass", 250.0);
        lp = j.value("lowpass", 5000.0);
        pingpong = j.value("pingpong", true);
        mix = param(j, "mix", 0.25, job.tempo);
        checkKeys(j, {"time", "ms", "feedback", "highpass", "lowpass", "pingpong", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        const double secs = timeMs > 0 ? timeMs / 1000.0 : timeBeats * 60.0 / c.job.tempo.bpmAtBeat(0);
        const double d = std::max(1.0, secs * sr);
        dsp::DelayLine L, R;
        L.resize((size_t)d + 4); R.resize((size_t)d + 4);
        Biquad hpl, hpr, lpl, lpr;
        hpl.set(Biquad::HighPass, hp, 0.7071, 0, sr); hpr = hpl;
        lpl.set(Biquad::LowPass, lp, 0.7071, 0, sr); lpr = lpl;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double tl = L.tap(d), tr = R.tap(d);
            const double fbl = lpl.process(hpl.process(tl)) * feedback, fbr = lpr.process(hpr.process(tr)) * feedback;
            const double inL = a.left[i], inR = a.right[i];
            if (pingpong) { L.push((inL + inR) * 0.5 + fbr); R.push(fbl); }
            else { L.push(inL + fbl); R.push(inR + fbr); }
            const double m = mix.constant() ? mix.at(0) : mix.at(i / sr);
            a.left[i] = blend(a.left[i], tl, m);
            a.right[i] = blend(a.right[i], tr, m);
        }
        return true;
    }
};

// -------------------------------------------------------------------------------- reverb
// 8-line feedback delay network: input diffusion, slowly modulated lines, per-line damping,
// Hadamard mixing, T60-accurate decay.
struct Reverb : Effect {
    double decay, size, predelayMs, damping, width, hp;
    Envelope mix;
    Reverb(const json &j, const Job &job) {
        label = "reverb";
        decay = std::max(0.1, j.value("decay", 2.5));
        size = std::clamp(j.value("size", 0.7), 0.0, 1.0);
        predelayMs = std::max(0.0, j.value("predelay", 15.0));
        damping = std::clamp(j.value("damping", 0.5), 0.0, 1.0);
        width = std::clamp(j.value("width", 1.0), 0.0, 1.5);
        hp = j.value("highpass", 150.0);
        mix = param(j, "mix", 0.3, job.tempo);
        checkKeys(j, {"decay", "size", "predelay", "damping", "width", "highpass", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        static const double baseMs[8] = {29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.7, 73.1};
        static const double diffMs[4] = {4.7, 3.6, 12.7, 9.3};
        const double scale = 0.45 + size * 1.1;
        dsp::DelayLine lines[8], diff[4], pre;
        double len[8], gain[8];
        dsp::OnePoleLP damp[8];
        for (int k = 0; k < 8; ++k) {
            len[k] = baseMs[k] * scale * sr / 1000.0;
            lines[k].resize((size_t)(len[k] + 64));
            gain[k] = std::pow(10.0, -3.0 * (len[k] / sr) / decay);
            damp[k].set(18000.0 * (1.0 - damping) + 1500.0 * damping, sr);
        }
        for (int k = 0; k < 4; ++k) diff[k].resize((size_t)(diffMs[k] * sr / 1000.0) + 4);
        const double preLen = std::max(1.0, predelayMs * sr / 1000.0);
        pre.resize((size_t)preLen + 4);
        Biquad inHp;
        inHp.set(Biquad::HighPass, hp, 0.7071, 0, sr);
        double phase = 0;
        const double lfoInc = 2 * dsp::kPi * 0.35 / sr, modDepth = 0.0012 * sr;
        for (size_t i = 0; i < a.frames(); ++i) {
            // input: mono, high-passed, pre-delayed, diffused by four allpasses
            pre.push(inHp.process((a.left[i] + a.right[i]) * 0.5));
            double x = pre.tap(preLen);
            for (int k = 0; k < 4; ++k) {
                const double dl = diffMs[k] * sr / 1000.0, delayed = diff[k].tap(dl);
                const double v = x + 0.62 * delayed;
                diff[k].push(v);
                x = delayed - 0.62 * v;
            }
            // read the lines (two are gently modulated to avoid metallic ringing)
            phase += lfoInc;
            double y[8];
            for (int k = 0; k < 8; ++k) {
                double d = len[k];
                if (k == 1) d += modDepth * std::sin(phase);
                if (k == 6) d += modDepth * std::sin(phase * 1.37 + 1.0);
                y[k] = damp[k].process(lines[k].tap(d)) * gain[k];
            }
            // Hadamard 8x8 (fast Walsh–Hadamard), normalised
            double h[8];
            for (int k = 0; k < 8; ++k) h[k] = y[k];
            for (int step = 1; step < 8; step <<= 1)
                for (int s = 0; s < 8; s += step << 1)
                    for (int k = s; k < s + step; ++k) { const double u = h[k], v = h[k + step]; h[k] = u + v; h[k + step] = u - v; }
            for (int k = 0; k < 8; ++k) lines[k].push(h[k] * 0.35355339 + x * ((k & 1) ? -0.5 : 0.5));
            double wl = (y[0] - y[2] + y[4] - y[6]) * 0.5, wr = (y[1] - y[3] + y[5] - y[7]) * 0.5;
            const double mid = (wl + wr) * 0.5, side = (wl - wr) * 0.5 * width;
            wl = mid + side; wr = mid - side;
            const double m = mix.constant() ? mix.at(0) : mix.at(i / sr);
            a.left[i] = blend(a.left[i], wl, m);
            a.right[i] = blend(a.right[i], wr, m);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------- compressor
struct Compressor : Effect {
    double threshold, ratio, attackMs, releaseMs, knee, makeup, mix;
    Compressor(const json &j, const Job &) {
        label = "compressor";
        threshold = j.value("threshold", -18.0);
        ratio = std::max(1.0, j.value("ratio", 3.0));
        attackMs = std::max(0.05, j.value("attack", 10.0));
        releaseMs = std::max(1.0, j.value("release", 150.0));
        knee = std::max(0.0, j.value("knee", 6.0));
        makeup = j.value("makeup", 0.0);
        mix = std::clamp(j.value("mix", 1.0), 0.0, 1.0);
        checkKeys(j, {"threshold", "ratio", "attack", "release", "knee", "makeup", "mix"}, *this);
    }
    double curve(double x) const {   // static gain computer with soft knee, dB in → dB out
        const double over = x - threshold;
        if (2 * over < -knee) return x;
        if (knee > 0 && 2 * std::fabs(over) <= knee) return x + (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
        return threshold + over / ratio;
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        const double att = std::exp(-1.0 / (attackMs * 0.001 * sr)), rel = std::exp(-1.0 / (releaseMs * 0.001 * sr));
        double g = 0, maxGr = 0;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double x = dsp::linToDb(std::max(std::fabs(a.left[i]), std::fabs(a.right[i])));
            const double gr = curve(x) - x;
            g = gr < g ? att * g + (1 - att) * gr : rel * g + (1 - rel) * gr;
            maxGr = std::min(maxGr, g);
            const float lin = (float)dbToLin(g + makeup);
            a.left[i] = blend(a.left[i], a.left[i] * lin, mix);
            a.right[i] = blend(a.right[i], a.right[i] * lin, mix);
        }
        if (maxGr < -18) warnings.push_back("compressor reached " + std::to_string((int)maxGr) + " dB of gain reduction");
        return true;
    }
};

// inter-sample peak between x[i] and x[i+1] (4x oversampling with a windowed-sinc kernel):
// what a DAC or a lossy encoder will actually reconstruct
inline double interPeak(const std::vector<float> &x, size_t i) {
    static double k[3][8];
    static bool init = false;
    if (!init) {
        for (int p = 0; p < 3; ++p) {
            const double frac = (p + 1) / 4.0;
            for (int t = 0; t < 8; ++t) {
                const double d = (t - 3) - frac, sinc = std::fabs(d) < 1e-9 ? 1 : std::sin(M_PI * d) / (M_PI * d);
                const double w = 0.5 + 0.5 * std::cos(M_PI * d / 4.5);   // Hann window
                k[p][t] = sinc * w;
            }
        }
        init = true;
    }
    const long n = (long)x.size();
    double pk = 0;
    for (int p = 0; p < 3; ++p) {
        double v = 0;
        for (int t = 0; t < 8; ++t) {
            const long j = (long)i + t - 3;
            if (j >= 0 && j < n) v += x[(size_t)j] * k[p][t];
        }
        pk = std::max(pk, std::fabs(v));
    }
    return pk;
}

// ------------------------------------------------------------------------------- limiter
// Look-ahead brickwall: the gain starts falling before a peak arrives, so the output
// never exceeds the ceiling and never clicks.
struct Limiter : Effect {
    double ceiling, releaseMs, lookMs;
    bool truePeak;
    Limiter(const json &j, const Job &) {
        label = "limiter";
        ceiling = j.value("ceiling", -1.0);
        releaseMs = std::max(1.0, j.value("release", 80.0));
        lookMs = std::clamp(j.value("lookahead", 5.0), 0.5, 50.0);
        truePeak = j.value("truePeak", true);
        checkKeys(j, {"ceiling", "release", "lookahead", "truePeak"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate, ceil = dbToLin(ceiling);
        const size_t n = a.frames(), L = std::max<size_t>(1, (size_t)(lookMs * 0.001 * sr));
        std::vector<float> req(n), mn(n);
        for (size_t i = 0; i < n; ++i) {
            double pk = std::max(std::fabs(a.left[i]), std::fabs(a.right[i]));
            if (truePeak && pk > ceil * 0.5) pk = std::max({pk, interPeak(a.left, i), interPeak(a.right, i)});
            req[i] = (float)(pk > ceil ? ceil / pk : 1.0);
        }
        std::deque<size_t> dq;   // forward-looking window minimum over [i, i+L]
        for (size_t ii = n; ii-- > 0;) {
            while (!dq.empty() && req[dq.back()] >= req[ii]) dq.pop_back();
            dq.push_back(ii);
            while (dq.front() > ii + L) dq.pop_front();
            mn[ii] = req[dq.front()];
        }
        const double rel = 1.0 - std::exp(-1.0 / (releaseMs * 0.001 * sr));
        double sum = 0, g = 1, minG = 1, worstAt = 0;
        for (size_t i = 0; i < n; ++i) {
            sum += mn[i];
            if (i >= L) sum -= mn[i - L];
            const double avg = sum / (double)std::min(i + 1, L);      // smooth attack ramp
            g = avg < g ? avg : g + (avg - g) * rel;                 // release
            if (g < minG) { minG = g; worstAt = (double)i; }
            a.left[i] = (float)std::clamp(a.left[i] * g, -ceil, ceil);
            a.right[i] = (float)std::clamp(a.right[i] * g, -ceil, ceil);
        }
        if (dsp::linToDb(minG) < -8) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "limiter: up to %.1f dB of gain reduction (at %.2f s); the input is very hot", -dsp::linToDb(minG), worstAt / sr);
            warnings.push_back(buf);
        }
        return true;
    }
};

// ------------------------------------------------------------------------------ saturate
struct Saturate : Effect {
    Envelope drive, mix;
    Saturate(const json &j, const Job &job) {
        label = "saturate";
        drive = param(j, "drive", 6, job.tempo);
        mix = param(j, "mix", 1, job.tempo);
        checkKeys(j, {"drive", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double t = i / sr, k = dbToLin(drive.at(t)), norm = 1.0 / std::tanh(k), m = mix.at(t);
            a.left[i] = blend(a.left[i], std::tanh(a.left[i] * k) * norm, m);
            a.right[i] = blend(a.right[i], std::tanh(a.right[i] * k) * norm, m);
        }
        return true;
    }
};

// -------------------------------------------------------------------------------- chorus
struct Chorus : Effect {
    double rate, depthMs, delayMs;
    Envelope mix;
    Chorus(const json &j, const Job &job) {
        label = "chorus";
        rate = j.value("rate", 0.3);
        depthMs = j.value("depth", 4.0);
        delayMs = j.value("delay", 14.0);
        mix = param(j, "mix", 0.35, job.tempo);
        checkKeys(j, {"rate", "depth", "delay", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        dsp::DelayLine L, R;
        const size_t cap = (size_t)((delayMs + depthMs + 5) * 0.001 * sr);
        L.resize(cap); R.resize(cap);
        double ph = 0;
        const double inc = 2 * dsp::kPi * rate / sr;
        for (size_t i = 0; i < a.frames(); ++i) {
            L.push(a.left[i]); R.push(a.right[i]);
            ph += inc;
            const double dl = (delayMs + depthMs * 0.5 * (1 + std::sin(ph))) * 0.001 * sr;
            const double dr = (delayMs + depthMs * 0.5 * (1 + std::sin(ph + dsp::kPi / 2))) * 0.001 * sr;
            const double m = mix.constant() ? mix.at(0) : mix.at(i / sr);
            a.left[i] = blend(a.left[i], L.tap(dl), m);
            a.right[i] = blend(a.right[i], R.tap(dr), m);
        }
        return true;
    }
};

// --------------------------------------------------------------------------------- width
struct Width : Effect {
    Envelope amount;
    Width(const json &j, const Job &job) { label = "width"; amount = param(j, "amount", 1, job.tempo); checkKeys(j, {"amount"}, *this); }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double w = amount.at(i / sr), m = (a.left[i] + a.right[i]) * 0.5, s = (a.left[i] - a.right[i]) * 0.5 * w;
            a.left[i] = (float)(m + s); a.right[i] = (float)(m - s);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------- duck
// Sidechain-style pumping keyed from another track's notes (deterministic, no detector).
struct Duck : Effect {
    std::string trigger;
    std::vector<int> keys;
    double attackMs, holdMs, releaseMs;
    Envelope depthDb;
    Duck(const json &j, const Job &job, std::string &err) {
        label = "duck";
        trigger = j.value("trigger", "");
        if (trigger.empty()) err = "duck needs \"trigger\": the name of the track whose notes cause ducking";
        for (auto &k : j.value("keys", json::array())) keys.push_back(parseKey(k));
        depthDb = param(j, "depth", 8.0, job.tempo);   // dB of ducking, automatable (e.g. to 0 as the kick fades out)
        attackMs = std::max(0.5, j.value("attack", 8.0));
        holdMs = std::max(0.0, j.value("hold", 20.0));
        releaseMs = std::max(5.0, j.value("release", 180.0));
        checkKeys(j, {"trigger", "keys", "depth", "attack", "hold", "release"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &err) override {
        const Track *src = nullptr;
        for (const auto &t : c.job.tracks) if (t.name == trigger) src = &t;
        if (!src) { err = "duck: no track named '" + trigger + "'"; return false; }
        std::vector<double> times;
        for (const auto &n : src->notes)
            if (keys.empty() || std::find(keys.begin(), keys.end(), n.key) != keys.end()) times.push_back(n.start);
        std::sort(times.begin(), times.end());
        if (times.empty()) { warnings.push_back("duck: trigger track '" + trigger + "' has no matching notes"); return true; }
        const double sr = c.job.sampleRate, att = attackMs / 1000, hold = holdMs / 1000, rel = releaseMs / 1000;
        size_t k = 0;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double t = i / sr;
            while (k + 1 < times.size() && times[k + 1] <= t) ++k;
            double amount = 0;   // 0 = no duck, 1 = full depth
            for (size_t m = (k > 0 ? k - 1 : 0); m <= k; ++m) {
                const double dt = t - times[m];
                if (dt < 0) continue;
                double v;
                if (dt < att) v = dt / att;
                else if (dt < att + hold) v = 1;
                else if (dt < att + hold + rel) { const double r = (dt - att - hold) / rel; v = 1 - r * r * (3 - 2 * r); }
                else v = 0;
                amount = std::max(amount, v);
            }
            const float g = (float)dbToLin(-std::fabs(depthDb.at(t)) * amount);
            a.left[i] *= g; a.right[i] *= g;
        }
        return true;
    }
};

// ------------------------------------------------------------------------- CLAP plugin
struct PluginFx : Effect {
    PluginSetup setup;
    Envelope mix;
    PluginFx(const json &j, const Job &job, std::string &err) {
        setup.spec = j.value("plugin", "");
        label = setup.spec;
        if (j.contains("state") && !j["state"].is_null()) {
            const auto &s = j["state"];
            setup.stateFile = s.is_string() ? s.get<std::string>() : s.at("file").get<std::string>();
            setup.stateFormat = s.is_object() ? s.value("format", "auto") : "auto";
            if (fs::path(setup.stateFile).is_relative()) setup.stateFile = (fs::path(job.baseDir) / setup.stateFile).string();
        }
        const json params = j.value("params", json::object());
        for (auto &[k, v] : params.items()) setup.params.push_back({k, v.get<double>()});
        if (j.contains("automate"))
            for (auto &[k, v] : j["automate"].items()) if (k != "mix") setup.automation.push_back({k, Envelope::parse(v, job.tempo, false)});
        setup.warmup = j.value("warmup", -1.0);
        setup.preset = j.value("preset", "");
        mix = param(j, "mix", 1, job.tempo);
        (void)err;
    }
    bool process(Audio &a, const FxContext &c, std::string &err) override {
        setup.verbose = c.verbose;
        OpenedPlugin p;
        if (!openPlugin(setup, "effect " + setup.spec, p, err)) return false;
        label = p.name;
        Audio wet;
        wet.resize(a.frames());
        if (!runPlugin(c.job, p, {}, &a, wet, err)) { err = "effect " + p.name + ": " + err; return false; }
        for (auto &w : p.warnings) warnings.push_back(w);
        {   // an effect whose output is just a scaled copy of its input did nothing audible but change the
            // level: typically an unlicensed or demo-mode plugin, or a preset the plugin ignored
            double xy = 0, xx = 0, yy = 0;
            for (size_t i = 0; i < a.frames(); ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    const double x = ch ? a.right[i] : a.left[i], y = ch ? wet.right[i] : wet.left[i];
                    xy += x * y; xx += x * x; yy += y * y;
                }
            if (xx > 1e-6 && yy > 1e-9) {
                const double k = xy / xx, residual = std::max(0.0, yy - k * xy);
                if (residual < yy * 1e-4)
                    warnings.push_back(p.name + " only changed the level (" + std::to_string((int)std::lround(dsp::linToDb(std::fabs(k)))) +
                                       " dB) and otherwise passed the audio through unchanged: licence or demo mode, bypass, or a preset it ignored?");
            } else if (xx > 1e-6 && yy <= 1e-9) warnings.push_back(p.name + " output silence");
        }
        const double sr = c.job.sampleRate;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double m = mix.constant() ? mix.at(0) : mix.at(i / sr);
            a.left[i] = blend(a.left[i], wet.left[i], m);
            a.right[i] = blend(a.right[i], wet.right[i], m);
        }
        return true;
    }
};


// ------------------------------------------------------------------------------ modulation
// Tempo-synced or free LFO settings shared by tremolo, vibrato and rotary: "rate" (Hz or "1/8"),
// "shape", "phase".
Lfo lfoFrom(const json &j, const Job &job, const char *defRate) {
    json spec = {{"rate", j.contains("rate") ? j["rate"] : json(defRate)}, {"shape", j.value("shape", "sine")},
                 {"phase", j.value("phase", 0.0)}, {"depth", 1.0}};
    if (spec["rate"].is_string() && spec["rate"].get<std::string>().find('/') == std::string::npos)
        spec["rate"] = std::stod(spec["rate"].get<std::string>());
    return Lfo::parse(spec, job.tempo);
}

// tremolo / autopan: amplitude LFO; "spread" offsets the right channel's phase (0.5 = autopan)
struct Tremolo : Effect {
    Lfo lfo;
    Envelope depth;
    double spread;
    Tremolo(const json &j, const Job &job) {
        label = "tremolo";
        lfo = lfoFrom(j, job, "1/8");
        depth = param(j, "depth", 0.5, job.tempo);
        spread = j.value("spread", 0.0);
        checkKeys(j, {"rate", "shape", "phase", "depth", "spread"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        Lfo right = lfo;
        right.phase += spread;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double t = i / sr, d = std::clamp(depth.at(t), 0.0, 1.0);
            a.left[i] *= (float)(1 - d * (0.5 - 0.5 * lfo.wave(t)));
            a.right[i] *= (float)(1 - d * (0.5 - 0.5 * right.wave(t)));
        }
        return true;
    }
};

// pan: balance a stereo signal (automatable, LFO-able "position" -1..1)
struct Pan : Effect {
    Envelope position;
    Pan(const json &j, const Job &job) { label = "pan"; position = param(j, "position", 0, job.tempo); checkKeys(j, {"position"}, *this); }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        double pl = 1, pr = 1;
        for (size_t i = 0; i < a.frames(); ++i) {
            if (i % 32 == 0) {
                const double ang = (std::clamp(position.at(i / sr), -1.0, 1.0) + 1.0) * dsp::kPi / 4.0;
                pl = std::cos(ang) * M_SQRT2; pr = std::sin(ang) * M_SQRT2;
            }
            a.left[i] *= (float)std::min(1.0, pl); a.right[i] *= (float)std::min(1.0, pr);
        }
        return true;
    }
};

// gate: a rhythmic step pattern ("x-x-xx--", digits 0-9 for levels) or note-keyed from a track
// ("trigger", like duck: opens on each note for "hold" ms), e.g. an 80s gated reverb
struct Gate : Effect {
    std::string pattern, trigger;
    std::vector<int> keys;
    double stepBeats, attackMs, holdMs, releaseMs, floorDb;
    Envelope mix;
    Gate(const json &j, const Job &job, std::string &err) {
        label = "gate";
        pattern = j.value("pattern", "");
        trigger = j.value("trigger", "");
        if (pattern.empty() == trigger.empty()) err = "gate needs either \"pattern\" (e.g. \"x-x-xx--\") or \"trigger\" (a track name)";
        const auto &st = j.contains("step") ? j["step"] : json("1/16");
        stepBeats = st.is_number() ? st.get<double>() : Lfo::noteBeats(st.get<std::string>());
        for (auto &k : j.value("keys", json::array())) keys.push_back(parseKey(k));
        attackMs = std::max(0.1, j.value("attack", 1.0));
        holdMs = std::max(0.0, j.value("hold", 120.0));
        releaseMs = std::max(0.5, j.value("release", 15.0));
        floorDb = -std::fabs(j.value("depth", 80.0));
        mix = param(j, "mix", 1, job.tempo);
        checkKeys(j, {"pattern", "trigger", "keys", "step", "attack", "hold", "release", "depth", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &err) override {
        const double sr = c.job.sampleRate, floor = dbToLin(floorDb);
        std::vector<double> times;
        if (!trigger.empty()) {
            const Track *src = nullptr;
            for (const auto &t : c.job.tracks) if (t.name == trigger) src = &t;
            if (!src) { err = "gate: no track named '" + trigger + "'"; return false; }
            for (const auto &n : src->notes)
                if (keys.empty() || std::find(keys.begin(), keys.end(), n.key) != keys.end()) times.push_back(n.start);
            std::sort(times.begin(), times.end());
        }
        const double att = 1.0 - std::exp(-1.0 / (attackMs * 0.001 * sr)), rel = 1.0 - std::exp(-1.0 / (releaseMs * 0.001 * sr));
        double g = floor;
        size_t k = 0;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double t = i / sr;
            double target;
            if (!pattern.empty()) {
                const double beat = c.job.tempo.secToBeat(t);
                const long step = (long)std::floor(beat / stepBeats);
                const char ch = pattern[(size_t)(((step % (long)pattern.size()) + (long)pattern.size()) % (long)pattern.size())];
                target = ch == 'x' || ch == 'X' ? 1.0 : (ch >= '0' && ch <= '9') ? (ch - '0') / 9.0 : 0.0;
                target = floor + (1 - floor) * target;
            } else {
                while (k < times.size() && times[k] <= t) ++k;
                target = k > 0 && t - times[k - 1] < holdMs * 0.001 ? 1.0 : floor;
            }
            g += (target - g) * (target > g ? att : rel);
            const double m = mix.constant() ? mix.at(0) : mix.at(t);
            a.left[i] = blend(a.left[i], a.left[i] * g, m);
            a.right[i] = blend(a.right[i], a.right[i] * g, m);
        }
        return true;
    }
};

// rotary speaker (Leslie): horn above ~800 Hz, drum below, each spinning with its own inertia;
// "speed" 0 = chorale (slow), 1 = tremolo (fast), automatable
struct Rotary : Effect {
    Envelope speed, mix;
    double hornSlow, hornFast, drumSlow, drumFast, crossover;
    Rotary(const json &j, const Job &job) {
        label = "rotary";
        speed = param(j, "speed", 0, job.tempo);
        mix = param(j, "mix", 1, job.tempo);
        hornSlow = j.value("hornSlow", 0.8); hornFast = j.value("hornFast", 6.7);
        drumSlow = j.value("drumSlow", 0.7); drumFast = j.value("drumFast", 5.8);
        crossover = j.value("crossover", 800.0);
        checkKeys(j, {"speed", "mix", "hornSlow", "hornFast", "drumSlow", "drumFast", "crossover"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        Biquad lp, hp;
        lp.set(Biquad::LowPass, crossover, 0.7071, 0, sr);
        hp.set(Biquad::HighPass, crossover, 0.7071, 0, sr);
        dsp::DelayLine hornL, hornR, drum;
        const size_t cap = (size_t)(0.004 * sr) + 8;
        hornL.resize(cap); hornR.resize(cap); drum.resize(cap);
        double hornHz = hornSlow, drumHz = drumSlow, hph = 0, dph = 0;
        const double hornAcc = 1.0 - std::exp(-1.0 / (0.7 * sr)), drumAcc = 1.0 - std::exp(-1.0 / (3.5 * sr));   // inertia
        for (size_t i = 0; i < a.frames(); ++i) {
            const double t = i / sr, sp = std::clamp(speed.at(t), 0.0, 1.0);
            hornHz += (hornSlow + (hornFast - hornSlow) * sp - hornHz) * hornAcc;
            drumHz += (drumSlow + (drumFast - drumSlow) * sp - drumHz) * drumAcc;
            hph += 2 * dsp::kPi * hornHz / sr;
            dph += 2 * dsp::kPi * drumHz / sr;
            const double in = (a.left[i] + a.right[i]) * 0.5;
            const double lo = lp.process(in), hi = hp.process(in);
            hornL.push(hi); hornR.push(hi); drum.push(lo);
            // horn: doppler (moving delay), amplitude and stereo position follow the rotation
            const double hs = std::sin(hph), hc = std::cos(hph);
            const double hl = hornL.tap(2 + 0.0012 * sr * (1 + hs)) * (0.75 + 0.25 * hc);
            const double hr = hornR.tap(2 + 0.0012 * sr * (1 - hs)) * (0.75 - 0.25 * hc);
            const double ds = std::sin(dph);
            const double d = drum.tap(2 + 0.0004 * sr * (1 + ds)) * (0.85 + 0.15 * ds);
            const double m = mix.constant() ? mix.at(0) : mix.at(t);
            a.left[i] = blend(a.left[i], d + hl * 1.1, m);
            a.right[i] = blend(a.right[i], d + hr * 1.1, m);
        }
        return true;
    }
};

// auto-wah: an envelope follower sweeps a resonant band-pass (or low-pass) between min and max
struct AutoWah : Effect {
    double minHz, maxHz, q, sensDb, attackMs, releaseMs;
    Biquad::Type type;
    Envelope mix;
    AutoWah(const json &j, const Job &job, std::string &err) {
        label = "autowah";
        minHz = j.value("min", 350.0); maxHz = j.value("max", 2800.0);
        q = j.value("resonance", 3.0);
        sensDb = j.value("sensitivity", 0.0);
        attackMs = std::max(0.5, j.value("attack", 6.0));
        releaseMs = std::max(1.0, j.value("release", 120.0));
        const std::string mode = j.value("mode", "bandpass");
        if (mode == "bandpass") type = Biquad::BandPass;
        else if (mode == "lowpass") type = Biquad::LowPass;
        else err = "autowah mode must be bandpass or lowpass";
        mix = param(j, "mix", 1, job.tempo);
        checkKeys(j, {"min", "max", "resonance", "sensitivity", "attack", "release", "mode", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        const double att = 1.0 - std::exp(-1.0 / (attackMs * 0.001 * sr)), rel = 1.0 - std::exp(-1.0 / (releaseMs * 0.001 * sr));
        Biquad fl, fr;
        double env = 0;
        const double gainComp = type == Biquad::BandPass ? std::sqrt(q) : 1.0;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double x = std::max(std::fabs(a.left[i]), std::fabs(a.right[i]));
            env += (x - env) * (x > env ? att : rel);
            if (i % 16 == 0) {
                // -36 dB .. 0 dB of envelope (plus sensitivity) maps onto min .. max
                const double pos = std::clamp((dsp::linToDb(env) + sensDb + 36.0) / 36.0, 0.0, 1.0);
                fl.set(type, minHz * std::pow(maxHz / minHz, pos), q, 0, sr);
                fr.b0 = fl.b0; fr.b1 = fl.b1; fr.b2 = fl.b2; fr.a1 = fl.a1; fr.a2 = fl.a2;
            }
            const double m = mix.constant() ? mix.at(0) : mix.at(i / sr);
            a.left[i] = blend(a.left[i], fl.process(a.left[i]) * gainComp, m);
            a.right[i] = blend(a.right[i], fr.process(a.right[i]) * gainComp, m);
        }
        return true;
    }
};

// bitcrush: fewer bits and sample-and-hold downsampling
struct Bitcrush : Effect {
    Envelope bits, downsample, mix;
    Bitcrush(const json &j, const Job &job) {
        label = "bitcrush";
        bits = param(j, "bits", 8, job.tempo);
        downsample = param(j, "downsample", 1, job.tempo);
        mix = param(j, "mix", 1, job.tempo);
        checkKeys(j, {"bits", "downsample", "mix"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        double holdL = 0, holdR = 0, count = 1e9;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double t = i / sr, ds = std::max(1.0, downsample.at(t));
            const double steps = std::pow(2.0, std::clamp(bits.at(t), 1.0, 24.0) - 1);
            if (++count >= ds) { count -= ds; holdL = std::round(a.left[i] * steps) / steps; holdR = std::round(a.right[i] * steps) / steps; }
            const double m = mix.constant() ? mix.at(0) : mix.at(t);
            a.left[i] = blend(a.left[i], holdL, m);
            a.right[i] = blend(a.right[i], holdR, m);
        }
        return true;
    }
};

// vibrato: pitch-only wobble (tape wow, flutter, singer's vibrato), depth in cents
struct Vibrato : Effect {
    Lfo lfo;
    Envelope depth;
    Vibrato(const json &j, const Job &job) {
        label = "vibrato";
        lfo = lfoFrom(j, job, "5.5");
        depth = param(j, "depth", 20, job.tempo);
        checkKeys(j, {"rate", "shape", "phase", "depth"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        // a delay swinging by A samples at f Hz shifts pitch by up to 2*pi*f*A/sr: solve A for the cents wanted
        const double f = lfo.hz > 0 ? lfo.hz : c.job.tempo.bpmAtBeat(0) / 60.0 / lfo.beats;
        const double maxCents = 100;
        const double maxA = (std::pow(2.0, maxCents / 1200) - 1) * sr / (2 * dsp::kPi * std::max(0.05, f));
        dsp::DelayLine L, R;
        L.resize((size_t)(2 * maxA) + 16); R.resize((size_t)(2 * maxA) + 16);
        // the wave is integrated so its derivative (the pitch) follows the LFO shape
        double integ = 0;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double t = i / sr;
            L.push(a.left[i]); R.push(a.right[i]);
            const double cents = std::clamp(depth.at(t), 0.0, maxCents);
            const double A = (std::pow(2.0, cents / 1200) - 1) * sr / (2 * dsp::kPi * std::max(0.05, f));
            integ = std::sin(2 * dsp::kPi * (lfo.hz > 0 ? t * lfo.hz : c.job.tempo.secToBeat(t) / lfo.beats) + lfo.phase * 2 * dsp::kPi);
            const double d = 2 + maxA + A * integ;
            a.left[i] = (float)L.tap(d);
            a.right[i] = (float)R.tap(d);
        }
        return true;
    }
};

// tape stop / varispeed: plays the incoming audio at "speed" (1 = normal, 0 = stopped), pitch
// and time together. Whenever speed is back at 1 the output is in sync with the input again.
struct TapeStop : Effect {
    Envelope speed;
    TapeStop(const json &j, const Job &job) { label = "tapestop"; speed = param(j, "speed", 1, job.tempo); checkKeys(j, {"speed"}, *this); }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate;
        const std::vector<float> inL = a.left, inR = a.right;
        const size_t n = a.frames();
        double pos = 0;
        bool synced = true;
        for (size_t i = 0; i < n; ++i) {
            const double sp = std::clamp(speed.at(i / sr), 0.0, 4.0);
            if (sp >= 0.9999 && std::fabs(sp - 1) < 1e-4) {
                if (!synced) {   // back in sync: 5 ms fade in from silence avoids a click
                    synced = true;
                }
                pos = (double)i;
                continue;
            }
            synced = false;
            pos += sp;
            const size_t i0 = (size_t)pos;
            const double f = pos - i0;
            float l = 0, r = 0;
            if (i0 + 1 < n) { l = (float)(inL[i0] * (1 - f) + inL[i0 + 1] * f); r = (float)(inR[i0] * (1 - f) + inR[i0 + 1] * f); }
            a.left[i] = l; a.right[i] = r;
        }
        return true;
    }
};

} // namespace

double truePeakDb(const Audio &a) {
    double pk = 0;
    for (size_t i = 0; i < a.frames(); ++i) {
        pk = std::max({pk, (double)std::fabs(a.left[i]), (double)std::fabs(a.right[i])});
        if (std::max(std::fabs(a.left[i]), std::fabs(a.right[i])) > pk * 0.5)
            pk = std::max({pk, interPeak(a.left, i), interPeak(a.right, i)});
    }
    return dsp::linToDb(pk);
}

std::vector<std::string> builtinEffectTypes() {
    return {"gain", "eq", "filter", "delay", "reverb", "compressor", "limiter", "saturate", "chorus", "width", "duck",
            "tremolo", "pan", "gate", "rotary", "autowah", "bitcrush", "vibrato", "tapestop"};
}

std::unique_ptr<Effect> makeEffect(const json &j, const Job &job, const std::string &context, std::string &err) {
    std::unique_ptr<Effect> fx;
    try {
        if (!j.is_object()) { err = context + ": each effect must be an object"; return nullptr; }
        if (j.contains("plugin")) fx = std::make_unique<PluginFx>(j, job, err);
        else {
            const std::string t = j.value("type", "");
            if (t == "gain") fx = std::make_unique<Gain>(j, job);
            else if (t == "eq") fx = std::make_unique<Eq>(j, job, err);
            else if (t == "filter") fx = std::make_unique<Filter>(j, job, err);
            else if (t == "delay") fx = std::make_unique<Delay>(j, job);
            else if (t == "reverb") fx = std::make_unique<Reverb>(j, job);
            else if (t == "compressor") fx = std::make_unique<Compressor>(j, job);
            else if (t == "limiter") fx = std::make_unique<Limiter>(j, job);
            else if (t == "saturate") fx = std::make_unique<Saturate>(j, job);
            else if (t == "chorus") fx = std::make_unique<Chorus>(j, job);
            else if (t == "width") fx = std::make_unique<Width>(j, job);
            else if (t == "duck") fx = std::make_unique<Duck>(j, job, err);
            else if (t == "tremolo") fx = std::make_unique<Tremolo>(j, job);
            else if (t == "pan") fx = std::make_unique<Pan>(j, job);
            else if (t == "gate") fx = std::make_unique<Gate>(j, job, err);
            else if (t == "rotary") fx = std::make_unique<Rotary>(j, job);
            else if (t == "autowah") fx = std::make_unique<AutoWah>(j, job, err);
            else if (t == "bitcrush") fx = std::make_unique<Bitcrush>(j, job);
            else if (t == "vibrato") fx = std::make_unique<Vibrato>(j, job);
            else if (t == "tapestop") fx = std::make_unique<TapeStop>(j, job);
            else {
                std::string list;
                for (auto &n : builtinEffectTypes()) list += (list.empty() ? "" : ", ") + n;
                err = context + ": unknown effect type '" + t + "' (built-in: " + list + "; or {\"plugin\": \"<clap id>\"})";
                return nullptr;
            }
        }
    } catch (const std::exception &e) {
        err = context + ": " + e.what();
        return nullptr;
    }
    if (!err.empty()) { err = context + ": " + err; return nullptr; }
    return fx;
}

} // namespace wl
