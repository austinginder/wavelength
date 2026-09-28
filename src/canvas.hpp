#pragma once
// Drawing for the PNG pictures (song pictures, patch cards): an RGB canvas with coverage-shaded
// rectangles, the web UI's JetBrains Mono (ASCII subset, built into the binary) and the colours.
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wl {

// the fonts of the web UI are built into the binary (cmake/embed-ui.cmake)
struct UiAsset { const char *path; const unsigned char *data; size_t size; };
extern const UiAsset kUiAssets[];
extern const size_t kUiAssetCount;

namespace gfx {

struct Rgb { uint8_t r, g, b; };
inline constexpr Rgb hex(uint32_t v) { return {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; }

inline const Rgb kBg = hex(0x0e1014), kPanel = hex(0x151820), kPanelAlt = hex(0x12151b), kGrid = hex(0x2a303b), kText = hex(0xe4e8ef),
          kDim = hex(0x8b94a4), kAmber = hex(0xf2b441), kRed = hex(0xe0525a), kGreen = hex(0x67c587), kMuted = hex(0x5a606b), kWhite = hex(0xffffff);
inline const Rgb kSections[] = {hex(0x2d3f5e), hex(0x45305e), hex(0x2b5a4c), hex(0x5e4a2d), hex(0x5e2f40), hex(0x2c5260)};
inline const Rgb kTracks[] = {hex(0x5ab0ff), hex(0xff8a5b), hex(0x7ddc7a), hex(0xd98cff), hex(0xffd35a), hex(0x4fd1c5), hex(0xff6fa8), hex(0xb5c46a)};

struct Canvas {
    int w, h;
    std::vector<uint8_t> px;
    Canvas(int w_, int h_, Rgb bg) : w(w_), h(h_), px((size_t)w_ * h_ * 3) {
        for (size_t i = 0; i < px.size(); i += 3) { px[i] = bg.r; px[i + 1] = bg.g; px[i + 2] = bg.b; }
    }
    void blend(int x, int y, Rgb c, double a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        a = std::min(1.0, a);
        uint8_t *p = &px[((size_t)y * w + x) * 3];
        p[0] = (uint8_t)std::lround(p[0] + (c.r - p[0]) * a);
        p[1] = (uint8_t)std::lround(p[1] + (c.g - p[1]) * a);
        p[2] = (uint8_t)std::lround(p[2] + (c.b - p[2]) * a);
    }
    // [x0, x1) x [y0, y1), fractional edges shaded by coverage
    void rect(double x0, double y0, double x1, double y1, Rgb c, double a = 1) {
        if (x1 <= x0 || y1 <= y0) return;
        const int ix0 = (int)std::floor(x0), ix1 = (int)std::ceil(x1), iy0 = (int)std::floor(y0), iy1 = (int)std::ceil(y1);
        for (int y = std::max(0, iy0); y < std::min(h, iy1); ++y) {
            const double cy = std::min<double>(y + 1, y1) - std::max<double>(y, y0);
            for (int x = std::max(0, ix0); x < std::min(w, ix1); ++x) {
                const double cx = std::min<double>(x + 1, x1) - std::max<double>(x, x0);
                blend(x, y, c, a * cx * cy);
            }
        }
    }
};

struct Font {
    stbtt_fontinfo info{};
    bool ok = false;
    Font() {
        for (size_t i = 0; i < kUiAssetCount; ++i)
            if (std::string(kUiAssets[i].path) == "fonts/jetbrains-mono-ascii.ttf")
                ok = stbtt_InitFont(&info, kUiAssets[i].data, 0) != 0;
    }
    double width(const std::string &s, double size) const {
        if (!ok) return s.size() * size * 0.6;
        const float scale = stbtt_ScaleForPixelHeight(&info, (float)size);
        double w = 0;
        for (unsigned char ch : s) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&info, ch, &adv, &lsb);
            w += adv * scale;
        }
        return w;
    }
    // draws `s` with its baseline at y; returns the width
    double draw(Canvas &cv, double x, double y, const std::string &s, double size, Rgb c, double a = 1) const {
        if (!ok) return 0;
        const float scale = stbtt_ScaleForPixelHeight(&info, (float)size);
        std::vector<unsigned char> bmp;
        const double start = x;
        for (unsigned char ch : s) {
            int adv, lsb, x0, y0, x1, y1;
            stbtt_GetCodepointHMetrics(&info, ch, &adv, &lsb);
            const float shift = (float)(x - std::floor(x));
            stbtt_GetCodepointBitmapBoxSubpixel(&info, ch, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
            const int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                bmp.assign((size_t)gw * gh, 0);
                stbtt_MakeCodepointBitmapSubpixel(&info, bmp.data(), gw, gh, gw, scale, scale, shift, 0, ch);
                const int bx = (int)std::floor(x) + x0, by = (int)std::lround(y) + y0;
                for (int j = 0; j < gh; ++j)
                    for (int i = 0; i < gw; ++i) cv.blend(bx + i, by + j, c, a * bmp[(size_t)j * gw + i] / 255.0);
            }
            x += adv * scale;
        }
        return x - start;
    }
    // `s` cut (with an ellipsis) to fit `maxW` pixels
    std::string fit(const std::string &s, double size, double maxW) const {
        if (width(s, size) <= maxW) return s;
        std::string t = s;
        while (!t.empty() && width(t + "\xe2\x80\xa6", size) > maxW) t.pop_back();
        return t.empty() ? t : t + "\xe2\x80\xa6";
    }
};

// The font draws bytes; the only non-ASCII character the picture uses is the ellipsis, drawn as "~".
inline std::string ascii(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) out += (char)c;
        else if (c == 0xe2 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0xa6) { out += "~"; i += 2; }
        else if ((c & 0xc0) != 0x80) out += '?';
    }
    return out;
}

inline std::string fmt(const char *f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

// magma-like colour ramp, 0 dark .. 1 bright
inline Rgb ramp(double t) {
    static const Rgb stops[] = {hex(0x0e1014), hex(0x2a1152), hex(0x6c1d81), hex(0xb5367a), hex(0xed5a5f), hex(0xfb9d61), hex(0xfcfdbf)};
    t = std::clamp(t, 0.0, 1.0) * 6;
    const int i = std::min(5, (int)t);
    const double f = t - i;
    const Rgb a = stops[i], b = stops[i + 1];
    return {(uint8_t)(a.r + (b.r - a.r) * f), (uint8_t)(a.g + (b.g - a.g) * f), (uint8_t)(a.b + (b.b - a.b) * f)};
}

} // namespace gfx
} // namespace wl
