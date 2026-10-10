#include "serve.hpp"

#include "builtins.hpp"
#include "engine.hpp"
#include "harmony.hpp"
#include "job.hpp"
#include "platform.hpp"
#include "review.hpp"
#include "song.hpp"
#include "riffs.hpp"
#include "sounds.hpp"
#include "synth.hpp"

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
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#ifndef _WIN32
#include <csignal>
#endif

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
        // a song's history objects are its revisions' files by hash: history/log.jsonl says what they are
        if (name == "objects" && it->path().parent_path().filename() == "history") { it.disable_recursion_pending(); continue; }
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

// what a builtin:sampler track plays, for the track list: a library or file name, or "custom kit" for a key -> file map
std::string samplerLabel(const json &s) {
    if (!s.is_object()) return "";
    for (const char *k : {"kit", "multisample", "sfz", "dspreset", "exs", "patch", "soundfont"})
        if (s.contains(k) && s[k].is_string()) return s[k].get<std::string>();
    if (s.contains("kit") && s["kit"].is_object()) return "custom kit";
    if (s.contains("sample") && s["sample"].is_string()) return fs::path(s["sample"].get<std::string>()).stem().string();
    return "";
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
        if (preset.empty() && t.contains("sampler")) preset = samplerLabel(t["sampler"]);
        tracks.push_back({{"name", t.value("name", "track" + std::to_string(i))}, {"plugin", t.value("plugin", std::string())}, {"preset", preset},
                          {"fx", fx}, {"gain", t.contains("gain") && t["gain"].is_number() ? t["gain"] : json(0)},
                          {"pan", t.contains("pan") && t["pan"].is_number() ? t["pan"] : json(0)}, {"output", t.value("output", std::string())},
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
    uint64_t run = 0;                   // which request this is: a re-request of the same id is a new run
    bool full = false;                  // a full render of the song (the Render button): stems, report, mp3 in its out folder
};

double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// this executable with `args`: its stdout, parsed as JSON (null when it failed)
json runSelf(std::vector<std::string> args, int timeoutSec) {
    args.insert(args.begin(), platform::selfExecutable());
    platform::Process proc;
    if (!platform::spawn(args, proc, true, true)) return nullptr;
    std::string out, crash;
    const bool done = platform::readOutput(proc, out, timeoutSec);
    if (!done) platform::kill(proc);
    else while (!platform::finished(proc, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    try { return json::parse(out); } catch (...) { return nullptr; }
}

std::string fnv(const std::string &s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
    return std::string(buf).substr(0, 12);
}

// The token pages send with every change. It lives in the settings folder (readable by this user only), so a
// server started again (after a rebuild, by a login item) still accepts the pages already open; other sites
// can never read it, which is all it guards against.
std::string serveToken() {
    const fs::path f = platform::dataDir() / "serve-token";
    {
        std::ifstream in(f);
        std::string t;
        if (in >> t && t.size() == 32 && t.find_first_not_of("0123456789abcdef") == std::string::npos) return t;
    }
    std::random_device rd;
    std::mt19937_64 g(rd());
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)g(), (unsigned long long)g());
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    std::ofstream(f) << buf << "\n";
    fs::permissions(f, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    return buf;
}

class Server {
public:
    explicit Server(const ServeOptions &o) : opt_(o), root_(fs::absolute(o.root)), token_(serveToken()) {}
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
    platform::Process running_;           // the preview render in progress (under mu_), for cancelling
    std::string runningId_;
    uint64_t runs_ = 0;
    // live notes: warm __play workers, one per song + track (+ job version), newest few kept
    struct PlayWorker {
        platform::Process proc;
        std::mutex use;          // one request at a time
        double lastUsed = 0;
        json info;               // the worker's hello: plugin, preset
    };
    std::mutex playMu_;
    std::map<std::string, std::shared_ptr<PlayWorker>> players_;
    json play(const fs::path &jobPath, const std::string &track, const std::string &key, const json &in, std::string &wav);
    json playSong(const std::string &slug, const json &in, std::string &wav);
    json playInstrument(const json &in, std::string &wav);
    fs::path playgroundJob(const std::string &plugin, const std::string &preset);
    // live playing: a __live worker per page (its session id) streams the instrument's audio while keys
    // turn notes on and off
    struct LiveSession {
        platform::Process proc;
        std::mutex io;           // stdin writes
        std::atomic<bool> streaming{false};
        json info;
        fs::path status;         // the plugin window's state, written by the worker
    };
    std::mutex liveMu_;
    std::map<std::string, std::shared_ptr<LiveSession>> lives_;
    json liveStart(const json &in);
    void liveStop(const std::string &id);
    // the Sounds page's knobs: a __knobs worker per sound (plugin, preset, state) lists its parameters and
    // answers display text as they move; at most four, stopped after 10 idle minutes (reapPlayers)
    std::mutex knobsMu_;
    std::map<std::string, std::shared_ptr<PlayWorker>> knobs_;
    json knobsAsk(const std::string &key, const fs::path *jobFile, const json &request);
    json knobsOpen(const json &in);
    // sounds.json: the instruments the Sounds page sets, applied on top of the job (sounds.hpp)
    json sounds(const std::string &slug);
    json soundsChange(const std::string &slug, const json &in);
    bool trackSound(const std::string &slug, const std::string &track, json &sound, json &params, std::string &err);
    json createSong(const std::string &title);
    void listSource(const std::string &slug, const std::string &path, const std::string &mediaType);
    // the instrument lists the playground shows: `plugins --json` and `presets <plugin> --json`, run in child
    // processes (a plugin never loads in this one) and kept for 10 minutes
    std::mutex listMu_;
    std::map<std::string, std::pair<double, json>> lists_;
    json listing(const std::vector<std::string> &args, const std::string &key);
    // title (manifest) and length (report) per song folder, read again only when those files change
    std::map<std::string, std::pair<std::string, json>> songInfo_;
    void reapPlayers(double idleSec);
    std::atomic<bool> stopping_{false};
    // the editor's live loop: bars rendered as a seamless loop with the page's mixer (render --loop --cache
    // --mix), newest request per song wins; only tracks whose sound changed render again
    struct LoopState {
        uint64_t want = 0, taken = 0, done = 0;   // newest request, the one rendering (or last taken), newest finished
        int from = 0, to = 0;
        json mix = json::object();
        std::string status = "queued", error;
        std::string wav;                          // the newest finished loop's audio
        json info = json::object();               // its window, times and which tracks rendered
    };
    std::mutex loopMu_;
    std::condition_variable loopCv_;
    std::map<std::string, LoopState> loops_;
    uint64_t loopSeq_ = 0;
    platform::Process loopRunning_;
    void loopWorker();
    json loopJson(const LoopState &l) const {
        return {{"want", l.want}, {"done", l.done}, {"status", l.status}, {"error", l.error}, {"info", l.info}, {"from", l.from}, {"to", l.to}};
    }

    // the riff library (riffs.hpp) answers to kRiffLibrarySlug wherever a song folder is asked for; it is made when first written
    fs::path songDir(const std::string &slug) const { return slug == kRiffLibrarySlug ? riffLibraryDir() : root_ / slug; }
    bool songExists(const std::string &slug) const { std::error_code ec; return slug == kRiffLibrarySlug || (validSlug(slug) && fs::is_directory(songDir(slug), ec)); }

    json songs();
    json song(const std::string &slug);
    json harmony(const std::string &slug);
    json previewJson(const Preview &p) const;
    json startPreview(const std::string &slug, std::vector<std::string> tracks, int from, int to);
    json startRender(const std::string &slug);
    json cancelPreviews(const std::string &slug, const std::string &id);
    bool rendering(const std::string &slug);
    json meta(const std::string &slug);
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
        json item = {{"slug", name}, {"mtime", mtime}, {"files", files.size()}, {"mp3", mp3}};
        // title from the manifest, length from the render report: cached until those files change
        const std::string rp = pick(files, "report.json", "out/report.json");
        const std::string sig = std::to_string(files.count("wavelength.json") ? files.at("wavelength.json").mtime : 0) + "|" + (rp.empty() ? "" : std::to_string(files.at(rp).mtime));
        std::lock_guard<std::mutex> lock(listMu_);   // songs() runs on every event stream's thread
        auto &cache = songInfo_[name];
        if (cache.first != sig) {
            json info = json::object(), m, rep;
            if (files.count("wavelength.json") && readJson(d.path() / "wavelength.json", m)) {
                if (m.contains("title") && m["title"].is_string()) info["title"] = m["title"];
                if (m.contains("authors") && m["authors"].is_array()) info["authors"] = m["authors"];
                if (m.contains("summary") && m["summary"].is_string()) info["summary"] = m["summary"];
            }
            if (!rp.empty() && readJson(d.path() / rp, rep)) {
                if (rep.contains("seconds")) info["seconds"] = rep["seconds"];
                if (rep.contains("mix") && rep["mix"].is_object() && rep["mix"].contains("lufs")) info["lufs"] = rep["mix"]["lufs"];
                if (rep.contains("tracks") && rep["tracks"].is_array()) info["tracks"] = rep["tracks"].size();
            }
            cache = {sig, info};
        }
        for (auto &[k, v] : cache.second.items()) item[k] = v;
        list.push_back(item);
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
    // the song's own render first (next to its report, or the kept render/), then MP3 before WAV, then the
    // shallower path: a set's mix ahead of the records' renders in its subfolders, however recent theirs are
    const std::string ownDir = reportPath.empty() ? "out" : fs::path(reportPath).parent_path().generic_string();
    auto own = [&](const std::string &p) { const std::string d = fs::path(p).parent_path().generic_string(); return d == ownDir || d == "render"; };
    auto depth = [](const std::string &p) { return std::count(p.begin(), p.end(), '/'); };
    std::sort(audio.begin(), audio.end(), [&](const std::string &a, const std::string &b) {
        if (own(a) != own(b)) return own(a);
        const bool am = a.substr(a.size() - 4) == ".mp3", bm = b.substr(b.size() - 4) == ".mp3";
        if (am != bm) return am;
        if (depth(a) != depth(b)) return depth(a) < depth(b);
        return files.at(a).mtime > files.at(b).mtime;
    });
    json docs = json::object();
    for (auto &[p, f] : files)
        if (p.size() > 3 && p.substr(p.size() - 3) == ".md" && f.size < 200000) docs[p] = readFile(dir / p);
    json info = json::object();
    {
        Song sg;
        std::string err;
        if (openSong(dir.string(), sg, err) && sg.hasManifest())
            info = {{"title", sg.manifest.value("title", slug)}, {"authors", sg.manifest.value("authors", json::array())}};
    }
    // note edits (edits.json) as the timeline shows them: applied, listed, and the ones that no longer match
    json edits = json::array(), unmatchedEdits = json::array();
    if (hasJob) {
        std::vector<std::pair<std::string, std::string>> misses;
        job = applySounds(applyNoteEdits(job, (dir / jobPath).parent_path().string(), &misses), (dir / jobPath).parent_path().string());
        for (auto &[t, why] : misses) unmatchedEdits.push_back({{"track", t}, {"why", why}});
        json ef;
        if (readJson(dir / "edits.json", ef) && ef.contains("edits") && ef["edits"].is_array()) edits = ef["edits"];
    }
    return {{"slug", slug}, {"meta", info}, {"files", filesJson(files)}, {"jobPath", jobPath.empty() ? json(nullptr) : json(jobPath)},
            {"edits", edits}, {"unmatchedEdits", unmatchedEdits},
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
    json o = {{"id", p.id}, {"song", p.song}, {"status", p.status}, {"tracks", p.tracks}, {"from", p.from}, {"to", p.to}, {"full", p.full}};
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
                      std::to_string(from) + "|" + std::to_string(to) + "|" + (files.count("edits.json") ? std::to_string(files.at("edits.json").mtime) : "") + "|" +
                      (files.count("sounds.json") ? std::to_string(files.at("sounds.json").mtime) : "");
    for (auto &t : tracks) sig += "|" + t;
    const std::string id = fnv(sig);
    std::lock_guard<std::mutex> lock(mu_);
    auto it = previews_.find(id);
    std::error_code gone;
    if (it != previews_.end() && it->second.status == "ready" && !fs::exists(dir / it->second.path, gone)) {   // `purge` took it: render again
        previews_.erase(it);
        it = previews_.end();
    }
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
    p.run = ++runs_;
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

// The Render button: the song's job rendered in full into the folder its last report is in (out/ by
// default), with an mp3 and the picture. Queued with the previews, one render at a time.
json Server::startRender(const std::string &slug) {
    const fs::path dir = songDir(slug);
    const FileMap files = scanSong(dir);
    const std::string jobPath = pick(files, "job.json", "job.json"), reportPath = pick(files, "report.json", "out/report.json");
    if (jobPath.empty()) return {{"error", "the song has no job.json yet"}};
    std::lock_guard<std::mutex> lock(mu_);
    for (auto &[id, q] : previews_)
        if (q.song == slug && q.full && (q.status == "queued" || q.status == "rendering")) return previewJson(q);
    Preview p;
    p.id = "render-" + fnv(slug + std::to_string(nowSec()));
    p.song = slug;
    p.full = true;
    p.dir = (reportPath.empty() ? dir / "out" : (dir / reportPath).parent_path()).string();
    p.queuedAt = nowSec();
    p.run = ++runs_;
    previews_[p.id] = p;
    queue_.push_back(p.id);
    ++previewVersion_;
    cv_.notify_all();
    return previewJson(p);
}

void Server::previewWorker() {
    double reaped = nowSec();
    while (!stopping_) {
        if (nowSec() - reaped > 30) { reapPlayers(600); reaped = nowSec(); }   // this loop wakes every second
        std::string id;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] { return !queue_.empty() || stopping_; });
            if (queue_.empty()) continue;
            id = queue_.front();
            queue_.pop_front();
            if (previews_[id].status == "cancelled") continue;
            previews_[id].status = "rendering";
            previews_[id].startedAt = nowSec();
            previews_[id].finishedAt = 0;
            previews_[id].error.clear();
        }
        ++previewVersion_;
        Preview p;
        { std::lock_guard<std::mutex> lock(mu_); p = previews_[id]; }
        const fs::path dir = songDir(p.song);
        const FileMap files = scanSong(dir);
        const std::string jobPath = pick(files, "job.json", "job.json"), reportPath = pick(files, "report.json", "out/report.json");
        std::vector<std::string> args = {platform::selfExecutable(), "render", (dir / jobPath).string(), "--out", p.dir, "--stems", "none", "--json"};
        if (p.full) args = {platform::selfExecutable(), "render", (dir / jobPath).string(), "--out", p.dir, "--deliver", "mp3", "--png", "--json"};
        else if (!reportPath.empty()) { args.push_back("--level-from"); args.push_back((dir / reportPath).string()); }
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
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cancelled = previews_[id].status == "cancelled";   // cancelled before it started
        }
        if (cancelled) error = "cancelled";
        else if (!platform::spawn(args, proc, true, true)) error = "could not start a render";
        else {
            {
                std::lock_guard<std::mutex> lock(mu_);
                running_ = proc;
                runningId_ = id;
            }
            platform::readOutput(proc, out, 900);
            std::string crash;
            while (!platform::finished(proc, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            try { rep = json::parse(out); } catch (...) {}
            ok = rep.is_object() && rep.value("ok", false);
            if (!ok) error = rep.is_object() && rep.contains("error") ? rep["error"].get<std::string>() : (crash.empty() ? "the render failed" : "the render crashed: " + crash);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            running_ = platform::Process();
            runningId_.clear();
            Preview &q = previews_[id];
            if (q.run != p.run) {   // asked for again while this run ended: the new request stands
                ++previewVersion_;
                continue;
            }
            q.finishedAt = nowSec();
            if (q.status == "cancelled") {   // stopped on request: no half-written preview left behind
                if (!q.full) fs::remove_all(p.dir, ec);
            } else if (ok) {
                q.status = "ready";
                q.path = fs::relative(fs::path(p.dir) / "mix.wav", dir, ec).generic_string();
                q.window = rep.value("window", json());
            } else {
                q.status = "failed";
                q.error = error.substr(0, 600);
            }
        }
        ++previewVersion_;
        if (!p.full) prunePreviews(fs::path(p.dir).parent_path());
    }
}

// The live loop: one render at a time, always of the newest request of a song (older ones are skipped).
// Output in out/.loop (hidden from the file list); the audio is kept in memory for the page to fetch.
void Server::loopWorker() {
    while (!stopping_) {
        std::string slug;
        LoopState req;
        {
            std::unique_lock<std::mutex> lock(loopMu_);
            loopCv_.wait_for(lock, std::chrono::milliseconds(500), [&] {
                if (stopping_) return true;
                for (auto &[s, l] : loops_) if (l.want > l.taken) return true;
                return false;
            });
            if (stopping_) break;
            for (auto &[s, l] : loops_) if (l.want > l.taken) { slug = s; break; }
            if (slug.empty()) continue;
            LoopState &l = loops_[slug];
            l.taken = l.want;
            l.status = "rendering";
            req.taken = l.taken; req.from = l.from; req.to = l.to; req.mix = l.mix;
        }
        const double t0 = nowSec();
        const fs::path dir = songDir(slug);
        const FileMap files = scanSong(dir);
        const std::string jobPath = pick(files, "job.json", "job.json"), reportPath = pick(files, "report.json", "out/report.json");
        const fs::path out = dir / "out" / ".loop";
        std::vector<std::string> args = {platform::selfExecutable(), "render", (dir / jobPath).string(), "--from", std::to_string(req.from),
                                         "--to", std::to_string(req.to), "--loop", "--cache", "--no-png", "--stems", "none", "--json", "--out", out.string()};
        if (!reportPath.empty()) { args.push_back("--level-from"); args.push_back((dir / reportPath).string()); }
        if (!req.mix.empty()) { args.push_back("--mix"); args.push_back(req.mix.dump()); }
        std::string text, error, wav, crash;
        json rep;
        platform::Process proc;
        if (jobPath.empty()) error = "the song has no job.json yet";
        else if (!platform::spawn(args, proc, true, true)) error = "could not start a render";
        else {
            { std::lock_guard<std::mutex> lock(loopMu_); loopRunning_ = proc; }
            platform::readOutput(proc, text, 300);
            while (!platform::finished(proc, crash)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            { std::lock_guard<std::mutex> lock(loopMu_); loopRunning_ = platform::Process(); }
            try { rep = json::parse(text); } catch (...) {}
            if (!rep.is_object() || !rep.value("ok", false))
                error = rep.is_object() && rep.contains("error") ? rep["error"].get<std::string>() : (crash.empty() ? "the render failed" : "the render crashed: " + crash);
            else wav = readFile(out / "mix.wav");
            if (error.empty() && wav.empty()) error = "the render wrote no audio";
        }
        json info = json::object();
        if (error.empty()) {
            json tracks = json::array();
            int cached = 0;
            for (auto &t : rep.value("tracks", json::array())) {
                const bool c = t.value("cached", false);
                cached += c;
                if (!c) tracks.push_back({{"name", t.value("name", std::string())}, {"seconds", t.value("renderSeconds", 0.0)}});
            }
            info = {{"window", rep.value("window", json())}, {"seconds", std::round((nowSec() - t0) * 100) / 100}, {"cached", cached},
                    {"tracks", rep.value("tracks", json::array()).size()}, {"rendered", tracks}, {"lufs", rep["mix"].value("lufs", -120.0)}};
        }
        std::lock_guard<std::mutex> lock(loopMu_);
        auto it = loops_.find(slug);
        if (it == loops_.end()) continue;   // stopped while it rendered
        LoopState &l = it->second;
        if (req.taken > l.done) {
            l.done = req.taken;
            if (error.empty()) { l.wav = std::move(wav); l.info = info; l.error.clear(); }
            else l.error = error.substr(0, 600);
        }
        l.status = l.want > l.done ? "rendering" : l.error.empty() ? "ready" : "failed";
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

// Cancel a song's previews (or one by id): queued ones leave the queue, the one rendering is stopped.
json Server::cancelPreviews(const std::string &slug, const std::string &id) {
    std::lock_guard<std::mutex> lock(mu_);
    int n = 0;
    for (auto q = queue_.begin(); q != queue_.end();) {
        Preview &p = previews_[*q];
        if ((id.empty() && p.song == slug) || *q == id) { p.status = "cancelled"; p.finishedAt = nowSec(); q = queue_.erase(q); ++n; } else ++q;
    }
    if (!runningId_.empty()) {
        Preview &p = previews_[runningId_];
        if ((id.empty() && p.song == slug) || runningId_ == id) { p.status = "cancelled"; platform::terminate(running_); ++n; }
    }
    ++previewVersion_;
    return {{"ok", true}, {"cancelled", n}};
}

bool Server::rendering(const std::string &slug) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto &[id, p] : previews_) if (p.song == slug && (p.status == "rendering" || p.status == "queued")) return true;
    return false;
}

// What the details form edits: the manifest's fields, or what a new manifest would say
json Server::meta(const std::string &slug) {
    Song song;
    std::string err;
    const bool open = openSong(songDir(slug).string(), song, err);
    const json m = open && song.hasManifest() ? song.manifest : newManifest(songDir(slug));
    json out = {{"manifest", open && song.hasManifest()}, {"slug", slug}};
    for (const char *k : {"title", "authors", "summary", "description", "prompt", "license", "tags", "created", "updated"})
        if (m.contains(k)) out[k] = m[k];
    if (!out.contains("authors")) out["authors"] = json::array();
    if (!out.contains("tags")) out["tags"] = json::array();
    return out;
}

// a folder name the song format allows: lowercase a-z, 0-9 and single dashes
bool songSlug(const std::string &s) {
    if (s.empty() || s.size() > 80 || s.front() == '-' || s.back() == '-') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        if (c == '-' && i && s[i - 1] == '-') return false;
    }
    return true;
}

json Server::listing(const std::vector<std::string> &args, const std::string &key) {
    {
        std::lock_guard<std::mutex> lock(listMu_);
        auto it = lists_.find(key);
        if (it != lists_.end() && nowSec() - it->second.first < 600) return it->second.second;
    }
    json r = runSelf(args, 120);
    if (r.is_null()) return {{"error", "the listing failed"}};
    std::lock_guard<std::mutex> lock(listMu_);
    lists_[key] = {nowSec(), r};
    return r;
}

// Stop play workers idle for `idleSec` (a loaded instrument can hold a lot of memory).
void Server::reapPlayers(double idleSec) {
    {
        std::lock_guard<std::mutex> lock(knobsMu_);
        const double now = nowSec();
        for (auto it = knobs_.begin(); it != knobs_.end();) {
            if (now - it->second->lastUsed > idleSec && it->second->use.try_lock()) {
                platform::kill(it->second->proc);
                it->second->use.unlock();
                it = knobs_.erase(it);
            } else ++it;
        }
    }
    std::lock_guard<std::mutex> lock(playMu_);
    const double now = nowSec();
    for (auto it = players_.begin(); it != players_.end();) {
        if (now - it->second->lastUsed > idleSec && it->second->use.try_lock()) {
            platform::kill(it->second->proc);
            it->second->use.unlock();
            it = players_.erase(it);
        } else ++it;
    }
}

// Live notes through a track's own instrument: a __play worker keeps it loaded, so a note renders in
// milliseconds after the first request (which loads the plugin). The WAV comes back in `wav`.
json Server::playSong(const std::string &slug, const json &in, std::string &wav) {
    const fs::path dir = songDir(slug);
    const FileMap files = scanSong(dir);
    const std::string jobPath = pick(files, "job.json", "job.json"), track = in.value("track", std::string());
    if (jobPath.empty()) return {{"error", "the song has no job.json yet"}};
    if (track.empty()) return {{"error", "which track?"}};
    const std::string sounds = files.count("sounds.json") ? std::to_string(files.at("sounds.json").mtime) : "";   // a sound set since: load it again
    return play(dir / jobPath, track, slug + "|" + track + "|" + std::to_string(files.at(jobPath).mtime) + "|" + sounds, in, wav);
}

// The playground: any installed instrument and preset, as a one-track job of its own
json Server::playInstrument(const json &in, std::string &wav) {
    const std::string plugin = in.value("plugin", std::string()), preset = in.value("preset", std::string());
    if (plugin.empty()) return {{"error", "which instrument?"}};
    return play(playgroundJob(plugin, preset), "Play", "playground|" + plugin + "|" + preset, in, wav);
}

fs::path Server::playgroundJob(const std::string &plugin, const std::string &preset) {
    const fs::path dir = platform::cacheDir() / "playground" / fnv(plugin + "|" + preset);
    std::error_code ec;
    fs::create_directories(dir, ec);
    json track = {{"name", "Play"}, {"plugin", plugin}};
    if (!preset.empty()) track["preset"] = preset;
    const json job = {{"tempo", 120}, {"tracks", json::array({track})}};
    std::ofstream(dir / "job.json") << job.dump(1);
    return dir / "job.json";
}

// ---- the Sounds page: knobs, sounds.json, new projects ----

// A knobs worker by key (spawned from `jobFile` when there is none), its hello for a null request, else its answer.
json Server::knobsAsk(const std::string &key, const fs::path *jobFile, const json &request) {
    std::shared_ptr<PlayWorker> w;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(knobsMu_);
        const double now = nowSec();
        auto it = knobs_.find(key);
        if (it != knobs_.end()) w = it->second;
        else {
            if (!jobFile) return {{"error", "gone"}};
            while (knobs_.size() >= 4) {   // four instruments' knobs at most: drop the least recently used idle one
                auto lru = knobs_.end();
                for (auto j = knobs_.begin(); j != knobs_.end(); ++j)
                    if (lru == knobs_.end() || j->second->lastUsed < lru->second->lastUsed) lru = j;
                if (!lru->second->use.try_lock()) break;
                platform::kill(lru->second->proc);
                lru->second->use.unlock();
                knobs_.erase(lru);
            }
            w = std::make_shared<PlayWorker>();
            knobs_[key] = w;
            fresh = true;
        }
        w->lastUsed = now;
    }
    std::lock_guard<std::mutex> use(w->use);
    auto fail = [&](const std::string &e) {
        platform::kill(w->proc);
        std::lock_guard<std::mutex> lock(knobsMu_);
        auto it = knobs_.find(key);
        if (it != knobs_.end() && it->second == w) knobs_.erase(it);
        return json{{"error", e}};
    };
    if (fresh || !w->proc.handle) {
        if (!jobFile) return fail("gone");
        if (!platform::spawn({platform::selfExecutable(), "__knobs", jobFile->string(), "Knobs"}, w->proc, true, true, true))
            return fail("could not start the instrument");
        std::string hello;
        if (!platform::readLine(w->proc, hello, 90000)) return fail("the instrument did not load in 90 s");
        try { w->info = json::parse(hello); } catch (...) { return fail("the instrument answered: " + hello.substr(0, 200)); }
        if (w->info.contains("error")) return fail(w->info["error"].get<std::string>());
    }
    if (request.is_null()) return w->info;
    std::string reply;
    if (!platform::writeInput(w->proc, request.dump() + "\n") || !platform::readLine(w->proc, reply, 20000)) return fail("the instrument stopped");
    try { return json::parse(reply); } catch (...) { return fail("the instrument answered: " + reply.substr(0, 200)); }
}

// A track's sound as it plays (the job with sounds.json applied), or its sounds.json entry when the job has no such
// track: `sound` gets its instrument keys (a state file's path made absolute), `params` its "params".
bool Server::trackSound(const std::string &slug, const std::string &track, json &sound, json &params, std::string &err) {
    const fs::path dir = songDir(slug);
    const FileMap files = scanSong(dir);
    const std::string jobPath = pick(files, "job.json", "job.json");
    const fs::path base = jobPath.empty() ? dir : (dir / jobPath).parent_path();
    json job, t;
    if (!jobPath.empty() && readJson(dir / jobPath, job)) {
        job = applySounds(job, base.string());
        for (auto &x : job.value("tracks", json::array()))
            if (x.is_object() && x.value("name", std::string()) == track) t = x;
    }
    json sf;
    if (t.is_null() && readJson(base / "sounds.json", sf))
        for (auto &e : sf.value("tracks", json::array()))
            if (e.is_object() && e.value("track", std::string()) == track) t = e;
    if (t.is_null()) { err = "no track named " + track; return false; }
    sound = json::object();
    for (const char *k : {"plugin", "preset", "state", "synth", "sampler", "warmup", "realtime"})
        if (t.contains(k)) sound[k] = t[k];
    params = t.contains("params") && t["params"].is_object() ? t["params"] : json::object();
    auto absolute = [&](json &v) {
        if (!v.is_string()) return;
        const fs::path p = fs::u8path(v.get<std::string>());
        if (p.is_relative() && v.get<std::string>().rfind("lib:", 0) != 0) v = (base / p).lexically_normal().u8string();
    };
    if (sound.contains("state")) {
        if (sound["state"].is_string()) absolute(sound["state"]);
        else if (sound["state"].is_object() && sound["state"].contains("file")) absolute(sound["state"]["file"]);
    }
    if (!sound.contains("plugin") || !sound["plugin"].is_string()) { err = "track '" + track + "' has no instrument"; return false; }
    return true;
}

// POST /api/knobs {song, track} or {plugin, preset}: the instrument's parameters, the preset's values, and the
// track's own params read as values ("current")
json Server::knobsOpen(const json &in) {
    json sound, params = json::object();
    std::string err;
    if (in.contains("song")) {
        const std::string slug = in.value("song", std::string());
        if (!songExists(slug)) return {{"error", "unknown song"}};
        if (!trackSound(slug, in.value("track", std::string()), sound, params, err)) return {{"error", err}};
    } else {
        if (in.value("plugin", std::string()).empty()) return {{"error", "which instrument?"}};
        sound = {{"plugin", in.value("plugin", std::string())}};
        if (!in.value("preset", std::string()).empty()) sound["preset"] = in["preset"];
    }
    const std::string plugin = sound.value("plugin", std::string());
    if (plugin.rfind("builtin:", 0) == 0 && plugin != "builtin:synth") return {{"error", plugin + " has no knobs to turn here"}};
    const std::string key = fnv(sound.dump());
    const fs::path dir = platform::cacheDir() / "knobs" / key;
    std::error_code ec;
    fs::create_directories(dir, ec);
    json track = sound;
    track["name"] = "Knobs";
    track["notes"] = json::array();
    std::ofstream(dir / "job.json") << json{{"tempo", 120}, {"tracks", json::array({track})}}.dump(1);
    const fs::path jobFile = dir / "job.json";
    json out = knobsAsk(key, &jobFile, nullptr);
    if (out.contains("error")) return out;
    out["key"] = key;
    out["sound"] = sound;
    out["current"] = json::array();
    if (!params.empty()) {
        const json r = knobsAsk(key, nullptr, {{"resolve", params}});
        out["current"] = r.value("values", json::array());
        out["unread"] = r.value("errors", json::array());
    }
    return out;
}

// the song's manifest lists `path` as a source file while it exists (history and packages keep it)
void Server::listSource(const std::string &slug, const std::string &path, const std::string &mediaType) {
    Song song;
    std::string err;
    std::error_code ec;
    if (!openSong(songDir(slug).string(), song, err) || !song.hasManifest()) return;
    json &files = song.manifest["files"];
    if (!files.is_array()) files = json::array();
    const bool listed = std::any_of(files.begin(), files.end(), [&](const json &x) { return x.value("path", std::string()) == path; });
    // the job is the manifest's "job" and never a file entry too (validate refuses that); an entry an older build added goes
    const bool wanted = path != song.jobFile() && fs::exists(songDir(slug) / path, ec);
    if (wanted && !listed) files.push_back({{"path", path}, {"role", "source"}, {"mediaType", mediaType}});
    else if (!wanted && listed) {
        json kept = json::array();
        for (auto &x : files) if (x.value("path", std::string()) != path) kept.push_back(x);
        files = kept;
    } else return;
    writeManifest(song, err);
}

// GET /api/sounds: every track's sound as it plays, which ones sounds.json sets, the tempo and the brief
json Server::sounds(const std::string &slug) {
    const fs::path dir = songDir(slug);
    const FileMap files = scanSong(dir);
    const std::string jobPath = pick(files, "job.json", "job.json");
    const fs::path base = jobPath.empty() ? dir : (dir / jobPath).parent_path();
    json job, sf;
    const bool hasJob = !jobPath.empty() && readJson(dir / jobPath, job);
    if (!readJson(base / "sounds.json", sf)) sf = json::object();
    std::map<std::string, json> entries;
    for (auto &e : sf.value("tracks", json::array()))
        if (e.is_object() && e.contains("track") && e["track"].is_string()) entries[e["track"].get<std::string>()] = e;
    std::vector<std::pair<std::string, std::string>> misses;
    const json played = hasJob ? applySounds(job, base.string(), &misses) : json::object();
    json tracks = json::array();
    std::set<std::string> seen;
    auto row = [&](const json &t, bool inJob) {
        const std::string name = t.value(inJob ? "name" : "track", std::string());
        json r = {{"name", name}, {"inJob", inJob}, {"custom", entries.count(name) > 0}};
        for (const char *k : {"plugin", "preset", "state", "params"}) if (t.contains(k)) r[k] = t[k];
        r["notes"] = t.contains("notes") && t["notes"].is_array() ? t["notes"].size() : 0;
        r["clips"] = t.contains("clips") && t["clips"].is_array() ? t["clips"].size() : 0;
        if (entries.count(name) && entries[name].contains("note")) r["note"] = entries[name]["note"];
        for (auto &[mt, why] : misses) if (mt == name) r["unapplied"] = why;
        seen.insert(name);
        tracks.push_back(r);
    };
    for (auto &t : played.value("tracks", json::array())) if (t.is_object()) row(t, true);
    for (auto &[name, e] : entries) if (!seen.count(name)) row(e, false);
    json tempo = nullptr, meter = json::array({4, 4});
    if (hasJob && job.contains("tempo")) tempo = job["tempo"];
    if (hasJob && job.contains("timeSignature") && job["timeSignature"].is_array() && job["timeSignature"].size() == 2) meter = job["timeSignature"];
    const json rf = readRiffFile(base / "riffs.json");
    const json &riffs = rf["riffs"];
    bool generator = false;   // a script writes the job: the page says job.json may be written again
    for (auto &[path, info] : files) if (path.find('/') == std::string::npos && path.size() > 3 && path.substr(path.size() - 3) == ".py") generator = true;
    return {{"song", slug}, {"tracks", tracks}, {"tempo", tempo}, {"timeSignature", meter}, {"riffs", riffs}, {"hasJob", hasJob}, {"generator", generator},
            {"brief", files.count("brief.md") ? readFile(dir / "brief.md") : std::string()}, {"path", dir.string()},
            {"library", slug == kRiffLibrarySlug}, {"groups", riffGroups(rf)}, {"songsDir", root_.string()}};
}

// POST /api/sounds {song, op, ...}: "set" {track, sound, note} a track's whole instrument; "add" {track, sound, note} a new
// track (in job.json too, with no notes); "remove" {track}; "reset" {track} back to the job's own sound; "note" {track, note};
// "tempo" {tempo}; "brief" {text}
json Server::soundsChange(const std::string &slug, const json &in) {
    std::lock_guard<std::mutex> lock(mu_);
    const fs::path dir = songDir(slug);
    if (slug == kRiffLibrarySlug) { std::error_code ec; fs::create_directories(dir, ec); }
    const FileMap files = scanSong(dir);
    std::string jobPath = pick(files, "job.json", "job.json");
    const fs::path base = jobPath.empty() ? dir : (dir / jobPath).parent_path();
    const fs::path soundsFile = base / "sounds.json", jobFile = jobPath.empty() ? dir / "job.json" : dir / jobPath;
    json sf, job;
    if (!readJson(soundsFile, sf) || !sf.contains("tracks") || !sf["tracks"].is_array())
        sf = {{"format", "wavelength.sounds"}, {"formatVersion", "1.0"}, {"tracks", json::array()}};
    const bool hasJob = readJson(jobFile, job) && job.contains("tracks") && job["tracks"].is_array();
    const std::string op = in.value("op", std::string()), name = in.value("track", std::string());
    std::string err;
    bool jobChanged = false;
    auto entry = [&]() -> json * {
        for (auto &e : sf["tracks"]) if (e.is_object() && e.value("track", std::string()) == name) return &e;
        return nullptr;
    };
    auto jobTrack = [&]() -> json * {
        if (!hasJob) return nullptr;
        for (auto &t : job["tracks"]) if (t.is_object() && t.value("name", std::string()) == name) return &t;
        return nullptr;
    };
    auto soundFrom = [&](const json &src, json &e) -> bool {   // a whole instrument: plugin, preset, state, params
        if (!src.is_object() || !src.contains("plugin") || !src["plugin"].is_string() || src["plugin"].get<std::string>().empty()) { err = "a sound needs a plugin"; return false; }
        e["plugin"] = src["plugin"];
        for (const char *k : {"preset", "state", "params"}) e.erase(k);
        if (src.contains("preset") && src["preset"].is_string() && !src["preset"].get<std::string>().empty()) e["preset"] = src["preset"].get<std::string>().substr(0, 300);
        if (src.contains("state") && (src["state"].is_string() || src["state"].is_object())) e["state"] = src["state"];
        if (src.contains("params") && src["params"].is_object() && !src["params"].empty()) {
            json ps = json::object();
            for (auto &[k, v] : src["params"].items())
                if ((v.is_number() || v.is_string()) && ps.size() < 4000) ps[k.substr(0, 200)] = v;
            e["params"] = ps;
        }
        return true;
    };
    auto setNote = [&](json &e) {
        if (!in.contains("note")) return;
        const std::string n = in["note"].is_string() ? in["note"].get<std::string>() : "";
        if (n.find_first_not_of(" \n\t") == std::string::npos) e.erase("note"); else e["note"] = n.substr(0, 2000);
    };
    if (op == "set" || op == "add") {
        if (name.empty() || name.size() > 80) return {{"error", "a track needs a name (80 characters at most)"}};
        if (op == "add" && (entry() || jobTrack())) return {{"error", "there is already a track named " + name}};
        json e = entry() ? *entry() : json{{"track", name}};
        if (!soundFrom(in.value("sound", json()), e)) return {{"error", err}};
        setNote(e);
        if (json *x = entry()) *x = e; else sf["tracks"].push_back(e);
        if (!jobTrack()) {   // a new track: in job.json too, with no notes yet, so the job is whole without sounds.json
            json t = {{"name", name}};
            for (const char *k : {"plugin", "preset", "state", "params"}) if (e.contains(k)) t[k] = e[k];
            t["notes"] = json::array();
            if (!hasJob) job = {{"tempo", in.contains("tempo") && in["tempo"].is_number() ? in["tempo"] : json(128)}, {"tracks", json::array()}};
            job["tracks"].push_back(t);
            jobChanged = true;
        }
    } else if (op == "remove" || op == "reset") {
        if (op == "remove") {
            size_t n = 0;
            const json rf = readRiffFile(base / "riffs.json");   // named: a loop over a temporary's member reads freed memory
            for (const auto &r : rf["riffs"]) if (r.value("track", std::string()) == name) ++n;
            if (n) return {{"error", std::to_string(n) + (n == 1 ? " riff plays" : " riffs play") + " on '" + name + "'; move or delete them first"}};
        }
        json kept = json::array();
        for (auto &e : sf["tracks"]) if (!e.is_object() || e.value("track", std::string()) != name) kept.push_back(e);
        sf["tracks"] = kept;
        if (op == "remove") {
            if (json *t = jobTrack()) {
                const bool empty = (!t->contains("notes") || (*t)["notes"].empty()) && (!t->contains("clips") || (*t)["clips"].empty());
                if (!empty) return {{"error", "'" + name + "' has notes in the job; only a track without notes or clips can be removed here"}};
                json rest = json::array();
                for (auto &x : job["tracks"]) if (!x.is_object() || x.value("name", std::string()) != name) rest.push_back(x);
                job["tracks"] = rest;
                jobChanged = true;
            }
        }
    } else if (op == "note") {
        json *e = entry();
        if (!e) {   // words for a track whose sound comes from the job: its sound as it plays, then the note
            json sound, params;
            if (!trackSound(slug, name, sound, params, err)) return {{"error", err}};
            json fresh = {{"track", name}};
            if (json *t = jobTrack()) for (const char *k : {"plugin", "preset", "state", "params"}) if (t->contains(k)) fresh[k] = (*t)[k];
            sf["tracks"].push_back(fresh);
            e = &sf["tracks"].back();
        }
        setNote(*e);
    } else if (op == "tempo") {
        const double bpm = in.value("tempo", 0.0);
        if (!(bpm >= 20 && bpm <= 400)) return {{"error", "a tempo from 20 to 400 BPM"}};
        if (!hasJob) return {{"error", "add a track first"}};
        if (job.contains("tempo") && !job["tempo"].is_number()) return {{"error", "the job has a tempo map; change it in the job"}};
        job["tempo"] = std::round(bpm * 100) / 100;
        jobChanged = true;
    } else if (op == "riff" || op == "riff-delete" || op == "riff-group" || op == "group-add" || op == "group-rename" || op == "group-delete") {
        const fs::path rfile = base / "riffs.json";
        json rf = readRiffFile(rfile);
        json &list = rf["riffs"];
        std::vector<std::string> groups = riffGroups(rf);
        std::string savedId;
        auto at = [&](const std::string &id) { for (size_t i = 0; i < list.size(); ++i) if (list[i].value("id", std::string()) == id) return (long)i; return -1L; };
        auto ids = [&]() {   // {id} or {ids: [...]}
            std::vector<std::string> v;
            if (in.contains("ids") && in["ids"].is_array()) for (auto &x : in["ids"]) if (x.is_string()) v.push_back(x.get<std::string>());
            if (in.contains("id") && in["id"].is_string()) v.push_back(in["id"].get<std::string>());
            return v;
        };
        auto groupName = [&](const char *k) {
            std::string g = in.value(k, std::string()).substr(0, 80);
            g.erase(0, g.find_first_not_of(" \t"));
            g.erase(g.find_last_not_of(" \t") + 1);
            return g;
        };
        auto lower = [](std::string x) { std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return (char)std::tolower(c); }); return x; };
        auto known = [&](const std::string &g) { for (auto &x : groups) if (lower(x) == lower(g)) return true; return false; };
        auto spelled = [&](const std::string &g) { for (auto &x : groups) if (lower(x) == lower(g)) return x; return g; };   // a group as first written
        if (op == "riff-delete") {
            const std::vector<std::string> gone = ids();
            if (gone.empty()) return {{"error", "which riffs?"}};
            for (const auto &id : gone) {
                const long i = at(id);
                if (i < 0) return {{"error", "no riff " + id}};
                list.erase(list.begin() + i);
            }
        } else if (op == "riff-group") {   // {ids, group}: "" takes them out of their group
            const std::string g = spelled(groupName("group"));
            for (const auto &id : ids()) {
                const long i = at(id);
                if (i < 0) return {{"error", "no riff " + id}};
                if (g.empty()) list[(size_t)i].erase("group"); else list[(size_t)i]["group"] = g;
            }
            if (!g.empty() && !known(g)) groups.push_back(g);
        } else if (op == "group-add") {
            const std::string g = groupName("group");
            if (g.empty()) return {{"error", "a group needs a name"}};
            if (known(g)) return {{"error", "there is already a group named " + spelled(g)}};
            groups.push_back(g);
        } else if (op == "group-rename") {
            const std::string from = groupName("group"), to = groupName("to");
            if (to.empty()) return {{"error", "a group needs a name"}};
            if (!known(from)) return {{"error", "no group named " + from}};
            if (lower(to) != lower(from) && known(to)) return {{"error", "there is already a group named " + spelled(to)}};
            const std::string was = spelled(from);
            for (auto &g : groups) if (g == was) g = to;
            for (auto &r : list) if (lower(r.value("group", std::string())) == lower(was)) r["group"] = to;
        } else if (op == "group-delete") {   // its riffs stay, in no group
            const std::string g = spelled(groupName("group"));
            if (!known(g)) return {{"error", "no group named " + g}};
            groups.erase(std::remove(groups.begin(), groups.end(), g), groups.end());
            for (auto &r : list) if (lower(r.value("group", std::string())) == lower(g)) r.erase("group");
        } else {
            json r;
            if (!normalizeRiff(in.value("riff", json::object()), r, err)) return {{"error", err}};
            if (r.contains("group")) {   // a group named in another case is the same group
                r["group"] = spelled(r["group"].get<std::string>());
                if (!known(r["group"].get<std::string>())) groups.push_back(r["group"].get<std::string>());
            }
            if (!r.contains("id")) {   // a new one
                std::set<std::string> taken;
                for (auto &x : list) taken.insert(x.value("id", std::string()));
                int n = 1;
                while (taken.count("riff-" + std::to_string(n))) ++n;
                r["id"] = "riff-" + std::to_string(n);
                savedId = r["id"];
                r["created"] = nowRfc3339();
                r["by"] = actor();
                list.push_back(r);
            } else {
                savedId = r["id"];
                const long i = at(savedId);
                if (i < 0) return {{"error", "no such riff"}};
                for (const char *k : {"created", "by", "from"}) if (list[(size_t)i].contains(k) && !r.contains(k)) r[k] = list[(size_t)i][k];
                r["updated"] = nowRfc3339();
                list[(size_t)i] = r;
            }
        }
        rf["groups"] = groups;
        if (!writeRiffFile(rfile, rf, err)) return {{"error", err}};
        std::error_code ec;
        listSource(slug, fs::relative(rfile, dir, ec).generic_string(), "application/json");
        return {{"ok", true}, {"id", savedId}, {"groups", riffGroups(rf)}};
    } else if (op == "brief") {
        const std::string text = in.value("text", std::string());
        std::error_code ec;
        if (text.find_first_not_of(" \n\t") == std::string::npos) fs::remove(dir / "brief.md", ec);
        else if (!platform::writeFileAtomic(dir / "brief.md", text.substr(0, 20000) + (text.back() == '\n' ? "" : "\n"), err)) return {{"error", err}};
        listSource(slug, "brief.md", "text/markdown");
        return {{"ok", true}};
    } else return {{"error", "unknown op"}};

    std::error_code ec;
    if (jobChanged && !platform::writeFileAtomic(jobFile, job.dump(1) + "\n", err)) return {{"error", err}};
    if (sf["tracks"].empty()) fs::remove(soundsFile, ec);
    else if (!platform::writeFileAtomic(soundsFile, sf.dump(1) + "\n", err)) return {{"error", err}};
    const std::string rel = fs::relative(soundsFile, dir, ec).generic_string();
    listSource(slug, rel, "application/json");
    if (jobChanged) listSource(slug, fs::relative(jobFile, dir, ec).generic_string(), "application/json");
    return {{"ok", true}};
}

