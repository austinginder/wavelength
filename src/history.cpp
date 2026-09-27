#include "history.hpp"

#include "platform.hpp"
#include "sha256.hpp"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace wl::history {

namespace {

fs::path dirOf(const Song &song) { return song.dir / "history"; }
fs::path logPath(const Song &song) { return dirOf(song) / "log.jsonl"; }
fs::path objectPath(const Song &song, const std::string &hex) { return dirOf(song) / "objects" / hex.substr(0, 2) / hex; }

std::string hexOf(const std::string &ref) { return ref.rfind("sha256:", 0) == 0 ? ref.substr(7) : ref; }

bool deflateZlib(const std::string &in, std::string &out) {
    uLongf n = compressBound((uLong)in.size());
    out.resize(n);
    if (compress2(reinterpret_cast<Bytef *>(&out[0]), &n, reinterpret_cast<const Bytef *>(in.data()), (uLong)in.size(), 9) != Z_OK) return false;
    out.resize(n);
    return true;
}

// one zlib stream with nothing after it, at most `limit` bytes out (a zlib bomb stops there)
constexpr size_t kMaxObject = (size_t)2 << 30;
bool inflateZlib(const std::string &in, std::string &out, size_t limit = kMaxObject) {
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    zs.avail_in = (uInt)in.size();
    out.clear();
    char buf[1 << 16];
    int rc;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        if ((rc != Z_OK && rc != Z_STREAM_END) || (rc == Z_OK && zs.avail_in == 0 && zs.avail_out != 0)) { inflateEnd(&zs); return false; }
        out.append(buf, sizeof buf - zs.avail_out);
        if (out.size() > limit) { inflateEnd(&zs); return false; }
    } while (rc != Z_STREAM_END);
    const bool trailing = zs.avail_in != 0;
    inflateEnd(&zs);
    return !trailing;
}

