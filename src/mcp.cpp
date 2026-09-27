#include "mcp.hpp"

#include "docs.hpp"
#include "platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef WAVELENGTH_VERSION
#define WAVELENGTH_VERSION "dev"
#endif

namespace fs = std::filesystem;

namespace wl {

namespace {

using json = nlohmann::json;

// protocol versions this server speaks; it answers with the client's when it knows it, else the newest
const char *kVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

const char *kInstructions =
    "Wavelength renders music offline through the synthesizers and sample libraries installed on this computer "
    "(and its own built-in synth, drums and effects): you write a JSON job (notes, sounds, effects, mix) and get back "
    "stems, a mix, a report with loudness per track and section, and a picture of the song. You can't hear the result: "
    "read the report and look at the picture, and ask the human to listen. Before writing a first job, read the guide "
    "(the `guide` tool, doc \"agents\", in full); `guide` with doc \"job-format\" or \"effects\" is the reference. "
    "Never guess instrument or preset names: list them with list_instruments and list_presets.";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string base64(const std::string &in) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint32_t)(uint8_t)in[i] << 16 | (uint32_t)(uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (uint32_t)(uint8_t)in[i] << 16 | (uint32_t)(uint8_t)in[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string fmt(const char *f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

// ---- tools ------------------------------------------------------------------------------------
json prop(const char *type, const char *desc) { return {{"type", type}, {"description", desc}}; }

json tools() {
    const json readOnly = {{"readOnlyHint", true}, {"openWorldHint", false}};
    const json writes = {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}};
    auto tool = [](const char *name, const char *title, const char *desc, json props, std::vector<std::string> required, json ann) {
        json schema = {{"type", "object"}, {"properties", props}};
        if (!required.empty()) schema["required"] = required;
        return json{{"name", name}, {"title", title}, {"description", desc}, {"inputSchema", schema}, {"annotations", ann}};
    };
    json list = json::array();
    list.push_back(tool("guide", "Read the Wavelength guide",
        "Wavelength's own documentation, matching this engine. doc \"agents\" is the operating guide: the render loop, choosing "
        "sounds, arranging, mixing, mastering, reading the report and the picture. Read it in full before writing a first job. "
        "\"job-format\" is the job JSON reference, \"effects\" the effects, buses and automation reference. Pass section (part of a "
        "heading) to read one part again.",
        {{"doc", {{"type", "string"}, {"enum", {"agents", "job-format", "effects"}}, {"description", "which document (default agents)"}}},
         {"section", prop("string", "part of a heading, e.g. \"Mixing\"; omit for the whole document")}},
        {}, readOnly));
    list.push_back(tool("list_instruments", "List instruments and effects",
        "The plugins installed on this computer (CLAP, VST3, VST2) and Wavelength's built-in instruments (builtin:synth needs no "
        "plugin). Use the names or ids it gives as a track's \"plugin\" or an effect's plugin; never guess them.",
        {{"search", prop("string", "only entries whose name, vendor or id contains this")},
         {"kind", {{"type", "string"}, {"enum", {"instrument", "effect", "all"}}, {"description", "default all"}}}},
        {}, readOnly));
    list.push_back(tool("list_presets", "List a plugin's presets",
        "Factory presets and preset files an instrument or effect can load by name (\"preset\" on a track). After `wavelength audition` "
        "has run for the plugin, tags such as dark, pluck, \"octave -1\" come with them and search matches them too.",
        {{"plugin", prop("string", "the plugin's name or id, or builtin:synth")}, {"search", prop("string", "only presets matching this")}},
        {"plugin"}, readOnly));
    list.push_back(tool("list_samples", "List sample libraries",
        "Sample libraries builtin:sampler plays: Bitwig multisamples, drum kits, SFZ instruments, SoundFonts and loops.",
        {{"search", prop("string", "only libraries matching this")}}, {}, readOnly));
    list.push_back(tool("plugin_params", "List a plugin's parameters",
        "A plugin's parameters with their ranges and display text (for \"params\" and automation.params), optionally after loading "
        "a preset. builtin:synth lists its own parameter names.",
        {{"plugin", prop("string", "the plugin's name or id")}, {"preset", prop("string", "load this preset first")},
         {"search", prop("string", "only parameters whose name contains this")}},
        {"plugin"}, readOnly));
    list.push_back(tool("lint", "Check a job's harmony",
        "Before rendering: wrong notes, out-of-key chords, clashes between parts, parallel fifths and octaves, from the job's notes.",
        {{"job", prop("string", "path to job.json")}}, {"job"}, readOnly));
    list.push_back(tool("render", "Render a song",
        "Renders a job through its instruments and effects and returns the report's summary (loudness of the mix, each section "
        "and track, warnings with their fixes) and the picture of the song (sections, loudness contour, spectrum, every track's "
        "notes over its level). Writes mix.wav, report.json and song.png into out. from_bar/to_bar render a few bars in seconds.",
        {{"job", prop("string", "path to job.json")}, {"out", prop("string", "output folder (default: out next to the job)")},
         {"from_bar", prop("number", "render from this bar (with to_bar)")}, {"to_bar", prop("number", "render up to this bar (exclusive)")},
         {"tracks", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "only these tracks (with the song's buses and master)"}}},
         {"stems", {{"type", "string"}, {"enum", {"none", "16", "24", "float"}}, {"description", "stem files (default none)"}}},
         {"deliver", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "extra files next to mix.wav: mp3, flac, wav:16..."}}},
         {"loop", prop("boolean", "with from_bar/to_bar: a seamless loop for games (tail folded onto the start, smpl loop chunk)")},
         {"picture", prop("boolean", "attach the song picture (default true)")}},
        {"job"}, writes));
    list.push_back(tool("picture", "Picture of an arrangement",
        "Draws a job's arrangement before rendering (sections, bars, every track's notes) and returns the image; the render tool "
        "draws the full picture with levels and spectrum.",
        {{"job", prop("string", "path to job.json")}, {"width", prop("number", "pixels, 800-3200 (default 1400)")}}, {"job"}, writes));
    list.push_back(tool("analyze", "Measure audio",
        "Measures a WAV, or a render folder (its mix, every stem and section): pitch, brightness, band balance, stereo width, "
        "onsets and envelope, and with peaks the strongest spectral peaks. For what the report doesn't say: an octave, a harsh band.",
        {{"path", prop("string", "a WAV file or a render folder")}, {"start", prop("number", "seconds")}, {"end", prop("number", "seconds")},
         {"song_time", prop("boolean", "start/end are song time (a render's lead-in added)")}, {"peaks", prop("boolean", "list spectral peaks")}},
        {"path"}, readOnly));
    list.push_back(tool("timeline", "Song time of bars and markers",
        "Song time of every marker and every few bars from the tempo map, in song and file seconds: find bar 57, or plan a length.",
        {{"job", prop("string", "path to job.json")}, {"every", prop("number", "bars between rows (default 8)")}}, {"job"}, readOnly));
    list.push_back(tool("import", "Import MIDI, MusicXML, DAWproject or a Bitwig project",
        "Makes a job from a MIDI file, a MusicXML score (MuseScore, Sibelius, Dorico), a DAWproject or a Bitwig Studio project (.bwproject, no export needed), ready to render and edit.",
        {{"file", prop("string", ".mid, .musicxml/.mxl, .dawproject or .bwproject")}, {"out", prop("string", "folder for job.json")}}, {"file", "out"}, writes));
    return list;
}

