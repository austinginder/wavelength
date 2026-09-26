#include "musicxml.hpp"

#include "midi_file.hpp"
#include "xml.hpp"
#include "zip.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

double r6(double v) { return std::round(v * 1e6) / 1e6; }

std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ---- one part, measure by measure, in score order ---------------------------------------------
struct NoteEv {
    double at = 0, dur = 0;          // quarter-note beats from the measure start
    int key = 60, voice = 1;
    bool tieStart = false, tieStop = false;
    double dynAttr = -1;             // the note's dynamics attribute (% of forte), -1 = none
    double accent = 0;               // velocity added by accents and sf marks
    double length = 1;               // fraction of the written length that sounds (staccato)
    std::vector<std::string> marks;
};

struct Measure {
    double length = 0;               // beats reached in this measure (pickups are short)
    double nominal = 0;              // beats from the time signature
    std::vector<NoteEv> notes;
    std::vector<std::pair<double, double>> tempos;       // at, quarter bpm
    std::vector<std::pair<double, double>> dynamics;     // at, level 0-1
    std::vector<std::pair<double, int>> wedges;          // at, +1 crescendo / -1 diminuendo / 0 stop
    std::vector<std::pair<double, std::string>> rehearsals;
    bool hasKey = false;
    int fifths = 0;
    std::string mode;                // "" when the score gives none
    bool forward = false, backward = false;
    int times = 2;
    std::set<int> ending;            // the ending (volta) this measure belongs to, empty = none
    bool endingStart = false, endingStop = false;
    bool segno = false, coda = false, fine = false, daCapo = false, dalSegno = false, toCoda = false;
};

struct Part {
    std::string id, name, instrumentName;
    int program = -1, channel = -1;  // 0-based; -1 = not given
    double pan = 0;
    bool percussion = false;
    std::map<std::string, int> unpitched;   // score-instrument id -> MIDI key
    std::vector<Measure> measures;
};

// a dynamics mark as a velocity (0-1); -1 = not a level (sf and friends are accents)
double dynamicLevel(const std::string &m) {
    static const std::map<std::string, double> levels = {
        {"pppppp", 0.08}, {"ppppp", 0.11}, {"pppp", 0.15}, {"ppp", 0.22}, {"pp", 0.3}, {"p", 0.4}, {"mp", 0.5}, {"mf", 0.6},
        {"f", 0.71}, {"ff", 0.83}, {"fff", 0.93}, {"ffff", 1.0}, {"fffff", 1.0}, {"ffffff", 1.0}, {"pf", 0.6}, {"n", 0.08}};
    auto it = levels.find(m);
    return it == levels.end() ? -1 : it->second;
}

// General MIDI program guessed from a part or instrument name
int programFromName(const std::string &n0) {
    const std::string n = lower(n0);
    static const std::vector<std::pair<const char *, int>> table = {
        {"harpsichord", 6}, {"clavi", 7}, {"celest", 8}, {"glock", 9}, {"music box", 10}, {"vibraphone", 11}, {"marimba", 12},
        {"xylophone", 13}, {"tubular", 14}, {"bells", 14}, {"dulcimer", 15}, {"organ", 19}, {"accordion", 21}, {"harmonica", 22},
        {"electric piano", 4}, {"rhodes", 4}, {"piano", 0}, {"nylon", 24}, {"classical guitar", 24}, {"acoustic guitar", 25},
        {"electric guitar", 27}, {"guitar", 25}, {"electric bass", 33}, {"bass guitar", 33}, {"fretless", 35},
        {"contrabass", 43}, {"double bass", 43}, {"violoncell", 42}, {"cello", 42}, {"viola", 41}, {"violin", 40}, {"fiddle", 40},
        {"harp", 46}, {"timpani", 47}, {"pizz", 45}, {"string", 48}, {"choir", 52}, {"chorus", 52}, {"soprano", 52}, {"alto sax", 65},
        {"tenor sax", 66}, {"baritone sax", 67}, {"soprano sax", 64}, {"sax", 65}, {"alto", 52}, {"tenor", 52}, {"baritone", 52},
        {"bass voice", 52}, {"voice", 52}, {"vocal", 52}, {"trumpet", 56}, {"cornet", 56}, {"flugel", 56}, {"trombone", 57}, {"tuba", 58},
        {"euphonium", 58}, {"horn", 60}, {"brass", 61}, {"oboe", 68}, {"english horn", 69}, {"cor anglais", 69}, {"bassoon", 70},
        {"clarinet", 71}, {"piccolo", 72}, {"flute", 73}, {"recorder", 74}, {"pan flute", 75}, {"ocarina", 79}, {"bass", 32}};
    for (auto &[k, p] : table) if (n.find(k) != std::string::npos) return p;
    return 0;
}

