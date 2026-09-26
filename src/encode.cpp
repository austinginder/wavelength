#include "encode.hpp"

#include "audio_file.hpp"
#include "effects.hpp"
#include "loudness.hpp"
#include "platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <type_traits>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace wl {

namespace {
// ---- samples ------------------------------------------------------------------------------
// interleaved stereo integers, lead-in first, `skip` frames left out; 16-bit gets TPDF dither (as writeWav)
std::vector<int32_t> quantize(const Audio &a, int bits, size_t lead, size_t skip) {
    skip = std::min(skip, a.frames());
    std::vector<int32_t> out((lead + a.frames() - skip) * 2, 0);
    const double full = bits == 16 ? 32767.0 : 8388607.0;
    uint32_t rng = 22222;
    auto tpdf = [&] {
        rng = rng * 1664525u + 1013904223u; const double r1 = (rng >> 8) / 16777216.0;
        rng = rng * 1664525u + 1013904223u; const double r2 = (rng >> 8) / 16777216.0;
        return r1 - r2;
    };
    size_t o = lead * 2;
    for (size_t i = skip; i < a.frames(); ++i)
        for (float s : {a.left[i], a.right[i]}) {
            const double v = s * full + (bits == 16 ? tpdf() : 0.0);
            out[o++] = (int32_t)std::lround(std::clamp(v, -full - 1, full));
        }
    return out;
}

// ---- MD5 (RFC 1321), for the FLAC STREAMINFO signature --------------------------------------
struct Md5 {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint8_t block[64];
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void compress(const uint8_t *p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
            0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
            0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
            0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
            0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t t = d;
            d = c; c = b;
            b = b + rol(a + f + K[i] + m[g], S[i]);
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    void add(const uint8_t *p, size_t n) {
        total += n;
        while (n) {
            const size_t k = std::min(n, 64 - used);
            std::memcpy(block + used, p, k);
            used += k; p += k; n -= k;
            if (used == 64) { compress(block); used = 0; }
        }
    }
    std::array<uint8_t, 16> finish() {
        const uint64_t bitsLen = total * 8;
        const uint8_t pad = 0x80, zero = 0;
        add(&pad, 1);
        while (used != 56) add(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bitsLen >> (8 * i));
        add(len, 8);
        std::array<uint8_t, 16> out;
        for (int i = 0; i < 16; ++i) out[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
        return out;
    }
};

// ---- FLAC ---------------------------------------------------------------------------------
struct BitWriter {
    std::vector<uint8_t> buf;
    uint64_t acc = 0;
    int n = 0;   // bits waiting in acc (< 8 between calls)
    void put(uint64_t v, int bits) {   // bits <= 32
        if (!bits) return;
        acc = (acc << bits) | (v & ((1ull << bits) - 1));
        n += bits;
        while (n >= 8) { buf.push_back((uint8_t)(acc >> (n - 8))); n -= 8; }
        acc &= (1ull << n) - 1;
    }
    void unary(uint32_t zeros) {   // `zeros` 0 bits, then a 1
        while (zeros >= 32) { put(0, 32); zeros -= 32; }
        put(1, (int)zeros + 1);
    }
    void align() { if (n) put(0, 8 - n); }
};

uint8_t crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; ++b) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}

void residual(const int64_t *x, size_t n, int order, std::vector<int64_t> &r) {
    r.resize(n - order);
    for (size_t i = order; i < n; ++i) {
        int64_t v;
        switch (order) {
        case 0: v = x[i]; break;
        case 1: v = x[i] - x[i - 1]; break;
        case 2: v = x[i] - 2 * x[i - 1] + x[i - 2]; break;
        case 3: v = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]; break;
        default: v = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4]; break;
        }
        r[i - order] = v;
    }
}

uint64_t zigzag(int64_t v) { return v >= 0 ? (uint64_t)v << 1 : ((uint64_t)(-v) << 1) - 1; }

// the cheapest Rice parameter for one partition, and its bit count
uint64_t riceBits(const uint64_t *u, size_t n, int &bestK) {
    if (!n) { bestK = 0; return 0; }
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum += u[i];
    const double mean = (double)sum / n;
    const int guess = std::clamp(mean > 1 ? (int)std::floor(std::log2(mean)) : 0, 0, 30);
    uint64_t best = UINT64_MAX;
    for (int k = std::max(0, guess - 1); k <= std::min(30, guess + 1); ++k) {
        uint64_t bits = (uint64_t)n * (k + 1);
        for (size_t i = 0; i < n; ++i) bits += u[i] >> k;
        if (bits < best) { best = bits; bestK = k; }
    }
    return best;
}

