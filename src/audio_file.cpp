#include "audio_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace wl {

namespace {
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3]; }

// integer or float samples, little- or big-endian, into l/r
bool interleaved(const uint8_t *data, size_t len, int channels, int bits, bool flt, bool bigEndian, bool unsigned8,
                 DecodedAudio &s, std::string &err) {
    if (channels < 1 || !(flt ? (bits == 32 || bits == 64) : (bits == 8 || bits == 16 || bits == 24 || bits == 32))) {
        err = "unsupported encoding (" + std::to_string(bits) + "-bit " + (flt ? "float" : "PCM") + ", " + std::to_string(channels) + " channels)";
        return false;
    }
    const size_t bps = bits / 8, frame = bps * channels, n = len / frame;
    s.l.resize(n);
    if (channels > 1) s.r.resize(n);
    uint8_t t[8];
    auto get = [&](const uint8_t *q) -> float {
        if (bigEndian) { for (size_t i = 0; i < bps; ++i) t[i] = q[bps - 1 - i]; q = t; }
        if (flt) { if (bits == 32) { float f; std::memcpy(&f, q, 4); return f; } double v; std::memcpy(&v, q, 8); return (float)v; }
        switch (bits) {
        case 8: return unsigned8 ? (q[0] - 128) / 128.f : (int8_t)q[0] / 128.f;
        case 16: return (int16_t)le16(q) / 32768.f;
        case 24: return (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.f;
        default: return (float)((int32_t)le32(q) / 2147483648.0);
        }
    };
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *q = data + i * frame;
        s.l[i] = get(q);
        if (channels > 1) s.r[i] = get(q + bps);
    }
    return true;
}

bool decodeWav(const uint8_t *d, size_t size, DecodedAudio &s, std::string &err) {
    int format = 0, channels = 0, bits = 0;
    const uint8_t *data = nullptr;
    size_t dataLen = 0;
    for (size_t p = 12; p + 8 <= size;) {
        const uint32_t len = le32(d + p + 4);
        const uint8_t *body = d + p + 8;
        const size_t avail = std::min<size_t>(len, size - p - 8);
        if (!std::memcmp(d + p, "fmt ", 4) && avail >= 16) {
            format = le16(body); channels = le16(body + 2); s.rate = le32(body + 4); bits = le16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = le16(body + 24);   // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (!std::memcmp(d + p, "data", 4)) {
            data = body; dataLen = avail;
        } else if (!std::memcmp(d + p, "smpl", 4) && avail >= 36 + 24 && le32(body + 28) > 0) {
            s.loopStart = le32(body + 36 + 8);          // first loop: cue id, type, start, end (inclusive), ...
            s.loopEnd = (double)le32(body + 36 + 12) + 1;
        }
        p += 8 + (size_t)len + (len & 1);
    }
    if (!data || channels < 1) { err = "WAV has no fmt/data chunk"; return false; }
    if (format != 1 && format != 3) { err = "unsupported WAV encoding (format " + std::to_string(format) + ")"; return false; }
    return interleaved(data, dataLen, channels, bits, format == 3, false, true, s, err);
}

// 80-bit IEEE 754 extended (AIFF sample rate)
double extended(const uint8_t *p) {
    const int exp = ((p[0] & 0x7f) << 8 | p[1]) - 16383 - 63;
    uint64_t mant = 0;
    for (int i = 0; i < 8; ++i) mant = mant << 8 | p[2 + i];
    const double v = std::ldexp((double)mant, exp);
    return p[0] & 0x80 ? -v : v;
}

bool decodeAiff(const uint8_t *d, size_t size, DecodedAudio &s, std::string &err) {
    const bool aifc = !std::memcmp(d + 8, "AIFC", 4);
    int channels = 0, bits = 0;
    std::string comp = "NONE";
    const uint8_t *data = nullptr;
    size_t dataLen = 0;
    for (size_t p = 12; p + 8 <= size;) {
        const uint32_t len = be32(d + p + 4);
        const uint8_t *body = d + p + 8;
        const size_t avail = std::min<size_t>(len, size - p - 8);
        if (!std::memcmp(d + p, "COMM", 4) && avail >= 18) {
            channels = be16(body); bits = be16(body + 6); s.rate = extended(body + 8);
            if (aifc && avail >= 22) comp.assign((const char *)body + 18, 4);
        } else if (!std::memcmp(d + p, "SSND", 4) && avail >= 8) {
            const uint32_t offset = be32(body);
            if (offset + 8 <= avail) { data = body + 8 + offset; dataLen = avail - 8 - offset; }
        }
        p += 8 + (size_t)len + (len & 1);
    }
    if (!data || channels < 1) { err = "AIFF has no COMM/SSND chunk"; return false; }
    std::string c = comp;
    for (auto &ch : c) ch = (char)std::tolower((unsigned char)ch);
    if (c == "none" || c == "twos") return interleaved(data, dataLen, channels, (bits + 7) / 8 * 8, false, true, false, s, err);
    if (c == "sowt") return interleaved(data, dataLen, channels, (bits + 7) / 8 * 8, false, false, false, s, err);
    if (c == "fl32") return interleaved(data, dataLen, channels, 32, true, true, false, s, err);
    if (c == "fl64") return interleaved(data, dataLen, channels, 64, true, true, false, s, err);
    err = "unsupported AIFF-C compression '" + comp + "'";
    return false;
}

uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// Core Audio Format: "caff" + version, then chunks (4-byte type, i64 BE size; -1 = to the end of the
// file for 'data'). Linear PCM only ('lpcm'; flags: 1 float, 2 little-endian).
struct CafInfo { double rate = 0; int channels = 0, bits = 0; bool flt = false, little = false; size_t dataAt = 0, dataLen = 0; std::string format; };
bool cafInfo(const uint8_t *d, size_t size, size_t fileSize, CafInfo &c, std::string &err) {
    if (size < 8 || std::memcmp(d, "caff", 4)) { err = "not a CAF file"; return false; }
    for (size_t p = 8; p + 12 <= size;) {
        const int64_t len = (int64_t)be64(d + p + 4);
        const uint8_t *body = d + p + 12;
        if (!std::memcmp(d + p, "desc", 4) && p + 12 + 32 <= size) {
            uint64_t bits = be64(body);
            std::memcpy(&c.rate, &bits, 8);
            c.format.assign((const char *)body + 8, 4);
            const uint32_t flags = be32(body + 12);
            c.channels = (int)be32(body + 24);
            c.bits = (int)be32(body + 28);
            c.flt = flags & 1;
            c.little = flags & 2;
        } else if (!std::memcmp(d + p, "data", 4)) {
            c.dataAt = p + 12 + 4;   // after the edit count
            c.dataLen = len < 0 ? fileSize - c.dataAt : std::min<size_t>((size_t)len - 4, fileSize - c.dataAt);
            break;   // data is the last chunk we need (a -1 size runs to the end)
        }
        if (len < 0) break;
        p += 12 + (size_t)len;
    }
    if (!c.dataAt || c.channels < 1) { err = "CAF has no desc/data chunk"; return false; }
    if (c.format != "lpcm") { err = "unsupported CAF encoding '" + c.format + "' (linear PCM only)"; return false; }
    return true;
}

bool decodeCaf(const uint8_t *d, size_t size, DecodedAudio &s, std::string &err) {
    CafInfo c;
    if (!cafInfo(d, size, size, c, err)) return false;
    s.rate = c.rate;
    return interleaved(d + c.dataAt, std::min(c.dataLen, size - c.dataAt), c.channels, c.bits, c.flt, !c.little, false, s, err);
}

bool isMp3(const uint8_t *d, size_t size) {
    if (size >= 3 && !std::memcmp(d, "ID3", 3)) return true;
    return size >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0 && (d[1] & 0x06) != 0;   // frame sync, layer set
}
} // namespace

bool decodeAudio(const uint8_t *d, size_t size, DecodedAudio &out, std::string &err) {
    out = DecodedAudio{};
    bool ok = false;
    if (size >= 12 && !std::memcmp(d, "RIFF", 4) && !std::memcmp(d + 8, "WAVE", 4)) ok = decodeWav(d, size, out, err);
    else if (size >= 12 && !std::memcmp(d, "FORM", 4) && (!std::memcmp(d + 8, "AIFF", 4) || !std::memcmp(d + 8, "AIFC", 4))) ok = decodeAiff(d, size, out, err);
    else if (size >= 8 && !std::memcmp(d, "caff", 4)) ok = decodeCaf(d, size, out, err);
    else if (size >= 4 && !std::memcmp(d, "fLaC", 4)) ok = codecs::flac(d, size, out, err);
    else if (size >= 4 && !std::memcmp(d, "OggS", 4)) {
        if (size >= 36 + 8 && !std::memcmp(d + 28, "OpusHead", 8)) { err = "Ogg Opus isn't supported (only Ogg Vorbis): convert it to FLAC or WAV"; return false; }
        ok = codecs::vorbis(d, size, out, err);
    }
    else if (isMp3(d, size)) ok = codecs::mp3(d, size, out, err);
    else { err = "not a WAV, AIFF, CAF, FLAC, MP3 or Ogg Vorbis file"; return false; }
    if (ok && !(out.rate > 0)) { err = "the file gives no sample rate"; return false; }
    return ok;
}

