#include "garageband.hpp"

#include "bplist.hpp"
#include "sampler.hpp"
#include "xml.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

constexpr int kTicksPerBeat = 960;
constexpr int64_t kRegionBias = 3840;      // a region stored at P shows from P + 3840 (one 4/4 bar later)
constexpr uint32_t kToEnd = 0x3fffffff;    // the end record's position; as a region length, "to the content's end"

// channel strip types (an AuCO object's payload +4)
enum : uint8_t { kAudio = 0x40, kAux = 0x42, kInst = 0x43, kOut = 0x44, kBus = 0x45, kMaster = 0x46, kInPair = 0x49, kOutPair = 0x4c };

double r3(double v) { return std::round(v * 1000) / 1000; }
double r4(double v) { return std::round(v * 10000) / 10000; }

// GarageBand's fader and send scale (0-127, 90 = 0 dB) in dB: MIDI volume's square law scaled to 0 dB at 90, which
// gives +5.98 dB at 127 (the faders' +6 dB top). A guess, not yet checked against GarageBand's own readout.
double faderDb(double v) { return v <= 0.01 ? -120.0 : 40.0 * std::log10(v / 90.0); }

bool validUtf8(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        const uint8_t c = p[i];
        const size_t len = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c < 0xf5 ? 4 : 0;
        if (!len || i + len > n) return false;
        for (size_t k = 1; k < len; ++k)
            if ((p[i + k] & 0xc0) != 0x80) return false;
        i += len;
    }
    return true;
}

// names as stored: UTF-8 when the bytes are valid UTF-8, else Latin-1 (both read ASCII alike)
std::string text(const uint8_t *p, size_t n) {
    if (validUtf8(p, n)) return std::string(reinterpret_cast<const char *>(p), n);
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 0x80) s += (char)p[i];
        else { s += (char)(0xc0 | p[i] >> 6); s += (char)(0x80 | (p[i] & 0x3f)); }
    }
    return s;
}

// a little-endian view of a payload: reads past its end give 0
struct View {
    const uint8_t *p = nullptr;
    size_t n = 0;
    uint8_t u8(size_t o) const { return o < n ? p[o] : 0; }
    uint16_t u16(size_t o) const { return o + 2 <= n ? (uint16_t)(p[o] | p[o + 1] << 8) : 0; }
    uint32_t u32(size_t o) const {
        return o + 4 <= n ? (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24 : 0;
    }
    uint64_t u64(size_t o) const { return (uint64_t)u32(o) | (uint64_t)u32(o + 4) << 32; }
    // bytes [a, b) up to the first NUL, as text
    std::string cstr(size_t a, size_t b) const {
        b = std::min(b, n);
        size_t e = a;
        while (e < b && p[e]) ++e;
        return a < e ? text(p + a, e - a) : "";
    }
    std::string raw(size_t a, size_t len) const { return a < n ? std::string(reinterpret_cast<const char *>(p) + a, std::min(len, n - a)) : ""; }
    bool has(size_t o, const char *s, size_t len) const { return o + len <= n && !std::memcmp(p + o, s, len); }
};

// A chunk of ProjectData: a 36-byte header (tag stored reversed, u16 version, u16 class, u32 object id at +0xa,
// a reference at +0xe (a channel strip's number for AuCO and AuCU), a sub-index at +0x12, the payload size at
// +0x1c), then the payload
struct Chunk {
    size_t off = 0;           // where its header starts
    char tag[5] = {};         // "Song", "Trak", "AuCO" ...
    uint16_t cls = 0;
    uint32_t oid = 0, ref = 0;
    View pl;
    bool is(const char *t) const { return !std::memcmp(tag, t, 4); }
};

// an event of a sequence: a 16-byte record (byte 0 type, +4 position) and the records that extend it (byte 7 >= 0x80)
struct Event {
    View rec;
    std::vector<View> ext;
    uint8_t type() const { return rec.u8(0); }
    uint32_t pos() const { return rec.u32(4); }
};

std::vector<Event> eventsOf(const View &pl) {
    std::vector<Event> out;
    for (size_t i = 0; i + 16 <= pl.n; i += 16) {
        const View r{pl.p + i, 16};
        if (r.u8(7) >= 0x80 && !out.empty()) out.back().ext.push_back(r);
        else out.push_back({r, {}});
    }
    return out;
}

// A plug-in record of a channel strip (the layout a .patch's channel strip uses): +6 insert order, +14 settings
// name, +112 bypass, +120 plug-in name, +132 maker, +148 flags (0x08000000 instrument, 0x02000000 MIDI effect)
struct Plugin {
    const Chunk *c = nullptr;
    std::string name, preset;
    int order = 0;
    bool bypassed = false, instrument = false, midiFx = false;
};

bool isPlugin(const Chunk &c) { return c.pl.n >= 140 && (c.pl.has(132, "GAME", 4) || c.pl.has(132, "MELC", 4)); }

Plugin pluginOf(const Chunk &c) {
    Plugin p;
    p.c = &c;
    p.name = c.pl.cstr(120, 132);
    p.preset = c.pl.cstr(14, 120);
    p.order = c.pl.u16(6);
    p.bypassed = c.pl.u8(112) != 0;
    const uint32_t flags = c.pl.n >= 152 ? c.pl.u32(148) : 0;
    p.instrument = flags & 0x08000000;
    p.midiFx = flags & 0x02000000;
    return p;
}

// Apple parameter #n = params[n]: the values of the record's "TSPP" block after its reserved first one
std::vector<float> pluginParams(const Plugin &p) {
    const View &pl = p.c->pl;
    static const char tag[] = "GAMETSPP";
    const uint8_t *f = std::search(pl.p, pl.p + pl.n, tag, tag + 8);
    const size_t g = (size_t)(f - pl.p);
    if (f == pl.p + pl.n || g < 12) return {};
    const size_t s = g - 12;
    const uint32_t count = pl.u32(s + 8);
    if (count > 4096 || s + 24 + 4 * (size_t)count > pl.n) return {};
    std::vector<float> v;
    for (uint32_t i = 1; i < count; ++i) {
        const uint32_t bits = pl.u32(s + 24 + 4 * (size_t)i);
        float x;
        std::memcpy(&x, &bits, 4);
        v.push_back(x);
    }
    return v;
}

// A send record (44 or 76 bytes): +0x14 the destination's code, +0x18 the level (8.24 fixed point on the fader's
// 0-127 scale), +0x3c the destination channel's UUID (76-byte records)
struct Send {
    uint8_t code = 0;
    double level = 0;
    std::string target;   // "" when not stored
};

// A channel strip object (AuCO) and the records that follow it (AuCU: sends, plug-ins, settings)
struct Channel {
    const Chunk *c = nullptr;
    uint32_t r = 0;               // its number (the chunk's reference)
    uint8_t type = 0;
    uint16_t idx = 0;             // within its type
    std::string name;             // "Inst 1", "Bus 2", "Output 1-2"
    double volume = 90, pan = 64; // fader (8.24 fixed point at +0x52, 90 = 0 dB), pan (8.8 at +0x58, 64 = centre)
    uint16_t input = 0;           // an aux: the bus it listens to
    std::string uuid;             // 16 bytes after its ff d5 marker; "" when not stored
    std::vector<const Chunk *> records;