std::string keyName(int fifths, const std::string &mode) {
    static const char *const majors[] = {"Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#"};
    static const char *const minors[] = {"Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#"};
    fifths = std::clamp(fifths, -7, 7);
    const std::string m = lower(mode);
    if (m == "minor" || m == "aeolian") return std::string(minors[fifths + 7]) + " minor";
    if (m == "major" || m == "ionian") return std::string(majors[fifths + 7]) + " major";
    // church modes: the tonic sits a fixed number of fifths from the major tonic
    static const std::map<std::string, int> shift = {{"dorian", 2}, {"phrygian", 4}, {"lydian", -1}, {"mixolydian", 1}, {"locrian", 5}};
    auto it = shift.find(m);
    if (it == shift.end()) return "";
    static const char *const names[] = {"Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#", "E#", "B#"};
    const int idx = fifths + 7 + it->second;
    if (idx < 0 || idx >= 20 || m == "locrian") return "";   // Wavelength keys have no locrian
    return std::string(names[idx]) + " " + m;
}

std::set<int> endingNumbers(const std::string &s) {
    std::set<int> out;
    std::string cur;
    for (char c : s + ",") {
        if (std::isdigit((unsigned char)c)) cur += c;
        else if (!cur.empty()) { out.insert(std::atoi(cur.c_str())); cur.clear(); }
    }
    return out;
}

void readSound(const xml::Node &snd, Measure &m, double pos) {
    if (snd.attr("tempo")) {
        const double t = snd.num("tempo", 0);
        if (t > 0) m.tempos.push_back({pos, t});
    }
    if (snd.attr("dynamics")) m.dynamics.push_back({pos, std::clamp(snd.num("dynamics", 70) * 0.9 / 127.0, 0.02, 1.0)});
    if (lower(snd.get("dacapo")) == "yes") m.daCapo = true;
    if (!snd.get("dalsegno").empty()) m.dalSegno = true;
    if (!snd.get("tocoda").empty()) m.toCoda = true;
    if (!snd.get("fine").empty()) m.fine = true;
    if (!snd.get("segno").empty()) m.segno = true;
    if (!snd.get("coda").empty()) m.coda = true;
}

