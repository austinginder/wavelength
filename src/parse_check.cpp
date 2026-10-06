// `wavelength __parse <format> <file>...`: runs one file reader over each file and prints a JSON line
// per file ({"file", "ok", "error"}). Internal: the regression checks feed it broken and hostile files,
// and scripts/fuzz.py feeds it mutated ones under a sanitizer build. A reader may refuse a file or throw
// (both are answers); it must never crash, read outside its buffer or hang.
#include "parse_check.hpp"

#include "apple_loops.hpp"
#include "audio_file.hpp"
#include "bitwig.hpp"
#include "bplist.hpp"
#include "dawproject.hpp"
#include "decent_sampler.hpp"
#include "garageband.hpp"
#include "logic_patches.hpp"
#include "midi_file.hpp"
#include "musicxml.hpp"
#include "preset_formats.hpp"
#include "retro_synth.hpp"
#include "sampler.hpp"
#include "sf2.hpp"
#include "sfz.hpp"
#include "state_file.hpp"
#include "xml.hpp"
#include "zip.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <chrono>
#include <map>

namespace fs = std::filesystem;
using nlohmann::json;

namespace wl {

namespace {
bool bytesOf(const std::string &path, std::vector<uint8_t> &out, std::string &err) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

using Reader = std::function<bool(const std::string &path, const std::string &tmp, std::string &err)>;

const std::map<std::string, Reader> &readers() {
    static const std::map<std::string, Reader> r = {
        {"bplist", [](const std::string &p, const std::string &, std::string &err) {
             std::vector<uint8_t> d;
             if (!bytesOf(p, d, err)) return false;
             json a, b;
             const bool plain = parseBinaryPlist(d.data(), d.size(), a, true, true);
             const bool archive = parseKeyedArchive(d.data(), d.size(), b);
             if (!plain && !archive) err = "not a binary plist";
             return plain || archive;
         }},
        {"zip", [](const std::string &p, const std::string &, std::string &err) {
             Zip z;
             if (!z.open(p, err)) return false;
             for (auto &name : z.names()) {
                 std::vector<uint8_t> d;
                 std::string e;
                 if (!z.read(name, d, e)) { err = e; return false; }
             }
             return true;
         }},
        {"xml", [](const std::string &p, const std::string &, std::string &err) {
             std::vector<uint8_t> d;
             if (!bytesOf(p, d, err)) return false;
             return xml::parse(std::string(d.begin(), d.end()), err) != nullptr;
         }},
        {"serum", [](const std::string &p, const std::string &, std::string &err) {
             std::vector<uint8_t> d, proc, ctrl;
             return bytesOf(p, d, err) && serumPresetToStates(d, proc, ctrl, err);
         }},
        {"valuetree", [](const std::string &p, const std::string &, std::string &err) {
             std::vector<uint8_t> d, state;
             return bytesOf(p, d, err) && valueTreeToJuceXml(d, state, err);
         }},
        {"sf2", [](const std::string &p, const std::string &, std::string &err) {
             SoundFont sf;
             if (!sf.open(p, err)) return false;
             for (auto &pr : sf.presets()) {
                 std::vector<Sf2Zone> zones;
                 std::string e;
                 sf.zones(pr, zones, e);
             }
             return true;
         }},
        {"audio", [](const std::string &p, const std::string &, std::string &err) {
             std::vector<uint8_t> d;
             DecodedAudio a;
             return bytesOf(p, d, err) && decodeAudio(d.data(), d.size(), a, err);
         }},
        {"sfz", [](const std::string &p, const std::string &, std::string &err) {
             SfzFile s;
             return parseSfz(p, s, err);
         }},
        {"exs", [](const std::string &p, const std::string &, std::string &err) {
             std::vector<std::string> files;
             return exsSampleFiles(p, files, err);
         }},
        {"dspreset", [](const std::string &p, const std::string &, std::string &err) {
             DecentPreset d;
             return readDecentPreset(p, d, err);
         }},
        {"caf", [](const std::string &p, const std::string &, std::string &err) {
             AppleLoop loop;
             std::vector<uint8_t> d, smf;
             const bool tags = readAppleLoop(p, loop, err);
             if (!bytesOf(p, d, err)) return false;
             std::string e;
             cafMidi(d, smf, e);
             GarageBandSynth strip;
             appleLoopStripSynth(d, strip);
             return tags;
         }},
        {"patch", [](const std::string &p, const std::string &, std::string &err) {   // a .patch folder
             std::vector<PatchChannel> chans;
             const bool ok = readPatchChannels(p, chans, err);
             smartControls(p);
             GarageBandSynth synth;
             std::string why;
             garageBandSynthPatch(p, synth, &why);
             return ok;
         }},
        {"state", [](const std::string &p, const std::string &, std::string &err) {
             StateFile s;
             return readStateFile(p, "auto", s, err);
         }},
        {"midi", [](const std::string &p, const std::string &tmp, std::string &err) {
             MidiImport m;
             return importMidiFile(p, tmp, "", m, err);
         }},
        {"musicxml", [](const std::string &p, const std::string &tmp, std::string &err) {
             MusicXmlImport m;
             return importMusicXml(p, tmp, "", m, err);
         }},
        {"dawproject", [](const std::string &p, const std::string &tmp, std::string &err) {
             DawprojectImport d;
             return importDawproject(p, tmp, d, err, "none");
         }},
        {"bwproject", [](const std::string &p, const std::string &, std::string &err) {
             bitwig::Project proj;
             return bitwig::load(p, proj, err);
         }},
        {"band", [](const std::string &p, const std::string &tmp, std::string &err) {   // a .band folder
             DawprojectImport d;
             return importGarageBand(p, tmp, d, err, true, false);
         }},
    };
    return r;
}
} // namespace

int parseCheck(const std::vector<std::string> &args, std::FILE *out) {
    if (args.size() < 2) {
        std::string names;
        for (auto &[k, _] : readers()) names += (names.empty() ? "" : ", ") + k;
        std::fprintf(out, "{\"ok\": false, \"error\": \"usage: __parse <format> <file>...; formats: %s\"}\n", names.c_str());
        return 2;
    }
    const auto it = readers().find(args[0]);
    if (it == readers().end()) {
        std::fprintf(out, "%s\n", json{{"ok", false}, {"error", "unknown format " + args[0]}}.dump().c_str());
        return 2;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path tmp = fs::temp_directory_path() / ("wl-parse-" + std::to_string(stamp));
    for (size_t i = 1; i < args.size(); ++i) {
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        std::string err;
        bool ok = false;
        try {
            ok = it->second(args[i], tmp.string(), err);
        } catch (const std::exception &e) {
            err = std::string("threw: ") + e.what();
        }
        json line = {{"file", args[i]}, {"ok", ok}};
        if (!ok) line["error"] = err;
        std::fprintf(out, "%s\n", line.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
        std::fflush(out);
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return 0;
}

} // namespace wl
