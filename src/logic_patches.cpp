#include "logic_patches.hpp"

#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace wl {

namespace {
std::string lower(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// instruments by the names channel strips give them; anything else in a slot is an effect
const std::set<std::string> &synthNames() {
    static const std::set<std::string> s = {"Alchemy", "Retro Synth", "ES2", "ES1", "ES E", "ES M", "ES P", "EFM1", "Sculpture",
                                            "Vintage B3", "Clav", "E-Piano", "Ultrabeat", "Church Orga", "Klopfgeist", "EVOC PS",
                                            "Mellotron", "Studio Piano", "Studio Strin", "Studio Horns", "Drum Machin", "Quick Sampl"};
    return s;
}

struct Record { size_t at, size; std::string name; };

// The plugin records of a channel strip: where each payload is, its size and its plugin's name
bool records(const std::vector<uint8_t> &d, std::vector<Record> &out) {
    const uint8_t magic[4] = {'U', 'C', 'u', 'A'};
    const auto first = std::search(d.begin(), d.begin() + (long)std::min<size_t>(d.size(), 4096), magic, magic + 4);
    for (size_t pos = (size_t)(first - d.begin()); pos + 36 <= d.size() && !std::memcmp(&d[pos], "UCuA", 4);) {
        const uint32_t n = le32(&d[pos + 0x1c]);
        Record r{pos + 36, n, ""};
        const uint8_t *pl = &d[pos + 36];
        if (n >= 140 && pos + 36 + 140 <= d.size() && (!std::memcmp(pl + 132, "MELC", 4) || !std::memcmp(pl + 132, "GAME", 4)))
            for (size_t k = 0; k < 12 && pl[120 + k]; ++k) r.name += (char)pl[120 + k];
        out.push_back(r);
        pos += 36 + (size_t)n;
    }
    return !out.empty();
}

bool readWhole(const std::string &path, std::vector<uint8_t> &d) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    d.resize((size_t)in.tellg());
    in.seekg(0);
    return (bool)in.read(reinterpret_cast<char *>(d.data()), (std::streamsize)d.size());
}

// the instrument record of a channel strip: the first record named as an instrument
const Record *instrumentOf(const std::vector<Record> &recs) {
    for (auto &r : recs)
        if (isSamplerInstrument(r.name) || synthNames().count(r.name)) return &r;
    return nullptr;
}

std::vector<std::string> channelFiles(const std::string &patchDir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(patchDir, ec))
        if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".cst") files.push_back(e.path().string());
    std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
        const bool ra = fs::path(a).filename() == "#Root.cst", rb = fs::path(b).filename() == "#Root.cst";
        return ra != rb ? ra : a < b;
    });
    return files;
}
} // namespace

bool isSamplerInstrument(const std::string &name) { return name == "Sampler" || name == "EXS24" || name == "Drum Kit"; }

std::vector<std::string> logicPatchRoots() {
    std::vector<std::string> out;
#if defined(__APPLE__)
    std::error_code ec;
    for (const fs::path &p : {fs::path("/Applications/GarageBand.app/Contents/Resources/Patches/Instrument"),
                              fs::path("/Applications/Logic Pro.app/Contents/Resources/Patches/Instrument"),
                              fs::path("/Applications/Logic Pro X.app/Contents/Resources/Patches/Instrument"),
                              fs::path("/Library/Application Support/Logic/Patches/Instrument"),
                              platform::homeDir() / "Music/Audio Music Apps/Patches/Instrument"})
        if (fs::is_directory(p, ec)) out.push_back(p.string());
#endif
    for (auto &p : platform::envPathList("WAVELENGTH_LOGIC_PATCHES")) out.push_back(p);
    return out;
}

const std::vector<LogicPatch> &logicPatches() {
    static std::vector<LogicPatch> patches;
    static std::once_flag once;
    std::call_once(once, [] {
        for (auto &root : logicPatchRoots()) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_directory(ec) || lower(it->path().extension().string()) != ".patch") continue;
                it.disable_recursion_pending();
                LogicPatch p;
                p.name = it->path().stem().u8string();
                p.path = it->path().string();
                const fs::path rel = it->path().lexically_relative(root);
                p.category = rel.begin() != rel.end() && std::next(rel.begin()) != rel.end() ? rel.begin()->u8string() : "";
                for (auto &f : channelFiles(p.path)) {
                    std::vector<uint8_t> d;
                    std::vector<Record> recs;
                    if (!readWhole(f, d) || !records(d, recs)) continue;
                    const Record *r = instrumentOf(recs);
                    if (!r) continue;
                    if (p.instrument.empty() || (!p.sampler && isSamplerInstrument(r->name))) p.instrument = r->name;
                    p.sampler |= isSamplerInstrument(r->name);
                }
                if (!p.instrument.empty()) patches.push_back(p);
            }
        }
        std::stable_sort(patches.begin(), patches.end(), [](const LogicPatch &a, const LogicPatch &b) { return lower(a.name) < lower(b.name); });
    });
    return patches;
}

const LogicPatch *logicPatchNamed(const std::string &name) {
    std::string q = lower(name);
    if (q.size() > 6 && q.compare(q.size() - 6, 6, ".patch") == 0) q.resize(q.size() - 6);
    for (auto &p : logicPatches()) if (lower(p.name) == q) return &p;
    return nullptr;
}

bool readPatchChannels(const std::string &patchDir, std::vector<PatchChannel> &out, std::string &err) {
    out.clear();
    for (auto &f : channelFiles(patchDir)) {
        std::vector<uint8_t> d;
        std::vector<Record> recs;
        if (!readWhole(f, d) || !records(d, recs)) continue;
        PatchChannel c;
        c.file = f;
        const Record *r = instrumentOf(recs);
        if (r) c.instrument = r->name;
        c.sampler = r && isSamplerInstrument(r->name);
        if (c.sampler) {
            c.data.swap(d);
            const size_t a = r->at, b = std::min(c.data.size(), r->at + r->size);
            // an instrument stored inside: the first EXS chunk header (its magic sits 16 bytes in)
            for (size_t p = a; p + 20 <= b; ++p)
                if ((c.data[p] == 'T' && !std::memcmp(&c.data[p], "TBOS", 4)) || (c.data[p] == 'S' && !std::memcmp(&c.data[p], "SOBT", 4)) ||
                    (c.data[p] == 'J' && !std::memcmp(&c.data[p], "JBOS", 4)) || (c.data[p] == 'S' && !std::memcmp(&c.data[p], "SOBJ", 4))) {
                    if (p >= a + 16) c.exsAt = p - 16;
                    break;
                }
            // else the instrument it names: "MELC" "PMAS" + 4 bytes + "<name>.exs"
            if (!c.exsAt)
                for (size_t p = a; p + 12 < b; ++p)
                    if (!std::memcmp(&c.data[p], "MELCPMAS", 8)) {
                        std::string n;
                        for (size_t k = p + 12; k < b && c.data[k] >= 32 && c.data[k] < 127; ++k) n += (char)c.data[k];
                        if (lower(fs::path(n).extension().string()) == ".exs") { c.exs = n; break; }
                    }
        }
        out.push_back(std::move(c));
    }
    if (out.empty()) { err = patchDir + " has no channel strips"; return false; }
    return true;
}

} // namespace wl