// POST /api/song/create {title}: a new song folder with a manifest; its job.json comes with the first track
json Server::createSong(const std::string &titleIn) {
    std::string title = titleIn.substr(0, 200);
    title.erase(0, title.find_first_not_of(" \t"));
    title.erase(title.find_last_not_of(" \t") + 1);
    if (title.empty()) return {{"error", "a project needs a name"}};
    std::string slug;
    for (unsigned char c : title) {
        if (std::isalnum(c)) slug += (char)std::tolower(c);
        else if (!slug.empty() && slug.back() != '-') slug += '-';
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.size() > 60) slug.resize(60);
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    if (slug.empty()) slug = "project";
    std::error_code ec;
    std::string pickSlug = slug;
    for (int n = 2; fs::exists(songDir(pickSlug), ec) && n < 1000; ++n) pickSlug = slug + "-" + std::to_string(n);
    if (!songSlug(pickSlug)) return {{"error", "could not make a folder name from that"}};
    if (!fs::create_directories(songDir(pickSlug), ec) || ec) return {{"error", "could not make the folder: " + ec.message()}};
    Song song;
    song.dir = songDir(pickSlug);
    song.manifest = newManifest(song.dir);
    song.manifest["title"] = title;
    song.manifest["slug"] = pickSlug;
    std::string err;
    if (!writeManifest(song, err)) return {{"error", err}};
    return {{"ok", true}, {"song", pickSlug}};
}

