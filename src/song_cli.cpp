#include "song_cli.hpp"

#include "fallback.hpp"
#include "history.hpp"
#include "platform.hpp"
#include "package.hpp"
#include "review.hpp"
#include "song.hpp"
#include "songdiff.hpp"
#include "term.hpp"
#include "migrate.hpp"
#include "purge.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace wl {

namespace {

using json = nlohmann::json;

struct CliArgs {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opts;
    bool has(const std::string &k) const { return opts.count(k) > 0; }
    std::string get(const std::string &k, const std::string &d = "") const { auto it = opts.find(k); return it == opts.end() ? d : it->second; }
};

// options each command takes; true = the option takes a value
const std::map<std::string, std::map<std::string, bool>> kCommands = {
    {"save", {{"--message", true}, {"-m", true}, {"--json", false}}},
    {"history", {{"--json", false}, {"--named", false}, {"--to-git", true}, {"--bundle", true}}},
    {"undo", {{"--json", false}}},
    {"redo", {{"--json", false}}},
    {"restore", {{"--json", false}}},
    {"diff", {{"--json", false}}},
    {"pack", {{"--json", false}, {"--out", true}, {"--no-history", false}, {"--no-render", false}, {"--no-review", false}}},
    {"unpack", {{"--json", false}, {"--out", true}, {"--force", false}}},
    {"validate", {{"--json", false}}},
    {"fallbacks", {{"--json", false}, {"--suggest", false}, {"--write", false}, {"--no-measure", false}}},
    {"migrate", {{"--json", false}, {"--license", true}, {"--author", true}, {"--dry-run", false}, {"--no-copy", false}}},
    {"purge", {{"--json", false}, {"--dry-run", false}}},
    {"comments", {{"--json", false}, {"--all", false}, {"--reply", true}, {"--text", true}, {"--done", false}, {"--resolve", true}, {"--reopen", true}}},
};

struct Out {
    std::FILE *f;
    bool json;
    int fail(const std::string &err) const {
        if (json) { std::fputs((nlohmann::json{{"ok", false}, {"error", err}}.dump(2) + "\n").c_str(), f); }
        else if (term::err().on) std::fprintf(stderr, "%s %s %s\n", term::err().fail().c_str(), term::err().red(term::err().bold("error:")).c_str(), err.c_str());
        else std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    void emit(const nlohmann::json &j) const { std::fputs((j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n").c_str(), f); }
};

// a line that says something worked: "✓ ..." on a terminal, the plain text in a pipe
void done(const Out &o, const std::string &text) {
    if (term::out().on) std::fprintf(o.f, "%s %s\n", term::out().ok().c_str(), text.c_str());
    else std::fprintf(o.f, "%s\n", text.c_str());
}

bool parseArgs(int argc, char **argv, CliArgs &a, std::string &err) {
    const std::string cmd = argv[1];
    const auto &known = kCommands.at(cmd);
    a.pos.push_back(cmd);
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        if (s.size() > 1 && s[0] == '-' && !(s.size() > 1 && std::isdigit((unsigned char)s[1]))) {
            std::string key = s, val;
            const size_t eq = s.find('=');
            if (eq != std::string::npos && s.rfind("--", 0) == 0) { key = s.substr(0, eq); val = s.substr(eq + 1); }
            auto it = known.find(key);
            if (it == known.end()) {
                std::string list;
                for (auto &[k, v] : known) list += (list.empty() ? "" : " ") + k;
                err = "unknown option " + key + " for `" + cmd + "` (it takes: " + list + ")";
                return false;
            }
            if (it->second && eq == std::string::npos) {
                if (i + 1 >= argc) { err = key + " needs a value"; return false; }
                val = argv[++i];
            }
            a.opts[key == "-m" ? "--message" : key] = val;
        } else a.pos.push_back(s);
    }
    return true;
}

std::string summary(const json &e) {
    const std::string op = e.value("op", std::string()), msg = e.value("message", std::string());
    if (op == "undo" || op == "redo" || op == "restore") return op + ": back to revision " + std::to_string(e.value("target", 0));
    std::string s = msg.empty() ? op : msg;
    if (e.contains("render") && e["render"].contains("lufs")) {
        char b[48];
        std::snprintf(b, sizeof b, " (%.1f LUFS)", e["render"]["lufs"].get<double>());
        s += b;
    }
    return s;
}

// a song whose history can start: `save` gives a folder without a manifest one
bool songFor(const CliArgs &a, Song &song, const Out &o, bool create, std::string &err) {
    if (!openSong(a.pos.size() > 1 ? a.pos[1] : ".", song, err)) return false;
    if (!song.hasManifest()) {
        if (!create) { err = song.dir.filename().string() + " has no wavelength.json yet: `wavelength save` or `wavelength migrate` makes one"; return false; }
        song.manifest = newManifest(song.dir);
        song.manifest["generator"] = {{"name", "wavelength"}, {"version", WAVELENGTH_VERSION}};
        if (!writeManifest(song, err)) return false;
        if (!o.json) std::fprintf(o.f, "%s%s/wavelength.json\n", term::out().on ? (term::out().ok() + " wrote ").c_str() : "wrote ", song.dir.filename().string().c_str());
    }
    return true;
}

int cmdSave(const CliArgs &a, const Out &o) {
    Song song;
    std::string err;
    if (!songFor(a, song, o, true, err)) return o.fail(err);
    std::vector<json> before;
    if (!history::read(song, before, err)) return o.fail(err);
    const int prev = history::current(before);
    const int rev = history::save(song, a.get("--message"), actor(), false, err);
    if (!rev) return o.fail(err);
    if (o.json) o.emit({{"ok", true}, {"revision", rev}, {"changed", rev != prev}});
    else if (term::out().on) {
        const term::Style &st = term::out();
        if (rev == prev) std::fprintf(o.f, "%s\n", st.dim("nothing changed since revision " + std::to_string(rev)).c_str());
        else std::fprintf(o.f, "%s %s%s\n", st.ok().c_str(), st.bold("revision " + std::to_string(rev)).c_str(), a.has("--message") ? (" " + st.dim(a.get("--message"))).c_str() : "");
    }
    else if (rev == prev) std::fprintf(o.f, "nothing changed since revision %d\n", rev);
    else std::fprintf(o.f, "revision %d%s\n", rev, a.has("--message") ? (": " + a.get("--message")).c_str() : "");
    return 0;
}

int cmdHistory(const CliArgs &a, const Out &o) {
    Song song;
    std::string err, warning;
    if (!songFor(a, song, o, false, err)) return o.fail(err);
    if (a.has("--to-git") || a.has("--bundle")) {
        const bool bundle = a.has("--bundle");
        const std::string target = bundle ? a.get("--bundle") : a.get("--to-git");
        if (!history::exportGit(song, target, bundle, err)) return o.fail(err);
        if (o.json) o.emit({{"ok", true}, {bundle ? "bundle" : "repository", target}});
        else done(o, std::string(bundle ? "bundle " : "repository ") + target);
        return 0;
    }
    std::vector<json> entries;
    if (!history::read(song, entries, err, &warning)) return o.fail(err);
    const int cur = history::current(entries);
    if (o.json) {
        json list = json::array();
        for (auto &e : entries) {
            if (a.has("--named") && !e.value("named", false)) continue;
            json r = e;
            r.erase("files");
            r["fileCount"] = e.value("files", json::object()).size();
            list.push_back(r);
        }
        json r = {{"ok", true}, {"song", song.title()}, {"current", cur}, {"revisions", list}};
        if (!warning.empty()) r["warning"] = warning;
        o.emit(r);
        return 0;
    }
    if (entries.empty()) { std::fprintf(o.f, "%s has no revisions yet: `wavelength save` makes the first\n", song.title().c_str()); return 0; }
    const int latest = entries.back().value("rev", 0);
    for (auto &e : entries) {
        if (a.has("--named") && !e.value("named", false)) continue;
        const int rev = e.value("rev", 0);
        const std::string when = e.value("time", std::string()).substr(0, 16);
        const term::Style &st = term::out();
        if (st.on) {
            const std::string op = e.value("op", std::string()), o7 = op + std::string(op.size() < 7 ? 7 - op.size() : 0, ' ');
            const std::string opStyled = op == "save" ? st.green(o7) : op == "render" ? st.blue(o7) : st.yellow(o7);
            std::string r5 = "r" + std::to_string(rev);
            r5 += std::string(r5.size() < 5 ? 5 - r5.size() : 0, ' ');
            std::fprintf(o.f, "%s %s %s %s %s %s\n", rev == latest ? st.green("\u25cf").c_str() : " ", rev == latest ? st.bold(r5).c_str() : r5.c_str(),
                         st.dim(when).c_str(), opStyled.c_str(), st.dim(term::pad(e.value("by", json::object()).value("name", std::string()).substr(0, 18), 18)).c_str(),
                         summary(e).c_str());
            continue;
        }
        std::fprintf(o.f, "%s r%-4d %-16s %-7s %-18s %s\n", rev == latest ? "*" : " ", rev, when.c_str(), e.value("op", std::string()).c_str(),
                     e.value("by", json::object()).value("name", std::string()).substr(0, 18).c_str(), summary(e).c_str());
    }
    if (!warning.empty()) std::fprintf(stderr, "warning: %s\n", warning.c_str());
    return 0;
}

int cmdStep(const CliArgs &a, const Out &o) {
    Song song;
    std::string err;
    const std::string cmd = a.pos[0];
    CliArgs b = a;
    if (cmd == "restore") {   // restore [song] <rev>
        if (a.pos.size() < 2) return o.fail("usage: wavelength restore [song] <revision>");
        const std::string revArg = a.pos.back();
        b.pos.pop_back();
        char *end = nullptr;
        const long target = std::strtol(revArg.c_str() + (revArg[0] == 'r' ? 1 : 0), &end, 10);
        if (!end || *end || target <= 0) return o.fail("'" + revArg + "' is not a revision number");
        if (!songFor(b, song, o, false, err)) return o.fail(err);
        const int rev = history::restore(song, (int)target, actor(), err);
        if (!rev) return o.fail(err);
        if (o.json) o.emit({{"ok", true}, {"revision", rev}, {"restored", target}});
        else done(o, "restored revision " + std::to_string(target) + " (now revision " + std::to_string(rev) + ")");
        return 0;
    }
    if (!songFor(b, song, o, false, err)) return o.fail(err);
    const int rev = cmd == "undo" ? history::undo(song, actor(), err) : history::redo(song, actor(), err);
    if (!rev) return o.fail(err);
    std::vector<json> entries;
    history::read(song, entries, err);
    const json *e = history::find(entries, rev);
    const int target = e ? e->value("target", 0) : 0;
    if (o.json) o.emit({{"ok", true}, {"revision", rev}, {"restored", target}});
    else done(o, cmd + ": the song is back at revision " + std::to_string(target) + " (now revision " + std::to_string(rev) + ")");
    return 0;
}

// "r12", "12" -> 12; "now" -> 0; -1 when it isn't a revision
int revisionArg(const std::string &s) {
    if (s == "now") return 0;
    const std::string n = s.size() > 1 && (s[0] == 'r' || s[0] == 'R') ? s.substr(1) : s;
    if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos) return -1;
    return std::atoi(n.c_str());
}

int cmdDiff(const CliArgs &a, const Out &o) {
    std::vector<int> revs;
    CliArgs b = a;
    b.pos = {a.pos[0]};
    for (size_t i = 1; i < a.pos.size(); ++i) {
        const int r = revisionArg(a.pos[i]);
        if (r >= 0 && !(i == 1 && fs::is_directory(a.pos[i]))) revs.push_back(r);
        else b.pos.push_back(a.pos[i]);
    }
    Song song;
    std::string err;
    if (!songFor(b, song, o, false, err)) return o.fail(err);
    std::vector<json> entries;
    if (!history::read(song, entries, err)) return o.fail(err);
    if (revs.size() > 2) return o.fail("usage: wavelength diff [song] [A [B]] (revisions like r12; B defaults to the folder now)");
    const int ra = revs.empty() ? history::current(entries) : revs[0], rb = revs.size() > 1 ? revs[1] : 0;
    if (ra <= 0 && revs.empty()) return o.fail("the song has no revisions to compare with yet (wavelength save)");
    json ja, jb;
    if (!history::jobAt(song, ra, ja, err) || !history::jobAt(song, rb, jb, err)) return o.fail(err);
    SongDiff d;
    if (!diffJobs(ja, jb, song.dir.string(), d, err)) return o.fail(err);
    const std::string from = ra ? "r" + std::to_string(ra) : "now", to = rb ? "r" + std::to_string(rb) : "now";
    if (o.json) { json r = diffToJson(d); r["ok"] = true; r["from"] = from; r["to"] = to; o.emit(r); }
    else std::fprintf(o.f, "%s -> %s\n%s", from.c_str(), to.c_str(), diffToText(d).c_str());
    return 0;
}

int cmdComments(const CliArgs &a, const Out &o) {
    Song song;
    std::string err;
    if (!songFor(a, song, o, false, err)) return o.fail(err);
    json data = review::read(song.dir);
    const std::string id = a.has("--reply") ? a.get("--reply") : a.has("--resolve") ? a.get("--resolve") : a.get("--reopen");
    if (!id.empty()) {   // answer, resolve or reopen one comment
        if (a.has("--reply") && a.get("--text").empty()) return o.fail("--reply needs --text \"...\"");
        bool found = false;
        int rev = 0;
        if (a.has("--reply") || a.has("--resolve")) {   // the revision that answers it: the folder as it is now
            rev = history::save(song, "", actor(), false, err);
            if (!rev) return o.fail(err);
        }
        for (auto &c : data["comments"]) {
            if (c.value("id", std::string()) != id) continue;
            found = true;
            c = review::normalize(c);
            const std::string when = nowRfc3339();
            if (!a.get("--text").empty()) c["replies"].push_back({{"author", actor()}, {"time", when}, {"revision", rev}, {"text", a.get("--text")}});
            if (a.has("--resolve") || a.has("--done")) { c["status"] = "done"; c["resolved"] = {{"revision", rev}, {"time", when}, {"by", actor()}}; }
            if (a.has("--reopen")) { c["status"] = "open"; c.erase("resolved"); }
        }
        if (!found) return o.fail("no comment " + id + " in " + song.title());
        if (!review::write(song.dir, data, err)) return o.fail(err);
        if (o.json) o.emit({{"ok", true}, {"comment", id}, {"revision", rev}});
        else done(o, "comment " + id + " " + (a.has("--reopen") ? "reopened" : a.has("--resolve") || a.has("--done") ? "resolved" : "answered"));
        return 0;
    }
    json list = json::array();
    for (auto &raw : data["comments"]) {
        json c = review::normalize(raw);
        if (!a.has("--all") && c.value("status", std::string("open")) == "done") continue;
        c["now"] = review::status(song, raw);
        list.push_back(c);
    }
    if (o.json) { o.emit({{"ok", true}, {"song", song.title()}, {"comments", list}}); return 0; }
    if (list.empty()) { std::fprintf(o.f, "%s: no %scomments\n", song.title().c_str(), a.has("--all") ? "" : "open "); return 0; }
    for (auto &c : list) {
        const json an = c["anchor"], now = c["now"];
        const std::string rev = an.contains("revision") ? "r" + an["revision"].dump() : "revision unknown";
        const term::Style &st = term::out();
        if (st.on) {   // a card per comment
            const bool done = c.value("status", std::string("open")) == "done";
            std::fprintf(o.f, "%s %s  %s  %s  %s\n", done ? st.ok().c_str() : st.yellow("\u25cf").c_str(), st.bold(an.value("ref", std::string("whole song"))).c_str(),
                         st.dim(rev).c_str(), st.dim(c.value("author", json::object()).value("name", std::string("?")) + ", " + c.value("created", std::string()).substr(0, 16)).c_str(),
                         st.dim(c.value("id", std::string())).c_str());
            std::fprintf(o.f, "  %s\n", c.value("text", std::string()).c_str());
            if (now.value("outdated", false)) for (auto &w : now["why"]) std::fprintf(o.f, "  %s %s\n", st.warn().c_str(), st.yellow("changed since: " + w.get<std::string>()).c_str());
            for (auto &r : c["replies"])
                std::fprintf(o.f, "  %s %s%s\n", st.dim("\u21b3").c_str(), r.value("text", std::string()).c_str(), r.contains("revision") ? st.dim("  r" + r["revision"].dump()).c_str() : "");
            std::fprintf(o.f, "\n");
            continue;
        }
        std::fprintf(o.f, "%s  %s  %s  %s  (%s, %s)\n", c.value("id", std::string()).c_str(), c.value("status", std::string("open")).c_str(), rev.c_str(),
                     an.value("ref", std::string("whole song")).c_str(), c.value("author", json::object()).value("name", std::string("?")).c_str(),
                     c.value("created", std::string()).substr(0, 16).c_str());
        std::fprintf(o.f, "  \"%s\"\n", c.value("text", std::string()).c_str());
        if (!now["since"].empty()) {
            std::string s;
            for (auto &e : now["since"]) s += (s.empty() ? "" : ", ") + std::string("r") + e["rev"].dump() + " " + e.value("op", std::string());
            std::fprintf(o.f, "  since: %s\n", s.c_str());
        }
        if (now.value("outdated", false)) for (auto &w : now["why"]) std::fprintf(o.f, "  changed: %s\n", w.get<std::string>().c_str());
        for (auto &r : c["replies"])
            std::fprintf(o.f, "  reply%s: %s\n", r.contains("revision") ? (" (r" + r["revision"].dump() + ")").c_str() : "", r.value("text", std::string()).c_str());
    }
    if (term::out().on) std::fprintf(o.f, "%s\n", term::out().dim("Answer with: wavelength comments " + (a.pos.size() > 1 ? a.pos[1] : std::string(".")) + " --reply <id> --text \"...\" [--done]").c_str());
    else std::fprintf(o.f, "\nAnswer with: wavelength comments %s --reply <id> --text \"...\" [--done]\n", a.pos.size() > 1 ? a.pos[1].c_str() : ".");
    return 0;
}

int cmdPack(const CliArgs &a, const Out &o) {
    Song song;
    std::string err;
    if (!songFor(a, song, o, false, err)) return o.fail(err);
    package::PackOptions opt;
    opt.history = !a.has("--no-history");
    opt.render = !a.has("--no-render");
    opt.review = !a.has("--no-review");
    json r;
    if (!package::pack(song, a.get("--out"), opt, r, err)) return o.fail(err);
    if (o.json) { o.emit(r); return 0; }
    {
        char size[64];
        std::snprintf(size, sizeof size, " (%.1f MB, %zu entries)", r["bytes"].get<double>() / 1048576.0, r["entries"].get<size_t>());
        done(o, (term::out().on ? term::out().bold(r["file"].get<std::string>()) : r["file"].get<std::string>()) + (term::out().on ? term::out().dim(size) : std::string(size)));
    }
    int missing = 0;
    for (auto &q : r["requires"]) {
        const std::string what = q.contains("plugin") ? q["plugin"].value("name", std::string()) + (q.contains("preset") ? " '" + q.value("preset", std::string()) + "'" : "")
                                                      : q["library"].value("kind", std::string()) + " '" + q["library"].value("name", std::string()) + "'";
        if (!q.value("fallback", false)) ++missing;
        if (term::out().on) {
            const term::Style &st = term::out();
            std::fprintf(o.f, "  %s needs %s %s%s\n", q.value("fallback", false) ? st.ok().c_str() : st.warn().c_str(), st.bold(term::pad(what, 40)).c_str(),
                         st.dim("track " + q.value("track", std::string())).c_str(), q.value("fallback", false) ? st.dim("  (has a fallback)").c_str() : st.yellow("  (no fallback)").c_str());
        } else
        std::fprintf(o.f, "  needs %-40s track %s%s\n", what.c_str(), q.value("track", std::string()).c_str(), q.value("fallback", false) ? "  (has a fallback)" : "");
    }
    if (missing) std::fprintf(o.f, "%d track%s without a fallback will be silent where their plugin or library is missing (wavelength fallbacks --suggest)\n", missing, missing == 1 ? "" : "s");
    return 0;
}

int cmdUnpack(const CliArgs &a, const Out &o) {
    if (a.pos.size() < 2) return o.fail("usage: wavelength unpack <file.wavelength> [--out DIR] [--force]");
    json r;
    std::string err;
    if (!package::unpack(a.pos[1], a.get("--out"), a.has("--force"), r, err)) return o.fail(err);
    if (o.json) o.emit(r);
    else done(o, r.value("title", std::string()) + (term::out().on ? " " + term::out().arrow() + " " : " -> ") + r["folder"].get<std::string>());
    return 0;
}

int cmdValidate(const CliArgs &a, const Out &o) {
    const json r = package::validate(a.pos.size() > 1 ? a.pos[1] : ".");
    if (o.json) { o.emit(r); return r["ok"] ? 0 : 1; }
    const term::Style &st = term::out();
    for (auto &p : r["problems"]) {
        const std::string path = p.value("path", std::string()), msg = p.value("message", std::string());
        if (st.on) std::fprintf(o.f, "%s %s%s\n", p.value("severity", std::string()) == "error" ? st.fail().c_str() : st.warn().c_str(),
                                path.empty() ? "" : (st.bold(path) + st.dim(": ")).c_str(), msg.c_str());
        else std::fprintf(o.f, "%-7s %s%s%s\n", p.value("severity", std::string()).c_str(), path.c_str(), path.empty() ? "" : ": ", msg.c_str());
    }
    if (st.on) std::fprintf(o.f, "%s\n", r["ok"] ? (st.ok() + " " + st.green("valid")).c_str() : (st.fail() + " " + st.red("not valid")).c_str());
    else std::fprintf(o.f, "%s\n", r["ok"] ? "valid" : "not valid");
    return r["ok"] ? 0 : 1;
}

// the job of a song folder, or a job file
bool jobOf(const std::string &arg, fs::path &jobPath, std::string &err) {
    std::error_code ec;
    const fs::path p = fs::absolute(arg.empty() ? "." : arg, ec);
    if (fs::is_regular_file(p, ec) && p.extension() == ".json" && p.filename() != "wavelength.json") { jobPath = p; return true; }
    Song song;
    if (!openSong(p.string(), song, err)) return false;
    jobPath = song.jobPath();
    return true;
}

// the newest report of a full render next to the job (render/ kept with the song, or out/)
json lastReport(const fs::path &dir) {
    std::error_code ec;
    fs::path best;
    for (const char *r : {"render/report.json", "out/report.json"})
        if (fs::exists(dir / r, ec) && (best.empty() || fs::last_write_time(dir / r, ec) > fs::last_write_time(best, ec))) best = dir / r;
    if (best.empty()) return nullptr;
    try { json j = json::parse(readText(best)); if (j.is_object() && j.contains("tracks") && !j.contains("window")) return j; } catch (...) {}
    return nullptr;
}

// each suggested fallback's gain, so its stem is as loud as the track's own in the last render: every
// entry at `index` is rendered in place of its track (those tracks only, one child render)
void measureFallbacks(const fs::path &jobPath, const json &job, json &suggestions, size_t index, const json &report, json &notes) {
    json tmp = job;
    std::vector<std::string> names;
    std::map<std::string, double> orig;
    for (auto &t : report["tracks"]) if (t.contains("lufs") && t["lufs"].is_number()) orig[t.value("name", std::string())] = t["lufs"].get<double>();
    const std::string baseDir = jobPath.parent_path().string();
    for (auto &t : tmp["tracks"]) {
        const std::string name = t.value("name", std::string());
        if (!suggestions.contains(name) || suggestions[name]["fallback"].size() <= index || name.find(',') != std::string::npos || !orig.count(name)) continue;
        const json &f = suggestions[name]["fallback"][index];
        std::string why;
        if (!soundAvailable(f, baseDir, why)) { notes.push_back("track '" + name + "': " + why + " here, so its fallback " + std::to_string(index + 1) + " is not level-matched"); continue; }
        t.erase("fallback");
        useSound(t, f);
        t.erase("gain");   // measured at 0 dB; stems are before the fader anyway
        names.push_back(name);
    }
    if (names.empty()) return;
    std::error_code ec;
    const std::string pid = std::to_string(platform::processId());
    const fs::path tmpJob = jobPath.parent_path() / (".wavelength-fallbacks-" + pid + ".json");
    const fs::path outDir = platform::cacheDir() / "fallbacks" / pid;
    std::string err, list;
    for (auto &n : names) list += (list.empty() ? "" : ",") + n;
    if (!writeText(tmpJob, tmp.dump(), err)) { notes.push_back("cannot measure the fallbacks: " + err); return; }
    platform::Process proc;
    std::string out, crash;
    json rep;
    if (platform::spawn({platform::selfExecutable(), "render", tmpJob.string(), "--tracks", list, "--out", outDir.string(), "--stems", "none", "--deliver", "none", "--json"},
                        proc, true, true)) {
        platform::readOutput(proc, out, 1800);
        while (!platform::finished(proc, crash)) platform::pumpEvents(50);
        try { rep = json::parse(out); } catch (...) {}
    }
    fs::remove(tmpJob, ec);
    fs::remove_all(outDir, ec);
    if (!rep.is_object() || !rep.contains("tracks")) { notes.push_back("cannot measure the fallbacks: " + (rep.is_object() ? rep.value("error", std::string("the render failed")) : std::string("the render failed"))); return; }
    const std::set<std::string> swapped(names.begin(), names.end());   // the render also plays sidechain keys, unswapped
    for (auto &t : rep["tracks"]) {
        const std::string name = t.value("name", std::string());
        if (!swapped.count(name) || !t.contains("lufs") || !t["lufs"].is_number() || t["lufs"].get<double>() < -70 || orig[name] < -70) continue;
        const json *track = nullptr;
        for (auto &x : job["tracks"]) if (x.value("name", std::string()) == name) track = &x;
        const double fader = track && track->contains("gain") && (*track)["gain"].is_number() ? (*track)["gain"].get<double>() : 0.0;
        const double delta = orig[name] - t["lufs"].get<double>();
        json &f = suggestions[name]["fallback"][index];
        f["gain"] = std::round(std::clamp(fader + delta, -40.0, 24.0) * 2) / 2 + 0.0;   // + 0.0: no "-0"
        suggestions[name]["measured"][index] = {{"own", std::round(orig[name] * 10) / 10}, {"fallback", std::round(t["lufs"].get<double>() * 10) / 10}};
    }
}

int cmdFallbacks(const CliArgs &a, const Out &o) {
    fs::path jobPath;
    std::string err;
    if (!jobOf(a.pos.size() > 1 ? a.pos[1] : ".", jobPath, err)) return o.fail(err);
    const std::string jobText = readText(jobPath);
    json job;
    try { job = json::parse(jobText); } catch (const std::exception &e) { return o.fail(jobPath.string() + " is not valid JSON: " + e.what()); }
    if (!job.contains("tracks") || !job["tracks"].is_array()) return o.fail(jobPath.string() + " has no tracks");
    const std::string baseDir = jobPath.parent_path().string();
    if (!a.has("--suggest") && !a.has("--write")) {   // what each track plays here
        json rows = json::array();
        for (auto &t : job["tracks"]) {
            if (!t.is_object()) continue;
            std::string why;
            const bool own = soundAvailable(t, baseDir, why);
            json row = {{"track", t.value("name", std::string())}, {"plugin", t.value("plugin", std::string())}, {"available", own},
                        {"fallbacks", t.contains("fallback") ? (t["fallback"].is_array() ? t["fallback"].size() : 1) : 0}};
            if (!own) {
                row["missing"] = why;
                const json list = !t.contains("fallback") ? json::array() : t["fallback"].is_array() ? t["fallback"] : json::array({t["fallback"]});
                for (size_t i = 0; i < list.size(); ++i) { std::string w; if (soundAvailable(list[i], baseDir, w)) { row["plays"] = i + 1; break; } }
            }
            rows.push_back(row);
        }
        if (o.json) { o.emit({{"ok", true}, {"tracks", rows}}); return 0; }
        int silent = 0, bare = 0;
        for (auto &r : rows) {
            const std::string status = r["available"] ? "plays" : r.contains("plays") ? "fallback " + r["plays"].dump() : "SILENT";
            silent += status == "SILENT";
            bare += r["fallbacks"] == 0 && r["plugin"].get<std::string>().rfind("builtin:", 0) != 0;
            const std::string count = r["fallbacks"] == 0 ? "no fallback" : r["fallbacks"].dump() + " fallback" + (r["fallbacks"] == 1 ? "" : "s");
            const term::Style &st = term::out();
            if (st.on) {
                const std::string mark = status == "plays" ? st.ok() : status == "SILENT" ? st.fail() : st.warn();
                const std::string what = status == "plays" ? "plays" : status == "SILENT" ? st.red("silent here") : st.yellow("plays " + status);
                std::fprintf(o.f, "  %s %s %s %s %s\n", mark.c_str(), st.bold(term::pad(r["track"].get<std::string>(), 24)).c_str(),
                             st.dim(term::pad(r["plugin"].get<std::string>(), 28)).c_str(), term::pad(what, 18).c_str(), st.dim(count).c_str());
            } else
            std::fprintf(o.f, "  %-24s %-28s %-11s %s\n", r["track"].get<std::string>().c_str(), r["plugin"].get<std::string>().c_str(), status.c_str(), count.c_str());
        }
        if (silent) std::fprintf(o.f, "%d track%s can't play here\n", silent, silent == 1 ? "" : "s");
        if (bare) std::fprintf(o.f, "%d track%s with a plugin or library have no fallback (wavelength fallbacks --suggest)\n", bare, bare == 1 ? "" : "s");
        return 0;
    }
    json suggestions = json::object(), notes = json::array();
    size_t longest = 0;
    for (auto &t : job["tracks"]) {
        const json s = suggestFallback(t);
        if (s.is_null()) continue;
        suggestions[t.value("name", std::string())] = s;
        longest = std::max(longest, s["fallback"].size());
    }
    const json report = lastReport(jobPath.parent_path());
    if (a.has("--no-measure")) {}
    else if (report.is_null()) notes.push_back("no render to match levels against: the fallbacks play at the track's fader (render first, then suggest again)");
    else for (size_t i = 0; i < longest; ++i) measureFallbacks(jobPath, job, suggestions, i, report, notes);
    bool wrote = false;
    if (a.has("--write") && !suggestions.empty()) {   // into the job, keeping its key order and indent
        nlohmann::ordered_json oj = nlohmann::ordered_json::parse(jobText);
        for (auto &t : oj["tracks"]) {
            const std::string name = t.value("name", std::string());
            if (!suggestions.contains(name) || t.contains("fallback")) continue;
            nlohmann::ordered_json rebuilt = nlohmann::ordered_json::object();
            const auto fb = nlohmann::ordered_json::parse(suggestions[name]["fallback"].dump());
            for (auto &[k, v] : t.items()) {
                if (k == "notes") rebuilt["fallback"] = fb;
                rebuilt[k] = v;
            }
            if (!rebuilt.contains("fallback")) rebuilt["fallback"] = fb;
            t = rebuilt;
        }
        const size_t nl = jobText.find('\n');
        int indent = -1;
        if (nl != std::string::npos) { indent = 0; while (nl + 1 + indent < jobText.size() && jobText[nl + 1 + indent] == ' ') ++indent; indent = std::max(indent, 1); }
        if (!writeText(jobPath, oj.dump(indent) + "\n", err)) return o.fail(err);
        wrote = true;
        Song song;
        if (openSong(jobPath.parent_path().string(), song, err) && song.hasManifest())
            for (auto &f : song.manifest.value("files", json::array()))
                if (f.value("role", std::string()) == "source" && fs::path(f.value("path", std::string())).extension() != ".json")
                    notes.push_back(f.value("path", std::string()) + " may rebuild " + jobPath.filename().string() + ": give it these fallbacks too, or the next run drops them");
    }
    if (o.json) { o.emit({{"ok", true}, {"job", jobPath.string()}, {"written", wrote}, {"suggestions", suggestions}, {"notes", notes}}); return 0; }
    for (auto &[name, s] : suggestions.items()) {
        if (term::out().on) {
            const term::Style &st = term::out();
            std::fprintf(o.f, "%s %s %s\n", st.bold(term::pad(name, 24)).c_str(), st.cyan(term::pad(s["role"].get<std::string>(), 8)).c_str(), st.dim(s["why"].get<std::string>()).c_str());
            std::fprintf(o.f, "    %s %s\n", st.dim("\"fallback\":").c_str(), s["fallback"].dump().c_str());
        } else {
        std::fprintf(o.f, "%-24s %-7s %s\n", name.c_str(), s["role"].get<std::string>().c_str(), s["why"].get<std::string>().c_str());
        std::fprintf(o.f, "    \"fallback\": %s\n", s["fallback"].dump().c_str());
        }
    }
    const term::Style &st = term::out();
    for (auto &n : notes) std::fprintf(o.f, "%s%s\n", st.on ? (st.warn() + " ").c_str() : "note: ", n.get<std::string>().c_str());
    if (suggestions.empty()) done(o, "every track is built in or already has a fallback");
    else if (wrote) done(o, "wrote " + std::to_string(suggestions.size()) + " fallback" + (suggestions.size() == 1 ? "" : "s") + " into " + jobPath.filename().string());
    else std::fprintf(o.f, "%zu suggestion%s (--write puts them in the job)\n", suggestions.size(), suggestions.size() == 1 ? "" : "s");
    return 0;
}

int cmdMigrate(const CliArgs &a, const Out &o) {
    migrate::Options opt;
    opt.license = a.get("--license");
    opt.author = a.get("--author");
    opt.dryRun = a.has("--dry-run");
    opt.copyOutside = !a.has("--no-copy");
    json r;
    std::string err;
    if (!migrate::run(a.pos.size() > 1 ? a.pos[1] : ".", opt, r, err)) return o.fail(err);
    if (o.json) { o.emit(r); return 0; }
    const term::Style &st = term::out();
    for (auto &c : r["changes"]) std::fprintf(o.f, "  %s%s\n", st.on ? (st.dim("\u00b7") + " ").c_str() : "", c.get<std::string>().c_str());
    for (auto &p : r["problems"]) std::fprintf(o.f, "%s%s\n", st.on ? (st.fail() + " ").c_str() : "problem: ", p.get<std::string>().c_str());
    for (auto &n : r["notes"]) std::fprintf(o.f, "%s%s\n", st.on ? (st.warn() + " ").c_str() : "note: ", n.get<std::string>().c_str());
    if (opt.dryRun) std::fprintf(o.f, "%zu changes (dry run: nothing written)\n", r["changes"].size());
    else if (r.contains("revision")) done(o, std::to_string(r["changes"].size()) + " changes, saved as revision " + std::to_string(r["revision"].get<int>()));
    else done(o, "already up to date");
    return 0;
}

int cmdPurge(const CliArgs &a, const Out &o) {
    purge::Options opt;
    opt.dryRun = a.has("--dry-run");
    std::vector<std::string> folders(a.pos.begin() + 1, a.pos.end());
    json r;
    std::string err;
    if (!purge::run(folders, opt, r, err)) return o.fail(err);
    if (o.json) { o.emit(r); return 0; }
    const term::Style &st = term::out();
    const std::string verb = opt.dryRun ? "would free" : "freed";
    for (auto &e : r["renders"]) {
        std::string what;
        int stems = 0;
        for (auto &f : e["removed"]) {
            const std::string p = f.get<std::string>();
            if (p.rfind("stems/", 0) == 0) ++stems;
            else if (p != "report.json" && p != "song.png") what += (what.empty() ? "" : ", ") + p;
        }
        if (stems) what += (what.empty() ? "" : ", ") + std::to_string(stems) + (stems == 1 ? " stem" : " stems");
        const int previews = e.value("previews", 0);
        if (previews) what += (what.empty() ? "" : ", ") + std::to_string(previews) + (previews == 1 ? " preview" : " previews");
        if (e.value("preview", false)) what = "preview cache";
        if (e.contains("madeMp3")) what += std::string(opt.dryRun ? ", would make " : ", made ") + e["madeMp3"].get<std::string>() +
                                         (e.value("replacedMp3", false) ? " (the one there is from an older render)" : "");
        const std::string size = purge::bytesText(e["bytes"].get<double>());
        std::fprintf(o.f, "  %s  %s  %s\n", (st.on ? st.bold(term::pad(e["dir"].get<std::string>(), 44)) : term::pad(e["dir"].get<std::string>(), 44)).c_str(),
                     term::pad(size, 8).c_str(), (st.on ? st.dim(what) : what).c_str());
        if (e.contains("error")) std::fprintf(o.f, "%s%s\n", st.on ? ("    " + st.warn() + " ").c_str() : "    note: ", e["error"].get<std::string>().c_str());
    }
    for (auto &s : r["skipped"])
        std::fprintf(o.f, "%s%s: %s\n", st.on ? (st.warn() + " ").c_str() : "skipped ", s["dir"].get<std::string>().c_str(), s["why"].get<std::string>().c_str());
    const size_t n = r["renders"].size();
    const int made = r["mp3sMade"].get<int>();
    std::string line = verb + " " + purge::bytesText(r["freedBytes"].get<double>()) + " in " + std::to_string(n) + (n == 1 ? " render" : " renders");
    if (made) line += std::string(opt.dryRun ? " (would make " : " (made ") + std::to_string(made) + (made == 1 ? " MP3" : " MP3s") + " first, so each still plays)";
    if (!n) line = opt.dryRun ? "nothing to purge" : "nothing to purge: no render WAVs here";
    done(o, line);
    const json &other = r["otherWav"];
    if (other["files"].get<size_t>()) {
        std::string where;
        for (auto &f : other["folders"]) where += std::string(where.empty() ? "" : ", ") + f["dir"].get<std::string>() + " " + purge::bytesText(f["bytes"].get<double>());
        const std::string text = "left alone: " + std::to_string(other["files"].get<size_t>()) + " other WAVs (" + purge::bytesText(other["bytes"].get<double>()) +
                                 "): sources, files a song uses, masters, bounces. Biggest: " + where;
        std::fprintf(o.f, "%s\n", (st.on ? st.dim(text) : text).c_str());
    }
    if (opt.dryRun && n) std::fprintf(o.f, "dry run: nothing deleted\n");
    return 0;
}

} // namespace

bool isSongCommand(const std::string &cmd) { return kCommands.count(cmd) > 0; }

int runSongCommand(int argc, char **argv, std::FILE *f) {
    CliArgs a;
    std::string err;
    bool asJson = false;
    for (int i = 2; i < argc; ++i) asJson |= std::string(argv[i]) == "--json";
    const Out o{f, asJson};
    if (!parseArgs(argc, argv, a, err)) return o.fail(err);
    const std::string cmd = a.pos[0];
    if (cmd == "save") return cmdSave(a, o);
    if (cmd == "history") return cmdHistory(a, o);
    if (cmd == "diff") return cmdDiff(a, o);
    if (cmd == "pack") return cmdPack(a, o);
    if (cmd == "unpack") return cmdUnpack(a, o);
    if (cmd == "validate") return cmdValidate(a, o);
    if (cmd == "comments") return cmdComments(a, o);
    if (cmd == "migrate") return cmdMigrate(a, o);
    if (cmd == "fallbacks") return cmdFallbacks(a, o);
    if (cmd == "purge") return cmdPurge(a, o);
    return cmdStep(a, o);
}

} // namespace wl