void readPart(const xml::Node &p, Part &part, size_t &graceSkipped) {
    int divisions = 1, transpose = 0;
    double beatsPerMeasure = 4;
    std::set<int> openEnding;
    for (const xml::Node *mx : p.all("measure")) {
        Measure m;
        double pos = 0, lastStart = 0;
        if (!openEnding.empty()) m.ending = openEnding;
        for (auto &cp : mx->children) {
            const xml::Node &c = *cp;
            if (c.tag == "attributes") {
                if (c.child("divisions")) divisions = std::max(1, (int)c.childNum("divisions", 1));
                if (const xml::Node *k = c.child("key"); k && k->child("fifths")) {
                    m.hasKey = true;
                    m.fifths = (int)k->childNum("fifths", 0);
                    m.mode = k->childText("mode");
                }
                if (const xml::Node *t = c.child("time"); t && t->child("beats")) {
                    double beats = 0;
                    for (auto &part2 : endingNumbers(t->childText("beats"))) beats += part2;   // "3+2" additive meters
                    const double type = t->childNum("beat-type", 4);
                    if (beats > 0 && type > 0) beatsPerMeasure = beats * 4.0 / type;
                }
                if (const xml::Node *tr = c.child("transpose")) transpose = (int)tr->childNum("chromatic", 0) + 12 * (int)tr->childNum("octave-change", 0);
            } else if (c.tag == "backup") {
                pos = std::max(0.0, pos - c.childNum("duration", 0) / divisions);
            } else if (c.tag == "forward") {
                pos += c.childNum("duration", 0) / divisions;
                m.length = std::max(m.length, pos);
            } else if (c.tag == "direction") {
                const double at = pos + c.childNum("offset", 0) / divisions;
                bool soundTempo = false;
                if (const xml::Node *snd = c.child("sound")) { soundTempo = snd->attr("tempo") != nullptr; readSound(*snd, m, at); }
                for (const xml::Node *dt : c.all("direction-type")) {
                    if (const xml::Node *dyn = dt->child("dynamics"))
                        for (auto &mk : dyn->children) {
                            const double lv = dynamicLevel(mk->tag);
                            if (lv >= 0 && !(c.child("sound") && c.child("sound")->attr("dynamics"))) m.dynamics.push_back({at, lv});
                        }
                    if (const xml::Node *w = dt->child("wedge")) {
                        const std::string type = w->get("type");
                        m.wedges.push_back({at, type == "crescendo" ? 1 : type == "diminuendo" ? -1 : 0});
                    }
                    if (const xml::Node *r = dt->child("rehearsal"); r && !r->text.empty()) m.rehearsals.push_back({at, r->text});
                    if (dt->child("segno")) m.segno = true;
                    if (dt->child("coda")) m.coda = true;
                    if (const xml::Node *mt = dt->child("metronome"); mt && !soundTempo && mt->child("per-minute")) {
                        static const std::map<std::string, double> unit = {{"whole", 4}, {"half", 2}, {"quarter", 1}, {"eighth", 0.5}, {"16th", 0.25}};
                        auto u = unit.find(mt->childText("beat-unit", "quarter"));
                        double q = u == unit.end() ? 1 : u->second;
                        if (mt->child("beat-unit-dot")) q *= 1.5;
                        const double bpm = mt->childNum("per-minute", 0) * q;
                        if (bpm > 0) m.tempos.push_back({at, bpm});
                    }
                }
            } else if (c.tag == "sound") {
                readSound(c, m, pos);
            } else if (c.tag == "barline") {
                if (const xml::Node *r = c.child("repeat")) {
                    if (r->get("direction") == "forward") m.forward = true;
                    else { m.backward = true; m.times = std::max(2, (int)r->num("times", 2)); }
                }
                if (const xml::Node *e = c.child("ending")) {
                    const std::string type = e->get("type");
                    if (type == "start") { openEnding = endingNumbers(e->get("number")); m.ending = openEnding; m.endingStart = true; }
                    else if (type == "stop" || type == "discontinue") { if (m.ending.empty()) m.ending = endingNumbers(e->get("number")); m.endingStop = true; openEnding.clear(); }
                }
                if (c.child("segno")) m.segno = true;
                if (c.child("coda")) m.coda = true;
            } else if (c.tag == "note") {
                const double dur = c.childNum("duration", 0) / divisions;
                if (c.child("grace")) { ++graceSkipped; continue; }
                if (c.child("cue")) continue;
                const bool chord = c.child("chord") != nullptr;
                const double start = chord ? lastStart : pos;
                if (!chord) { lastStart = pos; pos += dur; }
                m.length = std::max(m.length, pos);
                if (c.child("rest")) continue;
                NoteEv n;
                n.at = start;
                n.dur = dur;
                n.voice = (int)c.childNum("voice", 1);
                if (const xml::Node *pt = c.child("pitch")) {
                    static const int steps[] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
                    const std::string st = pt->childText("step", "C");
                    const int s = !st.empty() && st[0] >= 'A' && st[0] <= 'G' ? steps[st[0] - 'A'] : 0;
                    n.key = (int)(pt->childNum("octave", 4) + 1) * 12 + s + (int)std::lround(pt->childNum("alter", 0)) + transpose;
                } else if (const xml::Node *up = c.child("unpitched")) {
                    part.percussion = true;
                    const xml::Node *inst = c.child("instrument");
                    auto it = inst ? part.unpitched.find(inst->get("id")) : part.unpitched.end();
                    if (it != part.unpitched.end()) n.key = it->second;
                    else {
                        static const int steps[] = {9, 11, 0, 2, 4, 5, 7};
                        const std::string st = up->childText("display-step", "C");
                        n.key = (int)(up->childNum("display-octave", 4) + 1) * 12 + (st.empty() || st[0] < 'A' || st[0] > 'G' ? 0 : steps[st[0] - 'A']);
                    }
                } else continue;
                if (n.key < 0 || n.key > 127) continue;
                if (c.attr("dynamics")) n.dynAttr = c.num("dynamics", 100);
                for (const xml::Node *t : c.all("tie")) {
                    if (t->get("type") == "start") n.tieStart = true;
                    if (t->get("type") == "stop") n.tieStop = true;
                }
                if (const xml::Node *nt = c.child("notations")) {
                    for (const xml::Node *t : nt->all("tied")) {
                        if (t->get("type") == "start") n.tieStart = true;
                        if (t->get("type") == "stop") n.tieStop = true;
                    }
                    if (const xml::Node *ar = nt->child("articulations"))
                        for (auto &a : ar->children) {
                            n.marks.push_back(a->tag);
                            if (a->tag == "staccato") n.length = std::min(n.length, 0.5);
                            else if (a->tag == "staccatissimo") n.length = std::min(n.length, 0.3);
                            else if (a->tag == "spiccato") n.length = std::min(n.length, 0.4);
                            else if (a->tag == "accent") n.accent = std::max(n.accent, 0.12);
                            else if (a->tag == "strong-accent") n.accent = std::max(n.accent, 0.18);
                            else if (a->tag == "detached-legato") n.length = std::min(n.length, 0.75);
                        }
                    if (const xml::Node *orn = nt->child("ornaments"))
                        for (auto &o : orn->children) if (o->tag != "accidental-mark") n.marks.push_back(o->tag);
                    if (const xml::Node *tech = nt->child("technical"))
                        for (auto &t : tech->children) if (t->tag == "pizzicato" || t->tag == "harmonic" || t->tag == "snap-pizzicato" || t->tag == "open-string") n.marks.push_back(t->tag);
                    if (nt->child("fermata")) n.marks.push_back("fermata");
                    if (const xml::Node *dyn = nt->child("dynamics"))
                        for (auto &mk : dyn->children) {
                            const double lv = dynamicLevel(mk->tag);
                            if (lv >= 0) m.dynamics.push_back({start, lv});
                            else n.accent = std::max(n.accent, 0.15);   // sf, sfz, fz, rfz ...
                        }
                }
                m.notes.push_back(n);
            }
        }
        m.nominal = beatsPerMeasure;
        part.measures.push_back(std::move(m));
    }
}

