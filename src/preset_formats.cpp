#include "preset_formats.hpp"

#include "bytes.hpp"
#include "xml.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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
    if (d.size() < 25 || n > d.size() - 25) { err = "truncated Xfer header"; return false; }
    size_t hn = (size_t)n;
    while (hn > 0 && d[17 + hn - 1] == 0) --hn;   // older files count a trailing NUL
    header = json::parse(d.begin() + 17, d.begin() + 17 + (long)hn, nullptr, false);
    if (header.is_discarded()) { err = "Xfer header is not JSON"; return false; }
    const size_t at = 17 + n;
    const uint32_t size = le32(&d[at]), version = le32(&d[at + 4]);
    if (version != 2 && version != 0) { err = "unsupported Xfer payload version " + std::to_string(version); return false; }
    if (size > (256u << 20)) { err = "Xfer payload of " + std::to_string(size) + " bytes is larger than any preset"; return false; }
    std::vector<uint8_t> raw(size);
    if (version == 2) {   // zstd(CBOR)
        const size_t got = ZSTD_decompress(raw.data(), raw.size(), d.data() + at + 8, d.size() - at - 8);
        if (ZSTD_isError(got) || got != size) { err = "Xfer payload does not decompress"; return false; }
        payload = json::from_cbor(raw, true, false);
    } else {   // older Serum 2 builds: zlib(UBJSON)
        uLongf got = size;
        if (uncompress(raw.data(), &got, d.data() + at + 8, (uLong)(d.size() - at - 8)) != Z_OK || got != size) { err = "Xfer payload does not decompress"; return false; }
        payload = json::from_ubjson(raw, true, false);
    }
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
    const size_t need = type == 1 ? 4 : type == 4 || type == 6 ? 8 : 0;
    if (len < need) { err = "truncated value"; return false; }
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

// ---- HISE user presets (.preset): the XML of the interface's controls ------------------------------
namespace {
// a JUCE ValueTree as ValueTree::writeToStream writes it; property values keep their var stream bytes
struct VtVar { uint8_t type = 0; std::vector<uint8_t> data; };
struct VtNode { std::string type; std::vector<std::pair<std::string, VtVar>> props; std::vector<VtNode> kids; };
bool vtRead(Reader &r, VtNode &n, int depth) {
    if (depth > 64) return false;
    n.type = r.cstr();
    const int64_t np = r.cint();
    if (!r.ok || np < 0 || np > 100000) return false;
    for (int64_t k = 0; k < np; ++k) {
        std::string name = r.cstr();
        const int64_t len = r.cint();
        if (!r.ok || len < 1 || r.i + (size_t)len > r.b.size()) return false;
        VtVar v;
        v.type = r.b[r.i];
        v.data.assign(r.b.begin() + (long)r.i + 1, r.b.begin() + (long)(r.i + (size_t)len));
        r.i += (size_t)len;
        n.props.push_back({name, v});
    }
    const int64_t nk = r.cint();
    if (!r.ok || nk < 0 || nk > 1000000) return false;
    n.kids.resize((size_t)nk);
    for (auto &c : n.kids) if (!vtRead(r, c, depth + 1)) return false;
    return r.ok;
}
void vtCint(std::vector<uint8_t> &o, int64_t v) {   // MemoryOutputStream::writeCompressedInt
    uint64_t a = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
    uint8_t buf[8];
    int n = 0;
    while (a) { buf[n++] = (uint8_t)(a & 0xff); a >>= 8; }
    o.push_back((uint8_t)(n | (v < 0 ? 0x80 : 0)));
    o.insert(o.end(), buf, buf + n);
}
void vtCstr(std::vector<uint8_t> &o, const std::string &s) { o.insert(o.end(), s.begin(), s.end()); o.push_back(0); }
void vtWrite(const VtNode &n, std::vector<uint8_t> &o) {
    vtCstr(o, n.type);
    vtCint(o, (int64_t)n.props.size());
    for (auto &[k, v] : n.props) {
        vtCstr(o, k);
        vtCint(o, 1 + (int64_t)v.data.size());
        o.push_back(v.type);
        o.insert(o.end(), v.data.begin(), v.data.end());
    }
    vtCint(o, (int64_t)n.kids.size());
    for (auto &c : n.kids) vtWrite(c, o);
}
VtVar vtString(const std::string &s) { VtVar v; v.type = 5; v.data.assign(s.begin(), s.end()); v.data.push_back(0); return v; }
VtVar vtDouble(double d) { VtVar v; v.type = 4; v.data.resize(8); std::memcpy(v.data.data(), &d, 8); return v; }
VtVar vtBool(bool b) { VtVar v; v.type = b ? 2 : 3; return v; }
// a preset element as HISE keeps it in the plugin's state: control values are numbers, flags booleans
VtNode vtFromXml(const xml::Node &e) {
    VtNode n;
    n.type = e.tag;
    for (auto &[k, v] : e.attrs) {
        char *end = nullptr;
        const double d = std::strtod(v.c_str(), &end);
        const bool number = !v.empty() && end && *end == 0;
        if (k == "value" && number) n.props.push_back({k, vtDouble(d)});
        else if (k == "Enabled" && (v == "0" || v == "1")) n.props.push_back({k, vtBool(v == "1")});
        else n.props.push_back({k, vtString(v)});
    }
    for (auto &c : e.children) n.kids.push_back(vtFromXml(*c));
    return n;
}
} // namespace

bool isHisePreset(const std::vector<uint8_t> &d) {
    const std::string head(d.begin(), d.begin() + (long)std::min<size_t>(d.size(), 512));
    return head.find("<Preset") != std::string::npos && (head.find("<Content") != std::string::npos || head.find("<?xml") != std::string::npos);
}

