#include "serve.hpp"

#include "harmony.hpp"
#include "job.hpp"
#include "platform.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace wl {

// the built-in UI (generated from ui/ at build time, see cmake/embed-ui.cmake)
struct UiAsset { const char *path; const unsigned char *data; size_t size; };
extern const UiAsset kUiAssets[];
extern const size_t kUiAssetCount;

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

const size_t kScanLimit = 3000;

// file times as Unix seconds (file_clock's epoch differs from system_clock's on some platforms)
double unixTime(const fs::path &p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (double)sys.time_since_epoch().count();
}

std::string readFile(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
bool readJson(const fs::path &p, json &out) {
    std::ifstream in(p);
    if (!in) return false;
    try { in >> out; } catch (...) { return false; }
    return out.is_object();
}

bool validSlug(const std::string &s) {
    return !s.empty() && s[0] != '.' && std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-'; });
}

struct FileInfo { double size, mtime; };
using FileMap = std::map<std::string, FileInfo>;

// every file in a song folder: relative path -> size, mtime (dot files and folders skipped)
FileMap scanSong(const fs::path &dir) {
    FileMap files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (!name.empty() && name[0] == '.') { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        if (files.size() >= kScanLimit) break;
        const std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        files[rel] = {(double)it->file_size(ec), unixTime(it->path())};
    }
    return files;
}

// prefer the song's own top-level file, else the newest with that name anywhere in the folder
std::string pick(const FileMap &files, const std::string &name, const std::string &preferred) {
    if (files.count(preferred)) return preferred;
    std::string best;
    for (auto &[p, f] : files)
        if (fs::path(p).filename() == name && (best.empty() || f.mtime > files.at(best).mtime)) best = p;
    return best;
}

json filesJson(const FileMap &files) {
    json o = json::object();
    for (auto &[p, f] : files) o[p] = {f.size, f.mtime};
    return o;
}

// the job trimmed to what the timeline draws: notes as [start, length, key, vel, inSeconds]
json jobSummary(const json &job) {
    json tracks = json::array();
    size_t i = 0;
    for (auto &t : job.value("tracks", json::array())) {
        ++i;
        json notes = json::array();
        for (auto &n : t.value("notes", json::array())) {
            const bool secs = !n.contains("beat") && n.contains("time");
            double vel = n.value("vel", 0.8);
            if (vel > 1) vel /= 127;
            int key = 60;
            try { key = parseKey(n.value("key", json(60))); } catch (...) {}
            key += t.value("transpose", 0);
            notes.push_back({secs ? n.value("time", 0.0) : n.value("beat", 0.0), secs ? n.value("length", 0.25) : n.value("dur", 0.25), key, vel, secs ? 1 : 0});
        }
        for (auto &c : t.value("clips", json::array())) notes.push_back({c.value("beat", 0.0), c.value("length", 4.0), 60, 0.8, 0});
        json fx = json::array();
        for (auto &f : t.value("fx", json::array())) fx.push_back(f.is_object() ? f.value("type", f.value("plugin", std::string("?"))) : std::string("?"));
        std::string preset = t.value("preset", std::string());
        if (preset.empty() && t.contains("state")) preset = t["state"].is_object() ? t["state"].value("file", std::string()) : (t["state"].is_string() ? t["state"].get<std::string>() : "");
        if (preset.empty() && t.contains("sampler") && t["sampler"].is_object())
            preset = t["sampler"].value("kit", t["sampler"].value("multisample", std::string()));
        tracks.push_back({{"name", t.value("name", "track" + std::to_string(i))}, {"plugin", t.value("plugin", std::string())}, {"preset", preset},
                          {"fx", fx}, {"gain", t.contains("gain") && t["gain"].is_number() ? t["gain"] : json(0)}, {"output", t.value("output", std::string())},
                          {"mute", t.value("mute", false)}, {"notes", notes}});
    }
    json master = json::array();
    if (job.contains("master") && job["master"].is_object())
        for (auto &f : job["master"].value("fx", json::array())) master.push_back(f.is_object() ? f.value("type", f.value("plugin", std::string("?"))) : std::string("?"));
    json buses = json::array();
    for (auto &b : job.value("buses", json::array())) buses.push_back(b.value("name", std::string()));
    return {{"timeSignature", job.value("timeSignature", json::array({4, 4}))}, {"keys", job.value("keys", json::array())},
            {"tempo", job.contains("tempo") ? job["tempo"] : json(120)}, {"leadIn", job.contains("leadIn") ? job["leadIn"] : json(nullptr)},
            {"markers", job.value("markers", json::array())}, {"buses", buses}, {"master", master}, {"tracks", tracks}};
}

bool isAudio(const std::string &p) {
    auto ends = [&](const char *e) { return p.size() >= std::strlen(e) && p.compare(p.size() - std::strlen(e), std::string::npos, e) == 0; };
    return (ends(".mp3") || ends(".wav")) && p.find("stems/") == std::string::npos && p.find("solo/") == std::string::npos && p.find("preview/") == std::string::npos;
}

// ---- the Claude Code transcript working in a song folder (optional: ~/.claude/projects) ----
std::string tailOf(const fs::path &p, size_t bytes) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    in.seekg(0, std::ios::end);
    const auto size = (size_t)in.tellg();
    in.seekg((std::streamoff)(size > bytes ? size - bytes : 0));
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}
fs::path findTranscript(const fs::path &root, const std::string &slug) {
    const char *home = std::getenv("HOME");
    if (!home) return {};
    const fs::path projects = fs::path(home) / ".claude" / "projects";
    std::error_code ec;
    if (!fs::is_directory(projects, ec)) return {};
    std::vector<std::pair<double, fs::path>> files;
    for (auto &d : fs::directory_iterator(projects, ec))
        if (d.is_directory(ec))
            for (auto &f : fs::directory_iterator(d.path(), ec))
                if (f.path().extension() == ".jsonl") files.push_back({unixTime(f.path()), f.path()});
    std::sort(files.begin(), files.end(), [](auto &a, auto &b) { return a.first > b.first; });
    const std::string needle = root.filename().string() + "/" + slug;
    const double now = (double)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (size_t i = 0; i < files.size() && i < 12; ++i) {
        if (now - files[i].first > 6 * 3600) break;
        const std::string tail = tailOf(files[i].second, 400000);
        // a session that runs commands in the folder, not one that only names it
        int hits = 0;
        for (size_t at = tail.find("\"input\":{"); at != std::string::npos && hits < 2; at = tail.find("\"input\":{", at + 9)) {
            const size_t eol = tail.find('\n', at);
            if (tail.substr(at, (eol == std::string::npos ? tail.size() : eol) - at).find(needle) != std::string::npos) ++hits;
        }
        if (hits >= 2) return files[i].second;
    }
    return {};
}
json agentFeed(const fs::path &path, size_t limit = 60) {
    json items = json::array();
    std::stringstream ss(tailOf(path, 600000));
    std::string line;
    std::getline(ss, line);   // likely a partial line
    while (std::getline(ss, line)) {
        json row;
        try { row = json::parse(line); } catch (...) { continue; }
        if (row.value("type", std::string()) != "assistant" || row.value("isSidechain", false)) continue;
        const std::string when = row.value("timestamp", std::string());
        if (!row.contains("message") || !row["message"].contains("content") || !row["message"]["content"].is_array()) continue;
        for (auto &part : row["message"]["content"]) {
            const std::string type = part.value("type", std::string());
            if (type == "text") {
                std::string t = part.value("text", std::string());
                if (t.find_first_not_of(" \n\t") == std::string::npos) continue;
                items.push_back({{"kind", "text"}, {"text", t.substr(0, 1200)}, {"at", when}});
            } else if (type == "tool_use") {
                const json in = part.value("input", json::object());
                std::string detail = in.value("description", in.value("file_path", in.value("command", in.value("prompt", std::string()))));
                items.push_back({{"kind", "tool"}, {"tool", part.value("name", std::string())}, {"text", detail.substr(0, 300)},
                                 {"code", in.contains("command") && in["command"].is_string() ? in["command"].get<std::string>().substr(0, 600) : ""}, {"at", when}});
            }
        }
    }
    if (items.size() > limit) items.erase(items.begin(), items.begin() + (long)(items.size() - limit));
    return items;
}

