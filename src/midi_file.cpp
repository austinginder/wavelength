#include "midi_file.hpp"

#include "catalog.hpp"
#include "sampler.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

double r3(double v) { return std::round(v * 1000) / 1000; }
double r6(double v) { return std::round(v * 1e6) / 1e6; }   // beats: exact for 1/32 and finer grids

const char *const kGmNames[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
    "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
    "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
    "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Fingered Bass", "Picked Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
    "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
    "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead", "Bass + Lead",
    "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
    "Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX", "Brightness FX", "Goblins FX", "Echoes FX", "Sci-fi FX",
    "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
    "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
    "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"};

// General MIDI programs -> Bitwig multisamples (first one installed wins; names are prefixes)
std::vector<std::string> gmSounds(int program) {
    const int p = program;
    if (p <= 3) return {"Grand Piano", "Acoustic Piano (Nektar)"};
    if (p <= 5) return {"Rhodes", "Grand Piano"};
    if (p <= 7) return {"Clavinet", "Grand Piano"};
    if (p == 11) return {"Freak Ass Vibraphone", "Marimba"};
    if (p <= 15) return {"Marimba", "Freak Ass Marimba", "Kalimbaphone Soft"};
    if (p <= 23) return {"Philicorda Vox 1", "Farfisa SO Flute", "Grand Piano"};
    if (p == 28) return {"Muted Guitar", "Mute Picking Strat", "Mathieu Pe's Electric Guitar"};
    if (p <= 31) return {"Mathieu Pe's Electric Guitar", "Seven String Electric Guitar"};
    if (p == 32 || p == 35) return {"Acoustic Bass Long", "Fingered V-Bass"};
    if (p <= 37) return {"Fingered V-Bass", "Acoustic Bass Long"};
    if (p <= 39) return {"Tolcha MS20 Bass 1", "Japan Bass 1", "Fingered V-Bass"};
    if (p == 44) return {"Orchestral Strings Tremolo", "Orchestral Strings Sustained"};
    if (p == 45) return {"Orchestral Strings Pizzicato", "Orchestral Strings Sustained"};
    if (p == 46) return {"Grand Piano"};
    if (p == 47) return {"Orchestral Percussion - Timpani Single Hits"};
    if (p <= 49) return {"Orchestral Strings Sustained", "Tolcha String Orchestra Orc1"};
    if (p <= 51) return {"Tolcha String Orchestra Orc1", "String Orchestra Orc1", "Orchestral Strings Sustained"};
    if (p <= 54) return {"Kurasu Droid Ahhs", "Orchestral Strings Sustained Soft"};
    if (p == 55) return {"Orchestral Brass Full Ensemble Marcato"};
    if (p == 56 || p == 59 || p == 60) return {"Orchestral Trumpet and Horn Ensemble Sustain", "Orchestral Brass Full Ensemble Sustained"};
    if (p <= 58) return {"Orchestral Trombone Ensemble and Tuba Sustai", "Orchestral Brass Full Ensemble Sustained"};
    if (p == 61) return {"Orchestral Brass Full Ensemble Sustained"};
    if (p <= 63) return {"Tolcha MS20 Distobrass", "Orchestral Brass Full Ensemble Sustained"};
    if (p <= 67) return {"Farfisa SO Alto Sax", "Bassoons and Clarinets Octave Sustained"};
    if (p <= 71) return {"Bassoons and Clarinets Octave Sustained", "Flutes and Clarinets Octave Sustained"};
    if (p <= 79) return {"Flutes and Clarinets Octave Sustained", "Farfisa SO Flute", "Melodica"};
    if (p == 80) return {"Tolcha Prodigy Square Hi", "Tolcha Juno Saw"};
    if (p <= 87) return {"Tolcha Juno Saw", "Tolcha Prodigy Saw"};
    if (p <= 95) return {"Lost Ones Pad", "Fuzz Pad 1", "Tolcha Juno Chorus"};
    if (p <= 103) return {"Pad Hefollows", "Lost Ones Pad"};
    if (p == 108) return {"Kalimbaphone Soft", "Marimba"};
    if (p <= 111) return {"Mathieu Pe's Electric Guitar", "Marimba"};
    if (p <= 119) return {"Marimba", "Orchestral Percussion - Single Hits"};
    return {"Select 3 FX", "Pad Hefollows"};
}

