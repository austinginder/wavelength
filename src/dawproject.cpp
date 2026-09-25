#include "dawproject.hpp"

#include "catalog.hpp"
#include "vst2_abi.hpp"
#include "xml.hpp"
#include "zip.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

double linToDb(double v) { return v > 1e-6 ? 20.0 * std::log10(v) : -120.0; }
double r3(double v) { return std::round(v * 1000) / 1000; }

struct Ctx {
    Zip zip;
    std::string outDir;
    DawprojectImport *res;
    std::set<std::string> written;   // plugin states already extracted
    double bpm = 120;
    std::map<std::string, std::string> busByChannel;    // channel id -> bus name (effect returns, groups)
    std::map<std::string, std::string> trackByChannel;  // channel id -> track name
    std::map<std::string, std::string> channelOfTrack;  // track id -> channel id
    std::map<std::string, std::pair<std::string, std::string>> paramTarget;   // parameter id -> (track/bus name, "volume"|"pan")
    std::set<std::string> names;
};

std::string uniqueName(Ctx &c, std::string n) {
    if (n.empty()) n = "Track";
    std::string cand = n;
    for (int k = 2; c.names.count(cand); ++k) cand = n + " " + std::to_string(k);
    c.names.insert(cand);
    return cand;
}

// copy a plugin state out of the archive; returns the job-relative path ("" on failure)
std::string extractState(Ctx &c, const std::string &archivePath) {
    if (archivePath.empty()) return "";
    const std::string rel = "plugins/" + fs::path(archivePath).filename().string();
    if (c.written.count(rel)) return rel;
    std::vector<uint8_t> data;
    std::string err;
    if (!c.zip.read(archivePath, data, err)) { c.res->notes.push_back("plugin state " + archivePath + " is missing from the file: " + err); return ""; }
    std::error_code ec;
    fs::create_directories(fs::path(c.outDir) / "plugins", ec);
    std::ofstream o(fs::path(c.outDir) / rel, std::ios::binary);
    o.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
    c.written.insert(rel);
    return rel;
}

double paramValue(const xml::Node *dev, const char *tag, double def) {
    const xml::Node *p = dev ? dev->child(tag) : nullptr;
    return p ? p->num("value", def) : def;
}