bool hiseWithPreset(const std::vector<uint8_t> &current, const std::vector<uint8_t> &preset, const std::string &name,
                    std::vector<uint8_t> &out, std::string &err) {
    // a VST3 build keeps the VST 2 chunk in JUCE's "VstW" wrapper: header, FXB bank, chunk size at 172, chunk at 176
    const bool wrapped = current.size() >= 176 && !std::memcmp(current.data(), "VstW", 4) && !std::memcmp(current.data() + 16, "CcnK", 4);
    size_t at = 0, len = current.size();
    if (wrapped) {
        const uint8_t *q = current.data() + 172;
        at = 176;
        len = std::min<size_t>((size_t)q[0] << 24 | (size_t)q[1] << 16 | (size_t)q[2] << 8 | q[3], current.size() - 176);
    }
    const std::vector<uint8_t> chunk(current.begin() + (long)at, current.begin() + (long)(at + len));
    Reader r{chunk};
    VtNode root;
    if (!vtRead(r, root, 0) || root.type != "ControlData") { err = "a HISE user preset loads into HISE plugins only (the state has no ControlData)"; return false; }
    std::string perr;
    auto doc = xml::parse(std::string(preset.begin(), preset.end()), perr);
    if (!doc || doc->tag != "Preset") { err = "is not a HISE user preset" + (perr.empty() ? "" : " (" + perr + ")"); return false; }
    auto replaceChild = [](VtNode &parent, VtNode child) {
        for (auto &k : parent.kids) if (k.type == child.type) { k = std::move(child); return; }
        parent.kids.push_back(std::move(child));
    };
    for (auto &c : doc->children) {
        if (c->tag == "Content") {
            VtNode *ui = nullptr;
            for (auto &k : root.kids) if (k.type == "InterfaceData") ui = &k;
            if (!ui) { root.kids.push_back(VtNode{"InterfaceData", {}, {}}); ui = &root.kids.back(); }
            replaceChild(*ui, vtFromXml(*c));
        } else if (c->tag == "MidiAutomation" || c->tag == "MPEData") replaceChild(root, vtFromXml(*c));
    }
    bool named = false;
    for (auto &[k, v] : root.props) if (k == "UserPreset") { v = vtString(name); named = true; }
    if (!named) root.props.push_back({"UserPreset", vtString(name)});
    std::vector<uint8_t> body;
    vtWrite(root, body);
    if (!wrapped) { out = body; return true; }
    out.assign(current.begin(), current.begin() + 176);
    auto be = [&](size_t p, uint32_t v) { for (int i = 0; i < 4; ++i) out[p + (size_t)i] = (uint8_t)(v >> (8 * (3 - i))); };
    be(20, (uint32_t)(176 - 24 + body.size()));
    be(172, (uint32_t)body.size());
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

// ---- Surge XT Effects .srgfx: one effect with Surge's own (storage) parameter values ------------
namespace {
struct SurgeFxParam { int isInt; float min, max; };
struct SurgeFxType { int type; int remap[12]; SurgeFxParam p[12]; };
const SurgeFxType kSurgeFxTypes[] = {
#include "surge_fx_table.inc"
};
std::string xmlAttr(const std::string &v) {
    std::string o;
    for (char c : v) o += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : std::string(1, c);
    return o;
}
std::string num9(double v) { char b[40]; std::snprintf(b, sizeof b, "%.9g", v); return b; }
} // namespace

bool isSurgeFxPreset(const std::vector<uint8_t> &d) {
    const std::string head(d.begin(), d.begin() + (long)std::min<size_t>(d.size(), 256));
    return head.find("<single-fx") != std::string::npos;
}

bool surgeFxWithPreset(const std::vector<uint8_t> &current, const std::vector<uint8_t> &file, std::vector<uint8_t> &out, std::string &err) {
    // the plugin's state: JUCE binary XML ("VC2!" + u32 LE length + <surgefx .../> + NUL), then the wrapper's own data
    if (current.size() < 8 || std::memcmp(current.data(), "VC2!", 4)) { err = "an .srgfx preset loads into Surge XT Effects only"; return false; }
    const uint32_t n = le32(current.data() + 4);
    if (8 + (size_t)n > current.size()) { err = "unexpected Surge XT Effects state"; return false; }
    std::string text(current.begin() + 8, current.begin() + 8 + n);
    while (!text.empty() && text.back() == 0) text.pop_back();
    const std::vector<uint8_t> tail(current.begin() + 8 + n, current.end());
    std::string perr;
    auto state = xml::parse(text, perr);
    if (!state || state->tag != "surgefx") { err = "an .srgfx preset loads into Surge XT Effects only"; return false; }
    // a factory preset ("This Cassette") has a stray non-ASCII byte in place of the space between two
    // attributes: read it as the space
    std::string src;
    bool quoted = false;
    for (size_t k = 0; k < file.size(); ++k) {
        const char c = (char)file[k];
        if (c == '"') quoted = !quoted;
        if (!quoted && (uint8_t)c >= 0x80 && k > 0 && (file[k - 1] == '"' || (uint8_t)file[k - 1] >= 0x80)) {
            if (src.empty() || src.back() != ' ') src += ' ';
            continue;
        }
        src += c;
    }
    auto preset = xml::parse(src, perr);
    const xml::Node *snap = preset ? preset->child("snapshot") : nullptr;
    if (!snap) { err = "is not a Surge effect preset (" + (perr.empty() ? "no <snapshot>" : perr) + ")"; return false; }
    const int type = (int)snap->num("type", 0);
    const SurgeFxType *t = nullptr;
    for (const auto &x : kSurgeFxTypes) if (x.type == type) t = &x;
    if (!t) { err = "Surge effect type " + std::to_string(type) + " is unknown to Wavelength"; return false; }
    auto &attrs = state->attrs;
    auto set = [&](const std::string &k, const std::string &v) {
        for (auto &a : attrs) if (a.first == k) { a.second = v; return; }
        attrs.push_back({k, v});
    };
    auto has = [&](const std::string &k) { return state->attr(k) != nullptr; };
    set("fxt", std::to_string(type));
    for (int slot = 0; slot < 12; ++slot) {
        const int i = t->remap[slot];
        const SurgeFxParam &p = t->p[i];
        const std::string key = "p" + std::to_string(i);
        const bool given = snap->attr(key) != nullptr;
        const double v = snap->num(key, p.min);
        double norm = 0;
        if (p.max > p.min) {   // Surge: floats (v - min) / (max - min); integers 0.005 + 0.99 x that
            const double x = std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0);
            norm = p.isInt ? 0.005 + 0.99 * x : x;
        }
        int features = 0;   // SurgeFXProcessor::ParamFeatureFlags
        if (snap->get(key + "_temposync") == "1") features |= 1;
        if (snap->get(key + "_extend_range") == "1") features |= 2;
        if (snap->get(key + "_absolute") == "1") features |= 4;
        if (snap->get(key + "_deactivated") == "1") features |= 8;
        const std::string s = std::to_string(slot);
        set("fxp_" + s, num9(norm));
        set("fxp_param_features_" + s, std::to_string(features));
        if (has("fxp_temposync_" + s)) set("fxp_temposync_" + s, (features & 1) ? "1" : "0");
        if (has("surgevaltype_" + s)) {   // newer builds read integer values from these
            set("surgevaltype_" + s, p.isInt ? "0" : "2");
            set("surgeval_" + s, p.isInt ? std::to_string((long)std::lround(given ? v : p.min)) : num9(given ? v : p.min));
        }
    }
    if (has("currentPresetName")) set("currentPresetName", snap->get("name"));
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n<surgefx";
    for (auto &[k, v] : attrs) x += " " + k + "=\"" + xmlAttr(v) + "\"";
    x += "/>\n";
    out = {'V', 'C', '2', '!'};
    put32(out, (uint32_t)x.size() + 1);
    out.insert(out.end(), x.begin(), x.end());
    out.push_back(0);
    out.insert(out.end(), tail.begin(), tail.end());
    return true;
}

bool isKiloheartsPreset(const std::vector<uint8_t> &d) {
    return d.size() >= 16 && le32(d.data()) == 6 && le32(d.data() + 8) == 2;
}

std::vector<uint8_t> kiloheartsPresetToState(const std::vector<uint8_t> &p) {
    // preset: u32 6 (version), u32 build, u32 2 (a preset file), body. The state wraps the same body:
    // u32 1, u32 payload length; payload = u32 6, u32 build, u32 1 (a state), u32 0, u32 0, u8 0, body
    std::vector<uint8_t> payload;
    put32(payload, 6);
    put32(payload, le32(p.data() + 4));
    put32(payload, 1);
    put32(payload, 0);
    put32(payload, 0);
    payload.push_back(0);
    payload.insert(payload.end(), p.begin() + 12, p.end());
    std::vector<uint8_t> state;
    put32(state, 1);
    put32(state, (uint32_t)payload.size());
    state.insert(state.end(), payload.begin(), payload.end());
    return state;
}

bool isSerumFxFile(const std::vector<uint8_t> &d) {
    if (!isXferJson(d) || d.size() < 17) return false;
    const uint64_t n = le64(&d[9]);
    if (17 + n > d.size()) return false;
    size_t hn = (size_t)n;
    while (hn > 0 && d[17 + hn - 1] == 0) --hn;
    const json h = json::parse(d.begin() + 17, d.begin() + 17 + (long)hn, nullptr, false);
    const std::string t = h.is_object() ? h.value("fileType", "") : "";
    return t == "SerumFX" || t == "SerumFXRack";
}

