// DAWproject export: a job as a project a DAW opens with its plugins loaded (see dawproject.hpp).
#include "dawproject.hpp"

#include "audio_file.hpp"
#include "catalog.hpp"
#include "engine.hpp"
#include "job.hpp"
#include "platform.hpp"
#include "zip.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {

std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

std::string slug(const std::string &s) {
    std::string o;
    for (char c : s) o += std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '-';
    return o;
}

bool isBuiltinSpec(const std::string &p) { return p.rfind("builtin:", 0) == 0; }

// what a state worker reports
struct SavedState { bool ok = false; std::string id, name, vendor, format, file, error; };

// One plugin's state from a `__save-state` worker (crash-isolated like render workers).
SavedState saveState(const std::string &jobPath, const std::string &where, const std::string &prefix) {
    SavedState s;
    platform::Process p;
    if (!platform::spawn({platform::selfExecutable(), "__save-state", jobPath, where, prefix}, p, true, true)) { s.error = "cannot start a worker"; return s; }
    std::string outText;
    if (!platform::readOutput(p, outText, 180)) { platform::kill(p); s.error = "the plugin did not finish within 3 minutes"; return s; }
    std::string crash;
    for (int i = 0; i < 200 && !platform::finished(p, crash); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const json r = json::parse(outText, nullptr, false);
    if (r.is_discarded() || !r.value("ok", false)) {
        s.error = r.is_object() && r.contains("error") ? r["error"].get<std::string>() : crash.empty() ? "the worker failed" : "the plugin crashed (" + crash + ")";
        return s;
    }
    s.ok = true;
    s.id = r.value("id", ""); s.name = r.value("name", ""); s.vendor = r.value("vendor", "");
    s.format = r.value("format", ""); s.file = r.value("file", "");
    return s;
}

struct Writer {
    std::ostringstream x;
    int next = 0;
    std::string id() { return "id" + std::to_string(next++); }
};

} // namespace

int saveStateWorker(const std::string &jobPath, const std::string &where, const std::string &prefix, std::FILE *out) {
    auto fail = [&](const std::string &e) { std::fprintf(out, "%s\n", json{{"ok", false}, {"error", e}}.dump().c_str()); return 1; };
    std::ifstream in(jobPath);
    const json j = in ? json::parse(in, nullptr, false) : json();
    Job job;
    std::string err;
    if (j.is_discarded() || !parseJob(j, fs::absolute(jobPath).parent_path().string(), job, err)) return fail("cannot read job: " + err);
    // where: "track:<i>" (instrument), "track:<i>:fx:<k>", "bus:<i>:fx:<k>", "master:fx:<k>"
    std::vector<std::string> part;
    for (std::stringstream ss(where); ss.good();) { std::string t; std::getline(ss, t, ':'); part.push_back(t); }
    PluginSetup setup;
    try {
        if (part.size() == 2 && part[0] == "track") {
            const Track &t = job.tracks.at(std::stoul(part[1]));
            setup.spec = t.plugin; setup.stateFile = t.stateFile; setup.stateFormat = t.stateFormat;
            setup.params = t.params; setup.preset = t.preset; setup.warmup = t.warmup;
        } else if (part.size() == 4 && part[0] == "track") setup = pluginEffectSetup(job.tracks.at(std::stoul(part[1])).fx.at(std::stoul(part[3])), job);
        else if (part.size() == 4 && part[0] == "bus") setup = pluginEffectSetup(job.buses.at(std::stoul(part[1])).fx.at(std::stoul(part[3])), job);
        else if (part.size() == 3 && part[0] == "master") setup = pluginEffectSetup(job.masterFx.at(std::stoul(part[2])), job);
        else return fail("bad target " + where);
    } catch (const std::exception &) { return fail("no plugin at " + where); }
    setup.automation.clear();   // the state is the starting point; curves stay in the job
    OpenedPlugin p;
    if (!openPlugin(setup, where, p, err)) return fail(err);
    if (!p.initial.empty() && !p.plugin->commitParams(p.initial, job.sampleRate, (uint32_t)job.blockSize, err)) return fail(err);
    p.plugin->pump(100);
    const std::string ext = p.format == "vst3" ? ".vstpreset" : p.format == "vst2" ? ".fxp" : ".clap-preset";
    size_t bytes = 0;
    if (!p.plugin->saveStateFile(prefix + ext, bytes, err)) return fail(err);
    PluginInfo info;
    std::string e2;
    resolvePlugin(setup.spec, info, e2);
    std::fprintf(out, "%s\n", json{{"ok", true}, {"id", p.id}, {"name", p.name}, {"vendor", info.vendor}, {"format", p.format}, {"file", prefix + ext}}.dump().c_str());
    return 0;
}

