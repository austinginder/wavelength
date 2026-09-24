#include "effects.hpp"

#include "automation.hpp"
#include "dsp.hpp"
#include "engine.hpp"

#include <algorithm>
#include <cmath>
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
        if (k == "type" || k == "automate" || k == "bypass") continue;
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

// ------------------------------------------------------------------------------- limiter
// Look-ahead brickwall: the gain starts falling before a peak arrives, so the output
// never exceeds the ceiling and never clicks.
struct Limiter : Effect {
    double ceiling, releaseMs, lookMs;
    Limiter(const json &j, const Job &) {
        label = "limiter";
        ceiling = j.value("ceiling", -1.0);
        releaseMs = std::max(1.0, j.value("release", 80.0));
        lookMs = std::clamp(j.value("lookahead", 5.0), 0.5, 50.0);
        checkKeys(j, {"ceiling", "release", "lookahead"}, *this);
    }
    bool process(Audio &a, const FxContext &c, std::string &) override {
        const double sr = c.job.sampleRate, ceil = dbToLin(ceiling);
        const size_t n = a.frames(), L = std::max<size_t>(1, (size_t)(lookMs * 0.001 * sr));
        std::vector<float> req(n), mn(n);
        for (size_t i = 0; i < n; ++i) {
            const double pk = std::max(std::fabs(a.left[i]), std::fabs(a.right[i]));
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
        double sum = 0, g = 1, minG = 1;
        for (size_t i = 0; i < n; ++i) {
            sum += mn[i];
            if (i >= L) sum -= mn[i - L];
            const double avg = sum / (double)std::min(i + 1, L);      // smooth attack ramp
            g = avg < g ? avg : g + (avg - g) * rel;                 // release
            minG = std::min(minG, g);
            a.left[i] = (float)std::clamp(a.left[i] * g, -ceil, ceil);
            a.right[i] = (float)std::clamp(a.right[i] * g, -ceil, ceil);
        }
        if (dsp::linToDb(minG) < -8) warnings.push_back("limiter reduced peaks by " + std::to_string((int)dsp::linToDb(minG)) + " dB: the input is very hot");
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
    double depthDb, attackMs, holdMs, releaseMs;
    Duck(const json &j, const Job &, std::string &err) {
        label = "duck";
        trigger = j.value("trigger", "");
        if (trigger.empty()) err = "duck needs \"trigger\": the name of the track whose notes cause ducking";
        for (auto &k : j.value("keys", json::array())) keys.push_back(parseKey(k));
        depthDb = -std::fabs(j.value("depth", 8.0));
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
            const float g = (float)dbToLin(depthDb * amount);
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
        const double sr = c.job.sampleRate;
        for (size_t i = 0; i < a.frames(); ++i) {
            const double m = mix.constant() ? mix.at(0) : mix.at(i / sr);
            a.left[i] = blend(a.left[i], wet.left[i], m);
            a.right[i] = blend(a.right[i], wet.right[i], m);
        }
        return true;
    }
};

} // namespace

std::vector<std::string> builtinEffectTypes() {
    return {"gain", "eq", "filter", "delay", "reverb", "compressor", "limiter", "saturate", "chorus", "width", "duck"};
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