bool serumFxWithFile(const std::vector<uint8_t> &current, const std::vector<uint8_t> &file, std::vector<uint8_t> &out, std::string &err) {
    json fh, fp, ch, cp;
    if (!readXfer(file, fh, fp, err)) return false;
    if (!readXfer(current, ch, cp, err)) { err = "a Serum FX file loads into Serum 2 FX only (" + err + ")"; return false; }
    if (ch.value("component", "") != "processor" || !cp.contains("FXRack0")) { err = "a Serum FX file loads into Serum 2 FX only"; return false; }
    const std::string type = fh.value("fileType", fp.value("fileType", ""));
    json rack;
    if (type == "SerumFXRack") {
        if (!fp.contains("FXRack") || !fp["FXRack"].is_object()) { err = "the rack file has no FXRack"; return false; }
        rack = fp["FXRack"];
    } else if (type == "SerumFX") {   // one module: the file's map without its metadata
        json entry = json::object();
        for (auto &[k, v] : fp.items())
            if (!std::set<std::string>{"fileType", "fxType", "product", "productVersion", "url", "vendor", "version", "hash"}.count(k)) entry[k] = v;
        rack = {{"FX", json::array({entry})}};
    } else { err = "is not a Serum FX or FX rack file"; return false; }
    // the file's modules replace the plugin's rack; the rack's own settings are kept unless the file has some
    json &target = cp["FXRack0"];
    if (!target.is_object()) target = json::object();
    target["FX"] = rack.value("FX", json::array());
    if (rack.contains("plainParams") && rack["plainParams"] != "default") target["plainParams"] = rack["plainParams"];
    if (rack.contains("parameters")) target["parameters"] = rack["parameters"];
    ch.erase("hash");
    out = writeXfer(ch, cp);
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

// ---- JUCE ValueTree binary, kept byte-exact (values are copied as raw var bytes) -------------
namespace {
struct VTree {
    std::string type;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> props;
    std::vector<VTree> kids;
    VTree *child(const std::string &t) { for (auto &k : kids) if (k.type == t) return &k; return nullptr; }
    void setProp(const std::string &k, const std::vector<uint8_t> &v) {
        for (auto &p : props) if (p.first == k) { p.second = v; return; }
        props.push_back({k, v});
    }
    void setProps(const VTree &src) { for (auto &p : src.props) setProp(p.first, p.second); }
    const std::vector<uint8_t> *prop(const std::string &k) const { for (auto &p : props) if (p.first == k) return &p.second; return nullptr; }
};

bool readVTree(Reader &r, VTree &t, int depth) {
    t.type = r.cstr();
    if (!r.ok || t.type.empty() || depth > 64) return false;
    const int64_t np = r.cint();
    if (np < 0 || np > 1000000) return false;
    for (int64_t i = 0; i < np; ++i) {
        std::string k = r.cstr();
        const int64_t n = r.cint();
        if (!r.ok || n < 0 || r.i + (size_t)n > r.b.size()) return false;
        t.props.push_back({k, std::vector<uint8_t>(r.b.begin() + (long)r.i, r.b.begin() + (long)(r.i + (size_t)n))});
        r.i += (size_t)n;
    }
    const int64_t nk = r.cint();
    if (!r.ok || nk < 0 || nk > 1000000) return false;
    t.kids.resize((size_t)nk);
    for (auto &k : t.kids) if (!readVTree(r, k, depth + 1)) return false;
    return true;
}
void writeCInt(std::vector<uint8_t> &o, int64_t v) {
    if (v == 0) { o.push_back(0); return; }
    const bool neg = v < 0;
    uint64_t u = neg ? (uint64_t)-v : (uint64_t)v;
    std::vector<uint8_t> b;
    while (u) { b.push_back((uint8_t)u); u >>= 8; }
    o.push_back((uint8_t)(b.size() | (neg ? 0x80 : 0)));
    o.insert(o.end(), b.begin(), b.end());
}
void writeVTree(std::vector<uint8_t> &o, const VTree &t) {
    o.insert(o.end(), t.type.begin(), t.type.end()); o.push_back(0);
    writeCInt(o, (int64_t)t.props.size());
    for (auto &[k, v] : t.props) { o.insert(o.end(), k.begin(), k.end()); o.push_back(0); writeCInt(o, (int64_t)v.size()); o.insert(o.end(), v.begin(), v.end()); }
    writeCInt(o, (int64_t)t.kids.size());
    for (auto &k : t.kids) writeVTree(o, k);
}
void putBe32At(std::vector<uint8_t> &o, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) o[at + (size_t)i] = (uint8_t)(v >> (8 * (3 - i))); }
} // namespace

bool isCherryPreset(const std::vector<uint8_t> &d) { return d.size() > 6 && std::memcmp(d.data(), "main\0", 5) == 0; }

bool cherryWithPreset(const std::vector<uint8_t> &st, const std::vector<uint8_t> &presetBytes, std::vector<uint8_t> &out, std::string &err) {
    VTree pre;
    Reader pr{presetBytes};
    if (!readVTree(pr, pre, 0) || pre.type != "main") { err = "not a Cherry Audio preset (JUCE ValueTree \"main\")"; return false; }
    const auto *name = pre.prop("presetName");
    if (st.size() > 0xb0 && std::memcmp(st.data(), "VstW", 4) == 0 && std::memcmp(st.data() + 16, "CcnK", 4) == 0) {
        // Voltage Modular: VstW header + FXB header, the JUCE chunk from 0xb0
        std::vector<uint8_t> chunk(st.begin() + 0xb0, st.end());
        Reader r{chunk};
        VTree vs;
        if (!readVTree(r, vs, 0) || !vs.child("presetInfo")) { err = "unexpected Voltage Modular state"; return false; }
        std::vector<uint8_t> tail(chunk.begin() + (long)r.i, chunk.end());
        VTree *pi = vs.child("presetInfo");
        pi->setProps(pre);
        if (name) pi->setProp("displayName", *name);
        pi->kids = pre.kids;
        std::vector<uint8_t> nc;
        writeVTree(nc, vs);
        nc.insert(nc.end(), tail.begin(), tail.end());
        out.assign(st.begin(), st.begin() + 0xb0);
        putBe32At(out, 20, (uint32_t)(out.size() - 24 + nc.size()));
        putBe32At(out, 0xac, (uint32_t)nc.size());
        out.insert(out.end(), nc.begin(), nc.end());
        return true;
    }
    // DCO-106 / MG-1 Plus / SEM: "savedState" (children curpreset, pt) + JUCE private-data tail
    Reader r{st};
    VTree ss;
    if (!readVTree(r, ss, 0) || !ss.child("pt")) { err = "a Cherry Audio preset loads into its Cherry Audio plugin only"; return false; }
    std::vector<uint8_t> tail(st.begin() + (long)r.i, st.end());
    VTree *pt = ss.child("pt");
    pt->setProps(pre);
    for (auto &c : pre.kids) {
        VTree *old = pt->child(c.type);
        if (c.type == "pd" && old) old->setProps(c);   // parameters a preset leaves out keep their init values
        else if (old) *old = c;                       // mpe, mappings
        else pt->kids.push_back(c);
    }
    if (VTree *cp = ss.child("curpreset"); cp && name) { cp->setProp("displayName", *name); cp->setProp("name", *name); }
    out.clear();
    writeVTree(out, ss);
    out.insert(out.end(), tail.begin(), tail.end());
    return true;
}

std::vector<std::string> guitarRigPaid(const std::string &xml) {
    // components of Guitar Rig's free edition (amps: Jump; cabs: Matched Cabinet (Pro); the basic
    // effects and modifiers); anything else is removed when the free licence loads the rack
    static const std::set<int> freeIds = {100, 2000, 4000, 6000, 7000, 8000, 9000, 12000, 14000, 15000, 18000, 19000, 24000, 25000,
                                          26000, 28000, 33000, 50000, 51000, 52000, 55000, 57000, 58000, 59000, 61000, 63000, 64000,
                                          66000, 67000, 68000, 69000, 70000, 82000, 83000, 88000, 89000, 90000, 98000, 99000, 101000,
                                          141000, 153000, 156000};
    std::set<std::string> names;
    for (size_t p = 0; (p = xml.find("<component id=\"", p)) != std::string::npos; ++p) {
        const int id = std::atoi(xml.c_str() + p + 15);
        const size_t nm = xml.find("name=\"", p), ne = nm == std::string::npos ? nm : xml.find('"', nm + 6);
        const std::string name = nm == std::string::npos ? "" : xml.substr(nm + 6, ne - nm - 6);
        if (!freeIds.count(id) && name != "Master FX") names.insert(name);   // Master FX becomes Global FX on load
    }
    return std::vector<std::string>(names.begin(), names.end());
}