// history/lock, created exclusively while this program writes the log or objects (a folder: creating
// one is atomic everywhere); a lock older than a minute was left by a program that died
struct Lock {
    fs::path dir;
    bool held = false;
    bool take(const Song &song, std::string &err) {
        dir = song.dir / "history" / "lock";
        std::error_code ec;
        fs::create_directories(dir.parent_path(), ec);
        for (int i = 0; i < 100; ++i) {
            if (fs::create_directory(dir, ec)) return held = true;
            const auto age = fs::file_time_type::clock::now() - fs::last_write_time(dir, ec);
            if (!ec && age > std::chrono::minutes(1)) { fs::remove(dir, ec); continue; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        err = "another program is writing this song's history (" + dir.string() + "); try again";
        return false;
    }
    ~Lock() { std::error_code ec; if (held) fs::remove(dir, ec); }
};

bool storeObject(const Song &song, const std::string &content, std::string &hex, std::string &err) {
    hex = sha256Hex(content);
    const fs::path p = objectPath(song, hex);
    std::error_code ec;
    if (fs::exists(p, ec)) return true;   // content that appears in many revisions is stored once
    std::string z;
    if (!deflateZlib(content, z)) { err = "cannot compress an object"; return false; }
    return writeText(p, z, err);
}

// the entry's files written back into the folder, and tracked files it doesn't have deleted
bool writeFiles(const Song &song, const json &files, std::string &err) {
    std::error_code ec;
    for (const auto &path : trackedFiles(song))
        if (!files.contains(path) && !fs::is_symlink(song.dir / fs::u8path(path), ec)) fs::remove(song.dir / fs::u8path(path), ec);
    for (auto &[path, ref] : files.items()) {
        if (!checkSongPath(path, err)) return false;
        std::string content;
        if (!readObject(song, ref.get<std::string>(), content, err)) return false;
        if (!writeText(song.dir / path, content, err)) return false;
    }
    return true;
}

json lastFiles(const std::vector<json> &entries) { return entries.empty() ? json::object() : entries.back().value("files", json::object()); }

// Undo walks the song's changes the way an editor does: a save or render that changed files is a change
// (one that changed nothing isn't), a restore is one; undo takes the latest back, redo reapplies it until
// the next change.
struct Stacks { std::vector<int> undo, redo; };
Stacks stacks(const std::vector<json> &entries) {
    Stacks s;
    json top;   // files of s.undo.back()
    for (auto &e : entries) {
        const std::string op = e.value("op", std::string());
        const int rev = e.value("rev", 0);
        if (op == "undo") {
            if (s.undo.size() > 1) { s.redo.push_back(s.undo.back()); s.undo.pop_back(); }
        } else if (op == "redo") {
            if (!s.redo.empty()) { s.undo.push_back(s.redo.back()); s.redo.pop_back(); }
        } else if (op == "restore" || s.undo.empty() || e.value("files", json::object()) != top) {
            s.undo.push_back(rev);
            s.redo.clear();
        }
        if (!s.undo.empty()) { const json *t = find(entries, s.undo.back()); top = t ? t->value("files", json::object()) : json::object(); }
    }
    return s;
}

// before undo/redo/restore: unsaved changes become a save point, so nothing is lost
bool saveUnsaved(const Song &song, std::vector<json> &entries, const json &by, std::string &err) {
    json files;
    if (!snapshot(song, files, err)) return false;
    if (!entries.empty() && files == lastFiles(entries)) return true;
    json e = {{"op", "save"}, {"by", by}, {"message", "unsaved changes"}, {"files", files}};
    return append(song, entries, e, err) > 0;
}

int restoreStep(const Song &song, std::vector<json> &entries, int target, const std::string &op, const json &by, std::string &err) {
    const json *t = find(entries, target);
    if (!t) { err = "no revision " + std::to_string(target); return 0; }
    const json files = t->value("files", json::object());
    if (!writeFiles(song, files, err)) return 0;
    json e = {{"op", op}, {"by", by}, {"target", target}, {"files", files}};
    return append(song, entries, e, err);
}

// RFC 3339 -> "<unix seconds> <+hhmm>" (git's raw date)
std::string gitDate(const std::string &t) {
    int Y = 1970, M = 1, D = 1, h = 0, m = 0, s = 0, oh = 0, om = 0;
    char sign = '+';
    if (std::sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) < 6) return "0 +0000";
    const size_t z = t.find_first_of("+-Z", 19);
    if (z != std::string::npos && t[z] != 'Z') { sign = t[z]; std::sscanf(t.c_str() + z + 1, "%d:%d", &oh, &om); }
    // days from the civil date (Howard Hinnant's algorithm)
    const int y = Y - (M <= 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + doe - 719468;
    long long secs = days * 86400 + h * 3600 + m * 60 + s;
    const int off = (oh * 60 + om) * 60;
    secs -= sign == '-' ? -off : off;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%lld %c%02d%02d", secs, sign, oh, om);
    return buf;
}

bool git(const std::vector<std::string> &args, std::string &out, std::string &err) {
    const std::string g = platform::findProgram("git");
    if (g.empty()) { err = "exporting history needs git on the PATH"; return false; }
    std::vector<std::string> cmd = {g};
    cmd.insert(cmd.end(), args.begin(), args.end());
    platform::Process p;
    if (!platform::spawn(cmd, p, true, true)) { err = "cannot run git"; return false; }
    out.clear();
    platform::readOutput(p, out, 120);
    std::string crash;
    while (!platform::finished(p, crash)) platform::pumpEvents(5);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return true;
}

void setEnv(const char *k, const std::string &v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

} // namespace

bool read(const Song &song, std::vector<json> &entries, std::string &err, std::string *warning) {
    entries.clear();
    std::ifstream in(logPath(song));
    if (!in) return true;   // no history yet
    std::string line;
    size_t n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            json e;
            std::string why;
            if (!parseJsonStrict(line, e, why)) throw std::runtime_error(why);
            if (!e.is_object() || !e.contains("rev") || !e["rev"].is_number_integer()) throw std::runtime_error("not a revision");
            entries.push_back(std::move(e));
        } catch (const std::exception &) {
            if (in.peek() == EOF) { if (warning) *warning = "history/log.jsonl line " + std::to_string(n) + " is incomplete and was skipped"; break; }
            err = "history/log.jsonl line " + std::to_string(n) + " is not a revision";
            return false;
        }
    }
    return true;
}

