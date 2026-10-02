#include "stage.hpp"

#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

// Role targets: the post-fader loudness each kind of part sits at (LUFS, integrated over where it plays). Checked
// in order, so the more specific roles come first ("Snare Roll" is a roll, "Lead Layer" a layer, "Basses" strings).
// The levels are the ones that balanced three trance and synth songs (Sixfold Sky, Transmission '99 and 002).
struct RoleRule { const char *name; double lufs; std::set<std::string> words; };
const std::vector<RoleRule> &rules() {
    static const std::vector<RoleRule> r = {
        {"Snare roll", -19.5, {"roll", "rolls", "fill", "fills"}},
        {"Kick", -12, {"kick", "kicks", "bd"}},
        {"Clap", -17.5, {"clap", "claps", "snare", "snares", "sd"}},
        {"Hats", -20, {"hat", "hats", "hihat", "hihats", "hh"}},
        {"Ride", -24, {"ride"}},
        {"Crash", -22, {"crash", "cymbal", "cymbals", "cym"}},
        {"Percussion", -25, {"perc", "percs", "percussion", "shaker", "shakers", "tamb", "tambourine", "tom", "toms", "conga", "congas",
                             "bongo", "bongos", "cowbell", "rim", "rimshot"}},
        {"Drums", -15, {"drums", "drum", "kit", "beat", "beats", "break", "breaks", "groove"}},
        {"FX", -22, {"fx", "sfx", "riser", "risers", "impact", "impacts", "sweep", "sweeps", "noise", "swell", "swells", "shepard",
                     "uplifter", "downlifter", "whoosh", "transition"}},
        {"Sub", -19, {"sub", "subs"}},
        {"Strings", -20, {"violin", "violins", "viola", "violas", "cello", "cellos", "celli", "basses", "contrabass", "orchestra", "orch"}},
        {"Bass", -15.5, {"bass", "bassline", "reese", "808"}},
        {"Acid", -18, {"acid", "303"}},
        {"Layer", -21, {"layer", "layers", "double", "doubles", "octave", "unison"}},
        {"Vocal", -18, {"vox", "vocal", "vocals", "voice", "voices", "talkbox"}},
        {"Lead", -16, {"lead", "leads", "hook", "melody", "theme", "solo", "riff", "riffs", "synth"}},
        {"Arp", -21, {"arp", "arps", "arpeggio", "pluck", "plucks", "seq", "sequence"}},
        {"Pad", -22, {"pad", "pads", "choir", "chords", "chord", "strings", "string", "atmos", "atmosphere", "drone", "texture"}},
        {"Keys", -18, {"piano", "keys", "rhodes", "organ", "epiano", "wurli", "clav", "clavinet", "harpsichord"}},
        {"Brass", -17, {"brass", "horn", "horns", "trumpet", "trumpets", "trombone", "trombones", "tuba"}},
        {"Bells", -22, {"bell", "bells", "glock", "glockenspiel", "mallet", "mallets", "celeste", "marimba", "vibes", "xylophone"}},
        {"Guitar", -19, {"guitar", "guitars", "gtr"}},
        {"Stab", -20, {"stab", "stabs"}},
    };
    return r;
}

std::vector<std::string> words(const std::string &s) {
    std::vector<std::string> out;
    std::string w;
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = i < s.size() ? (char)std::tolower((unsigned char)s[i]) : ' ';
        if (std::isalnum((unsigned char)c)) { w += c; continue; }
        if (!w.empty()) out.push_back(w);
        w.clear();
    }
    return out;
}

double r1(double v) { return std::round(v * 10) / 10; }

bool readJsonFile(const fs::path &p, json &out) {
    std::ifstream in(p);
    if (!in) return false;
    out = json::parse(in, nullptr, false);
    return !out.is_discarded();
}

// a target value: a number (LUFS) or {"lufs": -12, "peak": -1}
bool targetOf(const json &v, double &lufs, double &peak) {
    peak = NAN;
    if (v.is_number()) { lufs = v.get<double>(); return true; }
    if (v.is_object() && v.contains("lufs") && v["lufs"].is_number()) {
        lufs = v["lufs"].get<double>();
        if (v.contains("peak") && v["peak"].is_number()) peak = v["peak"].get<double>();
        return true;
    }
    return false;
}

} // namespace