// ---- previews: renders of bars and tracks, one at a time, in child processes ----
struct Preview {
    std::string id, song, dir;          // dir: absolute output folder
    std::vector<std::string> tracks;
    int from = 0, to = 0;               // bars (to exclusive); 0 = the whole song
    std::string status = "queued";      // queued, rendering, ready, failed, cancelled
    std::string path, error;            // path: mix.wav relative to the song folder
    json window;
    double queuedAt = 0, startedAt = 0, finishedAt = 0;
};

double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::string fnv(const std::string &s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return std::string(buf).substr(0, 12);
}

class Server {
public:
    explicit Server(const ServeOptions &o) : opt_(o), root_(fs::absolute(o.root)) {
        std::random_device rd;
        std::mt19937_64 g(rd());
        char buf[33];
        std::snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)g(), (unsigned long long)g());
        token_ = buf;
    }
    int run();

private:
    ServeOptions opt_;
    fs::path root_;
    std::string token_;
    httplib::Server http_;
    std::mutex mu_;                       // previews, review files
    std::condition_variable cv_;
    std::map<std::string, Preview> previews_;
    std::deque<std::string> queue_;
    std::atomic<uint64_t> previewVersion_{0};
    std::atomic<bool> stopping_{false};

    fs::path songDir(const std::string &slug) const { return root_ / slug; }
    bool songExists(const std::string &slug) const { std::error_code ec; return validSlug(slug) && fs::is_directory(songDir(slug), ec); }

    json songs();
    json song(const std::string &slug);
    json harmony(const std::string &slug);
    json previewJson(const Preview &p) const;
    json startPreview(const std::string &slug, std::vector<std::string> tracks, int from, int to);
    void previewWorker();
    void prunePreviews(const fs::path &dir);
    bool allowed(const httplib::Request &req) const;
    std::string uiFile(const std::string &path, std::string &type) const;
};