struct Coded { int order = 0, partOrder = 0; std::vector<int> params; uint64_t bits = 0; };

// the best partition order for a fixed-predictor residual of a block of `bs` samples
Coded planResidual(const std::vector<int64_t> &r, size_t bs, int order) {
    std::vector<uint64_t> u(r.size());
    for (size_t i = 0; i < r.size(); ++i) u[i] = zigzag(r[i]);
    Coded best;
    best.bits = UINT64_MAX;
    for (int p = 0; p <= 8; ++p) {
        if (bs % (1u << p) || (bs >> p) <= (size_t)order) break;
        const size_t part = bs >> p;
        Coded c;
        c.order = order; c.partOrder = p;
        c.bits = 6;
        size_t at = 0;
        for (size_t i = 0; i < ((size_t)1 << p); ++i) {
            const size_t cnt = i == 0 ? part - order : part;
            int k = 0;
            c.bits += 5 + riceBits(u.data() + at, cnt, k);
            c.params.push_back(k);
            at += cnt;
        }
        if (c.bits < best.bits) best = c;
    }
    return best;
}

// one channel of a block: constant, fixed predictor (order 0-4) or verbatim, whichever is smallest
void encodeSubframe(BitWriter &w, const std::vector<int64_t> &x, int bps) {
    const size_t bs = x.size();
    if (std::all_of(x.begin(), x.end(), [&](int64_t v) { return v == x[0]; })) {
        w.put(0, 1); w.put(0, 6); w.put(0, 1);
        w.put((uint64_t)x[0], bps);
        return;
    }
    std::vector<int64_t> r;
    Coded best;
    best.bits = UINT64_MAX;
    std::vector<int64_t> bestR;
    for (int o = 0; o <= 4 && (size_t)o < bs; ++o) {
        residual(x.data(), bs, o, r);
        Coded c = planResidual(r, bs, o);
        c.bits += (uint64_t)o * bps;
        if (c.bits < best.bits) { best = c; bestR = r; }
    }
    if (best.bits == UINT64_MAX || best.bits >= (uint64_t)bs * bps) {   // verbatim
        w.put(0, 1); w.put(1, 6); w.put(0, 1);
        for (int64_t v : x) w.put((uint64_t)v, bps);
        return;
    }
    w.put(0, 1); w.put(0x08 | best.order, 6); w.put(0, 1);
    for (int i = 0; i < best.order; ++i) w.put((uint64_t)x[i], bps);
    w.put(1, 2);                 // Rice coding with 5-bit parameters
    w.put(best.partOrder, 4);
    size_t at = 0;
    const size_t part = bs >> best.partOrder;
    for (size_t i = 0; i < best.params.size(); ++i) {
        const int k = best.params[i];
        w.put(k, 5);
        const size_t cnt = i == 0 ? part - best.order : part;
        for (size_t j = 0; j < cnt; ++j) {
            const uint64_t u = zigzag(bestR[at + j]);
            w.unary((uint32_t)(u >> k));
            w.put(u, k);
        }
        at += cnt;
    }
}

// a cost estimate for choosing the stereo mode: the smallest sum of fixed-predictor residuals
uint64_t roughCost(const std::vector<int64_t> &x) {
    uint64_t best = UINT64_MAX;
    std::vector<int64_t> r;
    for (int o = 0; o <= 4 && (size_t)o < x.size(); ++o) {
        residual(x.data(), x.size(), o, r);
        uint64_t s = 0;
        for (int64_t v : r) s += (uint64_t)(v < 0 ? -v : v);
        best = std::min(best, s);
    }
    return best;
}

void putUtf8(BitWriter &w, uint64_t v) {
    if (v < 0x80) { w.put(v, 8); return; }
    int bytes = 2;
    while (bytes < 7 && v >= (1ull << (5 * bytes + 1))) ++bytes;
    w.put(((0xFF00 >> bytes) & 0xFF) | (v >> (6 * (bytes - 1))), 8);
    for (int i = bytes - 2; i >= 0; --i) w.put(0x80 | ((v >> (6 * i)) & 0x3F), 8);
}