bool guitarRigRackState(const std::vector<uint8_t> &rack, std::vector<uint8_t> &state,
                        std::vector<std::string> &paid, std::string &err) {
    // " LMX" + u32 1 + u32 length + XML; the rack is the block holding <gr-instrument-chunk
    std::vector<uint8_t> inst;
    for (size_t i = 0; i + 12 <= rack.size(); ++i) {
        if (std::memcmp(&rack[i], " LMX\x01\0\0\0", 8) != 0) continue;
        const uint32_t n = le32(&rack[i + 8]);
        if (i + 12 + n > rack.size()) break;
        const std::string head(rack.begin() + (long)i + 12, rack.begin() + (long)(i + 12 + std::min<uint32_t>(n, 300)));
        if (head.find("<gr-instrument-chunk") != std::string::npos) { inst.assign(rack.begin() + (long)i, rack.begin() + (long)(i + 12 + n)); break; }
    }
    if (inst.empty()) { err = "not a Guitar Rig rack preset (no <gr-instrument-chunk>)"; return false; }
    paid = guitarRigPaid(std::string(inst.begin() + 12, inst.end()));
    auto chunk = [&](const char *tag, const std::vector<uint8_t> &raw) {
        uLongf zlen = compressBound(raw.size());
        std::vector<uint8_t> z(zlen);
        compress2(z.data(), &zlen, raw.data(), raw.size(), 9);
        std::vector<uint8_t> c = {'a', 't', 'a', 'd'};
        put32(c, 2);
        c.insert(c.end(), tag, tag + 4);
        c.insert(c.end(), {'b', 'i', 'l', 'z'});
        put32(c, (uint32_t)zlen);
        put32(c, (uint32_t)raw.size());
        c.insert(c.end(), z.begin(), z.begin() + (long)zlen);
        return c;
    };
    std::vector<uint8_t> snd;
    put32(snd, 2);
    for (char c : std::string("-IN-R$iN")) snd.push_back((uint8_t)c);
    put32(snd, 0);
    const std::string info = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\" ?>\n<soundinfo version=\"400\">\n\n"
                             "  <properties/>\n\n  <components>\n    <component>Guitar Rig 4</component>\n  </components>\n\n"
                             "  <attributes>\n    <attribute>\n      <value>Effect</value>\n    </attribute>\n  </attributes>\n\n</soundinfo>\n";
    state = {'-', 'i', 'n', '-'};
    put32(state, 2);
    const std::string doc = "#NI#CS#Document##NI#SoundShell#Sound#";
    state.insert(state.end(), doc.begin(), doc.end());
    state.insert(state.end(), 11, 0);
    put32(state, 0x145);
    for (auto &c : {chunk("dnss", snd), chunk("ofni", std::vector<uint8_t>(info.begin(), info.end())), chunk("tsrp", inst), chunk("DNSS", snd)})
        state.insert(state.end(), c.begin(), c.end());
    return true;
}

namespace {
std::vector<uint8_t> juceXmlBlob(const std::string &xml) {
    std::vector<uint8_t> out = {'V', 'C', '2', '!'};
    put32(out, (uint32_t)xml.size() + 1);
    out.insert(out.end(), xml.begin(), xml.end());
    out.push_back(0);
    return out;
}
std::string luaQuote(const std::string &s) {
    std::string o;
    for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
    return o;
}
} // namespace

bool aasBank(const std::string &bankPath, std::string &bankId, std::string &bankName, std::vector<AasProgram> &programs) {
    std::ifstream in(bankPath);
    if (!in) return false;
    std::string line;
    AasProgram cur;
    auto field = [](const std::string &l, const char *key, std::string &v) {   // `    key = "value",`
        const std::string k = std::string("    ") + key + " = \"";
        if (l.rfind(k, 0) != 0 || l.size() < k.size() + 2 || l.compare(l.size() - 2, 2, "\",") != 0) return false;
        v = l.substr(k.size(), l.size() - k.size() - 2);
        return true;
    };
    while (std::getline(in, line)) {
        std::string v;
        if (bankId.empty() && line.rfind("  id = \"", 0) == 0) bankId = line.substr(8, line.find('"', 8) - 8);
        if (bankName.empty() && line.rfind("  name = \"", 0) == 0) bankName = line.substr(10, line.find('"', 10) - 10);
        if (field(line, "name", v)) cur.name = v;
        else if (field(line, "folder", v) || (cur.category.empty() && field(line, "category", v))) cur.category = v;
        else if (line == "  }," || line == "  {") {
            if (!cur.name.empty()) programs.push_back(cur);
            cur = AasProgram{};
        }
    }
    if (!cur.name.empty()) programs.push_back(cur);
    return !bankId.empty() && !programs.empty();
}

std::vector<uint8_t> aasProgramState(int index1, const std::string &name, const std::string &bankId, const std::string &bankName) {
    const std::string s = "{\n\tcurrentProgram = " + std::to_string(index1) + ",\n\tactiveBankId = \"" + bankId +
                          "\",\n\tcurrentProgramBankName = \"" + luaQuote(bankName) + "\",\n\tactiveBank = 1,\n\tpolyphony = 12,\n"
                          "\tcurrentProgramBankId = \"" + bankId + "\",\n\tversion = 1,\n\tcurrentProgramName = \"" + luaQuote(name) +
                          "\",\n\tcurrentProgramBank = 1,\n\tparameters = {\n\t\t0.99999994039536,\n\t},\n\tactiveBankName = \"" +
                          luaQuote(bankName) + "\",\n}";
    return std::vector<uint8_t>(s.begin(), s.end());
}

bool meldaPresets(const std::string &bankPath, std::vector<MeldaPreset> &out, std::string &err) {
    std::ifstream in(bankPath, std::ios::binary);
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 4) { err = "cannot read " + bankPath; return false; }
    struct Frame { std::string tag; std::map<std::string, std::string> attrs; std::vector<uint8_t> value; };
    std::vector<Frame> stack(1);
    auto cstr = [&](size_t &i) { std::string s; while (i < b.size() && b[i]) s += (char)b[i++]; ++i; return s; };
    for (size_t i = 0; i < b.size();) {
        const uint8_t c = b[i++];
        if (c == 0) continue;
        if (c == '~') break;
        if (c == 'M') { i += 2; continue; }
        if (c == 'X') { Frame f; f.tag = cstr(i); stack.push_back(std::move(f)); }
        else if (c == 'A') {
            const std::string n = cstr(i);
            if (i >= b.size()) break;
            const char t = (char)b[i++];
            if (t == 's') stack.back().attrs[n] = cstr(i);
            else if (t == 'u') {   // u32 LE byte length + UTF-8, no NUL
                if (i + 4 > b.size()) break;
                const uint32_t len = le32(&b[i]);
                if (i + 4 + len > b.size()) break;
                stack.back().attrs[n].assign(b.begin() + (long)i + 4, b.begin() + (long)(i + 4 + len));
                i += 4 + len;
            } else {
                const size_t w = t == '1' ? 1 : (t == '4' || t == 'f') ? 4 : 8;
                if (i + w > b.size()) break;
                if (t == '1') stack.back().attrs[n] = std::to_string(b[i]);
                else if (t == '4') stack.back().attrs[n] = std::to_string((int32_t)le32(&b[i]));
                i += w;
            }
        } else if (c == 'V') {
            if (i >= b.size()) break;
            if (b[i] == 's') {   // a NUL-terminated string: "$" + base64 (with '-' for '/') of the value
                ++i;
                const std::string text = cstr(i);
                std::vector<uint8_t> &v = stack.back().value;
                v.clear();
                if (!text.empty() && text[0] == '$') {
                    uint32_t acc = 0;
                    int bits = 0;
                    for (size_t k = 1; k < text.size(); ++k) {
                        const char ch = text[k];
                        const int d = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
                                    : ch == '+' ? 62 : ch == '-' || ch == '/' ? 63 : -1;
                        if (d < 0) continue;   // '=' padding
                        acc = (acc << 6) | (uint32_t)d;
                        if ((bits += 6) >= 8) { bits -= 8; v.push_back((uint8_t)(acc >> bits)); }
                    }
                } else v.assign(text.begin(), text.end());
                continue;
            }
            if (i + 5 > b.size()) break;
            const uint32_t n = le32(&b[i + 1]);
            if (i + 5 + n > b.size()) break;
            stack.back().value.assign(b.begin() + (long)i + 5, b.begin() + (long)(i + 5 + n));
            i += 5 + n;
        } else if (c == '/') {
            if (stack.size() < 2) break;
            Frame f = std::move(stack.back());
            stack.pop_back();
            if (f.tag != "preset") continue;
            MeldaPreset p;
            p.name = f.attrs["name"];
            for (auto &s : stack) if (s.tag == "Directory") p.category += (p.category.empty() ? "" : "/") + s.attrs["Name"];
            if (f.attrs["compressed"] == "1") {
                uLongf len = (uLongf)f.value.size() * 20 + 65536;
                std::vector<uint8_t> x;
                int rc;
                do { x.resize(len); rc = uncompress(x.data(), &len, f.value.data(), f.value.size()); if (rc == Z_BUF_ERROR) len *= 2; } while (rc == Z_BUF_ERROR);
                if (rc != Z_OK) continue;
                x.resize(len);
                p.state = std::move(x);
            } else p.state = f.value;
            out.push_back(std::move(p));
        } else { err = "unexpected MBXX data in " + bankPath; return !out.empty(); }
    }
    return !out.empty();
}