// the order measures are played in: repeats, endings, D.C./D.S. al Fine / al Coda
std::vector<size_t> playOrder(const std::vector<Measure> &ms, std::vector<std::string> &notes) {
    std::vector<size_t> order;
    const size_t n = ms.size();
    size_t i = 0, repeatStart = 0;
    int pass = 1;
    bool jumped = false;
    auto find = [&](auto pred, size_t from) { for (size_t k = from; k < n; ++k) if (pred(ms[k])) return k; return n; };
    while (i < n && order.size() < 20000) {
        const Measure &m = ms[i];
        if (m.forward && i != repeatStart) { repeatStart = i; pass = 1; }
        if (!m.ending.empty() && (m.endingStart || i == 0 || ms[i - 1].ending != m.ending)) {   // the first measure of an ending
            size_t k = i;   // just past the ending
            while (k < n && ms[k].ending == m.ending && !(k > i && ms[k].endingStart)) { if (ms[k].endingStop) { ++k; break; } ++k; }
            // after a D.C./D.S. only the last ending plays; before it, the one for this pass
            const bool play = jumped ? !(k < n && ms[k].endingStart) : m.ending.count(pass) > 0;
            if (!play) { i = k; continue; }
        }
        order.push_back(i);
        if (jumped && m.fine) break;
        if (jumped && m.toCoda) {
            const size_t c = find([](const Measure &x) { return x.coda; }, i + 1);
            if (c < n) { i = c; continue; }
        }
        if (m.backward && !jumped) {
            if (pass < m.times) { ++pass; i = repeatStart; continue; }
            pass = 1;
            repeatStart = i + 1;
        }
        if ((m.daCapo || m.dalSegno) && !jumped) {
            jumped = true;
            size_t to = 0;
            if (m.dalSegno) { to = find([](const Measure &x) { return x.segno; }, 0); if (to >= n) to = 0; }
            i = to;   // repeats are not taken again after the jump
            continue;
        }
        ++i;
    }
    if (order.size() >= 20000) notes.push_back("the repeats did not resolve (over 20000 measures); stopped there");
    return order;
}