    double gainDb() const { return faderDb(volume); }
    double panUnit() const { return std::clamp((pan - 64.0) / 64.0, -1.0, 1.0); }
    std::vector<Send> sends() const {
        std::vector<Send> out;
        for (const Chunk *c : records) {
            if (c->pl.n != 44 && c->pl.n != 76) continue;
            Send s;
            s.code = c->pl.u8(0x14);
            s.level = c->pl.u32(0x18) / 16777216.0;
            if (c->pl.n >= 0x4c) s.target = c->pl.raw(0x3c, 16);
            out.push_back(s);
        }
        return out;
    }
    std::vector<Plugin> plugins() const {
        std::vector<Plugin> out;
        for (const Chunk *c : records)
            if (isPlugin(*c)) out.push_back(pluginOf(*c));
        return out;
    }
    bool instrument(Plugin &out) const {
        for (auto &p : plugins())
            if (p.instrument) { out = p; return true; }
        return false;
    }
    // the channel strip setting the track was made from ("Grand Piano.cst") and its category ("Pianos and Keyboards")
    std::pair<std::string, std::string> setting() const {
        for (const Chunk *c : records) {
            const View &pl = c->pl;
            if ((pl.n == 176 || pl.n == 192) && (pl.u8(0) == 0xb0 || pl.u8(0) == 0xc4)) {
                std::string name = pl.cstr(0x10, 0x50);
                if (name.empty()) name = pl.cstr(14, 0x50);
                return {name, pl.cstr(0x50, 0x90)};
            }
        }
        return {};
    }
};

std::string trimmed(std::string s) {
    auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f); };
    while (!s.empty() && space(s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && space(s[a])) ++a;
    return s.substr(a);
}

Channel channelOf(const Chunk &c) {
    Channel ch;
    const View &pl = c.pl;
    ch.c = &c;
    ch.r = c.ref;
    ch.type = pl.u8(4);
    ch.idx = pl.u16(6);
    ch.name = trimmed(pl.cstr(0x3c, 0x4c));
    ch.volume = pl.u32(0x52) / 16777216.0;
    ch.pan = pl.u16(0x58) / 256.0;
    ch.input = pl.u16(0x5e);
    for (size_t k = 0xc0; k + 1 < pl.n; ++k)
        if (pl.p[k] == 0xff && pl.p[k + 1] == 0xd5) {
            if (k + 17 <= pl.n) ch.uuid = pl.raw(k + 1, 16);
            break;
        }
    return ch;
}

// An environment object (Envi): a track's or a channel's named object. Its name (u16 length at +0x9e, the bytes
// at +0xa0) is followed, at an even offset, by its channel strip's number + 1.
struct Envi {
    uint32_t oid = 0;
    std::string name;
    int64_t ch = -1;
};

Envi enviOf(const Chunk &c) {
    Envi e;
    const View &pl = c.pl;
    e.oid = c.oid;
    const size_t n = pl.n > 0xa0 ? pl.u16(0x9e) : 0;
    e.name = pl.n > 0xa0 ? text(pl.p + 0xa0, std::min(n, pl.n - 0xa0)) : "";
    const size_t o = 0xa0 + n + (n & 1);
    e.ch = o + 2 <= pl.n ? (int64_t)pl.u16(o) - 1 : -1;
    return e;
}

// A sequence (MSeq): the arrangement, a region, a folder, a global list; its events (EvSq) and tracks (Trak)
struct Sequence {
    uint16_t cls = 0;
    uint32_t oid = 0;
    std::string name;
    std::vector<const Chunk *> traks;
    const Chunk *evsq = nullptr;
    std::vector<Event> events() const { return evsq ? eventsOf(evsq->pl) : std::vector<Event>{}; }
};

// An audio file (AuFl): its name (UTF-16) and, at fixed distances from the name's end, its folder in Media/
// and its length in frames, sample rate, channels and bits
struct AudioFile {
    std::string name, folder;
    uint32_t frames = 0, rate = 0;
};

std::string utf16le(const View &pl, size_t at, size_t units) {
    std::string s;
    auto put = [&](uint32_t c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xc0 | c >> 6); s += (char)(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { s += (char)(0xe0 | c >> 12); s += (char)(0x80 | (c >> 6 & 0x3f)); s += (char)(0x80 | (c & 0x3f)); }
        else { s += (char)(0xf0 | c >> 18); s += (char)(0x80 | (c >> 12 & 0x3f)); s += (char)(0x80 | (c >> 6 & 0x3f)); s += (char)(0x80 | (c & 0x3f)); }
    };
    for (size_t i = 0; i < units && at + 2 * i + 2 <= pl.n; ++i) {
        const uint32_t u = pl.u16(at + 2 * i);
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < units && pl.u16(at + 2 * i + 2) >= 0xdc00 && pl.u16(at + 2 * i + 2) < 0xe000) {
            put(0x10000 + ((u - 0xd800) << 10) + (pl.u16(at + 2 * i + 2) - 0xdc00));
            ++i;
        } else put(u >= 0xd800 && u < 0xe000 ? 0xfffd : u);
    }
    return s;
}

AudioFile audioFileOf(const Chunk &c) {
    AudioFile f;
    const View &pl = c.pl;
    const size_t n = pl.u16(8), b = 10 + 2 * n;
    f.name = utf16le(pl, 10, n);
    if (pl.n > b + 0x90) f.folder = pl.cstr(b + 0x8a, b + 0xca);
    if (pl.n > b + 0x1d8) f.frames = pl.u32(b + 0x1d4);
    if (pl.n > b + 0x1de) f.rate = pl.u16(b + 0x1dc);
    return f;
}

// An audio region (AuRg; header +0xa its file, +0xe its index among the file's regions): +0x0e first frame,
// +0x16 length in frames, +0x4a its name
struct AudioRegion {
    uint32_t file = 0;
    uint64_t start = 0;
    uint32_t frames = 0;
    std::string name;
};

AudioRegion audioRegionOf(const Chunk &c) {
    AudioRegion r;
    const View &pl = c.pl;
    r.file = c.oid;
    if (pl.n > 0x16) r.start = pl.u64(0x0e);
    if (pl.n > 0x1a) r.frames = pl.u32(0x16);
    const size_t n = pl.n > 0x4c ? pl.u16(0x4a) : 0;
    if (pl.n > 0x4c) r.name = text(pl.p + 0x4c, std::min(n, pl.n - 0x4c));
    return r;
}