int rateCode(int sr) {
    switch (sr) {
    case 88200: return 1; case 176400: return 2; case 192000: return 3; case 8000: return 4; case 16000: return 5;
    case 22050: return 6; case 24000: return 7; case 32000: return 8; case 44100: return 9; case 48000: return 10; case 96000: return 11;
    default: return 0;   // from STREAMINFO
    }
}

// ---- MP3 ----------------------------------------------------------------------------------
struct Lame {
    void *(*init)() = nullptr;
    int (*setInRate)(void *, int) = nullptr;
    int (*setChannels)(void *, int) = nullptr;
    int (*setBrate)(void *, int) = nullptr;
    int (*setQuality)(void *, int) = nullptr;
    int (*setMode)(void *, int) = nullptr;
    int (*setVbr)(void *, int) = nullptr;
    int (*setVbrTag)(void *, int) = nullptr;
    void (*setId3Auto)(void *, int) = nullptr;
    int (*initParams)(void *) = nullptr;
    int (*encodeFloat)(void *, const float *, const float *, int, unsigned char *, int) = nullptr;
    int (*flush)(void *, unsigned char *, int) = nullptr;
    size_t (*lametag)(void *, unsigned char *, size_t) = nullptr;
    int (*close)(void *) = nullptr;
    const char *(*version)() = nullptr;
    std::string library;
    bool ok = false;
};

const Lame &lame() {
    static Lame l;
    static bool tried = false;
    if (tried) return l;
    tried = true;
    std::vector<std::string> names;
    const char *env = std::getenv("WAVELENGTH_LAME");
    if (env && std::string(env) == "none") return l;   // use ffmpeg
    if (env) names.push_back(env);
#if defined(__APPLE__)
    names.insert(names.end(), {"/opt/homebrew/lib/libmp3lame.dylib", "/opt/homebrew/lib/libmp3lame.0.dylib", "/usr/local/lib/libmp3lame.dylib",
                               "/usr/local/lib/libmp3lame.0.dylib", "/opt/local/lib/libmp3lame.dylib", "libmp3lame.dylib", "libmp3lame.0.dylib"});
#elif defined(_WIN32)
    names.insert(names.end(), {"libmp3lame.dll", "libmp3lame-0.dll", "mp3lame.dll"});
#else
    names.insert(names.end(), {"libmp3lame.so.0", "libmp3lame.so"});
#endif
    void *lib = platform::openSharedLibrary(names, l.library);
    if (!lib) return l;
    auto sym = [&](auto &fn, const char *name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(platform::sharedSymbol(lib, name)); return fn != nullptr; };
    l.ok = sym(l.init, "lame_init") && sym(l.setInRate, "lame_set_in_samplerate") && sym(l.setChannels, "lame_set_num_channels") &&
           sym(l.setBrate, "lame_set_brate") && sym(l.setQuality, "lame_set_quality") && sym(l.setMode, "lame_set_mode") &&
           sym(l.setVbr, "lame_set_VBR") && sym(l.setVbrTag, "lame_set_bWriteVbrTag") && sym(l.setId3Auto, "lame_set_write_id3tag_automatic") &&
           sym(l.initParams, "lame_init_params") && sym(l.encodeFloat, "lame_encode_buffer_ieee_float") &&
           sym(l.flush, "lame_encode_flush") && sym(l.lametag, "lame_get_lametag_frame") && sym(l.close, "lame_close");
    sym(l.version, "get_lame_version");
    return l;
}

