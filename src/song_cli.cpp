#include "song_cli.hpp"

#include "history.hpp"
#include "song.hpp"

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
    return cmdStep(a, o);
}

} // namespace wl