// a property list file (binary, or XML) as JSON; null when there is none or it can't be read
json readPlist(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return nullptr;
    const std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json out;
    if (parseBinaryPlist(reinterpret_cast<const uint8_t *>(d.data()), d.size(), out)) return out;
    std::string err;
    const auto root = xml::parse(d, err);
    if (!root || root->tag != "plist" || root->children.empty()) return nullptr;
    std::function<json(const xml::Node &)> value = [&](const xml::Node &n) -> json {
        if (n.tag == "dict") {
            json o = json::object();
            for (size_t i = 0; i + 1 < n.children.size(); i += 2)
                if (n.children[i]->tag == "key") o[n.children[i]->text] = value(*n.children[i + 1]);
            return o;
        }
        if (n.tag == "array") {
            json a = json::array();
            for (auto &c : n.children) a.push_back(value(*c));
            return a;
        }
        if (n.tag == "integer") return std::strtoll(n.text.c_str(), nullptr, 10);
        if (n.tag == "real") return std::strtod(n.text.c_str(), nullptr);
        if (n.tag == "true" || n.tag == "false") return n.tag == "true";
        return n.text;   // string, date, data
    };
    return value(*root->children.front());
}

struct Project {
    std::string path;                 // the package
    std::vector<uint8_t> data;        // Alternatives/000/ProjectData
    json meta = json::object();       // Alternatives/000/MetaData.plist
    std::vector<Chunk> chunks;
    // objects by number, in the order first seen (a later object with the same number takes its place)
    std::vector<Channel> channels;
    std::map<uint32_t, size_t> channelAt;
    std::vector<Envi> envis;
    std::map<uint32_t, size_t> enviAt;
    std::vector<Sequence> seqs;
    std::map<std::pair<uint16_t, uint32_t>, size_t> seqAt;
    std::map<uint32_t, AudioFile> files;
    std::map<std::pair<uint32_t, uint32_t>, AudioRegion> regions;   // (file, index)
    const Sequence *arrange = nullptr;

    const Channel *channel(int64_t r) const { auto it = r < 0 ? channelAt.end() : channelAt.find((uint32_t)r); return it == channelAt.end() ? nullptr : &channels[it->second]; }
    const Envi *envi(uint32_t oid) const { auto it = enviAt.find(oid); return it == enviAt.end() ? nullptr : &envis[it->second]; }
    const Sequence *seq(uint16_t cls, uint32_t oid) const { auto it = seqAt.find({cls, oid}); return it == seqAt.end() ? nullptr : &seqs[it->second]; }
    uint16_t version() const { return data.size() >= 6 ? (uint16_t)(data[4] | data[5] << 8) : 0; }

    bool load(const std::string &where, std::string &err) {
        std::error_code ec;
        fs::path pkg = fs::absolute(fs::u8path(where), ec).lexically_normal();
        if (pkg.filename().empty()) pkg = pkg.parent_path();   // "My Song.band/"
        path = pkg.u8string();
        const fs::path alt = pkg / "Alternatives" / "000";
        if (!fs::is_directory(pkg, ec)) { err = "no GarageBand project at " + where; return false; }
        std::ifstream in(alt / "ProjectData", std::ios::binary);
        if (!in) { err = where + " has no Alternatives/000/ProjectData: not a GarageBand 10 project (GarageBand for iOS projects aren't read)"; return false; }
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (data.size() < 0x18 || std::memcmp(data.data(), "#G\xc0\xab", 4)) { err = where + ": its ProjectData isn't a GarageBand project file"; return false; }
        if (fs::exists(alt / "MetaData.plist", ec)) {
            meta = readPlist(alt / "MetaData.plist");
            if (!meta.is_object()) { err = where + ": its MetaData.plist can't be read"; return false; }
        }
        // the chunk walk: from the 24-byte file header to the end of the file exactly
        size_t p = 0x18;
        while (p + 36 <= data.size()) {
            const View h{data.data() + p, 36};
            Chunk c;
            c.off = p;
            for (int k = 0; k < 4; ++k) c.tag[k] = (char)h.p[3 - k];
            c.cls = h.u16(6);
            c.oid = h.u32(0xa);
            c.ref = h.u32(0xe);
            const size_t size = h.u32(0x1c);
            if (p + 36 + size > data.size()) {
                char b[160];
                std::snprintf(b, sizeof b, ": chunk %s at 0x%zx runs past the end of its ProjectData", c.tag, p);
                err = where + b;
                return false;
            }
            c.pl = {data.data() + p + 36, size};
            chunks.push_back(c);
            p += 36 + size;
        }
        if (p != data.size()) {
            char b[160];
            std::snprintf(b, sizeof b, ": its ProjectData ends inside a chunk header (0x%zx of 0x%zx)", p, data.size());
            err = where + b;
            return false;
        }
        int64_t curCh = -1, curSeq = -1;
        for (const Chunk &c : chunks) {
            if (c.is("AuCO")) {   // a channel strip; its records follow it
                curCh = -1;
                if (c.pl.n <= 0x60) continue;
                Channel ch = channelOf(c);
                auto it = channelAt.find(ch.r);
                if (it != channelAt.end()) channels[it->second] = ch;
                else { channelAt[ch.r] = channels.size(); channels.push_back(ch); }
                curCh = (int64_t)channelAt[ch.r];
            } else if (c.is("AuCU")) {
                if (curCh >= 0 && c.ref == channels[(size_t)curCh].r) channels[(size_t)curCh].records.push_back(&c);
            } else if (c.is("Envi")) {
                const Envi e = enviOf(c);
                auto it = enviAt.find(e.oid);
                if (it != enviAt.end()) envis[it->second] = e;
                else { enviAt[e.oid] = envis.size(); envis.push_back(e); }
            } else if (c.is("MSeq")) {   // a sequence: its tracks (Trak) follow it, its events (EvSq) have its class and id
                Sequence s;
                s.cls = c.cls;
                s.oid = c.oid;
                const size_t n = c.pl.u16(0x10);
                if (c.pl.n > 0x12) s.name = text(c.pl.p + 0x12, std::min(n, c.pl.n - 0x12));
                auto it = seqAt.find({s.cls, s.oid});
                if (it != seqAt.end()) seqs[it->second] = s;
                else { seqAt[{s.cls, s.oid}] = seqs.size(); seqs.push_back(s); }
                curSeq = (int64_t)seqAt[{s.cls, s.oid}];
            } else if (c.is("Trak")) {
                if (curSeq >= 0 && c.oid == seqs[(size_t)curSeq].oid && c.pl.n >= 40) seqs[(size_t)curSeq].traks.push_back(&c);
            } else if (c.is("EvSq")) {
                auto it = seqAt.find({c.cls, c.oid});
                if (it != seqAt.end() && !seqs[it->second].evsq) seqs[it->second].evsq = &c;
            } else if (c.is("AuFl")) files[c.oid] = audioFileOf(c);
            else if (c.is("AuRg")) regions[{c.oid, c.ref}] = audioRegionOf(c);
        }
        // the arrangement: the class 0x17 sequence that lists the tracks, the Master Track among them (id 4)
        for (auto &s : seqs) {
            if (s.cls != 0x17) continue;
            bool master = false;
            for (const Chunk *t : s.traks) master |= t->pl.u16(0) == 3;
            if (master && (!arrange || s.oid == 4)) arrange = &s;
        }
        if (!arrange) arrange = seq(0x17, 4);
        if (!arrange) { err = where + ": no arrangement found in its ProjectData"; return false; }
        return true;
    }

