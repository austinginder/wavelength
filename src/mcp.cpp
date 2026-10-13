#include "mcp.hpp"

#include "docs.hpp"
#include "job.hpp"
#include "platform.hpp"
#include "song.hpp"

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
    "Never guess instrument or preset names: list them with list_instruments and list_presets. If you can't write files "
    "yourself, write_job saves a job into a song folder (new songs go to the songs folder; list_songs lists them) and "
    "read_job reads one back; every tool takes a song by its folder or its name.";

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

// ---- songs ------------------------------------------------------------------------------------
// Where new songs go when the agent names no folder: $WAVELENGTH_SONGS, else Music/Wavelength in the
// home folder (outside Documents, which macOS asks permission for).
fs::path songsRoot() {
    const char *e = std::getenv("WAVELENGTH_SONGS");
    return e && *e ? fs::u8path(e) : platform::homeDir() / "Music" / "Wavelength";
}

// A song argument: a path, or the bare name of a folder in songsRoot() (clients such as Claude
// Desktop have no working folder that means anything).
fs::path songFolder(const std::string &s) {
    std::error_code ec;
    const fs::path p = fs::u8path(s);
    if (p.is_relative() && !p.has_parent_path() && !fs::exists(p, ec)) return songsRoot() / p;
    return fs::absolute(p, ec);
}

// A job argument: a job file, a song folder (its job.json) or a song's name.
std::string jobFile(const std::string &s) {
    std::error_code ec;
    fs::path p = songFolder(s);
    if (fs::is_directory(p, ec)) p /= "job.json";
    return p.u8string();
}

std::string slugOf(const std::string &title) {
    std::string slug;
    for (unsigned char c : title) {
        if (std::isalnum(c)) slug += (char)std::tolower(c);
        else if (!slug.empty() && slug.back() != '-') slug += '-';
    }
    if (slug.size() > 60) slug.resize(60);
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug.empty() ? "song" : slug;
}

// JSON for people and agents to read: anything that fits in 100 characters on one line (a note, a
// marker, a short list), the rest indented
std::string tidyJson(const json &j, int indent = 0) {
    const std::string flat = j.dump(-1, ' ', false, json::error_handler_t::replace);
    if (flat.size() + indent <= 100 || !j.is_structured() || j.empty()) return flat;
    const std::string pad(indent + 2, ' ');
    std::string s = j.is_object() ? "{\n" : "[\n";
    bool first = true;
    for (auto it = j.begin(); it != j.end(); ++it) {
        s += (first ? "" : ",\n") + pad + (j.is_object() ? json(it.key()).dump() + ": " : std::string()) + tidyJson(*it, indent + 2);
        first = false;
    }
    return s + "\n" + std::string(indent, ' ') + (j.is_object() ? "}" : "]");
}

