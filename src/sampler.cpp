#include "sampler.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool readFile(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize((size_t)f.tellg());
    f.seekg(0);
    f.read(reinterpret_cast<char *>(out.data()), (std::streamsize)out.size());
    return (bool)f;
}

uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// ---- zip (stored or deflated entries; no zip64) -------------------------------------------
struct ZipEntry { std::string name; uint16_t method; uint32_t compSize, size, localOffset; };

class Zip {
public:
    bool open(const std::string &path, std::string &err) {
        path_ = path;
        f_.open(path, std::ios::binary);
        if (!f_) { err = "cannot open " + path; return false; }
        f_.seekg(0, std::ios::end);
        const size_t size = (size_t)f_.tellg();
        const size_t tail = std::min<size_t>(size, 65536 + 22);
        std::vector<uint8_t> buf(tail);
        f_.seekg((std::streamoff)(size - tail));
        f_.read(reinterpret_cast<char *>(buf.data()), (std::streamsize)tail);
        long eocd = -1;
        for (long i = (long)tail - 22; i >= 0; --i)
            if (u32(&buf[i]) == 0x06054b50) { eocd = i; break; }
        if (eocd < 0) { err = path + " is not a zip archive"; return false; }
        const uint32_t cdSize = u32(&buf[eocd + 12]), cdOffset = u32(&buf[eocd + 16]);
        std::vector<uint8_t> cd(cdSize);
        f_.seekg(cdOffset);
        f_.read(reinterpret_cast<char *>(cd.data()), cdSize);
        for (size_t p = 0; p + 46 <= cd.size() && u32(&cd[p]) == 0x02014b50;) {
            ZipEntry e;
            e.method = u16(&cd[p + 10]);
            e.compSize = u32(&cd[p + 20]);
            e.size = u32(&cd[p + 24]);
            const uint16_t nameLen = u16(&cd[p + 28]), extraLen = u16(&cd[p + 30]), commentLen = u16(&cd[p + 32]);
            e.localOffset = u32(&cd[p + 42]);
            e.name.assign(reinterpret_cast<const char *>(&cd[p + 46]), nameLen);
            entries_.push_back(e);
            p += 46 + nameLen + extraLen + commentLen;
        }
        return true;
    }
    bool read(const std::string &name, std::vector<uint8_t> &out, std::string &err) {
        const ZipEntry *e = nullptr;
        for (auto &x : entries_) if (x.name == name) { e = &x; break; }
        if (!e) for (auto &x : entries_) if (lower(x.name) == lower(name)) { e = &x; break; }
        if (!e) { err = path_ + " has no entry '" + name + "'"; return false; }
        uint8_t lh[30];
        f_.seekg(e->localOffset);
        f_.read(reinterpret_cast<char *>(lh), 30);
        if (u32(lh) != 0x04034b50) { err = "bad zip entry header for " + name; return false; }
        f_.seekg(e->localOffset + 30 + u16(lh + 26) + u16(lh + 28));
        std::vector<uint8_t> comp(e->compSize);
        f_.read(reinterpret_cast<char *>(comp.data()), e->compSize);
        if (e->method == 0) { out = std::move(comp); return true; }
        if (e->method != 8) { err = name + ": unsupported zip compression " + std::to_string(e->method); return false; }
        out.resize(e->size);
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { err = "zlib init failed"; return false; }
        zs.next_in = comp.data(); zs.avail_in = (uInt)comp.size();
        zs.next_out = out.data(); zs.avail_out = (uInt)out.size();
        const int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) { err = name + ": corrupt deflate data"; return false; }
        return true;
    }
private:
    std::string path_;
    std::ifstream f_;
    std::vector<ZipEntry> entries_;
};

// ---- WAV decoding -------------------------------------------------------------------------
struct SampleData {
    double rate = 44100;
    std::vector<float> l, r;     // r empty = mono
    size_t frames() const { return l.size(); }
};