    // ---- song-wide settings ----
    // (tick, bpm): the tempo list (class 3, id 0; id 4 holds an unused 120 BPM), event 0x60: extension +0 = BPM x 10000
    std::vector<std::pair<int64_t, double>> tempos() const {
        std::vector<std::pair<int64_t, double>> out;
        if (const Sequence *s = seq(3, 0))
            for (auto &e : s->events())
                if (e.type() == 0x60 && !e.ext.empty()) out.push_back({e.pos(), e.ext[0].u32(0) / 10000.0});
        if (out.empty() && meta.contains("BeatsPerMinute") && meta["BeatsPerMinute"].is_number() && meta["BeatsPerMinute"].get<double>() != 0)
            out.push_back({38400, meta["BeatsPerMinute"].get<double>()});
        return out;
    }
    // (tick, numerator, denominator): event 0x30 of the class 1 list (+11 log2 denominator, +12 numerator)
    std::vector<std::array<int64_t, 3>> signatures() const {
        std::vector<std::array<int64_t, 3>> out;
        if (const Sequence *s = seq(1, 0))
            for (auto &e : s->events())
                if (e.type() == 0x30) out.push_back({(int64_t)e.pos(), e.rec.u8(12), (int64_t)1 << std::min<int>(e.rec.u8(11), 30)});
        if (out.empty()) {
            auto num = [&](const char *k, int def) { return meta.contains(k) && meta[k].is_number() ? (int64_t)meta[k].get<double>() : def; };
            out.push_back({0, num("SongSignatureNumerator", 4), num("SongSignatureDenominator", 4)});
        }
        return out;
    }
    std::string key() const {
        const std::string k = meta.contains("SongKey") && meta["SongKey"].is_string() ? meta["SongKey"].get<std::string>() : "";
        const std::string g = meta.contains("SongGenderKey") && meta["SongGenderKey"].is_string() ? meta["SongGenderKey"].get<std::string>() : "";
        return k.empty() ? "" : g.empty() ? k : k + " " + g;
    }
    // the cycle (start, end ticks): event 0x10 of the class 0x16 list, +4 = end - 1, extension +12 = start
    bool cycle(int64_t &from, int64_t &to) const {
        if (const Sequence *s = seq(0x16, 0))
            for (auto &e : s->events())
                if (e.type() == 0x10 && !e.ext.empty()) { from = e.ext[0].u32(12); to = (int64_t)e.pos() + 1; return true; }
        return false;
    }

    // ---- tracks ----
    struct Track {
        std::string name, kind;       // kind: "instrument", "audio", "drummer", "master", "aux", "bus", "other"
        const Envi *envi = nullptr;   // the track's object
        const Channel *channel = nullptr;
    };
    // The arrangement's Trak entries in order: +0 u16 1 = track, 3 = Master Track, 4 = Drummer; +8 its object
    std::vector<Track> tracks() const {
        std::vector<Track> out;
        for (size_t i = 0; i < arrange->traks.size(); ++i) {
            const View &t = arrange->traks[i]->pl;
            const uint16_t type = t.u16(0);
            Track tr;
            tr.envi = envi(t.u32(8));
            tr.channel = tr.envi ? channel(tr.envi->ch) : nullptr;
            tr.name = tr.envi ? tr.envi->name : "Track " + std::to_string(i + 1);
            if (type == 3) tr.kind = "master";
            else if (type == 4) tr.kind = "drummer";
            else {
                const int ct = tr.channel ? tr.channel->type : -1;
                tr.kind = ct == kAudio ? "audio" : ct == kInst ? "instrument" : ct == kAux ? "aux" : ct == kBus ? "bus" : "other";
            }
            out.push_back(tr);
        }
        return out;
    }

    // The channel a send feeds: the one with its destination's UUID, else by its code, which counts the mono outputs
    // first, then the buses: an aux listening to that bus (its input counts the stereo input pairs first), or the
    // Bus channel itself (GarageBand's Echo and Reverb)
    const Channel *busTarget(const Send &s) const {
        if (!s.target.empty())
            for (auto &ch : channels)
                if (ch.uuid == s.target) return &ch;
        int64_t outs = 0, pairs = 0;
        for (auto &ch : channels) { outs += ch.type == kOut; pairs += ch.type == kInPair; }
        const int64_t bus = (int64_t)s.code - outs + 1, auxInput = pairs + bus - 1;
        for (auto &ch : channels)
            if (ch.type == kAux && ch.input == auxInput) return &ch;
        for (auto &ch : channels)
            if (ch.type == kBus && ch.idx == bus - 1) return &ch;
        return nullptr;
    }
    // the name GarageBand shows for a channel: its object's ("Echo", "Large Hall/3.9s Prince Hall One"), else its own
    std::string label(const Channel &ch) const {
        for (auto &e : envis)
            if (e.ch == (int64_t)ch.r && !e.name.empty()) return e.name;
        return ch.name;
    }

    // ---- regions ----
    struct Placement {
        bool midi = true;
        int64_t start = 0;                 // in the parent's time: the stored position + the bias
        int64_t length = 0;                // ticks; 0 = to the content's end
        uint32_t envi = 0;                 // the track's object
        const Sequence *seq = nullptr;     // a MIDI region's sequence
        const AudioRegion *region = nullptr;
    };
    // The regions placed in a sequence: 0x20 MIDI (or folder) placements, 0x24 audio. Extension 1: +0 the track's
    // object, +12 the length; a MIDI region's extension 2: +0 its sequence (class 0x17); an audio region's 0xbc
    // extension: +8 its index, +12 its file.
    std::vector<Placement> placements(const Sequence &s, int64_t bias) const {
        std::vector<Placement> out;
        for (auto &e : s.events()) {
            if ((e.type() != 0x20 && e.type() != 0x24) || e.ext.empty()) continue;
            Placement p;
            p.envi = e.ext[0].u32(0);
            const uint32_t len = e.ext[0].u32(12);
            p.length = len == kToEnd ? 0 : len;
            p.start = (int64_t)e.pos() + bias;
            if (e.type() == 0x20) {
                p.seq = e.ext.size() > 1 ? seq(0x17, e.ext[1].u32(0)) : nullptr;
            } else {
                p.midi = false;
                for (auto &x : e.ext)
                    if (x.u8(7) == 0xbc) {
                        auto it = regions.find({x.u32(12), x.u32(8)});
                        if (it != regions.end()) p.region = &it->second;
                        break;
                    }
            }
            out.push_back(p);
        }
        return out;
    }