bool loadScore(const std::string &path, std::string &text, std::string &err) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) { err = "cannot read " + path; return false; }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (text.size() < 4 || text.compare(0, 2, "PK") != 0) return true;
    Zip z;   // .mxl: META-INF/container.xml names the score
    if (!z.open(path, err)) return false;
    std::vector<uint8_t> c;
    std::string rootfile;
    if (z.read("META-INF/container.xml", c, err)) {
        std::string e2;
        auto root = xml::parse(std::string(c.begin(), c.end()), e2);
        std::vector<const xml::Node *> rf;
        if (root) root->walk("rootfile", rf);
        for (auto *r : rf) {
            const std::string mt = r->get("media-type");
            if (mt.empty() || mt.find("musicxml") != std::string::npos) { rootfile = r->get("full-path"); break; }
        }
    }
    if (rootfile.empty())
        for (auto &nm : z.names()) {
            const std::string l = lower(nm);
            if (l.rfind("meta-inf/", 0) != 0 && (fs::path(l).extension() == ".xml" || fs::path(l).extension() == ".musicxml")) { rootfile = nm; break; }
        }
    std::vector<uint8_t> x;
    if (rootfile.empty() || !z.read(rootfile, x, err)) { err = path + ": no MusicXML score inside the .mxl"; return false; }
    text.assign(x.begin(), x.end());
    return true;
}

} // namespace