// A live session: {id, plugin, preset} or {id, song, track}. Replaces the page's earlier session.
json Server::liveStart(const json &in) {
    const std::string id = in.value("id", std::string());
    if (id.empty() || id.size() > 64) return {{"error", "a session id"}};
    fs::path jobFile;
    std::string track;
    if (in.contains("song")) {
        const std::string slug = in.value("song", std::string());
        if (!songExists(slug)) return {{"error", "unknown song"}};
        const std::string jp = pick(scanSong(songDir(slug)), "job.json", "job.json");
        if (jp.empty()) return {{"error", "the song has no job.json yet"}};
        jobFile = songDir(slug) / jp;
        track = in.value("track", std::string());
    } else {
        if (in.value("plugin", std::string()).empty()) return {{"error", "which instrument?"}};
        jobFile = playgroundJob(in.value("plugin", std::string()), in.value("preset", std::string()));
        track = "Play";
    }
    liveStop(id);
    {
        std::lock_guard<std::mutex> lock(liveMu_);
        while (lives_.size() >= 3) {   // three live instruments at most: stop the oldest
            platform::kill(lives_.begin()->second->proc);
            lives_.erase(lives_.begin());
        }
    }
    auto ls = std::make_shared<LiveSession>();
    std::error_code ec;
    fs::create_directories(platform::cacheDir() / "live", ec);
    ls->status = platform::cacheDir() / "live" / (fnv(id) + ".json");
    fs::remove(ls->status, ec);
    if (!platform::spawn({platform::selfExecutable(), "__live", jobFile.string(), track, ls->status.string()}, ls->proc, true, true, true)) return {{"error", "could not start the instrument"}};
    std::string hello;
    if (!platform::readLine(ls->proc, hello, 90000)) { platform::kill(ls->proc); return {{"error", "the instrument did not load in 90 s"}}; }
    try { ls->info = json::parse(hello); } catch (...) { platform::kill(ls->proc); return {{"error", "the instrument answered: " + hello.substr(0, 200)}}; }
    if (ls->info.contains("error")) { platform::kill(ls->proc); return ls->info; }
    {
        std::lock_guard<std::mutex> lock(liveMu_);
        lives_[id] = ls;
    }
    json o = ls->info;
    o["ok"] = true;
    return o;
}