json Server::songs() {
    json list = json::array();
    std::error_code ec;
    for (auto &d : fs::directory_iterator(root_, ec)) {
        const std::string name = d.path().filename().string();
        if (!d.is_directory(ec) || name.empty() || name[0] == '.' || name[0] == '_' || !validSlug(name)) continue;   // _style, _tools: not songs
        const FileMap files = scanSong(d.path());
        double mtime = unixTime(d.path());
        bool mp3 = false;
        for (auto &[p, f] : files) { mtime = std::max(mtime, f.mtime); if (p.size() > 4 && p.substr(p.size() - 4) == ".mp3" && isAudio(p)) mp3 = true; }
        list.push_back({{"slug", name}, {"mtime", mtime}, {"files", files.size()}, {"mp3", mp3}});
    }
    std::sort(list.begin(), list.end(), [](const json &a, const json &b) { return a["mtime"].get<double>() > b["mtime"].get<double>(); });
    return list;
}

json Server::song(const std::string &slug) {
    const fs::path dir = songDir(slug);
    const FileMap files = scanSong(dir);
    const std::string jobPath = pick(files, "job.json", "job.json"), reportPath = pick(files, "report.json", "out/report.json");
    json job, report;
    const bool hasJob = !jobPath.empty() && readJson(dir / jobPath, job);
    const bool hasReport = !reportPath.empty() && readJson(dir / reportPath, report);
    std::vector<std::string> audio;
    for (auto &[p, f] : files) if (isAudio(p)) audio.push_back(p);
    std::sort(audio.begin(), audio.end(), [&](const std::string &a, const std::string &b) {
        const bool am = a.substr(a.size() - 4) == ".mp3", bm = b.substr(b.size() - 4) == ".mp3";
        if (am != bm) return am;
        return files.at(a).mtime > files.at(b).mtime;
    });
    json docs = json::object();
    for (auto &[p, f] : files)
        if (p.size() > 3 && p.substr(p.size() - 3) == ".md" && f.size < 200000) docs[p] = readFile(dir / p);
    return {{"slug", slug}, {"files", filesJson(files)}, {"jobPath", jobPath.empty() ? json(nullptr) : json(jobPath)},
            {"job", hasJob ? jobSummary(job) : json(nullptr)}, {"reportPath", reportPath.empty() ? json(nullptr) : json(reportPath)},
            {"report", hasReport ? report : json(nullptr)}, {"audio", audio}, {"docs", docs}};
}