bool mp3WithLame(const Lame &L, const std::string &path, const Audio &a, int sr, int kbps, size_t lead, size_t skip, std::string &err) {
    void *g = L.init();
    if (!g) { err = "LAME did not start"; return false; }
    L.setInRate(g, sr);
    L.setChannels(g, 2);
    L.setVbr(g, 0);            // CBR
    L.setBrate(g, kbps);
    L.setMode(g, 1);           // joint stereo
    L.setQuality(g, 2);        // LAME's "high quality" (-h)
    L.setVbrTag(g, 1);         // Info/LAME tag: encoder delay and padding, for gapless decoding
    L.setId3Auto(g, 0);
    if (L.initParams(g) < 0) { L.close(g); err = "LAME rejected the settings (" + std::to_string(sr) + " Hz, " + std::to_string(kbps) + " kbps)"; return false; }
    std::ofstream o(fs::path(path), std::ios::binary);
    if (!o) { L.close(g); err = "cannot write " + path; return false; }
    const size_t chunk = 8192;
    std::vector<unsigned char> out(chunk * 5 / 4 + 7200);
    std::vector<float> l(chunk), r(chunk);
    skip = std::min(skip, a.frames());
    const size_t total = lead + a.frames() - skip;
    for (size_t at = 0; at < total; at += chunk) {
        const size_t n = std::min(chunk, total - at);
        for (size_t i = 0; i < n; ++i) {
            const size_t f = at + i;
            l[i] = f < lead ? 0.f : a.left[f - lead + skip];
            r[i] = f < lead ? 0.f : a.right[f - lead + skip];
        }
        const int bytes = L.encodeFloat(g, l.data(), r.data(), (int)n, out.data(), (int)out.size());
        if (bytes < 0) { L.close(g); err = "LAME failed while encoding (" + std::to_string(bytes) + ")"; return false; }
        o.write(reinterpret_cast<const char *>(out.data()), bytes);
    }
    const int tail = L.flush(g, out.data(), (int)out.size());
    if (tail > 0) o.write(reinterpret_cast<const char *>(out.data()), tail);
    const size_t tag = L.lametag(g, out.data(), out.size());
    if (tag > 0 && tag <= out.size()) { o.seekp(0); o.write(reinterpret_cast<const char *>(out.data()), (std::streamsize)tag); }   // over the placeholder frame
    L.close(g);
    if (!o) { err = "write failed for " + path + " (disk full?)"; return false; }
    return true;
}