void Server::liveStop(const std::string &id) {
    std::shared_ptr<LiveSession> ls;
    {
        std::lock_guard<std::mutex> lock(liveMu_);
        auto it = lives_.find(id);
        if (it == lives_.end()) return;
        ls = it->second;
        lives_.erase(it);
    }
    std::lock_guard<std::mutex> lock(ls->io);
    platform::writeInput(ls->proc, "{\"stop\":true}\n");
    platform::terminate(ls->proc);   // the stream's reader reaps it
    if (!ls->streaming) platform::kill(ls->proc);
    std::error_code ec;
    fs::remove(ls->status, ec);
}

json Server::play(const fs::path &jobFile, const std::string &track, const std::string &key, const json &in, std::string &wav) {
    const std::string slug = key.substr(0, key.find('|'));
    std::shared_ptr<PlayWorker> w;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(playMu_);
        const double now = nowSec();
        for (auto it = players_.begin(); it != players_.end();) {   // idle 10 min, or the song's job changed: stop it
            const bool stale = it->first.rfind(slug + "|" + track + "|", 0) == 0 && it->first != key;
            if ((now - it->second->lastUsed > 600 || stale) && it->second->use.try_lock()) {
                platform::kill(it->second->proc);
                it->second->use.unlock();
                it = players_.erase(it);
            } else ++it;
        }
        auto it = players_.find(key);
        if (it != players_.end()) w = it->second;
        else {
            while (players_.size() >= 4) {   // at most four instruments loaded: drop the least recently used idle one
                auto lru = players_.end();
                for (auto j = players_.begin(); j != players_.end(); ++j)
                    if (lru == players_.end() || j->second->lastUsed < lru->second->lastUsed) lru = j;
                if (!lru->second->use.try_lock()) break;
                platform::kill(lru->second->proc);
                lru->second->use.unlock();
                players_.erase(lru);
            }
            w = std::make_shared<PlayWorker>();
            w->lastUsed = now;
            players_[key] = w;
            fresh = true;
        }
        w->lastUsed = now;
    }
    std::lock_guard<std::mutex> use(w->use);
    auto fail = [&](const std::string &e) {
        platform::kill(w->proc);
        std::lock_guard<std::mutex> lock(playMu_);
        auto it = players_.find(key);
        if (it != players_.end() && it->second == w) players_.erase(it);
        return json{{"error", e}};
    };
    const auto t0 = Clock::now();
    if (fresh || !w->proc.handle) {
        if (!platform::spawn({platform::selfExecutable(), "__play", jobFile.string(), track}, w->proc, true, true, true))
            return fail("could not start the instrument");
        std::string hello;
        if (!platform::readLine(w->proc, hello, 90000)) return fail("the instrument did not load in 90 s");
        try { w->info = json::parse(hello); } catch (...) { return fail("the instrument answered: " + hello.substr(0, 200)); }
        if (w->info.contains("error")) return fail(w->info["error"].get<std::string>());
    }
    // the request: notes in seconds from the clip's start
    json notes = json::array();
    double end = 0;
    for (auto &n : in.value("notes", json::array())) {
        if (!n.is_object() || notes.size() >= 512) continue;   // a recorded riff plays back through here too
        const double start = std::min(40.0, std::max(0.0, n.value("start", 0.0))), dur = std::min(32.0, std::max(0.02, n.value("dur", 0.5)));
        notes.push_back({{"key", std::max(0, std::min(127, n.value("key", 60)))}, {"vel", std::max(0.0, std::min(1.0, n.value("vel", 0.8)))}, {"start", start}, {"dur", dur}});
        end = std::max(end, start + dur);
    }
    if (notes.empty()) return {{"error", "no notes"}};
    const fs::path out = platform::cacheDir() / "play" / (fnv(key) + "-" + std::to_string(platform::processId()) + ".wav");
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    // "length": at least this long (a riff's loop, past its last note)
    const double seconds = std::min(45.0, std::max(end + std::min(4.0, in.value("tail", 1.2)), in.value("length", 0.0)));
    const json req = {{"notes", notes}, {"seconds", seconds}, {"out", out.string()}};
    std::string line;
    if (!platform::writeInput(w->proc, req.dump() + "\n") || !platform::readLine(w->proc, line, 30000)) return fail("the instrument stopped answering");
    json r;
    try { r = json::parse(line); } catch (...) { return fail("the instrument answered: " + line.substr(0, 200)); }
    if (!r.value("ok", false)) return {{"error", r.value("error", std::string("the note did not render"))}};
    wav = readFile(out);
    fs::remove(out, ec);
    json o = w->info;
    o["ok"] = true;
    o["renderMs"] = r.value("ms", 0);
    o["ms"] = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    o["loaded"] = fresh;
    return o;
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
         : ext == ".svg" ? "image/svg+xml" : ext == ".png" ? "image/png" : "application/octet-stream";
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
    // a handler that throws (a song whose files have a shape it didn't expect) answers with the reason, not an empty 500
    http_.set_exception_handler([](const httplib::Request &, httplib::Response &res, std::exception_ptr ep) {
        std::string why = "unknown error";
        try { std::rethrow_exception(ep); } catch (const std::exception &e) { why = e.what(); } catch (...) {}
        res.status = 500;
        res.set_header("Cache-Control", "no-store");
        res.set_content(json{{"error", why}}.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
    });
    auto slugOf = [this](const httplib::Request &req, std::string &slug) { slug = req.get_param_value("song"); return songExists(slug); };
    auto songFrom = [&](const json &in, std::string &slug) { slug = in.value("song", std::string()); return songExists(slug); };

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
    http_.Get("/favicon.ico", [this](const httplib::Request &, httplib::Response &res) {   // for browsers that ask before reading the page's links
        std::string type, body = uiFile("icons/favicon-32.png", type);
        if (body.empty()) { res.status = 404; return; }
        res.set_header("Cache-Control", "max-age=86400");
        res.set_content(body, type);
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
    // comments as the page reads them: the spec's shape (review.hpp), each with how the song moved since
    auto reviewView = [&](const std::string &slug) {
        Song song;
        std::string err;
        if (!openSong(songDir(slug).string(), song, err)) { song = Song{}; song.dir = songDir(slug); }
        json out = {{"comments", json::array()}};
        const json data = review::read(song.dir);   // named: the loop must not outlive it
        for (auto &c : data["comments"]) {
            json n = review::normalize(c);
            n["now"] = review::status(song, c);
            out["comments"].push_back(n);
        }
        return out;
    };
    http_.Get("/api/review", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        sendJson(res, reviewView(s));
    });
    http_.Post("/api/review", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        std::lock_guard<std::mutex> lock(mu_);
        json data = review::read(songDir(s));
        const std::string op = in.value("op", std::string());
        if (op == "add") {
            std::string text = in.value("text", std::string());
            if (text.find_first_not_of(" \n\t") == std::string::npos) return sendJson(res, {{"error", "empty comment"}}, 400);
            const auto now = std::chrono::system_clock::now();
            const std::time_t tt = std::chrono::system_clock::to_time_t(now);
            char idb[32];
            std::strftime(idb, sizeof idb, "c%y%m%d%H%M%S", std::localtime(&tt));
            const std::string stamp = nowRfc3339();
            // the anchor keeps what was heard: the render's revision and report, the bars, time and tracks picked
            Song song;
            std::string serr;
            if (!openSong(songDir(s).string(), song, serr)) { song = Song{}; song.dir = songDir(s); }
            json c = {{"id", std::string(idb) + fnv(text + stamp).substr(0, 3)}, {"created", stamp}, {"author", actor()}, {"status", "open"},
                      {"text", text.substr(0, 4000)}, {"anchor", review::anchorFor(song, in, in.value("report", std::string()))}};
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
        std::string werr;
        if (!review::write(songDir(s), data, werr)) return sendJson(res, {{"error", werr}}, 500);
        sendJson(res, reviewView(s));
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

    // note edits: POST {song, op: "add", edits: [...]} appends to edits.json; "undo" drops the last saved batch;
    // "clear" removes the file. A song with a manifest lists edits.json as a source file (history keeps it).
    http_.Post("/api/edits", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s, err;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        std::lock_guard<std::mutex> lock(mu_);
        const fs::path f = songDir(s) / "edits.json";
        json file;
        if (!readJson(f, file) || !file.contains("edits") || !file["edits"].is_array()) file = {{"format", "wavelength.edits"}, {"formatVersion", "1.0"}, {"edits", json::array()}};
        const std::string op = in.value("op", std::string());
        std::error_code ec;
        if (op == "add") {
            const std::string batch = "b" + fnv(req.body + std::to_string(nowSec())).substr(0, 8), when = nowRfc3339();
            size_t n = 0;
            for (auto &e : in.value("edits", json::array())) {
                if (!e.is_object() || !e.contains("track") || !e["track"].is_string()) continue;
                json x = {{"track", e["track"]}, {"batch", batch}, {"time", when}, {"by", actor()}};
                if (e.contains("add") && e["add"].is_object()) x["add"] = e["add"];
                else if (e.contains("at") && e["at"].is_object()) {
                    x["at"] = e["at"];
                    if (e.value("delete", false)) x["delete"] = true;
                    else if (e.contains("to") && e["to"].is_object()) x["to"] = e["to"];
                    else continue;
                } else continue;
                file["edits"].push_back(x);
                ++n;
            }
            if (!n) return sendJson(res, {{"error", "no edits"}}, 400);
        } else if (op == "undo") {   // the last saved batch
            json &list = file["edits"];
            if (list.empty()) return sendJson(res, {{"error", "nothing to undo"}}, 400);
            const std::string last = list.back().value("batch", std::string());
            while (!list.empty() && list.back().value("batch", std::string()) == last) list.erase(list.end() - 1);
        } else if (op == "clear") {
            file["edits"] = json::array();
        } else return sendJson(res, {{"error", "unknown op"}}, 400);
        if (file["edits"].empty()) fs::remove(f, ec);
        else if (!platform::writeFileAtomic(f, file.dump(1) + "\n", err)) return sendJson(res, {{"error", err}}, 500);
        Song song;
        if (openSong(songDir(s).string(), song, err) && song.hasManifest()) {   // edits.json is part of the song's source
            json &files = song.manifest["files"];
            if (!files.is_array()) files = json::array();
            const bool listed = std::any_of(files.begin(), files.end(), [](const json &x) { return x.value("path", std::string()) == "edits.json"; });
            const bool exists = fs::exists(f, ec);
            if (exists && !listed) { files.push_back({{"path", "edits.json"}, {"role", "source"}, {"mediaType", "application/json"}}); writeManifest(song, err); }
            if (!exists && listed) {
                json kept = json::array();
                for (auto &x : files) if (x.value("path", std::string()) != "edits.json") kept.push_back(x);
                files = kept;
                writeManifest(song, err);
            }
        }
        sendJson(res, {{"ok", true}, {"count", file["edits"].size()}});
    });
    // the Sounds page: GET /api/sounds?song= lists every track's sound; POST /api/sounds {song, op, ...} changes
    // sounds.json (see soundsChange); POST /api/knobs {song, track} | {plugin, preset} loads an instrument's knobs;
    // POST /api/knobs/ask {key, text | parse | store} asks its worker; POST /api/song/create {title} makes a project
    http_.Get("/api/sounds", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        sendJson(res, sounds(s));
    });
    http_.Post("/api/sounds", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        const json r = soundsChange(s, in);
        sendJson(res, r, r.contains("error") ? 400 : 200);
    });
    http_.Post("/api/knobs", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        const json r = knobsOpen(in);
        sendJson(res, r, r.contains("error") ? 500 : 200);
    });
    http_.Post("/api/knobs/ask", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        json q = json::object();
        for (const char *k : {"text", "parse", "store"}) if (in.contains(k)) q[k] = in[k];
        if (in.contains("parse")) q["text"] = in.value("text", std::string());
        if (q.empty()) return sendJson(res, {{"error", "text, parse or store"}}, 400);
        const json r = knobsAsk(in.value("key", std::string()), nullptr, q);
        sendJson(res, r, r.value("error", std::string()) == "gone" ? 410 : 200);
    });
    // POST /api/riffs/use {song, ids}: library riffs copied into a song with the instruments they play on (riffs.hpp)
    http_.Post("/api/riffs/use", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s, err;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s) || s == kRiffLibrarySlug) return sendJson(res, {{"error", "unknown song"}}, 404);
        std::vector<std::string> ids;
        for (const auto &x : in.value("ids", json::array())) if (x.is_string()) ids.push_back(x.get<std::string>());
        if (ids.empty()) return sendJson(res, {{"error", "which riffs?"}}, 400);
        std::lock_guard<std::mutex> lock(mu_);
        const json rf = readRiffFile(riffLibraryDir() / "riffs.json");
        std::vector<json> picked;
        if (!resolveRiffs(rf, ids, picked, err)) return sendJson(res, {{"error", err}}, 400);
        json r = useRiffs(songDir(s), picked, riffLibraryDir(), err);
        if (!err.empty()) return sendJson(res, {{"error", err}}, 400);
        r["ok"] = true;
        sendJson(res, r);
    });
    http_.Post("/api/song/create", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        const json r = createSong(in.value("title", std::string()));
        sendJson(res, r, r.contains("error") ? 400 : 200);
    });
    // the Render button: POST {song}
    http_.Post("/api/render", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        json r = startRender(s);
        sendJson(res, r, r.contains("error") ? 400 : 200);
    });

    // cancel previews: POST {song} stops all of that song's, {id} one
    http_.Post("/api/preview/cancel", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        sendJson(res, cancelPreviews(in.value("song", std::string()), in.value("id", std::string())));
    });

    // the editor's live loop: POST {song, from, to (bars), mix: {track: {gain, pan, mute}}} asks for the loop as it
    // should sound now; GET says where it is; GET audio is the newest finished loop (a WAV); POST stop ends it
    http_.Post("/api/loop", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        const std::string s = in.value("song", std::string());
        if (!songExists(s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        const int from = in.value("from", 0), to = in.value("to", 0);
        if (from < 1 || to <= from) return sendJson(res, {{"error", "from/to are bars, to after from"}}, 400);
        std::lock_guard<std::mutex> lock(loopMu_);
        LoopState &l = loops_[s];
        if (l.from != from || l.to != to) { l.wav.clear(); l.info = json::object(); l.error.clear(); }   // other bars: the old loop is no answer
        l.want = ++loopSeq_;
        l.from = from;
        l.to = to;
        l.mix = in.contains("mix") && in["mix"].is_object() ? in["mix"] : json::object();
        if (l.status != "rendering") l.status = "queued";
        loopCv_.notify_all();
        sendJson(res, loopJson(l));
    });
    http_.Get("/api/loop", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(loopMu_);
        auto it = loops_.find(req.get_param_value("song"));
        if (it == loops_.end()) return sendJson(res, {{"error", "no loop"}}, 404);
        sendJson(res, loopJson(it->second));
    });
    http_.Get("/api/loop/audio", [&](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lock(loopMu_);
        auto it = loops_.find(req.get_param_value("song"));
        if (it == loops_.end() || it->second.wav.empty()) return sendJson(res, {{"error", "no loop audio yet"}}, 404);
        res.set_header("X-Wavelength-Loop", std::to_string(it->second.done));
        res.set_header("Cache-Control", "no-store");
        res.set_content(it->second.wav, "audio/wav");
    });
    http_.Post("/api/loop/stop", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        std::lock_guard<std::mutex> lock(loopMu_);
        loops_.erase(in.value("song", std::string()));
        sendJson(res, {{"ok", true}});
    });

    // song folder actions: rename, move to the trash, show in the file browser, edit the manifest's details
    http_.Post("/api/song/rename", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        if (s == kRiffLibrarySlug) return sendJson(res, {{"error", "the riff library is not a song"}}, 400);
        const std::string to = in.value("to", std::string());
        if (to == s) return sendJson(res, {{"ok", true}, {"song", s}});
        if (!songSlug(to)) return sendJson(res, {{"error", "a folder name is lowercase letters, digits and single dashes"}}, 400);
        std::error_code ec;
        if (fs::exists(songDir(to), ec)) return sendJson(res, {{"error", "there is already a folder named " + to}}, 409);
        if (rendering(s)) return sendJson(res, {{"error", "a preview of this song is rendering; cancel it first"}}, 409);
        fs::rename(songDir(s), songDir(to), ec);
        if (ec) return sendJson(res, {{"error", "could not rename the folder: " + ec.message()}}, 500);
        Song song;
        std::string err;
        if (openSong(songDir(to).string(), song, err) && song.hasManifest()) {   // the manifest's slug is the folder's name
            song.manifest["slug"] = to;
            song.manifest["updated"] = nowRfc3339();
            writeManifest(song, err);
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto it = previews_.begin(); it != previews_.end();) it = it->second.song == s ? previews_.erase(it) : std::next(it);
        }
        sendJson(res, {{"ok", true}, {"song", to}});
    });
    http_.Post("/api/song/trash", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        if (s == kRiffLibrarySlug) return sendJson(res, {{"error", "the riff library is not a song"}}, 400);
        cancelPreviews(s, "");
        for (int i = 0; i < 50 && rendering(s); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let a stopped render end
        std::string where, err;
        if (!platform::moveToTrash(songDir(s), where, err)) return sendJson(res, {{"error", err}}, 500);
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto it = previews_.begin(); it != previews_.end();) it = it->second.song == s ? previews_.erase(it) : std::next(it);
        }
        sendJson(res, {{"ok", true}, {"trash", where}});
    });
    http_.Post("/api/song/reveal", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s, err;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        const std::string file = in.value("file", std::string());
        fs::path target = songDir(s);
        if (!file.empty()) {
            if (!checkSongPath(file, err)) return sendJson(res, {{"error", err}}, 400);
            target /= file;
        }
        if (!platform::reveal(target, err)) return sendJson(res, {{"error", err}}, 500);
        sendJson(res, {{"ok", true}});
    });
    http_.Get("/api/meta", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        sendJson(res, meta(s));
    });
    // POST {song, title, authors: [{name, role, kind}], summary, description, license, tags}: writes
    // wavelength.json (a folder without one gets a new manifest, as `wavelength save` would make)
    http_.Post("/api/meta", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s, err;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        Song song;
        if (!openSong(songDir(s).string(), song, err)) { song = Song{}; song.dir = songDir(s); }
        if (!song.hasManifest()) song.manifest = newManifest(song.dir);
        json &m = song.manifest;
        auto text = [&](const char *k, size_t max) {
            if (!in.contains(k)) return;
            const std::string v = in[k].is_string() ? in[k].get<std::string>() : "";
            if (v.find_first_not_of(" \n\t") == std::string::npos) m.erase(k); else m[k] = v.substr(0, max);
        };
        if (in.contains("title")) {
            const std::string t = in["title"].is_string() ? in["title"].get<std::string>() : "";
            if (t.find_first_not_of(" \t") == std::string::npos) return sendJson(res, {{"error", "a song needs a title"}}, 400);
            m["title"] = t.substr(0, 200);
        }
        text("summary", 500);
        text("description", 20000);
        text("license", 100);
        if (in.contains("authors")) {
            json authors = json::array();
            for (auto &a : in["authors"].is_array() ? in["authors"] : json::array()) {
                if (!a.is_object() || !a.contains("name") || !a["name"].is_string()) continue;
                const std::string name = a["name"].get<std::string>();
                if (name.find_first_not_of(" \t") == std::string::npos) continue;
                json o = {{"name", name.substr(0, 120)}};
                if (a.contains("role") && a["role"].is_string() && !a["role"].get<std::string>().empty()) o["role"] = a["role"].get<std::string>().substr(0, 60);
                if (a.value("kind", std::string()) == "ai") o["kind"] = "ai";
                if (a.contains("url") && a["url"].is_string() && !a["url"].get<std::string>().empty()) o["url"] = a["url"];
                authors.push_back(o);
            }
            if (authors.empty()) m.erase("authors"); else m["authors"] = authors;
        }
        if (in.contains("tags")) {
            json tags = json::array();
            for (auto &t : in["tags"].is_array() ? in["tags"] : json::array())
                if (t.is_string() && !t.get<std::string>().empty()) tags.push_back(t.get<std::string>().substr(0, 40));
            if (tags.empty()) m.erase("tags"); else m["tags"] = tags;
        }
        m["slug"] = s;
        m["updated"] = nowRfc3339();
        m["generator"] = {{"name", "wavelength"}, {"version", WAVELENGTH_VERSION}};
        if (!writeManifest(song, err)) return sendJson(res, {{"error", err}}, 500);
        sendJson(res, meta(s));
    });

    // the playground's lists: instruments (plugins --json) and one instrument's presets
    http_.Get("/api/instruments", [&](const httplib::Request &, httplib::Response &res) {
        const json r = listing({"plugins", "--json"}, "plugins");
        if (r.contains("error")) return sendJson(res, r, 500);
        json out = json::array();
        for (auto &p : r.value("plugins", json::array()))
            if (!p.value("blocked", false) && p.contains("features") && std::find(p["features"].begin(), p["features"].end(), "instrument") != p["features"].end())
                out.push_back({{"format", p.value("format", "")}, {"id", p.value("id", "")}, {"name", p.value("name", "")}, {"vendor", p.value("vendor", "")},
                               {"arch", p.value("arch", "")}});
        sendJson(res, {{"instruments", out}});
    });
    http_.Get("/api/presets", [&](const httplib::Request &req, httplib::Response &res) {
        const std::string plugin = req.get_param_value("plugin");
        if (plugin.empty()) return sendJson(res, {{"error", "which instrument?"}}, 400);
        json r = listing({"presets", plugin, "--json"}, "presets|" + plugin);
        if (r.contains("error") && !r.contains("presets")) r["presets"] = json::array();
        sendJson(res, r);
    });
    // how much a song folder holds (the Trash dialog says it)
    http_.Get("/api/song/usage", [&](const httplib::Request &req, httplib::Response &res) {
        std::string s;
        if (!slugOf(req, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
        double bytes = 0;
        size_t count = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(songDir(s), fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) { bytes += (double)it->file_size(ec); ++count; }
        }
        sendJson(res, {{"bytes", bytes}, {"files", count}});
    });

    // live playing: POST /api/live/start {id, plugin, preset | song, track} loads the instrument; GET
    // /api/live/stream?id= is its audio (16-bit stereo PCM at the answer's sampleRate, as it plays);
    // POST /api/live/event {id, on: key, vel} / {id, off: key} / {id, allOff: true} / {id, param: id, value: plain};
    // POST /api/live/stop {id}
    http_.Post("/api/live/start", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        const json r = liveStart(in);
        sendJson(res, r, r.contains("error") ? (r.value("error", std::string()) == "live" ? 409 : 500) : 200);
    });
    http_.Get("/api/live/stream", [&](const httplib::Request &req, httplib::Response &res) {
        const std::string id = req.get_param_value("id");
        std::shared_ptr<LiveSession> ls;
        {
            std::lock_guard<std::mutex> lock(liveMu_);
            auto it = lives_.find(id);
            if (it != lives_.end()) ls = it->second;
        }
        if (!ls || ls->streaming.exchange(true)) return sendJson(res, {{"error", "no such live session"}}, 404);
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Accel-Buffering", "no");
        // text/event-stream: proxies (Caddy for *.localhost) pass it through unbuffered; the body is raw PCM
        res.set_chunked_content_provider("text/event-stream", [this, ls, id](size_t, httplib::DataSink &sink) {
            std::string chunk;
            while (!stopping_) {
                if (!platform::readSome(ls->proc, chunk, 200)) break;   // the worker ended
                if (!chunk.empty() && !sink.write(chunk.data(), chunk.size())) break;   // the page went away
            }
            std::string crash;
            platform::kill(ls->proc);
            {
                std::lock_guard<std::mutex> lock(liveMu_);
                auto it = lives_.find(id);
                if (it != lives_.end() && it->second == ls) lives_.erase(it);
            }
            sink.done();
            return true;
        });
    });
    http_.Post("/api/live/event", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        std::shared_ptr<LiveSession> ls;
        {
            std::lock_guard<std::mutex> lock(liveMu_);
            auto it = lives_.find(in.value("id", std::string()));
            if (it != lives_.end()) ls = it->second;
        }
        if (!ls) return sendJson(res, {{"error", "no such live session"}}, 404);
        std::string lines;   // one event, or "events": [...] in order (a chord, a release and a new key)
        for (const json &e : in.contains("events") && in["events"].is_array() ? in["events"] : json::array({in})) {
            if (e.contains("on")) lines += json{{"on", std::clamp(e.value("on", 60), 0, 127)}, {"vel", std::clamp(e.value("vel", 0.8), 0.0, 1.0)}}.dump() + "\n";
            else if (e.contains("off")) lines += json{{"off", std::clamp(e.value("off", 60), 0, 127)}}.dump() + "\n";
            else if (e.value("allOff", false)) lines += "{\"allOff\":true}\n";
            else if (e.contains("param") && e["param"].is_number_unsigned() && e.contains("value") && e["value"].is_number())
                lines += json{{"param", e["param"].get<uint32_t>()}, {"value", e["value"].get<double>()}}.dump() + "\n";
        }
        if (lines.empty()) return sendJson(res, {{"error", "on, off, allOff or param"}}, 400);
        std::lock_guard<std::mutex> lock(ls->io);
        if (!platform::writeInput(ls->proc, lines)) return sendJson(res, {{"error", "the instrument stopped"}}, 410);
        sendJson(res, {{"ok", true}});
    });
    // the plugin's own window: POST /api/live/editor {id, open}; GET /api/live/edits?id= is whether it's open and
    // the values changed in it ({"open", "error", "seq", "values": {id: plain}})
    http_.Post("/api/live/editor", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        std::shared_ptr<LiveSession> ls;
        {
            std::lock_guard<std::mutex> lock(liveMu_);
            auto it = lives_.find(in.value("id", std::string()));
            if (it != lives_.end()) ls = it->second;
        }
        if (!ls) return sendJson(res, {{"error", "no such live session"}}, 404);
        std::lock_guard<std::mutex> lock(ls->io);
        if (!platform::writeInput(ls->proc, in.value("open", true) ? "{\"editor\":true}\n" : "{\"editor\":false}\n"))
            return sendJson(res, {{"error", "the instrument stopped"}}, 410);
        sendJson(res, {{"ok", true}});
    });
    http_.Get("/api/live/edits", [&](const httplib::Request &req, httplib::Response &res) {
        std::shared_ptr<LiveSession> ls;
        {
            std::lock_guard<std::mutex> lock(liveMu_);
            auto it = lives_.find(req.get_param_value("id"));
            if (it != lives_.end()) ls = it->second;
        }
        if (!ls) return sendJson(res, {{"error", "no such live session"}}, 404);
        json st;
        if (!readJson(ls->status, st)) st = {{"open", false}, {"seq", 0}, {"values", json::object()}};
        sendJson(res, st);
    });
    http_.Post("/api/live/stop", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        liveStop(in.value("id", std::string()));
        sendJson(res, {{"ok", true}});
    });

    // live notes: POST {song, track, notes: [{key, vel, start, dur}] (seconds), tail} answers audio/wav,
    // with the worker's details in the X-Wavelength-Play header
    http_.Post("/api/play", [&](const httplib::Request &req, httplib::Response &res) {
        json in;
        std::string s;
        try { in = json::parse(req.body); } catch (...) { return sendJson(res, {{"error", "bad request"}}, 400); }
        std::string wav;
        json r;
        if (in.contains("song")) {
            if (!songFrom(in, s)) return sendJson(res, {{"error", "unknown song"}}, 404);
            r = playSong(s, in, wav);
        } else r = playInstrument(in, wav);
        if (r.contains("error")) return sendJson(res, r, 500);
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Wavelength-Play", r.dump(-1, ' ', true, json::error_handler_t::replace));
        res.set_content(wav, "audio/wav");
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
    std::thread looper([this] { loopWorker(); });
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
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);   // a play worker that died must not take the server with it on the next write
#endif
    const bool ok = http_.listen(opt_.host, opt_.port);
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(playMu_);
        for (auto &[k, w] : players_) platform::kill(w->proc);
        players_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(liveMu_);
        for (auto &[k, l] : lives_) platform::kill(l->proc);
        lives_.clear();
    }
    cv_.notify_all();
    worker.join();
    {
        std::lock_guard<std::mutex> lock(loopMu_);
        platform::kill(loopRunning_);
    }
    loopCv_.notify_all();
    looper.join();
    if (!ok) { std::fprintf(stderr, "error: could not listen on %s:%d (in use?)\n", opt_.host.c_str(), opt_.port); return 1; }
    return 0;
}

} // namespace