// the installed multisample for a program ("" = none)
std::string gmMultisample(int program) {
    const auto &lib = sampleLibrary();
    auto lower = [](std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; };
    for (const std::string &want : gmSounds(program)) {
        const std::string w = lower(want);
        const SampleLibraryEntry *best = nullptr;
        for (auto &e : lib) {
            if (e.kind != "multisample") continue;
            const std::string n = lower(e.name);
            if (n == w) return e.path;
            if (n.rfind(w, 0) == 0 && (!best || e.name.size() < best->name.size())) best = &e;
        }
        if (best) return best->path;
    }
    return "";
}

// ---- reading -----------------------------------------------------------------------------------

struct Ev {
    uint64_t tick;
    int order;           // file order, for stable sorting
    uint8_t status;      // 0x80..0xEF, or 0xFF meta
    uint8_t a = 0, b = 0;
    uint8_t metaType = 0;
    std::string data;    // meta payload
};

bool readFile(const std::string &path, std::vector<uint8_t> &d, std::string &err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    d.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool parseTrack(const uint8_t *p, size_t n, std::vector<Ev> &out, int &order, std::string &err) {
    size_t i = 0;
    uint64_t tick = 0;
    uint8_t running = 0;
    auto vlq = [&](uint32_t &v) {
        v = 0;
        for (int k = 0; k < 4; ++k) {
            if (i >= n) return false;
            const uint8_t c = p[i++];
            v = v << 7 | (c & 0x7f);
            if (!(c & 0x80)) return true;
        }
        return false;
    };
    while (i < n) {
        uint32_t delta;
        if (!vlq(delta)) { err = "truncated delta time"; return false; }
        tick += delta;
        if (i >= n) break;
        uint8_t st = p[i];
        if (st & 0x80) { ++i; if (st < 0xf0) running = st; }
        else if (running) st = running;
        else { err = "data byte without a status"; return false; }
        if (st == 0xff) {
            if (i >= n) break;
            const uint8_t type = p[i++];
            uint32_t len;
            if (!vlq(len) || i + len > n) { err = "truncated meta event"; return false; }
            Ev e{tick, order++, 0xff, 0, 0, 0, {}};
            e.metaType = type;
            e.data.assign(reinterpret_cast<const char *>(p + i), len);
            i += len;
            out.push_back(std::move(e));
            if (type == 0x2f) break;
            continue;
        }
        if (st == 0xf0 || st == 0xf7) {   // sysex: skipped
            uint32_t len;
            if (!vlq(len) || i + len > n) { err = "truncated sysex"; return false; }
            i += len;
            continue;
        }
        const int kind = st & 0xf0;
        const int nData = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
        if (i + (size_t)nData > n) break;
        Ev e{tick, order++, st, 0, 0, 0, {}};
        e.a = p[i] & 0x7f;
        if (nData == 2) e.b = p[i + 1] & 0x7f;
        i += (size_t)nData;
        out.push_back(e);
    }
    return true;
}

} // namespace

std::string gmProgramName(int program) { return kGmNames[std::clamp(program, 0, 127)]; }
std::string gmProgramSound(int program) { return gmMultisample(std::clamp(program, 0, 127)); }