Role roleOf(const std::string &trackName, const std::string &plugin, bool kit) {
    const auto ws = words(trackName);
    for (const auto &r : rules())
        for (const auto &w : ws)
            if (r.words.count(w)) return {r.name, r.lufs};
    if (plugin == "builtin:drums" || kit) return {"Drums", -15};
    if (plugin == "builtin:fx" || plugin == "builtin:shepard") return {"FX", -22};
    return {};
}

bool stageGains(const StageOptions &opt, json &result, std::string &err) {
    std::error_code ec;
    const fs::path jobPath = fs::absolute(opt.job, ec), dir = jobPath.parent_path();
    json job;
    if (!readJsonFile(jobPath, job) || !job.is_object()) { err = "cannot read " + opt.job; return false; }

    // targets: role defaults, then targets.json beside the job, then --targets
    json given = json::object();
    std::vector<std::string> sources;
    for (const fs::path &p : {dir / "targets.json", opt.targets.empty() ? fs::path() : fs::path(opt.targets)}) {
        if (p.empty() || !fs::exists(p, ec)) {
            if (!p.empty() && p != dir / "targets.json") { err = "no targets file " + p.string(); return false; }
            continue;
        }
        json t;
        if (!readJsonFile(p, t) || !t.is_object()) { err = p.string() + " is not a JSON object of track name -> LUFS"; return false; }
        for (auto &[k, v] : t.items()) given[k] = v;
        sources.push_back(p.string());
    }

    // measure: an existing report, or one render (no stems, picture or deliveries; tracks cached for the next render)
    json report;
    std::string reportPath;
    if (!opt.report.empty()) {
        reportPath = opt.report;
        if (!readJsonFile(reportPath, report) || !report.contains("tracks")) { err = "cannot read the report " + reportPath; return false; }
    } else {
        const fs::path out = opt.out.empty() ? dir / "out" / "stage" : fs::path(opt.out);
        std::vector<std::string> args = {platform::selfExecutable(), "render", jobPath.string(), "--out", out.string(), "--stems", "none",
                                         "--no-png", "--deliver", "none", "--cache", "--json"};
        if (opt.jobs >= 0) { args.push_back("--jobs"); args.push_back(std::to_string(opt.jobs)); }
        platform::Process p;
        if (!platform::spawn(args, p, true, !opt.verbose)) { err = "cannot start the render"; return false; }
        std::string text, crash;
        platform::readOutput(p, text, 24 * 3600);
        while (!platform::finished(p, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        report = json::parse(text, nullptr, false);
        if (!report.is_object() || !report.value("ok", false) || !report.contains("tracks")) {
            const std::string e = report.is_object() ? report.value("error", std::string()) : std::string();
            err = "the render failed" + (!e.empty() ? ": " + e : crash.empty() ? std::string() : " (" + crash + ")");
            return false;
        }
        reportPath = (out / "report.json").string();
        fs::remove(out / "mix.wav", ec);   // only the numbers are wanted
    }

    std::map<std::string, json> measured;
    for (auto &t : report["tracks"]) measured[t.value("name", "")] = t;
    std::map<std::string, json> measuredBus;
    for (auto &b : report.value("buses", json::array())) measuredBus[b.value("name", "")] = b;

    json gainsOut = json::object(), tracks = json::array(), buses = json::array(), warnings = json::array();
    std::vector<std::string> noTarget, silent;
    json &jt = job["tracks"];
    for (size_t i = 0; jt.is_array() && i < jt.size(); ++i) {
        json &t = jt[i];
        const std::string name = t.value("name", "track" + std::to_string(i + 1)), plugin = t.value("plugin", "");
        const bool kit = t.contains("sampler") && t["sampler"].is_object() && (t["sampler"].contains("kit") || t["sampler"].contains("map"));
        double target = NAN, peakCap = NAN;
        std::string role;
        if (given.contains(name) && targetOf(given[name], target, peakCap)) role = "targets";
        else {
            const Role r = roleOf(name, plugin, kit);
            if (!r.name.empty()) {
                role = r.name;
                if (!given.contains("role:" + r.name) || !targetOf(given["role:" + r.name], target, peakCap)) target = r.lufs;
            }
        }
        if (role.empty()) { noTarget.push_back(name); continue; }
        if (!measured.count(name)) continue;
        const json &m = measured[name];
        const double lufs = m.value("lufs", -120.0);
        const double peak = m.contains("levels") ? m["levels"].value("peakDb", -120.0) : -120.0;
        if (lufs <= -70) { silent.push_back(name); continue; }
        double gain = target - lufs;
        bool capped = false;
        if (!std::isnan(peakCap) && peak > -100 && peakCap - peak < gain) { gain = peakCap - peak; capped = true; }
        gain = r1(gain);
        const double old = t.contains("gain") && t["gain"].is_number() ? t["gain"].get<double>() : 0.0;
        gainsOut[name] = gain;
        json row = {{"name", name}, {"role", role}, {"target", r1(target)}, {"lufs", r1(lufs)}, {"gain", gain}, {"was", r1(old)},
                    {"peakAfterFaderDb", r1(peak + gain)}};
        if (capped) row["cappedByPeak"] = r1(peakCap);
        tracks.push_back(row);
        if (peak + gain > 0) {
            char buf[420];
            std::snprintf(buf, sizeof buf, "'%s' peaks at %+.1f dBFS after its fader (%+.1f dB): the master limiter will work on its transients; "
                          "tame them on the track (a clip or limiter), or cap the fader: {\"%s\": {\"lufs\": %.1f, \"peak\": -1}} in targets.json",
                          name.c_str(), peak + gain, gain, name.c_str(), target);
            warnings.push_back(buf);
        }
        if (opt.apply) t["gain"] = gain;
    }
    json &jb = job["buses"];
    for (size_t i = 0; jb.is_array() && i < jb.size(); ++i) {
        json &b = jb[i];
        const std::string name = b.value("name", ""), key = "bus:" + name;
        double target = NAN, peakCap = NAN;
        if (!given.contains(key) || !targetOf(given[key], target, peakCap) || !measuredBus.count(name)) continue;
        const double lufs = measuredBus[name].value("lufs", -120.0);
        if (lufs <= -70) { silent.push_back(key); continue; }
        const double gain = r1(target - lufs);
        gainsOut[key] = gain;
        buses.push_back({{"name", name}, {"target", r1(target)}, {"lufs", r1(lufs)}, {"gain", gain},
                         {"was", r1(b.contains("gain") && b["gain"].is_number() ? b["gain"].get<double>() : 0.0)}});
        if (opt.apply) b["gain"] = gain;
    }
    auto list = [](const std::vector<std::string> &v) { std::string s; for (auto &x : v) s += (s.empty() ? "'" : "', '") + x; return s + "'"; };
    if (!noTarget.empty())
        warnings.push_back("no target for " + list(noTarget) + " (no word of the name says a role): their faders stay; give them one in targets.json");
    if (!silent.empty()) warnings.push_back(list(silent) + " made no sound in the render: their faders stay");

    // write: gains.json (merged with what's there), and with --apply the job itself
    std::string gainsFile;
    if (!opt.dryRun) {
        const fs::path gp = opt.write.empty() ? dir / "gains.json" : fs::path(opt.write);
        json merged = json::object();
        if (fs::exists(gp, ec) && (!readJsonFile(gp, merged) || !merged.is_object())) { err = gp.string() + " is not a JSON object"; return false; }
        for (auto &[k, v] : gainsOut.items()) merged[k] = v;
        std::ofstream(gp) << merged.dump(1) << "\n";
        gainsFile = gp.string();
        if (opt.apply) {
            std::ofstream(jobPath) << job.dump(1) << "\n";
            for (const char *gen : {"make-job.py", "make.py", "make-set.py"})
                if (fs::exists(dir / gen, ec)) {
                    warnings.push_back(std::string("job.json is written by ") + gen + ": its next run drops the applied gains; read gains.json there instead");
                    break;
                }
        }
    }
    result = {{"ok", true}, {"report", reportPath}, {"targets", sources}, {"gainsFile", gainsFile.empty() ? json(nullptr) : json(gainsFile)},
              {"applied", opt.apply && !opt.dryRun}, {"tracks", tracks}, {"buses", buses}, {"warnings", warnings}};
    return true;
}

} // namespace wl