bool soundboxState(const std::vector<uint8_t> &sbset, std::vector<uint8_t> &state, std::string &err) {
    std::string x(sbset.begin(), sbset.end());
    const size_t a = x.find("<Audiomodern.Soundbox_Preset");
    if (a == std::string::npos) { err = "not a Soundbox preset (.sbset)"; return false; }
    x = x.substr(a);
    x.replace(0, 29, "<Audiomodern.Soundbox_State preset=\"\" ");
    for (size_t p; (p = x.find("</Audiomodern.Soundbox_Preset>")) != std::string::npos;) x.replace(p, 30, "</Audiomodern.Soundbox_State>");
    state = juceXmlBlob("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + x);
    return true;
}

bool decentSamplerState(const std::vector<uint8_t> &dspreset, const std::string &presetPath, std::vector<uint8_t> &state,
                        std::string &err) {
    std::string x(dspreset.begin(), dspreset.end());
    const size_t root = x.find("<DecentSampler");
    if (root == std::string::npos) { err = "not a DecentSampler preset (.dspreset)"; return false; }
    // samples are relative to the preset's folder: tell the plugin where that is
    const std::string dir = presetPath.substr(0, presetPath.find_last_of('/') + 1);
    std::string attr = " _samplePath=\"" + dir + "\"";
    x.insert(root + 14, attr);
    state = juceXmlBlob(x.rfind("<?xml", 0) == 0 ? x : "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + x);
    return true;
}

bool isSynplantPatch(const std::vector<uint8_t> &d) {
    return d.size() > 16 && std::memcmp(d.data(), "SynplantPatch: {", 16) == 0;
}

namespace {
uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void putBe32(std::vector<uint8_t> &o, uint32_t v) { for (int i = 3; i >= 0; --i) o.push_back((uint8_t)(v >> (8 * i))); }
} // namespace

namespace {
// Sonic Charge plugins (Synplant, Echobode, Permut8) share one state layout:
// 59 a2 cd 18, u8 0, u8 current program, u32 LE FXB length, FXB (big-endian "CcnK" ... "FBCh", fxID at
// +16, chunk size at +156, chunk at +160). The chunk is a header (";yMqmrAM" + the reversed fxID + a
// version, 16 bytes; Permut8: 4 magic bytes), u32 LE body length, zlib(body).
const uint8_t kScMagic[4] = {0x59, 0xa2, 0xcd, 0x18};
// The same chunk reaches us wrapped four ways: the VST3 state (magic, u8 0, u8 program, u32 LE length,
// FXB bank), a bare FXB bank (VST 2 bank files), an FXP program (an Audio Unit's "vstdata"), or the
// chunk itself (a VST 2 plugin's own chunk). The state is written back the way it came.
struct ScState {
    std::vector<uint8_t> prefix, head;   // bytes before the FXB/FXP; the FXB/FXP header up to its chunk size field
    bool vst3 = false, wrapped = false;  // VST3 state prefix; FXB/FXP around the chunk
    const uint8_t *chunk = nullptr;
    size_t header = 0;
    std::vector<uint8_t> body;
};
bool scUnpack(const std::vector<uint8_t> &st, const char *fxid, const std::string &plugin, const std::string &what, ScState &sc, std::string &err) {
    const std::string wrong = "a " + what + " loads into " + plugin + " only";
    size_t at = 0;
    if (st.size() >= 10 && !std::memcmp(st.data(), kScMagic, 4)) { sc.vst3 = true; at = 10; sc.prefix.assign(st.begin(), st.begin() + 10); }
    size_t chunkAt = at, csize = st.size() - at;
    if (st.size() >= at + 60 && !std::memcmp(st.data() + at, "CcnK", 4)) {
        const bool bank = !std::memcmp(st.data() + at + 8, "FBCh", 4), program = !std::memcmp(st.data() + at + 8, "FPCh", 4);
        if (!bank && !program) { err = wrong; return false; }
        if (std::memcmp(st.data() + at + 16, fxid, 4)) { err = wrong; return false; }
        const size_t sizeAt = at + (bank ? 156 : 56);
        if (st.size() < sizeAt + 4) { err = "unexpected " + plugin + " state layout"; return false; }
        sc.wrapped = true;
        sc.head.assign(st.begin() + (long)at, st.begin() + (long)sizeAt);
        csize = be32(st.data() + sizeAt);
        chunkAt = sizeAt + 4;
        if (chunkAt + csize > st.size()) { err = "unexpected " + plugin + " state layout"; return false; }
    } else if (sc.vst3) { err = wrong; return false; }
    sc.chunk = st.data() + chunkAt;
    if (csize < 8) { err = "unexpected " + plugin + " state layout"; return false; }
    if (!std::memcmp(sc.chunk + 4, "mrAM", 4)) {   // ";yMq" for a bank, other bytes for one program: then "mrAM", the fxID reversed
        sc.header = 16;
        const char rev[4] = {fxid[3], fxid[2], fxid[1], fxid[0]};
        if (csize < 20 || std::memcmp(sc.chunk + 8, rev, 4)) { err = wrong; return false; }
    } else if (!sc.wrapped && std::strcmp(fxid, "NuPr")) { err = wrong; return false; }   // a bare chunk must say whose it is
    else sc.header = 4;
    if (csize < sc.header + 4) { err = "unexpected " + plugin + " state layout"; return false; }
    uLongf blen = le32(sc.chunk + sc.header);
    sc.body.resize(blen);
    if (uncompress(sc.body.data(), &blen, sc.chunk + sc.header + 4, csize - sc.header - 4) != Z_OK || blen != sc.body.size()) {
        err = plugin + " state does not decompress"; return false;
    }
    return true;
}
bool scPack(const ScState &sc, const std::vector<uint8_t> &body, int current, std::vector<uint8_t> &out, std::string &err) {
    uLongf zlen = compressBound(body.size());
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, body.data(), body.size(), 6) != Z_OK) { err = "zlib failed"; return false; }
    z.resize(zlen);
    std::vector<uint8_t> newChunk(sc.chunk, sc.chunk + sc.header);
    put32(newChunk, (uint32_t)body.size());
    newChunk.insert(newChunk.end(), z.begin(), z.end());
    if (!sc.wrapped) { out = newChunk; return true; }
    std::vector<uint8_t> rest(sc.head.begin() + 8, sc.head.end());   // from "FBCh"/"FPCh" to the chunk size
    putBe32(rest, (uint32_t)newChunk.size());
    rest.insert(rest.end(), newChunk.begin(), newChunk.end());
    std::vector<uint8_t> fx = {'C', 'c', 'n', 'K'};
    putBe32(fx, (uint32_t)rest.size());
    fx.insert(fx.end(), rest.begin(), rest.end());
    if (!sc.vst3) { out = fx; return true; }
    out = {kScMagic[0], kScMagic[1], kScMagic[2], kScMagic[3], 0, (uint8_t)current};
    put32(out, (uint32_t)fx.size());
    out.insert(out.end(), fx.begin(), fx.end());
    return true;
}
// a text patch (Synplant, Echobode) in program slot 0, named: its keys are sorted, so a missing
// name line goes before the first top-level key that sorts after "name"
bool scWithPatch(const std::vector<uint8_t> &st, const std::vector<uint8_t> &patch, const std::string &name, const char *fxid,
                 const std::string &plugin, const std::string &what, std::vector<uint8_t> &out, std::string &err) {
    ScState sc;
    if (!scUnpack(st, fxid, plugin, what, sc, err)) return false;
    const auto &body = sc.body;
    // one program (an FXP, an Audio Unit's state): u32 LE length + the patch text, no list
    const bool single = body.size() >= 8 && le32(body.data()) == body.size() - 4 && std::isalpha(body[4]);
    const uint32_t count = single ? 0 : body.size() >= 4 ? le32(body.data()) : 0;
    size_t p = 4;
    std::vector<std::vector<uint8_t>> patches;
    for (uint32_t i = 0; i < count && p + 4 <= body.size(); ++i) {
        const uint32_t n = le32(&body[p]);
        if (p + 4 + n > body.size()) break;
        patches.emplace_back(body.begin() + (long)p + 4, body.begin() + (long)(p + 4 + n));
        p += 4 + n;
    }
    if (!single && patches.empty()) { err = plugin + " state has no programs"; return false; }
    const std::vector<uint8_t> trailer = single ? std::vector<uint8_t>() : std::vector<uint8_t>(body.begin() + (long)p, body.end());
    if (single) patches.push_back({});
    std::string text(patch.begin(), patch.end()), esc;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    for (char c : name) { if (c == '\\' || c == '"') esc += '\\'; esc += c; }
    const size_t nm = text.find("\n\tname: \"");
    if (nm != std::string::npos) text.replace(nm + 1, text.find('\n', nm + 1) - nm - 1, "\tname: \"" + esc + "\"");
    else
        for (size_t at = text.find("\n\t"); at != std::string::npos; at = text.find("\n\t", at + 1)) {
            if (text[at + 2] == '\t') continue;   // nested key
            if (text.compare(at + 2, text.find(':', at) - at - 2, "name") > 0) { text.insert(at + 1, "\tname: \"" + esc + "\"\n"); break; }
        }
    if (text.empty() || text.back() != '\n') text += '\n';
    patches[0].assign(text.begin(), text.end());
    std::vector<uint8_t> nb;
    if (!single) put32(nb, (uint32_t)patches.size());
    for (auto &t : patches) { put32(nb, (uint32_t)t.size()); nb.insert(nb.end(), t.begin(), t.end()); }
    nb.insert(nb.end(), trailer.begin(), trailer.end());
    return scPack(sc, nb, 0, out, err);
}
} // namespace