bool importMidiFile(const std::string &path, const std::string &outDir, const std::string &instrument, MidiImport &res, std::string &err) {
    std::vector<uint8_t> d;
    if (!readFile(path, d, err)) return false;
    size_t pos = 0;
    if (d.size() >= 8 && std::equal(d.begin(), d.begin() + 4, "RIFF")) pos = 20;   // RMID wrapper
    if (d.size() < pos + 14 || !std::equal(d.begin() + (long)pos, d.begin() + (long)pos + 4, "MThd")) { err = path + " is not a Standard MIDI File"; return false; }
    const uint32_t hlen = be32(&d[pos + 4]);
    res.format = be16(&d[pos + 8]);
    const int ntrks = be16(&d[pos + 10]);
    const uint16_t division = be16(&d[pos + 12]);
    double ticksPerBeat = division;
    bool smpte = false;
    if (division & 0x8000) {   // SMPTE: ticks per second; read as 120 bpm
        const int fps = -(int8_t)(division >> 8), sub = division & 0xff;
        ticksPerBeat = fps * sub / 2.0;
        smpte = true;
        res.notes.push_back("the file is timed in SMPTE frames, not beats: read as 120 bpm");
    }
    if (ticksPerBeat <= 0) { err = path + ": bad time division"; return false; }
    pos += 8 + hlen;
    std::vector<std::vector<Ev>> tracks;
    int order = 0;
    while (pos + 8 <= d.size() && (int)tracks.size() < ntrks) {
        const uint32_t len = be32(&d[pos + 4]);
        if (std::equal(d.begin() + (long)pos, d.begin() + (long)pos + 4, "MTrk")) {
            if (pos + 8 + len > d.size()) { err = path + ": truncated track"; return false; }
            tracks.emplace_back();
            std::string e;
            if (!parseTrack(&d[pos + 8], len, tracks.back(), order, e)) { err = path + ": track " + std::to_string(tracks.size()) + ": " + e; return false; }
        }
        pos += 8 + len;
    }
    if (tracks.empty()) { err = path + " has no tracks"; return false; }
    auto beat = [&](uint64_t tick) { return r6((double)tick / ticksPerBeat); };

    // conductor: tempo, time signature, markers (from any track)
    std::vector<std::pair<uint64_t, double>> tempos;
    json markers = json::array();
    json timeSig;
    bool extraTimeSig = false;
    for (auto &t : tracks)
        for (auto &e : t) {
            if (e.status != 0xff) continue;
            if (e.metaType == 0x51 && e.data.size() == 3 && !smpte) {
                const uint32_t us = (uint32_t)(uint8_t)e.data[0] << 16 | (uint32_t)(uint8_t)e.data[1] << 8 | (uint8_t)e.data[2];
                if (us) tempos.push_back({e.tick, 60e6 / us});
            } else if (e.metaType == 0x58 && e.data.size() >= 2) {
                const json ts = {(int)(uint8_t)e.data[0], 1 << (uint8_t)e.data[1]};
                if (timeSig.is_null()) timeSig = ts;
                else if (ts != timeSig) extraTimeSig = true;
            } else if (e.metaType == 0x06 || e.metaType == 0x07) {
                markers.push_back({{"beat", beat(e.tick)}, {"name", e.data.empty() ? "Marker" : e.data}});
            }
        }
    std::sort(tempos.begin(), tempos.end(), [](auto &a, auto &b) { return a.first < b.first; });
    json job = json::object();
    if (tempos.empty() || tempos.size() == 1) job["tempo"] = r6(tempos.empty() ? 120.0 : tempos[0].second);
    else {
        json map = json::array();
        if (tempos[0].first > 0) map.push_back({{"beat", 0}, {"bpm", r6(tempos[0].second)}});
        for (auto &[tk, bpm] : tempos) {
            if (!map.empty() && std::fabs(map.back()["beat"].get<double>() - beat(tk)) < 1e-9) map.back()["bpm"] = r6(bpm);
            else if (map.empty() || std::fabs(map.back()["bpm"].get<double>() - bpm) > 1e-6) map.push_back({{"beat", beat(tk)}, {"bpm", r6(bpm)}});
        }
        job["tempo"] = map.size() == 1 ? map[0]["bpm"] : map;
    }
    if (!timeSig.is_null()) job["timeSignature"] = timeSig;
    if (extraTimeSig) res.notes.push_back("time signature changes after the first are left out (Wavelength keeps one)");

    // parts: one per (track, channel)
    struct Part {
        std::string trackName;
        int channel = 0, program = -1;
        json notes = json::array();
        std::map<int, std::vector<std::pair<uint64_t, int>>> cc;    // controller -> (tick, value)
        std::vector<std::pair<uint64_t, int>> bend, pressure;       // bend: -8192..8191
        double bendRange = 2;
    };
    std::vector<Part> parts;
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        auto &evs = tracks[ti];
        std::stable_sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) { return a.tick < b.tick; });
        std::string name;
        for (auto &e : evs) if (e.status == 0xff && e.metaType == 0x03 && !e.data.empty()) { name = e.data; break; }
        std::map<int, size_t> partOf;   // channel -> index in parts
        struct On { uint64_t tick; int vel; int seq; };
        int onSeq = 0;
        std::map<int, std::vector<On>> held;                    // channel*128+key -> note-ons, oldest first
        std::map<int, bool> pedal;                              // channel -> sustain down
        std::map<int, std::vector<std::pair<size_t, int>>> sustained;   // channel -> (note index in part, key) waiting for pedal up
        std::map<int, std::pair<int, int>> rpn;                 // channel -> (msb, lsb)
        uint64_t lastTick = evs.empty() ? 0 : evs.back().tick;
        auto part = [&](int ch) -> Part & {
            auto it = partOf.find(ch);
            if (it != partOf.end()) return parts[it->second];
            partOf[ch] = parts.size();
            parts.emplace_back();
            parts.back().trackName = name;
            parts.back().channel = ch;
            return parts.back();
        };
        auto finish = [&](Part &p, int ch, int key, uint64_t endTick) {
            auto &h = held[ch * 128 + key];
            if (h.empty()) return;
            const On on = h.front();
            h.erase(h.begin());
            const double b0 = (double)on.tick / ticksPerBeat, b1 = (double)std::max(endTick, on.tick + 1) / ticksPerBeat;
            p.notes.push_back({{"beat", r6(b0)}, {"dur", r6(std::max(b1 - b0, 0.001))}, {"key", key}, {"vel", r3(on.vel / 127.0)}, {"_seq", on.seq}});
            ++res.noteCount;
            if (pedal[ch]) sustained[ch].push_back({p.notes.size() - 1, key});
        };
        for (auto &e : evs) {
            if (e.status == 0xff) continue;
            const int kind = e.status & 0xf0, ch = e.status & 0x0f;
            if (kind == 0x90 && e.b > 0) {
                Part &p = part(ch);
                // a new note on a key held by the pedal ends the sustained one here
                auto &s = sustained[ch];
                for (auto it = s.begin(); it != s.end();)
                    if (it->second == e.a) {
                        json &n = p.notes[it->first];
                        n["dur"] = r6(std::max((double)e.tick / ticksPerBeat - n["beat"].get<double>(), 0.001));
                        it = s.erase(it);
                    } else ++it;
                held[ch * 128 + e.a].push_back({e.tick, e.b, onSeq++});
            } else if (kind == 0x80 || kind == 0x90) finish(part(ch), ch, e.a, e.tick);
            else if (kind == 0xb0) {
                Part &p = part(ch);
                if (e.a == 64) {   // sustain pedal: fold into note lengths
                    const bool down = e.b >= 64;
                    if (!down && pedal[ch])
                        for (auto &[idx, key] : sustained[ch]) {
                            json &n = p.notes[idx];
                            n["dur"] = r6(std::max((double)e.tick / ticksPerBeat - n["beat"].get<double>(), n["dur"].get<double>()));
                        }
                    if (!down) sustained[ch].clear();
                    pedal[ch] = down;
                } else if (e.a == 101) rpn[ch].first = e.b;
                else if (e.a == 100) rpn[ch].second = e.b;
                else if (e.a == 6 && rpn.count(ch) && rpn[ch] == std::make_pair(0, 0)) p.bendRange = e.b;   // RPN 0: bend range
                else if (e.a == 38 || e.a == 98 || e.a == 99 || e.a == 0 || e.a == 32) {}   // RPN/NRPN data, bank select
                else p.cc[e.a].push_back({e.tick, e.b});
            } else if (kind == 0xc0) { Part &p = part(ch); if (p.program < 0) p.program = e.a; }
            else if (kind == 0xe0) part(ch).bend.push_back({e.tick, (e.b << 7 | e.a) - 8192});
            else if (kind == 0xd0) part(ch).pressure.push_back({e.tick, e.a});
        }
        for (auto &[k, h] : held) while (!h.empty()) finish(parts[partOf[k / 128]], k / 128, k % 128, lastTick);
        if (partOf.size() > 1)
            for (auto &[ch, idx] : partOf) parts[idx].trackName += (parts[idx].trackName.empty() ? "" : " ") + std::string("ch ") + std::to_string(ch + 1);
    }

    // tracks
    json out = json::array();
    std::set<std::string> used;
    std::map<std::string, std::string> soundOf;   // program -> multisample path (looked up once)
    for (auto &p : parts) {
        if (p.notes.empty()) continue;
        std::sort(p.notes.begin(), p.notes.end(), [](const json &a, const json &b) {   // by start, then as played
            const double x = a["beat"].get<double>(), y = b["beat"].get<double>();
            return x != y ? x < y : a["_seq"].get<int>() < b["_seq"].get<int>();
        });
        for (auto &n : p.notes) n.erase("_seq");
        const bool drums = p.channel == 9;
        const int program = std::max(p.program, 0);
        std::string name = p.trackName;
        if (name.empty()) name = drums ? "Drums" : kGmNames[program];
        std::string unique = name;
        for (int k = 2; used.count(unique); ++k) unique = name + " " + std::to_string(k);
        used.insert(unique);
        json t = {{"name", unique}};
        bool sampler = false;
        if (drums) t["plugin"] = "builtin:drums";
        else if (!instrument.empty()) t["plugin"] = instrument;
        else {
            const std::string key = std::to_string(program);
            if (!soundOf.count(key)) soundOf[key] = gmMultisample(program);
            if (!soundOf[key].empty()) {
                t["plugin"] = "builtin:sampler";
                t["sampler"] = {{"multisample", soundOf[key]}};
                sampler = true;
            } else {
                t["plugin"] = "builtin:drums";
                res.notes.push_back(unique + ": no sound for " + kGmNames[program] + " in the sample library; set \"plugin\" (builtin:drums stands in)");
            }
        }
        if (p.program >= 0) t["midiProgram"] = p.program;   // kept for export
        t["midiChannel"] = p.channel;   // kept for export
        // controllers: volume and pan to the fader, expression to rides, the rest as MIDI
        // a curve starting after beat 0 gets the controller's resting value before it (automation
        // holds its first point's value before that point)
        auto curve = [&](const std::vector<std::pair<uint64_t, int>> &pts, auto map, double rest = 0.0) {
            json c = json::array();
            if (!pts.empty() && pts[0].first > 0) c.push_back({0.0, rest});
            for (auto &[tk, v] : pts) {
                const double y = r6(map(v));
                if (!c.empty() && std::fabs(c.back()[0].get<double>() - beat(tk)) < 1e-9) c.back()[1] = y;
                else if (c.empty() || std::fabs(c.back()[1].get<double>() - y) > 1e-9) c.push_back({beat(tk), y});
            }
            return c;
        };
        auto volDb = [](int v) { return v <= 0 ? -60.0 : std::max(-60.0, 40 * std::log10(v / 127.0)); };
        auto panOf = [](int v) { return std::clamp((v - 64) / 63.0, -1.0, 1.0); };
        json autom = json::object();
        std::vector<int> dropped;
        auto steps = [](json c) { return json{{"points", c}, {"curve", "step"}}; };
        for (auto &[num, pts] : p.cc) {
            if (num == 7 || num == 10) {
                json c = curve(pts, num == 7 ? std::function<double(int)>(volDb) : std::function<double(int)>(panOf));
                if (c.size() == 1) t[num == 7 ? "gain" : "pan"] = c[0][1];
                else if (!c.empty()) autom[num == 7 ? "gain" : "pan"] = steps(c);
            } else if (num == 11) {
                json c = curve(pts, std::function<double(int)>(volDb));
                if (!c.empty()) autom["rides"] = steps(c);
            } else {
                if (sampler || drums) { dropped.push_back(num); continue; }   // the sampler and the drum kit take no MIDI controllers
                autom["cc"][std::to_string(num)] = steps(curve(pts, [](int v) { return (double)v; }, num == 11 ? 127.0 : 0.0));
            }
        }
        if (!p.bend.empty()) {
            if (sampler) {   // per-note bends: the bend in force while each note sounds
                for (auto &n : p.notes) {
                    const double b0 = n["beat"].get<double>(), b1 = b0 + n["dur"].get<double>();
                    json nb = json::array();
                    double before = 0;
                    for (auto &[tk, v] : p.bend) {
                        const double bt = (double)tk / ticksPerBeat, st = r6(v / 8192.0 * p.bendRange);   // semitones
                        if (bt <= b0) { before = st; continue; }
                        if (bt >= b1) break;
                        if (nb.empty()) nb.push_back({0, before});
                        nb.push_back({r6(bt - b0), st});
                    }
                    if (nb.empty() && std::fabs(before) > 1e-3) nb = json::array({json::array({0, before})});
                    if (!nb.empty()) n["bend"] = nb;
                }
            } else if (!drums) {
                autom["pitchbend"] = steps(curve(p.bend, [&](int v) { return v / 8192.0 * p.bendRange; }));
                if (p.bendRange != 2) t["bendRange"] = p.bendRange;
                res.notes.push_back(unique + ": pitch bend assumes the plugin bends +-" + std::to_string((int)p.bendRange) + " semitones (set \"bendRange\")");
            }
        }
        if (!dropped.empty()) {
            std::string l;
            for (int d : dropped) l += (l.empty() ? "" : ", ") + std::to_string(d);
            res.notes.push_back(unique + ": CC " + l + " left out (built-in instruments take no MIDI controllers; a plugin would)");
        }
        if (!p.pressure.empty() && !sampler && !drums) autom["pressure"] = steps(curve(p.pressure, [](int v) { return (double)v; }));
        if (!autom.empty()) t["automation"] = autom;
        t["notes"] = p.notes;
        out.push_back(t);
        ++res.tracks;
    }
    if (out.empty()) { err = path + " has no notes"; return false; }
    job["tracks"] = out;
    if (!markers.empty()) job["markers"] = markers;
    res.job = job;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::ofstream o(fs::path(outDir) / "job.json");
    o << job.dump(1) << "\n";
    if (!o) { err = "cannot write " + (fs::path(outDir) / "job.json").string(); return false; }
    return true;
}

