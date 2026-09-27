#include "song_cli.hpp"

#include "history.hpp"
#include "package.hpp"
#include "review.hpp"
#include "song.hpp"
#include "songdiff.hpp"

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
    {"comments", {{"--json", false}, {"--all", false}, {"--reply", true}, {"--text", true}, {"--done", false}, {"--resolve", true}, {"--reopen", true}}},
};

struct Out {
    std::FILE *f;
    bool json;
    int fail(const std::string &err) const {
        if (json) { std::fputs((nlohmann::json{{"ok", false}, {"error", err}}.dump(2) + "\n").c_str(), f); }
        else std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    void emit(const nlohmann::json &j) const { std::fputs((j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n").c_str(), f); }
};

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
        if (!create) { err = song.dir.filename().string() + " has no wavelength.json yet: `wavelength save` or `wavelength upgrade` makes one"; return false; }
        song.manifest = newManifest(song.dir);
        song.manifest["generator"] = {{"name", "wavelength"}, {"version", WAVELENGTH_VERSION}};
        if (!writeManifest(song, err)) return false;
        if (!o.json) std::fprintf(o.f, "wrote %s/wavelength.json\n", song.dir.filename().string().c_str());
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
        else std::fprintf(o.f, "%s %s\n", bundle ? "bundle" : "repository", target.c_str());
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
        else std::fprintf(o.f, "restored revision %ld (now revision %d)\n", target, rev);
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
    else std::fprintf(o.f, "%s: the song is back at revision %d (now revision %d)\n", cmd.c_str(), target, rev);
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
        else std::fprintf(o.f, "comment %s %s\n", id.c_str(), a.has("--reopen") ? "reopened" : a.has("--resolve") || a.has("--done") ? "resolved" : "answered");
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
    std::fprintf(o.f, "\nAnswer with: wavelength comments %s --reply <id> --text \"...\" [--done]\n", a.pos.size() > 1 ? a.pos[1].c_str() : ".");
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
    std::fprintf(o.f, "%s (%.1f MB, %zu entries)\n", r["file"].get<std::string>().c_str(), r["bytes"].get<double>() / 1048576.0, r["entries"].get<size_t>());
    int missing = 0;
    for (auto &q : r["requires"]) {
        const std::string what = q.contains("plugin") ? q["plugin"].value("name", std::string()) + (q.contains("preset") ? " '" + q.value("preset", std::string()) + "'" : "")
                                                      : q["library"].value("kind", std::string()) + " '" + q["library"].value("name", std::string()) + "'";
        if (!q.value("fallback", false)) ++missing;
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
    else std::fprintf(o.f, "%s -> %s\n", r.value("title", std::string()).c_str(), r["folder"].get<std::string>().c_str());
    return 0;
}

int cmdValidate(const CliArgs &a, const Out &o) {
    const json r = package::validate(a.pos.size() > 1 ? a.pos[1] : ".");
    if (o.json) { o.emit(r); return r["ok"] ? 0 : 1; }
    for (auto &p : r["problems"])
        std::fprintf(o.f, "%-7s %s%s%s\n", p.value("severity", std::string()).c_str(), p.value("path", std::string()).c_str(), p.value("path", std::string()).empty() ? "" : ": ",
                     p.value("message", std::string()).c_str());
    std::fprintf(o.f, "%s\n", r["ok"] ? "valid" : "not valid");
    return r["ok"] ? 0 : 1;
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
    return cmdStep(a, o);
}

} // namespace wl
