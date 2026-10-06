#include "picture.hpp"

#include "canvas.hpp"

#include "dsp.hpp"
#include "loudness.hpp"

#include <stb_image_write.h>   // compiled in image_codecs.cpp

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <set>

namespace wl {

namespace {

using namespace gfx;

std::string clock(double s) {
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", (int)(s / 60), (int)std::fmod(s, 60.0));
    return b;
}
bool unpitched(const Track &t) {
    return !t.harmony || t.plugin == "builtin:drums" || t.plugin == "builtin:fx" || t.plugin == "builtin:shepard";
}

} // namespace

bool writePicture(const std::string &path, const Job &job, const Picture &pic, int &height, std::string &err) {
    const Font font;
    const bool rendered = pic.mix != nullptr;
    const int W = std::clamp(pic.width, 800, 3200);
    const double scale = W / 1400.0;   // text and lane sizes follow the width
    auto S = [&](double v) { return v * scale; };
    const int labelW = (int)S(196), right = (int)S(16);
    const double plotX = labelW, plotW = W - labelW - right;
    const double t0 = pic.from, t1 = std::max(pic.seconds, pic.from + 0.1), dur = t1 - t0;
    auto X = [&](double sec) { return plotX + (sec - t0) / dur * plotW; };
    const size_t n = job.tracks.size();

    const int headerH = (int)S(66), rulerH = (int)S(38), gap = (int)S(8), loudH = rendered ? (int)S(120) : 0, specH = rendered ? (int)S(150) : 0;
    const int laneH = (int)std::clamp(S(640) / std::max<size_t>(1, n), S(22), S(56));
    const int axisH = (int)S(26), footH = (int)S(26);
    const int rulerY = headerH, loudY = rulerY + rulerH + gap, specY = loudY + loudH + (rendered ? gap : 0);
    const int lanesY = specY + specH + (rendered ? gap : 0), axisY = lanesY + laneH * (int)n;
    const int H = axisY + axisH + footH;
    Canvas cv(W, H, kBg);
    const MeterMap &meter = job.meter;

    // ---- header -----------------------------------------------------------------------------
    font.draw(cv, S(16), S(32), ascii(font.fit(pic.title, S(22), W - S(32))), S(22), kText);
    {
        std::string stats = clock(pic.seconds - pic.from);
        if (rendered) {
            stats += "   " + fmt("%.1f LUFS", pic.mixLufs) + "   LRA " + fmt("%.1f", pic.lra) + "   true peak " + fmt("%.1f dBTP", pic.truePeak);
        }
        // Lowest to highest tempo in the window: ramps are linear between points, so the extremes
        // sit at the window's ends or at a tempo point inside it.
        const double beat0 = job.tempo.secToBeat(t0), beat1 = job.tempo.secToBeat(t1);
        double b0 = job.tempo.bpmAtBeat(beat0), b1 = b0;
        auto take = [&](double beat) {
            const double v = job.tempo.bpmAtBeat(beat);
            b0 = std::min(b0, v);
            b1 = std::max(b1, v);
        };
        take(beat1);
        for (const auto& p : job.tempo.points())
            if (p.beat > beat0 && p.beat < beat1) take(p.beat);
        stats += "   " + fmt("%.0f", b0) + (std::fabs(b1 - b0) > 0.5 ? fmt("-%.0f", b1) : "") + " BPM   " + std::to_string(job.tsigNum) + "/" +
                 std::to_string(job.tsigDen) + "   " + std::to_string(n) + (n == 1 ? " track" : " tracks");
        if (!rendered) stats += "   (arrangement: not rendered)";
        font.draw(cv, S(16), S(55), stats, S(14), kDim);
    }

    // ---- sections and bars ------------------------------------------------------------------
    struct Sec { std::string name; double a, b; };
    std::vector<Sec> secs;
    for (size_t m = 0; m < job.markers.size(); ++m) {
        const double a = job.markers[m].sec, b = m + 1 < job.markers.size() ? job.markers[m + 1].sec : t1;
        if (b > t0 && a < t1) secs.push_back({job.markers[m].name, std::max(a, t0), std::min(b, t1)});
    }
    cv.rect(plotX, rulerY, plotX + plotW, rulerY + S(22), kPanel);
    for (size_t k = 0; k < secs.size(); ++k) {
        const double xa = X(secs[k].a), xb = X(secs[k].b);
        cv.rect(xa, rulerY, xb, rulerY + S(22), kSections[k % 6], 0.9);
        const std::string label = font.fit(secs[k].name, S(13), xb - xa - S(8));
        if (!label.empty()) font.draw(cv, xa + S(5), rulerY + S(16), ascii(label), S(13), kText);
    }
    font.draw(cv, S(10), rulerY + S(16), "Sections", S(13), kDim);
    font.draw(cv, S(10), rulerY + S(34), "Bars", S(11), kDim);
    const double firstBar = meter.barIndex(job.tempo.secToBeat(t0)) + 1, lastBar = meter.barIndex(job.tempo.secToBeat(t1)) + 1;
    int barStep = 1;
    for (int s : {1, 2, 4, 8, 16, 32, 64, 128}) {
        barStep = s;
        const double b = std::max(1.0, firstBar);
        if (X(job.tempo.beatToSec(meter.barToBeat(b + s))) - X(job.tempo.beatToSec(meter.barToBeat(b))) >= S(34)) break;
    }
    std::vector<double> barLines;   // x of labelled bars, for faint guides in the lanes
    for (double b = std::max(1.0, firstBar); b <= lastBar; ++b) {
        const double x = X(job.tempo.beatToSec(meter.barToBeat(b)));
        if (x < plotX - 0.5 || x > plotX + plotW) continue;
        const bool labelled = ((long)b - 1) % barStep == 0;
        cv.rect(x, rulerY + S(22), x + 1, rulerY + S(labelled ? 28 : 25), labelled ? kDim : kGrid);
        if (labelled) {
            font.draw(cv, x + S(2), rulerY + S(37), std::to_string((long)b), S(11), kDim);
            barLines.push_back(x);
        }
    }

    // ---- loudness ---------------------------------------------------------------------------
    const double sr = job.sampleRate;
    auto guides = [&](int y0, int h) {
        for (double x : barLines) cv.rect(x, y0, x + 1, y0 + h, kWhite, 0.035);
        for (size_t k = 1; k < secs.size(); ++k) { const double x = X(secs[k].a); cv.rect(x, y0, x + 1, y0 + h, kWhite, 0.2); }
    };
    if (rendered) {
        const Audio &mix = *pic.mix;
        cv.rect(plotX, loudY, plotX + plotW, loudY + loudH, kPanel);
        const double lo = -42, hi = -2;
        auto Y = [&](double lufs) { return loudY + (1 - (std::clamp(lufs, lo, hi) - lo) / (hi - lo)) * loudH; };
        for (double g : {-36.0, -30.0, -24.0, -18.0, -14.0, -10.0, -6.0}) {
            cv.rect(plotX, Y(g), plotX + plotW, Y(g) + 1, g == -14 ? kDim : kGrid, g == -14 ? 0.5 : 1);
            const std::string t = fmt("%.0f", g);
            font.draw(cv, labelW - S(8) - font.width(t, S(11)), Y(g) + S(4), t, S(11), kDim);
        }
        for (auto &[a, b] : pic.dropouts) cv.rect(X(std::max(a, t0)), loudY, X(std::min(b, t1)), loudY + loudH, kRed, 0.28);
        guides(loudY, loudH);
        const size_t f0 = (size_t)(t0 * sr), f1 = std::min(mix.frames(), (size_t)(t1 * sr));
        const double hop = std::max(0.02, dur / plotW);
        // momentary (0.4 s) faintly behind, short-term (3 s) as the contour
        for (int pass = 0; pass < 2; ++pass) {
            const double win = pass == 0 ? 0.4 : 3.0;
            const std::vector<double> tl = loudnessTimeline(mix, (int)sr, win, hop, f0, f1);
            double prevY = -1;
            for (size_t i = 0; i < tl.size(); ++i) {
                const double x = X(t0 + i * hop + win / 2);
                if (x < plotX || x >= plotX + plotW) continue;
                if (tl[i] < -70) { prevY = -1; continue; }
                const double y = Y(tl[i]);
                if (pass == 1) cv.rect(x, y, x + hop / dur * plotW + 0.5, loudY + loudH, kAmber, 0.16);
                const double ya = prevY < 0 ? y : std::min(prevY, y), yb = prevY < 0 ? y : std::max(prevY, y);
                cv.rect(x, ya - (pass ? 1.0 : 0.5), x + hop / dur * plotW + 0.5, yb + (pass ? 1.0 : 0.5), pass ? kAmber : kWhite, pass ? 1.0 : 0.3);
                prevY = y;
            }
        }
        for (size_t k = 0; k < secs.size() && k < pic.sectionLufs.size(); ++k) {   // each section's integrated loudness
            const double v = pic.sectionLufs[k];
            if (v < -70) continue;
            const double xa = X(secs[k].a), xb = X(secs[k].b), y = Y(v);
            cv.rect(xa + 2, y - 1, xb - 2, y + 1, kWhite, 0.75);
            const std::string t = fmt("%.1f", v);
            if (xb - xa > font.width(t, S(12)) + S(8)) font.draw(cv, xa + S(4), y - S(4), t, S(12), kText);
        }
        // the arrangement check at each boundary it applies to: how far the section lands over the bars before it
        for (size_t k = 1; k < secs.size() && k < pic.sectionChecks.size(); ++k) {
            const auto [need, jump] = pic.sectionChecks[k];
            if (need <= 0) continue;
            const Rgb c = jump < need ? kRed : (need >= 2 && jump < 3) ? kAmber : kGreen;
            const double x = X(secs[k].a);
            cv.rect(x - 1, loudY, x + 2, loudY + loudH, c, 0.55);
            const std::string t = fmt("%+.1f", jump), up = jump >= 0 ? "^" : "v";
            const double w = font.width(t, S(12)), yb = loudY + loudH - S(8);
            cv.rect(x + S(3), yb - S(13), x + S(3) + w + S(18), yb + S(4), kPanel, 0.85);
            font.draw(cv, x + S(6), yb, up, S(12), c);
            font.draw(cv, x + S(16), yb, t, S(12), c);
        }
        font.draw(cv, S(10), loudY + S(18), "Loudness", S(15), kText);
        font.draw(cv, S(10), loudY + S(34), "LUFS, 3 s", S(11), kDim);
        font.draw(cv, S(10), loudY + S(48), "line = section", S(11), kDim);

        // ---- spectrum -----------------------------------------------------------------------
        cv.rect(plotX, specY, plotX + plotW, specY + specH, kPanel);
        const size_t N = 4096;
        const double fLo = 30, fHi = 16000, binHz = sr / N;
        std::vector<double> hann(N);
        double hsum = 0;
        for (size_t i = 0; i < N; ++i) { hann[i] = 0.5 - 0.5 * std::cos(2 * dsp::kPi * i / (N - 1)); hsum += hann[i]; }
        std::vector<std::complex<double>> buf(N);
        std::vector<double> mag(N / 2);
        for (int col = 0; col < (int)plotW; ++col) {
            const double t = t0 + (col + 0.5) / plotW * dur;
            const long c = (long)(t * sr);
            for (size_t i = 0; i < N; ++i) {
                const long k = c - (long)N / 2 + (long)i;
                const double v = k >= 0 && (size_t)k < mix.frames() ? 0.5 * (mix.left[(size_t)k] + mix.right[(size_t)k]) : 0.0;
                buf[i] = v * hann[i];
            }
            dsp::fft(buf);
            for (size_t i = 0; i < N / 2; ++i) mag[i] = std::abs(buf[i]) * 2 / hsum;
            for (int row = 0; row < specH; ++row) {
                const double ua = (specH - row - 1.0) / specH, ub = (specH - row + 0.0) / specH;
                const double fa = fLo * std::pow(fHi / fLo, ua), fb = fLo * std::pow(fHi / fLo, ub), fm = std::sqrt(fa * fb);
                double m;
                if (fb - fa < binHz) {   // below a bin: interpolate
                    const double pos = fm / binHz;
                    const size_t k = std::min(N / 2 - 2, (size_t)pos);
                    m = mag[k] + (mag[k + 1] - mag[k]) * (pos - k);
                } else {
                    m = 0;
                    for (size_t k = (size_t)(fa / binHz); k <= std::min(N / 2 - 1, (size_t)(fb / binHz)); ++k) m = std::max(m, mag[k]);
                }
                const double db = 20 * std::log10(std::max(m, 1e-9)) + 3 * std::log2(fm / 1000);   // +3 dB/oct: pink noise reads flat
                const double v = (db + 90) / 72;
                if (v > 0) cv.blend((int)plotX + col, specY + row, ramp(v), 1);
            }
        }
        for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
            const double y = specY + (1 - std::log(f / fLo) / std::log(fHi / fLo)) * specH;
            cv.rect(plotX, y, plotX + plotW, y + 1, kWhite, 0.07);
            const std::string t = f >= 1000 ? fmt("%.0fk", f / 1000) : fmt("%.0f", f);
            font.draw(cv, labelW - S(8) - font.width(t, S(11)), y + S(4), t, S(11), kDim);
        }
        guides(specY, specH);
        font.draw(cv, S(10), specY + S(18), "Spectrum", S(15), kText);
        font.draw(cv, S(10), specY + S(34), "mix, Hz", S(11), kDim);
    }

