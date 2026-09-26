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
    else if (size >= 4 && !std::memcmp(d, "fLaC", 4)) ok = codecs::flac(d, size, out, err);
    else if (size >= 4 && !std::memcmp(d, "OggS", 4)) {
        if (size >= 36 + 8 && !std::memcmp(d + 28, "OpusHead", 8)) { err = "Ogg Opus isn't supported (only Ogg Vorbis): convert it to FLAC or WAV"; return false; }
        ok = codecs::vorbis(d, size, out, err);
    }
    else if (isMp3(d, size)) ok = codecs::mp3(d, size, out, err);
    else { err = "not a WAV, AIFF, FLAC, MP3 or Ogg Vorbis file"; return false; }
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

bool isAudioFileName(const std::string &path) {
    std::string e = std::filesystem::path(path).extension().string();
    for (auto &c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".wav" || e == ".aif" || e == ".aiff" || e == ".aifc" || e == ".flac" || e == ".mp3" || e == ".ogg";
}

} // namespace wl
