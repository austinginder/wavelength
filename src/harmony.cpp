#include "harmony.hpp"

#include "analyze.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace wl {

using json = nlohmann::json;

namespace {

// Krumhansl-Kessler key profiles
const double kMajor[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
const double kMinor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

using PC = std::array<double, 12>;

// Note names spelled for a key: sharp keys (G D A E B F# C# major and their relative minors) use sharps
bool sharpKey(int tonic, bool minor) {
    const int major = minor ? (tonic + 3) % 12 : tonic;
    return major == 7 || major == 2 || major == 9 || major == 4 || major == 11 || major == 6 || major == 1;
}
std::string pcName(int pc, bool sharps) {
    static const char *sh[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char *fl[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    return (sharps ? sh : fl)[((pc % 12) + 12) % 12];
}
// a chord's tones spelled from its root's letter: F# major is F# A# C#, Eb major is Eb G Bb
std::vector<std::string> spellChord(int root, const std::vector<int> &intervals, bool sharps) {
    static const int naturals[7] = {0, 2, 4, 5, 7, 9, 11};
    static const char letters[7] = {'C', 'D', 'E', 'F', 'G', 'A', 'B'};
    const std::string rn = pcName(root, sharps);
    int li = 0;
    while (letters[li] != rn[0]) ++li;
    std::vector<std::string> out;
    for (int iv : intervals) {
        const int steps = iv <= 2 ? (iv == 0 ? 0 : 1) : iv <= 4 ? 2 : iv == 5 ? 3 : iv <= 7 ? 4 : iv <= 9 ? 5 : 6;
        const int L = (li + steps) % 7, pc = (root + iv) % 12;
        int acc = ((pc - naturals[L]) % 12 + 12) % 12;
        if (acc > 6) acc -= 12;
        std::string n(1, letters[L]);
        for (int a = 0; a < acc; ++a) n += '#';
        for (int a = 0; a > acc; --a) n += 'b';
        out.push_back(n);
    }
    return out;
}

double correlate(const PC &x, const double *profile, int tonic) {
    double mx = 0, mp = 0;
    for (int i = 0; i < 12; ++i) { mx += x[(size_t)((i + tonic) % 12)]; mp += profile[i]; }
    mx /= 12; mp /= 12;
    double num = 0, dx = 0, dp = 0;
    for (int i = 0; i < 12; ++i) {
        const double a = x[(size_t)((i + tonic) % 12)] - mx, b = profile[i] - mp;
        num += a * b; dx += a * a; dp += b * b;
    }
    return dx > 0 && dp > 0 ? num / std::sqrt(dx * dp) : 0;
}

const char *kModes[6] = {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian"};

// Pitch classes that belong to a key. Minor: natural minor plus the raised 6th and 7th (melodic and
// harmonic minor: the leading tone and the major V). Major: the major scale plus the raised 5th
// (the relative minor's leading tone, the III/V-of-vi chord). Declared modes: their scale, plus the
// leading tone for dorian (its major V).
bool inKey(int pc, int tonic, bool minor, int mode = -1) {
    static const std::set<int> sets[6] = {{0, 2, 4, 5, 7, 8, 9, 11}, {0, 2, 3, 5, 7, 8, 9, 10, 11}, {0, 2, 3, 5, 7, 9, 10, 11},
                                          {0, 1, 3, 5, 7, 8, 10}, {0, 2, 4, 6, 7, 9, 11}, {0, 2, 4, 5, 7, 9, 10}};
    const int m = mode >= 0 && mode < 6 ? mode : (minor ? 1 : 0);
    return sets[m].count(((pc - tonic) % 12 + 12) % 12) > 0;
}

struct Chord {
    int root = -1;
    std::string suffix;
    std::vector<int> pcs, intervals;
    bool sharps = false;   // the key's preference; a chord that would need more accidentals takes the other spelling
    bool spelling() const {
        auto count = [&](bool sh) { int c = 0; for (auto &n : spellChord(root, intervals, sh)) c += (int)n.size() - 1; return c; };
        const int mine = count(sharps), other = count(!sharps);
        return other < mine ? !sharps : sharps;
    }
    std::string name() const { return root < 0 ? "-" : pcName(root, spelling()) + suffix; }
    std::string spelled() const {
        std::string s;
        for (auto &n : spellChord(root, intervals, spelling())) s += (s.empty() ? "" : " ") + n;
        return s;
    }
};

Chord chordOf(const PC &w, int bass, bool sharps = false) {
    static const std::vector<std::pair<std::string, std::vector<int>>> shapes = {
        {"", {0, 4, 7}}, {"m", {0, 3, 7}}, {"dim", {0, 3, 6}}, {"sus4", {0, 5, 7}}, {"sus2", {0, 2, 7}}, {"5", {0, 7}}};
    double total = 0;
    for (double v : w) total += v;
    Chord best;
    if (total <= 0) return best;
    double bestScore = -1e9;
    for (int r = 0; r < 12; ++r)
        for (auto &[suffix, iv] : shapes) {
            std::set<int> pcs;
            for (int i : iv) pcs.insert((r + i) % 12);
            double in = 0, out = 0;
            for (int p = 0; p < 12; ++p) (pcs.count(p) ? in : out) += w[(size_t)p];
            double score = in - 0.6 * out - 0.05 * total * (double)iv.size() / 3;
            if (bass == r) score *= 1.15;
            if (score > bestScore) {
                bestScore = score;
                best.root = r;
                best.suffix = suffix;
                best.pcs.clear();
                best.intervals = iv;
                best.sharps = sharps;
                for (int i : iv) best.pcs.push_back((r + i) % 12);
            }
        }
    return best;
}

struct N { double b0, b1; int key; size_t track; };

std::string barBeat(double beat, double bpb) {
    const double b = std::round(beat * 1000) / 1000;
    char buf[48];
    const double bar = std::floor(b / bpb);
    std::snprintf(buf, sizeof buf, "bar %d beat %.2f", (int)bar + 1, b - bar * bpb + 1);
    return buf;
}

} // namespace

std::string keyLabel(int tonic, bool minor, int mode) {
    const int m = mode >= 0 && mode < 6 ? mode : (minor ? 1 : 0);
    return pcName(tonic, sharpKey(tonic, minor)) + " " + kModes[m];
}

bool parseKeyName(const std::string &in, int &tonic, bool &minor, std::string &err, int *modeOut) {
    std::string s;
    for (char c : in) if (!std::isspace((unsigned char)c)) s += c;
    if (s.empty()) { err = "empty key name"; return false; }
    static const std::map<char, int> letters = {{'C', 0}, {'D', 2}, {'E', 4}, {'F', 5}, {'G', 7}, {'A', 9}, {'B', 11}};
    auto it = letters.find((char)std::toupper((unsigned char)s[0]));
    if (it == letters.end()) { err = "key '" + in + "': start with a note letter A-G (\"D minor\", \"F# major\", \"Bbm\")"; return false; }
    int pc = it->second;
    size_t i = 1;
    for (; i < s.size() && (s[i] == '#' || s[i] == 'b'); ++i) pc += s[i] == '#' ? 1 : -1;
    std::string mode = s.substr(i);
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    int m = -1;
    if (mode.empty() || mode == "major" || mode == "maj" || mode == "ionian") m = 0;
    else if (mode == "m" || mode == "min" || mode == "minor" || mode == "aeolian") m = 1;
    else for (int i = 2; i < 6; ++i) if (mode == kModes[i]) m = i;
    if (m < 0) { err = "key '" + in + "': the mode is major, minor, dorian, phrygian, lydian or mixolydian"; return false; }
    minor = m == 1 || m == 2 || m == 3;
    if (modeOut) *modeOut = m;
    // "Dm" arrives lowercased as "m"; "DM" would too, but nobody writes major that way here
    tonic = ((pc % 12) + 12) % 12;
    return true;
}

json analyzeHarmony(const Job &job, const HarmonyOptions &o) {
    const double bpb = job.tsigNum * 4.0 / job.tsigDen;
    std::vector<N> notes;
    double end = 0;
    for (size_t ti : o.tracks)
        for (auto &n : job.tracks[ti].notes) {
            const double b0 = job.tempo.secToBeat(n.start), b1 = job.tempo.secToBeat(n.start + n.length);
            if (b1 <= b0) continue;
            notes.push_back({b0, b1, n.key, ti});
            end = std::max(end, b1);
        }
    json out = {{"keys", json::array()}, {"bars", json::array()}, {"problems", json::array()}, {"info", json::array()}};
    if (notes.empty()) return out;
    const int bars = (int)std::ceil(end / bpb - 1e-9);
    std::vector<std::vector<size_t>> byBar((size_t)bars);
    for (size_t i = 0; i < notes.size(); ++i) {
        const int a = std::max(0, (int)std::floor(notes[i].b0 / bpb)), z = std::min(bars - 1, (int)std::floor((notes[i].b1 - 1e-9) / bpb));
        for (int b = a; b <= z; ++b) byBar[(size_t)b].push_back(i);
    }
    // sample every 16th: each distinct sounding pitch class weighs its time, the bass gets as much again.
    // "harm" only counts steps where two or more pitch classes sound: a lone chromatic line (a walk-down,
    // a passing note on its own) is melody, not a chord outside the key.
    const double step = 0.25;
    std::vector<PC> all((size_t)bars), harm((size_t)bars), bass((size_t)bars), half0((size_t)bars), half1((size_t)bars);
    std::vector<int> bassNote((size_t)bars, -1);
    for (int b = 0; b < bars; ++b) {
        std::map<int, double> bassTime;
        for (double t = b * bpb; t < (b + 1) * bpb - 1e-9; t += step) {
            const double c = t + step / 2;
            std::set<int> pcs;
            int lo = 999;
            for (size_t i : byBar[(size_t)b])
                if (notes[i].b0 <= c && c < notes[i].b1) { pcs.insert(notes[i].key % 12); lo = std::min(lo, notes[i].key); }
            if (pcs.empty()) continue;
            PC &h = (c - b * bpb) < bpb / 2 ? half0[(size_t)b] : half1[(size_t)b];
            for (int p : pcs) {
                all[(size_t)b][(size_t)p] += step;
                h[(size_t)p] += step;
                if (pcs.size() >= 2) harm[(size_t)b][(size_t)p] += step;
            }
            all[(size_t)b][(size_t)(lo % 12)] += step;
            h[(size_t)(lo % 12)] += step;
            bass[(size_t)b][(size_t)(lo % 12)] += step;
            bassTime[lo % 12] += step;
        }
        double best = 0;
        for (auto &[p, tm] : bassTime) if (tm > best) { best = tm; bassNote[(size_t)b] = p; }
    }
    // ---- the key of every bar ----
    struct K { int tonic; bool minor; bool checks; std::string source; int mode = -1; };
    std::vector<K> key((size_t)bars);
    if (!o.keys.empty()) {
        for (int b = 0; b < bars; ++b) {
            const KeyMark *m = &o.keys.front();
            for (auto &k : o.keys) if (k.beat <= b * bpb + 1e-6) m = &k;
            key[(size_t)b] = {m->tonic, m->minor, m->checks, "declared", m->mode};
        }
    } else {
        int prev = -1;
        std::vector<bool> ambiguous((size_t)bars, false);
        for (int b = 0; b < bars; ++b) {
            PC w{}, bw{};
            for (int x = std::max(0, b - 3); x <= std::min(bars - 1, b + 4); ++x)
                for (int p = 0; p < 12; ++p) { w[(size_t)p] += all[(size_t)x][(size_t)p]; bw[(size_t)p] += bass[(size_t)x][(size_t)p]; }
            double total = 0;
            for (double v : w) total += v;
            if (total <= 0) { key[(size_t)b] = prev >= 0 ? key[(size_t)(b - 1)] : K{0, true, true, "detected"}; continue; }
            int bestK = 0;
            double bestS = -2;
            for (int k = 0; k < 24; ++k) {
                if (w[(size_t)(k % 12)] < 0.05 * total) continue;   // a key whose tonic never sounds isn't the key
                double s = correlate(w, k < 12 ? kMajor : kMinor, k % 12);
                if (k == prev) s += 0.03;   // hysteresis: a key holds until another fits clearly better
                if (s > bestS) { bestS = s; bestK = k; }
            }
            // a major key and its relative minor share their notes: the minor's leading tone, or more bass
            // on its tonic, makes it the minor
            if (bestK < 12) {
                const int rel = (bestK + 9) % 12;
                if (w[(size_t)rel] >= 0.05 * total && (w[(size_t)((rel + 11) % 12)] >= 0.03 * total || bw[(size_t)rel] > bw[(size_t)bestK])) bestK = 12 + rel;
            }
            // the mode is the third you hear over the tonic. Fewer than three pitch classes, or no third at
            // all (an open-fifth drone, a power-chord riff): the key can't be heard, so the bars take the key
            // of the music after them (or before, at the end)
            const int t = bestK % 12;
            const double m3 = w[(size_t)((t + 3) % 12)], M3 = w[(size_t)((t + 4) % 12)];
            int distinct = 0;
            for (double v : w) if (v >= 0.05 * total) ++distinct;
            if (distinct < 3 || m3 + M3 < 0.05 * total) ambiguous[(size_t)b] = true;
            else bestK = t + (m3 > M3 ? 12 : 0);
            key[(size_t)b] = {t, bestK >= 12, true, "detected"};
            prev = bestK;
        }
        for (int b = 0; b < bars; ++b) {
            if (!ambiguous[(size_t)b]) continue;
            int x = b;
            while (x < bars && ambiguous[(size_t)x]) ++x;
            int y = b - 1;
            if (x < bars) key[(size_t)b] = key[(size_t)x];
            else if (y >= 0) key[(size_t)b] = key[(size_t)y];
        }
        // a detected key has to hold for 4 bars; shorter runs join the key before them (or after, at the start)
        for (bool changed = true; changed;) {
            changed = false;
            for (int b = 0; b < bars;) {
                int e = b;
                while (e + 1 < bars && key[(size_t)(e + 1)].tonic == key[(size_t)b].tonic && key[(size_t)(e + 1)].minor == key[(size_t)b].minor) ++e;
                if (e - b + 1 < 4 && (b > 0 || e + 1 < bars)) {
                    const K fill = b > 0 ? key[(size_t)(b - 1)] : key[(size_t)(e + 1)];
                    for (int x = b; x <= e; ++x) key[(size_t)x] = fill;
                    changed = true;
                    break;
                }
                b = e + 1;
            }
        }
        // the window looks 4 bars ahead, so a detected change can start early: move each boundary (up to 4
        // bars either way) to where the bars fit the two keys best; on a tie the old key holds longer
        auto fit = [&](int b, const K &k) {
            double t = 0, in = 0;
            for (int p = 0; p < 12; ++p) { t += all[(size_t)b][(size_t)p]; if (inKey(p, k.tonic, k.minor)) in += all[(size_t)b][(size_t)p]; }
            return t > 0 ? in / t : 1.0;
        };
        for (int s0 = 1; s0 < bars; ++s0) {
            if (key[(size_t)s0].tonic == key[(size_t)(s0 - 1)].tonic && key[(size_t)s0].minor == key[(size_t)(s0 - 1)].minor) continue;
            const K ka = key[(size_t)(s0 - 1)], kb = key[(size_t)s0];
            int a0 = s0 - 1, b1 = s0;
            while (a0 > 0 && key[(size_t)(a0 - 1)].tonic == ka.tonic && key[(size_t)(a0 - 1)].minor == ka.minor) --a0;
            while (b1 + 1 < bars && key[(size_t)(b1 + 1)].tonic == kb.tonic && key[(size_t)(b1 + 1)].minor == kb.minor) ++b1;
            const int lo = std::max(a0 + 1, s0 - 4), hi = std::min(b1, s0 + 4);
            int best = s0;
            double bestFit = -1;
            for (int c = lo; c <= hi; ++c) {
                double f = 0;
                for (int x = lo - 1; x <= hi; ++x) f += fit(x, x < c ? ka : kb);
                if (f >= bestFit - 1e-9) { bestFit = f; best = c; }
            }
            for (int x = lo - 1; x <= hi; ++x) key[(size_t)x] = x < best ? ka : kb;
            s0 = std::max(s0, best);
        }
    }
    for (int b = 0; b < bars;) {
        int e = b;
        while (e + 1 < bars && key[(size_t)(e + 1)].tonic == key[(size_t)b].tonic && key[(size_t)(e + 1)].minor == key[(size_t)b].minor &&
               key[(size_t)(e + 1)].checks == key[(size_t)b].checks && key[(size_t)(e + 1)].mode == key[(size_t)b].mode) ++e;
        json k = {{"from", b + 1}, {"to", e + 1}, {"key", keyLabel(key[(size_t)b].tonic, key[(size_t)b].minor, key[(size_t)b].mode)}, {"source", key[(size_t)b].source}};
        if (!key[(size_t)b].checks) k["checks"] = false;
        out["keys"].push_back(k);
        b = e + 1;
    }
    // ---- chords and bars outside their key ----
    std::vector<Chord> chord((size_t)bars);
    std::vector<bool> outside((size_t)bars, false);
    std::vector<std::vector<int>> outPcs((size_t)bars);
    auto inRange = [&](double beat) { return beat >= o.fromBeat - 1e-6 && beat < o.toBeat - 1e-6; };
    for (int b = 0; b < bars; ++b) {
        chord[(size_t)b] = chordOf(all[(size_t)b], bassNote[(size_t)b], sharpKey(key[(size_t)b].tonic, key[(size_t)b].minor));
        const K &k = key[(size_t)b];
        double total = 0, off = 0;
        for (int p = 0; p < 12; ++p) {
            total += harm[(size_t)b][(size_t)p];
            if (!inKey(p, k.tonic, k.minor, k.mode)) off += harm[(size_t)b][(size_t)p];
        }
        if (total > 0 && off >= 0.5 && off / total >= 0.2) {
            outside[(size_t)b] = true;
            for (int p = 0; p < 12; ++p)
                if (!inKey(p, k.tonic, k.minor, k.mode) && harm[(size_t)b][(size_t)p] >= 0.1 * off) outPcs[(size_t)b].push_back(p);
        }
        json row = {{"bar", b + 1}, {"chord", chord[(size_t)b].name()}, {"key", keyLabel(k.tonic, k.minor, k.mode)}};
        const bool sk = sharpKey(key[(size_t)b].tonic, key[(size_t)b].minor);
        const Chord c0 = chordOf(half0[(size_t)b], -1, sk), c1 = chordOf(half1[(size_t)b], -1, sk);
        if (c0.root >= 0 && c1.root >= 0 && c0.name() != c1.name()) row["halves"] = {c0.name(), c1.name()};
        if (outside[(size_t)b]) {
            json names = json::array();
            for (int p : outPcs[(size_t)b]) names.push_back(pcName(p, sharpKey(k.tonic, k.minor)));
            row["outside"] = names;
        }
        if (inRange(b * bpb)) out["bars"].push_back(row);
    }
    for (int b = 0; b < bars;) {
        if (!outside[(size_t)b]) { ++b; continue; }
        int e = b;
        while (e + 1 < bars && outside[(size_t)(e + 1)] && key[(size_t)(e + 1)].tonic == key[(size_t)b].tonic) ++e;
        const K &k = key[(size_t)b];
        const int len = e - b + 1;
        const bool before = b > 0 && !outside[(size_t)(b - 1)], after = e + 1 < bars && !outside[(size_t)(e + 1)];
        if (len <= o.maxExcursionBars && before && after && k.checks && inRange(b * bpb)) {
            PC w{};
            for (int x = b; x <= e; ++x) for (int p = 0; p < 12; ++p) w[(size_t)p] += all[(size_t)x][(size_t)p];
            const Chord c = chordOf(w, bassNote[(size_t)e], sharpKey(k.tonic, k.minor));
            const Chord &next = chord[(size_t)(e + 1)];
            json names = json::array();
            std::set<int> offs;
            for (int x = b; x <= e; ++x) for (int p : outPcs[(size_t)x]) offs.insert(p);
            for (int p : offs) {   // spelled as in the chord when it is one of its tones
                const auto sp = spellChord(c.root, c.intervals, c.spelling());
                const auto at = std::find(c.pcs.begin(), c.pcs.end(), p);
                names.push_back(at != c.pcs.end() ? sp[(size_t)(at - c.pcs.begin())] : pcName(p, sharpKey(k.tonic, k.minor)));
            }
            const std::string span = len == 1 ? "bar " + std::to_string(b + 1) : "bars " + std::to_string(b + 1) + "-" + std::to_string(e + 1);
            // a major chord resolving down a fifth to a chord of the key is a secondary dominant: tension on purpose
            if ((c.suffix.empty() || c.suffix == "5") && next.root == (c.root + 5) % 12 && inKey(next.root, k.tonic, k.minor, k.mode)) {
                out["info"].push_back({{"kind", "secondary dominant"}, {"bars", {b + 1, e + 1}}, {"at", span}, {"chord", c.name()},
                                       {"detail", c.name() + " (" + c.spelled() + ") leads to " + next.name() + ": V of " + next.name() + " in " + keyLabel(k.tonic, k.minor, k.mode)}});
            } else {
                out["problems"].push_back({{"kind", "key excursion"}, {"bars", {b + 1, e + 1}}, {"at", span}, {"key", keyLabel(k.tonic, k.minor, k.mode)},
                                           {"chord", c.name()}, {"notes", names}, {"back", "bar " + std::to_string(e + 2)},
                                           {"detail", c.name() + " (" + c.spelled() + ") with " + [&] { std::string t; for (auto &x : names) t += (t.empty() ? "" : ", ") + x.get<std::string>(); return t; }() +
                                                          " is outside " + keyLabel(k.tonic, k.minor, k.mode) + " for " +
                                                          (len == 1 ? std::string("1 bar") : std::to_string(len) + " bars") + " and goes straight back at bar " +
                                                          std::to_string(e + 2) + ": it sounds like a " + (len == 1 ? "one" : std::to_string(len)) +
                                                          "-bar key change. Use a chord of the key (bVI-bVII-V climbs in minor), make it a passing note, "
                                                          "or declare the key change in \"keys\""}});
            }
        }
        b = e + 1;
    }
    // ---- clashes: two parts a minor second (or ninth) apart, both held a beat or more ----
    std::set<std::tuple<size_t, size_t, int, int, int>> seen;
    std::map<std::pair<size_t, size_t>, json> rubs;
    for (int b = 0; b < bars; ++b) {
        auto &ids = byBar[(size_t)b];
        for (size_t x = 0; x < ids.size(); ++x)
            for (size_t y = x + 1; y < ids.size(); ++y) {
                const N &p = notes[ids[x]], &q = notes[ids[y]];
                if (p.track == q.track || p.b1 - p.b0 < 1 || q.b1 - q.b0 < 1) continue;
                const double s = std::max(p.b0, q.b0), e = std::min(p.b1, q.b1);
                if (e - s < 1 || std::floor(s / bpb) != b) continue;   // report once, in the bar the overlap starts
                const N &hi = p.key > q.key ? p : q, &lo = p.key > q.key ? q : p;
                const int d = hi.key - lo.key;
                if (d <= 0 || d % 12 != 1) continue;
                if (!key[(size_t)b].checks || !inRange(s)) continue;
                const size_t ta = std::min(hi.track, lo.track), tb = std::max(hi.track, lo.track);
                if (!seen.insert({ta, tb, b, hi.key, lo.key}).second) continue;
                char held[32];
                std::snprintf(held, sizeof held, "%.1f", e - s);
                const K &k = key[(size_t)b];
                if (inKey(hi.key % 12, k.tonic, k.minor, k.mode) && inKey(lo.key % 12, k.tonic, k.minor, k.mode)) {
                    // both notes belong to the key: a rub (a major 7th voiced under its root, a suspension, a part
                    // one chord early or late). Grouped per pair of tracks; worth a look, not an error.
                    json &r = rubs[{hi.track, lo.track}];
                    if (r.is_null()) r = {{"tracks", {job.tracks[hi.track].name, job.tracks[lo.track].name}}, {"count", 0}, {"bars", json::array()},
                                          {"example", keyName(hi.key) + " over " + keyName(lo.key) + " (" + (d == 1 ? "minor 2nd" : "minor 9th") + ", " + held + " beats, " + barBeat(s, bpb) + ")"}};
                    r["count"] = r["count"].get<int>() + 1;
                    if (r["bars"].empty() || r["bars"].back().get<int>() != b + 1) r["bars"].push_back(b + 1);
                    continue;
                }
                out["problems"].push_back({{"kind", "clash"}, {"bars", {b + 1, b + 1}}, {"at", barBeat(s, bpb)},
                                           {"tracks", {job.tracks[hi.track].name, job.tracks[lo.track].name}},
                                           {"notes", {keyName(hi.key), keyName(lo.key)}},
                                           {"detail", job.tracks[hi.track].name + " " + keyName(hi.key) + " against " + job.tracks[lo.track].name + " " +
                                                          keyName(lo.key) + ": " + (d == 1 ? "minor 2nd" : "minor 9th") + " held " + held + " beats"}});
            }
    }
    out["rubs"] = json::array();
    for (auto &[pair, r] : rubs) out["rubs"].push_back(r);
    std::stable_sort(out["rubs"].begin(), out["rubs"].end(), [](const json &a, const json &b) { return a["count"].get<int>() > b["count"].get<int>(); });
    std::stable_sort(out["problems"].begin(), out["problems"].end(),
                     [](const json &a, const json &b) { return a["bars"][0].get<int>() < b["bars"][0].get<int>(); });
    return out;
}

} // namespace wl
