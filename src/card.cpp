#include "card.hpp"

#include "analyze.hpp"
#include "canvas.hpp"
#include "catalog.hpp"
#include "dsp.hpp"
#include "platform.hpp"
#include "wav.hpp"

#include <stb_image_write.h>   // compiled in image_codecs.cpp

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace wl {

namespace {

using namespace gfx;

// ---- the probe phrase (seconds; rendered at 120 BPM, so a beat is 0.5 s, with no lead-in) ----------
constexpr double kHeldOn = 0.5, kHeldOff = 2.5;                  // C4 held 2 s after 0.5 s of silence
constexpr double kKeyOn[4] = {4.0, 5.0, 6.0, 7.0}, kKeyLen = 0.6; // C2 C3 C4 C5
constexpr int kKeys[4] = {36, 48, 60, 72};
constexpr double kChordOn = 8.0, kChordLen = 1.0;                 // C4 Eb4 G4
constexpr double kRunOn = 9.5, kRunStep = 0.125;                  // eight 16ths, C4 D4 Eb4 F4 G4 F4 Eb4 D4
constexpr int kRun[8] = {60, 62, 63, 65, 67, 65, 63, 62};
constexpr double kEnd = 11.5;

json probeNotes() {
    json n = json::array();
    auto note = [&](double on, double len, int key) { n.push_back({{"beat", on * 2}, {"dur", len * 2}, {"key", key}, {"vel", 0.8}}); };
    note(kHeldOn, kHeldOff - kHeldOn, 60);
    for (int i = 0; i < 4; ++i) note(kKeyOn[i], kKeyLen, kKeys[i]);
    for (int k : {60, 63, 67}) note(kChordOn, kChordLen, k);
    for (int i = 0; i < 8; ++i) note(kRunOn + i * kRunStep, kRunStep * 0.8, kRun[i]);
    return n;
}

std::string label(const CardItem &it) {
    if (!it.preset.empty()) return it.preset;
    if (!it.state.empty()) return fs::path(it.state).filename().string();
    return "default";
}

// ---- measurements ---------------------------------------------------------------------------------
struct Measure {
    bool ok = false;
    std::string error, pluginName, format;
    Audio audio;
    int sr = 48000;
    Analysis held, whole;
    double peakDb = -120;                  // of the whole probe
    double heldLevelDb = -120;             // median level while the C4 is held
    double tailMs = 0;                     // note-off until 40 dB under the held level
    bool tailOpen = false;                 // the tail outlasted the gap before the next probe note
    double attackMs = 0;                   // note-on until 3 dB under the held level
    double noiseBeforeDb = -120;           // level before the first note
    struct Key { int key; double lufs; int sounds; };
    Key keys[4];
    double chordLufs = -120, runLufs = -120;
    int runOnsets = 0;
    std::vector<double> pitchCents;        // every 20 ms of the held note, cents from `soundsKey` (nan = unvoiced)
    int soundsKey = -1;
    std::vector<std::string> flags;
    json js;
};

double rmsDb(const Audio &a, int sr, double t0, double t1) {
    const size_t f0 = (size_t)std::max(0.0, t0 * sr), f1 = std::min(a.frames(), (size_t)std::max(0.0, t1 * sr));
    if (f1 <= f0) return -120;
    double s = 0;
    for (size_t i = f0; i < f1; ++i) s += 0.5 * ((double)a.left[i] * a.left[i] + (double)a.right[i] * a.right[i]);
    return 10 * std::log10(std::max(s / (f1 - f0), 1e-12));
}

// the sounding key of [t0, t1): YIN when it is sure, else the spacing of the strongest spectral peaks
// (a detuned supersaw has no clean period, but its partials still sit on multiples of the fundamental)
int soundingKey(const Audio &a, int sr, double t0, double t1, const Analysis &an) {
    if (an.pitchConfidence > 0.3 && an.pitchKey >= 0) return an.pitchKey;
    if (an.lufs < -70) return -1;
    double binHz = 0;
    const auto peaks = spectralPeaks(a, sr, t0, t1, 12, binHz);
    if (peaks.size() < 3) return -1;
    const double top = peaks.front().levelDb;
    int best = -1, bestHits = 0;
    for (const auto &c : peaks) {
        if (c.hz < 25 || c.levelDb < top - 24) continue;
        int hits = 0;
        for (const auto &q : peaks) {
            const double r = q.hz / c.hz, n = std::round(r);
            if (n >= 1 && std::fabs(r - n) < 0.03 * n) ++hits;
        }
        // the lowest candidate that explains most peaks
        if (hits > bestHits || (hits == bestHits && best >= 0 && c.key < best)) { bestHits = hits; best = c.key; }
    }
    return bestHits >= 3 ? best : -1;
}

void measure(Measure &m) {
    const Audio &a = m.audio;
    const int sr = m.sr;
    m.whole = analyzeAudio(a, sr);
    m.peakDb = m.whole.peakDb;
    m.held = analyzeAudio(a, sr, kHeldOn, kHeldOff);
    m.soundsKey = soundingKey(a, sr, kHeldOn + 0.3, kHeldOff, m.held);
    // the held level: median of 10 ms windows over the note's second half (past the attack)
    std::vector<double> lv;
    for (double t = kHeldOn + 1.0; t < kHeldOff; t += 0.01) lv.push_back(rmsDb(a, sr, t, t + 0.01));
    std::sort(lv.begin(), lv.end());
    m.heldLevelDb = lv.empty() ? -120 : lv[lv.size() / 2];
    // attack: note-on until the level first reaches 3 dB under the held level (20 ms windows)
    m.attackMs = 0;
    for (double t = kHeldOn; t < kHeldOff; t += 0.002)
        if (rmsDb(a, sr, t - 0.01, t) >= m.heldLevelDb - 3) { m.attackMs = std::max(0.0, (t - 0.005 - kHeldOn) * 1000); break; }
    // tail: from note-off until the level stays 40 dB under the held level (or under -70 dBFS)
    const double floorDb = std::max(m.heldLevelDb - 40, -70.0);
    double last = kHeldOff;
    for (double t = kHeldOff; t < kKeyOn[0] - 0.01; t += 0.01)
        if (rmsDb(a, sr, t, t + 0.01) > floorDb) last = t + 0.01;
    m.tailMs = (last - kHeldOff) * 1000;
    m.tailOpen = last >= kKeyOn[0] - 0.02;   // still sounding when the next probe note starts
    m.noiseBeforeDb = rmsDb(a, sr, 0.05, kHeldOn - 0.02);
    for (int i = 0; i < 4; ++i) {
        const Analysis k = analyzeAudio(a, sr, kKeyOn[i], kKeyOn[i] + kKeyLen);
        m.keys[i] = {kKeys[i], k.lufs, soundingKey(a, sr, kKeyOn[i] + 0.1, kKeyOn[i] + kKeyLen, k)};
    }
    m.chordLufs = analyzeAudio(a, sr, kChordOn, kChordOn + kChordLen).lufs;
    const Analysis run = analyzeAudio(a, sr, kRunOn - 0.02, kRunOn + 8 * kRunStep + 0.05);
    m.runLufs = run.lufs;
    m.runOnsets = (int)run.onsets.size();
    // pitch every 20 ms (80 ms windows) of the held note
    for (double t = kHeldOn; t + 0.08 <= kHeldOff; t += 0.02) {
        const Analysis w = analyzeAudio(a, sr, t, t + 0.08);
        if (m.soundsKey < 0 || w.pitchHz <= 0 || w.pitchConfidence < 0.5) { m.pitchCents.push_back(NAN); continue; }
        m.pitchCents.push_back(1200 * std::log2(w.pitchHz / (440 * std::pow(2.0, (m.soundsKey - 69) / 12.0))));
    }

    // flags: what an agent should know before using the patch
    auto &f = m.flags;
    if (m.whole.silent || m.held.lufs < -70) f.push_back("silent on a held C4");
    if (m.soundsKey >= 0 && m.soundsKey != 60) {
        const int d = m.soundsKey - 60;
        f.push_back("C4 sounds " + keyName(m.soundsKey) + (d % 12 == 0 ? " (octave " + std::string(d > 0 ? "+" : "") + std::to_string(d / 12) + ": transpose " + std::to_string(-d) + ")"
                                                                       : " (" + std::string(d > 0 ? "+" : "") + std::to_string(d) + " st)"));
    } else if (m.soundsKey < 0 && m.held.lufs > -70) f.push_back("no clear pitch (noise, drum or detuned)");
    if (m.attackMs >= 250) f.push_back("slow attack " + fmt("%.0f", m.attackMs) + " ms");
    if (m.tailOpen) f.push_back("long tail: still sounding " + fmt("%.1f", m.tailMs / 1000) + " s after note-off (release, delay or reverb in the patch)");
    else if (m.tailMs >= 800) f.push_back("long tail " + fmt("%.1f", m.tailMs / 1000) + " s after note-off");
    if (m.noiseBeforeDb > -60 && m.noiseBeforeDb > m.heldLevelDb - 30) f.push_back("sound before the first note (" + fmt("%.0f", m.noiseBeforeDb) + " dBFS): self-playing or noisy");
    {   // percussive: the held note falls well under its attack peak
        double peak = -120;
        for (double t = kHeldOn; t < kHeldOn + 0.3; t += 0.005) peak = std::max(peak, rmsDb(a, sr, t, t + 0.01));
        if (peak > -70 && m.heldLevelDb < peak - 15)
            f.push_back(m.heldLevelDb < -70 ? std::string("percussive: dies out within a held note")
                                            : "percussive: a held note falls " + fmt("%.0f", peak - m.heldLevelDb) + " dB under its attack");
    }
    for (auto &k : m.keys)
        if (k.lufs < -70) f.push_back(keyName(k.key) + " is silent (outside its range?)");
    {
        std::vector<double> v;   // voiced frames near the sounding key (octave errors of a fading note left out), 10-90%
        for (double c : m.pitchCents) if (!std::isnan(c) && std::fabs(c) < 300) v.push_back(c);
        if (v.size() > 10) {
            std::sort(v.begin(), v.end());
            const double lo = v[v.size() / 10], hi = v[v.size() * 9 / 10];
            if (hi - lo > 40) f.push_back("pitch moves " + fmt("%.0f", hi - lo) + " cents over the held note (vibrato, glide or drift)");
        }
    }
    if (m.held.width < 0.02 && m.held.lufs > -70) f.push_back("mono");
    if (m.runOnsets > 0 && m.runOnsets < 5) f.push_back("16th run blurs: " + std::to_string(m.runOnsets) + " of 8 onsets heard");

    json keys = json::array();
    for (auto &k : m.keys)
        keys.push_back({{"key", keyName(k.key)}, {"lufs", std::round(k.lufs * 10) / 10}, {"sounds", k.sounds >= 0 ? json(keyName(k.sounds)) : json(nullptr)}});
    auto r1 = [](double v) { return std::round(v * 10) / 10; };
    m.js = {{"sounds", m.soundsKey >= 0 ? json(keyName(m.soundsKey)) : json(nullptr)},
            {"transpose", m.soundsKey >= 0 && (m.soundsKey - 60) % 12 == 0 ? json(60 - m.soundsKey) : json(nullptr)},
            {"held", analysisToJson(m.held, false)}, {"heldLevelDb", r1(m.heldLevelDb)}, {"attackMs", std::round(m.attackMs)}, {"tailMs", std::round(m.tailMs)}, {"tailOpen", m.tailOpen},
            {"noiseBeforeDb", r1(m.noiseBeforeDb)}, {"keys", keys}, {"chordLufs", r1(m.chordLufs)},
            {"run", {{"lufs", r1(m.runLufs)}, {"onsets", m.runOnsets}, {"notes", 8}}}, {"flags", m.flags}};
}

// ---- drawing --------------------------------------------------------------------------------------
double noteHz(int key) { return 440 * std::pow(2.0, (key - 69) / 12.0); }

struct Panel { double x, y, w, h; };

// spectrogram of [t0, t1) into p, log frequency fLo..fHi, +3 dB/oct like the song picture
void spectrogram(Canvas &cv, const Audio &a, int sr, double t0, double t1, const Panel &p, double fLo, double fHi, size_t N) {
    std::vector<double> hann(N);
    double hsum = 0;
    for (size_t i = 0; i < N; ++i) { hann[i] = 0.5 - 0.5 * std::cos(2 * dsp::kPi * i / (N - 1)); hsum += hann[i]; }
    std::vector<std::complex<double>> buf(N);
    std::vector<double> mag(N / 2);
    const double binHz = (double)sr / N;
    const int cols = (int)p.w, rows = (int)p.h;
    for (int col = 0; col < cols; ++col) {
        const long c = (long)((t0 + (col + 0.5) / cols * (t1 - t0)) * sr);
        for (size_t i = 0; i < N; ++i) {
            const long k = c - (long)N / 2 + (long)i;
            const double v = k >= 0 && (size_t)k < a.frames() ? 0.5 * (a.left[(size_t)k] + a.right[(size_t)k]) : 0.0;
            buf[i] = v * hann[i];
        }
        dsp::fft(buf);
        for (size_t i = 0; i < N / 2; ++i) mag[i] = std::abs(buf[i]) * 2 / hsum;
        for (int row = 0; row < rows; ++row) {
            const double ua = (rows - row - 1.0) / rows, ub = (rows - row + 0.0) / rows;
            const double fa = fLo * std::pow(fHi / fLo, ua), fb = fLo * std::pow(fHi / fLo, ub), fm = std::sqrt(fa * fb);
            double mm;
            if (fb - fa < binHz) {
                const double pos = fm / binHz;
                const size_t k = std::min(N / 2 - 2, (size_t)pos);
                mm = mag[k] + (mag[k + 1] - mag[k]) * (pos - k);
            } else {
                mm = 0;
                for (size_t k = (size_t)(fa / binHz); k <= std::min(N / 2 - 1, (size_t)(fb / binHz)); ++k) mm = std::max(mm, mag[k]);
            }
            const double db = 20 * std::log10(std::max(mm, 1e-9)) + 3 * std::log2(fm / 1000);
            const double v = (db + 90) / 72;
            if (v > 0) cv.blend((int)p.x + col, (int)p.y + row, ramp(v), 1);
        }
    }
}

double freqY(const Panel &p, double f, double fLo, double fHi) { return p.y + (1 - std::log(f / fLo) / std::log(fHi / fLo)) * p.h; }

// level (5 ms RMS, dB under the probe's peak) as an amber line over the panel: top = peak, bottom = 60 dB under
void levelLine(Canvas &cv, const Audio &a, int sr, double t0, double t1, const Panel &p, double peakDb, double alpha = 1) {
    double prev = -1;
    for (int col = 0; col < (int)p.w; ++col) {
        const double ta = t0 + col / p.w * (t1 - t0), tb = t0 + (col + 1) / p.w * (t1 - t0);
        const double tm = 0.5 * (ta + tb), db = rmsDb(a, sr, tm - 0.01, tm + 0.01) + 3;   // 20 ms RMS; a sine's RMS is 3 dB under its peak
        const double v = std::clamp((db - (peakDb - 60)) / 60, 0.0, 1.0);
        const double y = p.y + (1 - v) * (p.h - 2) + 1;
        if (v <= 0) { prev = -1; continue; }
        const double ya = prev < 0 ? y : std::min(prev, y), yb = prev < 0 ? y : std::max(prev, y);
        cv.rect(p.x + col, ya - 1, p.x + col + 1, yb + 1, kAmber, alpha);
        prev = y;
    }
}

void frame(Canvas &cv, const Panel &p) { cv.rect(p.x, p.y, p.x + p.w, p.y + p.h, kPanel); }

void noteAxis(Canvas &cv, const Font &font, const Panel &p, double fLo, double fHi, double size, bool labels) {
    for (int oct = 1; oct <= 8; ++oct) {
        const double f = noteHz(12 * (oct + 1));
        if (f < fLo || f > fHi) continue;
        const double y = freqY(p, f, fLo, fHi);
        cv.rect(p.x, y, p.x + p.w, y + 1, kWhite, 0.08);
        if (labels) font.draw(cv, p.x - size * 2.2, y + size * 0.35, "C" + std::to_string(oct), size, kDim);
    }
}

void markTime(Canvas &cv, const Panel &p, double t0, double t1, double t, Rgb c, double a = 0.7) {
    if (t < t0 || t > t1) return;
    const double x = p.x + (t - t0) / (t1 - t0) * p.w;
    cv.rect(x, p.y, x + 1, p.y + p.h, c, a);
}

// the full card for one patch
void drawCard(Canvas &cv, const Font &font, const Measure &m, const CardItem &it, double S, int W) {
    const double L = 64 * S, R = 16 * S;
    const double fLo = 30, fHi = 16000;
    font.draw(cv, 16 * S, 34 * S, ascii(font.fit(label(it), 24 * S, W - 32 * S)), 24 * S, kText);
    font.draw(cv, 16 * S, 56 * S, ascii(font.fit(m.pluginName + "  (" + m.format + ")  " + it.plugin, 13 * S, W - 32 * S)), 13 * S, kDim);
    if (!m.ok) {
        font.draw(cv, 16 * S, 96 * S, ascii(font.fit("failed: " + m.error, 15 * S, W - 32 * S)), 15 * S, kRed);
        return;
    }
    {
        std::string s = (m.soundsKey >= 0 ? "C4 sounds " + keyName(m.soundsKey) : std::string("no clear pitch")) + "   " + fmt("%.1f LUFS", m.held.lufs) +
                        "   attack " + fmt("%.0f ms", m.attackMs) + "   tail " + (m.tailOpen ? fmt("%.1f s+", m.tailMs / 1000) : fmt("%.2f s", m.tailMs / 1000)) + "   width " + fmt("%.2f", m.held.width) +
                        "   brightness " + fmt("%.0f Hz", m.held.centroidHz);
        font.draw(cv, 16 * S, 80 * S, s, 14 * S, kText);
    }
    // ---- held C4 -----------------------------------------------------------------------------------
    const double t0 = kHeldOn - 0.1, t1 = kKeyOn[0] - 0.1;
    Panel held{L, 100 * S, W - L - R, 300 * S};
    frame(cv, held);
    spectrogram(cv, m.audio, m.sr, t0, t1, held, fLo, fHi, 4096);
    noteAxis(cv, font, held, fLo, fHi, 11 * S, true);
    if (m.soundsKey >= 0) {   // the harmonics of the sounding note, ticked on the right edge
        const double f0 = noteHz(m.soundsKey);
        for (int h = 1; h <= 16 && f0 * h < fHi; ++h) {
            const double y = freqY(held, f0 * h, fLo, fHi);
            cv.rect(held.x + held.w - (h == 1 ? 18 : 9) * S, y, held.x + held.w, y + 1.5, kGreen, h == 1 ? 1 : 0.7);
        }
        font.draw(cv, held.x + held.w - 60 * S, freqY(held, f0, fLo, fHi) - 4 * S, "f0 " + keyName(m.soundsKey), 11 * S, kGreen);
    }
    levelLine(cv, m.audio, m.sr, t0, t1, held, m.peakDb);
    markTime(cv, held, t0, t1, kHeldOn, kWhite, 0.5);
    markTime(cv, held, t0, t1, kHeldOff, kWhite, 0.5);
    if (m.attackMs > 0) markTime(cv, held, t0, t1, kHeldOn + m.attackMs / 1000, kGreen, 0.8);
    markTime(cv, held, t0, t1, kHeldOff + m.tailMs / 1000, kRed, 0.6);
    for (double ms = 0; kHeldOn + ms / 1000 <= t1; ms += 250) {   // ms from note-on under the panel
        const double x = held.x + (kHeldOn + ms / 1000 - t0) / (t1 - t0) * held.w;
        cv.rect(x, held.y + held.h, x + 1, held.y + held.h + 4 * S, kDim);
        font.draw(cv, x + 2 * S, held.y + held.h + 14 * S, fmt("%.0f", ms), 10 * S, kDim);
    }
    font.draw(cv, held.x + 6 * S, held.y + 16 * S, "held C4: note on | attack (green) | note off | tail end (red); amber = level, top = peak, 60 dB range", 12 * S, kText);
    font.draw(cv, held.x - 30 * S, held.y + held.h + 14 * S, "ms", 10 * S, kDim);

    // ---- pitch over the held note, waveform ----------------------------------------------------------
    const double r2 = held.y + held.h + 30 * S;
    Panel pitch{L, r2, (W - L - R) * 0.62, 120 * S}, wave{L + (W - L - R) * 0.62 + 16 * S, r2, (W - L - R) * 0.38 - 16 * S, 120 * S};
    frame(cv, pitch);
    frame(cv, wave);
    for (int c : {-50, 0, 50}) {
        const double y = pitch.y + pitch.h / 2 - c / 100.0 * pitch.h / 2;
        cv.rect(pitch.x, y, pitch.x + pitch.w, y + 1, c == 0 ? kDim : kGrid, c == 0 ? 0.6 : 1);
        font.draw(cv, pitch.x - 40 * S, y + 4 * S, fmt("%+.0f", (double)c), 10 * S, kDim);
    }
    for (size_t i = 0; i < m.pitchCents.size(); ++i) {
        const double c = m.pitchCents[i];
        if (std::isnan(c)) continue;
        const double x = pitch.x + (i + 0.5) / m.pitchCents.size() * pitch.w, y = pitch.y + pitch.h / 2 - std::clamp(c, -100.0, 100.0) / 100 * pitch.h / 2;
        cv.rect(x - 1.5 * S, y - 1.5 * S, x + 1.5 * S, y + 1.5 * S, kTracks[0]);
    }
    size_t voiced = 0;
    for (double c : m.pitchCents) if (!std::isnan(c)) ++voiced;
    font.draw(cv, pitch.x + 6 * S, pitch.y + 15 * S,
              m.soundsKey < 0 ? std::string("pitch: none found")
              : "pitch over the held note, cents from " + keyName(m.soundsKey) + " (+-100)" +
                    (voiced * 3 < m.pitchCents.size() ? std::string("; mostly no single period (detuned or noisy)") : std::string()),
              12 * S, kText);
    {   // two periods (or 20 ms) of the held note's second half, from a rising zero crossing
        const double f0 = m.soundsKey >= 0 ? noteHz(m.soundsKey) : 0;
        const size_t len = f0 > 0 ? (size_t)(2 * m.sr / f0) : (size_t)(0.02 * m.sr);
        size_t s = (size_t)((kHeldOn + 1.2) * m.sr);
        auto mono = [&](size_t i) { return i < m.audio.frames() ? 0.5 * (m.audio.left[i] + m.audio.right[i]) : 0.0; };
        for (size_t k = 0; k < (size_t)(0.02 * m.sr); ++k) if (mono(s + k) <= 0 && mono(s + k + 1) > 0) { s += k + 1; break; }
        double peak = 1e-6;
        for (size_t i = 0; i < len; ++i) peak = std::max(peak, std::fabs(mono(s + i)));
        double prev = NAN;
        for (int col = 0; col < (int)wave.w; ++col) {
            const size_t a = s + (size_t)(col / wave.w * len), b = std::max(a + 1, s + (size_t)((col + 1) / wave.w * len));
            double lo = 1, hi = -1;
            for (size_t i = a; i < b; ++i) { lo = std::min(lo, mono(i) / peak); hi = std::max(hi, mono(i) / peak); }
            if (!std::isnan(prev)) { lo = std::min(lo, prev); hi = std::max(hi, prev); }
            const double mid = wave.y + wave.h / 2, amp = wave.h * 0.42;
            cv.rect(wave.x + col, mid - hi * amp - 1, wave.x + col + 1, mid - lo * amp + 1, kTracks[2]);
            prev = mono(b - 1) / peak;
        }
        cv.rect(wave.x, wave.y + wave.h / 2, wave.x + wave.w, wave.y + wave.h / 2 + 1, kWhite, 0.12);
        font.draw(cv, wave.x + 6 * S, wave.y + 15 * S, f0 > 0 ? "waveform: 2 periods, normalized" : "waveform: 20 ms, normalized", 12 * S, kText);
    }

    // ---- C2 C3 C4 C5 --------------------------------------------------------------------------------
    const double r3 = r2 + pitch.h + 34 * S, kw = (W - L - R - 3 * 12 * S) / 4;
    for (int i = 0; i < 4; ++i) {
        Panel p{L + i * (kw + 12 * S), r3, kw, 150 * S};
        frame(cv, p);
        spectrogram(cv, m.audio, m.sr, kKeyOn[i] - 0.05, kKeyOn[i] + 0.95, p, fLo, fHi, 4096);
        noteAxis(cv, font, p, fLo, fHi, 10 * S, i == 0);
        levelLine(cv, m.audio, m.sr, kKeyOn[i] - 0.05, kKeyOn[i] + 0.95, p, m.peakDb);
        const auto &k = m.keys[i];
        const std::string t = keyName(k.key) + (k.lufs < -70 ? ": silent" : ": " + fmt("%.1f LUFS", k.lufs) + (k.sounds >= 0 ? ", sounds " + keyName(k.sounds) : ""));
        font.draw(cv, p.x, p.y - 6 * S, t, 12 * S, k.lufs < -70 ? kRed : kText);
    }

    // ---- chord and run ------------------------------------------------------------------------------
    const double r4 = r3 + 150 * S + 34 * S;
    Panel chord{L, r4, (W - L - R) * 0.42, 150 * S}, run{L + (W - L - R) * 0.42 + 16 * S, r4, (W - L - R) * 0.58 - 16 * S, 150 * S};
    frame(cv, chord);
    frame(cv, run);
    spectrogram(cv, m.audio, m.sr, kChordOn - 0.05, kChordOn + 1.45, chord, fLo, fHi, 4096);
    spectrogram(cv, m.audio, m.sr, kRunOn - 0.05, kEnd, run, fLo, fHi, 2048);
    noteAxis(cv, font, chord, fLo, fHi, 10 * S, true);
    noteAxis(cv, font, run, fLo, fHi, 10 * S, false);
    levelLine(cv, m.audio, m.sr, kChordOn - 0.05, kChordOn + 1.45, chord, m.peakDb);
    levelLine(cv, m.audio, m.sr, kRunOn - 0.05, kEnd, run, m.peakDb);
    markTime(cv, chord, kChordOn - 0.05, kChordOn + 1.45, kChordOn + kChordLen, kWhite, 0.5);
    for (int i = 0; i < 8; ++i) markTime(cv, run, kRunOn - 0.05, kEnd, kRunOn + i * kRunStep, kWhite, 0.25);
    font.draw(cv, chord.x, chord.y - 6 * S, "C minor chord (1 s): " + fmt("%.1f LUFS", m.chordLufs), 12 * S, kText);
    font.draw(cv, run.x, run.y - 6 * S, "16ths at 120 BPM: " + fmt("%.1f LUFS", m.runLufs) + ", " + std::to_string(m.runOnsets) + " of 8 onsets heard", 12 * S, kText);

    // ---- flags --------------------------------------------------------------------------------------
    double y = r4 + 150 * S + 26 * S;
    font.draw(cv, 16 * S, y, m.flags.empty() ? "Flags: none" : "Flags:", 14 * S, m.flags.empty() ? kGreen : kAmber);
    for (auto &f : m.flags) { font.draw(cv, 80 * S, y, ascii(font.fit(f, 13 * S, W - 96 * S)), 13 * S, kText); y += 18 * S; }
    if (m.flags.empty()) y += 18 * S;
    font.draw(cv, 16 * S, y + 8 * S,
              "Spectrum: +3 dB/oct (pink noise reads flat), 30 Hz-16 kHz, lines at each C. Amber: level, 60 dB under the probe's peak at the bottom.", 11 * S, kDim);
}

// a compact card for the contact sheet: the whole probe in one strip
void drawCompact(Canvas &cv, const Font &font, const Measure &m, const CardItem &it, double x, double y, double w, double S, int index) {
    cv.rect(x, y, x + w, y + 236 * S, kPanelAlt);
    cv.rect(x, y, x + 4 * S, y + 236 * S, kTracks[index % 8]);
    font.draw(cv, x + 12 * S, y + 22 * S, ascii(font.fit(std::to_string(index + 1) + ". " + label(it), 16 * S, w - 24 * S)), 16 * S, kText);
    font.draw(cv, x + 12 * S, y + 40 * S, ascii(font.fit(m.pluginName + " (" + m.format + ")", 11 * S, w - 24 * S)), 11 * S, kDim);
    if (!m.ok) {
        font.draw(cv, x + 12 * S, y + 70 * S, ascii(font.fit("failed: " + m.error, 13 * S, w - 24 * S)), 13 * S, kRed);
        return;
    }
    const std::string s = (m.soundsKey >= 0 ? "C4 > " + keyName(m.soundsKey) : std::string("no pitch")) + "  " + fmt("%.1f LUFS", m.held.lufs) + "  att " +
                          fmt("%.0f ms", m.attackMs) + "  tail " + fmt(m.tailOpen ? "%.1f s+" : "%.1f s", m.tailMs / 1000) + "  wid " + fmt("%.2f", m.held.width) + "  " +
                          fmt("%.0f Hz", m.held.centroidHz);
    font.draw(cv, x + 12 * S, y + 58 * S, ascii(font.fit(s, 12 * S, w - 24 * S)), 12 * S, kText);
    Panel p{x + 12 * S, y + 66 * S, w - 24 * S, 120 * S};
    frame(cv, p);
    spectrogram(cv, m.audio, m.sr, 0.3, kEnd, p, 30, 16000, 2048);
    noteAxis(cv, font, p, 30, 16000, 9 * S, false);
    levelLine(cv, m.audio, m.sr, 0.3, kEnd, p, m.peakDb, 0.9);
    const double t0 = 0.3, t1 = kEnd;
    auto X = [&](double t) { return p.x + (t - t0) / (t1 - t0) * p.w; };
    const std::pair<double, const char *> marks[] = {{kHeldOn, "C4 held"}, {kKeyOn[0], "C2"}, {kKeyOn[1], "C3"}, {kKeyOn[2], "C4"}, {kKeyOn[3], "C5"}, {kChordOn, "chord"}, {kRunOn, "16ths"}};
    for (auto &[t, n] : marks) {
        cv.rect(X(t), p.y + p.h, X(t) + 1, p.y + p.h + 4 * S, kDim);
        font.draw(cv, X(t) + 2 * S, p.y + p.h + 13 * S, n, 10 * S, kDim);
    }
    std::string flags;
    for (auto &f : m.flags) flags += (flags.empty() ? "" : "; ") + f;
    font.draw(cv, x + 12 * S, y + 224 * S, ascii(font.fit(flags.empty() ? "no flags" : flags, 11 * S, w - 24 * S)), 11 * S, flags.empty() ? kGreen : kAmber);
}

} // namespace

bool makeCards(const CardOptions &opt, json &report, std::string &err) {
    if (opt.items.empty()) { err = "no plugin to draw"; return false; }
    std::vector<Measure> ms(opt.items.size());
    // one job, one track per patch, every track the probe phrase; rendered in a child process
    json tracks = json::array();
    for (size_t i = 0; i < opt.items.size(); ++i) {
        const auto &it = opt.items[i];
        PluginInfo info;
        std::string e;
        if (resolvePlugin(it.plugin, info, e)) { ms[i].pluginName = info.name; ms[i].format = info.format; }
        else if (it.plugin.rfind("builtin:", 0) == 0) { ms[i].pluginName = it.plugin; ms[i].format = "builtin"; }
        else { ms[i].pluginName = it.plugin; ms[i].format = "?"; }
        json t = {{"name", "p" + std::to_string(i + 1)}, {"plugin", it.plugin}, {"notes", probeNotes()}};
        if (!it.preset.empty()) t["preset"] = it.preset;
        if (!it.state.empty()) t["state"] = fs::absolute(it.state).string();
        tracks.push_back(t);
    }
    std::error_code ec;
    const fs::path dir = opt.keep.empty() ? fs::temp_directory_path() / ("wavelength-card-" + std::to_string(platform::processId())) : fs::path(opt.keep);
    fs::create_directories(dir, ec);
    struct Cleanup { fs::path d; bool on; ~Cleanup() { std::error_code e; if (on) fs::remove_all(d, e); } } cleanup{dir, opt.keep.empty()};
    const fs::path jobPath = dir / "probe.json", outDir = dir / "out";
    json rep;
    std::string crash;
    for (;;) {   // a track that can't be built (unknown preset, missing plugin) fails the whole render: drop it and go again
        json job = {{"tempo", 120}, {"leadIn", 0}, {"tail", 0.5}, {"stems", "24"}, {"length", kEnd}, {"tracks", tracks}};
        std::ofstream(jobPath) << job.dump(1);
        std::vector<std::string> args = {platform::selfExecutable(), "render", jobPath.string(), "--out", outDir.string(), "--json"};
        if (opt.jobs >= 0) { args.push_back("--jobs"); args.push_back(std::to_string(opt.jobs)); }
        platform::Process p;
        if (!platform::spawn(args, p, true, !opt.verbose)) { err = "cannot start the probe render"; return false; }
        std::string text;
        platform::readOutput(p, text, 1800);
        while (!platform::finished(p, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const json out = json::parse(text, nullptr, false);
        const std::string e = out.is_object() ? out.value("error", std::string()) : std::string();
        // "p3: no preset named ..." or "track 'p3': builtin:synth has no patch ..."
        static const std::regex named(R"(^(?:track ')?p(\d+)'?: )");
        std::smatch mt;
        int bad = -1;
        size_t colon = 0;
        if (!out.value("ok", false) && !out.contains("tracks") && std::regex_search(e, mt, named)) { bad = std::stoi(mt[1]) - 1; colon = (size_t)mt.length(0) - 2; }
        json keep = json::array();
        for (auto &t : tracks) if (t["name"] != "p" + std::to_string(bad + 1)) keep.push_back(t);
        if (bad >= 0 && bad < (int)ms.size() && keep.size() < tracks.size()) {
            ms[(size_t)bad].error = e.substr(colon + 2);
            tracks = keep;
            if (tracks.empty()) break;
            continue;
        }
        std::ifstream rin(outDir / "report.json");
        rep = rin ? json::parse(rin, nullptr, false) : json();
        if (!rep.is_object() || !rep.contains("tracks")) {
            err = "the probe render failed" + (!e.empty() ? ": " + e : crash.empty() ? std::string() : " (" + crash + ")");
            return false;
        }
        break;
    }
    if (!rep.is_object()) rep = json{{"tracks", json::array()}};
    std::map<std::string, std::string> trackErr;
    for (auto &w : rep.value("warnings", json::array()))
        if (w.is_string())
            for (size_t i = 0; i < opt.items.size(); ++i) {
                const std::string key = "track 'p" + std::to_string(i + 1) + "'";
                if (w.get<std::string>().rfind(key, 0) == 0 && trackErr[key].empty()) trackErr[key] = w.get<std::string>().substr(key.size() + 2);
            }
    for (size_t i = 0; i < opt.items.size(); ++i) {
        Measure &m = ms[i];
        const std::string name = "p" + std::to_string(i + 1);
        std::string file;
        for (auto &t : rep["tracks"]) if (t.value("name", "") == name) file = t.value("file", "");
        std::string e;
        if (file.empty() || !readWav(file, m.audio, m.sr, e)) {
            if (m.error.empty()) m.error = trackErr["track '" + name + "'"];
            if (m.error.empty()) m.error = file.empty() ? "no audio (the track failed)" : e;
        } else {
            m.ok = true;
            measure(m);
        }
    }

    // ---- picture ------------------------------------------------------------------------------------
    const Font font;
    const int W = std::clamp(opt.width, 800, 3200);
    const double S = W / 1400.0;
    int H;
    std::vector<uint8_t> px;
    if (ms.size() == 1) {
        H = (int)(1000 * S + 18 * S * std::max<size_t>(1, ms[0].flags.size()));
        Canvas cv(W, H, kBg);
        drawCard(cv, font, ms[0], opt.items[0], S, W);
        px = std::move(cv.px);
    } else {
        const int cols = W >= 1800 ? 3 : 2;
        const double gap = 14 * S, cw = (W - gap * (cols + 1)) / cols, ch = 236 * S;
        const int rows = (int)((ms.size() + cols - 1) / cols);
        H = (int)(52 * S + rows * (ch + gap) + 30 * S);
        Canvas cv(W, H, kBg);
        font.draw(cv, 16 * S, 32 * S, std::to_string(ms.size()) + " patches, one probe each: held C4, C2-C5, C minor chord, 16ths", 18 * S, kText);
        for (size_t i = 0; i < ms.size(); ++i)
            drawCompact(cv, font, ms[i], opt.items[i], gap + (i % cols) * (cw + gap), 48 * S + (i / cols) * (ch + gap), cw, S, (int)i);
        font.draw(cv, 16 * S, H - 10 * S,
                  "Spectrum +3 dB/oct, 30 Hz-16 kHz, lines at each C; amber = level (60 dB range from the patch's peak). `wavelength card <plugin> <preset>` for a full card.",
                  11 * S, kDim);
        px = std::move(cv.px);
    }
    stbi_write_png_compression_level = 9;
    if (!stbi_write_png(opt.out.c_str(), W, H, 3, px.data(), W * 3)) { err = "cannot write " + opt.out; return false; }

    json items = json::array();
    for (size_t i = 0; i < ms.size(); ++i) {
        json j = {{"plugin", opt.items[i].plugin}, {"name", ms[i].pluginName}, {"format", ms[i].format}, {"ok", ms[i].ok}};
        if (!opt.items[i].preset.empty()) j["preset"] = opt.items[i].preset;
        if (!opt.items[i].state.empty()) j["state"] = opt.items[i].state;
        if (ms[i].ok) j.update(ms[i].js);
        else j["error"] = ms[i].error;
        items.push_back(j);
    }
    report = {{"ok", true}, {"file", opt.out}, {"width", W}, {"height", H}, {"patches", items}};
    if (!opt.keep.empty()) report["probe"] = dir.string();
    return true;
}

} // namespace wl