std::string readText(const fs::path &f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
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
    const json jobProp = prop("string", "the song: its folder, its job.json, or its name in the songs folder");
    json list = json::array();
    list.push_back(tool("guide", "Read the Wavelength guide",
        "Wavelength's own documentation, matching this engine. doc \"agents\" is the operating guide: the render loop, choosing "
        "sounds, arranging, mixing, mastering, reading the report and the picture. Read it in full before writing a first job. "
        "\"job-format\" is the job JSON reference, \"effects\" the effects, buses and automation reference, \"song-format\" the "
        "song folder and .wavelength format (history, comments, packages). Pass section (part of a heading) to read one part again.",
        {{"doc", {{"type", "string"}, {"enum", {"agents", "job-format", "effects", "song-format"}}, {"description", "which document (default agents)"}}},
         {"section", prop("string", "part of a heading, e.g. \"Mixing\"; omit for the whole document")}},
        {}, readOnly));
    const std::string root = songsRoot().u8string();
    list.push_back(tool("list_songs", "List the songs",
        ("The songs in the songs folder (" + root + "; WAVELENGTH_SONGS sets another): each one's name, how long ago its job "
         "changed and whether it has a render. Give a song's name as job or song to the other tools.").c_str(),
        {{"search", prop("string", "only songs whose name contains this")}}, {}, readOnly));
    list.push_back(tool("read_job", "Read a song's job",
        "A song's job.json as it is now, with the files beside it that change what it plays or say what it is for: sounds.json "
        "(sounds a person chose and tuned by ear), edits.json (note edits from the review page), riffs.json (recorded riffs) and "
        "brief.md (what they want). Read it before changing a song you didn't write in this conversation.",
        {{"job", jobProp}}, {"job"}, readOnly));
    list.push_back(tool("write_job", "Write a song's job",
        ("Saves a job (the JSON the job-format guide describes) as a song's job.json, for clients that can't write files "
         "themselves. Without song, it makes a new song folder in " + root + ", named after title. The job is parsed first: "
         "one that doesn't parse is refused with the reason and nothing is written. It replaces the song's job.json (use save "
         "first to keep the old one as a revision); then lint and render it by the song's name or folder.").c_str(),
        {{"job", {{"type", "object"}, {"description", "the job itself: tempo, tracks with their sounds and notes, markers, master..."}}},
         {"song", prop("string", "the song to write: its folder or its name (omit for a new song)")},
         {"title", prop("string", "a new song's title; its folder is named after it")}},
        {"job"}, writes));
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
        "Sample libraries builtin:sampler plays: Bitwig multisamples, drum kits, SFZ instruments, SoundFonts, Logic and GarageBand "
        "instruments (exs) and patches (patch), loops, and impulse responses (ir) for the convolve effect.",
        {{"search", prop("string", "only libraries matching this")}}, {}, readOnly));
    list.push_back(tool("list_loops", "List Apple Loops",
        "The Apple Loops GarageBand and Logic install (macOS): category, key, tempo and length in beats. A builtin:audio clip "
        "{\"file\": \"lib:Apple Loops/<name>.caf\", \"beat\": 0, \"repeat\": 4, \"key\": \"song\"} follows the song's tempo and moves "
        "into the job's key. Give \"notes\" a loop's name to get the notes inside a software-instrument loop, in beats.",
        {{"search", prop("string", "only loops whose name, category, genre, key or descriptors contain all these words")},
         {"key", prop("string", "a key (\"D minor\"): each loop's shift into it, and the notes moved into it")},
         {"notes", prop("string", "a loop's name: its notes instead of the list")}}, {}, readOnly));
    list.push_back(tool("plugin_params", "List a plugin's parameters",
        "A plugin's parameters with their ranges and display text (for \"params\" and automation.params), optionally after loading "
        "a preset. builtin:synth lists its own parameter names.",
        {{"plugin", prop("string", "the plugin's name or id")}, {"preset", prop("string", "load this preset first")},
         {"search", prop("string", "only parameters whose name contains this")}},
        {"plugin"}, readOnly));
    list.push_back(tool("save_preset", "Save a sound as a preset file",
        "Saves a plugin's sound as a file a track loads as \"state\": a starting preset or state file plus parameter changes "
        "(as in a track's \"params\": plain values or the plugin's own text; names from plugin_params). Keep a sound you designed so other "
        "songs and DAW projects can use it; save it inside the song folder so the song carries it. The file is the format's own "
        "state (Wavelength reads its content, not its name): name it .clap-preset for CLAP, .vstpreset for VST3, .aupreset for "
        "Audio Units, .fxb for VST2.",
        {{"plugin", prop("string", "the plugin's name or id (not builtin:synth: its sound is the track's \"synth\" object)")},
         {"out", prop("string", "the file to write, a full path: <song folder>/sounds/lead.clap-preset")},
         {"preset", prop("string", "start from this preset (default: the plugin's default sound)")},
         {"state", prop("string", "start from this state file instead (a .vital, .clap-preset, .vstpreset, .fxp, ...)")},
         {"params", {{"type", "object"},
                     {"description", "parameter values to set on top, each a number or the plugin's own text: {\"Filter Cutoff\": 0.4} or "
                                     "{\"Filter Cutoff\": \"800 Hz\"}; \"Module/Name\" or \"#id\" when a name is ambiguous"}}}},
        {"plugin", "out"}, writes));
    list.push_back(tool("lint", "Check a job's harmony",
        "Before rendering: wrong notes, out-of-key chords, clashes between parts, parallel fifths and octaves, from the job's notes.",
        {{"job", jobProp}}, {"job"}, readOnly));
    list.push_back(tool("stage", "Set every fader from its loudness",
        "Gain staging: renders the job once and sets each track's fader to target - its stem LUFS, the target from the track's role "
        "(a word of its name: Kick -12, Bass -15.5, Sub -19, Lead -16, Pad -22, Arp -21 ...) or targets.json beside the job. Writes "
        "gains.json beside the job (for a generator script); apply also sets the gains in the job. Lists tracks no role fits and tracks "
        "that peak over 0 dBFS after their fader.",
        {{"job", jobProp}, {"apply", prop("boolean", "also set the gains in the job file")},
         {"report", prop("string", "measure this render's report.json instead of rendering")}},
        {"job"}, writes));
    list.push_back(tool("render", "Render a song",
        "Renders a job through its instruments and effects and returns the report's summary (loudness of the mix, each section "
        "and track, warnings with their fixes) and the picture of the song (sections, loudness contour, spectrum, every track's "
        "notes over its level). Writes mix.wav, report.json and song.png into out. from_bar/to_bar render a few bars in seconds.",
        {{"job", jobProp}, {"out", prop("string", "output folder (default: out next to the job)")},
         {"from_bar", prop("number", "render from this bar (with to_bar)")}, {"to_bar", prop("number", "render up to this bar (exclusive)")},
         {"tracks", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "only these tracks (with the song's buses and master)"}}},
         {"stems", {{"type", "string"}, {"enum", {"none", "16", "24", "float"}}, {"description", "stem files (default none)"}}},
         {"deliver", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "extra files next to mix.wav: mp3, flac, wav:16..."}}},
         {"loop", prop("boolean", "with from_bar/to_bar: a seamless loop for games (tail folded onto the start, smpl loop chunk)")},
         {"picture", prop("boolean", "attach the song picture (default true)")},
         {"keep", prop("boolean", "a song's whole render: keep its MP3, picture and report in the song's render/ (the render that goes with it)")},
         {"fallbacks", prop("boolean", "play every track's first available fallback, as a computer without the plugins would")}},
        {"job"}, writes));
    list.push_back(tool("picture", "Picture of an arrangement",
        "Draws a job's arrangement before rendering (sections, bars, every track's notes) and returns the image; the render tool "
        "draws the full picture with levels and spectrum.",
        {{"job", jobProp}, {"width", prop("number", "pixels, 800-3200 (default 1400)")}}, {"job"}, writes));
    list.push_back(tool("patch_card", "See how presets sound",
        "Plays each patch through one probe (a held C4, C2-C5, a C minor chord, 16ths) and returns a picture: one patch = a full card "
        "(the held note's spectrum with note names and harmonics, level, attack and tail, pitch over time, waveform, the four octaves, "
        "the chord and the run), several = a contact sheet to compare. With the numbers and flags: the note it really sounds (and the "
        "transpose that fixes it), slow attack, long tail, percussive, silent low notes, noise before the first note. Use it to choose "
        "sounds before writing them into a job.",
        {{"patches", {{"type", "array"}, {"description", "the patches to draw"},
                      {"items", {{"type", "object"}, {"properties", {{"plugin", prop("string", "the plugin's name or id, or builtin:synth")},
                                                                     {"preset", prop("string", "a preset name (none: the default sound)")}}},
                                 {"required", {"plugin"}}}}}},
         {"out", prop("string", "the PNG to write (default: in Wavelength's cache folder)")},
         {"width", prop("number", "pixels, 800-3200 (default 1400)")}},
        {"patches"}, writes));
    list.push_back(tool("analyze", "Measure audio",
        "Measures a WAV, or a render folder (its mix, every stem and section): pitch, brightness, band balance, stereo width, "
        "onsets and envelope, and with peaks the strongest spectral peaks. For what the report doesn't say: an octave, a harsh band.",
        {{"path", prop("string", "a WAV file or a render folder")}, {"start", prop("number", "seconds")}, {"end", prop("number", "seconds")},
         {"song_time", prop("boolean", "start/end are song time (a render's lead-in added)")}, {"peaks", prop("boolean", "list spectral peaks")}},
        {"path"}, readOnly));
    list.push_back(tool("timeline", "Song time of bars and markers",
        "Song time of every marker and every few bars from the tempo map, in song and file seconds: find bar 57, or plan a length.",
        {{"job", jobProp}, {"every", prop("number", "bars between rows (default 8)")}}, {"job"}, readOnly));
    const json song = prop("string", "the song: its folder, or its name in the songs folder");
    list.push_back(tool("save", "Save a revision of a song",
        "Saves the song's job and its source, notes and media files as a revision in its history (the first save makes the "
        "manifest). Save before a risky change; renders are revisions too.",
        {{"song", song}, {"message", prop("string", "what changed")}}, {"song"}, writes));
    list.push_back(tool("history", "A song's revisions",
        "Every revision, newest last: saves, renders (with their loudness), undo, redo, restore, and which one is current.",
        {{"song", song}}, {"song"}, readOnly));
    list.push_back(tool("undo", "Undo, redo or restore a song",
        "Undo steps back one change the way an editor does, redo steps forward again, restore brings back any revision's files. "
        "Each adds a revision, so nothing is lost.",
        {{"song", song}, {"action", {{"type", "string"}, {"enum", {"undo", "redo", "restore"}}, {"description", "default undo"}}},
         {"revision", prop("number", "for restore: the revision to bring back")}},
        {"song"}, writes));
    list.push_back(tool("diff", "What changed in a song",
        "What changed in the music between two revisions, by track and bar: sounds, mix, effects and curves, notes.",
        {{"song", song}, {"from", prop("number", "revision (default: the last one)")}, {"to", prop("number", "revision (default: the folder now)")}},
        {"song"}, readOnly));
    list.push_back(tool("comments", "A song's review comments",
        "The human's open comments from the review UI, each with the revision and render it was made on, the bars, tracks and "
        "notes it points at, and whether the music there changed since. Read them before working on a song.",
        {{"song", song}, {"all", prop("boolean", "include resolved ones")}}, {"song"}, readOnly));
    list.push_back(tool("reply", "Answer a comment",
        "Replies to a comment (what you changed and where) and, with done, resolves it at the song's current revision.",
        {{"song", song}, {"id", prop("string", "the comment's id")}, {"text", prop("string", "the reply")}, {"done", prop("boolean", "resolve it")}},
        {"song", "id", "text"}, writes));
    list.push_back(tool("fallbacks", "Stand-in sounds for a song",
        "What each track plays on this computer; with suggest, a built-in stand-in for every plugin or library track without "
        "one, level-matched to the last render; with write, adds them to the job.",
        {{"song", song}, {"suggest", prop("boolean", "propose fallbacks")}, {"write", prop("boolean", "write them into the job")}},
        {"song"}, writes));
    list.push_back(tool("pack", "Pack a song into one file",
        "Makes a .wavelength file (a ZIP of the song folder: job, listed files, comments, history) to share, and lists the "
        "plugins and libraries it needs. Validates the song first.",
        {{"song", song}, {"out", prop("string", "the .wavelength file (default <slug>.wavelength in the current folder)")}}, {"song"}, writes));
    list.push_back(tool("import", "Import MIDI, MusicXML, DAWproject, a Bitwig or a GarageBand project",
        "Makes a job from a MIDI file, a MusicXML score (MuseScore, Sibelius, Dorico), a DAWproject, a Bitwig Studio project (.bwproject, no export needed) "
        "or a GarageBand project (.band: its tracks play their own channel strips), ready to render and edit.",
        {{"file", prop("string", ".mid, .musicxml/.mxl, .dawproject, .bwproject or .band")}, {"out", prop("string", "the song folder for job.json, or a new song's name in the songs folder")}}, {"file", "out"}, writes));
    return list;
}