int serve(const ServeOptions &o) {
    Server s(o);
    return s.run();
}

// `wavelength __live <job.json> <track>` (internal, started by serve): the track's instrument playing
// continuously. After a JSON hello line, stdout is 16-bit stereo PCM paced to the clock (a little ahead);
// stdin lines turn notes on and off: {"on": 60, "vel": 0.8}, {"off": 60}, {"allOff": true}, {"stop": true};
// {"param": id, "value": plain} turns a knob; {"editor": true/false} opens or closes the plugin's own window, and
// `statusPath` (when given) keeps {"open", "error", "seq", "values": {id: plain}}: the values changed in it.
int liveWorker(const std::string &jobPath, const std::string &trackName, const std::string &statusPath, std::FILE *out) {
    auto say = [&](const json &j) { std::fprintf(out, "%s\n", j.dump(-1, ' ', false, json::error_handler_t::replace).c_str()); std::fflush(out); };
    json j;
    if (!readJson(jobPath, j)) { say({{"error", "cannot read " + jobPath}}); return 1; }
    Job job;
    std::string err;
    try {
        if (!parseJob(j, fs::path(jobPath).parent_path().string(), job, err)) { say({{"error", err}}); return 1; }
    } catch (const std::exception &e) { say({{"error", e.what()}}); return 1; }
    const auto it = std::find_if(job.tracks.begin(), job.tracks.end(), [&](const Track &t) { return t.name == trackName; });
    if (it == job.tracks.end()) { say({{"error", "no track named " + trackName}}); return 1; }
    const Track track = *it;
    if (isBuiltin(track.plugin)) { say({{"error", "live"}, {"why", "built-in instruments play note by note"}}); return 1; }
    PluginSetup setup;
    setup.spec = track.plugin;
    setup.stateFile = track.stateFile;
    setup.stateFormat = track.stateFormat;
    setup.params = track.params;
    setup.warmup = track.warmup;
    setup.realtime = track.realtime;
    setup.preset = track.preset;
    OpenedPlugin p;
    if (!openPlugin(setup, track.name, p, err)) { say({{"error", err}}); return 1; }
    if (!p.plugin->canPlayLive()) { say({{"error", "live"}, {"why", std::string(p.plugin->format()) + " instruments play note by note"}}); return 1; }
    Audio prime;   // settle it once with the full warmup (samples stream after activation)
    prime.resize((size_t)(0.2 * job.sampleRate));
    if (!runPlugin(job, p, {}, nullptr, prime, err)) { say({{"error", err}}); return 1; }
    p.plugin->warmup = 0.02;

    // knobs: {"param": id, "value": plain} reaches the plugin in the next block (CLAP wants each parameter's cookie)
    std::map<ParamId, void *> cookies;
    for (const auto &pi : p.plugin->params()) cookies[pi.id] = pi.cookie;

    // key presses from stdin, on a thread of their own
    std::mutex qm;
    std::vector<TimedEvent> queue;
    std::atomic<bool> stop{false};
    std::atomic<int> editorWant{0};   // 1 open the plugin's window, -1 close it
    std::thread reader([&] {
        char buf[4096];
        while (!stop && std::fgets(buf, sizeof buf, stdin)) {
            json e;
            try { e = json::parse(buf); } catch (...) { continue; }
            std::lock_guard<std::mutex> lock(qm);
            if (e.value("stop", false)) { stop = true; break; }
            if (e.contains("editor")) { editorWant = e.value("editor", false) ? 1 : -1; continue; }
            if (e.value("allOff", false)) {
                for (int k = 0; k < 128; ++k) queue.push_back({0, false, k, 0, 0.0});
            } else if (e.contains("on")) queue.push_back({0, true, e.value("on", 60), 0, e.value("vel", 0.8)});
            else if (e.contains("off")) queue.push_back({0, false, e.value("off", 60), 0, 0.0});
            else if (e.contains("param") && e["param"].is_number_unsigned() && e.contains("value") && e["value"].is_number()) {
                const auto c = cookies.find(e["param"].get<ParamId>());
                if (c == cookies.end()) continue;
                TimedEvent t{0, false, 0, 0, 0.0};
                t.kind = TimedEvent::Param;
                t.param = c->first;
                t.cookie = c->second;
                t.value = e["value"].get<double>();
                queue.push_back(t);
            }
        }
        stop = true;   // the server went away
    });
    reader.detach();   // blocked in fgets at exit; quickExit ends it

    Job live = job;
    live.blockSize = 128;   // 2.7 ms at 48 kHz: a key sounds within a block
    const double sr = job.sampleRate, lead = 0.025;   // stay 25 ms ahead of the clock
    const auto t0 = Clock::now();
    int64_t written = 0;
    std::vector<int16_t> pcm;
    say({{"track", track.name}, {"plugin", p.name}, {"preset", p.preset}, {"sampleRate", job.sampleRate}, {"gainDb", track.gainDb}});
    const float fader = (float)std::pow(10.0, std::min(track.gainDb, 12.0) / 20);
    p.plugin->liveEvents = [&](int64_t pos, uint32_t, std::vector<TimedEvent> &evs) {
        std::lock_guard<std::mutex> lock(qm);
        for (auto &e : queue) { e.frame = pos; evs.push_back(e); }
        queue.clear();
    };
    p.plugin->liveOutput = [&](const float *l, const float *r, uint32_t n) {
        pcm.resize((size_t)n * 2);
        for (uint32_t i = 0; i < n; ++i) {
            pcm[2 * i] = (int16_t)std::lround(std::clamp(l[i] * fader, -1.f, 1.f) * 32767);
            pcm[2 * i + 1] = (int16_t)std::lround(std::clamp(r[i] * fader, -1.f, 1.f) * 32767);
        }
        if (std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), out) != pcm.size() || std::fflush(out) != 0) return false;
        written += n;
        const auto due = t0 + std::chrono::duration<double>(written / sr - lead);
        std::this_thread::sleep_until(std::chrono::time_point_cast<Clock::duration>(due));
        return !stop.load();
    };
    // the plugin's own window, on this (the main) thread: opened and closed on request, kept going, and what it
    // changes written for serve every 100 ms
    json editState = {{"open", false}, {"seq", 0}, {"values", json::object()}};
    auto writeStatus = [&] {
        if (statusPath.empty()) return;
        editState["seq"] = editState["seq"].get<int>() + 1;
        std::string werr;
        platform::writeFileAtomic(statusPath, editState.dump(), werr);
    };
    auto takeEdits = [&] {
        std::vector<std::pair<ParamId, double>> edits;
        p.plugin->takeEdits(edits);
        for (auto &[id, v] : edits) editState["values"][std::to_string(id)] = v;
        return !edits.empty();
    };
    const std::string title = track.name + " - " + p.name + (p.preset.empty() ? "" : " - " + p.preset);
    auto lastLook = Clock::now();
    p.plugin->liveMain = [&] {
        const int want = editorWant.exchange(0);
        if (want == 1 && !p.plugin->editorOpen()) {
            std::string e;
            editState["values"] = json::object();
            if (p.plugin->openEditor(title, e)) { editState["open"] = true; editState.erase("error"); }
            else editState["error"] = e;
            writeStatus();
        } else if (want == -1 && p.plugin->editorOpen()) {
            takeEdits();
            p.plugin->closeEditor();
            editState["open"] = false;
            writeStatus();
        }
        if (!p.plugin->editorOpen()) return;
        if (!p.plugin->serviceEditor()) {   // the person closed it
            takeEdits();
            editState["open"] = false;
            writeStatus();
            return;
        }
        if (Clock::now() - lastLook > std::chrono::milliseconds(100)) {
            lastLook = Clock::now();
            if (takeEdits()) writeStatus();
        }
    };
    Audio none;
    const bool ok = runPlugin(live, p, {}, nullptr, none, err);
    p.plugin->closeEditor();
    if (!ok) { std::fprintf(stderr, "live: %s\n", err.c_str()); return 1; }
    return 0;
}

