#include "preset_formats.hpp"

#include <nlohmann/json.hpp>
#include <zstd.h>

#include <cstdio>
#include <cstring>
#include <set>

using json = nlohmann::json;

namespace wl {

namespace {

uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
void put32(std::vector<uint8_t> &o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (8 * i))); }
void put64(std::vector<uint8_t> &o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back((uint8_t)(v >> (8 * i))); }

// ---- MD5 (RFC 1321), for the hash Xfer writes into its headers ------------------------------
std::string md5hex(const std::vector<uint8_t> &msg) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
                              6, 10, 15, 21, 6, 10, 15, 21};
    std::vector<uint8_t> m = msg;
    const uint64_t bits = (uint64_t)msg.size() * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    put64(m, bits);
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) w[i] = le32(&m[off + i * 4]);
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
            const uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    char out[33];
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xff);
    return out;
}

// ---- Xfer container -------------------------------------------------------------------------
bool readXfer(const std::vector<uint8_t> &d, json &header, json &payload, std::string &err) {
    if (!isXferJson(d) || d.size() < 17) { err = "not an Xfer (Serum 2) file"; return false; }
    const uint64_t n = le64(&d[9]);
    if (17 + n + 8 > d.size()) { err = "truncated Xfer header"; return false; }
    header = json::parse(d.begin() + 17, d.begin() + 17 + (long)n, nullptr, false);
    if (header.is_discarded()) { err = "Xfer header is not JSON"; return false; }
    const size_t at = 17 + n;
    const uint32_t size = le32(&d[at]), version = le32(&d[at + 4]);
    if (version != 2) { err = "unsupported Xfer payload version " + std::to_string(version); return false; }
    std::vector<uint8_t> raw(size);
    const size_t got = ZSTD_decompress(raw.data(), raw.size(), d.data() + at + 8, d.size() - at - 8);
    if (ZSTD_isError(got) || got != size) { err = "Xfer payload does not decompress"; return false; }
    payload = json::from_cbor(raw, true, false);
    if (payload.is_discarded() || !payload.is_object()) { err = "Xfer payload is not a CBOR map"; return false; }
    return true;
}

std::vector<uint8_t> writeXfer(json header, const json &payload) {
    const std::vector<uint8_t> raw = json::to_cbor(payload);
    std::vector<uint8_t> z(ZSTD_compressBound(raw.size()));
    z.resize(ZSTD_compress(z.data(), z.size(), raw.data(), raw.size(), 3));
    header["hash"] = md5hex(z);
    const std::string h = header.dump();   // compact, keys sorted, like Serum's own
    std::vector<uint8_t> out = {'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0};
    put64(out, h.size());
    out.insert(out.end(), h.begin(), h.end());
    put32(out, (uint32_t)raw.size());
    put32(out, 2);
    out.insert(out.end(), z.begin(), z.end());
    return out;
}

// ---- JUCE ValueTree binary ------------------------------------------------------------------
struct Reader {
    const std::vector<uint8_t> &b;
    size_t i = 0;
    bool ok = true;
    uint8_t byte() { if (i >= b.size()) { ok = false; return 0; } return b[i++]; }
    int64_t cint() {   // MemoryOutputStream::writeCompressedInt
        const uint8_t head = byte();
        const int n = head & 0x7f;
        if (n > 8) { ok = false; return 0; }
        uint64_t v = 0;
        for (int k = 0; k < n; ++k) v |= (uint64_t)byte() << (8 * k);
        return (head & 0x80) ? -(int64_t)v : (int64_t)v;
    }
    std::string cstr() {
        std::string s;
        while (ok) { const uint8_t c = byte(); if (!c) break; s += (char)c; }
        return s;
    }
};

std::string xmlEscape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\n': o += "&#10;"; break;
        case '\r': o += "&#13;"; break;
        case '\t': o += "&#9;"; break;
        default: o += c;
        }
    }
    return o;
}

// var::readFromStream, as the text ValueTree::createXml would write for it
bool readVar(Reader &r, std::string &out, std::string &err) {
    const int64_t size = r.cint();
    if (size <= 0) { out.clear(); return r.ok; }
    const size_t start = r.i, end = start + (size_t)size;
    if (end > r.b.size()) { err = "truncated value"; return false; }
    const uint8_t type = r.byte();
    const uint8_t *p = r.b.data() + r.i;
    const size_t len = end - r.i;
    char buf[40];
    switch (type) {
    case 1: std::snprintf(buf, sizeof buf, "%d", (int32_t)le32(p)); out = buf; break;
    case 2: out = "1"; break;
    case 3: out = "0"; break;
    case 4: { double v; std::memcpy(&v, p, 8); std::snprintf(buf, sizeof buf, "%.17g", v); out = buf; break; }
    case 5: out.assign(reinterpret_cast<const char *>(p), len); while (!out.empty() && out.back() == 0) out.pop_back(); break;
    case 6: std::snprintf(buf, sizeof buf, "%lld", (long long)le64(p)); out = buf; break;
    case 9: out.clear(); break;
    default: err = "ValueTree value of type " + std::to_string(type) + " (array or binary) is not supported"; return false;
    }
    r.i = end;
    return true;
}

