#include "apple_loops.hpp"

#include "harmony.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace wl {

namespace {
uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
std::string lower(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }

// 29819273-B5BF-4AEF-B78D-62D1EF90BB2C: Apple's "information" uuid chunk (u32 count, then that many
// NUL-terminated key/value string pairs)
const uint8_t kTagsUuid[16] = {0x29, 0x81, 0x92, 0x73, 0xB5, 0xBF, 0x4A, 0xEF, 0xB7, 0x8D, 0x62, 0xD1, 0xEF, 0x90, 0xBB, 0x2C};

// "Bb" -> 10; the letter and an optional sharp or flat (a stray letter after it, as in Apple's "Bc", is ignored)
int pitchClass(const std::string &k) {
    static const int letters[7] = {9, 11, 0, 2, 4, 5, 7};   // A..G
    if (k.empty() || std::toupper((unsigned char)k[0]) < 'A' || std::toupper((unsigned char)k[0]) > 'G') return -1;
    int pc = letters[std::toupper((unsigned char)k[0]) - 'A'];
    if (k.size() > 1 && k[1] == '#') ++pc;
    else if (k.size() > 1 && k[1] == 'b') --pc;
    return (pc + 12) % 12;
}
} // namespace

bool readAppleLoop(const std::string &path, AppleLoop &out, std::string &err) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) { err = "cannot read " + path; return false; }
    in.seekg(0, std::ios::end);
    const uint64_t size = (uint64_t)in.tellg();
    in.seekg(0);
    uint8_t h[12];
    if (!in.read(reinterpret_cast<char *>(h), 8) || std::memcmp(h, "caff", 4)) { err = "not a CAF file"; return false; }
    out = AppleLoop{};
    out.path = path;
    out.name = fs::u8path(path).stem().u8string();
    out.folder = fs::u8path(path).parent_path().filename().u8string();
    double rate = 0;
    uint32_t bytesPerPacket = 0, framesPerPacket = 0;
    int64_t validFrames = -1, dataBytes = -1;
    bool tags = false;
    for (uint64_t pos = 8; pos + 12 <= size;) {
        in.seekg((std::streamoff)pos);
        if (!in.read(reinterpret_cast<char *>(h), 12)) break;
        const int64_t len = (int64_t)be64(h + 4);
        const uint64_t body = pos + 12, n = len < 0 ? size - body : std::min<uint64_t>((uint64_t)len, size - body);
        auto read = [&](std::vector<uint8_t> &b) { b.resize((size_t)n); in.seekg((std::streamoff)body); return (bool)in.read(reinterpret_cast<char *>(b.data()), (std::streamsize)n); };
        std::vector<uint8_t> b;
        if (!std::memcmp(h, "desc", 4) && n >= 32 && read(b)) {
            uint64_t bits = be64(b.data());
            std::memcpy(&rate, &bits, 8);
            bytesPerPacket = be32(b.data() + 16);
            framesPerPacket = be32(b.data() + 20);
        } else if (!std::memcmp(h, "pakt", 4) && n >= 24 && read(b)) {
            validFrames = (int64_t)be64(b.data() + 8);
        } else if (!std::memcmp(h, "data", 4)) {
            dataBytes = (int64_t)n - 4;
        } else if (!std::memcmp(h, "midi", 4)) {
            out.midi = n >= 14;
        } else if (!std::memcmp(h, "uuid", 4) && n >= 20 && n < (1u << 20) && read(b) && !std::memcmp(b.data(), kTagsUuid, 16)) {
            tags = true;
            std::vector<std::string> s;
            for (size_t i = 20; i < b.size();) {
                const size_t e = std::find(b.begin() + (long)i, b.end(), 0) - b.begin();
                s.emplace_back(reinterpret_cast<const char *>(b.data()) + i, e - i);
                i = e + 1;
            }
            for (size_t i = 0; i + 1 < s.size(); i += 2) {
                const std::string &k = s[i], &v = s[i + 1];
                if (k == "beat count") out.beats = std::max(0, std::atoi(v.c_str()));
                else if (k == "key signature") out.key = v;
                else if (k == "key type") out.scale = v;
                else if (k == "time signature") out.timeSignature = v;
                else if (k == "category") out.category = v;
                else if (k == "subcategory") out.subcategory = v;
                else if (k == "genre") out.genre = v;
                else if (k == "descriptors")
                    for (size_t a = 0; a <= v.size();) {
                        const size_t c = std::min(v.find(',', a), v.size());
                        if (c > a) out.descriptors.push_back(v.substr(a, c - a));
                        a = c + 1;
                    }
            }
        }
        if (len < 0) break;
        pos = body + (uint64_t)len;
    }
    if (!tags) { err = "not an Apple Loop (a CAF without Apple Loop tags)"; return false; }
    // the key as a note name: a letter and an optional sharp or flat (Apple spells a few "Bc": B)
    if (pitchClass(out.key) < 0) out.key.clear();
    else out.key = out.key.substr(0, out.key.size() > 1 && (out.key[1] == '#' || out.key[1] == 'b') ? 2 : 1);
    if (out.key.empty()) out.scale.clear();
    // the audio's length: the valid frames of a compressed file (packet table), else PCM bytes / frame size
    if (rate > 0) {
        if (validFrames >= 0) out.seconds = (double)validFrames / rate;
        else if (dataBytes > 0 && bytesPerPacket > 0 && framesPerPacket > 0) out.seconds = (double)(dataBytes / bytesPerPacket) * framesPerPacket / rate;
    }
    if (out.beats > 0 && out.seconds > 0) out.bpm = out.beats * 60.0 / out.seconds;
    return true;
}