// the harmony check (lint --harmony --chords) on the song's job; drum and effect tracks left out by name
json Server::harmony(const std::string &slug) {
    const fs::path dir = songDir(slug);
    const std::string jobPath = pick(scanSong(dir), "job.json", "job.json");
    json j;
    if (jobPath.empty() || !readJson(dir / jobPath, j)) return {{"ok", false}};
    Job job;
    std::string err;
    try {
        if (!parseJob(j, (dir / jobPath).parent_path().string(), job, err)) return {{"ok", false}, {"error", err}};
    } catch (const std::exception &e) { return {{"ok", false}, {"error", e.what()}}; }
    static const std::regex notHarmony("kick|snare|hat|clap|perc|tom|crash|cym|ride|roll|drum|sfx|\\bfx\\b|ping|heart|noise|riser|impact|sweep|laser|shaker|rim",
                                       std::regex::icase);
    HarmonyOptions o;
    o.keys = job.keys;
    json ignored = json::array();
    for (size_t i = 0; i < job.tracks.size(); ++i) {
        const Track &t = job.tracks[i];
        if (t.notes.empty() || t.plugin == "builtin:drums" || t.plugin == "builtin:fx" || t.plugin == "builtin:audio" ||
            (t.sampler.is_object() && (t.sampler.contains("kit") || t.sampler.contains("map"))))
            continue;
        if (std::regex_search(t.name, notHarmony)) { ignored.push_back(t.name); continue; }
        o.tracks.push_back(i);
    }
    json r = analyzeHarmony(job, o);
    r["ok"] = true;
    r["ignored"] = ignored;
    return r;
}

json Server::previewJson(const Preview &p) const {
    json o = {{"id", p.id}, {"song", p.song}, {"status", p.status}, {"tracks", p.tracks}, {"from", p.from}, {"to", p.to}};
    if (!p.path.empty()) o["path"] = p.path;
    if (!p.error.empty()) o["error"] = p.error;
    if (!p.window.is_null()) o["window"] = p.window;
    if (p.startedAt > 0) o["seconds"] = std::round(((p.finishedAt > 0 ? p.finishedAt : nowSec()) - p.startedAt) * 10) / 10;
    return o;
}