namespace {
// the Sounds page groups builtin:synth's parameters the way a synth panel would
const char *synthModule(const std::string &name) {
    if (name == "cutoff" || name == "resonance" || name == "drive" || name == "env" || name == "keytrack") return "Filter";
    if (name == "detune" || name == "spread") return "Unison";
    if (name == "pw" || name == "fm" || name == "sub" || name == "noise") return "Oscillators";
    if (name == "lfo" || name == "glide") return "Motion";
    return "Output";
}
std::string synthText(const SynthParamInfo &p, double v) {
    char buf[48];
    if (p.unit == "Hz") std::snprintf(buf, sizeof buf, v >= 1000 ? "%.2f kHz" : "%.0f Hz", v >= 1000 ? v / 1000 : v);
    else if (p.unit == "dB") std::snprintf(buf, sizeof buf, "%+.1f dB", v);
    else if (p.unit == "oct") std::snprintf(buf, sizeof buf, "%+.2f oct", v);
    else if (p.unit.empty()) std::snprintf(buf, sizeof buf, "%.2f", v);
    else std::snprintf(buf, sizeof buf, "%.2f %s", v, p.unit.c_str());
    return buf;
}
std::string lowerAscii(std::string s) { for (auto &c : s) c = (char)std::tolower((unsigned char)c); return s; }
double roundSig(double v, int digits) {
    if (v == 0 || !std::isfinite(v)) return v;
    const double m = std::pow(10.0, digits - 1 - (int)std::floor(std::log10(std::fabs(v))));
    return std::round(v * m) / m;
}
} // namespace