    struct Note { int64_t tick = 0, len = 0; int key = 0; double vel = 0; int ch = 0; };
    struct Ctrl { int64_t tick = 0; std::string type; int cc = 0, value = 0; };
    // The notes and controllers of a region whose sequence starts at song tick `start`: content tick `origin` (bar 1)
    // sounds at `start`, nothing before it or past `length` plays. Folder regions (Drummer's) recurse.
    void regionNotes(const Sequence &s, int64_t start, int64_t length, int64_t origin, int64_t bias, std::vector<Note> &notes,
                     std::vector<Ctrl> &ctrl, int depth = 0) const {
        const int64_t end = length ? start + length : 0;
        if (depth < 4)
            for (auto &p : placements(s, bias)) {
                if (!p.midi || !p.seq) continue;
                std::vector<Note> n2;
                std::vector<Ctrl> c2;
                regionNotes(*p.seq, start + (p.start - origin), p.length, origin, bias, n2, c2, depth + 1);
                for (auto &x : n2) if (!length || x.tick < end) notes.push_back(x);
                for (auto &x : c2) if (!length || x.tick < end) ctrl.push_back(x);
            }
        for (auto &e : s.events()) {
            // MIDI events reuse MIDI status bytes: +12 = data 1 (the key), +11 = data 2 (velocity, value)
            const uint8_t type = e.type(), st = type & 0xf0;
            if (type < 0x80 || type >= 0xf0 || e.pos() >= kToEnd) continue;
            const int64_t t = start + ((int64_t)e.pos() - origin);
            if (t < start || (length && t >= end)) continue;
            const int ch = type & 0x0f, d1 = e.rec.u8(12), d2 = e.rec.u8(11);
            if (st == 0x90) {   // extension +12: the length in ticks; +10..11: a high-resolution velocity (/ 32767)
                Note n;
                n.tick = t;
                n.len = e.ext.empty() ? kTicksPerBeat / 4 : e.ext[0].u32(12);
                const uint16_t hires = e.rec.u16(10);
                n.vel = std::clamp(e.rec.u8(10) && hires ? hires / 32767.0 : d2 / 127.0, 0.0, 1.0);
                n.key = d1;
                n.ch = ch;
                notes.push_back(n);
            } else if (st == 0xb0) ctrl.push_back({t, "cc", d1, d2});
            else if (st == 0xe0) ctrl.push_back({t, "pitchbend", 0, (d2 << 7 | d1) - 8192});
            else if (st == 0xd0) ctrl.push_back({t, "pressure", 0, d1});
            else if (st == 0xa0) ctrl.push_back({t, "polypressure", 0, d2});
            else if (st == 0xc0) ctrl.push_back({t, "program", 0, d1});
        }
    }

    // events in each track's automation sequence (the class 0x17 "Automation" sequence places one per track), by
    // track object, in the order first seen
    std::vector<std::pair<uint32_t, int>> automation() const {
        std::vector<std::pair<uint32_t, int>> out;
        const Sequence *autoSeq = nullptr;
        for (auto &s : seqs)
            if (s.cls == 0x17 && s.name == "Automation") { autoSeq = &s; break; }
        if (!autoSeq) return out;
        for (auto &p : placements(*autoSeq, kRegionBias)) {
            if (!p.seq) continue;
            int n = 0;
            for (auto &e : p.seq->events()) n += e.type() != 0xf1;
            if (!n) continue;
            auto it = std::find_if(out.begin(), out.end(), [&](auto &x) { return x.first == p.envi; });
            if (it == out.end()) out.push_back({p.envi, n}); else it->second += n;
        }
        return out;
    }
};

std::string lowerAscii(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }

// a track or channel name as a folder name
std::string safeName(const std::string &s) {
    std::string o;
    bool run = false;   // a run of characters a file name can't hold becomes one '-'
    for (char c : s) {
#if defined(_WIN32)
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
#else
        const bool bad = c == '/' || c == '\\' || c == ':';
#endif
        if (!bad) o += c;
        else if (!run) o += '-';
        run = bad;
    }
    o = trimmed(o);
    size_t a = 0, b = o.size();
    while (a < b && o[a] == '.') ++a;
    while (b > a && o[b - 1] == '.') --b;
    o = o.substr(a, b - a);
    return o.empty() ? "Track" : o;
}

struct Converter {
    const Project &P;
    std::string outDir;
    fs::path patchDir;
    bool allTracks = false, copyMedia = false;
    std::vector<std::string> warn;
    std::set<std::string> usedNames;
    std::vector<json> buses;                 // in the order first sent to
    std::map<uint32_t, size_t> busOf;        // channel number -> its bus
    int64_t barTicks = 3840, bar1 = 38400, bias = kRegionBias;

    Converter(const Project &p, const std::string &out) : P(p), outDir(out), patchDir(fs::u8path(out) / "patches") {
        const auto sigs = P.signatures();
        const int64_t num = sigs[0][1], den = std::max<int64_t>(1, sigs[0][2]);
        // bar 1 = ten bars of the first signature after tick 0 (38400 in 4/4); regions show one bar after their stored
        // position. Both seen in 4/4 only.
        barTicks = std::llround(num * 4.0 / den * kTicksPerBeat);
        bar1 = 10 * barTicks;
        bias = barTicks;
        char b[200];
        if (num != 4 || den != 4) {
            std::snprintf(b, sizeof b, "time signature %lld/%lld: bar 1 and region positions assume one stored bar = one %lld/%lld bar (verified in 4/4 only)",
                          (long long)num, (long long)den, (long long)num, (long long)den);
            warn.push_back(b);
        }
        if (sigs.size() > 1) {
            std::snprintf(b, sizeof b, "%zu time signature changes: only the first (%lld/%lld) is written", sigs.size() - 1, (long long)num, (long long)den);
            warn.push_back(b);
        }
    }

    double beat(int64_t tick) const { return (double)(tick - bar1) / kTicksPerBeat; }

    // A patch folder holding a channel strip as GarageBand stores it: the channel's object as the header, then every
    // record of it. Returns its path relative to the job.
    std::string writePatch(const Channel &ch, const std::string &name, const std::string &sub = "") {
        const std::string base = safeName(name);
        std::string n = base;
        for (int k = 2; usedNames.count(lowerAscii(sub + n)); ++k) n = base + " " + std::to_string(k);
        usedNames.insert(lowerAscii(sub + n));
        const std::string rel = "patches/" + (sub.empty() ? "" : sub + "/") + n + ".patch";
        const fs::path dir = fs::u8path(outDir) / fs::u8path(rel);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream f(dir / "#Root.cst", std::ios::binary);
        auto put = [&](const Chunk &c) { f.write(reinterpret_cast<const char *>(P.data.data() + c.off), (std::streamsize)(36 + c.pl.n)); };
        put(*ch.c);
        for (const Chunk *r : ch.records) put(*r);
        return rel;
    }

    // what plays from a patch folder and its effects as Wavelength's (`samples --patch`); null when it can't be read
    json describe(const std::string &rel) const {
        std::string err;
        std::error_code ec;
        return describePatch(fs::absolute(fs::u8path(outDir) / fs::u8path(rel), ec).u8string(), outDir, err);
    }

