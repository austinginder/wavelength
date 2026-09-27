#include "review.hpp"

#include "history.hpp"
#include "sha256.hpp"
#include "songdiff.hpp"

#include <algorithm>

namespace fs = std::filesystem;

namespace wl::review {

json read(const fs::path &songDir) {
    json data = {{"comments", json::array()}};
    std::error_code ec;
    if (!fs::exists(songDir / "review.json", ec)) return data;
    try {
        json j = json::parse(readText(songDir / "review.json"));
        if (j.is_object() && j.contains("comments") && j["comments"].is_array()) data = j;
    } catch (...) {}
    return data;
}

bool write(const fs::path &songDir, const json &data, std::string &err) { return writeText(songDir / "review.json", data.dump(4) + "\n", err); }

json normalize(const json &c) {
    json out = c;
    if (!out.contains("anchor") || !out["anchor"].is_object()) {
        json a = json::object();
        for (const char *k : {"ref", "bars", "beats", "time", "tracks", "notes"})
            if (c.contains(k)) { a[k] = c[k]; out.erase(k); }
        if (c.contains("render")) { a["renderTime"] = c["render"]; out.erase("render"); }   // a report's modification time, before revisions
        out["anchor"] = a;
    }
    if (c.contains("reply") && c["reply"].is_string()) {
        if (!out.contains("replies") || !out["replies"].is_array()) out["replies"] = json::array();
        out["replies"].push_back({{"text", c["reply"]}});
        out.erase("reply");
    }
    if (!out.contains("replies")) out["replies"] = json::array();
    return out;
}

json anchorFor(const Song &song, const json &sel, const std::string &reportRel) {
    json a = json::object();
    for (const char *k : {"ref", "bars", "beats", "time", "tracks", "notes"})
        if (sel.contains(k)) a[k] = sel[k];
    std::string err;
    const std::string rel = reportRel.empty() ? "out/report.json" : reportRel;
    std::error_code ec;
    const fs::path report = song.dir / rel;
    if (checkSongPath(rel, err) && fs::is_regular_file(report, ec)) {
        a["render"] = "sha256:" + sha256File(report.string());
        try {
            const json r = json::parse(readText(report));
            if (r.contains("song") && r["song"].contains("revision")) a["revision"] = r["song"]["revision"];
        } catch (...) {}
    }
    if (!a.contains("revision")) {   // no render with a revision: the song as it is now
        std::vector<json> entries;
        if (history::read(song, entries, err) && !entries.empty()) a["revision"] = history::current(entries);
    }
    return a;
}

json status(const Song &song, const json &comment) {
    const json c = normalize(comment);
    const json a = c.value("anchor", json::object());
    json out = {{"revision", a.contains("revision") ? a["revision"] : json()}, {"since", json::array()}, {"outdated", false}, {"why", json::array()}};
    if (!a.contains("revision") || !a["revision"].is_number_integer()) return out;
    const int rev = a["revision"].get<int>();
    std::vector<json> entries;
    std::string err;
    if (!history::read(song, entries, err)) return out;
    for (auto &e : entries)
        if (e.value("rev", 0) > rev) {
            json s = {{"rev", e["rev"]}, {"op", e.value("op", std::string())}};
            if (e.contains("message")) s["message"] = e["message"];
            out["since"].push_back(s);
        }
    json then, now;
    if (!history::jobAt(song, rev, then, err) || !history::jobAt(song, 0, now, err)) return out;
    SongDiff d;
    if (!diffJobs(then, now, song.dir.string(), d, err)) return out;
    std::vector<std::string> tracks;
    const json anchorTracks = a.contains("tracks") && a["tracks"].is_array() ? a["tracks"] : json::array();
    for (auto &t : anchorTracks) if (t.is_string()) tracks.push_back(t.get<std::string>());
    int b0 = 1, b1 = 1 << 20;
    const json bars = a.contains("bars") ? a["bars"] : json();
    if (bars.is_array() && bars.size() == 2 && bars[0].is_number() && bars[1].is_number()) { b0 = (int)bars[0].get<double>(); b1 = (int)bars[1].get<double>(); }
    std::vector<std::string> why;
    if (diffTouches(d, tracks, b0, b1, why)) { out["outdated"] = true; out["why"] = why; }
    return out;
}

} // namespace wl::review
