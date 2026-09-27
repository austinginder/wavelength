#include "migrate.hpp"

#include "audition.hpp"
#include "catalog.hpp"
#include "history.hpp"
#include "presets.hpp"
#include "review.hpp"
#include "sampler.hpp"
#include "sha256.hpp"
#include "song.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace wl::migrate {

using json = nlohmann::json;

namespace {

std::string lowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// site.json descriptions are HTML: paragraphs, emphasis and links become Markdown, other tags go
std::string htmlToMarkdown(const std::string &h) {
    std::string out;
    std::string href;
    for (size_t i = 0; i < h.size();) {
        if (h[i] == '<') {
            const size_t end = h.find('>', i);
            if (end == std::string::npos) break;
            std::string tag = h.substr(i + 1, end - i - 1);
            const bool closing = !tag.empty() && tag[0] == '/';
            std::string name = lowerStr(tag.substr(closing ? 1 : 0, tag.find_first_of(" /", closing ? 1 : 0) - (closing ? 1 : 0)));
            if (name == "p" || name == "div" || name == "ul" || name == "ol") { if (closing) out += "\n\n"; }
            else if (name == "br") out += "\n";
            else if (name == "li") out += closing ? "\n" : "- ";
            else if (name == "em" || name == "i") out += "*";
            else if (name == "strong" || name == "b") out += "**";
            else if (name == "a" && !closing) {
                const size_t a = tag.find("href=\"");
                href = a == std::string::npos ? "" : tag.substr(a + 6, tag.find('"', a + 6) - a - 6);
                if (!href.empty()) out += "[";
            } else if (name == "a" && closing && !href.empty()) { out += "](" + href + ")"; href.clear(); }
            i = end + 1;
        } else if (h[i] == '&') {
            static const std::pair<const char *, const char *> ents[] = {
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#039;", "'"}, {"&nbsp;", " "},
                {"&rsquo;", "’"}, {"&lsquo;", "‘"}, {"&ldquo;", "“"}, {"&rdquo;", "”"}, {"&ndash;", "–"},
                {"&mdash;", "—"}, {"&#8217;", "’"}, {"&hellip;", "…"}};
            bool hit = false;
            for (auto &[e, v] : ents)
                if (h.compare(i, std::strlen(e), e) == 0) { out += v; i += std::strlen(e); hit = true; break; }
            if (!hit) out += h[i++];
        } else out += h[i++];
    }
    while (out.find("\n\n\n") != std::string::npos) out.replace(out.find("\n\n\n"), 3, "\n\n");
    const size_t a = out.find_first_not_of(" \n"), b = out.find_last_not_of(" \n");
    return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

// "2026-09-25 22:00:00" (local time) -> RFC 3339
std::string siteDate(const std::string &d) {
    std::tm t{};
    if (std::sscanf(d.c_str(), "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) < 3) return "";
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = -1;
    const std::time_t tt = std::mktime(&t);
    return tt == (std::time_t)-1 ? "" : rfc3339(tt);
}

// the job's own layout when it is written back: key order from the file, its indent
void mergeInto(nlohmann::ordered_json &o, const json &j) {
    if (o.is_object() && j.is_object()) {
        std::vector<std::string> gone;
        for (auto &[k, v] : o.items()) if (!j.contains(k)) gone.push_back(k);
        for (auto &k : gone) o.erase(k);
        for (auto &[k, v] : j.items()) {
            if (o.contains(k)) mergeInto(o[k], v);
            else o[k] = nlohmann::ordered_json::parse(v.dump());
        }
    } else if (o.is_array() && j.is_array() && o.size() == j.size()) {
        for (size_t i = 0; i < j.size(); ++i) mergeInto(o[i], j[i]);
    } else if (json::parse(o.dump()) != j) o = nlohmann::ordered_json::parse(j.dump());
}

int indentOf(const std::string &text) {
    const size_t nl = text.find('\n');
    if (nl == std::string::npos) return -1;
    size_t n = 0;
    while (nl + 1 + n < text.size() && text[nl + 1 + n] == ' ') ++n;
    return (int)std::max<size_t>(n, 1);
}

bool sameFile(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return fs::file_size(a, ec) == fs::file_size(b, ec) && sha256File(a.string()) == sha256File(b.string());
}

struct Upgrader {
    Song &song;
    const Options &opt;
    json changes = json::array(), problems = json::array(), notes = json::array();
    std::map<std::string, std::string> placed;   // source file -> its path in the song
    std::set<std::string> taken;                 // paths handed out in this run (a dry run writes nothing)
    std::map<std::string, std::vector<PresetInfo>> presetCache;

    Upgrader(Song &s, const Options &o) : song(s), opt(o) {}

    // a copy of `src` in the song at `want` (media/...), or a free name next to it; "" on error
    std::string place(const fs::path &src, const std::string &want) {
        std::error_code ec;
        const std::string key = src.lexically_normal().string();
        if (placed.count(key)) return placed[key];
        const fs::path w = fs::u8path(want);
        std::string rel = want;
        for (int n = 2;; ++n) {
            const fs::path dst = song.dir / fs::u8path(rel);
            if (!taken.count(lowerStr(rel)) && (!fs::exists(dst, ec) || (fs::is_regular_file(src, ec) && sameFile(src, dst)))) break;
            rel = (w.parent_path() / (w.stem().u8string() + "-" + std::to_string(n) + w.extension().u8string())).generic_u8string();
        }
        taken.insert(lowerStr(rel));
        if (!opt.dryRun) {
            const fs::path dst = song.dir / fs::u8path(rel);
            fs::create_directories(dst.parent_path(), ec);
            if (fs::is_directory(src, ec)) fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            else if (!fs::exists(dst, ec)) fs::copy_file(src, dst, ec);
            if (ec) { problems.push_back("cannot copy " + src.string() + " to " + rel + ": " + ec.message()); return ""; }
        }
        return placed[key] = rel;
    }

    // a preset name that loads exactly this file for the plugin, or ""
    std::string presetFor(const std::string &plugin, const std::string &file) {
        if (plugin.empty() || plugin.rfind("builtin:", 0) == 0) return "";
        PluginInfo info;
        std::string err;
        if (!resolvePlugin(plugin, info, err, true)) return "";
        if (!presetCache.count(info.id)) presetCache[info.id] = listPresets(info, false, err);
        const auto &list = presetCache[info.id];
        const std::string want = fs::u8path(file).lexically_normal().u8string();
        std::string hash;   // the same bytes under another folder (a preset listed once by name) load the same sound
        const std::string stem = fs::u8path(file.substr(0, file.find('#'))).stem().u8string();
        auto is = [&](const PresetInfo &p) {
            if (p.location == want || p.loadKey == want) return true;
            if (p.name != stem || want.find('#') != std::string::npos || !fs::is_regular_file(fs::u8path(p.location))) return false;
            if (hash.empty()) hash = sha256File(want);
            return sha256File(p.location) == hash;
        };
        for (auto &p : list) {
            if (!is(p)) continue;
            for (const std::string &q : {p.name, p.category.empty() ? std::string() : p.category + "/" + p.name}) {
                PresetInfo hit;
                if (!q.empty() && findPreset(list, q, hit, err) && is(hit)) return q;
            }
        }
        return "";
    }
};

} // namespace

bool run(const std::string &songDir, const Options &opt, json &result, std::string &err) {
    Song song;
    if (!openSong(songDir, song, err)) return false;
    Upgrader u(song, opt);
    std::error_code ec;
    const bool fresh = !song.hasManifest();
    json m = fresh ? newManifest(song.dir) : song.manifest;
    if (fresh) u.changes.push_back("wavelength.json: a new manifest");

    // ---- the manifest: site.json (the website's song page) and the options
    json site;
    if (fs::exists(song.dir / "site.json", ec)) try { site = json::parse(readText(song.dir / "site.json")); } catch (...) {}
    auto set = [&](const char *key, const json &v, bool force = false) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) return;
        if (!force && !fresh && m.contains(key)) return;
        if (m.contains(key) && m[key] == v) return;
        m[key] = v;
        u.changes.push_back(std::string("wavelength.json: ") + key);
    };
    if (site.is_object()) {
        std::string why;
        set("title", site.value("title", std::string()));
        const std::string slug = site.value("slug", std::string());
        if (checkSongPath(slug, why) && slug.find('/') == std::string::npos) set("slug", slug);
        set("created", siteDate(site.value("date", std::string())));
        set("summary", site.value("summary", std::string()));
        set("prompt", site.value("prompt", std::string()));
        set("description", htmlToMarkdown(site.value("description", std::string())));
        if (site.contains("model") && site["model"].is_string() && (fresh || !m.contains("authors")))
            set("authors", json::array({json::object({{"name", site["model"]}, {"role", "composer"}, {"kind", "ai"}})}), true);
        json rest = site;
        for (const char *k : {"title", "slug", "summary", "prompt", "description"}) rest.erase(k);
        if (fresh || !m.contains("metadata") || !m["metadata"].contains("run.wavelength.site")) {
            m["metadata"]["run.wavelength.site"] = rest;
            u.changes.push_back("wavelength.json: metadata[\"run.wavelength.site\"] (the rest of site.json)");
        }
    }
    if (!opt.license.empty()) set("license", opt.license, true);
    if (!opt.author.empty()) {
        json authors = m.value("authors", json::array());
        bool has = false;
        for (auto &a : authors) has |= a.value("name", std::string()) == opt.author;
        if (!has) {
            authors.insert(authors.begin(), json::object({{"name", opt.author}, {"role", "producer"}}));
            set("authors", authors, true);
        }
    }

    // ---- the job: every file it uses is part of the song, or named
    const fs::path jobPath = song.dir / fs::u8path(m.value("job", std::string("job.json")));
    const std::string jobText = readText(jobPath);
    json job;
    try { job = json::parse(jobText); } catch (const std::exception &e) { err = jobPath.string() + " is not valid JSON: " + e.what(); return false; }
    const auto jobTime = fs::last_write_time(jobPath, ec);
    std::set<std::string> used;   // song files the job uses
    bool absolute = false;        // the job named files by absolute path (a generator wrote them)
    const fs::path dir = song.dir.lexically_normal();
    for (auto &r : fileRefs(job)) {
        const fs::path p = fs::u8path(r.path);
        auto rewrite = [&](const std::string &to) {
            u.changes.push_back(r.where + ": " + r.path + " -> " + to);
            *r.slot = to + r.suffix;
        };
        std::string why;
        if (p.is_relative()) {
            if (!checkSongPath(r.path, why)) { u.problems.push_back(r.where + ": " + why); continue; }
            if (fs::exists(song.dir / p, ec)) {
                if (r.path.rfind("out/", 0) == 0) {   // scratch: a copy in media/
                    const std::string rel = u.place(song.dir / p, "media/" + r.path.substr(4));
                    if (!rel.empty()) { rewrite(rel); used.insert(rel); }
                } else used.insert(r.path);
                continue;
            }
            const std::string lib = r.state ? "" : libraryRef(resolveSampleFile(r.path, ""));   // a path under a sample root
            if (!lib.empty()) { rewrite(lib); continue; }
            u.problems.push_back(r.where + ": " + r.path + " is missing");
            continue;
        }
        const fs::path abs = p.lexically_normal();
        absolute = true;
        if (!fs::exists(abs, ec)) { u.problems.push_back(r.where + ": " + r.path + " is missing"); continue; }
        const fs::path rel = abs.lexically_relative(dir);
        if (!rel.empty() && *rel.begin() != "..") {   // inside the song
            std::string to = rel.generic_u8string();
            if (to.rfind("out/", 0) == 0) to = u.place(abs, "media/" + to.substr(4));
            if (!to.empty()) { rewrite(to); used.insert(to); }
            continue;
        }
        if (!r.state) {   // a sample library's file: by name
            const std::string lib = libraryRef(abs.string());
            if (!lib.empty()) { rewrite(lib); continue; }
        } else {          // a preset file: by the plugin's preset name
            const std::string name = u.presetFor(r.owner->value("plugin", std::string()), abs.string() + r.suffix);
            if (!name.empty()) {
                u.changes.push_back(r.where + ": " + r.path + r.suffix + " -> preset \"" + name + "\"");
                r.owner->erase("state");
                (*r.owner)["preset"] = name;
                continue;
            }
        }
        if (!opt.copyOutside) { u.problems.push_back(r.where + ": uses " + r.path + " from outside the song (move it into media/ or name it)"); continue; }
        const std::string to = u.place(abs, "media/" + abs.filename().u8string());
        if (to.empty()) continue;
        rewrite(to);
        used.insert(to);
        u.notes.push_back("copied " + r.path + " into " + to + ": check that its licence lets you share it before you share the song");
    }
    // the files it uses are listed, as media
    std::set<std::string> listed;
    for (auto &f : m.value("files", json::array())) listed.insert(f.value("path", std::string()));
    json files = m.value("files", json::array());
    auto list = [&](const std::string &rel, const std::string &role) {
        if (!listed.insert(rel).second) return;
        files.push_back({{"path", rel}, {"role", role}});
        u.changes.push_back("wavelength.json: lists " + rel + " (" + role + ")");
    };
    for (auto &rel : used) {
        const fs::path full = song.dir / fs::u8path(rel);
        if (fs::is_directory(full, ec)) {
            for (auto it = fs::recursive_directory_iterator(full, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) list(it->path().lexically_relative(song.dir).generic_u8string(), "media");
        } else list(rel, "media");
    }
    const bool jobChanged = job != json::parse(jobText);

    // ---- review.json in the current shape
    if (fs::exists(song.dir / "review.json", ec)) {
        const json data = review::read(song.dir);
        json out = data;
        out["comments"] = json::array();
        for (auto &c : data["comments"]) out["comments"].push_back(review::normalize(c));
        if (out != data) {
            u.changes.push_back("review.json: " + std::to_string(out["comments"].size()) + " comments in the current shape (anchors, replies)");
            std::string werr;
            if (!opt.dryRun && !review::write(song.dir, out, werr)) u.problems.push_back("review.json: " + werr);
        }
    }

    // ---- the last render, when it is of this job
    const fs::path report = song.dir / "out" / "report.json";
    if (!m.contains("render") && fs::exists(report, ec) && fs::last_write_time(report, ec) >= jobTime) {
        fs::path mp3;
        for (auto &e : fs::directory_iterator(song.dir / "out", ec))
            if (lowerStr(e.path().extension().string()) == ".mp3" && e.last_write_time(ec) >= jobTime &&
                (mp3.empty() || e.last_write_time(ec) > fs::last_write_time(mp3, ec)))
                mp3 = e.path();
        if (mp3.empty()) u.notes.push_back("the last render has no MP3: `wavelength render job.json --keep` keeps one with the song");
        else {
            json r = {{"mix", "render/mix.mp3"}, {"report", "render/report.json"}};
            const fs::path png = song.dir / "out" / "song.png";
            const bool hasPng = fs::exists(png, ec) && fs::last_write_time(png, ec) >= jobTime;
            if (hasPng) r["picture"] = "render/song.png";
            std::string newJob = jobText;
            if (jobChanged) {
                nlohmann::ordered_json o = nlohmann::ordered_json::parse(jobText);
                mergeInto(o, job);
                newJob = o.dump(indentOf(jobText)) + "\n";
            }
            r["job"] = "sha256:" + sha256Hex(newJob);
            if (!opt.dryRun) {
                std::string werr;
                fs::create_directories(song.dir / "render", ec);
                fs::copy_file(mp3, song.dir / "render/mix.mp3", fs::copy_options::overwrite_existing, ec);
                if (hasPng) fs::copy_file(png, song.dir / "render/song.png", fs::copy_options::overwrite_existing, ec);
                json rep;
                try { rep = json::parse(readText(report)); } catch (...) {}
                if (rep.is_object()) rep["song"] = {{"job", r["job"]}};
                if (!writeText(song.dir / "render/report.json", rep.dump(2) + "\n", werr)) u.problems.push_back(werr);
            }
            m["render"] = r;
            for (const char *f : {"render/mix.mp3", "render/song.png", "render/report.json"})
                if (std::string(f) != "render/song.png" || hasPng) list(f, "render");
            u.changes.push_back("render/: the last render (" + mp3.filename().u8string() + ") kept with the song");
        }
    }
    m["files"] = files;

    // ---- what stays for the author
    if (absolute)
        for (auto &f : m["files"])
            if (f.value("role", std::string()) == "source" && lowerStr(fs::u8path(f.value("path", std::string())).extension().string()) != ".json")
                u.notes.push_back("the job named files by absolute path, and " + f.value("path", std::string()) + " may be what wrote them: running "
                                  "it again brings them back unless it writes paths relative to the song folder (media/...)");
    int noFallback = 0;
    for (auto &t : job.value("tracks", json::array()))
        if (t.is_object() && t.value("plugin", std::string()).rfind("builtin:", 0) != 0 && !t.contains("fallback")) ++noFallback;
    if (noFallback)
        u.notes.push_back(std::to_string(noFallback) + " tracks play a plugin with no fallback: they are silent where it isn't installed "
                          "(wavelength fallbacks --suggest)");
    if (!m.contains("license")) u.notes.push_back("no licence: nobody may reuse the song (--license CC-BY-4.0, for example)");

    // ---- write, then a revision
    int rev = 0;
    if (!opt.dryRun) {
        if (jobChanged) {
            nlohmann::ordered_json o = nlohmann::ordered_json::parse(jobText);
            mergeInto(o, job);
            if (!writeText(jobPath, o.dump(indentOf(jobText)) + "\n", err)) return false;
        }
        if (m != song.manifest) {
            m["updated"] = nowRfc3339();
            song.manifest = m;
            if (!writeManifest(song, err)) return false;
        }
        std::vector<json> entries;
        if (!u.changes.empty() || (history::read(song, entries, err) && entries.empty())) {
            rev = history::save(song, "migrated to the song format", actor(), false, err);
            if (!rev) return false;
        }
    }
    result = {{"ok", u.problems.empty()}, {"song", song.dir.string()}, {"dryRun", opt.dryRun}, {"changes", u.changes},
              {"problems", u.problems}, {"notes", u.notes}};
    if (rev > 0) result["revision"] = rev;
    return true;
}

} // namespace wl::migrate