bool synplantWithPatch(const std::vector<uint8_t> &st, const std::vector<uint8_t> &patch, const std::string &name,
                       std::vector<uint8_t> &out, std::string &err) {
    return scWithPatch(st, patch, name, "NuSP", "Synplant", ".synplant patch", out, err);
}

bool isEchobodePatch(const std::vector<uint8_t> &d) {
    return d.size() > 16 && std::memcmp(d.data(), "EchobodePatch: {", 16) == 0;
}

bool echobodeWithPatch(const std::vector<uint8_t> &st, const std::vector<uint8_t> &patch, const std::string &name,
                       std::vector<uint8_t> &out, std::string &err) {
    return scWithPatch(st, patch, name, "NuEB", "Echobode", ".echobode patch", out, err);
}

// ---- Permut8 banks (.p8bank): 30 programs A0..C9 of display values ------------------------------
namespace {
struct P8Program { std::string slot, name; bool modified = false; std::map<std::string, std::string> fields; };
bool parseP8Bank(const std::vector<uint8_t> &d, std::string &current, std::vector<P8Program> &progs) {
    std::string text(d.begin(), d.end());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.rfind("Permut8BankV1: {", 0) != 0) return false;
    std::istringstream in(text);
    std::string line;
    P8Program *cur = nullptr;
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
        return s;
    };
    while (std::getline(in, line)) {
        if (line.rfind("\tCurrent Program: ", 0) == 0) current = trim(line.substr(18));
        else if (line.size() == 7 && line.rfind("\t\t", 0) == 0 && line[2] >= 'A' && line[2] <= 'C' && std::isdigit((unsigned char)line[3]) && line.compare(4, 3, ": {") == 0) {
            progs.push_back({});
            cur = &progs.back();
            cur->slot = line.substr(2, 2);
        } else if (cur && line.rfind("\t\t}", 0) == 0) cur = nullptr;
        else if (cur && line.rfind("\t\t\t", 0) == 0) {
            const size_t c = line.find(": ");
            if (c == std::string::npos) continue;
            const std::string k = trim(line.substr(0, c)), v = trim(line.substr(c + 2));
            if (k == "Name") {
                std::string n = v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v, u;
                for (size_t i = 0; i < n.size(); ++i) { if (n[i] == '\\' && i + 1 < n.size()) ++i; u += n[i]; }
                cur->name = u;
            } else if (k == "Modified") cur->modified = v == "true";
            else cur->fields[k] = v;
        }
    }
    return progs.size() == 30;
}
int p8Slot(const std::string &s) { return (s[0] - 'A') * 10 + (s[1] - '0'); }
double p8Num(const std::string &s) { return std::strtod(s.c_str() + s.find_first_of("+-.0123456789"), nullptr); }
// display text -> normalized value, the inverse of Permut8's own parameter display
bool p8Normalize(const std::string &key, const std::string &t, float &v) {
    static const std::map<std::string, std::vector<std::string>> enums = {
        {"FilterPlacement", {"Off", "Input", "Feedback", "Output"}}, {"SyncMode", {"Off", "Dotted", "Triplets", "Standard"}},
        {"Operator1", {"NOP", "AND", "MUL", "OSC", "RND"}}, {"Operator2", {"NOP", "OR", "XOR", "MSK", "SUB"}}};
    auto clamp = [](double x) { return (float)std::max(0.0, std::min(1.0, x)); };
    const double ln600 = std::log(600.0);
    if (key == "Limiter" || key == "FeedbackFlip" || key == "FeedbackInvert" || key == "Reverse") { v = t == "On"; return true; }
    if (auto e = enums.find(key); e != enums.end()) {
        auto it = std::find(e->second.begin(), e->second.end(), t);
        if (it == e->second.end()) return false;
        v = (float)(it - e->second.begin()) / (float)(e->second.size() - 1);
        return true;
    }
    if (key == "InputLevel" || key == "OutputLevel") {   // dB = 54 * sign(2x-1) * (2x-1)^2
        const double db = std::max(-54.0, std::min(54.0, p8Num(t)));
        v = (float)(0.5 + 0.5 * std::copysign(std::sqrt(std::fabs(db) / 54.0), db));
        return true;
    }
    if (key == "FilterFreq") {   // LP 40 Hz..(24 kHz) | "---" | HP (20 Hz)..12 kHz
        if (t.rfind("---", 0) == 0) { v = 0.5f; return true; }
        const double f = std::max(p8Num(t.substr(2)), 1e-9);
        if (t.rfind("LP", 0) == 0) { v = clamp(0.5 * std::log(f / 40.0) / ln600); return true; }
        if (t.rfind("HP", 0) == 0) { v = clamp(0.5 + 0.5 * std::log(f / 20.0) / ln600); return true; }
        return false;
    }
    if (key == "FeedbackAmount") { v = clamp(p8Num(t) / 110.0); return true; }
    if (key == "Mix") { v = clamp(p8Num(t) / 100.0); return true; }
    if (key == "ClockFreq") {
        if (t.size() > 3 && t.compare(t.size() - 3, 3, "kHz") == 0) {   // sync off: 0..44.1 kHz linear, then 3 octaves to 352.8
            const double f = p8Num(t);
            v = f <= 44.1 ? clamp(f / 88.2) : clamp(0.5 + 0.5 * std::log(f / 44.1) / std::log(8.0));
            return true;
        }
        std::string base = t;
        while (!base.empty() && (base.back() == 'D' || base.back() == 'T')) base.pop_back();   // 1/2D, 1/8T: from SyncMode
        static const std::vector<std::string> steps = {"8/1", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8"};
        auto it = std::find(steps.begin(), steps.end(), base);
        if (it == steps.end()) return false;
        v = (float)(it - steps.begin()) / 6.0f;
        return true;
    }
    if (key.rfind("Operand", 0) == 0) { v = (float)std::strtol(t.c_str(), nullptr, 16) / 255.0f; return true; }
    return false;
}
const char *const kP8Keys[18] = {"InputLevel", "Limiter", "FilterFreq", "FilterPlacement", "FeedbackAmount", "FeedbackFlip",
                                 "FeedbackInvert", "OutputLevel", "Mix", "ClockFreq", "SyncMode", "Reverse", "Operator1",
                                 "Operand1.High", "Operand1.Low", "Operator2", "Operand2.High", "Operand2.Low"};
} // namespace

bool isPermut8Bank(const std::vector<uint8_t> &d) {
    return d.size() > 16 && std::memcmp(d.data(), "Permut8BankV1: {", 16) == 0;
}

std::vector<std::string> permut8BankPrograms(const std::vector<uint8_t> &bank) {
    std::string current;
    std::vector<P8Program> progs;
    std::vector<std::string> names(30);
    if (!parseP8Bank(bank, current, progs)) return {};
    for (auto &p : progs) names[p8Slot(p.slot)] = p.name.empty() ? p.slot : p.name;
    return names;
}