bool decodeWav(const std::vector<uint8_t> &d, SampleData &s, std::string &err) {
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) { err = "not a RIFF/WAVE file"; return false; }
    int format = 0, channels = 0, bits = 0;
    const uint8_t *data = nullptr;
    size_t dataLen = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t len = u32(&d[p + 4]);
        const uint8_t *body = &d[p + 8];
        const size_t avail = std::min<size_t>(len, d.size() - p - 8);
        if (!memcmp(&d[p], "fmt ", 4) && avail >= 16) {
            format = u16(body); channels = u16(body + 2); s.rate = u32(body + 4); bits = u16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = u16(body + 24);   // WAVE_FORMAT_EXTENSIBLE sub-format
        } else if (!memcmp(&d[p], "data", 4)) {
            data = body; dataLen = avail;
        }
        p += 8 + len + (len & 1);
    }
    if (!data || channels < 1) { err = "WAV has no fmt/data chunk"; return false; }
    if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && (bits == 32 || bits == 64)))) {
        err = "unsupported WAV encoding (format " + std::to_string(format) + ", " + std::to_string(bits) + " bit)";
        return false;
    }
    const size_t bps = bits / 8, frame = bps * channels, n = dataLen / frame;
    s.l.resize(n);
    if (channels > 1) s.r.resize(n);
    auto get = [&](const uint8_t *q) -> float {
        if (format == 3) { if (bits == 32) { float f; memcpy(&f, q, 4); return f; } double v; memcpy(&v, q, 8); return (float)v; }
        switch (bits) {
        case 8: return (q[0] - 128) / 128.f;
        case 16: return (int16_t)u16(q) / 32768.f;
        case 24: return (float)((int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.f;
        default: return (float)((int32_t)u32(q) / 2147483648.0);
        }
    };
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *q = data + i * frame;
        s.l[i] = get(q);
        if (channels > 1) s.r[i] = get(q + bps);
    }
    return true;
}