    // GarageBand's Echo: parameters #16 Time (an index: 1/2., 1/2, 1/2T, 1/4., ... 1/16T), #17 Repeat %, #18 Color,
    // #19 Dry %, #20 Wet %, as a tempo-synced delay (its Color as a low-pass: a guess)
    static json echoFx(const Plugin &p) {
        const std::vector<float> v = pluginParams(p);
        auto g = [&](size_t i, double def) { return i < v.size() && std::isfinite(v[i]) ? (double)v[i] : def; };
        const long idx = std::lround(std::nearbyint(std::clamp(g(16, 4), -1e6, 1e6)));
        static const double base[] = {2.0, 1.0, 0.5, 0.25}, mult[] = {1.5, 1.0, 2.0 / 3.0};
        const double time = base[std::clamp(idx / 3, 0L, 3L)] * mult[(idx % 3 + 3) % 3];
        const double dry = g(19, 0) / 100.0, wet = g(20, 100) / 100.0;
        if (wet <= 0) return json::array();
        json fx = {{"type", "delay"}, {"time", r4(time)}, {"feedback", r4(std::min(0.95, g(17, 30) / 100.0))},
                   {"mix", dry + wet > 0 ? r4(wet / (dry + wet)) : 1.0}, {"lowpass", r3(std::min(20000.0, 6000.0 * std::pow(2.0, g(18, 0) / 50.0)))},
                   {"highpass", 100}};
        json out = json::array({fx});
        if (dry + wet > 0 && std::fabs(20 * std::log10(dry + wet)) > 0.05) out.push_back({{"type", "gain"}, {"db", r3(20 * std::log10(dry + wet))}});
        return out;
    }

    // the effects of a channel without an instrument (an audio track, an aux, the master) as Wavelength fx
    json channelFx(const Channel &ch, const std::string &label, const std::string &rel) {
        const json desc = describe(rel);
        json fx = desc.is_object() && desc.contains("effects") ? desc["effects"] : json::array();
        std::vector<std::string> notes;
        if (desc.is_object() && desc.contains("effectNotes"))
            for (auto &n : desc["effectNotes"])
                if (n.get<std::string>().rfind("Echo: ", 0) != 0) notes.push_back(n.get<std::string>());
        // Echo isn't one of the patch effects: played here, first in the chain or after the others
        std::vector<Plugin> chain;
        for (auto &p : ch.plugins())
            if (!p.instrument && !p.midiFx) chain.push_back(p);
        std::stable_sort(chain.begin(), chain.end(), [](const Plugin &a, const Plugin &b) { return a.order < b.order; });
        bool echo = false;
        for (size_t i = 0; i < chain.size(); ++i) {
            if (chain[i].name != "Echo" || chain[i].bypassed) continue;
            const json e = echoFx(chain[i]);
            if (i == 0) fx.insert(fx.begin(), e.begin(), e.end());
            else fx.insert(fx.end(), e.begin(), e.end());
            echo = true;
        }
        if (echo) notes.push_back("Echo: played as a tempo-synced delay (its Color as a low-pass: a guess)");
        if (!desc.is_object()) {
            std::string names;
            for (auto &p : chain)
                if (!p.bypassed && p.name != "Echo") names += (names.empty() ? "" : ", ") + p.name;
            if (!names.empty()) notes.push_back("effects not translated (its channel strip can't be read): " + names);
        }
        for (auto &n : notes) warn.push_back(label + ": " + n);
        return fx;
    }

    // ---- tracks ----
    // A Sampler instrument saved inside the project (Media/Sampler Instruments/<name>.exs): the name the instrument's
    // record gives ("MELC" "PMAS" + 4 bytes + "<name>.exs") or its settings name
    std::string localExs(const Plugin &inst) const {
        std::vector<std::string> names;
        const View &pl = inst.c->pl;
        for (size_t at = 0; at + 8 <= pl.n && names.empty(); ++at) {
            if (!pl.has(at, "MELCPMAS", 8) || at + 12 > pl.n) continue;
            if (std::find(pl.p + at + 8, pl.p + at + 12, '\n') != pl.p + at + 12) continue;
            for (size_t e = at + 12; e < pl.n && pl.p[e] >= 0x20 && pl.p[e] <= 0x7e; ++e)
                if (e > at + 12 && pl.has(e, ".exs", 4)) { names.push_back(pl.raw(at + 12, e + 4 - (at + 12))); break; }
        }
        if (const std::string lp = lowerAscii(inst.preset); lp.size() >= 4 && lp.compare(lp.size() - 4, 4, ".exs") == 0) names.push_back(inst.preset);
        if (!P.meta.contains("SamplerInstrumentsFiles") || !P.meta["SamplerInstrumentsFiles"].is_array()) return "";
        for (auto &n : names)
            for (auto &f : P.meta["SamplerInstrumentsFiles"]) {
                if (!f.is_string()) continue;
                const fs::path rel = fs::u8path(f.get<std::string>());
                if (rel.filename().u8string() != n || rel.is_absolute()) continue;
                const fs::path p = fs::u8path(P.path) / "Media" / rel;
                std::error_code ec;
                if (fs::exists(p, ec)) return p.u8string();
            }
        return "";
    }

    static std::string fallbackPreset(const std::string &inst, const std::string &category, const std::string &setting) {
        const std::string s = lowerAscii(category + " " + setting + " " + inst);
        static const std::vector<std::pair<std::string, std::string>> roles = {
            {"bass", "BA Analog"}, {"organ", "KY Organ"}, {"b3", "KY Organ"}, {"piano", "KY Electric Piano"}, {"clav", "PL Pluck"},
            {"string", "PD Strings"}, {"pad", "PD Warm"}, {"lead", "LD Saw"}, {"pluck", "PL Pluck"}};
        for (auto &[k, v] : roles)
            if (s.find(k) != std::string::npos) return v;
        return "Init";
    }

    // the regions of a track: its notes and controllers, and its audio regions
    void regionEvents(const Project::Track &t, std::vector<Project::Note> &notes, std::vector<Project::Ctrl> &ctrl,
                      std::vector<Project::Placement> &clips) {
        for (auto &p : P.placements(*P.arrange, bias)) {
            if (!t.envi || p.envi != t.envi->oid) continue;
            if (!p.midi) { clips.push_back(p); continue; }
            if (!p.seq) { warn.push_back(t.name + ": a region whose sequence is missing, left out"); continue; }
            std::vector<Project::Note> n;
            std::vector<Project::Ctrl> c;
            P.regionNotes(*p.seq, p.start, p.length, bar1, bias, n, c);
            if (n.empty() && c.empty()) {
                char b[64];
                std::snprintf(b, sizeof b, "%.2f", 1 + (double)(p.start - bar1) / barTicks);
                warn.push_back(t.name + ": region \"" + p.seq->name + "\" at bar " + b + " holds no notes");
            }
            notes.insert(notes.end(), n.begin(), n.end());
            ctrl.insert(ctrl.end(), c.begin(), c.end());
        }
    }