bool mp3WithFfmpeg(const std::string &ffmpeg, const std::string &wav, const std::string &path, int kbps, std::string &err) {
    platform::Process p;
    const std::vector<std::string> args = {ffmpeg, "-y", "-loglevel", "error", "-i", wav, "-c:a", "libmp3lame", "-b:a", std::to_string(kbps) + "k", path};
    if (!platform::spawn(args, p, false, false)) { err = "cannot start " + ffmpeg; return false; }
    std::string crash;
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (!platform::finished(p, crash)) {
        if (std::chrono::steady_clock::now() > until) { platform::kill(p); err = "ffmpeg took over 10 minutes"; return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::error_code ec;
    if (!crash.empty() || !fs::exists(path, ec) || fs::file_size(path, ec) == 0) { err = "ffmpeg did not write " + path + (crash.empty() ? "" : " (" + crash + ")"); return false; }
    return true;
}

double r1(double v) { return std::round(v * 10) / 10; }
} // namespace

bool writeFlac(const std::string &path, const Audio &a, int sampleRate, int bits, size_t leadFrames, size_t skipFrames, std::string &err) {
    if (bits != 16) bits = 24;
    const std::vector<int32_t> pcm = quantize(a, bits, leadFrames, skipFrames);
    const size_t frames = pcm.size() / 2, block = 4096;
    Md5 md5;
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(pcm.size() * (bits / 8));
        for (int32_t s : pcm) for (int b = 0; b < bits / 8; ++b) bytes.push_back((uint8_t)(s >> (8 * b)));
        md5.add(bytes.data(), bytes.size());
    }
    std::vector<uint8_t> body;
    uint32_t minFrame = UINT32_MAX, maxFrame = 0;
    std::vector<int64_t> L, R, M, S;
    for (size_t at = 0, num = 0; at < frames; at += block, ++num) {
        const size_t bs = std::min(block, frames - at);
        L.resize(bs); R.resize(bs); M.resize(bs); S.resize(bs);
        for (size_t i = 0; i < bs; ++i) {
            L[i] = pcm[(at + i) * 2]; R[i] = pcm[(at + i) * 2 + 1];
            M[i] = (L[i] + R[i]) >> 1; S[i] = L[i] - R[i];
        }
        const uint64_t cl = roughCost(L), cr = roughCost(R), cm = roughCost(M), cs = roughCost(S);
        // 1 = left/right, 8 = left/side, 9 = side/right, 10 = mid/side
        int mode = 1;
        uint64_t best = cl + cr;
        if (cl + cs < best) { best = cl + cs; mode = 8; }
        if (cs + cr < best) { best = cs + cr; mode = 9; }
        if (cm + cs < best) { best = cm + cs; mode = 10; }
        BitWriter w;
        w.put(0x3FFE, 14); w.put(0, 1); w.put(0, 1);   // sync, reserved, fixed block size
        w.put(7, 4);                                    // block size: 16 bits at the end of the header
        w.put(rateCode(sampleRate), 4);
        w.put(mode, 4);
        w.put(bits == 16 ? 4 : 6, 3); w.put(0, 1);
        putUtf8(w, num);
        w.put(bs - 1, 16);
        w.put(crc8(w.buf.data(), w.buf.size()), 8);
        const int sb = bits + 1;   // the side channel needs one more bit
        switch (mode) {
        case 1: encodeSubframe(w, L, bits); encodeSubframe(w, R, bits); break;
        case 8: encodeSubframe(w, L, bits); encodeSubframe(w, S, sb); break;
        case 9: encodeSubframe(w, S, sb); encodeSubframe(w, R, bits); break;
        default: encodeSubframe(w, M, bits); encodeSubframe(w, S, sb); break;
        }
        w.align();
        const uint16_t crc = crc16(w.buf.data(), w.buf.size());
        w.put(crc, 16);
        minFrame = std::min(minFrame, (uint32_t)w.buf.size());
        maxFrame = std::max(maxFrame, (uint32_t)w.buf.size());
        body.insert(body.end(), w.buf.begin(), w.buf.end());
    }
    BitWriter h;
    for (char c : std::string("fLaC")) h.put((uint8_t)c, 8);
    h.put(0, 1); h.put(0, 7); h.put(34, 24);   // STREAMINFO (not the last block)
    const uint32_t bsMin = frames < block ? (uint32_t)std::max<size_t>(16, frames) : (uint32_t)block;
    h.put(bsMin, 16); h.put(bsMin, 16);
    h.put(frames ? minFrame : 0, 24); h.put(maxFrame, 24);
    h.put(sampleRate, 20); h.put(1, 3); h.put(bits - 1, 5);
    h.put((uint64_t)frames >> 32, 4); h.put(frames & 0xFFFFFFFF, 32);
    for (uint8_t b : md5.finish()) h.put(b, 8);
    const std::string vendor = std::string("Wavelength ") + WAVELENGTH_VERSION;
    h.put(1, 1); h.put(4, 7); h.put(4 + vendor.size() + 4, 24);   // VORBIS_COMMENT, last: vendor, no comments
    auto le32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) h.put((v >> (8 * i)) & 0xFF, 8); };
    le32((uint32_t)vendor.size());
    for (char c : vendor) h.put((uint8_t)c, 8);
    le32(0);
    std::ofstream o(fs::path(path), std::ios::binary);
    if (!o) { err = "cannot write " + path; return false; }
    o.write(reinterpret_cast<const char *>(h.buf.data()), (std::streamsize)h.buf.size());
    o.write(reinterpret_cast<const char *>(body.data()), (std::streamsize)body.size());
    if (!o) { err = "write failed for " + path + " (disk full?)"; return false; }
    return true;
}