bool exportDawproject(const Job &job, const json &raw, const std::string &jobPath, const std::string &outPath, DawprojectExport &res, std::string &err) {
    const std::string tmp = (fs::temp_directory_path() / ("wavelength-export-" + std::to_string(platform::processId()))).string();
    std::error_code ec;
    fs::create_directories(tmp, ec);
    struct Cleanup { std::string d; ~Cleanup() { std::error_code e; fs::remove_all(d, e); } } cleanup{tmp};
    ZipWriter zip;
    Writer w;
    std::set<std::string> usedFiles;
    int stateCount = 0;

    // a device for the plugin at `where`, with its state in plugins/ ("" when it can't be saved)
    auto device = [&](const std::string &where, const std::string &label, const std::string &role) -> std::string {
        const SavedState s = saveState(fs::absolute(jobPath).string(), where, (fs::path(tmp) / ("state-" + std::to_string(stateCount++))).string());
        if (!s.ok) { res.notes.push_back(label + ": no state saved (" + s.error + "); left out"); return ""; }
        std::ifstream f(s.file, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string name = "plugins/" + slug(label) + fs::path(s.file).extension().string();
        for (int k = 2; usedFiles.count(name); ++k) name = "plugins/" + slug(label) + "-" + std::to_string(k) + fs::path(s.file).extension().string();
        usedFiles.insert(name);
        zip.add(name, std::move(bytes));
        ++res.plugins;
        std::string tag = "ClapPlugin", deviceId = s.id;
        if (s.format == "vst3") tag = "Vst3Plugin";
        else if (s.format == "vst2") {   // the four-character code as a decimal number
            tag = "Vst2Plugin";
            uint32_t v = 0;
            for (char c : s.id.substr(0, 4)) v = v << 8 | (uint8_t)c;
            deviceId = std::to_string((int32_t)v);
        }
        std::ostringstream d;
        d << "<" << tag << " id=\"" << w.id() << "\" deviceID=\"" << esc(deviceId) << "\" deviceName=\"" << esc(s.name) << "\" deviceVendor=\""
          << esc(s.vendor) << "\" deviceRole=\"" << role << "\" loaded=\"true\" name=\"" << esc(s.name) << "\">"
          << "<Enabled id=\"" << w.id() << "\" name=\"On/Off\" value=\"true\"/><State path=\"" << esc(name) << "\"/></" << tag << ">";
        return d.str();
    };
    auto param = [&](const std::string &tag, const std::string &unit, double v) {
        return "<" + tag + " id=\"" + w.id() + "\" name=\"" + tag + "\" unit=\"" + unit + "\" value=\"" + num(v) + "\"/>";
    };
    // a built-in eq, compressor or limiter as the DAWproject standard device ("" for the others)
    auto standardDevice = [&](const json &e) -> std::string {
        const std::string t = e.value("type", "");
        const std::string head = " id=\"" + w.id() + "\" deviceRole=\"audioFX\" loaded=\"true\"";
        if (t == "eq" && e.contains("bands") && e["bands"].is_array()) {
            std::string bands;
            for (auto &b : e["bands"]) {
                const std::string bt = b.value("type", "peak");
                const std::string type = bt == "highpass" ? "highPass" : bt == "lowpass" ? "lowPass" : bt == "bandpass" ? "bandPass" :
                                         bt == "lowshelf" ? "lowShelf" : bt == "highshelf" ? "highShelf" : "bell";
                bands += "<Band type=\"" + type + "\">" + param("Freq", "hertz", b.value("freq", 1000.0)) +
                         (type == "bell" || type == "lowShelf" || type == "highShelf" ? param("Gain", "decibel", b.value("gain", 0.0)) : "") +
                         param("Q", "linear", b.value("q", 0.707)) + "</Band>";
            }
            return "<Equalizer" + head + " deviceName=\"EQ\" name=\"EQ\"><Enabled id=\"" + w.id() + "\" value=\"true\"/>" + bands + "</Equalizer>";
        }
        if (t == "compressor" && !e.contains("sidechain"))
            return "<Compressor" + head + " deviceName=\"Compressor\" name=\"Compressor\"><Enabled id=\"" + w.id() + "\" value=\"true\"/>" +
                   param("Attack", "seconds", e.value("attack", 10.0) / 1000) + param("OutputGain", "decibel", e.value("makeup", 0.0)) +
                   param("Ratio", "linear", e.value("ratio", 3.0)) + param("Release", "seconds", e.value("release", 150.0) / 1000) +
                   param("Threshold", "decibel", e.value("threshold", -18.0)) + "</Compressor>";
        if (t == "limiter")
            return "<Limiter" + head + " deviceName=\"Limiter\" name=\"Limiter\"><Enabled id=\"" + w.id() + "\" value=\"true\"/>" +
                   param("Release", "seconds", e.value("release", 80.0) / 1000) + param("Threshold", "decibel", e.value("ceiling", -1.0)) + "</Limiter>";
        return "";
    };
    // plugin effects and built-in eq/compressor/limiter of a chain; other built-in effects have no DAW counterpart
    auto chainDevices = [&](const json &fx, const std::string &prefix, const std::string &owner) {
        std::string out;
        std::vector<std::string> builtin;
        for (size_t k = 0; k < fx.size(); ++k) {
            if (fx[k].contains("plugin")) { out += device(prefix + ":fx:" + std::to_string(k), owner + " " + fx[k].value("plugin", "fx"), "audioFX"); continue; }
            if (fx[k].value("automate", json::object()).size() || fx[k].contains("lfo")) { builtin.push_back(fx[k].value("type", "?") + " (automated)"); continue; }
            const std::string d = standardDevice(fx[k]);
            if (!d.empty()) out += d;
            else builtin.push_back(fx[k].value("type", "?") + (fx[k].value("type", "") == "compressor" ? " (sidechain)" : ""));
        }
        if (!builtin.empty()) {
            std::string l;
            for (auto &b : builtin) l += (l.empty() ? "" : ", ") + b;
            res.notes.push_back(owner + ": built-in effects (" + l + ") have no DAW counterpart; left out");
        }
        return out;
    };
    auto volume = [&](double db) { return "<Volume id=\"" + w.id() + "\" name=\"Volume\" unit=\"linear\" min=\"0\" max=\"2\" value=\"" + num(std::pow(10.0, db / 20)) + "\"/>"; };

    const std::string masterId = w.id(), masterChannel = w.id();
    std::map<std::string, std::string> busChannel;   // bus name -> channel id
    std::vector<std::string> busTrackIds;
    for (auto &b : job.buses) { busTrackIds.push_back(w.id()); busChannel[b.name] = w.id(); }

    std::ostringstream structure, lanes;
    // tracks
    const double songEnd = [&] {
        double e = 0;
        for (auto &t : job.tracks) for (auto &n : t.notes) e = std::max(e, job.tempo.secToBeat(n.start + n.length));
        return std::ceil(std::max(e, 4.0) / 4) * 4;
    }();
    for (size_t i = 0; i < job.tracks.size(); ++i) {
        const Track &t = job.tracks[i];
        const std::string trackId = w.id(), channelId = w.id();
        const bool audio = t.plugin == "builtin:audio";
        std::string devices;
        if (!isBuiltinSpec(t.plugin)) devices = device("track:" + std::to_string(i), t.name, "instrument");
        else if (!audio) res.notes.push_back(t.name + ": " + t.plugin + " has no DAW counterpart; the notes come across on an empty instrument track");
        devices += chainDevices(t.fx, "track:" + std::to_string(i), t.name);
        const std::string dest = !t.output.empty() && busChannel.count(t.output) ? busChannel[t.output] : masterChannel;
        const std::string volId = w.id(), panId = w.id();
        structure << "<Track id=\"" << trackId << "\" name=\"" << esc(t.name) << "\" contentType=\"" << (audio ? "audio" : "notes") << "\" loaded=\"true\">"
                  << "<Channel id=\"" << channelId << "\" role=\"regular\" audioChannels=\"2\" destination=\"" << dest << "\" solo=\"false\">";
        if (!devices.empty()) structure << "<Devices>" << devices << "</Devices>";
        structure << "<Mute id=\"" << w.id() << "\" name=\"Mute\" value=\"" << (t.mute ? "true" : "false") << "\"/>"
                  << "<Pan id=\"" << panId << "\" name=\"Pan\" unit=\"normalized\" min=\"0\" max=\"1\" value=\"" << num((t.pan + 1) / 2) << "\"/>";
        if (!t.sends.empty()) {
            structure << "<Sends>";
            for (auto &[bus, db] : t.sends)
                if (busChannel.count(bus))
                    structure << "<Send id=\"" << w.id() << "\" destination=\"" << busChannel[bus] << "\" type=\"post\"><Volume id=\"" << w.id()
                              << "\" name=\"Send\" unit=\"linear\" min=\"0\" max=\"1\" value=\"" << num(std::min(1.0, std::pow(10.0, db / 20))) << "\"/></Send>";
            structure << "</Sends>";
        }
        structure << "<Volume id=\"" << volId << "\" name=\"Volume\" unit=\"linear\" min=\"0\" max=\"2\" value=\"" << num(std::pow(10.0, t.gainDb / 20)) << "\"/>"
                  << "</Channel></Track>";
        if (!t.sendAutomation.empty()) res.notes.push_back(t.name + ": send automation left out (static send levels kept)");

        // arrangement: one clip of the song's length holding every note, plus fader and pan curves
        lanes << "<Lanes id=\"" << w.id() << "\" track=\"" << trackId << "\">";
        if (!t.notes.empty()) {
            lanes << "<Clips id=\"" << w.id() << "\"><Clip time=\"0\" duration=\"" << num(songEnd) << "\" playStart=\"0\"><Notes id=\"" << w.id() << "\">";
            for (auto &n : t.notes) {
                const double b = job.tempo.secToBeat(n.start), e = job.tempo.secToBeat(n.start + n.length);
                lanes << "<Note time=\"" << num(b) << "\" duration=\"" << num(std::max(1e-3, e - b)) << "\" channel=\"" << n.channel << "\" key=\"" << n.key
                      << "\" vel=\"" << num(n.velocity) << "\" rel=\"" << num(n.velocity) << "\"/>";
                ++res.noteCount;
            }
            lanes << "</Notes></Clip></Clips>";
        }
        if (audio) {   // audio clips: files played as they are (trimmed); stretched, pitched, reversed or rendered clips are left out
            std::string clips;
            size_t skipped = 0;
            for (auto &c : t.clips) {
                if (!c.contains("file") || !c["file"].is_string() || c.contains("bpm") || c.contains("pitch") || c.value("reverse", false) || c.contains("endAt")) { ++skipped; continue; }
                std::string src = c["file"].get<std::string>();
                if (fs::path(src).is_relative()) src = (fs::path(job.baseDir) / src).string();
                Audio a;
                int sr = 0;
                std::string e2;
                if (!readAudio(src, a, sr, e2)) { ++skipped; continue; }
                const double fileSec = (double)a.frames() / sr, start = c.value("start", 0.0);
                const double len = c.contains("length") ? c["length"].get<double>() : fileSec - start;
                const double b0 = c.value("beat", 0.0), b1 = job.tempo.secToBeat(job.tempo.beatToSec(b0) + len);
                std::string name = "audio/" + fs::path(src).filename().string();
                if (!usedFiles.count(name)) {
                    std::ifstream f(src, std::ios::binary);
                    zip.add(name, std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
                    usedFiles.insert(name);
                }
                clips += "<Clip time=\"" + num(b0) + "\" duration=\"" + num(b1 - b0) + "\" contentTimeUnit=\"seconds\" playStart=\"" + num(start) +
                         "\" playStop=\"" + num(start + len) + "\"><Audio id=\"" + w.id() + "\" channels=\"2\" sampleRate=\"" + std::to_string(sr) +
                         "\" duration=\"" + num(fileSec) + "\"><File path=\"" + esc(name) + "\"/></Audio></Clip>";
            }
            if (!clips.empty()) lanes << "<Clips id=\"" << w.id() << "\">" << clips << "</Clips>";
            if (skipped) res.notes.push_back(t.name + ": " + std::to_string(skipped) + " audio clip(s) that stretch, pitch, reverse or render left out");
        }
        auto curve = [&](const std::string &param, const std::string &unit, auto value) {   // sampled every 1/4 beat, changes only
            lanes << "<Points id=\"" << w.id() << "\" unit=\"" << unit << "\"><Target parameter=\"" << param << "\"/>";
            double last = NAN;
            for (double b = 0; b <= songEnd + 1e-9; b += 0.25) {
                const double v = value(job.tempo.beatToSec(b));
                if (std::isnan(last) || std::fabs(v - last) > 1e-4 || b + 0.25 > songEnd) lanes << "<RealPoint time=\"" << num(b) << "\" value=\"" << num(v) << "\" interpolation=\"linear\"/>";
                last = v;
            }
            lanes << "</Points>";
        };
        if (!t.gainAutomation.empty()) curve(volId, "linear", [&](double s) { return std::pow(10.0, (t.gainDb + t.gainAutomation.at(s)) / 20); });
        if (!t.panAutomation.empty()) curve(panId, "normalized", [&](double s) { return (std::clamp(t.panAutomation.at(s), -1.0, 1.0) + 1) / 2; });
        if (!t.paramAutomation.empty()) res.notes.push_back(t.name + ": plugin parameter automation left out");
        lanes << "</Lanes>";
        ++res.tracks;
    }
    // buses as effect tracks (sends and track outputs reach them)
    for (size_t b = 0; b < job.buses.size(); ++b) {
        const Bus &bus = job.buses[b];
        const std::string devices = chainDevices(bus.fx, "bus:" + std::to_string(b), bus.name);
        const std::string dest = !bus.output.empty() && busChannel.count(bus.output) ? busChannel[bus.output] : masterChannel;
        structure << "<Track id=\"" << busTrackIds[b] << "\" name=\"" << esc(bus.name) << "\" contentType=\"audio\" loaded=\"true\">"
                  << "<Channel id=\"" << busChannel[bus.name] << "\" role=\"effect\" audioChannels=\"2\" destination=\"" << dest << "\" solo=\"false\">";
        if (!devices.empty()) structure << "<Devices>" << devices << "</Devices>";
        structure << "<Mute id=\"" << w.id() << "\" name=\"Mute\" value=\"false\"/><Pan id=\"" << w.id()
                  << "\" name=\"Pan\" unit=\"normalized\" min=\"0\" max=\"1\" value=\"0.500000\"/>" << volume(bus.gainDb) << "</Channel></Track>";
        if (!bus.gainAutomation.empty()) res.notes.push_back("bus " + bus.name + ": gain automation left out");
        ++res.buses;
    }
    // master
    {
        const std::string devices = chainDevices(job.masterFx, "master", "master");
        structure << "<Track id=\"" << masterId << "\" name=\"Master\" contentType=\"audio notes\" loaded=\"true\">"
                  << "<Channel id=\"" << masterChannel << "\" role=\"master\" audioChannels=\"2\" solo=\"false\">";
        if (!devices.empty()) structure << "<Devices>" << devices << "</Devices>";
        structure << "<Mute id=\"" << w.id() << "\" name=\"Mute\" value=\"false\"/><Pan id=\"" << w.id()
                  << "\" name=\"Pan\" unit=\"normalized\" min=\"0\" max=\"1\" value=\"0.500000\"/>" << volume(job.masterGainDb) << "</Channel></Track>";
        if (raw.contains("normalize")) res.notes.push_back("\"normalize\" (the render's peak normalizing) has no DAW counterpart");
        if (raw.contains("master") && raw["master"].is_object() && raw["master"].contains("loudness"))
            res.notes.push_back("master: the loudness target has no DAW counterpart (the master fader stays at its own gain)");
    }

    // transport and tempo map
    const std::string tempoId = w.id();
    const json tj = raw.value("tempo", json(120));
    double bpm0 = 120;
    std::ostringstream tempoAuto;
    if (tj.is_number()) bpm0 = tj.get<double>();
    else if (tj.is_array() && !tj.empty()) {
        bpm0 = tj[0].value("bpm", 120.0);
        if (tj.size() > 1) {
            tempoAuto << "<TempoAutomation id=\"" << w.id() << "\" unit=\"bpm\"><Target parameter=\"" << tempoId << "\"/>";
            double prevBpm = bpm0;
            for (auto &p : tj) {
                const double beat = p.contains("bar") ? (p["bar"].get<double>() - 1) * job.tsigNum * 4.0 / job.tsigDen : p.value("beat", 0.0);
                const double v = p.value("bpm", prevBpm);
                // a step holds the previous tempo up to the point; a ramp slides to it
                if (!p.value("ramp", false) && beat > 0) tempoAuto << "<RealPoint time=\"" << num(beat) << "\" value=\"" << num(prevBpm) << "\" interpolation=\"hold\"/>";
                tempoAuto << "<RealPoint time=\"" << num(beat) << "\" value=\"" << num(v) << "\" interpolation=\"" << (p.value("ramp", false) ? "linear" : "hold") << "\"/>";
                prevBpm = v;
            }
            tempoAuto << "</TempoAutomation>";
        }
    }
    std::ostringstream markers;
    if (!job.markers.empty()) {
        markers << "<Markers id=\"" << w.id() << "\">";
        for (auto &m : job.markers) markers << "<Marker time=\"" << num(m.beat) << "\" name=\"" << esc(m.name) << "\"/>";
        markers << "</Markers>";
    }
    std::ostringstream project;
    project << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Project version=\"1.0\">"
            << "<Application name=\"Wavelength\" version=\"" << WAVELENGTH_VERSION << "\"/>"
            << "<Transport><Tempo id=\"" << tempoId << "\" name=\"Tempo\" unit=\"bpm\" min=\"20\" max=\"666\" value=\"" << num(bpm0) << "\"/>"
            << "<TimeSignature id=\"" << w.id() << "\" name=\"Time Signature\" numerator=\"" << job.tsigNum << "\" denominator=\"" << job.tsigDen << "\"/></Transport>"
            << "<Structure>" << structure.str() << "</Structure>"
            << "<Arrangement id=\"" << w.id() << "\"><Lanes id=\"" << w.id() << "\" timeUnit=\"beats\">" << lanes.str() << "</Lanes>"
            << markers.str() << tempoAuto.str() << "</Arrangement></Project>\n";
    const std::string title = raw.value("title", fs::path(jobPath).parent_path().filename().string());
    zip.add("project.xml", project.str());
    zip.add("metadata.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<MetaData><Title>" + esc(title) +
                                "</Title><Comment>Exported from Wavelength</Comment></MetaData>\n");
    return zip.write(outPath, err);
}

} // namespace wl