// ---- writing -----------------------------------------------------------------------------------

namespace {

// prio orders events at one tick (offs, names, programs, RPN, controllers, notes); `seq` keeps an
// ordered run (RPN) together; anything still tied sorts by its bytes, so the file doesn't depend on
// the order of the job's notes
struct OutEv { uint64_t tick; int prio; std::vector<uint8_t> bytes; int seq = 0; };

void vlq(std::vector<uint8_t> &o, uint32_t v) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = v & 0x7f;
    while (v >>= 7) buf[n++] = (uint8_t)(0x80 | (v & 0x7f));
    while (n) o.push_back(buf[--n]);
}

std::vector<uint8_t> meta(uint8_t type, const std::string &s) {
    std::vector<uint8_t> b = {0xff, type};
    vlq(b, (uint32_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
    return b;
}

std::vector<uint8_t> chunk(std::vector<OutEv> evs) {
    std::stable_sort(evs.begin(), evs.end(), [](const OutEv &a, const OutEv &b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.prio != b.prio) return a.prio < b.prio;
        if (a.seq != b.seq) return a.seq < b.seq;
        return a.bytes < b.bytes;
    });
    std::vector<uint8_t> body;
    uint64_t last = 0;
    for (auto &e : evs) {
        vlq(body, (uint32_t)(e.tick - last));
        last = e.tick;
        body.insert(body.end(), e.bytes.begin(), e.bytes.end());
    }
    vlq(body, 0);
    body.insert(body.end(), {0xff, 0x2f, 0x00});
    std::vector<uint8_t> c = {'M', 'T', 'r', 'k'};
    for (int s = 24; s >= 0; s -= 8) c.push_back((uint8_t)(body.size() >> s));
    c.insert(c.end(), body.begin(), body.end());
    return c;
}

} // namespace