// ---- the server -------------------------------------------------------------------------------
class Server {
public:
    explicit Server(std::FILE *out) : out_(out) {}

    int run() {
        std::thread reader([this] {
            for (std::string line; std::getline(std::cin, line);) {
                std::lock_guard<std::mutex> g(m_);
                lines_.push_back(line);
                cv_.notify_one();
            }
            std::lock_guard<std::mutex> g(m_);
            eof_ = true;
            cv_.notify_one();
        });
        reader.detach();
        for (;;) {
            json msg;
            if (!deferred_.empty()) { msg = deferred_.front(); deferred_.pop_front(); }
            else if (!next(msg, -1)) break;
            handle(msg);
        }
        return 0;
    }

private:
    std::FILE *out_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    bool eof_ = false;
    std::deque<json> deferred_;   // requests that came in while a tool ran

    // the next parsed message; false at the end of input (or after timeoutMs with nothing, when >= 0)
    bool next(json &msg, int timeoutMs) {
        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> l(m_);
                if (timeoutMs < 0) cv_.wait(l, [this] { return !lines_.empty() || eof_; });
                else if (!cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [this] { return !lines_.empty() || eof_; })) return false;
                if (lines_.empty()) return false;
                line = std::move(lines_.front());
                lines_.pop_front();
            }
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            try { msg = json::parse(line); return true; }
            catch (const std::exception &) { send({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "parse error"}}}}); }
        }
    }

    void send(const json &j) {
        const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
        std::fputs(s.c_str(), out_);
        std::fputc('\n', out_);
        std::fflush(out_);
    }
    void reply(const json &id, const json &result) { send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); }
    void fault(const json &id, int code, const std::string &msg) { send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", msg}}}}); }

    void handle(const json &msg) {
        if (!msg.is_object()) return;
        const std::string method = msg.value("method", "");
        const bool request = msg.contains("id");
        const json id = request ? msg["id"] : json();
        const json params = msg.value("params", json::object());
        if (method == "initialize") {
            std::string v = params.value("protocolVersion", kVersions[0]);
            if (std::find_if(std::begin(kVersions), std::end(kVersions), [&](const char *k) { return v == k; }) == std::end(kVersions)) v = kVersions[0];
            reply(id, {{"protocolVersion", v},
                       {"capabilities", {{"tools", {{"listChanged", false}}}}},
                       {"serverInfo", {{"name", "wavelength"}, {"title", "Wavelength"}, {"version", WAVELENGTH_VERSION}}},
                       {"instructions", kInstructions}});
        } else if (method == "ping") {
            if (request) reply(id, json::object());
        } else if (method == "tools/list") {
            reply(id, {{"tools", tools()}});
        } else if (method == "tools/call") {
            call(id, params);
        } else if (request) {
            if (method.rfind("notifications/", 0) != 0) fault(id, -32601, "method not found: " + method);
        }
        // notifications (initialized, cancelled for a request no longer running) need no answer
    }

    struct Result { json content = json::array(); bool error = false; };
    static Result text(const std::string &s, bool error = false) {
        Result r;
        r.content.push_back({{"type", "text"}, {"text", s}});
        r.error = error;
        return r;
    }

    // runs `wavelength <args>` and returns its JSON; false (with err) when it could not run, crashed or was cancelled
    bool runSelf(const std::vector<std::string> &args, const json &id, const json &progress, int timeoutSec, json &out, std::string &err) {
        std::vector<std::string> cmd = {platform::selfExecutable()};
        cmd.insert(cmd.end(), args.begin(), args.end());
        platform::Process p;
        if (!platform::spawn(cmd, p, true, true)) { err = "could not start " + cmd[0]; return false; }
        std::string text;
        std::atomic<bool> done{false};
        bool read = false;
        std::thread t([&] { read = platform::readOutput(p, text, timeoutSec); done = true; });
        auto last = std::chrono::steady_clock::now();
        bool cancelled = false;
        double tick = 0;
        while (!done) {
            json msg;
            if (next(msg, 200)) {   // while the tool runs: answer pings, stop on cancel, keep the rest for later
                const std::string m = msg.value("method", "");
                if (m == "ping" && msg.contains("id")) reply(msg["id"], json::object());
                else if (m == "notifications/cancelled" && msg.contains("params") && msg["params"].value("requestId", json()) == id) {
                    cancelled = true;
                    platform::kill(p);
                } else deferred_.push_back(msg);
            }
            if (!progress.is_null() && std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {   // keep the client's timeout alive
                last = std::chrono::steady_clock::now();
                tick += 1;
                send({{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
                      {"params", {{"progressToken", progress}, {"progress", tick}, {"message", "working (" + fmt("%.0f", tick * 5) + " s)"}}}});
            }
        }
        t.join();
        std::string crash;
        for (int i = 0; i < 200 && !platform::finished(p, crash); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!read && !cancelled) platform::kill(p);
        if (cancelled) { err = "cancelled"; return false; }
        if (!read) { err = "timed out after " + std::to_string(timeoutSec) + " s"; return false; }
        try { out = json::parse(text); }
        catch (const std::exception &) {
            err = crash.empty() ? "no JSON from `wavelength " + (args.empty() ? std::string() : args[0]) + "`" : "wavelength " + args[0] + " crashed: " + crash;
            return false;
        }
        return true;
    }

    void call(const json &id, const json &params) {
        const std::string name = params.value("name", "");
        const json a = params.value("arguments", json::object());
        const json progress = params.contains("_meta") && params["_meta"].contains("progressToken") ? params["_meta"]["progressToken"] : json();
        bool known = false;
        for (auto &t : tools()) known |= t["name"] == name;
        if (!known) { fault(id, -32602, "unknown tool: " + name); return; }
        Result r;
        try { r = tool(name, a, id, progress); }
        catch (const std::exception &e) { r = text(std::string("error: ") + e.what(), true); }
        if (r.error && r.content.empty()) r = text("error", true);
        if (cancelledNow_) { cancelledNow_ = false; return; }   // a cancelled request gets no answer
        reply(id, {{"content", r.content}, {"isError", r.error}});
    }
    bool cancelledNow_ = false;

    // runs a command for a tool; on failure, the tool's error result
    bool run(const std::vector<std::string> &args, const json &id, const json &progress, int timeoutSec, json &out, Result &fail) {
        std::string err;
        if (!runSelf(args, id, progress, timeoutSec, out, err)) {
            if (err == "cancelled") cancelledNow_ = true;
            fail = text("error: " + err, true);
            return false;
        }
        if (out.is_object() && out.contains("ok") && out["ok"] == false && out.contains("error") && !out.contains("mix")) {
            fail = text("error: " + out["error"].get<std::string>(), true);
            return false;
        }
        return true;
    }

    static std::string str(const json &a, const char *k) { return a.contains(k) && a[k].is_string() ? a[k].get<std::string>() : std::string(); }
    static std::string need(const json &a, const char *k) {
        const std::string v = str(a, k);
        if (v.empty()) throw std::runtime_error(std::string("\"") + k + "\" is required");
        return v;
    }
    static std::string numArg(const json &v) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", v.get<double>());
        return b;
    }
    static json image(const std::string &path) {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return {{"type", "image"}, {"data", base64(ss.str())}, {"mimeType", "image/png"}};
    }

    Result tool(const std::string &name, const json &a, const json &id, const json &progress) {
        Result fail;
        json out;
        if (name == "guide") {
            std::string doc = str(a, "doc");
            if (doc.empty()) doc = "agents";
            std::string t, err;
            if (!docText(doc, str(a, "section"), t, err)) return text("error: " + err, true);
            return text(t);
        }
        if (name == "list_instruments") {
            if (!run({"plugins", "--json"}, id, progress, 900, out, fail)) return fail;
            const std::string q = lower(str(a, "search")), kind = str(a, "kind");
            std::string lines;
            size_t shown = 0, total = 0;
            for (auto &p : out.value("plugins", json::array())) {
                const auto feats = p.value("features", json::array());
                const bool inst = std::find(feats.begin(), feats.end(), "instrument") != feats.end();
                if (p.value("blocked", false)) continue;
                if ((kind == "instrument" && !inst) || (kind == "effect" && inst)) continue;
                const std::string hay = lower(p.value("name", "") + " " + p.value("vendor", "") + " " + p.value("id", ""));
                if (!q.empty() && hay.find(q) == std::string::npos) continue;
                ++total;
                if (shown >= 300) continue;
                ++shown;
                lines += p.value("name", "") + "  [" + p.value("id", "") + "]  " + p.value("format", "") + ", " + p.value("vendor", "") +
                         (inst ? ", instrument" : ", effect") + "\n";
            }
            if (total > shown) lines += "... " + std::to_string(total - shown) + " more: narrow with search\n";
            return text(lines.empty() ? "nothing matches" : lines + "\nUse the name (or the id in brackets) as \"plugin\"; prefix vst3: or clap: when a name exists in both formats.");
        }
        if (name == "list_presets") {
            std::vector<std::string> args = {"presets", need(a, "plugin"), "--json"};
            if (!str(a, "search").empty()) { args.push_back("--search"); args.push_back(str(a, "search")); }
            if (!run(args, id, progress, 900, out, fail)) return fail;
            std::string lines;
            size_t shown = 0;
            const auto list = out.value("presets", json::array());
            for (auto &p : list) {
                if (shown++ >= 400) break;
                std::string tags;
                if (p.contains("audition") && p["audition"].contains("tags"))
                    for (auto &t : p["audition"]["tags"]) tags += (tags.empty() ? "" : ", ") + t.get<std::string>();
                lines += (p.value("category", "").empty() ? "" : p.value("category", "") + " / ") + p.value("name", "") +
                         (p.value("description", "").empty() ? "" : "  (" + p.value("description", "").substr(0, 100) + ")") + (tags.empty() ? "" : "  [" + tags + "]") + "\n";
            }
            if (list.size() > 400) lines += "... " + std::to_string(list.size() - 400) + " more: narrow with search\n";
            return text(lines.empty() ? "no presets match" : lines + "\nUse the name as \"preset\".");
        }
        if (name == "list_samples") {
            std::vector<std::string> args = {"samples", "--json"};
            if (!str(a, "search").empty()) { args.push_back("--search"); args.push_back(str(a, "search")); }
            if (!run(args, id, progress, 600, out, fail)) return fail;
            std::string lines;
            size_t shown = 0;
            const auto list = out.value("samples", json::array());
            for (auto &s : list) {
                if (shown++ >= 300) break;
                lines += s.value("kind", "") + ": " + s.value("name", "") + (s.value("category", "").empty() ? "" : "  (" + s.value("category", "") + ")") + "\n";
            }
            if (list.size() > 300) lines += "... " + std::to_string(list.size() - 300) + " more: narrow with search\n";
            return text(lines.empty() ? "no sample libraries match (samples --install-soundfont adds General MIDI sounds)"
                                      : lines + "\nUse with \"plugin\": \"builtin:sampler\" and \"sampler\": {\"multisample\"|\"kit\"|\"sfz\"|\"soundfont\": \"<name>\"}.");
        }
        if (name == "plugin_params") {
            std::vector<std::string> args = {"params", need(a, "plugin"), "--json"};
            if (!str(a, "preset").empty()) { args.push_back("--preset"); args.push_back(str(a, "preset")); }
            if (!run(args, id, progress, 600, out, fail)) return fail;
            const std::string q = lower(str(a, "search"));
            std::string lines;
            size_t shown = 0, total = 0;
            for (auto &p : out.value("params", json::array())) {
                if (!q.empty() && lower(p.value("name", "")).find(q) == std::string::npos) continue;
                ++total;
                if (shown >= 400) continue;
                ++shown;
                std::string range = p.contains("min") && p.contains("max") ? " " + numArg(p["min"]) + ".." + numArg(p["max"]) : "";
                std::string now = p.contains("display") ? "  now \"" + p.value("display", "") + "\"" : p.contains("default") ? "  default " + numArg(p["default"]) : "";
                const std::string module = p.value("module", "");
                lines += (module.empty() ? "" : module + "/") + p.value("name", "") + range + (p.value("unit", "").empty() ? "" : " " + p.value("unit", "")) + now +
                         (p.value("description", "").empty() ? "" : "  " + p.value("description", "")) + "\n";
            }
            if (total > shown) lines += "... " + std::to_string(total - shown) + " more: narrow with search\n";
            return text(lines.empty() ? "no parameters match" : lines);
        }
        if (name == "lint") {
            if (!run({"lint", need(a, "job"), "--harmony", "--json"}, id, progress, 300, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "timeline") {
            std::vector<std::string> args = {"timeline", need(a, "job"), "--json"};
            if (a.contains("every") && a["every"].is_number()) { args.push_back("--every"); args.push_back(numArg(a["every"])); }
            if (!run(args, id, progress, 120, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "analyze") {
            std::vector<std::string> args = {"analyze", need(a, "path"), "--json"};
            for (const char *k : {"start", "end"})
                if (a.contains(k) && a[k].is_number()) { args.push_back(std::string("--") + k); args.push_back(numArg(a[k])); }
            if (a.value("song_time", false)) args.push_back("--song-time");
            if (a.value("peaks", false)) args.push_back("--peaks");
            if (!run(args, id, progress, 600, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "import") {
            if (!run({"import", need(a, "file"), "--out", need(a, "out"), "--json"}, id, progress, 600, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "picture") {
            std::vector<std::string> args = {"picture", need(a, "job"), "--json"};
            if (a.contains("width") && a["width"].is_number()) { args.push_back("--width"); args.push_back(numArg(a["width"])); }
            if (!run(args, id, progress, 120, out, fail)) return fail;
            Result r = text("arrangement: " + out.value("file", ""));
            r.content.push_back(image(out.value("file", "")));
            return r;
        }
        if (name == "render") return render(a, id, progress);
        return text("error: unknown tool " + name, true);
    }

    Result render(const json &a, const json &id, const json &progress) {
        const std::string job = need(a, "job");
        std::string outDir = str(a, "out");
        if (outDir.empty()) outDir = (fs::path(job).parent_path() / "out").string();
        std::vector<std::string> args = {"render", job, "--out", outDir, "--json", "--stems", str(a, "stems").empty() ? "none" : str(a, "stems")};
        const bool picture = a.value("picture", true);
        if (picture) args.push_back("--png");
        if (a.contains("from_bar") && a["from_bar"].is_number()) { args.push_back("--from"); args.push_back(numArg(a["from_bar"])); }
        if (a.contains("to_bar") && a["to_bar"].is_number()) { args.push_back("--to"); args.push_back(numArg(a["to_bar"])); }
        if (a.value("loop", false)) args.push_back("--loop");
        if (a.contains("tracks") && a["tracks"].is_array()) {
            std::string t;
            for (auto &x : a["tracks"]) if (x.is_string()) t += (t.empty() ? "" : ",") + x.get<std::string>();
            if (!t.empty()) { args.push_back("--tracks"); args.push_back(t); }
        }
        if (a.contains("deliver") && a["deliver"].is_array()) {
            std::string d;
            for (auto &x : a["deliver"]) if (x.is_string()) d += (d.empty() ? "" : ",") + x.get<std::string>();
            if (!d.empty()) { args.push_back("--deliver"); args.push_back(d); }
        }
        json r;
        Result fail;
        if (!run(args, id, progress, 3600, r, fail)) return fail;
        // the report's essentials: the full report is report.json in the output folder
        std::string s;
        const json mix = r.value("mix", json::object());
        const double dur = r.value("duration", 0.0);
        s += std::string(r.value("ok", false) ? "ok" : "NOT OK") + "   " + fmt("%d:", (int)(dur / 60)) + fmt("%04.1f", std::fmod(dur, 60.0)) +
             "   mix " + fmt("%.1f LUFS", mix.value("lufs", -120.0)) + "   LRA " + fmt("%.1f", mix.value("lra", 0.0)) +
             "   true peak " + fmt("%.1f dBTP", mix.value("truePeakDb", -120.0)) + "   rendered in " + fmt("%.1f s", r.value("renderSeconds", 0.0)) + "\n";
        if (r.contains("error")) s += "error: " + r["error"].get<std::string>() + "\n";
        if (r.contains("window")) s += "window: bars " + numArg(r["window"].value("fromBar", json(0))) + "-" + numArg(r["window"].value("toBar", json(0))) + "\n";
        const auto sections = r.value("sections", json::array());
        if (!sections.empty()) {
            s += "sections:";
            for (auto &x : sections) {
                s += "  " + x.value("name", "") + " " + fmt("%.1f", x.value("lufs", -120.0));
                if (x.contains("change") && x["change"].is_number()) s += fmt(" (%+.1f)", x["change"].get<double>());
                s += ",";
            }
            s.pop_back();
            s += "\n";
        }
        s += "tracks (stem LUFS before the fader):";
        for (auto &t : r.value("tracks", json::array())) s += "  " + t.value("name", "") + " " + fmt("%.1f", t.value("lufs", -120.0)) + ",";
        s.pop_back();
        s += "\n";
        const auto warnings = r.value("warnings", json::array());
        if (!warnings.empty()) {
            s += "warnings (" + std::to_string(warnings.size()) + "):\n";
            for (auto &w : warnings) s += "- " + w.get<std::string>() + "\n";
        } else s += "no warnings\n";
        const auto failed = r.value("failedTracks", json::array());
        if (!failed.empty()) s += "failed tracks: " + failed.dump() + "\n";
        s += "files: mix " + mix.value("file", "") + ", report " + (fs::path(outDir) / "report.json").string();
        if (r.contains("picture")) s += ", picture " + r["picture"].value("file", "");
        for (auto &d : mix.value("deliveries", json::array())) s += ", " + d.value("format", "") + " " + d.value("file", "");
        s += "\nThe full report (per-section track levels, automation, dropouts) is report.json. You can't hear this: ask the human to listen.";
        Result res = text(s, !r.value("ok", false));
        if (picture && r.contains("picture")) res.content.push_back(image(r["picture"].value("file", "")));
        return res;
    }
};

} // namespace

int runMcp(std::FILE *out) {
    Server s(out);
    return s.run();
}

} // namespace wl