// One device: a plugin (with state), a DAWproject standard effect, or a DAW's own device (noted, left out).
// Returns a json object: {"plugin": spec, "state": path} for instruments/plugin effects, or a built-in effect.
bool mapDevice(Ctx &c, const xml::Node &d, const std::string &where, json &out) {
    const std::string tag = d.tag, name = d.get("name", d.get("deviceName"));
    const xml::Node *en = d.child("Enabled");
    if (en && en->get("value", "true") == "false") { c.res->notes.push_back(where + ": device '" + name + "' is switched off in the DAW; left out"); return false; }
    std::string spec;
    if (tag == "Vst3Plugin") spec = "vst3:" + d.get("deviceID");
    else if (tag == "ClapPlugin") spec = "clap:" + d.get("deviceID");
    else if (tag == "Vst2Plugin") spec = "vst2:" + vst2::fourcc((int32_t)std::strtoll(d.get("deviceID").c_str(), nullptr, 10));
    else if (tag == "AuPlugin") { c.res->notes.push_back(where + ": Audio Unit '" + name + "' can't be hosted yet; left out"); return false; }
    if (!spec.empty()) {
        out = {{"plugin", spec}};
        if (const xml::Node *st = d.child("State")) {
            const std::string rel = extractState(c, st->get("path"));
            if (!rel.empty()) out["state"] = rel;
        }
        PluginInfo pi;
        std::string e;
        if (!resolvePlugin(spec, pi, e)) {   // fall back to the name when the id isn't installed in this format
            PluginInfo byName;
            if (resolvePlugin(name, byName, e)) {
                c.res->notes.push_back(where + ": " + spec + " is not installed; using " + byName.format + " '" + byName.name + "' by name (its state may not load)");
                out["plugin"] = byName.format + ":" + byName.id;
            } else c.res->notes.push_back(where + ": plugin '" + name + "' (" + spec + ") is not installed");
        }
        ++c.res->plugins;
        return true;
    }
    // DAWproject standard devices
    if (tag == "Equalizer") {
        json bands = json::array();
        for (const xml::Node *b : d.all("Band")) {
            const std::string type = b->get("type", "bell");
            std::string t = type == "highPass" ? "highpass" : type == "lowPass" ? "lowpass" : type == "lowShelf" ? "lowshelf" : type == "highShelf" ? "highshelf" : "peak";
            const xml::Node *on = b->child("Enabled");
            if (on && on->get("value", "true") == "false") continue;
            json bj = {{"type", t}, {"freq", r3(paramValue(b, "Freq", 1000))}};
            if (t == "peak" || t == "lowshelf" || t == "highshelf") bj["gain"] = r3(paramValue(b, "Gain", 0));
            if (b->child("Q")) bj["q"] = r3(paramValue(b, "Q", 0.707));
            bands.push_back(bj);
        }
        out = {{"type", "eq"}, {"bands", bands}};
        return true;
    }
    if (tag == "Compressor") {
        out = {{"type", "compressor"}, {"threshold", r3(paramValue(&d, "Threshold", -20))}, {"ratio", r3(paramValue(&d, "Ratio", 4))},
               {"attack", r3(paramValue(&d, "Attack", 0.01) * 1000)}, {"release", r3(paramValue(&d, "Release", 0.1) * 1000)},
               {"makeup", r3(paramValue(&d, "OutputGain", 0))}};
        return true;
    }
    if (tag == "Limiter") {
        out = {{"type", "limiter"}, {"ceiling", r3(paramValue(&d, "Threshold", -1))}, {"release", r3(paramValue(&d, "Release", 0.08) * 1000)}};
        return true;
    }
    if (tag == "NoiseGate") { c.res->notes.push_back(where + ": noise gate left out (no built-in gate by threshold yet)"); return false; }
    // a DAW's own device: DAWproject carries its name, not its settings
    if (d.get("deviceName") == "Drum Machine" && d.get("deviceRole") == "instrument") return false;   // the caller puts a kit in its place
    c.res->notes.push_back(where + ": " + d.get("deviceName", name) + " is a device of the exporting DAW; its settings aren't in the file, so it is left out");
    return false;
}

// Notes of a lane or clip, into song beats. `offset` = song beat of content time 0; notes outside
// [from, to) song beats are dropped and ones crossing `to` are shortened.
void collectNotes(Ctx &c, const xml::Node &n, double offset, double from, double to, json &notes) {
    for (auto &chp : n.children) {
        const xml::Node &ch = *chp;
        if (ch.tag == "Notes") {
            for (const xml::Node *note : ch.all("Note")) {
                const double t = offset + note->num("time"), d = note->num("duration");
                if (t < from - 1e-9 || t >= to - 1e-9) continue;
                const double dur = std::min(d, to - t);
                if (dur <= 0) continue;
                json nj = {{"beat", r3(t)}, {"dur", r3(dur)}, {"key", (int)note->num("key", 60)}, {"vel", r3(std::clamp(note->num("vel", 0.8), 0.0, 1.0))}};
                const int chn = (int)note->num("channel", 0);
                if (chn) nj["channel"] = chn;
                notes.push_back(nj);
                ++c.res->noteCount;
            }
        } else if (ch.tag == "Clips") {
            for (const xml::Node *clip : ch.all("Clip")) {
                const double t = offset + clip->num("time"), d = clip->num("duration");
                const double ps = clip->num("playStart", 0);
                const double cs = std::max(from, t), ce = std::min(to, t + d);
                if (ce <= cs) continue;
                const bool loops = clip->attr("loopEnd") != nullptr;
                const double ls = clip->num("loopStart", 0), le = clip->num("loopEnd", 0);
                if (!loops || le <= ls) { collectNotes(c, *clip, t - ps, cs, ce, notes); continue; }
                // looping content: play from playStart to loopEnd, then loopStart..loopEnd again until the clip ends
                double pos = ps, song = t;
                for (int guard = 0; song < t + d - 1e-9 && guard < 10000; ++guard) {
                    const double segEnd = pos < le ? le : pos + (le - ls);
                    const double len = segEnd - pos;
                    collectNotes(c, *clip, song - pos, std::max(cs, song), std::min(ce, song + len), notes);
                    song += len;
                    pos = ls;
                }
            }
        } else if (ch.tag == "Lanes") collectNotes(c, ch, offset, from, to, notes);
    }
}