// ---- tiny XML reader for multisample.xml ----------------------------------------------------
std::string unescape(std::string v) {
    static const std::pair<const char *, const char *> ents[] = {{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (auto &[from, to] : ents)
        for (size_t p; (p = v.find(from)) != std::string::npos;) v.replace(p, strlen(from), to);
    return v;
}

struct Tag { std::string name; std::map<std::string, std::string> attrs; bool closing = false, selfClosing = false; };

std::vector<Tag> tags(const std::string &xml) {
    std::vector<Tag> out;
    for (size_t p = 0; (p = xml.find('<', p)) != std::string::npos;) {
        const size_t end = xml.find('>', p);
        if (end == std::string::npos) break;
        std::string body = xml.substr(p + 1, end - p - 1);
        p = end + 1;
        if (body.empty() || body[0] == '?' || body[0] == '!') continue;
        Tag t;
        if (body[0] == '/') { t.closing = true; body.erase(0, 1); }
        if (!body.empty() && body.back() == '/') { t.selfClosing = true; body.pop_back(); }
        size_t i = 0;
        while (i < body.size() && !isspace((unsigned char)body[i])) ++i;
        t.name = body.substr(0, i);
        while (i < body.size()) {
            while (i < body.size() && isspace((unsigned char)body[i])) ++i;
            const size_t eq = body.find('=', i);
            if (eq == std::string::npos) break;
            std::string key = body.substr(i, eq - i);
            while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
            size_t q = eq + 1;
            while (q < body.size() && isspace((unsigned char)body[q])) ++q;
            if (q >= body.size()) break;
            const char quote = body[q];
            const size_t close = body.find(quote, q + 1);
            if (close == std::string::npos) break;
            t.attrs[key] = unescape(body.substr(q + 1, close - q - 1));
            i = close + 1;
        }
        out.push_back(std::move(t));
    }
    return out;
}

double num(const std::map<std::string, std::string> &a, const char *k, double def) {
    auto it = a.find(k);
    if (it == a.end() || it->second.empty()) return def;
    if (it->second == "true") return 1;
    if (it->second == "false") return 0;
    return std::atof(it->second.c_str());
}

// ---- zones --------------------------------------------------------------------------------
struct Zone {
    std::string file;
    int keyLow = 0, keyHigh = 127, root = 60;
    double keyTrack = 1, tune = 0, gainDb = 0;
    int velLow = 0, velHigh = 127, velLowFade = 0, velHighFade = 0;
    int selLow = 0, selHigh = 127;
    double start = 0, stop = -1;             // frames; stop < 0 = end of file
    enum Loop { Off, Always, Sustain } loop = Off;
    double loopStart = 0, loopStop = 0, loopFade = 0;
    bool reverse = false, roundRobin = false;
};

bool parseMultisample(const std::string &xml, std::vector<Zone> &zones, std::string &err) {
    Zone *z = nullptr;
    for (const auto &t : tags(xml)) {
        if (t.name == "sample" && !t.closing) {
            zones.emplace_back();
            z = &zones.back();
            auto it = t.attrs.find("file");
            if (it == t.attrs.end()) { err = "multisample.xml: <sample> without file"; return false; }
            z->file = it->second;
            z->gainDb = num(t.attrs, "gain", 0);
            z->tune = num(t.attrs, "tune", 0);
            z->start = num(t.attrs, "sample-start", 0);
            z->stop = num(t.attrs, "sample-stop", -1);
            z->reverse = num(t.attrs, "reverse", 0) > 0;
            auto zl = t.attrs.find("zone-logic");
            z->roundRobin = zl != t.attrs.end() && zl->second == "round-robin";
            if (t.selfClosing) z = nullptr;
        } else if (t.name == "sample" && t.closing) {
            z = nullptr;
        } else if (z && t.name == "key") {
            z->root = (int)num(t.attrs, "root", 60);
            z->keyLow = (int)num(t.attrs, "low", 0);
            z->keyHigh = (int)num(t.attrs, "high", 127);
            z->keyTrack = num(t.attrs, "track", 1);
            z->tune += num(t.attrs, "tune", 0);
        } else if (z && t.name == "velocity") {
            z->velLow = (int)num(t.attrs, "low", 0);
            z->velHigh = (int)num(t.attrs, "high", 127);
            z->velLowFade = (int)num(t.attrs, "low-fade", 0);
            z->velHighFade = (int)num(t.attrs, "high-fade", 0);
        } else if (z && t.name == "select") {
            z->selLow = (int)num(t.attrs, "low", 0);
            z->selHigh = (int)num(t.attrs, "high", 127);
        } else if (z && t.name == "loop") {
            auto m = t.attrs.find("mode");
            const std::string mode = m == t.attrs.end() ? "off" : m->second;
            z->loop = mode == "loop" ? Zone::Always : mode == "sustain" ? Zone::Sustain : Zone::Off;
            z->loopStart = num(t.attrs, "start", 0);
            z->loopStop = num(t.attrs, "stop", 0);
            z->loopFade = std::clamp(num(t.attrs, "fade", 0), 0.0, 1.0);
        }
    }
    if (zones.empty()) { err = "multisample.xml has no samples"; return false; }
    return true;
}

// ---- library index ------------------------------------------------------------------------
std::string home() { const char *h = getenv("HOME"); return h ? h : ""; }

int gmKeyFor(const std::string &file, std::set<int> &taken) {
    const std::string n = lower(fs::path(file).stem().string());
    std::vector<std::string> tok;
    std::string cur;
    for (char c : n) { if (isalnum((unsigned char)c)) cur += c; else { if (!cur.empty()) tok.push_back(cur); cur.clear(); } }
    if (!cur.empty()) tok.push_back(cur);
    auto has = [&](std::initializer_list<const char *> words) {
        for (const char *w : words) {
            if (strlen(w) <= 3) { for (auto &t : tok) if (t == w) return true; }
            else if (n.find(w) != std::string::npos) return true;
        }
        return false;
    };
    auto pick = [&](std::initializer_list<int> keys) {
        for (int k : keys) if (!taken.count(k)) { taken.insert(k); return k; }
        return -1;
    };
    const bool hat = has({"hat", "hh", "hihat", "chh", "ohh", "oh", "ch"});
    if (hat && has({"open", "ohh", "oh"})) return pick({46});
    if (hat && has({"pedal", "foot"})) return pick({44});
    if (hat) return pick({42, 44});
    if (has({"crash"})) return pick({49, 57});
    if (has({"ride"})) return has({"bell"}) ? pick({53}) : pick({51, 59});
    if (has({"china"})) return pick({52});
    if (has({"splash"})) return pick({55});
    if (has({"cymbal", "cym"})) return pick({49, 57, 55});
    if (has({"kick", "bd", "bassdrum", "bass drum", "kck"})) return pick({36, 35});
    if (has({"rim", "rimshot", "sidestick", "side stick", "rs"})) return pick({37});
    if (has({"clap", "cp", "clp"})) return pick({39});
    if (has({"snare", "sd", "snr"})) return pick({38, 40});
    if (has({"tom"})) {
        if (has({"floor", "low", "lo", "lt"})) return pick({45, 41, 43});
        if (has({"high", "hi", "ht"})) return pick({50, 48});
        return pick({47, 48, 45, 50});
    }
    if (has({"cowbell", "cow"})) return pick({56});
    if (has({"tambourine", "tamb", "tambo"})) return pick({54});
    if (has({"shaker", "maraca", "maracas"})) return pick({70, 82});
    if (has({"conga"})) return has({"high", "hi"}) ? pick({62, 63}) : pick({63, 64, 62});
    if (has({"bongo"})) return has({"high", "hi"}) ? pick({60, 61}) : pick({61, 60});
    if (has({"clave", "claves"})) return pick({75});
    if (has({"block", "wood"})) return pick({76, 77});
    if (has({"agogo"})) return pick({67, 68});
    if (has({"cabasa"})) return pick({69});
    if (has({"timbale"})) return pick({65, 66});
    if (has({"guiro"})) return pick({73, 74});
    if (has({"whistle"})) return pick({71, 72});
    if (has({"triangle"})) return pick({81, 80});
    return -1;
}

bool isWav(const fs::path &p) { return lower(p.extension().string()) == ".wav"; }

std::vector<std::string> wavsIn(const std::string &dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && isWav(e.path())) out.push_back(e.path().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool looksLikeKit(const std::vector<std::string> &wavs) {
    if (wavs.size() < 4 || wavs.size() > 400) return false;
    std::set<int> taken;
    int roles = 0;
    for (auto &w : wavs) if (gmKeyFor(w, taken) >= 0) ++roles;
    return taken.count(36) || taken.count(35) ? roles >= 3 : false;
}

std::string multisampleCategory(const std::string &xml) {
    const auto a = xml.find("<category>"), b = xml.find("</category>");
    return a != std::string::npos && b > a ? xml.substr(a + 10, b - a - 10) : "";
}

std::string resolveIn(const std::string &name, const std::string &baseDir) {
    fs::path p(name);
    if (p.is_absolute()) return fs::exists(p) ? p.string() : "";
    if (!baseDir.empty() && fs::exists(fs::path(baseDir) / p)) return (fs::path(baseDir) / p).string();
    for (auto &root : sampleRoots()) if (fs::exists(fs::path(root) / p)) return (fs::path(root) / p).string();
    return "";
}

// find a library entry by exact name, then by path suffix, then by unique substring
bool findEntry(const std::string &kind, const std::string &query, std::string &path, std::string &err) {
    const auto &lib = sampleLibrary();
    const std::string q = lower(query);
    for (auto &e : lib) if (e.kind == kind && lower(e.name) == q) { path = e.path; return true; }
    for (auto &e : lib) {
        const std::string lp = lower(e.path);
        if (e.kind == kind && lp.size() >= q.size() && lp.compare(lp.size() - q.size(), q.size(), q) == 0) { path = e.path; return true; }
    }
    std::vector<const SampleLibraryEntry *> hits;
    std::set<std::string> names;
    for (auto &e : lib) if (e.kind == kind && lower(e.name).find(q) != std::string::npos && names.insert(e.name).second) hits.push_back(&e);
    if (hits.size() == 1) { path = hits[0]->path; return true; }
    err = hits.empty() ? "no " + kind + " named '" + query + "'" : "'" + query + "' matches " + std::to_string(hits.size()) + " " + kind + "s";
    if (!hits.empty()) {
        err += ": ";
        for (size_t i = 0; i < hits.size() && i < 6; ++i) err += (i ? ", " : "") + hits[i]->name;
        if (hits.size() > 6) err += ", ...";
    }
    err += " (run `wavelength samples --search <text>`)";
    return false;
}

// ---- playback -----------------------------------------------------------------------------
struct Voice {
    const Zone *zone;
    std::shared_ptr<SampleData> data;
    size_t startFrame;
    double noteLen;     // seconds until note-off (inf for one-shots)
    double cutAt;       // seconds until a choke cuts the voice (inf = never)
    double amp, ratio;
};

inline float cubic(const std::vector<float> &x, double pos) {
    const long i = (long)pos;
    const float f = (float)(pos - i);
    const long n = (long)x.size();
    auto at = [&](long k) { return x[(size_t)std::clamp(k, 0L, n - 1)]; };
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void play(const Voice &v, Audio &out, double sr, double attack, double release) {
    const Zone &z = *v.zone;
    const SampleData &s = *v.data;
    const double stop = z.stop > 0 ? std::min<double>(z.stop, (double)s.frames()) : (double)s.frames();
    const double loopLen = z.loopStop - z.loopStart;
    const bool canLoop = z.loop != Zone::Off && loopLen > 16 && z.loopStop <= stop && !z.reverse;
    const double fadeLen = canLoop ? std::min(z.loopFade * loopLen, z.loopStart) : 0;
    const bool stereo = !s.r.empty();
    double pos = z.start;
    const double chokeFade = 0.004;
    for (size_t i = 0;; ++i) {
        const size_t idx = v.startFrame + i;
        if (idx >= out.frames()) break;
        const double t = i / sr;
        double env = attack > 0 ? std::min(1.0, t / attack) : 1.0;
        if (t > v.noteLen) {
            if (release <= 0) break;
            const double r = 1.0 - (t - v.noteLen) / release;
            if (r <= 0) break;
            env *= r * r;
        }
        if (t > v.cutAt) {
            const double c = 1.0 - (t - v.cutAt) / chokeFade;
            if (c <= 0) break;
            env *= c;
        }
        const bool looping = canLoop && (z.loop == Zone::Always || t <= v.noteLen);
        if (looping) while (pos >= z.loopStop) pos -= loopLen;
        if (pos >= stop) break;
        const double rp = z.reverse ? stop - 1 - (pos - z.start) : pos;
        if (rp < 0) break;
        float l = cubic(s.l, rp), r = stereo ? cubic(s.r, rp) : l;
        if (looping && fadeLen > 0 && pos >= z.loopStop - fadeLen) {
            const float w = (float)((pos - (z.loopStop - fadeLen)) / fadeLen);
            const double alt = pos - loopLen;
            l = l * (1 - w) + cubic(s.l, alt) * w;
            r = r * (1 - w) + (stereo ? cubic(s.r, alt) : cubic(s.l, alt)) * w;
        }
        const double g = v.amp * env;
        out.left[idx] += (float)(l * g);
        out.right[idx] += (float)(r * g);
        pos += v.ratio;
    }
}

} // namespace

std::vector<std::string> sampleRoots() {
    std::vector<std::string> roots;
    if (const char *env = getenv("WAVELENGTH_SAMPLES_PATH")) {
        std::string s = env;
        for (size_t a = 0, b; a <= s.size(); a = b + 1) {
            b = s.find(':', a);
            if (b == std::string::npos) b = s.size();
            if (b > a) roots.push_back(s.substr(a, b - a));
        }
    }
    // Bitwig Studio's installed sound content, newest package format first
    const fs::path bitwig = fs::path(home()) / "Library/Application Support/Bitwig/Bitwig Studio/installed-packages";
    std::error_code ec;
    std::vector<std::string> versions;
    for (auto &e : fs::directory_iterator(bitwig, ec)) if (e.is_directory(ec)) versions.push_back(e.path().string());
    std::sort(versions.rbegin(), versions.rend());
    roots.insert(roots.end(), versions.begin(), versions.end());
    const fs::path userLib = fs::path(home()) / "Documents/Bitwig Studio/Library";
    if (fs::exists(userLib, ec)) roots.push_back(userLib.string());
    return roots;
}

const std::vector<SampleLibraryEntry> &sampleLibrary() {
    static std::vector<SampleLibraryEntry> lib;
    static std::once_flag once;
    std::call_once(once, [] {
        std::set<std::string> seen;   // the same package appears under several format versions
        for (auto &root : sampleRoots()) {
            std::error_code ec;
            std::map<std::string, std::vector<std::string>> dirWavs;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const auto &p = it->path();
                const std::string ext = lower(p.extension().string());
                if (ext == ".multisample") {
                    const std::string name = p.stem().string();
                    if (!seen.insert("m:" + name).second) continue;
                    SampleLibraryEntry e{"multisample", name, p.string(), "", 0};
                    Zip z;
                    std::string err;
                    std::vector<uint8_t> x;
                    if (z.open(p.string(), err) && z.read("multisample.xml", x, err)) {
                        const std::string xml(x.begin(), x.end());
                        e.category = multisampleCategory(xml);
                        for (size_t q = 0; (q = xml.find("<sample ", q)) != std::string::npos; ++q) ++e.count;
                    }
                    lib.push_back(e);
                } else if (ext == ".wav") {
                    dirWavs[p.parent_path().string()].push_back(p.string());
                }
            }
            for (auto &[dir, wavs] : dirWavs) {
                std::sort(wavs.begin(), wavs.end());
                if (!looksLikeKit(wavs)) continue;
                const fs::path d(dir);
                const std::string name = d.filename().string();
                if (!seen.insert("k:" + d.parent_path().filename().string() + "/" + name).second) continue;
                lib.push_back({"kit", name, dir, d.parent_path().filename().string(), wavs.size()});
            }
        }
        std::sort(lib.begin(), lib.end(), [](auto &a, auto &b) { return a.kind != b.kind ? a.kind < b.kind : lower(a.name) < lower(b.name); });
    });
    return lib;
}

bool kitMap(const std::string &nameOrPath, const std::string &baseDir, std::vector<std::pair<int, std::string>> &map,
            std::vector<std::string> &unmapped, std::string &resolved, std::string &err) {
    resolved = resolveIn(nameOrPath, baseDir);
    if (resolved.empty() || !fs::is_directory(resolved)) {
        if (!findEntry("kit", nameOrPath, resolved, err)) return false;
    }
    std::set<int> taken;
    for (auto &w : wavsIn(resolved)) {
        const int k = gmKeyFor(w, taken);
        if (k >= 0) map.push_back({k, w}); else unmapped.push_back(w);
    }
    // anything unrecognised goes on the free keys from 60 up
    int next = 60;
    for (auto &w : unmapped) {
        while (taken.count(next) && next < 127) ++next;
        taken.insert(next);
        map.push_back({next, w});
    }
    std::sort(map.begin(), map.end());
    if (map.empty()) { err = resolved + " has no WAV files"; return false; }
    return true;
}

bool renderSampler(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err) {
    const json &cfg = track.sampler;
    if (!cfg.is_object()) { err = "track '" + track.name + "': builtin:sampler needs a \"sampler\" object (multisample, kit or sample)"; return false; }
    static const std::set<std::string> known = {"multisample", "kit", "map", "sample", "root", "attack", "release", "oneShot",
                                                "select", "transpose", "velocity", "choke", "gain"};
    for (auto &[k, v] : cfg.items()) if (!known.count(k)) warnings.push_back("sampler: unknown setting '" + k + "'");

    std::vector<Zone> zones;
    std::unique_ptr<Zip> zip;
    std::string source;
    bool isKit = false;
    if (cfg.contains("multisample")) {
        const std::string q = cfg["multisample"].get<std::string>();
        std::string path = resolveIn(q, job.baseDir);
        if (path.empty() && !findEntry("multisample", q, path, err)) return false;
        std::string xml;
        if (fs::is_directory(path)) {
            std::vector<uint8_t> x;
            if (!readFile((fs::path(path) / "multisample.xml").string(), x)) { err = path + " has no multisample.xml"; return false; }
            xml.assign(x.begin(), x.end());
            source = path;
        } else {
            zip = std::make_unique<Zip>();
            std::vector<uint8_t> x;
            if (!zip->open(path, err) || !zip->read("multisample.xml", x, err)) return false;
            xml.assign(x.begin(), x.end());
            source = path;
        }
        if (!parseMultisample(xml, zones, err)) { err = fs::path(path).filename().string() + ": " + err; return false; }
    } else if (cfg.contains("kit") || cfg.contains("map")) {
        isKit = true;
        std::vector<std::pair<int, std::string>> map;
        std::string dir;
        if (cfg.contains("kit") && cfg["kit"].is_string()) {
            std::vector<std::string> unmapped;
            if (!kitMap(cfg["kit"].get<std::string>(), job.baseDir, map, unmapped, dir, err)) return false;
        }
        json explicitMap = cfg.contains("kit") && cfg["kit"].is_object() ? cfg["kit"] : cfg.value("map", json::object());
        for (auto &[k, v] : explicitMap.items()) {
            const int key = std::atoi(k.c_str());
            std::string file = v.get<std::string>();
            std::string path = !dir.empty() && fs::exists(fs::path(dir) / file) ? (fs::path(dir) / file).string() : resolveIn(file, job.baseDir);
            if (path.empty()) { err = "sampler: cannot find kit sample '" + file + "'"; return false; }
            map.erase(std::remove_if(map.begin(), map.end(), [&](auto &m) { return m.first == key; }), map.end());
            map.push_back({key, path});
        }
        for (auto &[key, file] : map) {
            Zone z;
            z.file = file; z.keyLow = z.keyHigh = z.root = key; z.keyTrack = 0;
            zones.push_back(z);
        }
        source = dir;
    } else if (cfg.contains("sample")) {
        const std::string f = cfg["sample"].get<std::string>();
        Zone z;
        z.file = resolveIn(f, job.baseDir);
        if (z.file.empty()) { err = "sampler: cannot find sample '" + f + "'"; return false; }
        z.root = cfg.value("root", 60);
        zones.push_back(z);
    } else {
        err = "track '" + track.name + "': the sampler needs \"multisample\", \"kit\"/\"map\" or \"sample\"";
        return false;
    }

    const double sr = job.sampleRate;
    const bool oneShot = cfg.value("oneShot", isKit);
    const double attack = cfg.value("attack", isKit ? 0.0 : 0.002);
    const double release = cfg.value("release", isKit ? 0.05 : 0.25);
    const int select = std::clamp(cfg.value("select", 0), 0, 127);
    const double transpose = cfg.value("transpose", 0.0);
    const double velSens = std::clamp(cfg.value("velocity", 1.0), 0.0, 1.0);
    const double gainDb = cfg.value("gain", 0.0);
    std::vector<std::set<int>> chokes;
    if (cfg.contains("choke")) for (auto &g : cfg["choke"]) chokes.push_back(g.get<std::set<int>>());
    else if (isKit) chokes.push_back({42, 44, 46});   // closed and pedal hats cut the open hat

    std::map<std::string, std::shared_ptr<SampleData>> cache;
    auto load = [&](const std::string &file, std::shared_ptr<SampleData> &outData) -> bool {
        auto it = cache.find(file);
        if (it != cache.end()) { outData = it->second; return true; }
        std::vector<uint8_t> bytes;
        std::string e2;
        if (zip) { if (!zip->read(file, bytes, e2)) { err = e2; return false; } }
        else {
            const std::string p = fs::path(file).is_absolute() ? file : (fs::path(source) / file).string();
            if (!readFile(p, bytes)) { err = "cannot read " + p; return false; }
        }
        auto d = std::make_shared<SampleData>();
        if (!decodeWav(bytes, *d, e2)) { err = fs::path(file).filename().string() + ": " + e2; return false; }
        cache[file] = outData = d;
        return true;
    };

    std::map<int, size_t> rr;   // round-robin position per key
    size_t silent = 0;
    for (size_t ni = 0; ni < track.notes.size(); ++ni) {
        const Note &n = track.notes[ni];
        const int vel127 = std::clamp((int)std::lround(n.velocity * 127), 1, 127);
        auto velOk = [&](const Zone &z) { return vel127 >= z.velLow && vel127 <= z.velHigh; };
        auto selOk = [&](const Zone &z) { return select >= z.selLow && select <= z.selHigh; };
        std::vector<const Zone *> hit;
        for (auto &z : zones) if (n.key >= z.keyLow && n.key <= z.keyHigh && velOk(z) && selOk(z)) hit.push_back(&z);
        if (hit.empty() && !isKit) {
            // no zone covers this key: stretch the zones with the nearest root (velocity first, then any)
            for (int pass = 0; pass < 2 && hit.empty(); ++pass) {
                int best = 1000;
                for (auto &z : zones) if ((pass || velOk(z)) && selOk(z)) best = std::min(best, std::abs(z.root - n.key));
                for (auto &z : zones) if ((pass || velOk(z)) && selOk(z) && std::abs(z.root - n.key) == best) hit.push_back(&z);
            }
        }
        if (hit.empty()) { ++silent; continue; }
        std::vector<const Zone *> play1, robin;
        for (auto *z : hit) (z->roundRobin ? robin : play1).push_back(z);
        if (!robin.empty()) play1.push_back(robin[rr[n.key]++ % robin.size()]);

        double cutAt = INFINITY;
        if (oneShot)
            for (auto &grp : chokes)
                if (grp.count(n.key))
                    for (size_t nj = 0; nj < track.notes.size(); ++nj) {
                        const Note &m = track.notes[nj];
                        if (nj != ni && grp.count(m.key) && m.key != n.key && m.start > n.start + 1e-6 && m.start - n.start < cutAt)
                            cutAt = m.start - n.start;
                    }
        const double velDb = velSens * 24 * std::log10(std::max(n.velocity, 0.01));
        for (auto *z : play1) {
            Voice v;
            v.zone = z;
            if (!load(z->file, v.data)) return false;
            double w = 1;
            if (z->velLowFade > 0 && vel127 < z->velLow + z->velLowFade) w *= (vel127 - z->velLow + 1.0) / (z->velLowFade + 1.0);
            if (z->velHighFade > 0 && vel127 > z->velHigh - z->velHighFade) w *= (z->velHigh - vel127 + 1.0) / (z->velHighFade + 1.0);
            v.amp = w * std::pow(10.0, (z->gainDb + gainDb + velDb) / 20);
            v.ratio = std::pow(2.0, (z->keyTrack * (n.key - z->root) + z->tune + transpose) / 12) * v.data->rate / sr;
            v.startFrame = (size_t)std::llround(n.start * sr);
            v.noteLen = oneShot ? INFINITY : n.length;
            v.cutAt = cutAt;
            play(v, out, sr, attack, release);
        }
    }
    if (silent) warnings.push_back(std::to_string(silent) + " note(s) matched no sample zone");
    return true;
}

} // namespace wl