int knobsWorker(const std::string &jobPath, const std::string &trackName, std::FILE *out) {
    auto say = [&](const json &j) { std::fprintf(out, "%s\n", j.dump(-1, ' ', false, json::error_handler_t::replace).c_str()); std::fflush(out); };
    json j;
    if (!readJson(jobPath, j)) { say({{"error", "cannot read " + jobPath}}); return 1; }
    Job job;
    std::string err;
    try {
        if (!parseJob(j, fs::path(jobPath).parent_path().string(), job, err)) { say({{"error", err}}); return 1; }
    } catch (const std::exception &e) { say({{"error", e.what()}}); return 1; }
    const auto it = std::find_if(job.tracks.begin(), job.tracks.end(), [&](const Track &t) { return t.name == trackName; });
    if (it == job.tracks.end()) { say({{"error", "no track named " + trackName}}); return 1; }
    const Track track = *it;
    std::string line;
    char buf[1 << 16];
    auto next = [&]() -> bool {   // one line of stdin
        line.clear();
        for (;;) {
            if (!std::fgets(buf, sizeof buf, stdin)) return false;   // the server went away
            line += buf;
            if (!line.empty() && line.back() == '\n') return true;
        }
    };

    if (track.plugin == "builtin:synth") {   // the synth's named parameters, from its patch
        const auto &ps = synthParams();
        std::vector<double> patch, current;
        Track t = track;
        t.params.clear();
        if (!synthParamValues(job, t, patch, current, err)) { say({{"error", err}}); return 1; }
        json list = json::array();
        for (size_t i = 0; i < ps.size(); ++i)
            list.push_back({{"id", i}, {"name", ps[i].name}, {"module", synthModule(ps[i].name)}, {"min", ps[i].min}, {"max", ps[i].max},
                            {"default", ps[i].def}, {"value", patch[i]}, {"display", synthText(ps[i], patch[i])}, {"stepped", false},
                            {"curve", ps[i].exp ? "exp" : "linear"}, {"about", ps[i].description}});
        say({{"plugin", "builtin:synth"}, {"format", "builtin"}, {"preset", track.preset}, {"params", list}});
        auto find = [&](const std::string &key) -> int {
            for (size_t i = 0; i < ps.size(); ++i) if (lowerAscii(ps[i].name) == lowerAscii(key)) return (int)i;
            return -1;
        };
        while (next()) {
            json q;
            try { q = json::parse(line); } catch (...) { say({{"error", "bad request"}}); continue; }
            auto valid = [&](const json &id) { return id.is_number_unsigned() && id.get<size_t>() < ps.size(); };
            if (q.contains("resolve") && q["resolve"].is_object()) {
                json values = json::array(), errors = json::array();
                for (auto &[k, v] : q["resolve"].items()) {
                    const int i = find(k);
                    double x = 0;
                    if (i < 0) { errors.push_back("builtin:synth has no parameter '" + k + "'"); continue; }
                    if (v.is_number()) x = v.get<double>();
                    else if (!v.is_string() || !(Envelope::builtinText(v.get<std::string>(), x))) { errors.push_back("can't read the value of '" + k + "'"); continue; }
                    x = std::clamp(x, ps[(size_t)i].min, ps[(size_t)i].max);
                    values.push_back({{"key", k}, {"id", i}, {"value", x}, {"display", synthText(ps[(size_t)i], x)}});
                }
                say({{"values", values}, {"errors", errors}});
            } else if (q.contains("text") && q["text"].is_array()) {
                json texts = json::array();
                for (auto &pair : q["text"]) texts.push_back(pair.is_array() && pair.size() == 2 && valid(pair[0]) && pair[1].is_number()
                                                                  ? json(synthText(ps[pair[0].get<size_t>()], pair[1].get<double>())) : json(""));
                say({{"text", texts}});
            } else if (q.contains("parse") && valid(q["parse"])) {
                const auto &p = ps[q["parse"].get<size_t>()];
                double x = 0;
                if (!Envelope::builtinText(q.value("text", std::string()), x)) { say({{"error", "a number, a note name, or text like \"800 Hz\""}}); continue; }
                x = std::clamp(x, p.min, p.max);
                say({{"value", x}, {"display", synthText(p, x)}});
            } else if (q.contains("store") && q["store"].is_array()) {
                json values = json::array();
                for (auto &pair : q["store"]) values.push_back(pair.is_array() && pair.size() == 2 && pair[1].is_number() ? json(roundSig(pair[1].get<double>(), 4)) : json(nullptr));
                say({{"store", values}});
            } else say({{"error", "resolve, text, parse or store"}});
        }
        return 0;
    }
    if (isBuiltin(track.plugin)) { say({{"error", track.plugin + " has no knobs to turn here"}}); return 1; }

    PluginSetup setup;   // the preset and state; the track's own "params" come as a resolve request
    setup.spec = track.plugin;
    setup.stateFile = track.stateFile;
    setup.stateFormat = track.stateFormat;
    setup.warmup = track.warmup;
    setup.preset = track.preset;
    OpenedPlugin p;
    if (!openPlugin(setup, track.name, p, err)) { say({{"error", err}}); return 1; }
    Plugin &plug = *p.plugin;
    plug.pump(p.format == "vst3" ? 500 : 150);
    if (p.format == "vst3") {   // some plugins (Surge XT) only publish real values after processing a few blocks
        auto ps = plug.params();
        if (!ps.empty()) plug.commitParams({{ps.front().id, ps.front().cookie, ps.front().value}}, 48000, 512, err);
    }
    std::map<ParamId, ParamInfo> byId;
    json list = json::array();
    for (const auto &pi : plug.params()) {
        if (pi.hidden || pi.readonly || pi.name.rfind("MIDI CC ", 0) == 0 || pi.name == "MIDI") continue;   // JUCE's MIDI placeholders too
        byId[pi.id] = pi;
        json o = {{"id", pi.id}, {"name", pi.name}, {"module", pi.module}, {"min", pi.min}, {"max", pi.max}, {"default", pi.def},
                  {"value", pi.value}, {"display", pi.display}, {"stepped", pi.stepped}};
        if (pi.stepped && std::fabs(pi.max - pi.min) <= 128) {   // a switch or a menu: the name of each step
            json labels = json::array();
            for (double v = std::min(pi.min, pi.max); v <= std::max(pi.min, pi.max) + 1e-9; v += 1) {
                std::string t;
                labels.push_back(plug.textForValue(pi.id, v, t) ? t : std::to_string((int)std::lround(v)));
            }
            o["labels"] = labels;
        }
        list.push_back(o);
    }
    say({{"plugin", p.name}, {"format", p.format}, {"preset", p.preset}, {"params", list}, {"warnings", p.warnings}});
    auto text = [&](const ParamInfo &pi, double v) {
        std::string t;
        return plug.textForValue(pi.id, v, t) ? t : std::string();
    };
    auto read = [&](const ParamInfo &pi, const std::string &s, double &v) {   // display text or a note name, as render reads it
        double hz;
        if (Envelope::noteHz(s, hz)) {
            char b[40];
            std::snprintf(b, sizeof b, "%.2f Hz", hz);
            if (plug.valueFromText(pi.id, b, v)) return true;
        }
        return plug.valueFromText(pi.id, s, v);
    };
    while (next()) {
        json q;
        try { q = json::parse(line); } catch (...) { say({{"error", "bad request"}}); continue; }
        auto known = [&](const json &id) -> const ParamInfo * {
            if (!id.is_number_unsigned()) return nullptr;
            auto f = byId.find(id.get<ParamId>());
            return f == byId.end() ? nullptr : &f->second;
        };
        if (q.contains("resolve") && q["resolve"].is_object()) {
            json values = json::array(), errors = json::array();
            for (auto &[k, v] : q["resolve"].items()) {
                ParamInfo pi;
                if (!plug.findParam(k, pi)) { errors.push_back("no parameter '" + k + "' on " + p.name); continue; }
                double x = 0;
                if (v.is_number()) x = v.get<double>();
                else if (!v.is_string() || !read(pi, v.get<std::string>(), x)) { errors.push_back(p.name + " could not read the value of '" + k + "'"); continue; }
                x = std::clamp(x, std::min(pi.min, pi.max), std::max(pi.min, pi.max));
                values.push_back({{"key", k}, {"id", pi.id}, {"value", x}, {"display", text(pi, x)}});
            }
            say({{"values", values}, {"errors", errors}});
        } else if (q.contains("text") && q["text"].is_array()) {
            json texts = json::array();
            for (auto &pair : q["text"]) {
                const ParamInfo *pi = pair.is_array() && pair.size() == 2 && pair[1].is_number() ? known(pair[0]) : nullptr;
                texts.push_back(pi ? text(*pi, pair[1].get<double>()) : std::string());
            }
            say({{"text", texts}});
        } else if (q.contains("parse") && known(q["parse"])) {
            const ParamInfo &pi = *known(q["parse"]);
            double x = 0;
            if (!read(pi, q.value("text", std::string()), x)) { say({{"error", p.name + " can't read that as a value of '" + pi.name + "'"}}); continue; }
            x = std::clamp(x, std::min(pi.min, pi.max), std::max(pi.min, pi.max));
            say({{"value", x}, {"display", text(pi, x)}});
        } else if (q.contains("store") && q["store"].is_array()) {
            json values = json::array();
            for (auto &pair : q["store"]) {
                const ParamInfo *pi = pair.is_array() && pair.size() == 2 && pair[1].is_number() ? known(pair[0]) : nullptr;
                if (!pi) { values.push_back(nullptr); continue; }
                const double v = pair[1].get<double>(), range = std::fabs(pi->max - pi->min);
                const std::string t = text(*pi, v);
                double back = 0;
                // readable text when the plugin reads it back as the same value: a step exactly, else within 0.1% of the range
                const bool same = !t.empty() && read(*pi, t, back) && (pi->stepped ? std::lround(back) == std::lround(v) : std::fabs(back - v) <= 0.001 * range);
                values.push_back(same ? json(t) : json(roundSig(v, 6)));
            }
            say({{"store", values}});
        } else say({{"error", "resolve, text, parse or store"}});
    }
    return 0;
}