// A preview: the song's current job rendered for bars from..to (or all of it) and some tracks (or
// all), at the level they have in the last full render. Cached by everything that shapes it.
json Server::startPreview(const std::string &slug, std::vector<std::string> tracks, int from, int to) {
    const fs::path dir = songDir(slug);
    const FileMap files = scanSong(dir);
    const std::string jobPath = pick(files, "job.json", "job.json"), reportPath = pick(files, "report.json", "out/report.json");
    if (jobPath.empty()) return {{"error", "the song has no job.json yet"}};
    std::sort(tracks.begin(), tracks.end());
    std::string sig = slug + "|" + std::to_string(files.at(jobPath).mtime) + "|" + (reportPath.empty() ? "" : std::to_string(files.at(reportPath).mtime)) + "|" +
                      std::to_string(from) + "|" + std::to_string(to);
    for (auto &t : tracks) sig += "|" + t;
    const std::string id = fnv(sig);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = previews_.find(id);
    if (it != previews_.end() && it->second.status != "failed" && it->second.status != "cancelled") return previewJson(it->second);
    Preview p;
    p.id = id;
    p.song = slug;
    p.tracks = tracks;
    p.from = from;
    p.to = to;
    const fs::path outBase = reportPath.empty() ? dir / "out" : (dir / reportPath).parent_path();
    p.dir = (outBase / "preview" / id).string();
    p.queuedAt = nowSec();
    // rendered before (by an earlier server run): ready at once
    json rep;
    std::error_code ec;
    if (fs::exists(fs::path(p.dir) / "mix.wav", ec) && readJson(fs::path(p.dir) / "report.json", rep) && rep.value("ok", false)) {
        p.status = "ready";
        p.path = fs::relative(fs::path(p.dir) / "mix.wav", dir, ec).generic_string();
        p.window = rep.value("window", json());
        previews_[id] = p;
        return previewJson(p);
    }
    // a newer request for the same song replaces the ones still waiting
    for (auto q = queue_.begin(); q != queue_.end();) {
        auto &old = previews_[*q];
        if (old.song == slug) { old.status = "cancelled"; q = queue_.erase(q); } else ++q;
    }
    previews_[id] = p;
    queue_.push_back(id);
    ++previewVersion_;
    cv_.notify_all();
    return previewJson(p);
}

void Server::previewWorker() {
    while (!stopping_) {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) continue;
            id = queue_.front();
            queue_.pop_front();
            previews_[id].status = "rendering";
            previews_[id].startedAt = nowSec();
        }
        ++previewVersion_;
        Preview p;
        { std::lock_guard<std::mutex> lock(mu_); p = previews_[id]; }
        const fs::path dir = songDir(p.song);
        const FileMap files = scanSong(dir);
        const std::string jobPath = pick(files, "job.json", "job.json"), reportPath = pick(files, "report.json", "out/report.json");
        std::vector<std::string> args = {platform::selfExecutable(), "render", (dir / jobPath).string(), "--out", p.dir, "--stems", "none", "--json"};
        if (!reportPath.empty()) { args.push_back("--level-from"); args.push_back((dir / reportPath).string()); }
        if (!p.tracks.empty()) {
            std::string t;
            for (auto &n : p.tracks) t += (t.empty() ? "" : ",") + n;
            args.push_back("--tracks");
            args.push_back(t);
        }
        if (p.from > 0 && p.to > p.from) {
            args.push_back("--from"); args.push_back(std::to_string(p.from));
            args.push_back("--to"); args.push_back(std::to_string(p.to));
        }
        std::error_code ec;
        fs::create_directories(p.dir, ec);
        platform::Process proc;
        std::string out, error;
        bool ok = false;
        json rep;
        if (!platform::spawn(args, proc, true, true)) error = "could not start a render";
        else {
            platform::readOutput(proc, out, 900);
            std::string crash;
            while (!platform::finished(proc, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            try { rep = json::parse(out); } catch (...) {}
            ok = rep.is_object() && rep.value("ok", false);
            if (!ok) error = rep.is_object() && rep.contains("error") ? rep["error"].get<std::string>() : (crash.empty() ? "the render failed" : "the render crashed: " + crash);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            Preview &q = previews_[id];
            q.finishedAt = nowSec();
            if (ok) {
                q.status = "ready";
                q.path = fs::relative(fs::path(p.dir) / "mix.wav", dir, ec).generic_string();
                q.window = rep.value("window", json());
            } else {
                q.status = "failed";
                q.error = error.substr(0, 600);
            }
        }
        ++previewVersion_;
        prunePreviews(fs::path(p.dir).parent_path());
    }
}

// keep the newest 40 previews of a song
void Server::prunePreviews(const fs::path &dir) {
    std::error_code ec;
    std::vector<std::pair<double, fs::path>> all;
    for (auto &d : fs::directory_iterator(dir, ec)) if (d.is_directory(ec)) all.push_back({unixTime(d.path()), d.path()});
    if (all.size() <= 40) return;
    std::sort(all.begin(), all.end(), [](auto &a, auto &b) { return a.first > b.first; });
    for (size_t i = 40; i < all.size(); ++i) fs::remove_all(all[i].second, ec);
}

