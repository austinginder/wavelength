#include "sf2.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

namespace wl {

namespace {
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string name20(const uint8_t *p) {
    std::string s(reinterpret_cast<const char *>(p), 20);
    s = s.substr(0, s.find('\0'));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// generator numbers (SoundFont 2.04, section 8.1.2)
enum : uint16_t {
    StartOffset = 0, EndOffset = 1, StartLoopOffset = 2, EndLoopOffset = 3, StartCoarse = 4, FilterFc = 8, FilterQ = 9,
    ModEnvToFc = 11, EndCoarse = 12, Pan = 17, DelayMod = 25, AttackMod = 26, HoldMod = 27, DecayMod = 28, SustainMod = 29, ReleaseMod = 30,
    KeyToModHold = 31, KeyToModDecay = 32, KeyToVolHold = 39, KeyToVolDecay = 40, DelayVol = 33, AttackVol = 34, HoldVol = 35, DecayVol = 36, SustainVol = 37, ReleaseVol = 38,
    Instrument = 41, KeyRange = 43, VelRange = 44, StartLoopCoarse = 45, Attenuation = 48, EndLoopCoarse = 50, CoarseTune = 51,
    FineTune = 52, SampleID = 53, SampleModes = 54, ScaleTuning = 56, ExclusiveClass = 57, RootKey = 58, GenCount = 61
};

// generator values of one zone: set flags + values (ranges packed lo | hi << 8)
struct GenSet {
    std::array<int32_t, GenCount> v{};
    std::array<bool, GenCount> set{};
    void put(uint16_t op, uint16_t amount) {
        if (op >= GenCount) return;
        v[op] = (op == KeyRange || op == VelRange) ? (int32_t)amount : (int32_t)(int16_t)amount;
        set[op] = true;
    }
    int32_t get(uint16_t op, int32_t def) const { return set[op] ? v[op] : def; }
};

double timecents(int32_t tc) { return tc <= -12000 ? 0.0 : std::pow(2.0, tc / 1200.0); }

struct ModRec { uint16_t src, dest; int16_t amount; uint16_t amtSrc, trans; };
bool sameMod(const ModRec &a, const ModRec &b) { return a.src == b.src && a.dest == b.dest && a.amtSrc == b.amtSrc; }

// the SoundFont 2.04 default modulators that change the sound of a static note (8.4)
const std::vector<ModRec> kDefaultMods = {
    {0x0502, Attenuation, 960, 0, 0},   // velocity -> attenuation (concave)
    {0x0102, FilterFc, -2400, 0, 0},    // velocity -> filter cutoff
    {0x0587, Attenuation, 960, 0, 0},   // CC7 volume -> attenuation
    {0x058B, Attenuation, 960, 0, 0},   // CC11 expression -> attenuation
};

// a modulator source: controllers at their usual resting values, velocity (0-1) and key as given
// `perNote` says the value depends on the note; `unknown` a source a note can't evaluate
double modSource(uint16_t src, int key, double vel, bool &perNote, bool &unknown) {
    const int idx = src & 0x7F;
    double x;
    if (src & 0x80) {   // MIDI controller
        switch (idx) {
        case 7: x = 100 / 127.0; break;
        case 10: x = 64 / 127.0; break;
        case 11: x = 1; break;
        default: x = 0; break;   // pedals, mod wheel, breath: at rest
        }
    } else {
        switch (idx) {
        case 0: return 1;                       // no controller (amount source): 1
        case 2: x = vel; perNote = true; break;  // note-on velocity
        case 3: x = key / 127.0; perNote = true; break;   // note number
        case 13: case 10: x = 0; break;          // pressure
        case 14: x = 0.5; break;                 // pitch wheel centred
        case 16: x = 2 / 127.0; break;           // pitch wheel sensitivity
        default: unknown = true; return 0;       // links
        }
    }
    if (src & 0x100) x = 1 - x;   // max -> min
    const int type = (src >> 10) & 0x3F;
    auto concave = [](double v) { return v >= 1 ? 1.0 : v <= 0 ? 0.0 : std::min(1.0, -40.0 / 96 * std::log10(1 - v)); };
    if (type == 1) x = concave(x);
    else if (type == 2) x = 1 - concave(1 - x);
    else if (type == 3) x = x >= 0.5 ? 1 : 0;
    if (src & 0x200) x = 2 * x - 1;   // bipolar
    return x;
}
} // namespace

bool SoundFont::open(const std::string &path, std::string &err) {
    path_ = path;
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    uint8_t h[12];
    f.read(reinterpret_cast<char *>(h), 12);
    if (!f || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "sfbk", 4)) { err = path + " is not a SoundFont (sf2/sf3)"; return false; }
    const uint64_t total = le32(h + 4) + 8;
    std::vector<uint8_t> pdta;
    for (uint64_t off = 12; off + 12 <= total;) {
        uint8_t c[12];
        f.seekg((std::streamoff)off);
        f.read(reinterpret_cast<char *>(c), 12);
        if (!f) break;
        const uint32_t n = le32(c + 4);
        if (!std::memcmp(c, "LIST", 4)) {
            if (!std::memcmp(c + 8, "sdta", 4)) {
                for (uint64_t o = off + 12; o + 8 <= off + 8 + n;) {
                    uint8_t s[8];
                    f.seekg((std::streamoff)o);
                    f.read(reinterpret_cast<char *>(s), 8);
                    const uint32_t m = le32(s + 4);
                    if (!std::memcmp(s, "smpl", 4)) { smplOffset_ = o + 8; smplSize_ = m; }
                    if (!std::memcmp(s, "sm24", 4)) { sm24Offset_ = o + 8; sm24Size_ = m; }
                    o += 8 + m + (m & 1);
                }
            } else if (!std::memcmp(c + 8, "pdta", 4)) {
                pdta.resize(n - 4);
                f.read(reinterpret_cast<char *>(pdta.data()), (std::streamsize)pdta.size());
            }
        }
        off += 8 + n + (n & 1);
    }
    if (pdta.empty() || !smplSize_) { err = path + ": no sample data or preset data"; return false; }
    for (size_t o = 0; o + 8 <= pdta.size();) {
        const uint8_t *c = &pdta[o];
        const uint32_t n = le32(c + 4);
        const uint8_t *b = c + 8;
        const size_t cnt46 = n / 46, cnt38 = n / 38, cnt22 = n / 22, cnt4 = n / 4;
        if (o + 8 + n > pdta.size()) break;
        if (!std::memcmp(c, "phdr", 4)) for (size_t i = 0; i < cnt38; ++i) { const uint8_t *r = b + i * 38; phdr_.push_back({name20(r), le16(r + 20), le16(r + 22), le16(r + 24)}); }
        else if (!std::memcmp(c, "pbag", 4)) for (size_t i = 0; i < cnt4; ++i) pbag_.push_back({le16(b + i * 4), le16(b + i * 4 + 2)});
        else if (!std::memcmp(c, "pmod", 4) || !std::memcmp(c, "imod", 4))
            for (size_t i = 0; i < n / 10; ++i) {
                const uint8_t *r = b + i * 10;
                (c[0] == 'p' ? pmod_ : imod_).push_back({le16(r), le16(r + 2), (int16_t)le16(r + 4), le16(r + 6), le16(r + 8)});
            }
        else if (!std::memcmp(c, "pgen", 4)) for (size_t i = 0; i < cnt4; ++i) pgen_.push_back({le16(b + i * 4), le16(b + i * 4 + 2)});
        else if (!std::memcmp(c, "inst", 4)) for (size_t i = 0; i < cnt22; ++i) inst_.push_back({name20(b + i * 22), le16(b + i * 22 + 20)});
        else if (!std::memcmp(c, "ibag", 4)) for (size_t i = 0; i < cnt4; ++i) ibag_.push_back({le16(b + i * 4), le16(b + i * 4 + 2)});
        else if (!std::memcmp(c, "igen", 4)) for (size_t i = 0; i < cnt4; ++i) igen_.push_back({le16(b + i * 4), le16(b + i * 4 + 2)});
        else if (!std::memcmp(c, "shdr", 4))
            for (size_t i = 0; i < cnt46; ++i) {
                const uint8_t *r = b + i * 46;
                shdr_.push_back({name20(r), le32(r + 20), le32(r + 24), le32(r + 28), le32(r + 32), le32(r + 36), r[40], (int8_t)r[41], le16(r + 42), le16(r + 44)});
            }
        o += 8 + n + (n & 1);
    }
    if (phdr_.size() < 2 || inst_.size() < 2 || shdr_.size() < 2) { err = path + ": empty SoundFont"; return false; }
    for (auto &s : shdr_) if (s.type & 0x10) compressed_ = true;
    for (size_t i = 0; i + 1 < phdr_.size(); ++i) presets_.push_back({phdr_[i].name, phdr_[i].bank, phdr_[i].preset});   // the last one is the terminal record
    return true;
}

const Sf2Preset *SoundFont::find(int bank, int program) const {
    for (auto &p : presets_) if (p.bank == bank && p.program == program) return &p;
    if (bank != 128) for (auto &p : presets_) if (p.bank == 0 && p.program == program) return &p;
    if (bank == 128) for (auto &p : presets_) if (p.bank == 128) return &p;   // a drum kit, any
    for (auto &p : presets_) if (p.program == program && p.bank != 128) return &p;
    return nullptr;
}

const Sf2Preset *SoundFont::findByName(const std::string &name) const {
    const std::string q = lower(name);
    for (auto &p : presets_) if (lower(p.name) == q) return &p;
    const Sf2Preset *hit = nullptr;
    for (auto &p : presets_)
        if (lower(p.name).find(q) != std::string::npos) { if (hit) return nullptr; hit = &p; }
    return hit;
}

std::string SoundFont::sampleName(int index) const { return index >= 0 && (size_t)index < shdr_.size() ? shdr_[index].name : ""; }

bool SoundFont::zones(const Sf2Preset &pr, std::vector<Sf2Zone> &out, std::string &err) const {
    size_t pi = 0;
    for (; pi + 1 < phdr_.size(); ++pi) if (phdr_[pi].bank == pr.bank && phdr_[pi].preset == pr.program && phdr_[pi].name == pr.name) break;
    if (pi + 1 >= phdr_.size()) { err = "no preset " + pr.name; return false; }
    auto genSet = [](const std::vector<Bag> &bags, const std::vector<Gen> &gens, size_t bag) {
        GenSet g;
        if (bag + 1 >= bags.size()) return g;
        for (size_t k = bags[bag].gen; k < bags[bag + 1].gen && k < gens.size(); ++k) g.put(gens[k].oper, gens[k].amount);
        return g;
    };
    auto modList = [](const std::vector<Bag> &bags, const std::vector<Mod> &mods, size_t bag) {
        std::vector<ModRec> out;
        if (bag + 1 >= bags.size()) return out;
        for (size_t k = bags[bag].mod; k < bags[bag + 1].mod && k < mods.size(); ++k)
            out.push_back({mods[k].src, mods[k].dest, mods[k].amount, mods[k].amtSrc, mods[k].trans});
        return out;
    };
    // local modulators replace identical ones (same sources and destination) from the level above
    auto overlay = [](std::vector<ModRec> base, const std::vector<ModRec> &top) {
        for (auto &m : top) {
            auto it = std::find_if(base.begin(), base.end(), [&](const ModRec &b) { return sameMod(b, m); });
            if (it != base.end()) *it = m; else base.push_back(m);
        }
        return base;
    };
    GenSet pGlobal;
    std::vector<ModRec> pGlobalMods;
    for (size_t pb = phdr_[pi].bag; pb < phdr_[pi + 1].bag; ++pb) {
        GenSet pz = genSet(pbag_, pgen_, pb);
        const std::vector<ModRec> pMods = overlay(pGlobalMods, modList(pbag_, pmod_, pb));
        if (!pz.set[Instrument] && pb == phdr_[pi].bag) pGlobalMods = pMods;
        if (!pz.set[Instrument]) { if (pb == phdr_[pi].bag) pGlobal = pz; continue; }   // the preset's global zone
        const size_t ii = (size_t)pz.v[Instrument];
        if (ii + 1 >= inst_.size()) continue;
        GenSet iGlobal;
        std::vector<ModRec> iGlobalMods = kDefaultMods;
        for (size_t ib = inst_[ii].bag; ib < inst_[ii + 1].bag; ++ib) {
            GenSet iz = genSet(ibag_, igen_, ib);
            const std::vector<ModRec> iMods = overlay(iGlobalMods, modList(ibag_, imod_, ib));
            if (!iz.set[SampleID]) { if (ib == inst_[ii].bag) { iGlobal = iz; iGlobalMods = iMods; } continue; }
            // modulators: controller-driven ones (at rest) become fixed offsets; velocity- and key-driven ones on
            // the filter, its resonance, the mod envelope depth and the level are kept for each note
            std::array<double, GenCount> modOff{};
            std::vector<Sf2Mod> perNote;
            std::vector<ModRec> all = iMods;
            all.insert(all.end(), pMods.begin(), pMods.end());   // preset modulators add on top
            for (auto &m : all) {
                if (m.dest >= GenCount || ((m.src & 0x7F) == 0 && !(m.src & 0x80))) continue;
                bool note = false, unknown = false;
                const double at0 = m.amount * modSource(m.src, 60, 0.8, note, unknown) * modSource(m.amtSrc, 60, 0.8, note, unknown);
                if (unknown) continue;
                if (!note) modOff[m.dest] += at0;
                else {
                    static const std::set<uint16_t> kept = {FilterFc, FilterQ, ModEnvToFc, Pan, AttackMod, HoldMod, DecayMod, ReleaseMod,
                                                            AttackVol, HoldVol, DecayVol, ReleaseVol, Attenuation, CoarseTune, FineTune};
                    if (kept.count(m.dest)) perNote.push_back({m.src, m.dest, m.amount, m.amtSrc});
                }
            }
            // instrument value: local, else its global zone, else the default
            auto iv = [&](uint16_t op, int32_t def) { return iz.set[op] ? iz.v[op] : iGlobal.get(op, def); };
            // preset values add on top (local, else the preset's global zone)
            auto pv = [&](uint16_t op) { return (pz.set[op] ? pz.v[op] : pGlobal.get(op, 0)) + (int32_t)std::lround(modOff[op]); };
            auto range = [&](uint16_t op, int &lo, int &hi) {
                const int32_t a = iv(op, 127 << 8), b = pz.set[op] ? pz.v[op] : pGlobal.get(op, 127 << 8);
                lo = std::max(a & 0xFF, b & 0xFF);
                hi = std::min((a >> 8) & 0xFF, (b >> 8) & 0xFF);
            };
            Sf2Zone z;
            z.sample = iz.v[SampleID];
            if (z.sample < 0 || (size_t)z.sample + 1 >= shdr_.size()) continue;
            const Shdr &s = shdr_[z.sample];
            range(KeyRange, z.keyLow, z.keyHigh);
            range(VelRange, z.velLow, z.velHigh);
            if (z.keyLow > z.keyHigh || z.velLow > z.velHigh) continue;
            const int rootGen = iv(RootKey, -1);
            z.root = rootGen >= 0 ? rootGen : (s.key <= 127 ? s.key : 60);
            z.tune = iv(CoarseTune, 0) + pv(CoarseTune) + (iv(FineTune, 0) + pv(FineTune) + s.corr) / 100.0;
            z.keyTrack = (iv(ScaleTuning, 100) + pv(ScaleTuning)) / 100.0;
            // the generator counts at 0.4 (E-mu hardware, as FluidSynth does); modulators count in full, per note
            z.attenuationCb = 0.4 * (iv(Attenuation, 0) + pv(Attenuation) - (int32_t)std::lround(modOff[Attenuation])) + modOff[Attenuation];
            z.pan = std::clamp((iv(Pan, 0) + pv(Pan)) / 500.0, -1.0, 1.0);
            const bool comp = s.type & 0x10;
            const double len = comp ? -1 : (double)s.end - s.start;
            const double startOff = iv(StartOffset, 0) + 32768.0 * iv(StartCoarse, 0);
            const double endOff = iv(EndOffset, 0) + 32768.0 * iv(EndCoarse, 0);
            z.start = std::max(0.0, startOff);
            z.stop = len > 0 ? len + endOff : -1;
            const double ls = comp ? s.loopStart : (double)s.loopStart - s.start, le = comp ? s.loopEnd : (double)s.loopEnd - s.start;
            z.loopStart = ls + iv(StartLoopOffset, 0) + 32768.0 * iv(StartLoopCoarse, 0);
            z.loopStop = le + iv(EndLoopOffset, 0) + 32768.0 * iv(EndLoopCoarse, 0);
            z.loopMode = iv(SampleModes, 0) & 3;
            if (z.loopMode == 2) z.loopMode = 0;
            z.delay = timecents(iv(DelayVol, -12000) + pv(DelayVol));
            z.attack = timecents(iv(AttackVol, -12000) + pv(AttackVol));
            z.hold = timecents(iv(HoldVol, -12000) + pv(HoldVol));
            z.decay = timecents(iv(DecayVol, -12000) + pv(DecayVol));
            z.release = timecents(iv(ReleaseVol, -12000) + pv(ReleaseVol));
            const double sus = std::clamp((double)(iv(SustainVol, 0) + pv(SustainVol)), 0.0, 1440.0);   // cB of attenuation
            z.sustain = sus >= 1000 ? 0.0 : std::pow(10.0, -sus / 200.0);
            z.fcCents = iv(FilterFc, 13500) + pv(FilterFc);
            z.qCb = iv(FilterQ, 0) + pv(FilterQ);
            z.modEnvToFc = iv(ModEnvToFc, 0) + pv(ModEnvToFc);
            z.modDelay = timecents(iv(DelayMod, -12000) + pv(DelayMod));
            z.modAttack = timecents(iv(AttackMod, -12000) + pv(AttackMod));
            z.modHold = timecents(iv(HoldMod, -12000) + pv(HoldMod));
            z.modDecay = timecents(iv(DecayMod, -12000) + pv(DecayMod));
            z.modSustain = 1 - std::clamp((iv(SustainMod, 0) + pv(SustainMod)) / 1000.0, 0.0, 1.0);
            z.modRelease = timecents(iv(ReleaseMod, -12000) + pv(ReleaseMod));
            z.keyToHold = iv(KeyToVolHold, 0) + pv(KeyToVolHold);
            z.keyToDecay = iv(KeyToVolDecay, 0) + pv(KeyToVolDecay);
            z.keyToModHold = iv(KeyToModHold, 0) + pv(KeyToModHold);
            z.keyToModDecay = iv(KeyToModDecay, 0) + pv(KeyToModDecay);
            z.perNote = perNote;
            z.exclusiveClass = iv(ExclusiveClass, 0);
            out.push_back(z);
        }
    }
    if (out.empty()) { err = "preset " + pr.name + " has no playable zones"; return false; }
    return true;
}

double Sf2Zone::modulate(uint16_t dest, int key, double vel) const {
    double sum = 0;
    for (auto &m : perNote) {
        if (m.dest != dest) continue;
        bool note = false, unknown = false;
        sum += m.amount * modSource(m.src, key, vel, note, unknown) * modSource(m.amtSrc, key, vel, note, unknown);
    }
    return sum;
}

bool SoundFont::sampleData(int index, DecodedAudio &out, std::string &err) const {
    if (index < 0 || (size_t)index >= shdr_.size()) { err = "no sample " + std::to_string(index); return false; }
    const Shdr &s = shdr_[index];
    std::ifstream f(std::filesystem::path(path_), std::ios::binary);
    if (!f) { err = "cannot read " + path_; return false; }
    if (s.type & 0x10) {   // SF3: an Ogg Vorbis stream at byte offsets start..end of smpl
        if (s.end <= s.start || s.end > smplSize_) { err = "sample " + s.name + " is out of range"; return false; }
        std::vector<uint8_t> ogg(s.end - s.start);
        f.seekg((std::streamoff)(smplOffset_ + s.start));
        f.read(reinterpret_cast<char *>(ogg.data()), (std::streamsize)ogg.size());
        if (!decodeAudio(ogg.data(), ogg.size(), out, err)) { err = "sample " + s.name + ": " + err; return false; }
        out.rate = s.rate ? s.rate : out.rate;
        return true;
    }
    if (s.end <= s.start || (uint64_t)s.end * 2 > smplSize_) { err = "sample " + s.name + " is out of range"; return false; }
    const size_t n = s.end - s.start;
    std::vector<uint8_t> raw(n * 2), low;
    f.seekg((std::streamoff)(smplOffset_ + (uint64_t)s.start * 2));
    f.read(reinterpret_cast<char *>(raw.data()), (std::streamsize)raw.size());
    if (sm24Size_ >= s.end) {   // 24-bit: the low bytes live in sm24
        low.resize(n);
        f.seekg((std::streamoff)(sm24Offset_ + s.start));
        f.read(reinterpret_cast<char *>(low.data()), (std::streamsize)n);
    }
    out = DecodedAudio{};
    out.rate = s.rate ? s.rate : 44100;
    out.l.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const int32_t v = (int32_t)(int16_t)le16(&raw[i * 2]) * 256 + (low.empty() ? 0 : low[i]);
        out.l[i] = (float)(v / 8388608.0);
    }
    return true;
}

} // namespace wl