int playWorker(const std::string &jobPath, const std::string &trackName, std::FILE *out) {
    auto say = [&](const json &j) { std::fprintf(out, "%s\n", j.dump(-1, ' ', false, json::error_handler_t::replace).c_str()); std::fflush(out); };
    json j;
    if (!readJson(jobPath, j)) { say({{"error", "cannot read " + jobPath}}); return 1; }
    Job job;
    std::string err;
    try {
        if (!parseJob(j, fs::path(jobPath).parent_path().string(), job, err)) { say({{"error", err}}); return 1; }
    } catch (const std::exception &e) { say({{"error", e.what()}}); return 1; }
    const auto it = std::find_if(job.tracks.begin(), job.tracks.end(), [&](const Track &t) { return t.name == trackName; });
    if (it == job.tracks.end()) { say({{"error", "no track named " + trackName}}); return 1; }
    const Track track = *it;
    const bool builtin = isBuiltin(track.plugin);
    OpenedPlugin p;
    if (!builtin) {
        PluginSetup setup;
        setup.spec = track.plugin;
        setup.stateFile = track.stateFile;
        setup.stateFormat = track.stateFormat;
        setup.params = track.params;
        setup.warmup = track.warmup;
        setup.realtime = track.realtime;
        setup.preset = track.preset;
        if (!openPlugin(setup, track.name, p, err)) { say({{"error", err}}); return 1; }
        // one silent render with the full warmup (sampled instruments stream after activation), then short ones
        Audio prime;
        prime.resize((size_t)(0.2 * job.sampleRate));
        if (!runPlugin(job, p, {}, nullptr, prime, err)) { say({{"error", err}}); return 1; }
        p.plugin->warmup = 0.02;
    }
    say({{"track", track.name}, {"plugin", builtin ? track.plugin : p.name}, {"preset", builtin ? track.preset : p.preset}, {"gainDb", track.gainDb}});
    Job quick = job;   // the instrument is loaded and settled: no wall-clock warmup per note
    quick.warmup = 0;
    const double fader = std::pow(10.0, track.gainDb / 20);
    std::string line;
    char buf[1 << 16];
    for (;;) {
        line.clear();
        for (;;) {   // one line of stdin (fgets: plain C stdio, no iostream in this file)
            if (!std::fgets(buf, sizeof buf, stdin)) return 0;   // the server went away
            line += buf;
            if (!line.empty() && line.back() == '\n') break;
        }
        json req;
        try { req = json::parse(line); } catch (...) { say({{"ok", false}, {"error", "bad request"}}); continue; }
        const auto t0 = Clock::now();
        Track t = track;
        t.notes.clear();
        for (auto &n : req.value("notes", json::array())) {
            Note note{};
            note.start = n.value("start", 0.0);
            note.length = n.value("dur", 0.5);
            note.key = n.value("key", 60);
            note.channel = 0;
            note.velocity = n.value("vel", 0.8);
            t.notes.push_back(note);
        }
        Audio audio;
        audio.resize((size_t)(std::max(0.1, req.value("seconds", 1.5)) * job.sampleRate));
        std::vector<std::string> warnings;
        bool ok;
        if (builtin) ok = renderBuiltin(t.plugin, quick, t, audio, warnings, err);
        else {
            const auto events = scheduleNotes(t.notes, job.sampleRate);
            ok = runPlugin(quick, p, events, nullptr, audio, err);
        }
        if (!ok) { say({{"ok", false}, {"error", err}}); continue; }
        muteGarbage(audio, job.sampleRate, t.name, warnings);
        for (size_t i = 0; i < audio.frames(); ++i) { audio.left[i] *= (float)fader; audio.right[i] *= (float)fader; }
        if (!writeWav(req.value("out", std::string()), audio, job.sampleRate, err, 16)) { say({{"ok", false}, {"error", err}}); continue; }
        say({{"ok", true}, {"ms", (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()}});
    }
}

} // namespace wl