bool parseDeliverySpec(const std::string &text, DeliverySpec &out, std::string &err) {
    out = DeliverySpec{};
    try {
        if (!text.empty() && text[0] == '{') {
            const json j = json::parse(text);
            out.format = j.value("format", "");
            out.bitrate = j.value("bitrate", 320);
            out.file = j.value("file", "");
            if (j.contains("bits")) out.bits = j["bits"].get<int>();
            else if (out.format == "wav") out.bits = 16;
            for (auto &[k, v] : j.items())
                if (k != "format" && k != "bitrate" && k != "bits" && k != "file") { err = "deliver: unknown setting '" + k + "'"; return false; }
        } else {
            const size_t c = text.find(':');
            out.format = text.substr(0, c);
            const int n = c == std::string::npos ? 0 : std::atoi(text.c_str() + c + 1);
            if (out.format == "mp3" && n) out.bitrate = n;
            else if (out.format == "wav") out.bits = n ? n : 16;
            else if (n) out.bits = n;
        }
    } catch (const std::exception &e) { err = std::string("deliver: ") + e.what(); return false; }
    for (auto &ch : out.format) ch = (char)std::tolower((unsigned char)ch);
    static const int rates[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    if (out.format == "mp3") {
        if (std::find(std::begin(rates), std::end(rates), out.bitrate) == std::end(rates)) { err = "deliver: mp3 bitrate must be one of 32-320 kbps (MPEG-1 layer III rates), not " + std::to_string(out.bitrate); return false; }
    } else if (out.format == "flac") {
        if (out.bits != 16 && out.bits != 24) { err = "deliver: flac bits must be 16 or 24"; return false; }
    } else if (out.format == "wav") {
        if (out.bits != 16 && out.bits != 24 && out.bits != 32) { err = "deliver: wav bits must be 16, 24 or 32"; return false; }
    } else { err = "deliver: format must be \"mp3\", \"flac\" or \"wav\" (got \"" + out.format + "\")"; return false; }
    return true;
}

std::string deliveryPath(const DeliverySpec &spec, const std::string &outDir) {
    std::string name = spec.file;
    if (name.empty()) {
        if (spec.format == "wav") name = "mix-" + std::to_string(spec.bits) + "bit.wav";
        else if (spec.format == "flac" && spec.bits == 16) name = "mix-16bit.flac";
        else name = "mix." + spec.format;
    }
    fs::path p(name);
    if (p.is_relative()) p = fs::path(outDir) / p;
    return p.string();
}

bool writeDeliveries(const std::vector<DeliverySpec> &specs, const Audio &mix, int sampleRate, size_t leadFrames, size_t skipFrames,
                     const std::string &outDir, const std::string &wavPath, double mixTruePeakDb, std::vector<Delivery> &out,
                     std::vector<std::string> &warnings, std::string &err) {
    for (size_t i = 0; i < specs.size(); ++i)
        for (size_t k = 0; k < i; ++k)
            if (deliveryPath(specs[i], outDir) == deliveryPath(specs[k], outDir)) {
                err = "deliver: two entries write " + fs::path(deliveryPath(specs[i], outDir)).filename().string() + "; give one a \"file\"";
                return false;
            }
    for (const auto &spec : specs) {
        Delivery d;
        d.spec = spec;
        const fs::path p(deliveryPath(spec, outDir));
        std::error_code ec;
        if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
        d.file = p.string();
        if (spec.format == "flac") {
            if (!writeFlac(d.file, mix, sampleRate, spec.bits, leadFrames, skipFrames, err)) return false;
            d.encoder = "wavelength";
        } else if (spec.format == "wav") {
            if (!writeWav(d.file, mix, sampleRate, err, spec.bits, leadFrames, skipFrames)) return false;
            d.encoder = "wavelength";
        } else {
            const Lame &L = lame();
            if (L.ok) {
                if (!mp3WithLame(L, d.file, mix, sampleRate, spec.bitrate, leadFrames, skipFrames, err)) return false;
                d.encoder = std::string("LAME ") + (L.version ? L.version() : "") + " (" + L.library + ")";
            } else if (const std::string ff = platform::findProgram("ffmpeg"); !ff.empty()) {
                if (!mp3WithFfmpeg(ff, wavPath, d.file, spec.bitrate, err)) return false;
                d.encoder = "ffmpeg (" + ff + ")";
            } else {
                err = "deliver mp3: no MP3 encoder found. Install LAME (macOS: brew install lame; Debian/Ubuntu: apt install libmp3lame0; "
                      "Windows: libmp3lame.dll next to wavelength.exe) or ffmpeg, or set $WAVELENGTH_LAME to libmp3lame's path";
                return false;
            }
        }
        // what a listener gets: decode the file again and measure it
        Audio back;
        int sr = 0;
        std::string e2;
        if (!readAudio(d.file, back, sr, e2)) { err = "deliver: cannot read back " + d.file + ": " + e2; return false; }
        d.truePeakDb = truePeakDb(back);
        d.lufs = integratedLufs(back, sr);
        d.peakDb = measure(back).peakDb;
        d.overshootDb = d.truePeakDb - mixTruePeakDb;
        if (spec.format == "mp3" && d.truePeakDb > -1.0) {   // lossless files peak where mix.wav does
            char buf[320];
            std::snprintf(buf, sizeof buf, "deliver: %s decodes to a true peak of %.1f dBTP (%+.1f dB over mix.wav's %.1f; MP3 encoding adds overshoot): "
                          "lower the master limiter's ceiling by about %.1f dB to keep it under -1 dBTP",
                          p.filename().string().c_str(), r1(d.truePeakDb), r1(d.overshootDb), r1(mixTruePeakDb), r1(d.truePeakDb + 1.0 + 0.05));
            warnings.push_back(buf);
        }
        out.push_back(d);
    }
    return true;
}

} // namespace wl
