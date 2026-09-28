#include "purge.hpp"

#include "audio_file.hpp"
#include "encode.hpp"
#include "song.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace wl::purge {

namespace {

using json = nlohmann::json;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// The report a render wrote ("mix" and its "tracks") or `wavelength master` wrote ("input" and "output"),
// not any report.json. `mixKey` names the object that holds the mix file and its deliveries.
bool readReport(const fs::path &p, json &rep, std::string &mixKey) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    rep = json::parse(in, nullptr, false);
    if (rep.is_discarded() || !rep.is_object()) return false;
    if (rep.contains("mix") && rep["mix"].is_object() && rep.contains("tracks") && rep["tracks"].is_array()) { mixKey = "mix"; return true; }
    if (rep.contains("output") && rep["output"].is_object() && rep.contains("input") && rep.contains("masterFx")) { mixKey = "output"; return true; }
    return false;
}

std::string key(const fs::path &p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(p, ec);
    return (ec ? p.lexically_normal() : c).generic_u8string();
}

bool plainFile(const fs::path &p) {   // a regular file, not a link to one
    std::error_code ec;
    return fs::is_regular_file(fs::symlink_status(p, ec));
}

double sizeOf(const fs::path &p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : (double)n;
}

double ageSeconds(const fs::path &p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 1e12;
    return std::chrono::duration<double>(fs::file_time_type::clock::now() - t).count();
}

// The song a render folder belongs to: the nearest folder at or above it with a job.json or a
// wavelength.json, a few levels up at most (out/, shootout/out-bass/, out/preview/<id>/).
fs::path songOf(const fs::path &dir) {
    std::error_code ec;
    fs::path d = dir;
    for (int i = 0; i < 4 && !d.empty(); ++i, d = d.parent_path()) {
        if (fs::exists(d / "wavelength.json", ec) || fs::exists(d / "job.json", ec)) return d;
        if (d == d.parent_path()) break;
    }
    return {};
}

// Every file a song keeps: the ones its manifest lists (sources, notes, media, render/) and the ones
// its job uses (states, samples, clips), wherever they are.
void protect(const fs::path &songDir, std::set<std::string> &keep) {
    Song s;
    std::string err;
    if (!openSong(songDir.string(), s, err)) return;
    for (auto &f : s.manifest.value("files", json::array()))
        if (f.is_object() && f.contains("path") && f["path"].is_string()) keep.insert(key(s.dir / fs::u8path(f["path"].get<std::string>())));
    std::ifstream in(s.jobPath(), std::ios::binary);
    const json job = json::parse(in, nullptr, false);
    if (job.is_discarded()) return;
    for (auto &[where, path] : jobFileRefs(job)) {
        const fs::path p = fs::u8path(path);
        keep.insert(key(p.is_absolute() ? p : s.jobPath().parent_path() / p));
    }
}

// An MP3 of this render (not an older one left in the folder): one its report lists as a delivery, or
// one written after its mix.wav.
bool currentMp3(const fs::path &dir, const fs::path &mix, const json &out) {
    for (auto &d : out.value("deliveries", json::array()))
        if (d.is_object() && d.value("format", std::string()) == "mp3" &&
            plainFile(dir / fs::u8path(d.value("file", std::string())).filename()))
            return true;
    std::error_code ec;
    const auto mixTime = fs::last_write_time(mix, ec);
    if (ec) return false;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        std::error_code e2;
        if (lower(e.path().extension().string()) == ".mp3" && plainFile(e.path()) &&
            fs::last_write_time(e.path(), e2) + std::chrono::seconds(2) >= mixTime && !e2)
            return true;
    }
    return false;
}

std::string shown(const fs::path &p, const fs::path &root) {
    const fs::path rel = p.lexically_relative(root.parent_path());
    return (!rel.empty() && *rel.begin() != ".." ? rel : p).generic_u8string();
}

} // namespace

std::string bytesText(double bytes) {
    char b[32];
    if (bytes >= 1e9) std::snprintf(b, sizeof b, "%.1f GB", bytes / 1e9);
    else if (bytes >= 1e6) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1e6);
    else std::snprintf(b, sizeof b, "%.0f KB", bytes / 1e3);
    return b;
}