// Volume/pan automation: Points lanes whose target is a channel's Volume or Pan parameter
void collectAutomation(Ctx &c, const xml::Node &lanes, std::map<std::string, json> &gainPts, std::map<std::string, json> &panPts, size_t &unmapped) {
    std::vector<const xml::Node *> pts;
    lanes.walk("Points", pts);
    for (const xml::Node *p : pts) {
        const xml::Node *target = p->child("Target");
        const std::string param = target ? target->get("parameter") : "";
        auto it = c.paramTarget.find(param);
        if (it == c.paramTarget.end()) { ++unmapped; continue; }
        json curve = json::array();
        for (const xml::Node *rp : p->all("RealPoint")) {
            const double v = rp->num("value");
            curve.push_back({r3(rp->num("time")), it->second.second == "volume" ? r3(linToDb(v)) : r3(v * 2 - 1)});
        }
        if (curve.empty()) continue;
        (it->second.second == "volume" ? gainPts : panPts)[it->second.first] = curve;
    }
}

} // namespace

bool importDawproject(const std::string &path, const std::string &outDir, DawprojectImport &res, std::string &err) {
    Ctx c;
    c.outDir = outDir;
    c.res = &res;
    if (!c.zip.open(path, err)) return false;
    std::vector<uint8_t> xmlBytes;
    if (!c.zip.read("project.xml", xmlBytes, err)) { err = path + ": " + err; return false; }
    auto root = xml::parse(std::string(xmlBytes.begin(), xmlBytes.end()), err);
    if (!root) { err = path + ": project.xml: " + err; return false; }
    if (const xml::Node *app = root->child("Application")) res.application = app->get("name") + " " + app->get("version");

    json job = json::object();
    // transport
    if (const xml::Node *tr = root->child("Transport")) {
        if (const xml::Node *t = tr->child("Tempo")) c.bpm = t->num("value", 120);
        if (const xml::Node *ts = tr->child("TimeSignature")) job["timeSignature"] = {(int)ts->num("numerator", 4), (int)ts->num("denominator", 4)};
    }
    job["tempo"] = r3(c.bpm);
    const xml::Node *arrangement = nullptr;
    if (const xml::Node *st = root->child("Arrangement")) arrangement = st;
    if (!arrangement) { err = path + " has no arrangement (only launcher clips?)"; return false; }
    const xml::Node *arrLanes = arrangement->child("Lanes");
    if (arrLanes && arrLanes->get("timeUnit", "beats") != "beats") res.notes.push_back("arrangement is timed in seconds; times were read as beats (check the tempo)");
    if (const xml::Node *ta = arrangement->child("TempoAutomation")) {   // a tempo map
        json map = json::array();
        for (const xml::Node *p : ta->all("RealPoint"))
            map.push_back({{"beat", r3(p->num("time"))}, {"bpm", r3(p->num("value"))}, {"ramp", p->get("interpolation") == "linear"}});
        if (map.size() > 1) job["tempo"] = map;
    }

    // tracks, effect returns and groups: walk the structure (groups nest tracks)
    const xml::Node *structure = root->child("Structure");
    if (!structure) { err = path + " has no Structure"; return false; }
    struct TrackRef { const xml::Node *track, *channel; std::string name, role; bool group; };
    std::vector<TrackRef> refs;
    std::function<void(const xml::Node &)> walk = [&](const xml::Node &n) {
        for (const xml::Node *t : n.all("Track")) {
            const xml::Node *ch = t->child("Channel");
            const bool group = !t->all("Track").empty();
            refs.push_back({t, ch, "", ch ? ch->get("role", "regular") : "regular", group});
            if (group) walk(*t);
        }
    };
    walk(*structure);
    std::string masterChannel;
    for (auto &r : refs) {
        if (!r.channel) continue;
        if (r.role == "master") { masterChannel = r.channel->get("id"); r.name = "Master"; continue; }
        r.name = uniqueName(c, r.track->get("name", "Track"));
        if (r.role == "effect" || r.group) c.busByChannel[r.channel->get("id")] = r.name;
        else c.trackByChannel[r.channel->get("id")] = r.name;
        c.channelOfTrack[r.track->get("id")] = r.channel->get("id");
        for (const char *p : {"Volume", "Pan"})
            if (const xml::Node *pn = r.channel->child(p)) c.paramTarget[pn->get("id")] = {r.name, p == std::string("Volume") ? "volume" : "pan"};
    }

    // arrangement notes per track, and volume/pan automation
    std::map<std::string, json> notesOf, gainPts, panPts;
    size_t unmappedAuto = 0;
    if (arrLanes)
        for (const xml::Node *ln : arrLanes->all("Lanes")) {
            const std::string trackId = ln->get("track");
            json notes = json::array();
            collectNotes(c, *ln, 0, -1e18, 1e18, notes);
            if (!notes.empty()) {
                auto ch = c.channelOfTrack.find(trackId);
                const std::string name = ch != c.channelOfTrack.end() && c.trackByChannel.count(ch->second) ? c.trackByChannel[ch->second] : "";
                if (!name.empty()) for (auto &n : notes) notesOf[name].push_back(n);
                else res.notes.push_back(std::to_string(notes.size()) + " notes on a lane without an instrument track were left out");
            }
        }
    collectAutomation(c, *arrangement, gainPts, panPts, unmappedAuto);
    if (unmappedAuto) res.notes.push_back(std::to_string(unmappedAuto) + " automation lane(s) on device parameters were left out (volume and pan automation are imported)");
    {
        size_t launcher = 0;
        std::vector<const xml::Node *> slots;
        root->walk("ClipSlot", slots);
        for (auto *s : slots) if (s->child("Clip")) ++launcher;
        if (launcher) res.notes.push_back(std::to_string(launcher) + " clip-launcher clip(s) aren't rendered: only the arrangement plays");
    }

    // build tracks and buses
    json tracks = json::array(), buses = json::array();
    json master = {{"gain", 0}, {"fx", json::array()}};
    for (auto &r : refs) {
        if (!r.channel) continue;
        const xml::Node &ch = *r.channel;
        const double vol = ch.child("Volume") ? ch.child("Volume")->num("value", 1) : 1;
        const double pan = ch.child("Pan") ? ch.child("Pan")->num("value", 0.5) * 2 - 1 : 0;
        const bool muted = ch.child("Mute") && ch.child("Mute")->get("value") == "true";
        json fx = json::array();
        json instrument;
        if (const xml::Node *devs = ch.child("Devices"))
            for (auto &dp : devs->children) {
                const std::string role = dp->get("deviceRole", "audioFX");
                if (role == "noteFX") { res.notes.push_back(r.name + ": note effect '" + dp->get("name") + "' left out (note effects aren't imported)"); continue; }
                json m;
                if (!mapDevice(c, *dp, r.name, m)) {
                    if (role == "instrument" && dp->get("deviceName") == "Drum Machine") {   // Bitwig's drum sampler: a GM kit stands in
                        instrument = {{"plugin", "builtin:drums"}};
                        res.notes.push_back(r.name + ": Bitwig Drum Machine samples aren't in the export; builtin:drums (General MIDI kit) stands in. Swap in a sampler kit that matches");
                    }
                    continue;
                }
                if (role == "instrument" && instrument.is_null()) instrument = m;
                else fx.push_back(m);
            }
        const std::string dest = ch.get("destination");
        std::string output;
        if (!dest.empty() && dest != masterChannel && c.busByChannel.count(dest)) output = c.busByChannel[dest];
        json sends = json::object();
        if (const xml::Node *ss = ch.child("Sends"))
            for (const xml::Node *s : ss->all("Send")) {
                const xml::Node *on = s->child("Enable"), *lv = s->child("Volume");
                const double v = lv ? lv->num("value", 0) : 0;
                if ((on && on->get("value") == "false") || v <= 1e-6 || !c.busByChannel.count(s->get("destination"))) continue;
                sends[c.busByChannel[s->get("destination")]] = r3(linToDb(v));
                if (s->get("type") == "pre") res.notes.push_back(r.name + ": a pre-fader send is imported as post-fader");
            }
        if (r.role == "master") {
            master["gain"] = r3(linToDb(vol));
            master["fx"] = fx;
            continue;
        }
        if (r.role == "effect" || r.group) {
            json b = {{"name", r.name}, {"gain", r3(linToDb(vol))}, {"fx", fx}};
            if (!output.empty()) b["output"] = output;
            if (gainPts.count(r.name)) b["automation"] = {{"gain", gainPts[r.name]}};
            buses.push_back(b);
            ++res.buses;
            continue;
        }
        if (!notesOf.count(r.name)) {
            if (r.track->get("contentType").find("notes") != std::string::npos) res.notes.push_back(r.name + ": no notes in the arrangement; left out");
            else res.notes.push_back(r.name + ": audio tracks (recorded clips) aren't imported yet; left out");
            continue;
        }
        if (instrument.is_null()) { res.notes.push_back(r.name + ": no instrument the file can describe; left out"); continue; }
        json t = instrument;
        t["name"] = r.name;
        t["gain"] = vol <= 1e-6 ? -60.0 : r3(linToDb(vol));
        if (std::fabs(pan) > 1e-3) t["pan"] = r3(pan);
        if (muted || vol <= 1e-6) t["mute"] = true;
        if (!fx.empty()) t["fx"] = fx;
        if (!output.empty()) t["output"] = output;
        if (!sends.empty()) t["sends"] = sends;
        json autom = json::object();
        if (gainPts.count(r.name)) {   // absolute volume -> dB relative to the fader
            json rel = json::array();
            for (auto &p : gainPts[r.name]) rel.push_back({p[0], r3(p[1].get<double>() - t["gain"].get<double>())});
            autom["gain"] = rel;
        }
        if (panPts.count(r.name)) autom["pan"] = panPts[r.name];
        if (!autom.empty()) t["automation"] = autom;
        t["notes"] = notesOf[r.name];
        tracks.push_back(t);
        ++res.tracks;
    }
    job["tracks"] = tracks;
    if (!buses.empty()) job["buses"] = buses;
    job["master"] = master;
    // markers
    json markers = json::array();
    if (const xml::Node *ms = arrangement->child("Markers"))
        for (const xml::Node *m : ms->all("Marker")) markers.push_back({{"beat", r3(m->num("time"))}, {"name", m->get("name", "Marker")}});
    if (!markers.empty()) job["markers"] = markers;
    res.job = job;

    std::error_code ec;
    fs::create_directories(outDir, ec);
    std::ofstream o(fs::path(outDir) / "job.json");
    o << job.dump(1) << "\n";
    if (!o) { err = "cannot write " + (fs::path(outDir) / "job.json").string(); return false; }
    return true;
}

} // namespace wl