    // CC64 as note lengths: a note released while the pedal is down sounds until the pedal comes up
    void applySustain(std::vector<Project::Note> &notes, const std::vector<Project::Ctrl> &ctrl, const std::string &name) {
        std::vector<Project::Ctrl> pedal;
        for (auto &c : ctrl)
            if (c.type == "cc" && c.cc == 64) pedal.push_back(c);
        std::stable_sort(pedal.begin(), pedal.end(), [](auto &a, auto &b) { return a.tick < b.tick; });
        if (std::none_of(pedal.begin(), pedal.end(), [](auto &c) { return c.value >= 64; })) return;
        int changed = 0;
        for (auto &n : notes) {
            const int64_t end = n.tick + n.len;
            bool down = false;
            for (auto &c : pedal) {
                if (c.tick > end) break;
                down = c.value >= 64;
            }
            if (!down) continue;
            int64_t up = end + 4 * kTicksPerBeat;
            for (auto &c : pedal)
                if (c.tick > end && c.value < 64) { up = c.tick; break; }
            n.len = up - n.tick;
            ++changed;
        }
        if (changed) warn.push_back(name + ": sustain pedal (CC64) played as longer notes (" + std::to_string(changed) + " notes)");
    }

    json makeClips(const std::vector<Project::Placement> &clips, const std::string &name) {
        json out = json::array();
        for (auto &p : clips) {
            if (!p.region) { warn.push_back(name + ": an audio region whose file record is missing, left out"); continue; }
            auto f = P.files.find(p.region->file);
            if (f == P.files.end()) { warn.push_back(name + ": audio region \"" + p.region->name + "\": file record missing, left out"); continue; }
            const AudioFile &af = f->second;
            const fs::path src = fs::u8path(P.path) / "Media" / fs::u8path(af.folder.empty() ? "Audio Files" : af.folder) / fs::u8path(af.name);
            std::error_code ec;
            if (!fs::exists(src, ec)) { warn.push_back(name + ": " + src.u8string() + " is not in the project (left out)"); continue; }
            std::string file = src.u8string();
            if (copyMedia) {
                fs::create_directories(fs::u8path(outDir) / "media", ec);
                fs::copy_file(src, fs::u8path(outDir) / "media" / fs::u8path(af.name), fs::copy_options::overwrite_existing, ec);
                file = "media/" + af.name;
            }
            json clip = {{"file", file}, {"beat", r4(beat(p.start))}};
            const double rate = af.rate ? af.rate : 44100;
            if (p.region->start) clip["start"] = r4((double)p.region->start / rate);
            if (p.region->frames && af.frames && p.region->frames < af.frames) clip["length"] = r4(p.region->frames / rate);
            out.push_back(clip);
        }
        if (!out.empty())
            warn.push_back(name + ": audio regions placed from their file records; region trims, loops, fades and gain are not decoded yet");
        return out;
    }

    // an aux or bus channel as a bus named as GarageBand shows it, its effects from its plug-ins
    json &bus(const Channel &ch) {
        if (auto it = busOf.find(ch.r); it != busOf.end()) return buses[it->second];
        const std::string label = P.label(ch);
        const std::string rel = writePatch(ch, label, "_aux");
        json b = {{"name", label}};
        const json fx = channelFx(ch, "bus " + label, rel);
        if (!fx.empty()) b["fx"] = fx;
        if (std::fabs(ch.gainDb()) > 0.005) b["gain"] = r3(ch.gainDb());
        if (std::fabs(ch.panUnit()) > 0.001) {
            char t[32];
            std::snprintf(t, sizeof t, "%.0f", ch.pan - 64);
            warn.push_back("bus " + label + ": its pan (" + t + ") is not written");
        }
        busOf[ch.r] = buses.size();
        buses.push_back(b);
        return buses.back();
    }

    // a track as a job track; null when it's left out
    json track(const Project::Track &t) {
        const std::string &name = t.name;
        std::vector<Project::Note> notes;
        std::vector<Project::Ctrl> ctrl;
        std::vector<Project::Placement> clips;
        regionEvents(t, notes, ctrl, clips);
        if (notes.empty() && clips.empty() && !allTracks) {
            if (t.kind != "master") warn.push_back(name + ": no regions, left out");
            return nullptr;
        }
        json job = {{"name", name}};
        if (!t.channel) { warn.push_back(name + ": its channel strip could not be found; left out"); return nullptr; }
        const Channel &ch = *t.channel;
        const std::string rel = writePatch(ch, name);
        Plugin inst;
        const bool hasInst = ch.instrument(inst);
        if ((t.kind == "instrument" || t.kind == "drummer") && hasInst) {
            const json desc = describe(rel);
            const auto [setting, category] = ch.setting();
            const bool plays = desc.is_object() && desc.value("plays", false);
            if (plays && desc.contains("synth")) {   // an Apple synth, re-created on builtin:synth from the patch folder
                job["plugin"] = "builtin:synth";
                job["preset"] = rel;
                warn.push_back(name + ": " + desc["synth"].value("instrument", inst.name) + " re-created on builtin:synth (an approximation)");
            } else if (plays) {
                job["plugin"] = "builtin:sampler";
                job["sampler"] = {{"patch", rel}};
            } else if (const std::string exs = localExs(inst); !exs.empty()) {   // a Sampler instrument made in the project
                job["plugin"] = "builtin:sampler";
                job["sampler"] = {{"exs", exs}};
                if (desc.is_object()) {
                    for (auto &n : desc.value("effectNotes", json::array())) warn.push_back(name + ": " + n.get<std::string>());
                    if (!desc.value("effects", json::array()).empty()) job["fx"] = desc["effects"];
                }
                warn.push_back(name + ": plays its project-local instrument " + fs::u8path(exs).filename().u8string() + " (the channel's effects as fx)");
            } else {
                const std::string why = desc.is_object() && desc.contains("why") ? desc["why"].get<std::string>() : "its channel strip can't be read";
                const bool sampler = inst.name == "Sampler" || inst.name == "EXS24" || inst.name == "Drum Kit";
                job["plugin"] = sampler ? "builtin:sampler" : "builtin:synth";
                if (sampler) job["sampler"] = {{"patch", rel}}; else job["preset"] = rel;
                const json standIn = {{"plugin", "builtin:synth"}, {"preset", fallbackPreset(inst.name, category, setting)}};
                job["fallback"] = json::array({standIn});
                warn.push_back(name + ": " + (setting.empty() ? inst.name : setting) + " (" + inst.name + ") doesn't play here: " + why +
                               "; a built-in stand-in is the fallback");
            }
            for (auto &p : ch.plugins())
                if (p.midiFx && !p.bypassed && p.name != "Arpeggiator") warn.push_back(name + ": MIDI effect " + p.name + " not played");
            if (category.rfind("Smart", 0) == 0 && (!notes.empty() || !ctrl.empty()))
                warn.push_back(name + ": " + category + " instrument: its regions play as stored notes (patterns GarageBand generates live from chord "
                               "strips or autoplay are not re-created)");
            if (t.kind == "drummer")
                warn.push_back(name + ": Drummer track: the performance stored in its region plays as notes (Drummer's own regeneration, fills and "
                               "follow settings are not re-created)");
        } else if (t.kind == "audio") {
            job["plugin"] = "builtin:audio";
            const json fx = channelFx(ch, name, rel);
            if (!fx.empty()) job["fx"] = fx;
        } else {
            warn.push_back(name + ": track kind " + t.kind + " not converted");
            return nullptr;
        }
        // fader, pan, sends
        if (std::fabs(ch.gainDb()) > 0.005) job["gain"] = r3(ch.gainDb());
        if (std::fabs(ch.panUnit()) > 0.001) job["pan"] = r3(ch.panUnit());
        json sends = json::object();
        for (auto &s : ch.sends()) {
            if (s.level <= 0.001) continue;
            const Channel *tg = P.busTarget(s);
            if (!tg) { warn.push_back(name + ": a send (code " + std::to_string(s.code) + ") whose bus could not be found"); continue; }
            sends[bus(*tg)["name"].get<std::string>()] = r3(faderDb(s.level));
        }
        if (!sends.empty()) job["sends"] = sends;
        // notes in beats from bar 1
        if (!notes.empty()) {
            applySustain(notes, ctrl, name);
            std::stable_sort(notes.begin(), notes.end(), [](auto &a, auto &b) { return a.tick != b.tick ? a.tick < b.tick : a.key < b.key; });
            json out = json::array();
            int dropped = 0;
            for (auto &n : notes) {
                const double b = beat(n.tick);
                if (b < 0) { ++dropped; continue; }
                json e = {{"beat", r4(b)}, {"dur", r4((double)std::max<int64_t>(1, n.len) / kTicksPerBeat)}, {"key", n.key}, {"vel", r3(n.vel)}};
                if (n.ch) e["channel"] = n.ch;
                out.push_back(e);
            }
            if (dropped) warn.push_back(name + ": " + std::to_string(dropped) + " notes before bar 1 left out");
            job["notes"] = out;
        }
        // controllers: the sustain pedal is in the note lengths; the rest is counted (and kept on audio tracks)
        std::map<std::string, int> other;
        for (auto &c : ctrl)
            if (!(c.type == "cc" && c.cc == 64)) ++other[c.type == "cc" ? "CC" + std::to_string(c.cc) : c.type];
        if (!other.empty()) {
            json cc = json::object();
            for (auto &c : ctrl)
                if (c.type == "cc" && c.cc != 64 && beat(c.tick) >= 0) cc[std::to_string(c.cc)].push_back({r4(beat(c.tick)), c.value});
            const std::string plugin = job["plugin"].get<std::string>();
            if (!cc.empty() && plugin != "builtin:synth" && plugin != "builtin:sampler")
                for (auto &[k, v] : cc.items()) job["automation"]["cc"][k] = {{"points", v}, {"shape", "step"}};
            std::string list;
            for (auto &[k, n] : other) list += (list.empty() ? "" : ", ") + k + " x" + std::to_string(n);
            warn.push_back(name + ": controller data not played by the built-in instruments: " + list);
        }
        if (!clips.empty()) job["clips"] = makeClips(clips, name);
        return job;
    }