bool run(const std::vector<std::string> &folders, const Options &opt, json &r, std::string &err) {
    std::vector<fs::path> roots;
    for (auto &f : folders.empty() ? std::vector<std::string>{"."} : folders) {
        std::error_code ec;
        fs::path p = fs::absolute(fs::u8path(f), ec).lexically_normal();
        if (!p.empty() && p.filename().empty()) p = p.parent_path();   // a trailing slash
        if (!fs::is_directory(p, ec)) { err = f + " is not a folder"; return false; }
        roots.push_back(p);
    }

    // ---- find the render folders (and every WAV, to say what is left) --------------------------------
    static const std::set<std::string> skipDirs = {"history", ".git", "node_modules", ".venv", "venv", "__pycache__"};
    struct Found { fs::path dir, root; };
    std::vector<Found> renders;
    std::vector<fs::path> wavs;
    std::set<std::string> seenDirs, seenWavs;
    for (auto &root : roots) {
        auto consider = [&](const fs::path &d) {
            if (plainFile(d / "report.json") && seenDirs.insert(key(d)).second) renders.push_back({d, root});
        };
        consider(root);
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            const auto &e = *it;
            if (e.is_symlink(e2)) continue;   // links are never followed or deleted
            if (e.is_directory(e2)) {
                if (skipDirs.count(e.path().filename().string())) { it.disable_recursion_pending(); continue; }
                consider(e.path());
            } else if (e.is_regular_file(e2) && lower(e.path().extension().string()) == ".wav" && seenWavs.insert(key(e.path())).second)
                wavs.push_back(e.path());
        }
    }
    std::sort(renders.begin(), renders.end(), [](const Found &a, const Found &b) { return a.dir < b.dir; });

    std::set<std::string> keep, songsSeen;
    for (auto &f : renders) {
        const fs::path s = songOf(f.dir);
        if (!s.empty() && songsSeen.insert(key(s)).second) protect(s, keep);
    }

    // ---- each render folder ------------------------------------------------------------------------------
    static const std::regex stemName(R"(^[0-9]{2,}-.+\.wav$)", std::regex::icase);
    json list = json::array(), skipped = json::array();
    std::map<std::string, size_t> at;   // render folder -> its entry in `list` (previews fold into their render)
    std::set<std::string> renderKeys;   // every file found as render output (purged or skipped): not "left alone"
    double freed = 0, masterBytes = 0;
    int mp3s = 0, masters = 0;
    for (auto &f : renders) {
        const fs::path &R = f.dir;
        json rep;
        std::string mk;
        if (!readReport(R / "report.json", rep, mk)) continue;   // some other report.json
        const bool master = mk == "output";
        const bool preview = R.parent_path().filename() == "preview";
        std::vector<fs::path> files;
        auto add = [&](const fs::path &p) {
            if (!plainFile(p) || keep.count(key(p))) return;
            for (auto &q : files) if (q == p) return;
            files.push_back(p);
        };
        std::string mixName = fs::u8path(rep[mk].value("file", std::string())).filename().u8string();
        if (lower(fs::u8path(mixName).extension().string()) != ".wav") mixName = "mix.wav";
        const fs::path mix = R / fs::u8path(mixName);
        add(mix);
        for (auto &d : rep[mk].value("deliveries", json::array()))   // lossless deliveries: FLAC, 16/24-bit WAV
            if (d.is_object() && d.value("format", std::string()) != "mp3" && !d.value("file", std::string()).empty())
                add(R / fs::u8path(d.value("file", std::string())).filename());
        std::set<std::string> stems;
        for (const char *listName : {"tracks", "buses"})
            for (auto &t : rep.value(listName, json::array()))
                if (t.is_object() && t.contains("file") && t["file"].is_string() && !t["file"].get<std::string>().empty())
                    stems.insert(fs::u8path(t["file"].get<std::string>()).filename().u8string());
        std::error_code ec;
        if (fs::is_directory(fs::symlink_status(R / "stems", ec)))   // this render's stems and older takes, named the render's way
            for (auto &e : fs::directory_iterator(R / "stems", ec)) {
                const std::string n = e.path().filename().u8string();
                if (stems.count(n) || std::regex_match(n, stemName)) add(e.path());
            }
        if (preview) { add(R / "report.json"); add(R / "song.png"); }   // a cache entry goes whole
        if (files.empty()) continue;
        for (auto &p : files) renderKeys.insert(key(p));
        if (master && !opt.masters) {   // a finished master: kept unless asked
            ++masters;
            for (auto &p : files) masterBytes += sizeOf(p);
            continue;
        }

        double newest = 1e12;
        for (auto &p : files) newest = std::min(newest, ageSeconds(p));
        newest = std::min(newest, ageSeconds(R / "report.json"));
        if (newest < opt.minAgeSeconds) {
            skipped.push_back({{"dir", shown(R, f.root)}, {"why", "written " + std::to_string((int)newest) + " s ago: it may still be rendering"}});
            continue;
        }

        json e = {{"dir", shown(R, f.root)}, {"removed", json::array()}, {"bytes", 0}, {"previews", 0}};
        if (master) e["master"] = true;
        // a render that would be left without anything to play gets an MP3 of its mix first
        const bool mixGoes = std::find(files.begin(), files.end(), mix) != files.end();
        if (!preview && mixGoes && !currentMp3(R, mix, rep[mk])) {
            if (plainFile(R / "mix.mp3")) e["replacedMp3"] = true;   // an older render's: it would play the wrong mix
            if (opt.dryRun) { e["madeMp3"] = "mix.mp3"; ++mp3s; }
            else {
                Audio a;
                int sr = 48000;
                std::string why;
                DeliverySpec spec;
                std::vector<Delivery> made;
                std::vector<std::string> warnings;
                bool ok = readAudio(mix.string(), a, sr, why) && parseDeliverySpec("mp3", spec, why);
                spec.file = "mix.mp3";
                const double peak = rep[mk].contains("truePeakDb") && rep[mk]["truePeakDb"].is_number() ? rep[mk]["truePeakDb"].get<double>() : -120.0;
                ok = ok && writeDeliveries({spec}, a, sr, 0, 0, R.string(), mix.string(), peak, made, warnings, why);
                if (ok && !made.empty()) {
                    e["madeMp3"] = "mix.mp3";
                    ++mp3s;
                    if (!rep[mk].contains("deliveries") || !rep[mk]["deliveries"].is_array()) rep[mk]["deliveries"] = json::array();
                    for (auto &d : deliveriesJson(made)) rep[mk]["deliveries"].push_back(d);
                } else {   // nothing to play without it: keep the WAV
                    files.erase(std::remove(files.begin(), files.end(), mix), files.end());
                    e["keptMix"] = true;
                    e["error"] = "kept " + mixName + ": no MP3 could be made (" + (why.empty() ? "no encoder" : why) + ")";
                }
            }
        }
        double bytes = 0;
        for (auto &p : files) {
            const double n = sizeOf(p);
            std::error_code rm;
            if (!opt.dryRun) fs::remove(p, rm);
            if (rm) { e["error"] = "cannot delete " + p.filename().u8string() + ": " + rm.message(); continue; }
            bytes += n;
            e["removed"].push_back(p.lexically_relative(R).generic_u8string());
        }
        if (!opt.dryRun) {
            std::error_code rm;
            if (fs::is_empty(R / "stems", rm) && !rm) fs::remove(R / "stems", rm);
            if (preview) {
                rm.clear();
                if (fs::is_empty(R, rm) && !rm) fs::remove(R, rm);
                rm.clear();
                if (fs::is_empty(R.parent_path(), rm) && !rm) fs::remove(R.parent_path(), rm);
            } else {   // the report says what went, so nobody looks for the files
                rep["purged"] = {{"at", nowRfc3339()}, {"removed", e["removed"]}};
                std::string why;
                if (!writeText(R / "report.json", rep.dump(2, ' ', false, json::error_handler_t::replace) + "\n", why)) e["error"] = why;
            }
        }
        e["bytes"] = bytes;
        freed += bytes;
        // a preview folds into the render it belongs to (out/preview/<id> -> out)
        const auto owner = preview ? at.find(key(R.parent_path().parent_path())) : at.end();
        if (owner != at.end()) {
            json &o = list[owner->second];
            o["bytes"] = o["bytes"].get<double>() + bytes;
            o["previews"] = o["previews"].get<int>() + 1;
        } else {
            if (preview) e["preview"] = true;
            at[key(R)] = list.size();
            list.push_back(e);
        }
    }

    // ---- what is left: WAVs no render wrote (sources, masters, frozen tracks) ---------------------------
    double otherBytes = 0;
    size_t otherFiles = 0;
    std::map<std::string, double> byDir;
    for (auto &w : wavs) {
        if (renderKeys.count(key(w))) continue;
        const double n = sizeOf(w);
        otherBytes += n;
        ++otherFiles;
        byDir[w.parent_path().generic_u8string()] += n;
    }
    std::vector<std::pair<double, std::string>> big;
    for (auto &[d, n] : byDir) big.push_back({n, d});
    std::sort(big.rbegin(), big.rend());
    json top = json::array();
    for (size_t i = 0; i < big.size() && i < 5; ++i) {
        fs::path d = fs::u8path(big[i].second);
        fs::path root = roots.front();
        for (auto &rt : roots) if (d.generic_u8string().rfind(rt.generic_u8string(), 0) == 0) root = rt;
        top.push_back({{"dir", shown(d, root)}, {"bytes", big[i].first}});
    }
    r = {{"ok", true}, {"dryRun", opt.dryRun}, {"renders", list}, {"skipped", skipped}, {"freedBytes", freed}, {"mp3sMade", mp3s},
         {"mastersKept", {{"count", masters}, {"bytes", masterBytes}}},
         {"otherWav", {{"files", otherFiles}, {"bytes", otherBytes}, {"folders", top}}}};
    return true;
}

} // namespace wl::purge