// Local only: the Host header must name this machine (no DNS rebinding), and changes need the token
// the page was served with (no other site can post here).
bool Server::allowed(const httplib::Request &req) const {
    const bool loopback = opt_.host == "127.0.0.1" || opt_.host == "localhost" || opt_.host == "::1";
    if (loopback) {
        std::string host = req.get_header_value("Host");
        host = host.substr(0, host.rfind(':') == std::string::npos || host.back() == ']' ? host.size() : host.rfind(':'));
        // names under .localhost always mean this machine (RFC 6761; nobody can register one), so a local
        // reverse proxy such as https://wavelength-ui.localhost is fine; other names could be DNS rebinding
        const bool dotLocalhost = host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0;
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]" && !dotLocalhost) return false;
    }
    if (req.method != "GET" && req.method != "HEAD" && req.get_header_value("X-Wavelength-Token") != token_) return false;
    return true;
}

std::string Server::uiFile(const std::string &path, std::string &type) const {
    const std::string ext = fs::path(path).extension().string();
    type = ext == ".html" ? "text/html; charset=utf-8" : ext == ".js" ? "text/javascript; charset=utf-8" : ext == ".css" ? "text/css; charset=utf-8"
         : ext == ".svg" ? "image/svg+xml" : "application/octet-stream";
    if (path.find("..") != std::string::npos) return "";
    if (!opt_.uiDir.empty()) {
        std::error_code ec;
        const fs::path f = fs::path(opt_.uiDir) / path;
        return fs::is_regular_file(f, ec) ? readFile(f) : "";
    }
    for (size_t i = 0; i < kUiAssetCount; ++i)
        if (path == kUiAssets[i].path) return std::string((const char *)kUiAssets[i].data, kUiAssets[i].size);
    return "";
}