    // ---- tracks -----------------------------------------------------------------------------
    std::set<std::string> failed;
    for (auto &t : pic.tracks) if (t.failed) failed.insert(t.name);
    for (size_t i = 0; i < n; ++i) {
        const Track &tr = job.tracks[i];
        const PictureTrack *pt = i < pic.tracks.size() ? &pic.tracks[i] : nullptr;
        const int y0 = lanesY + laneH * (int)i;
        cv.rect(plotX, y0, plotX + plotW, y0 + laneH, i % 2 ? kPanelAlt : kPanel);
        const bool muted = tr.mute, bad = pt && pt->failed;
        const Rgb col = muted ? kMuted : kTracks[i % 8];
        cv.rect(0, y0 + 1, S(4), y0 + laneH - 1, col);
        // post-fader level, loudest 50 ms per pixel column: where the track is heard, and how loud
        if (pt && !pt->level.empty() && !muted) {
            const double hop = pic.levelHop;
            for (int c = 0; c < (int)plotW; ++c) {
                const double ta = t0 + c / plotW * dur, tb = t0 + (c + 1) / plotW * dur;
                size_t ka = (size_t)std::max(0.0, ta / hop), kb = (size_t)std::max(0.0, tb / hop);
                float ms = 0;
                for (size_t k = ka; k <= kb && k < pt->level.size(); ++k) ms = std::max(ms, pt->level[k]);
                if (ms <= 0) continue;
                const double db = 10 * std::log10(ms), v = std::clamp((db + 54) / 48, 0.0, 1.0);
                if (v > 0) cv.rect(plotX + c, y0 + laneH - 1 - v * (laneH - 4), plotX + c + 1, y0 + laneH - 1, col, 0.26);
            }
        }
        guides(y0, laneH);
        // notes: pitch within the track's own range (unpitched tracks: a row per key)
        std::vector<int> keys;
        for (auto &nt : tr.notes) keys.push_back(nt.key);
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        if (!keys.empty()) {
            const bool rows = unpitched(tr);
            const int kmin = keys.front(), kmax = keys.back();
            const double span = rows ? (double)keys.size() : (double)(kmax - kmin + 1);
            const double rowH = std::min(S(9), (laneH - 6) / std::max(1.0, span));   // a narrow range sits in the middle of the lane
            const double top = y0 + (laneH - rowH * span) / 2;
            for (auto &nt : tr.notes) {
                const double pos = rows ? (double)(std::lower_bound(keys.begin(), keys.end(), nt.key) - keys.begin()) : (double)(nt.key - kmin);
                const double y = top + (span - 1 - pos) * rowH;
                const double xa = X(nt.start), xb = std::max(xa + 1.5, X(nt.start + nt.length));
                if (xb < plotX || xa > plotX + plotW) continue;
                cv.rect(std::max(xa, plotX), y, std::min(xb, plotX + plotW), y + std::max(1.5, rowH - (rowH > 4 ? 1 : 0)), col, 0.45 + 0.55 * nt.velocity);
            }
        }
        // label: name, and its loudness or state
        std::string sub;
        Rgb subCol = kDim;
        if (bad) { sub = "failed"; subCol = kRed; }
        else if (muted) sub = "muted";
        const double inMix = pt ? (pt->postLufs > -70 ? pt->postLufs : pt->lufs) : -120;   // its level in the mix, not the raw stem
        if (bad || muted) {}
        else if (inMix > -70) sub = fmt("%.1f LUFS", inMix);
        else if (pt && rendered) sub = "silent";
        const double nameSize = S(laneH >= 30 ? 14 : 12);
        if (laneH >= S(34) && !sub.empty()) {
            font.draw(cv, S(12), y0 + laneH / 2.0 - S(2), ascii(font.fit(tr.name, nameSize, labelW - S(20))), nameSize, muted ? kDim : kText);
            font.draw(cv, S(12), y0 + laneH / 2.0 + S(13), sub, S(11), subCol);
        } else {
            const std::string right = sub.empty() ? "" : " " + (bad ? std::string("failed") : muted ? std::string("muted") : inMix > -70 ? fmt("%.0f", inMix) : "");
            const double rw = font.width(right, S(11));
            font.draw(cv, S(12), y0 + laneH / 2.0 + S(5), ascii(font.fit(tr.name, nameSize, labelW - S(20) - rw)), nameSize, muted ? kDim : kText);
            if (!right.empty()) font.draw(cv, labelW - S(8) - rw, y0 + laneH / 2.0 + S(5), right, S(11), subCol);
        }
    }

    // ---- time axis and legend -----------------------------------------------------------------
    double step = 1;
    for (double s : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0}) { step = s; if (s / dur * plotW >= S(64)) break; }
    for (double t = std::ceil(t0 / step) * step; t <= t1 + 1e-9; t += step) {
        const double x = X(t);
        cv.rect(x, axisY, x + 1, axisY + S(5), kDim);
        font.draw(cv, x + S(2), axisY + S(18), clock(t - t0), S(11), kDim);
    }
    font.draw(cv, S(10), axisY + S(18), "Time", S(11), kDim);
    font.draw(cv, S(10), H - S(8),
              rendered ? "Lanes: notes over post-fader level, LUFS after the track's fader. Spectrum: +3 dB/oct, 30 Hz-16 kHz. Red: dropouts. "
                         "Arrows: drop/lift jumps (last 2 bars vs first 4), green 3+ dB, amber passes, red weak."
                       : "Lanes: notes (pitch within each track's range; drums a row per key).",
              S(11), kDim);

    stbi_write_png_compression_level = 9;
    if (!stbi_write_png(path.c_str(), W, H, 3, cv.px.data(), W * 3)) { err = "cannot write " + path; return false; }
    height = H;
    return true;
}

} // namespace wl