bool permut8WithBank(const std::vector<uint8_t> &st, const std::vector<uint8_t> &bank, int program, std::vector<uint8_t> &out, std::string &err) {
    ScState sc;
    if (!scUnpack(st, "NuPr", "Permut8", ".p8bank", sc, err)) return false;
    std::string current;
    std::vector<P8Program> progs;
    if (!parseP8Bank(bank, current, progs)) { err = "is not a Permut8 bank with 30 programs"; return false; }
    const auto &body = sc.body;   // u32 0 + 3 floats (write protect, reset, MIDI control), u32 count, programs, trailer
    if (body.size() < 20) { err = "unexpected Permut8 state layout"; return false; }
    size_t p = 20;
    for (uint32_t i = 0, n = le32(body.data() + 16); i < n; ++i) {
        if (p + 8 > body.size()) { err = "unexpected Permut8 state layout"; return false; }
        p += 8 + le32(body.data() + p + 4) + 1 + 72;
    }
    if (p > body.size()) { err = "unexpected Permut8 state layout"; return false; }
    std::vector<uint8_t> nb(body.begin(), body.begin() + 16);
    put32(nb, 30);
    std::sort(progs.begin(), progs.end(), [](const P8Program &a, const P8Program &b) { return p8Slot(a.slot) < p8Slot(b.slot); });
    for (auto &pr : progs) {
        std::string name = pr.name.substr(0, 24);   // Permut8 rejects the whole state for a name over 24 bytes
        while (!name.empty() && ((uint8_t)name.back() & 0xC0) == 0x80) name.pop_back();   // don't cut a UTF-8 sequence
        if (!name.empty() && (uint8_t)name.back() >= 0xC0) name.pop_back();
        put32(nb, 0x39685ab6);
        put32(nb, (uint32_t)name.size());
        nb.insert(nb.end(), name.begin(), name.end());
        nb.push_back(pr.modified ? 1 : 0);
        for (const char *k : kP8Keys) {
            float v = 0;
            auto f = pr.fields.find(k);
            if (f == pr.fields.end() || !p8Normalize(k, f->second, v)) { err = "program " + pr.slot + ": cannot read " + k; return false; }
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            put32(nb, bits);
        }
    }
    nb.insert(nb.end(), body.begin() + (long)p, body.end());
    const int sel = program >= 0 ? program : (current.size() == 2 ? p8Slot(current) : 0);
    return scPack(sc, nb, std::max(0, std::min(29, sel)), out, err);
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

std::vector<uint8_t> h2pToLegacyState(const std::vector<uint8_t> &text, const std::string &name) {
    const std::string head = "#pgm=" + name + "\n";
    std::vector<uint8_t> state(head.begin(), head.end());
    state.insert(state.end(), text.begin(), text.end());
    while (!state.empty() && state.back() == 0) state.pop_back();
    state.push_back(0);
    state.push_back(0);
    return state;
}

namespace {
// the root element of an OB-Xd bank: <discoDSP>, or <Datsounds> in banks saved by the original 2DaT Obxd
size_t obxdRoot(const std::string &xml) {
    const size_t a = xml.find("<discoDSP"), b = xml.find("<Datsounds");
    return std::min(a, b);
}
// the JUCE XML document inside an OB-Xd bank (.fxb "FBCh": chunk size at 156, "VC2!" + u32 LE length + XML at 160)
bool obxdBankXml(const std::vector<uint8_t> &fxb, std::string &xml) {
    if (fxb.size() < 168 || std::memcmp(fxb.data(), "CcnK", 4) || std::memcmp(fxb.data() + 8, "FBCh", 4) ||
        std::memcmp(fxb.data() + 160, "VC2!", 4)) return false;
    const uint32_t n = le32(fxb.data() + 164);
    if (168 + (size_t)n > fxb.size()) return false;
    xml.assign(fxb.begin() + 168, fxb.begin() + 168 + n);
    while (!xml.empty() && xml.back() == 0) xml.pop_back();
    return obxdRoot(xml) != std::string::npos;
}
std::string xmlUnescape(std::string s) {
    for (const auto &[from, to] : std::vector<std::pair<std::string, std::string>>{{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}})
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}
} // namespace

std::vector<std::string> obxdBankPrograms(const std::vector<uint8_t> &fxb) {
    std::vector<std::string> names;
    std::string xml;
    if (!obxdBankXml(fxb, xml)) return names;
    for (size_t at = 0; (at = xml.find("<program ", at)) != std::string::npos; ++at) {
        const size_t end = xml.find('>', at);
        const size_t pn = xml.find("programName=\"", at);
        std::string name;
        if (pn != std::string::npos && pn < end) name = xmlUnescape(xml.substr(pn + 13, xml.find('"', pn + 13) - pn - 13));
        while (!name.empty() && name.back() == ' ') name.pop_back();
        names.push_back(name.empty() ? "Program " + std::to_string(names.size() + 1) : name);
    }
    return names;
}

bool obxdBankState(const std::vector<uint8_t> &fxb, int program, std::vector<uint8_t> &state, std::string &err) {
    std::string xml;
    if (!obxdBankXml(fxb, xml)) { err = "is not an OB-Xd bank"; return false; }
    const size_t root = obxdRoot(xml);
    const size_t cp = xml.find("currentProgram=\"", root);
    if (cp == std::string::npos || cp > xml.find('>', root)) { err = "OB-Xd bank without a current program"; return false; }
    const size_t v = cp + 16;
    xml.replace(v, xml.find('"', v) - v, std::to_string(program));
    state = juceXmlBlob(xml);
    return true;
}

bool isFullBucketBank(const std::vector<uint8_t> &fxb) {
    return fxb.size() >= 172 && std::memcmp(fxb.data(), "CcnK", 4) == 0 && std::memcmp(fxb.data() + 8, "FBCh", 4) == 0 &&
           std::memcmp(fxb.data() + 160, "tffp", 4) == 0;
}

std::vector<std::pair<std::string, std::vector<uint8_t>>> fullBucketBankPrograms(const std::vector<uint8_t> &fxb) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    if (!isFullBucketBank(fxb)) return out;
    const size_t end = std::min(fxb.size(), 160 + (size_t)be32(fxb.data() + 156));
    // an entry: u32 LE name length + name + u8 1, then the program
    auto entry = [&](size_t at, std::string &name, size_t &program) {
        if (at + 4 > end) return false;
        const uint32_t n = le32(fxb.data() + at);
        if (n > 256 || at + 4 + n + 1 > end || fxb[at + 4 + n] != 1) return false;
        name.assign(fxb.begin() + (long)at + 4, fxb.begin() + (long)(at + 4 + n));
        program = at + 4 + n + 1;
        return true;
    };
    std::string name;
    size_t program = 0;
    if (!entry(168, name, program)) return out;
    size_t v = program;
    while (v < end && fxb[v] >= 0x20 && fxb[v] < 0x7f) ++v;
    const std::string version(fxb.begin() + (long)program, fxb.begin() + (long)v);   // "FB-02_1.1.0"
    if (version.size() < 3 || version.find('_') == std::string::npos) return out;
    for (;;) {
        // programs have no length: one ends where the entry of the next copy of the version string begins
        size_t next = end, nextProgram = 0;
        std::string nextName;
        for (size_t s = program + version.size(); next == end && s + version.size() <= end; ++s) {
            if (std::memcmp(fxb.data() + s, version.data(), version.size()) != 0) continue;
            for (size_t n = 0; n <= 64 && s >= program + n + 5; ++n) {
                std::string nm;
                size_t p = 0;
                if (entry(s - 1 - n - 4, nm, p) && p == s) { next = s - 1 - n - 4; nextProgram = s; nextName = nm; break; }
            }
        }
        const size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
        out.push_back({a == std::string::npos ? "" : name.substr(a, b - a + 1), std::vector<uint8_t>(fxb.begin() + (long)program, fxb.begin() + (long)next)});
        if (next == end) break;
        program = nextProgram;
        name = nextName;
    }
    return out;
}

bool fullBucketBankState(const std::vector<uint8_t> &fxb, int program, std::vector<uint8_t> &state, std::string &err) {
    const auto programs = fullBucketBankPrograms(fxb);
    if (programs.empty()) { err = "is not a Full Bucket bank"; return false; }
    if (program < 0 || program >= (int)programs.size()) { err = "has no program " + std::to_string(program) + " (0-" + std::to_string(programs.size() - 1) + ")"; return false; }
    state = programs[(size_t)program].second;
    return true;
}