int current(const std::vector<json> &entries) { return entries.empty() ? 0 : entries.back().value("rev", 0); }

const json *find(const std::vector<json> &entries, int rev) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        if (it->value("rev", -1) == rev) return &*it;
    return nullptr;
}

bool snapshot(const Song &song, json &files, std::string &err) {
    files = json::object();
    std::error_code ec;
    for (const auto &path : trackedFiles(song)) {
        if (fs::is_symlink(song.dir / fs::u8path(path), ec)) {   // never follow a link out of the song
            err = path + " is a symbolic link; a song can't contain links (copy the file in instead)";
            return false;
        }
        std::string hex;
        if (!storeObject(song, readText(song.dir / path), hex, err)) return false;
        files[path] = "sha256:" + hex;
    }
    return true;
}

int append(const Song &song, std::vector<json> &entries, json entry, std::string &err) {
    const int rev = entries.empty() ? 1 : entries.back().value("rev", 0) + 1;
    json e = {{"rev", rev}, {"time", nowRfc3339()}};
    for (auto &[k, v] : entry.items()) e[k] = v;
    e["parent"] = current(entries);
    std::error_code ec;
    fs::create_directories(dirOf(song), ec);
    std::ofstream out(logPath(song), std::ios::app | std::ios::binary);
    out << e.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
    if (!out) { err = "cannot write " + logPath(song).string(); return 0; }
    entries.push_back(e);
    return rev;
}

int save(const Song &song, const std::string &message, const json &by, bool always, std::string &err) {
    Lock lock;
    if (!lock.take(song, err)) return 0;
    std::vector<json> entries;
    if (!read(song, entries, err)) return 0;
    json files;
    if (!snapshot(song, files, err)) return 0;
    if (!always && message.empty() && !entries.empty() && files == lastFiles(entries)) return current(entries);
    json e = {{"op", "save"}, {"by", by}, {"files", files}};
    if (!message.empty()) { e["message"] = message; e["named"] = true; }
    return append(song, entries, e, err);
}

int undo(const Song &song, const json &by, std::string &err) {
    Lock lock;
    if (!lock.take(song, err)) return 0;
    std::vector<json> entries;
    if (!read(song, entries, err)) return 0;
    if (!saveUnsaved(song, entries, by, err)) return 0;
    const Stacks s = stacks(entries);
    if (s.undo.size() < 2) { err = entries.empty() ? "nothing to undo: the song has no revisions yet" : "nothing to undo: the song is at its first version"; return 0; }
    return restoreStep(song, entries, s.undo[s.undo.size() - 2], "undo", by, err);
}

int redo(const Song &song, const json &by, std::string &err) {
    Lock lock;
    if (!lock.take(song, err)) return 0;
    std::vector<json> entries;
    if (!read(song, entries, err)) return 0;
    const Stacks s = stacks(entries);
    if (s.redo.empty()) { err = "nothing to redo"; return 0; }
    json files;
    if (!snapshot(song, files, err)) return 0;
    if (files != lastFiles(entries)) { err = "the song changed since the undo; save or undo those changes before redo"; return 0; }
    return restoreStep(song, entries, s.redo.back(), "redo", by, err);
}

int restore(const Song &song, int target, const json &by, std::string &err) {
    Lock lock;
    if (!lock.take(song, err)) return 0;
    std::vector<json> entries;
    if (!read(song, entries, err)) return 0;
    if (!find(entries, target)) { err = "no revision " + std::to_string(target) + " in the history"; return 0; }
    if (!saveUnsaved(song, entries, by, err)) return 0;
    return restoreStep(song, entries, target, "restore", by, err);
}