bool importMusicXml(const std::string &path, const std::string &outDir, const std::string &instrument, MusicXmlImport &res, std::string &err) {
    std::string text;
    if (!loadScore(path, text, err)) return false;
    std::string perr;
    auto root = xml::parse(text, perr);
    if (!root) { err = path + ": " + perr; return false; }
    if (root->tag == "score-timewise") { err = path + " is a timewise score; export it as partwise MusicXML (the usual kind)"; return false; }
    if (root->tag != "score-partwise") { err = path + " is not a MusicXML score (root <" + root->tag + ">)"; return false; }

    // part list: names, MIDI programs and channels, percussion keys
    std::vector<Part> parts;
    std::map<std::string, size_t> partIndex;
    if (const xml::Node *pl = root->child("part-list"))
        for (const xml::Node *sp : pl->all("score-part")) {
            Part p;
            p.id = sp->get("id");
            p.name = sp->childText("part-name");
            if (const xml::Node *si = sp->child("score-instrument")) p.instrumentName = si->childText("instrument-name");
            for (const xml::Node *mi : sp->all("midi-instrument")) {
                if (mi->child("midi-channel") && p.channel < 0) p.channel = (int)mi->childNum("midi-channel", 1) - 1;
                if (mi->child("midi-program") && p.program < 0) p.program = (int)mi->childNum("midi-program", 1) - 1;
                if (mi->child("pan")) p.pan = std::clamp(mi->childNum("pan", 0) / 90.0, -1.0, 1.0);
                if (mi->child("midi-unpitched")) p.unpitched[mi->get("id")] = (int)mi->childNum("midi-unpitched", 1) - 1;
            }
            if (p.channel == 9) p.percussion = true;
            partIndex[p.id] = parts.size();
            parts.push_back(std::move(p));
        }
    size_t graceSkipped = 0;
    for (const xml::Node *pn : root->all("part")) {
        auto it = partIndex.find(pn->get("id"));
        if (it == partIndex.end()) { partIndex[pn->get("id")] = parts.size(); parts.push_back(Part{}); parts.back().id = pn->get("id"); it = partIndex.find(pn->get("id")); }
        readPart(*pn, parts[it->second], graceSkipped);
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Part &p) { return p.measures.empty(); }), parts.end());
    if (parts.empty()) { err = path + " has no parts with measures"; return false; }

    // one measure structure for all parts (the first part's barlines), lengths the longest of any part
    size_t count = parts[0].measures.size();
    for (auto &p : parts) if (p.measures.size() != count) { res.notes.push_back("parts have different measure counts; the shortest sets the length"); break; }
    for (auto &p : parts) count = std::min(count, p.measures.size());
    std::vector<Measure> shape(parts[0].measures.begin(), parts[0].measures.begin() + (long)count);
    std::vector<double> length(count, 0);
    for (size_t k = 0; k < count; ++k) {
        for (auto &p : parts) length[k] = std::max(length[k], p.measures[k].length);
        if (length[k] <= 0) length[k] = shape[k].nominal;   // an empty measure: its time signature's length
    }
    const std::vector<size_t> order = playOrder(shape, res.notes);
    res.measures = count;
    res.playedMeasures = order.size();
    for (auto &m : shape) if (m.daCapo || m.dalSegno) { res.notes.push_back("D.C./D.S. played once, repeats not taken after the jump"); break; }
    if (graceSkipped) res.notes.push_back(std::to_string(graceSkipped) + " grace note(s) left out");

    std::vector<double> startOf(order.size());
    double t = 0;
    for (size_t o = 0; o < order.size(); ++o) { startOf[o] = t; t += length[order[o]]; }

    // tempo, markers and keys from every part, in playing order
    std::map<double, double> tempoAt;
    json markers = json::array(), keys = json::array();
    std::set<std::string> seenRehearsal;
    std::string lastKey;
    for (size_t o = 0; o < order.size(); ++o) {
        const size_t k = order[o];
        for (auto &p : parts) {
            for (auto &[at, bpm] : p.measures[k].tempos) if (!tempoAt.count(r6(startOf[o] + at))) tempoAt[r6(startOf[o] + at)] = bpm;
            for (auto &[at, name] : p.measures[k].rehearsals)
                if (seenRehearsal.insert(std::to_string(o) + name).second) markers.push_back({{"beat", r6(startOf[o] + at)}, {"name", name}});
        }
        const Measure &m0 = parts[0].measures[k];
        if (m0.hasKey && !m0.mode.empty()) {
            const std::string kn = keyName(m0.fifths, m0.mode);
            if (!kn.empty() && kn != lastKey) { keys.push_back({{"beat", r6(startOf[o])}, {"key", kn}}); lastKey = kn; }
        }
    }
    bool keyWithoutMode = false;
    for (auto &m : parts[0].measures) if (m.hasKey && m.mode.empty()) keyWithoutMode = true;
    if (keyWithoutMode) res.notes.push_back("key signatures without a mode (major/minor) are not written to \"keys\"; lint --harmony detects the key");
    json job = json::object();
    if (tempoAt.empty()) { job["tempo"] = 120; res.notes.push_back("the score has no tempo: 120 bpm"); }
    else {
        json map = json::array();
        for (auto &[b, bpm] : tempoAt)
            if (map.empty() || std::fabs(map.back()["bpm"].get<double>() - bpm) > 1e-6) map.push_back({{"beat", map.empty() ? 0.0 : b}, {"bpm", r6(bpm)}});
        job["tempo"] = map.size() == 1 ? map[0]["bpm"] : map;
    }
    // the first time signature
    {
        const xml::Node *first = nullptr;
        std::vector<const xml::Node *> times;
        root->walk("time", times);
        for (auto *tm : times) if (tm->child("beats")) { first = tm; break; }
        if (first) {
            int beats = 0;
            for (int b : endingNumbers(first->childText("beats"))) beats += b;
            job["timeSignature"] = {beats > 0 ? beats : 4, (int)first->childNum("beat-type", 4)};
            std::set<std::string> distinct;
            for (auto *tm : times) if (tm->child("beats")) distinct.insert(tm->childText("beats") + "/" + tm->childText("beat-type"));
            if (distinct.size() > 1) res.notes.push_back("time signature changes are kept in the timing but not in \"timeSignature\" (Wavelength keeps one)");
        }
    }
    if (!markers.empty()) job["markers"] = markers;
    if (!keys.empty()) job["keys"] = keys;

    // tracks
    json tracks = json::array();
    std::set<std::string> used;
    std::map<int, std::string> soundOf;
    int nextChannel = 0;
    for (auto &p : parts) {
        // dynamics timeline in playing order: level marks and hairpins
        std::vector<std::pair<double, double>> levels;       // beat, level
        std::vector<std::pair<double, int>> wedges;          // beat, +1/-1/0
        for (size_t o = 0; o < order.size(); ++o) {
            const Measure &m = p.measures[order[o]];
            for (auto &[at, lv] : m.dynamics) levels.push_back({startOf[o] + at, lv});
            for (auto &[at, w] : m.wedges) wedges.push_back({startOf[o] + at, w});
        }
        std::stable_sort(levels.begin(), levels.end(), [](auto &a, auto &b) { return a.first < b.first; });
        const double defaultLevel = 0.63;
        auto levelAt = [&](double b) {   // the last mark at or before b
            double lv = defaultLevel;
            for (auto &[at, l] : levels) { if (at > b + 1e-9) break; lv = l; }
            return lv;
        };
        auto levelWithWedges = [&](double b) {   // inside a hairpin: from the level at its start toward the next mark (or +-0.15)
            for (size_t w = 0; w < wedges.size(); ++w) {
                if (wedges[w].second == 0 || wedges[w].first > b) continue;
                size_t s = w + 1;
                while (s < wedges.size() && wedges[s].second != 0) ++s;
                const double end = s < wedges.size() ? wedges[s].first : wedges[w].first + 4;
                if (b > end) continue;
                const double from = levelAt(wedges[w].first);
                double to = from + 0.15 * wedges[w].second;
                for (auto &[at, l] : levels) if (at >= end - 1e-9 && at <= end + 4) { to = l; break; }
                const double f = end > wedges[w].first ? (b - wedges[w].first) / (end - wedges[w].first) : 1;
                return from + (to - from) * std::clamp(f, 0.0, 1.0);
            }
            return levelAt(b);
        };
        json notes = json::array();
        std::map<std::pair<int, int>, size_t> tieOpen;   // (key, voice) -> index in notes
        for (size_t o = 0; o < order.size(); ++o) {
            const Measure &m = p.measures[order[o]];
            for (const NoteEv &n : m.notes) {
                const double beat = startOf[o] + n.at;
                const auto tk = std::make_pair(n.key, n.voice);
                auto it = tieOpen.find(tk);
                if (n.tieStop && it != tieOpen.end()) {   // continues a tied note
                    json &prev = notes[it->second];
                    prev["dur"] = r6((beat + n.dur * n.length) - prev["beat"].get<double>());
                    if (!n.tieStart) tieOpen.erase(it);
                    continue;
                }
                double vel = n.dynAttr >= 0 ? n.dynAttr * 0.9 / 127.0 : levelWithWedges(beat);
                vel = std::clamp(vel + n.accent, 0.05, 1.0);
                json j = {{"beat", r6(beat)}, {"dur", r6(std::max(0.01, n.dur * n.length))}, {"key", n.key}, {"vel", std::round(vel * 1000) / 1000}};
                if (!n.marks.empty()) j["marks"] = n.marks;
                if (n.tieStart) tieOpen[tk] = notes.size();
                notes.push_back(j);
                ++res.noteCount;
            }
        }
        if (notes.empty()) continue;
        std::stable_sort(notes.begin(), notes.end(), [](const json &a, const json &b) { return a["beat"].get<double>() < b["beat"].get<double>(); });
        std::string name = !p.name.empty() && lower(p.name) != "musicxml part" ? p.name : !p.instrumentName.empty() ? p.instrumentName : "Part " + p.id;
        std::string unique = name;
        for (int k = 2; used.count(unique); ++k) unique = name + " " + std::to_string(k);
        used.insert(unique);
        json tj = {{"name", unique}};
        const int program = p.program >= 0 ? p.program : programFromName(name + " " + p.instrumentName);
        if (p.percussion) tj["plugin"] = "builtin:drums";
        else if (!instrument.empty()) tj["plugin"] = instrument;
        else {
            if (!soundOf.count(program)) soundOf[program] = gmProgramSound(program);
            if (!soundOf[program].empty()) { tj["plugin"] = "builtin:sampler"; tj["sampler"] = {{"multisample", soundOf[program]}}; }
            else {
                tj["plugin"] = "builtin:drums";
                res.notes.push_back(unique + ": no sound for " + gmProgramName(program) + " in the sample library; set \"plugin\" (builtin:drums stands in)");
            }
        }
        if (!p.percussion) tj["midiProgram"] = program;
        int channel = p.channel;
        if (channel < 0 && p.percussion) channel = 9;
        if (channel < 0) { if (nextChannel == 9) ++nextChannel; channel = std::min(15, nextChannel++); }
        tj["midiChannel"] = channel;   // kept for export
        if (std::fabs(p.pan) > 1e-3) tj["pan"] = std::round(p.pan * 100) / 100;
        tj["notes"] = notes;
        tracks.push_back(tj);
        ++res.tracks;
    }
    if (tracks.empty()) { err = path + " has no notes"; return false; }
    job["tracks"] = tracks;
    res.job = job;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::ofstream o(fs::path(outDir) / "job.json");
    o << job.dump(1) << "\n";
    if (!o) { err = "cannot write " + (fs::path(outDir) / "job.json").string(); return false; }
    return true;
}

} // namespace wl