bool exportMidiFile(const Job &job, const json &raw, const std::string &path, std::vector<std::string> &notes, std::string &err) {
    constexpr int kPpq = 960;
    auto tickOfBeat = [](double b) { return (uint64_t)std::llround(std::max(0.0, b) * kPpq); };
    auto tickOfSec = [&](double s) { return tickOfBeat(job.tempo.secToBeat(s)); };
    double endBeat = 0;
    for (auto &t : job.tracks)
        for (auto &n : t.notes) endBeat = std::max(endBeat, job.tempo.secToBeat(n.start + n.length));
    for (auto &m : job.markers) endBeat = std::max(endBeat, m.beat);

    // conductor track
    std::vector<OutEv> cond;
    cond.push_back({0, 0, meta(0x03, fs::path(path).stem().string())});
    {
        std::vector<uint8_t> ts = {0xff, 0x58, 4, (uint8_t)job.tsigNum, 2, 24, 8};
        int den = job.tsigDen, pow2 = 0;
        while (den > 1) { den >>= 1; ++pow2; }
        ts[4] = (uint8_t)pow2;
        cond.push_back({0, 0, ts});
    }
    auto tempoEv = [](double bpm) {
        const uint32_t us = (uint32_t)std::llround(60e6 / std::clamp(bpm, 1.0, 1000.0));
        return std::vector<uint8_t>{0xff, 0x51, 3, (uint8_t)(us >> 16), (uint8_t)(us >> 8), (uint8_t)us};
    };
    // tempo: exact steps from the job's map; ramps sampled every 1/16 beat
    std::vector<std::pair<double, bool>> points;   // beat, ramp
    if (raw.contains("tempo") && raw["tempo"].is_array())
        for (auto &p : raw["tempo"]) points.push_back({p.value("beat", 0.0), p.value("ramp", false)});
    else points.push_back({0, false});
    double lastBpm = -1;
    for (size_t k = 0; k < points.size(); ++k) {
        const double b = points[k].first;
        if (points[k].second && k > 0)
            for (double x = points[k - 1].first + 0.0625; x < b - 1e-9; x += 0.0625) {
                const double bpm = job.tempo.bpmAtBeat(x);
                if (std::fabs(bpm - lastBpm) > 0.005) { cond.push_back({tickOfBeat(x), 1, tempoEv(bpm)}); lastBpm = bpm; }
            }
        const double bpm = job.tempo.bpmAtBeat(b + 1e-9);
        if (std::fabs(bpm - lastBpm) > 0.005) { cond.push_back({tickOfBeat(b), 1, tempoEv(bpm)}); lastBpm = bpm; }
    }
    for (auto &m : job.markers) cond.push_back({tickOfBeat(m.beat), 2, meta(0x06, m.name)});
    std::vector<std::vector<uint8_t>> chunks = {chunk(cond)};

    // one track per job track
    int nextChannel = 0;
    const json rawTracks = raw.value("tracks", json::array());
    for (size_t ti = 0; ti < job.tracks.size(); ++ti) {
        const Track &t = job.tracks[ti];
        const json rt = ti < rawTracks.size() ? rawTracks[ti] : json::object();
        if (t.notes.empty()) { notes.push_back(t.name + ": no notes (audio clips) ; left out"); continue; }
        const int kept = rt.contains("midiChannel") && rt["midiChannel"].is_number_integer() ? std::clamp(rt["midiChannel"].get<int>(), 0, 15) : -1;
        const bool drums = kept >= 0 ? kept == 9 : (t.plugin == "builtin:drums" || (rt.contains("sampler") && rt["sampler"].is_object() && rt["sampler"].contains("kit")));
        int ch = kept >= 0 ? kept : drums ? 9 : nextChannel;
        if (kept < 0 && !drums) { ++nextChannel; if (nextChannel == 9) ++nextChannel; if (nextChannel > 15) { nextChannel = 0; notes.push_back("more than 15 melodic tracks: channels repeat (each track stays on its own MIDI track)"); } }
        std::vector<OutEv> ev;
        ev.push_back({0, 0, meta(0x03, t.name)});
        if (rt.contains("midiProgram") && !drums) ev.push_back({0, 1, {(uint8_t)(0xc0 | ch), (uint8_t)std::clamp(rt["midiProgram"].get<int>(), 0, 127)}});
        auto cc = [&](uint64_t tick, int num, int v) { ev.push_back({tick, 2, {(uint8_t)(0xb0 | ch), (uint8_t)num, (uint8_t)std::clamp(v, 0, 127)}}); };
        // the fader as CC7 (40 log10 curve, 127 = 0 dB), rides as CC11, pan as CC10
        const json ra = rt.value("automation", json::object());
        auto dbToCc = [](double db) { return std::clamp((int)std::lround(127 * std::pow(10.0, std::max(db, -60.0) / 40)), 0, 127); };
        std::vector<Envelope> faderCurve, rideCurves;
        try {
            if (ra.contains("gain")) faderCurve.push_back(Envelope::parse(ra["gain"], job.tempo, false));
            if (ra.contains("rides")) {
                const json &r = ra["rides"];
                if (r.is_object() && !r.contains("points") && !r.contains("value")) for (auto &c : r.items()) rideCurves.push_back(Envelope::parse(c.value(), job.tempo, false));
                else rideCurves.push_back(Envelope::parse(r, job.tempo, false));
            }
        } catch (const std::exception &) {}
        if (faderCurve.empty() && (t.gainDb < -0.01 || rt.contains("gain"))) cc(0, 7, dbToCc(t.gainDb));
        if (t.gainDb > 0.01 || (!faderCurve.empty() && faderCurve[0].at(0) > 0.01)) notes.push_back(t.name + ": fader above 0 dB is written as CC7 127");
        if (t.panAutomation.empty() && std::fabs(t.pan) > 1e-3) cc(0, 10, (int)std::lround(64 + t.pan * 63));
        const bool hasBend = !t.bendAutomation.empty();
        if (hasBend && std::fabs(t.bendRange - 2) > 1e-6) {   // RPN 0 = bend range
            const int rpn[6][2] = {{101, 0}, {100, 0}, {6, (int)std::lround(t.bendRange)}, {38, 0}, {101, 127}, {100, 127}};
            for (int k = 0; k < 6; ++k) ev.push_back({0, 1, {(uint8_t)(0xb0 | ch), (uint8_t)rpn[k][0], (uint8_t)std::clamp(rpn[k][1], 0, 127)}, k});
        }
        size_t perNoteBends = 0;
        for (auto &n : t.notes) {
            const int nch = n.channel ? n.channel : ch;
            const uint64_t on = tickOfSec(n.start), off = std::max(on + 1, tickOfSec(n.start + n.length));
            const int vel = std::clamp((int)std::lround(n.velocity * 127), 1, 127);
            ev.push_back({on, 4, {(uint8_t)(0x90 | nch), (uint8_t)std::clamp(n.key, 0, 127), (uint8_t)vel}});
            ev.push_back({off, 0, {(uint8_t)(0x80 | nch), (uint8_t)std::clamp(n.key, 0, 127), 0}});
            if (!n.bend.empty()) ++perNoteBends;
        }
        if (perNoteBends) notes.push_back(t.name + ": " + std::to_string(perNoteBends) + " per-note bend(s) left out (MIDI bends the whole channel)");
        // automation, sampled every 1/32 beat, an event whenever the MIDI value changes
        auto sample = [&](const Envelope &e, int status, auto toValue) {
            int last = -100000;
            for (double b = 0; b <= endBeat + 1e-9; b += 1.0 / 32) {
                const int v = toValue(e.at(job.tempo.beatToSec(b + 1e-6)));   // just after a step, which lands on the grid
                if (v == last) continue;
                last = v;
                if (status == 0xe0) ev.push_back({tickOfBeat(b), 3, {(uint8_t)(0xe0 | ch), (uint8_t)(v & 0x7f), (uint8_t)(v >> 7)}});
                else if (status == 0xd0) ev.push_back({tickOfBeat(b), 3, {(uint8_t)(0xd0 | ch), (uint8_t)v}});
                else cc(tickOfBeat(b), status, v);
            }
        };
        for (auto &[num, env] : t.ccAutomation) sample(env, num, [](double v) { return std::clamp((int)std::lround(v), 0, 127); });
        if (!faderCurve.empty()) sample(faderCurve[0], 7, dbToCc);
        if (!rideCurves.empty()) {
            int last = -1;
            for (double b = 0; b <= endBeat + 1e-9; b += 1.0 / 32) {
                double db = 0;
                for (auto &e : rideCurves) db += e.at(job.tempo.beatToSec(b + 1e-6));
                const int v = dbToCc(db);
                if (v != last) { cc(tickOfBeat(b), 11, v); last = v; }
            }
        }
        if (!t.panAutomation.empty()) sample(t.panAutomation, 10, [](double p) { return std::clamp((int)std::lround(64 + p * 63), 0, 127); });
        if (hasBend) sample(t.bendAutomation, 0xe0, [&](double st) { return std::clamp((int)std::lround(st / t.bendRange * 8192) + 8192, 0, 16383); });
        if (!t.pressureAutomation.empty()) sample(t.pressureAutomation, 0xd0, [](double v) { return std::clamp((int)std::lround(v), 0, 127); });
        chunks.push_back(chunk(ev));
    }

    std::ofstream o(path, std::ios::binary);
    if (!o) { err = "cannot write " + path; return false; }
    const uint16_t ntrks = (uint16_t)chunks.size();
    const uint8_t header[14] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, (uint8_t)(ntrks >> 8), (uint8_t)ntrks, (uint8_t)(kPpq >> 8), (uint8_t)kPpq};
    o.write(reinterpret_cast<const char *>(header), 14);
    for (auto &c : chunks) o.write(reinterpret_cast<const char *>(c.data()), (std::streamsize)c.size());
    if (!o) { err = "cannot write " + path; return false; }
    return true;
}

} // namespace wl