// ---- the server -------------------------------------------------------------------------------
class Server {
public:
    explicit Server(std::FILE *out) : out_(out) {}

    int run() {
        // Warm the plugin cache now, so the first list_instruments on a new computer doesn't spend
        // its client's tool timeout scanning (a second `plugins` waits for this one's lock, then reads
        // what it found). Its output goes to /dev/null, never into the protocol.
        platform::Process scan;
        if (platform::spawn({platform::selfExecutable(), "plugins", "--json"}, scan, false, true))
            std::thread([scan]() mutable {
                std::string crash;
                while (!platform::finished(scan, crash)) std::this_thread::sleep_for(std::chrono::seconds(1));
            }).detach();
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
        if (name == "list_songs") {
            const fs::path root = songsRoot();
            const std::string q = lower(str(a, "search"));
            std::error_code ec;
            struct Row { std::string name; fs::file_time_type changed; bool rendered; };
            std::vector<Row> rows;
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::path job = it->path() / "job.json";
                if (!it->is_directory(ec) || !fs::exists(job, ec)) continue;
                const std::string n = it->path().filename().u8string();
                if (!q.empty() && lower(n).find(q) == std::string::npos) continue;
                rows.push_back({n, fs::last_write_time(job, ec), fs::exists(it->path() / "out" / "mix.wav", ec) || fs::exists(it->path() / "render", ec)});
            }
            if (rows.empty()) return text("no songs" + std::string(q.empty() ? "" : " matching that") + " in " + root.u8string() + ": write_job makes one");
            std::sort(rows.begin(), rows.end(), [](const Row &x, const Row &y) { return x.changed > y.changed; });
            std::string lines = "songs in " + root.u8string() + ", newest first:\n";
            const auto now = fs::file_time_type::clock::now();
            for (auto &r : rows) {
                const long days = (long)std::chrono::duration_cast<std::chrono::hours>(now - r.changed).count() / 24;
                lines += r.name + "  (job changed " + (days < 1 ? std::string("today") : days == 1 ? std::string("yesterday") : std::to_string(days) + " days ago") +
                         (r.rendered ? ", rendered" : "") + ")\n";
            }
            return text(lines);
        }
        if (name == "read_job") {
            const fs::path file = fs::u8path(jobFile(need(a, "job"))), dir = file.parent_path();
            std::error_code ec;
            if (!fs::is_regular_file(file, ec)) return text("error: no job at " + file.u8string() + " (list_songs lists the songs)", true);
            const uintmax_t kMax = 512 * 1024;
            if (fs::file_size(file, ec) > kMax) return text("error: " + file.u8string() + " is over 512 KB: too big to read whole here", true);
            std::string t = "job.json (" + file.u8string() + "):\n" + readText(file);
            for (const char *f : {"brief.md", "sounds.json", "edits.json", "riffs.json"}) {
                const fs::path x = dir / f;
                if (!fs::is_regular_file(x, ec)) continue;
                t += std::string("\n\n") + f + (fs::file_size(x, ec) > kMax ? ": over 512 KB, not shown" : ":\n" + readText(x));
            }
            return text(t);
        }
        if (name == "write_job") {
            if (!a.contains("job") || !a["job"].is_object()) return text("error: job is the job itself, a JSON object (tempo, tracks, ...)", true);
            std::error_code ec;
            fs::path dir;
            if (!str(a, "song").empty()) dir = songFolder(str(a, "song"));
            else {
                const std::string slug = slugOf(str(a, "title"));
                dir = songsRoot() / slug;
                for (int n = 2; fs::exists(dir, ec) && n < 1000; ++n) dir = songsRoot() / (slug + "-" + std::to_string(n));
            }
            Job parsed;
            std::string err;
            bool ok = false;
            try { ok = parseJob(a["job"], dir.u8string(), parsed, err, true); }
            catch (const std::exception &e) { err = e.what(); }
            if (!ok) return text("error: the job doesn't parse, so nothing was written: " + err, true);
            fs::create_directories(dir, ec);
            if (ec) return text("error: could not make " + dir.u8string() + ": " + ec.message(), true);
            const fs::path file = dir / "job.json", tmp = dir / "job.json.tmp";
            {
                std::ofstream o(tmp, std::ios::binary);
                o << tidyJson(a["job"]) << "\n";
                if (!o) return text("error: could not write " + tmp.u8string(), true);
            }
            fs::rename(tmp, file, ec);
            if (ec) return text("error: could not write " + file.u8string() + ": " + ec.message(), true);
            std::string names;
            size_t notes = 0;
            for (auto &tr : parsed.tracks) { names += (names.empty() ? "" : ", ") + tr.name; notes += tr.notes.size(); }
            return text("wrote " + file.u8string() + ": " + std::to_string(parsed.tracks.size()) + " tracks (" + names + "), " + std::to_string(notes) +
                        " notes, " + std::to_string(parsed.markers.size()) + " markers\nlint and render it with job \"" +
                        (fs::equivalent(dir.parent_path(), songsRoot(), ec) ? dir.filename().u8string() : dir.u8string()) + "\"");
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
                                      : lines + "\nUse with \"plugin\": \"builtin:sampler\" and \"sampler\": {\"<kind>\": \"<name>\"} (multisample, kit, sfz, exs, patch, soundfont).");
        }
        if (name == "list_loops") {
            std::vector<std::string> args = {"loops", "--json"};
            for (const char *k : {"search", "key", "notes"})
                if (!str(a, k).empty()) { args.push_back(std::string("--") + k); args.push_back(str(a, k)); }
            if (!run(args, id, progress, 600, out, fail)) return fail;
            if (!str(a, "notes").empty()) return text(out.dump(1));
            std::string lines;
            size_t shown = 0;
            const auto list = out.value("loops", json::array());
            for (auto &l : list) {
                if (shown++ >= 200) break;
                const std::string key = !l.contains("key") ? "no key" : l["key"].get<std::string>() + (l.value("scale", "").empty() ? "" : " " + l.value("scale", ""));
                lines += l.value("name", "") + "  (" + l.value("category", "") + "/" + l.value("subcategory", "") + ", " + key + ", " +
                         std::to_string((int)std::lround(l.value("bpm", 0.0))) + " bpm, " + std::to_string(l.value("beats", 0)) + " beats" +
                         (l.value("notes", false) ? ", notes" : "") + (l.contains("shift") ? ", shift " + std::to_string(l["shift"].get<int>()) : "") + ")\n";
            }
            if (list.size() > 200) lines += "... " + std::to_string(list.size() - 200) + " more: narrow with search\n";
            return text(lines.empty() ? "no Apple Loops match (GarageBand or Logic Pro installs them on macOS)"
                                      : lines + "\nPlay one on a builtin:audio track: {\"file\": \"lib:Apple Loops/<name>.caf\", \"beat\": 0, \"repeat\": 4, \"key\": \"song\"}.");
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
        if (name == "stage") {
            std::vector<std::string> args = {"stage", jobFile(need(a, "job")), "--json"};
            if (a.value("apply", false)) args.push_back("--apply");
            if (a.contains("report") && a["report"].is_string()) { args.push_back("--report"); args.push_back(a["report"].get<std::string>()); }
            if (!run(args, id, progress, 7200, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "lint") {   // the harmony check, then voice leading (parallel fifths and octaves)
            if (!run({"lint", jobFile(need(a, "job")), "--harmony", "--json"}, id, progress, 300, out, fail)) return fail;
            json voices;
            Result vfail;
            json both = {{"harmony", out}};
            if (run({"lint", jobFile(need(a, "job")), "--json"}, id, progress, 300, voices, vfail)) both["voiceLeading"] = voices;
            else both["voiceLeading"] = {{"error", vfail.content.empty() ? json("failed") : vfail.content[0].value("text", json("failed"))}};
            return text(both.dump(1));
        }
        if (name == "timeline") {
            std::vector<std::string> args = {"timeline", jobFile(need(a, "job")), "--json"};
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
            if (!run({"import", need(a, "file"), "--out", songFolder(need(a, "out")).u8string(), "--json"}, id, progress, 600, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "picture") {
            std::vector<std::string> args = {"picture", jobFile(need(a, "job")), "--json"};
            if (a.contains("width") && a["width"].is_number()) { args.push_back("--width"); args.push_back(numArg(a["width"])); }
            if (!run(args, id, progress, 120, out, fail)) return fail;
            Result r = text("arrangement: " + out.value("file", ""));
            r.content.push_back(image(out.value("file", "")));
            return r;
        }
        if (name == "save_preset") {
            const std::string plugin = need(a, "plugin"), file = fs::absolute(fs::u8path(need(a, "out"))).lexically_normal().u8string();
            if (!str(a, "preset").empty() && !str(a, "state").empty()) return text("error: give preset or state, not both", true);
            std::vector<std::string> args = {"state", "save", plugin, "--out", file, "--json"};
            if (!str(a, "preset").empty()) { args.push_back("--preset"); args.push_back(str(a, "preset")); }
            if (!str(a, "state").empty()) { args.push_back("--state"); args.push_back(str(a, "state")); }
            if (a.contains("params") && !a["params"].is_null()) {
                if (!a["params"].is_object()) return text("error: params is an object of parameter names and values", true);
                for (auto &[k, v] : a["params"].items()) {
                    if (!v.is_number() && !v.is_string()) return text("error: params \"" + k + "\" needs a number or the plugin's own text (plugin_params shows both)", true);
                    args.push_back("--set");
                    args.push_back(k + "=" + (v.is_string() ? v.get<std::string>() : v.dump()));
                }
            }
            std::error_code ec;
            fs::create_directories(fs::u8path(file).parent_path(), ec);
            if (!run(args, id, progress, 600, out, fail)) return fail;
            return text("saved " + std::to_string(out.value("bytes", 0)) + " bytes of " + out.value("format", "") + " state to " + file +
                        "\nUse it on a track: " + json{{"plugin", plugin}, {"state", file}}.dump() +
                        " (in a song, write the path relative to the job's folder: it is read from there)");
        }
        if (name == "patch_card") {
            if (!a.contains("patches") || !a["patches"].is_array() || a["patches"].empty()) return text("patches: give at least one {plugin, preset}", true);
            std::vector<std::string> args = {"card"};
            for (auto &p : a["patches"]) {
                if (!p.is_object() || str(p, "plugin").empty()) return text("patches: every entry needs a plugin", true);
                if (args.size() > 1) args.push_back("+");
                args.push_back(str(p, "plugin"));
                if (!str(p, "preset").empty()) args.push_back(str(p, "preset"));
            }
            std::string file = str(a, "out");
            if (file.empty()) {
                std::error_code ec;
                const auto dir = platform::cacheDir() / "cards";
                std::filesystem::create_directories(dir, ec);
                file = (dir / ("card-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".png")).string();
            }
            args.insert(args.end(), {"--out", file, "--json"});
            if (a.contains("width") && a["width"].is_number()) { args.push_back("--width"); args.push_back(numArg(a["width"])); }
            if (!run(args, id, progress, 900, out, fail)) return fail;
            json summary = out;
            for (auto &p : summary["patches"]) p.erase("held");   // the picture shows it; keep the summary short
            Result r = text(summary.dump(1));
            r.content.push_back(image(out.value("file", "")));
            return r;
        }
        if (name == "render") return render(a, id, progress);
        if (name == "save") {
            std::vector<std::string> args = {"save", songFolder(need(a, "song")).u8string(), "--json"};
            if (!str(a, "message").empty()) { args.push_back("-m"); args.push_back(str(a, "message")); }
            if (!run(args, id, progress, 120, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "history" || name == "comments") {
            std::vector<std::string> args = {name, songFolder(need(a, "song")).u8string(), "--json"};
            if (name == "comments" && a.value("all", false)) args.push_back("--all");
            if (!run(args, id, progress, 120, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "undo") {
            const std::string action = str(a, "action").empty() ? "undo" : str(a, "action");
            if (action != "undo" && action != "redo" && action != "restore") return text("error: action is undo, redo or restore", true);
            std::vector<std::string> args = {action, songFolder(need(a, "song")).u8string()};
            if (action == "restore") {
                if (!a.contains("revision") || !a["revision"].is_number()) return text("error: restore needs a revision", true);
                args.push_back(numArg(a["revision"]));
            }
            args.push_back("--json");
            if (!run(args, id, progress, 120, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "diff") {
            std::vector<std::string> args = {"diff", songFolder(need(a, "song")).u8string()};
            for (const char *k : {"from", "to"})
                if (a.contains(k) && a[k].is_number()) args.push_back("r" + numArg(a[k]));
            args.push_back("--json");
            if (!run(args, id, progress, 120, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "reply") {
            std::vector<std::string> args = {"comments", songFolder(need(a, "song")).u8string(), "--reply", need(a, "id"), "--text", need(a, "text"), "--json"};
            if (a.value("done", false)) args.push_back("--done");
            if (!run(args, id, progress, 120, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "fallbacks") {
            std::vector<std::string> args = {"fallbacks", songFolder(need(a, "song")).u8string(), "--json"};
            if (a.value("suggest", false) || a.value("write", false)) args.push_back("--suggest");
            if (a.value("write", false)) args.push_back("--write");
            if (!run(args, id, progress, 1800, out, fail)) return fail;
            return text(out.dump(1));
        }
        if (name == "pack") {
            std::vector<std::string> args = {"pack", songFolder(need(a, "song")).u8string(), "--json"};
            if (!str(a, "out").empty()) { args.push_back("--out"); args.push_back(str(a, "out")); }
            if (!run(args, id, progress, 600, out, fail)) return fail;
            return text(out.dump(1));
        }
        return text("error: unknown tool " + name, true);
    }

    Result render(const json &a, const json &id, const json &progress) {
        const std::string job = jobFile(need(a, "job"));
        std::string outDir = str(a, "out");
        if (outDir.empty()) outDir = (fs::path(job).parent_path() / "out").string();
        std::vector<std::string> args = {"render", job, "--out", outDir, "--json", "--stems", str(a, "stems").empty() ? "none" : str(a, "stems")};
        const bool picture = a.value("picture", true);
        if (picture) args.push_back("--png");
        if (a.contains("from_bar") && a["from_bar"].is_number()) { args.push_back("--from"); args.push_back(numArg(a["from_bar"])); }
        if (a.contains("to_bar") && a["to_bar"].is_number()) { args.push_back("--to"); args.push_back(numArg(a["to_bar"])); }
        if (a.value("loop", false)) args.push_back("--loop");
        if (a.value("keep", false)) args.push_back("--keep");
        if (a.value("fallbacks", false)) args.push_back("--fallbacks");
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