bool fireflyWithPreset(const std::vector<uint8_t> &fireflyState, const std::vector<uint8_t> &preset, const std::string &name,
                       std::vector<uint8_t> &out, std::string &err) {
    std::string cur(fireflyState.begin(), fireflyState.end());
    while (!cur.empty() && cur.back() == 0) cur.pop_back();
    json state = json::parse(cur, nullptr, false), p = json::parse(preset.begin(), preset.end(), nullptr, false);
    // {"paramNameOverrides", "patchState": the patch}, or (older presets) the patch itself
    const json patch = p.is_object() && p.contains("patchState") ? p["patchState"] : p;
    if (!patch.is_object() || !patch.contains("magic") || !patch.contains("state")) { err = "is not a Firefly Synth 2 preset"; return false; }
    if (!state.is_object() || !state.contains("edit") || !state["edit"].is_object() ||
        state["edit"].value("magic", "") != patch.value("magic", "")) {
        err = "a .ff2preset loads into Firefly Synth 2 only";
        return false;
    }
    state["edit"] = patch;
    if (state.contains("gui") && state["gui"].is_object()) {
        state["gui"]["patchName"] = name;
        if (p.contains("paramNameOverrides")) state["gui"]["paramNameOverrides"] = p["paramNameOverrides"];
    }
    const std::string text = state.dump(2);
    out.assign(text.begin(), text.end());
    return true;
}

std::vector<uint8_t> juceXmlState(const std::vector<uint8_t> &xml) {
    std::string x(xml.begin(), xml.end());
    while (!x.empty() && (x.back() == 0 || std::isspace((unsigned char)x.back()))) x.pop_back();
    // Vaporizer2 reads its preset files as display values whatever their version, but a V2.00000 state as
    // internal values (and ignores it); V2.10000 is the same patch generation read as display values
    const std::string v200 = "PatchVersion=\"VASTVaporizerParamsV2.00000\"";
    const size_t root = x.find("<VASTvaporizer2 "), v = x.find(v200);
    if (root != std::string::npos && v != std::string::npos && v < x.find('>', root)) x.replace(v + 36, 1, "1");
    return juceXmlBlob(x);
}

// --- Native Instruments containers: an item is u64 size, u32 1, "hsin", u64, uuid[16], then a stack of
// frames (u64 size, domain fourcc, u32 type, u32 version; innermost the base item frame, type 1), each
// frame's data after the frame it wraps, then u32 1, u32 child count and the children (u32 0, domain,
// u32 type, item).

bool isNiContainer(const std::vector<uint8_t> &d) { return d.size() >= 40 && std::memcmp(d.data() + 12, "hsin", 4) == 0; }

namespace {

std::string urlOfPath(const std::string &path) {
    static const char *hex = "0123456789ABCDEF";
    std::string u = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') u += (char)c;
        else { u += '%'; u += hex[c >> 4]; u += hex[c & 15]; }
    }
    return u;
}

struct NiFile {
    std::ifstream in;
    uint64_t size = 0;
    bool read(uint64_t at, void *out, size_t n) {
        if (at + n > size) return false;
        in.seekg((std::streamoff)at);
        return (bool)in.read(static_cast<char *>(out), (std::streamsize)n);
    }
    bool u32(uint64_t at, uint32_t &v) { uint8_t b[4]; if (!read(at, b, 4)) return false; v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; return true; }
    bool u64(uint64_t at, uint64_t &v) { uint32_t lo, hi; if (!u32(at, lo) || !u32(at + 4, hi)) return false; v = lo | (uint64_t)hi << 32; return true; }
};

struct NiFrame { std::string domain; uint32_t type = 0; uint64_t dataAt = 0, dataSize = 0; };

// an item's frames (outermost first) and where its children start; false if it is not an item
bool niItem(NiFile &f, uint64_t at, std::vector<NiFrame> &frames, uint64_t &children, uint64_t &end) {
    uint64_t size;
    char magic[4];
    if (!f.u64(at, size) || !f.read(at + 12, magic, 4) || std::memcmp(magic, "hsin", 4) != 0 || at + size > f.size) return false;
    end = at + size;
    frames.clear();
    std::vector<uint64_t> starts, sizes;
    for (uint64_t o = at + 40;;) {
        uint64_t fs;
        char dom[4];
        uint32_t type;
        if (!f.u64(o, fs) || !f.read(o + 8, dom, 4) || !f.u32(o + 12, type) || fs < 20 || o + fs > end || frames.size() > 32) return false;
        NiFrame fr;
        fr.domain.assign(dom, 4);
        std::reverse(fr.domain.begin(), fr.domain.end());
        fr.type = type;
        frames.push_back(fr);
        starts.push_back(o);
        sizes.push_back(fs);
        if (type == 1 && fr.domain == "NISD") break;
        o += 20;
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        const uint64_t inner = i + 1 < frames.size() ? starts[i + 1] + sizes[i + 1] : starts[i] + 20;
        frames[i].dataAt = inner;
        frames[i].dataSize = starts[i] + sizes[i] - inner;
    }
    children = starts[0] + sizes[0];
    return true;
}

// a UTF-16 string stored as u32 length + code units (ASCII kept, the rest as '?')
std::string niString(NiFile &f, uint64_t at) {
    uint32_t n;
    if (!f.u32(at, n) || n > 1024) return "";
    std::vector<uint8_t> b(n * 2);
    if (!f.read(at + 4, b.data(), b.size())) return "";
    std::string s;
    for (size_t i = 0; i + 1 < b.size(); i += 2) { const uint16_t c = (uint16_t)(b[i] | b[i + 1] << 8); s += c < 128 ? (char)c : '?'; }
    return s;
}

void niWalk(NiFile &f, uint64_t at, int depth, std::string bank, std::vector<std::string> &out) {
    std::vector<NiFrame> frames;
    uint64_t children, end;
    if (depth > 12 || !niItem(f, at, frames, children, end)) return;
    for (const auto &fr : frames) {
        if (fr.domain != "NISD") continue;
        if (fr.type == 100 && fr.dataSize >= 8) bank = niString(f, fr.dataAt + 4);                 // a snapshot bank's name
        if (fr.type == 108 && fr.dataSize >= 24) {                                                   // a preset header: its name
            const std::string name = niString(f, fr.dataAt + 16);
            if (!name.empty()) out.push_back(bank.empty() ? name : bank + "/" + name);
            return;
        }
        if (fr.type == 117) return;   // the ensemble itself (megabytes, no snapshots in it)
    }
    uint32_t one, n;
    if (!f.u32(children, one) || !f.u32(children + 4, n) || n > 4096) return;
    uint64_t o = children + 8;
    for (uint32_t i = 0; i < n && o + 12 < end; ++i) {
        uint64_t size;
        if (!f.u64(o + 12, size) || size < 40) return;
        niWalk(f, o + 12, depth + 1, bank, out);
        o += 12 + size;
    }
}

} // namespace

std::vector<uint8_t> reaktorEnsembleState(const std::string &path) {
    // Reaktor's own chunk: "\x01" "4RIN" u32 17, 0, 1 (an ensemble follows), "CSAR" with the file's URL, its
    // name and its folder's URL, then an instance block, left empty: Reaktor loads the ensemble as saved
    std::vector<uint8_t> c = {0x01, '4', 'R', 'I', 'N', 17, 0, 0, 0, 0, 1, 'C', 'S', 'A', 'R', 5, 0, 0, 0, 0, 3, 0, 0, 0, 2, 1};
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) c.push_back((uint8_t)(v >> (8 * i))); };
    auto text = [&](const std::string &s) { c.insert(c.end(), s.begin(), s.end()); };
    const size_t slash = path.find_last_of('/');
    std::string name = path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    text(urlOfPath(path));
    c.push_back(0);
    u32(4);
    u32((uint32_t)name.size());
    text(name);
    u32(1);
    u32(3);
    c.push_back(2);
    c.push_back(2);
    text(urlOfPath(path.substr(0, slash + 1)));
    c.push_back(0);
    u32(0);
    u32(0);
    return c;
}

std::vector<std::string> reaktorSnapshots(const std::string &path) {
    std::vector<std::string> out;
    NiFile f;
    f.in.open(path, std::ios::binary);
    if (!f.in) return out;
    f.in.seekg(0, std::ios::end);
    f.size = (uint64_t)f.in.tellg();
    niWalk(f, 0, 0, "", out);
    return out;
}

} // namespace wl