bool readAudio(const std::string &path, Audio &out, int &sampleRate, std::string &err) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) { err = "cannot read " + path; return false; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    DecodedAudio d;
    if (!decodeAudio(bytes.data(), bytes.size(), d, err)) { err = path + ": " + err; return false; }
    sampleRate = (int)std::lround(d.rate);
    out.left = std::move(d.l);
    out.right = d.r.empty() ? out.left : std::move(d.r);
    return true;
}

bool readAudioFrames(const std::string &path, double from, double to, DecodedAudio &out, std::string &err) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) { err = "cannot read " + path; return false; }
    in.seekg(0, std::ios::end);
    const size_t fileSize = (size_t)in.tellg();
    in.seekg(0);
    std::vector<uint8_t> head(std::min<size_t>(fileSize, 65536));
    in.read(reinterpret_cast<char *>(head.data()), (std::streamsize)head.size());
    const size_t a = (size_t)std::max(0.0, from), b = (size_t)std::max(from, to);
    // PCM in a CAF or WAV: read just the frames asked for
    CafInfo c;
    std::string e2;
    size_t dataAt = 0, dataLen = 0, frame = 0;
    int channels = 0, bits = 0;
    bool flt = false, big = false, known = false;
    if (head.size() >= 8 && !std::memcmp(head.data(), "caff", 4) && cafInfo(head.data(), head.size(), fileSize, c, e2)) {
        out = DecodedAudio{};
        out.rate = c.rate; dataAt = c.dataAt; dataLen = c.dataLen; channels = c.channels; bits = c.bits; flt = c.flt; big = !c.little; known = true;
    } else if (head.size() >= 12 && !std::memcmp(head.data(), "RIFF", 4) && !std::memcmp(head.data() + 8, "WAVE", 4)) {
        out = DecodedAudio{};
        int format = 0;
        for (size_t p = 12; p + 8 <= head.size();) {
            const uint32_t len = le32(head.data() + p + 4);
            if (!std::memcmp(head.data() + p, "fmt ", 4) && p + 8 + 16 <= head.size()) {
                const uint8_t *body = head.data() + p + 8;
                format = le16(body); channels = le16(body + 2); out.rate = le32(body + 4); bits = le16(body + 14);
                if (format == 0xFFFE && p + 8 + 26 <= head.size()) format = le16(body + 24);
            } else if (!std::memcmp(head.data() + p, "data", 4)) { dataAt = p + 8; dataLen = std::min<size_t>(len, fileSize - dataAt); break; }
            p += 8 + (size_t)len + (len & 1);
        }
        known = dataAt && (format == 1 || format == 3);
        flt = format == 3;
    }
    if (known && channels > 0 && bits % 8 == 0) {
        frame = (size_t)channels * (bits / 8);
        const size_t frames = dataLen / frame, lo = std::min(a, frames), hi = std::min(b, frames);
        std::vector<uint8_t> bytes((hi - lo) * frame);
        in.clear();
        in.seekg((std::streamoff)(dataAt + lo * frame));
        in.read(reinterpret_cast<char *>(bytes.data()), (std::streamsize)bytes.size());
        return interleaved(bytes.data(), bytes.size(), channels, bits, flt, big, bits == 8, out, err);
    }
    // anything else: decode it all, keep the range
    in.clear();
    in.seekg(0);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!decodeAudio(bytes.data(), bytes.size(), out, err)) return false;
    const size_t lo = std::min(a, out.l.size()), hi = std::min(b, out.l.size());
    out.l = std::vector<float>(out.l.begin() + (long)lo, out.l.begin() + (long)hi);
    if (!out.r.empty()) out.r = std::vector<float>(out.r.begin() + (long)lo, out.r.begin() + (long)hi);
    out.loopStart = out.loopEnd = -1;
    return true;
}

bool isAudioFileName(const std::string &path) {
    std::string e = std::filesystem::path(path).extension().string();
    for (auto &c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".wav" || e == ".aif" || e == ".aiff" || e == ".aifc" || e == ".caf" || e == ".flac" || e == ".mp3" || e == ".ogg";
}

} // namespace wl