int Server::run() {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) { std::fprintf(stderr, "error: %s is not a folder\n", root_.string().c_str()); return 1; }
    http_.new_task_queue = [] { return new httplib::ThreadPool(48); };   // live-update streams each hold a thread
    http_.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        if (allowed(req)) return httplib::Server::HandlerResponse::Unhandled;
        res.status = 403;
        res.set_content("{\"error\":\"forbidden\"}", "application/json");
        return httplib::Server::HandlerResponse::Handled;
    });
    auto sendJson = [](httplib::Response &res, const json &j, int status = 200) {
        res.status = status;
        res.set_header("Cache-Control", "no-store");
        res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
    };
    auto slugOf = [this](const httplib::Request &req, std::string &slug) { slug = req.get_param_value("song"); return songExists(slug); };

    // the UI
    http_.Get("/", [this](const httplib::Request &, httplib::Response &res) {
        std::string type, html = uiFile("index.html", type);
        if (html.empty()) { res.status = 404; return; }
        const std::string mark = "<!--wavelength-->";
        const size_t at = html.find(mark);
        const std::string meta = "<meta name=\"wavelength-token\" content=\"" + token_ + "\"><meta name=\"wavelength-version\" content=\"" WAVELENGTH_VERSION "\">";
        if (at != std::string::npos) html.replace(at, mark.size(), meta);
        res.set_header("Cache-Control", "no-store");
        res.set_content(html, type);
    });
    http_.Get(R"(/ui/(.+))", [this](const httplib::Request &req, httplib::Response &res) {
        std::string type, body = uiFile(req.matches[1], type);
        if (body.empty()) { res.status = 404; return; }
        res.set_header("Cache-Control", "no-store");
        res.set_content(body, type);
    });
    // song files (audio with ranges): httplib serves them, confined to the songs folder
    if (!http_.set_mount_point("/songs", root_.string())) { std::fprintf(stderr, "error: cannot serve %s\n", root_.string().c_str()); return 1; }
    http_.set_file_extension_and_mimetype_mapping("mp3", "audio/mpeg");
    http_.set_file_extension_and_mimetype_mapping("wav", "audio/wav");
    http_.set_file_extension_and_mimetype_mapping("md", "text/plain; charset=utf-8");

    http_.Get("/api/songs", [&](const httplib::Request &, httplib::Response &res) { sendJson(res, {{"songs", songs()}}); });
    http_.Get("/api/song", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        sendJson(res, song(s));
    });
    http_.Get("/api/harmony", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        sendJson(res, harmony(s));
    });
    http_.Get("/api/agent", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        const fs::path t = findTranscript(root_, s);
        sendJson(res, {{"session", t.empty() ? json(nullptr) : json(t.stem().string())}, {"mtime", t.empty() ? json(nullptr) : json(unixTime(t))},
                       {"items", t.empty() ? json::array() : agentFeed(t)}});
    });

    // review comments: GET lists them, POST {op: add|status|delete} changes them. The agent answers by
    // editing review.json itself ("status": "done" and a "reply" the editor shows under the comment).
    auto readReview = [](const fs::path &p) {
        json d;
        if (!readJson(p, d) || !d.contains("comments") || !d["comments"].is_array()) d = {{"comments", json::array()}};
        return d;
    };
    http_.Get("/api/review", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        sendJson(res, readReview(songDir(s) / "review.json"));
    });
    http_.Post("/api/review", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        std::lock_guard<std::mutex> lock(mu_);
        const fs::path path = songDir(s) / "review.json";
        json data = readReview(path);
        const std::string op = in.value("op", std::string());
        if (op == "add") {
            std::string text = in.value("text", std::string());
            if (text.find_first_not_of(" \n\t") == std::string::npos) return sendJson(res, {{"error", "empty comment"}}, 400);
            const auto now = std::chrono::system_clock::now();
            const std::time_t tt = std::chrono::system_clock::to_time_t(now);
            char stamp[32], idb[32];
            std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S%z", std::localtime(&tt));
            std::strftime(idb, sizeof idb, "c%y%m%d%H%M%S", std::localtime(&tt));
            json c = {{"id", std::string(idb) + fnv(text + stamp).substr(0, 3)}, {"created", stamp}, {"status", "open"}, {"text", text.substr(0, 4000)}};
            for (const char *k : {"ref", "bars", "beats", "time", "tracks", "notes", "render"}) if (in.contains(k)) c[k] = in[k];
            data["comments"].push_back(c);
        } else if (op == "status" || op == "delete") {
            json kept = json::array();
            for (auto &c : data["comments"]) {
                if (c.value("id", std::string()) != in.value("id", std::string())) { kept.push_back(c); continue; }
                if (op == "delete") continue;
                const std::string st = in.value("status", std::string("open"));
                c["status"] = st == "done" ? "done" : "open";
                kept.push_back(c);
            }
            data["comments"] = kept;
        } else return sendJson(res, {{"error", "unknown op"}}, 400);
        std::ofstream(path) << data.dump(4) << "\n";
        sendJson(res, data);
    });

    // previews: POST {song, tracks: [...], from: bar, to: bar} starts one (or returns the cached one)
    http_.Post("/api/preview", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        const std::string s = in.value("song", std::string());
        if (!songExists(s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        std::vector<std::string> tracks;
        for (auto &t : in.value("tracks", json::array())) if (t.is_string()) tracks.push_back(t.get<std::string>());
        const int from = in.value("from", 0), to = in.value("to", 0);
        json r = startPreview(s, tracks, from, to);
        sendJson(res, r, r.contains("error") ? 400 : 200);
    });
    http_.Get("/api/preview", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = previews_.find(req.get_param_value("id"));
        if (it == previews_.end()) return sendJson(res, {{"error", "unknown preview"}}, 404);
        sendJson(res, previewJson(it->second));
    });

    // live updates (server-sent events): file changes in the song folder, the song list, previews and
    // the agent's transcript. Each stream lasts ~45 s; EventSource reconnects on its own.
    http_.Get("/api/events", [&](const httplib::Request &req, httplib::Response &res) {
        std::string slug = req.get_param_value("song");
        if (!songExists(slug)) slug.clear();
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider("text/event-stream", [this, slug](size_t, httplib::DataSink &sink) {
            auto send = [&](const std::string &event, const json &data) {
                const std::string msg = "event: " + event + "\ndata: " + data.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
                return sink.write(msg.data(), msg.size());
            };
            const std::string hello = "retry: 1000\n\n";
            if (!sink.write(hello.data(), hello.size())) return false;
            FileMap prev = slug.empty() ? FileMap() : scanSong(songDir(slug));
            fs::path transcript = slug.empty() ? fs::path() : findTranscript(root_, slug);
            double prevAgent = transcript.empty() ? 0 : unixTime(transcript);
            uint64_t prevPreview = previewVersion_;
            std::string prevSongs;
            if (!send("hello", {{"song", slug}, {"agent", !transcript.empty()}})) return false;
            for (int tick = 0; tick < 45 && !stopping_; ++tick) {
                if (tick % 3 == 0) {
                    const json list = songs();
                    std::string sig;
                    for (auto &s : list) sig += s["slug"].get<std::string>() + std::to_string(s["mtime"].get<double>()) + std::to_string(s["files"].get<size_t>());
                    if (sig != prevSongs) { if (!send("songs", list)) return false; prevSongs = sig; }
                }
                if (!slug.empty()) {
                    const FileMap files = scanSong(songDir(slug));
                    json changes = json::array();
                    for (auto &[p, f] : files) {
                        auto it = prev.find(p);
                        if (it == prev.end()) changes.push_back({{"type", "added"}, {"path", p}, {"size", f.size}});
                        else if (it->second.size != f.size || it->second.mtime != f.mtime) changes.push_back({{"type", "changed"}, {"path", p}, {"size", f.size}});
                    }
                    for (auto &[p, f] : prev) if (!files.count(p)) changes.push_back({{"type", "removed"}, {"path", p}});
                    if (!changes.empty() && !send("files", changes)) return false;
                    prev = files;
                    if (tick % 5 == 4 && transcript.empty()) transcript = findTranscript(root_, slug);
                    if (!transcript.empty()) {
                        const double m = unixTime(transcript);
                        if (m != prevAgent) { if (!send("agent", {{"mtime", m}})) return false; prevAgent = m; }
                    }
                    if (previewVersion_ != prevPreview) {
                        prevPreview = previewVersion_;
                        json ps = json::array();
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            for (auto &[id, p] : previews_) if (p.song == slug) ps.push_back(previewJson(p));
                        }
                        if (!send("previews", ps)) return false;
                    }
                }
                const std::string tickMsg = ": tick\n\n";
                if (!sink.write(tickMsg.data(), tickMsg.size())) return false;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            sink.done();
            return true;
        });
    });

    std::thread worker([this] { previewWorker(); });
    const std::string url = "http://" + std::string(opt_.host == "0.0.0.0" ? "127.0.0.1" : opt_.host) + ":" + std::to_string(opt_.port) + "/";
    std::fprintf(stderr, "Wavelength %s serving %s\n  %s\n  (Ctrl+C to stop)\n", WAVELENGTH_VERSION, root_.string().c_str(), url.c_str());
    if (opt_.open) {
#if defined(__APPLE__)
        std::system(("open '" + url + "' >/dev/null 2>&1 &").c_str());
#elif defined(_WIN32)
        std::system(("start \"\" \"" + url + "\"").c_str());
#else
        if (std::system(("xdg-open '" + url + "' >/dev/null 2>&1 &").c_str()) != 0) std::fprintf(stderr, "open %s in a browser\n", url.c_str());
#endif
    }
    const bool ok = http_.listen(opt_.host, opt_.port);
    stopping_ = true;
    cv_.notify_all();
    worker.join();
    if (!ok) { std::fprintf(stderr, "error: could not listen on %s:%d (in use?)\n", opt_.host.c_str(), opt_.port); return 1; }
    return 0;
}

} // namespace

int serve(const ServeOptions &o) {
    Server s(o);
    return s.run();
}

} // namespace wl