bool readTree(Reader &r, std::string &xml, int depth, std::string &err) {
    const std::string name = r.cstr();
    if (!r.ok || name.empty() || depth > 64) { err = "not a JUCE ValueTree"; return false; }
    xml += "<" + name;
    const int64_t props = r.cint();
    if (props < 0 || props > 100000) { err = "not a JUCE ValueTree"; return false; }
    for (int64_t p = 0; p < props; ++p) {
        const std::string key = r.cstr();
        std::string value;
        if (!readVar(r, value, err)) return false;
        xml += " " + key + "=\"" + xmlEscape(value) + "\"";
    }
    const int64_t kids = r.cint();
    if (!r.ok || kids < 0 || kids > 100000) { err = "not a JUCE ValueTree"; return false; }
    if (!kids) { xml += "/>"; return true; }
    xml += ">";
    for (int64_t k = 0; k < kids; ++k) if (!readTree(r, xml, depth + 1, err)) return false;
    xml += "</" + name + ">";
    return true;
}

} // namespace

bool isXferJson(const std::vector<uint8_t> &d) { return d.size() >= 9 && std::memcmp(d.data(), "XferJson\0", 9) == 0; }

bool serumPresetToStates(const std::vector<uint8_t> &file, std::vector<uint8_t> &processor, std::vector<uint8_t> &controller,
                         std::string &err) {
    json header, payload;
    if (!readXfer(file, header, payload, err)) return false;
    if (header.value("component", "") == "processor") { processor = file; return true; }   // already a state
    // keys the controller owns (display, browser, oscillator editors); these clip and rack keys
    // live in both halves
    static const std::set<std::string> controllerOnly = {"ClipPlayer", "Filter", "GranularOsc", "MultiSampleOsc", "Osc",
                                                         "SerumGUI", "SpectralOsc", "WTOsc", "arpBankDisplayName",
                                                         "clipBankDisplayName", "presetAuthor", "presetDescription", "presetName"};
    auto shared = [](const std::string &k) {
        for (const char *p : {"ArpClip", "MidiClip", "FXRack", "VoicePanel", "LFO"})
            if (k.rfind(p, 0) == 0 && k.find("PointModBus") == std::string::npos) return true;
        return false;
    };
    json proc = json::object(), ctl = json::object();
    for (auto &[k, v] : payload.items()) {
        if (k == "fileType") continue;
        if (!controllerOnly.count(k)) proc[k] = v;
        if (controllerOnly.count(k) || shared(k)) ctl[k] = v;
    }
    proc["component"] = "processor";
    ctl["component"] = "controller";
    ctl["presetHasBeenEdited"] = false;
    json meta = json::object();
    for (const char *k : {"product", "productVersion", "url", "vendor", "version"})
        if (header.contains(k)) { meta[k] = header[k]; proc[k] = header[k]; ctl[k] = header[k]; }
    json ph = meta, ch = meta;
    ph["component"] = "processor";
    ch["component"] = "controller";
    for (const char *k : {"presetName", "presetAuthor", "presetDescription"}) ch[k] = header.value(k, "");
    processor = writeXfer(ph, proc);
    controller = writeXfer(ch, ctl);
    return true;
}

bool valueTreeToJuceXml(const std::vector<uint8_t> &file, std::vector<uint8_t> &state, std::string &err) {
    Reader r{file};
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n";
    if (!readTree(r, xml, 0, err)) return false;
    xml += "\n";
    state = {'V', 'C', '2', '!'};
    put32(state, (uint32_t)xml.size() + 1);
    state.insert(state.end(), xml.begin(), xml.end());
    state.push_back(0);
    return true;
}

bool isDx7Cartridge(const std::vector<uint8_t> &d) {
    return d.size() == 4104 && d[0] == 0xF0 && d[1] == 0x43 && d[3] == 0x09 && d[4] == 0x20 && d[5] == 0x00;
}