int recordRender(const std::string &jobPath, const std::string &outDir, json &report, std::string &err) {
    Song song;
    if (!songOfJob(jobPath, song)) return 0;
    Lock lock;
    if (!lock.take(song, err)) return 0;
    std::vector<json> entries;
    if (!read(song, entries, err)) return 0;
    json files;
    if (!snapshot(song, files, err)) return 0;
    const int rev = entries.empty() ? 1 : entries.back().value("rev", 0) + 1;
    report["song"] = {{"revision", rev}, {"job", files.value(song.jobFile(), std::string())}};
    const fs::path reportPath = fs::path(outDir) / "report.json";
    if (!writeText(reportPath, report.dump(2, ' ', false, json::error_handler_t::replace) + "\n", err)) return 0;
    const json mix = report.value("mix", json::object());
    json r = {{"report", "sha256:" + sha256File(reportPath.string())}};
    const std::string mixFile = mix.value("file", std::string());
    if (!mixFile.empty()) { const std::string h = sha256File(mixFile); if (!h.empty()) r["mix"] = "sha256:" + h; }
    for (const char *k : {"lufs", "lra", "truePeakDb"})
        if (mix.contains(k) && mix[k].is_number()) r[std::string(k) == "truePeakDb" ? "truePeak" : k] = mix[k];
    if (report.contains("duration")) r["seconds"] = report["duration"];
    json sections = json::array();
    for (auto &s : report.value("sections", json::array())) sections.push_back({{"name", s.value("name", std::string())}, {"lufs", s.value("lufs", -120.0)}});
    if (!sections.empty()) r["sections"] = sections;
    json e = {{"op", "render"}, {"by", actor()}, {"files", files}, {"render", r}};
    return append(song, entries, e, err);
}