    // Output 1-2's effects and its fader plus the Master channel's
    json master() {
        const Channel *out = nullptr, *mst = nullptr;
        for (auto &c : P.channels) {
            if (c.type == kOutPair && !out) out = &c;
            if (c.type == kMaster && !mst) mst = &c;
        }
        json m = json::object();
        const double g = (out ? out->gainDb() : 0) + (mst ? mst->gainDb() : 0);
        if (std::fabs(g) > 0.005) m["gain"] = r3(g);
        if (out) {
            const json fx = channelFx(*out, "master", writePatch(*out, "_master"));
            if (!fx.empty()) m["fx"] = fx;
        }
        return m;
    }

    json convert() {
        std::error_code ec;
        fs::create_directories(patchDir, ec);
        json job = json::object();
        auto tempos = P.tempos();
        std::sort(tempos.begin(), tempos.end());
        if (tempos.empty()) {
            job["tempo"] = 120.0;
            warn.push_back("no tempo found: 120 BPM");
        } else if (tempos.size() == 1) job["tempo"] = r4(tempos[0].second);
        else {
            job["tempo"] = json::array();
            for (auto &[t, bpm] : tempos) job["tempo"].push_back({{"beat", r4(std::max(0.0, beat(t)))}, {"bpm", r4(bpm)}});
            warn.push_back("tempo map: " + std::to_string(tempos.size()) + " tempo events written as steps (GarageBand ramps not decoded)");
        }
        const auto sig = P.signatures()[0];
        job["timeSignature"] = {sig[1], sig[2]};
        if (!P.key().empty()) {
            const json key = {{"bar", 1}, {"key", P.key()}};
            job["keys"] = json::array({key});
        }
        job["tail"] = 3.0;
        const json rate = P.meta.contains("SampleRate") && P.meta["SampleRate"].is_number() ? P.meta["SampleRate"] : json(nullptr);
        if (!rate.is_null() && rate.get<double>() != 0) job["sampleRate"] = (int)rate.get<double>();
        json tracks = json::array();
        for (auto &t : P.tracks()) {
            if (t.kind == "master") continue;
            json j = track(t);
            if (!j.is_null()) tracks.push_back(j);
        }
        job["tracks"] = tracks;
        if (!buses.empty()) job["buses"] = buses;
        const json m = master();
        if (!m.empty()) job["master"] = m;
        for (auto &[oid, n] : P.automation()) {
            const Envi *e = P.envi(oid);
            char b[32];
            std::snprintf(b, sizeof b, "object %x", oid);
            warn.push_back((e ? e->name : std::string(b)) + ": " + std::to_string(n) + " automation events not converted");
        }
        int64_t c0 = 0, c1 = 0;
        const bool cyc = P.cycle(c0, c1);
        char format[48];
        std::snprintf(format, sizeof format, "GarageBand ProjectData %x", P.version());
        const json info = readPlist(fs::u8path(P.path) / "Resources" / "ProjectInformation.plist");
        job["import"] = {{"from", P.path}, {"format", format},
                         {"savedWith", info.is_object() && info.contains("LastSavedFrom") ? info["LastSavedFrom"] : json(nullptr)},
                         {"sampleRate", rate}, {"cycle", cyc ? json::array({r4(beat(c0)), r4(beat(c1))}) : json(nullptr)}, {"warnings", warn}};
        return job;
    }
};

} // namespace

bool importGarageBand(const std::string &path, const std::string &outDir, DawprojectImport &out, std::string &err, bool allTracks, bool copyMedia) {
    Project P;
    if (!P.load(path, err)) return false;
    Converter c(P, outDir);
    c.allTracks = allTracks;
    c.copyMedia = copyMedia;
    json job = c.convert();
    out.notes = c.warn;
    out.tracks = job["tracks"].size();
    out.buses = job.contains("buses") ? job["buses"].size() : 0;
    for (auto &t : job["tracks"]) out.noteCount += t.contains("notes") ? t["notes"].size() : 0;
    const json &saved = job["import"]["savedWith"];
    out.application = saved.is_string() ? saved.get<std::string>() : "GarageBand";
    out.job = job;
    std::ofstream o(fs::u8path(outDir) / "job.json");
    o << job.dump(1, ' ', false, json::error_handler_t::replace) << "\n";
    if (!o) { err = "cannot write " + (fs::u8path(outDir) / "job.json").u8string(); return false; }
    return true;
}

} // namespace wl