namespace {
// JUCE MemoryBlock::toBase64Encoding: "<size>." + 6-bit little-endian groups in its own alphabet
const char *kJuceB64 = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
std::string juceB64(const std::vector<uint8_t> &d) {
    std::string s = std::to_string(d.size()) + ".";
    const size_t bits = d.size() * 8;
    for (size_t b = 0; b < bits; b += 6) {
        int v = 0;
        for (int k = 0; k < 6 && b + k < bits; ++k) if (d[(b + k) / 8] >> ((b + k) % 8) & 1) v |= 1 << k;
        s += kJuceB64[v];
    }
    return s;
}
std::vector<uint8_t> juceUnB64(const std::string &s) {
    const size_t dot = s.find('.');
    const size_t size = dot == std::string::npos ? 0 : (size_t)std::atol(s.substr(0, dot).c_str());
    std::vector<uint8_t> d(size, 0);
    size_t b = 0;
    for (size_t i = dot + 1; i < s.size(); ++i) {
        const char *p = std::strchr(kJuceB64, s[i]);
        const int v = p ? (int)(p - kJuceB64) : 0;
        for (int k = 0; k < 6; ++k, ++b) if (b / 8 < size && (v >> k & 1)) d[b / 8] |= (uint8_t)(1 << (b % 8));
    }
    return d;
}
// DX7 packed voice (128 bytes, bulk dump) -> VCED single-voice layout (155 bytes)
std::vector<uint8_t> unpackDx7Voice(const uint8_t *p) {
    std::vector<uint8_t> o;
    for (int op = 0; op < 6; ++op) {
        const uint8_t *q = p + op * 17;
        o.insert(o.end(), q, q + 11);                                  // EG rates/levels, break point, depths
        o.push_back(q[11] & 3); o.push_back((q[11] >> 2) & 3);         // curves
        o.push_back(q[12] & 7);                                        // rate scaling
        o.push_back(q[13] & 3); o.push_back((q[13] >> 2) & 7);         // amp mod / key velocity sensitivity
        o.push_back(q[14]);                                            // output level
        o.push_back(q[15] & 1); o.push_back((q[15] >> 1) & 31);        // osc mode, coarse
        o.push_back(q[16]);                                            // fine
        o.push_back((q[12] >> 3) & 15);                                // detune
    }
    o.insert(o.end(), p + 102, p + 110);                               // pitch EG
    o.push_back(p[110] & 31); o.push_back(p[111] & 7); o.push_back((p[111] >> 3) & 1);   // algorithm, feedback, key sync
    o.insert(o.end(), p + 112, p + 116);                               // LFO speed, delay, PMD, AMD
    o.push_back(p[116] & 1); o.push_back((p[116] >> 1) & 7); o.push_back((p[116] >> 4) & 7);   // LFO sync, wave, PMS
    o.push_back(p[117]);                                               // transpose
    o.insert(o.end(), p + 118, p + 128);                               // name
    return o;
}
bool setAttr(std::string &xml, const std::string &name, const std::string &value) {
    const std::string key = " " + name + "=\"";
    const size_t a = xml.find(key);
    if (a == std::string::npos) return false;
    const size_t v = a + key.size(), e = xml.find('"', v);
    xml.replace(v, e - v, value);
    return true;
}
} // namespace

bool dexedWithVoice(const std::vector<uint8_t> &dexedState, const std::vector<uint8_t> &cart, int voice,
                    std::vector<uint8_t> &out, std::string &err) {
    if (dexedState.size() < 9 || std::memcmp(dexedState.data(), "VC2!", 4) || voice < 0 || voice > 31) {
        err = "a DX7 cartridge loads into Dexed only (its state is JUCE XML); voices are numbered 0-31";
        return false;
    }
    std::string xml(dexedState.begin() + 8, dexedState.end());
    while (!xml.empty() && xml.back() == 0) xml.pop_back();
    if (xml.find("<dexedState") == std::string::npos) { err = "a DX7 cartridge loads into Dexed only"; return false; }
    // the edit buffer: 155-byte voice, then the operator on/off byte (all six on) and Dexed's own tail
    std::vector<uint8_t> program(161, 0);
    const size_t p = xml.find(" base64:program=\"");
    if (p != std::string::npos) {
        const size_t v = p + 17, e = xml.find('"', v);
        const auto old = juceUnB64(xml.substr(v, e - v));
        for (size_t i = 155; i < 161 && i < old.size(); ++i) program[i] = old[i];
    }
    const auto unpacked = unpackDx7Voice(cart.data() + 6 + voice * 128);
    std::copy(unpacked.begin(), unpacked.end(), program.begin());
    program[155] = 0x3F;
    // NamedValueSet::copyToXmlAttributes stores binary values as attributes named "base64:<name>"
    if (!setAttr(xml, "base64:sysex", juceB64(cart)) || !setAttr(xml, "base64:program", juceB64(program))) {
        err = "Dexed state has no cartridge / program blob";
        return false;
    }
    setAttr(xml, "currentProgram", std::to_string(voice));
    setAttr(xml, "opSwitch", "111111");
    out = {'V', 'C', '2', '!'};
    put32(out, (uint32_t)xml.size() + 1);
    out.insert(out.end(), xml.begin(), xml.end());
    out.push_back(0);
    return true;
}

bool looksLikeH2p(const std::vector<uint8_t> &d) {
    auto starts = [&](const char *s) { return d.size() >= std::strlen(s) && std::memcmp(d.data(), s, std::strlen(s)) == 0; };
    return starts("/*@Meta") || starts("#AM=");
}

std::vector<uint8_t> h2pToState(const std::vector<uint8_t> &text, const std::string &name) {
    std::vector<uint8_t> body;
    const std::string head = "#pgm=" + name + ".h2p\n";
    body.insert(body.end(), head.begin(), head.end());
    body.insert(body.end(), text.begin(), text.end());
    while (!body.empty() && body.back() == 0) body.pop_back();
    body.push_back(0);
    body.push_back(0);
    std::vector<uint8_t> state;
    put32(state, (uint32_t)body.size());
    state.insert(state.end(), body.begin(), body.end());
    return state;
}

} // namespace wl