bool readObject(const Song &song, const std::string &hash, std::string &out, std::string &err) {
    if (hash.rfind("sha256:", 0) != 0) { err = "unsupported hash '" + hash.substr(0, 24) + "' (this Wavelength reads sha256)"; return false; }
    const std::string hex = hexOf(hash);
    if (hex.size() != 64 || hex.find_first_not_of("0123456789abcdef") != std::string::npos) { err = "bad object name '" + hash + "'"; return false; }
    std::ifstream in(objectPath(song, hex), std::ios::binary);
    if (!in) { err = "history object " + hex.substr(0, 12) + " is missing"; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    if (!inflateZlib(ss.str(), out)) { err = "history object " + hex.substr(0, 12) + " is corrupt"; return false; }
    if (sha256Hex(out) != hex) { err = "history object " + hex.substr(0, 12) + " does not match its hash"; return false; }
    return true;
}

bool objectIsFile(const Song &song, const std::string &path, const std::string &hash) {
    std::string why;
    std::error_code ec;
    if (!checkSongPath(path, why) || !fs::is_regular_file(song.dir / fs::u8path(path), ec)) return false;
    return sha256File((song.dir / fs::u8path(path)).string()) == hexOf(hash);
}

bool restoreObjects(const Song &song, int &restored, std::string &err) {
    restored = 0;
    std::vector<json> entries;
    if (!read(song, entries, err)) return false;
    std::error_code ec;
    std::set<std::string> done;
    for (auto &e : entries) {
        const json files = e.value("files", json::object());
        for (auto &[path, ref] : files.items()) {
            const std::string hex = hexOf(ref.get<std::string>());
            if (done.count(hex) || fs::exists(objectPath(song, hex), ec) || !objectIsFile(song, path, hex)) continue;
            std::string stored;
            if (!storeObject(song, readText(song.dir / fs::u8path(path)), stored, err)) return false;
            done.insert(hex);
            ++restored;
        }
    }
    return true;
}

bool readFile(const Song &song, int rev, const std::string &path, std::string &out, std::string &err) {
    std::vector<json> entries;
    if (!read(song, entries, err)) return false;
    const json *e = find(entries, rev);
    if (!e) { err = "no revision " + std::to_string(rev); return false; }
    const json files = e->value("files", json::object());
    if (!files.contains(path)) { err = path + " is not in revision " + std::to_string(rev); return false; }
    return readObject(song, files[path].get<std::string>(), out, err);
}

bool jobAt(const Song &song, int rev, json &job, std::string &err) {
    std::string text;
    if (rev <= 0) text = readText(song.jobPath());
    else if (!readFile(song, rev, song.jobFile(), text, err)) return false;
    try { job = json::parse(text); }
    catch (const std::exception &e) { err = std::string("the job is not valid JSON: ") + e.what(); return false; }
    return true;
}

bool exportGit(const Song &song, const std::string &target, bool bundle, std::string &err) {
    std::vector<json> entries;
    if (!read(song, entries, err)) return false;
    if (entries.empty()) { err = "the song has no history yet (wavelength save)"; return false; }
    std::error_code ec;
    const fs::path repo = bundle ? fs::temp_directory_path(ec) / ("wavelength-git-" + std::to_string(platform::processId())) : fs::absolute(target);
    if (!bundle && fs::exists(repo, ec) && !fs::is_empty(repo, ec)) { err = target + " exists and is not empty"; return false; }
    fs::remove_all(bundle ? repo : fs::path(), ec);
    fs::create_directories(repo, ec);
    struct Cleanup { fs::path p; bool on; ~Cleanup() { std::error_code c; if (on) fs::remove_all(p, c); } } cleanup{repo, bundle};
    const std::string R = repo.string();
    std::string out;
    if (!git({"init", "-q", "-b", "main", R}, out, err)) return false;
    const fs::path tmp = repo / ".git" / "wavelength-export";
    fs::create_directories(tmp, ec);
    const std::string index = (tmp / "index").string();
    std::map<std::string, std::string> blobs;   // sha256 -> git blob id
    std::map<int, std::string> commits;         // rev -> commit id
    for (auto &e : entries) {
        fs::remove(index, ec);
        setEnv("GIT_INDEX_FILE", index);
        const json files = e.value("files", json::object());   // named: items() must not outlive its object
        for (auto &[path, ref] : files.items()) {
            const std::string h = hexOf(ref.get<std::string>());
            if (!blobs.count(h)) {
                std::string content;
                if (!readObject(song, ref.get<std::string>(), content, err)) return false;
                const fs::path f = tmp / "blob";
                if (!writeText(f, content, err)) return false;
                if (!git({"-C", R, "hash-object", "-w", f.string()}, out, err) || out.size() < 40) { err = "git hash-object failed"; return false; }
                blobs[h] = out;
            }
            if (!git({"-C", R, "update-index", "--add", "--cacheinfo", "100644," + blobs[h] + "," + path}, out, err)) return false;
        }
        if (!git({"-C", R, "write-tree"}, out, err) || out.size() < 40) { err = "git write-tree failed"; return false; }
        const std::string tree = out;
        const json by = e.value("by", json::object());
        const std::string who = by.value("name", std::string("Wavelength")), when = gitDate(e.value("time", std::string()));
        for (const char *k : {"GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME"}) setEnv(k, who);
        for (const char *k : {"GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL"}) setEnv(k, "");
        for (const char *k : {"GIT_AUTHOR_DATE", "GIT_COMMITTER_DATE"}) setEnv(k, when);
        std::string msg = e.value("message", std::string());
        const std::string op = e.value("op", std::string());
        if (msg.empty()) msg = op == "render" ? "render" : op == "undo" || op == "redo" || op == "restore" ? op + " to revision " + std::to_string(e.value("target", 0)) : "save";
        msg += "\n\nWavelength-Revision: " + std::to_string(e.value("rev", 0));
        std::vector<std::string> args = {"-C", R, "commit-tree", tree};
        const int parent = e.value("parent", 0);
        if (parent > 0 && commits.count(parent)) { args.push_back("-p"); args.push_back(commits[parent]); }
        args.push_back("-m");
        args.push_back(msg);
        if (!git(args, out, err) || out.size() < 40) { err = "git commit-tree failed" + (out.empty() ? std::string() : ": " + out); return false; }
        commits[e.value("rev", 0)] = out;
    }
    const std::string head = commits[entries.back().value("rev", 0)];
    fs::remove_all(tmp, ec);
    for (const char *k : {"GIT_INDEX_FILE"}) setEnv(k, "");
#ifndef _WIN32
    unsetenv("GIT_INDEX_FILE");
#endif
    if (!git({"-C", R, "update-ref", "refs/heads/main", head}, out, err)) return false;
    if (bundle) {
        const std::string file = fs::absolute(target).string();
        if (!git({"-C", R, "bundle", "create", file, "HEAD", "main"}, out, err)) return false;
        if (!fs::exists(file, ec)) { err = "git bundle create failed"; return false; }
        return true;
    }
    return git({"-C", R, "reset", "-q", "--hard", "main"}, out, err);
}

} // namespace wl::history