const AppleLoop *appleLoopAt(const std::string &path) {
    static std::mutex m;
    static std::map<std::string, std::unique_ptr<AppleLoop>> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(path);
    if (it == cache.end()) {
        auto loop = std::make_unique<AppleLoop>();
        std::string err;
        if (!readAppleLoop(path, *loop, err)) loop.reset();
        it = cache.emplace(path, std::move(loop)).first;
    }
    return it->second.get();
}

std::vector<std::string> appleLoopRoots() {
    std::vector<std::string> out;
#if defined(__APPLE__)
    std::error_code ec;
    for (const fs::path &p : {fs::path("/Library/Audio/Apple Loops"), platform::homeDir() / "Library/Audio/Apple Loops"})
        if (fs::is_directory(p, ec)) out.push_back(p.string());
#endif
    for (auto &p : platform::envPathList("WAVELENGTH_APPLE_LOOPS")) out.push_back(p);
    return out;
}

const std::map<std::string, std::string> &appleLoopFiles() {
    static std::map<std::string, std::string> files;   // lower-case name -> path, the first in path order
    static std::once_flag once;
    std::call_once(once, [] {
        std::vector<std::string> all;
        for (auto &root : appleLoopRoots()) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".caf") all.push_back(it->path().string());
            }
        }
        std::sort(all.begin(), all.end());
        for (auto &f : all) files.emplace(lower(fs::u8path(f).stem().u8string()), f);
    });
    return files;
}

size_t appleLoopCount() { return appleLoopFiles().size(); }

const std::vector<AppleLoop> &appleLoops() {
    static std::vector<AppleLoop> loops;
    static std::once_flag once;
    std::call_once(once, [] {
        for (auto &[name, path] : appleLoopFiles())
            if (const AppleLoop *l = appleLoopAt(path)) loops.push_back(*l);
    });
    return loops;
}

const AppleLoop *findAppleLoop(const std::string &name) {
    std::string q = lower(name);
    if (q.size() > 4 && q.compare(q.size() - 4, 4, ".caf") == 0) q.resize(q.size() - 4);
    auto &files = appleLoopFiles();
    auto it = files.find(q);
    return it == files.end() ? nullptr : appleLoopAt(it->second);
}

bool cafMidi(const std::vector<uint8_t> &d, std::vector<uint8_t> &smf, std::string &err) {
    if (d.size() < 8 || std::memcmp(d.data(), "caff", 4)) { err = "not a CAF file"; return false; }
    for (size_t p = 8; p + 12 <= d.size();) {
        const int64_t len = (int64_t)be64(d.data() + p + 4);
        const size_t body = p + 12, n = len < 0 ? d.size() - body : std::min<size_t>((size_t)len, d.size() - body);
        if (!std::memcmp(d.data() + p, "midi", 4) && n >= 14 && !std::memcmp(d.data() + body, "MThd", 4)) {
            smf.assign(d.begin() + (long)body, d.begin() + (long)(body + n));
            return true;
        }
        if (len < 0) break;
        p = body + (size_t)len;
    }
    err = "has no notes inside (an audio loop: play it as a builtin:audio clip)";
    return false;
}

bool appleLoopShift(const AppleLoop &loop, int tonic, bool minor, int &semitones) {
    int from = pitchClass(loop.key);
    if (from < 0 || tonic < 0) return false;
    int to = tonic;
    // a minor loop into a major key (or the other way): aim for the relative key, so its notes stay in the scale
    const bool loopMinor = loop.scale == "minor", loopMajor = loop.scale == "major";
    if ((loopMinor && !minor) || (loopMajor && minor)) {
        if (loopMinor) from = (from + 3) % 12;
        else to = (to + 3) % 12;
    }
    int s = ((to - from) % 12 + 12) % 12;
    if (s > 5) s -= 12;
    semitones = s;
    return true;
}

bool appleLoopShift(const AppleLoop &loop, const std::string &key, int &semitones, std::string &err) {
    if (loop.key.empty()) { err = "'" + loop.name + "' has no key (a drum or effect loop): it plays as recorded"; return false; }
    int tonic = 0;
    bool minor = false;
    if (!parseKeyName(key, tonic, minor, err)) return false;
    return appleLoopShift(loop, tonic, minor, semitones);
}

} // namespace wl
