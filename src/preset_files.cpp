#include "preset_files.hpp"

#include "kit.hpp"
#include "platform.hpp"

#include "preset_formats.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace wl {

namespace {

std::string squash(std::string s) {   // "Serum 2" == "serum2", "Odin2" == "odin2"
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

const std::set<std::string> kExtensions = {".vstpreset", ".fxp", ".fxb", ".serumpreset", ".odin", ".h2p", ".vital", ".nksf", ".synplant",
                                           ".dco106preset", ".mg1preset", ".sempreset", ".voltagepreset", ".ngrr", ".mtpreset", ".mtdrum", ".wlstate", ".sbset", ".dspreset",
                                           ".hxp", ".echobode", ".serumfx", ".serumfxrack", ".tide", ".srgfx", ".preset"};

// a child folder of `dir` whose squashed name is one of `names`
std::vector<fs::path> childrenNamed(const fs::path &dir, const std::vector<std::string> &names) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string n = squash(e.path().filename().string());
        if (std::find(names.begin(), names.end(), n) != names.end()) out.push_back(e.path());
    }
    return out;
}

// a preset folder can hold other plugins' files (a Diva .h2p saved among Serum presets): keep a
// file only when its format can belong to this plugin
bool belongsTo(const fs::path &file, const std::string &ext, const PluginInfo &plugin) {
    const std::string p = squash(plugin.name);
    if (ext == ".serumpreset") return p.find("serum") != std::string::npos;
    if (ext == ".odin") return p.find("odin") != std::string::npos;
    if (ext == ".synplant") return p.find("synplant") != std::string::npos;
    if (ext == ".dco106preset") return p == "dco106";
    if (ext == ".mg1preset") return p.find("mg1") != std::string::npos;
    if (ext == ".sempreset") return p == "synthesizerexpandermodule";
    if (ext == ".voltagepreset") return p == "voltagemodular";
    if (ext == ".ngrr") return p.find("guitarrig") != std::string::npos;
    if (ext == ".mtpreset" || ext == ".mtdrum") return p.find("microtonic") != std::string::npos;
    if (ext == ".sbset") return p == "soundbox";
    if (ext == ".dspreset") return p == "decentsampler";
    if (ext == ".hxp") return p == "helix";
    if (ext == ".echobode") return p == "echobode";
    if (ext == ".tide") return p == "pendulate";
    if (ext == ".srgfx") return p == "surgexteffects";
    if (ext == ".preset") {   // HISE user presets (other plugins use .preset for their own formats), in <Product>/User Presets/
        bool ours = false;
        for (fs::path d = file.parent_path(); !d.empty() && d != d.parent_path(); d = d.parent_path())
            if (d.filename() == "User Presets") { ours = squash(d.parent_path().filename().string()) == p; break; }
        if (!ours) return false;
        std::ifstream in(file, std::ios::binary);
        std::vector<uint8_t> head(512);
        in.read(reinterpret_cast<char *>(head.data()), (std::streamsize)head.size());
        head.resize((size_t)in.gcount());
        return isHisePreset(head);
    }
    if (ext == ".serumfx" || ext == ".serumfxrack") return p == "serum2fx";
    if (ext == ".fxp") {   // Serum 1 patches (fxID "XfsX") turn up in Serum 2's folders; Serum 2 can't load them
        std::ifstream in(file, std::ios::binary);
        char head[20] = {};
        in.read(head, sizeof head);
        if (std::memcmp(head, "CcnK", 4) == 0 && std::memcmp(head + 16, "XfsX", 4) == 0) return p == "serum";
        return true;
    }
    if (ext != ".h2p" && ext != ".vstpreset") return true;
    std::ifstream in(file, std::ios::binary);
    std::string head(4096, '\0');
    in.read(&head[0], (std::streamsize)head.size());
    head.resize((size_t)in.gcount());
    if (ext == ".vstpreset")   // "VST3" + version + the 32-character class id of the plugin it belongs to
        return head.size() >= 40 && squash(head.substr(8, 32)) == squash(plugin.id);
    const size_t am = head.find("#AM=");   // u-he: the plugin the preset was saved by
    if (am == std::string::npos) return true;
    const size_t end = head.find_first_of("\r\n", am);
    return squash(head.substr(am + 4, end - am - 4)) == p;
}

// DecentSampler's own settings name the folder its libraries are installed to ("sampleLibraryDirectory")
std::vector<fs::path> decentSamplerLibraries() {
    const std::string home = platform::homeDir().string();
    std::vector<fs::path> out;
    for (const fs::path &settings : {fs::path(home) / "Music/Audio Music Apps/Decidedly/DecentSampler/DecentSampler.xml",
                                    fs::path(getenv("APPDATA") ? getenv("APPDATA") : home) / "Decidedly/DecentSampler/DecentSampler.xml",
                                    fs::path(home) / ".config/Decidedly/DecentSampler/DecentSampler.xml"}) {
        std::ifstream in(settings, std::ios::binary);
        if (!in) continue;
        const std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = "name=\"sampleLibraryDirectory\" val=\"";
        const size_t at = xml.find(key);
        if (at == std::string::npos) continue;
        std::string dir = xml.substr(at + key.size(), xml.find('"', at + key.size()) - at - key.size());
        for (size_t amp; (amp = dir.find("&amp;")) != std::string::npos;) dir.replace(amp, 5, "&");
        if (!dir.empty()) out.push_back(dir);
    }
    return out;
}

// ---- NKS presets: Native Instruments' RIFF "NIKS" files name the plugin they belong to --------
struct NksEntry { std::string path, name, category, vendor, bank, magic, uid; };

std::string cachePath() {
    return (platform::cacheDir() / "nks-v2.json").string();
}

bool readNks(const std::string &path, NksEntry &e) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> d(65536);
    in.read(reinterpret_cast<char *>(d.data()), (std::streamsize)d.size());
    d.resize((size_t)in.gcount());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "NIKS", 4)) return false;
    e.path = path;
    for (size_t i = 12; i + 8 <= d.size();) {
        const std::string id(d.begin() + (long)i, d.begin() + (long)i + 4);
        const uint32_t n = d[i + 4] | d[i + 5] << 8 | d[i + 6] << 16 | (uint32_t)d[i + 7] << 24;
        if (id == "PCHK") {
            // SynthMaster: the payload's kind says which plugin's bank it names, whatever PLID says
            // (the Player's NKS folder carries SynthMaster One presets too)
            if (i + 8 + 28 <= d.size() && std::memcmp(&d[i + 16], "133k", 4) == 0) {
                if (std::memcmp(&d[i + 20], "p1ms", 4) == 0) e.magic = "536D3169";        // 'Sm1i'
                else if (std::memcmp(&d[i + 20], "lpms", 4) == 0) e.magic = "536D7069";   // 'Smpi'
            }
            break;
        }
        if (i + 8 + n > d.size()) break;   // metadata chunks come first
        if ((id == "NISI" || id == "PLID") && n > 4) {
            const std::vector<uint8_t> body(d.begin() + (long)i + 12, d.begin() + (long)(i + 8 + n));
            const json m = json::from_msgpack(body, true, false);
            if (m.is_object() && id == "NISI") {
                e.name = m.value("name", "");
                e.vendor = m.value("vendor", "");
                if (m.contains("bankchain") && m["bankchain"].is_array() && !m["bankchain"].empty()) {
                    e.bank = m["bankchain"][0].is_string() ? m["bankchain"][0].get<std::string>() : "";
                    if (m["bankchain"].size() > 1 && m["bankchain"][1].is_string()) e.category = m["bankchain"][1].get<std::string>();
                }
                if (m.contains("types") && m["types"].is_array() && !m["types"].empty() && m["types"][0].is_array()) {
                    std::string t;
                    for (auto &x : m["types"][0]) if (x.is_string()) t += (t.empty() ? "" : "/") + x.get<std::string>();
                    if (!t.empty()) e.category = t;
                }
            } else if (m.is_object() && id == "PLID") {
                if (m.contains("VST.magic") && m["VST.magic"].is_number()) {
                    const uint32_t v = m["VST.magic"].get<uint32_t>();
                    char hex[9];
                    std::snprintf(hex, sizeof hex, "%08X", v);
                    e.magic = hex;
                }
                if (m.contains("VST3.uid") && m["VST3.uid"].is_array()) {
                    for (auto &x : m["VST3.uid"]) {
                        char hex[9];
                        std::snprintf(hex, sizeof hex, "%08X", x.get<uint32_t>());
                        e.uid += hex;
                    }
                }
            }
        }
        i += 8 + n + (n & 1);
    }
    if (e.name.empty()) e.name = fs::path(path).stem().string();
    return true;
}

std::vector<NksEntry> scanNks() {
    std::vector<NksEntry> out;
    const std::string home = platform::homeDir().string();
    for (const auto &root : {home + "/Documents", std::string("/Users/Shared"), std::string("/Library/Application Support"),
                                   home + "/Library/Application Support", home + "/Spitfire", std::string("/Library/Audio/Presets"),
                                   home + "/Library/Audio/Presets"}) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it.depth() >= 7 && it->is_directory(ec)) { it.disable_recursion_pending(); continue; }
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            // .nksfx: effect presets (u-he Zebrify), .nksr: Reaktor Blocks racks; same container
            if ((ext != ".nksf" && ext != ".nksfx" && ext != ".nksr") || !it->is_regular_file(ec)) continue;
            NksEntry e;
            if (readNks(it->path().string(), e)) out.push_back(e);
        }
    }
    std::error_code ec;
    fs::create_directories(fs::path(cachePath()).parent_path(), ec);
    json j = json::array();
    for (auto &e : out) j.push_back({e.path, e.name, e.category, e.vendor, e.bank, e.magic, e.uid});
    std::string werr;
    platform::writeFileAtomic(cachePath(), j.dump(), werr);   // other processes read it meanwhile
    return out;
}

std::vector<NksEntry> &nksIndex(bool rescan) {
    static std::vector<NksEntry> index;
    static bool loaded = false;
    if (rescan) { index = scanNks(); loaded = true; return index; }
    if (loaded) return index;
    loaded = true;
    std::ifstream in(cachePath());
    const json j = in ? json::parse(in, nullptr, false) : json();
    if (!j.is_array()) { index = scanNks(); return index; }
    for (auto &x : j) index.push_back({x[0], x[1], x[2], x[3], x[4], x[5], x[6]});
    return index;
}

// NKS files name their plugin by VST2 id ("VST.magic", which VST3 versions of VST2 plugins embed
// in their class id), by VST3 uid, or only by bank name ("DUNE 3")
bool nksBelongsTo(const NksEntry &e, const PluginInfo &plugin) {
    std::string id = plugin.id;
    std::transform(id.begin(), id.end(), id.begin(), ::toupper);
    if (!e.uid.empty()) return e.uid == id;
    if (!e.magic.empty() && id.size() == 32) {
        if (id.substr(6, 8) == e.magic || id.substr(24, 8) == e.magic) return true;   // "VST" + magic..., or ...magic
    }
    return !e.bank.empty() && squash(e.bank) == squash(plugin.name);
}

} // namespace

std::vector<PresetInfo> nksPresets(const PluginInfo &plugin, bool rescan) {
    std::vector<PresetInfo> out;
    for (const auto &e : nksIndex(rescan)) {
        if (!nksBelongsTo(e, plugin)) continue;
        PresetInfo p;
        p.name = e.name;
        p.category = e.category.empty() ? e.bank : e.category;
        p.creator = e.vendor;
        p.location = p.loadKey = e.path;
        p.stateFile = true;
        out.push_back(p);
    }
    return out;
}

std::vector<PresetInfo> filePresets(const PluginInfo &plugin) {
    const std::string home = platform::homeDir().string();
    const std::string p = squash(plugin.name);
    const std::vector<std::string> names = {p, p + "presets", p + "patches"};
    std::vector<fs::path> dirs;
    // the VST3 convention: <Presets>/<Vendor>/<Plugin>/, plus <Presets>/<Plugin>/
    for (const auto &root : {fs::path("/Library/Audio/Presets"), fs::path(home) / "Library/Audio/Presets"}) {
        for (auto &d : childrenNamed(root, names)) dirs.push_back(d);
        std::error_code ec;
        for (auto &vendor : fs::directory_iterator(root, ec))
            if (vendor.is_directory(ec)) for (auto &d : childrenNamed(vendor.path(), names)) dirs.push_back(d);
    }
    // vendors that keep patches elsewhere
    if (p == "surgext") {
#if defined(__APPLE__)
        const fs::path surge = "/Library/Application Support/Surge XT";
#elif defined(_WIN32)
        const fs::path surge = fs::path(getenv("PROGRAMDATA") ? getenv("PROGRAMDATA") : "C:\\ProgramData") / "Surge XT";
#else
        const fs::path surge = fs::exists("/usr/share/surge-xt") ? "/usr/share/surge-xt" : "/usr/local/share/surge-xt";
#endif
        dirs.push_back(surge / "patches_factory");
        dirs.push_back(surge / "patches_3rdparty");
        dirs.push_back(kitDir() / "surge-xt" / "SurgeXTData" / "patches_factory");   // `wavelength kit`
        dirs.push_back(kitDir() / "surge-xt" / "SurgeXTData" / "patches_3rdparty");
    }
    if (p == "surgext") dirs.push_back(fs::path(home) / "Documents/Surge XT/Patches");
    if (p == "surgexteffects") {   // the effect presets Surge XT and Surge XT Effects share
#if defined(__APPLE__)
        dirs.push_back("/Library/Application Support/Surge XT/fx_presets");
#elif defined(_WIN32)
        dirs.push_back(fs::path(getenv("PROGRAMDATA") ? getenv("PROGRAMDATA") : "C:\\ProgramData") / "Surge XT/fx_presets");
#else
        dirs.push_back(fs::exists("/usr/share/surge-xt") ? "/usr/share/surge-xt/fx_presets" : "/usr/local/share/surge-xt/fx_presets");
#endif
        dirs.push_back(kitDir() / "surge-xt" / "SurgeXTData" / "fx_presets");
        dirs.push_back(fs::path(home) / "Documents/Surge XT/FX Presets");
    }
    if (p == "surge") {   // Surge 1.x, the synth before Surge XT: the same .fxp patches in its own folders
#if defined(__APPLE__)
        const fs::path surge = "/Library/Application Support/Surge";
#elif defined(_WIN32)
        const fs::path surge = fs::path(getenv("PROGRAMDATA") ? getenv("PROGRAMDATA") : "C:\\ProgramData") / "Surge";
#else
        const fs::path surge = "/usr/share/Surge";
#endif
        dirs.push_back(surge / "patches_factory");
        dirs.push_back(surge / "patches_3rdparty");
        dirs.push_back(fs::path(home) / "Documents/Surge");
    }
    // presets Wavelength extracted itself (scripts/extract-embedded-presets.py)
    dirs.push_back(platform::dataDir() / "Presets" / plugin.name);
    // Cherry Audio keeps presets in Application Support
    dirs.push_back(fs::path(home) / "Library/Application Support/CherryAudio" / plugin.name);
    if (p == "voltagemodular") dirs.push_back(fs::path(home) / "Library/Application Support/Voltage");
    if (p == "soundbox") dirs.push_back(fs::path(home) / "Library/Application Support/Audiomodern/Soundbox/Presets/Imported");
    if (p == "decentsampler") {
        dirs.push_back(fs::path(home) / "Documents/Decent Sampler");
        dirs.push_back(fs::path(home) / "Library/Application Support/DecentSampler");
        for (auto &d : decentSamplerLibraries()) dirs.push_back(d);
    }
    if (p.find("microtonic") != std::string::npos) {   // kits and drums; "By Category" gives drum categories
        dirs.push_back("/Library/Audio/Presets/Sonic Charge/Microtonic Presets");
        dirs.push_back("/Library/Audio/Presets/Sonic Charge/Microtonic Drum Patches");
    }
    if (p.find("guitarrig") != std::string::npos) {
        dirs.push_back("/Library/Application Support/Native Instruments/" + plugin.name + "/Rack Presets");
        dirs.push_back(fs::path(home) / "Documents/Native Instruments/User Content" / plugin.name / "Rack Presets");
    }
    if (p == "vital") {   // Vital's own library: factory banks, packs and user presets
        dirs.push_back(fs::path(home) / "Music/Vital");
        dirs.push_back(fs::path(home) / "Documents/Vital");
    }
    // HISE plugins keep user presets in <app data>/<Company>/<Product>/User Presets
    {
#if defined(__APPLE__)
        const fs::path appData = fs::path(home) / "Library/Application Support";
#elif defined(_WIN32)
        const fs::path appData = getenv("APPDATA") ? fs::path(getenv("APPDATA")) : fs::path(home);
#else
        const fs::path appData = fs::path(home) / ".config";
#endif
        std::error_code ec;
        for (auto &company : fs::directory_iterator(appData, ec)) {
            if (!company.is_directory(ec)) continue;
            const fs::path presets = company.path() / plugin.name / "User Presets";
            if (fs::is_directory(presets, ec)) dirs.push_back(presets);
        }
    }
    if (p == "pendulate")   // Newfangled Audio: the preset (.tide) is the plugin's state JSON
        for (const char *d : {"Documents", "Music"}) dirs.push_back(fs::path(home) / d / "Newfangled Audio/Pendulate/Presets");
    if (p == "serum2fx")   // Serum 2's effect presets and racks
        for (const fs::path &root : {fs::path("/Library/Audio/Presets"), fs::path(home) / "Library/Audio/Presets"})
            dirs.push_back(root / "Xfer Records/Serum 2 Presets/Effect Chains");
    if (p == "helix") {   // audjoo Helix: factory patches beside the plugin, user patches in Application Support
        for (const char *d : {"/Library/Audio/Plug-Ins/VST/audjoo_helix_data/patches", "/Library/Audio/Plug-Ins/VST3/audjoo_helix_data/patches"})
            dirs.push_back(d);
        dirs.push_back(fs::path(plugin.bundlePath).parent_path() / "audjoo_helix_data" / "patches");
        dirs.push_back(fs::path(home) / "Library/Audio/Plug-Ins/VST/audjoo_helix_data/patches");
        dirs.push_back(fs::path(home) / "Library/Application Support/audjoo_helix_userdata/patches");
    }
    if (p == "obxf") {
        dirs.push_back("/Library/Application Support/Surge Synth Team/OB-Xf/Patches");
        dirs.push_back(fs::path(home) / "Documents/Surge Synth Team/OB-Xf/Patches");
        dirs.push_back(kitDir() / "ob-xf" / "OB-Xf" / "Patches");   // `wavelength kit`
    }

    std::vector<PresetInfo> out;
    std::set<std::string> seen;
    for (const auto &dir : dirs) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (!kExtensions.count(ext) || !belongsTo(it->path(), ext, plugin)) continue;
            const std::string file = it->path().string();
            if (it->path().filename().string().rfind("._", 0) == 0 || file.find("/__MACOSX/") != std::string::npos) continue;   // macOS zip leftovers
            if (!seen.insert(file).second) continue;
            PresetInfo pi;
            pi.name = it->path().stem().string();
            pi.category = it->path().parent_path().filename().string();
            if ((ext == ".serumfx" || ext == ".serumfxrack") && (pi.category == "Factory" || pi.category == "User"))
                pi.category = it->path().parent_path().parent_path().filename().string();   // Effect Chains/<Delay>/Factory/
            if (ext == ".ngrr") {   // Guitar Rig: category from the rack's own tags, licence status in the description
                std::ifstream in(it->path(), std::ios::binary);
                const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const size_t us = raw.find("<user-set>RP://");
                if (us != std::string::npos) {
                    std::string tag = raw.substr(us + 15, raw.find("</user-set>", us) - us - 15);
                    std::replace(tag.begin(), tag.end(), '\t', '/');
                    for (size_t amp; (amp = tag.find("&amp;")) != std::string::npos;) tag.replace(amp, 5, "&");
                    if (tag.rfind("FX Types/", 0) == 0) tag.erase(0, 9);
                    pi.category = tag;
                }
                const size_t gi = raw.find("<gr-instrument-chunk");
                const auto paid = gi == std::string::npos ? std::vector<std::string>() : guitarRigPaid(raw.substr(gi));
                std::string d = paid.empty() ? "free edition" : "needs Guitar Rig Pro:";
                for (auto &x : paid) d += " " + x + ",";
                if (!paid.empty()) d.pop_back();
                pi.description = d;
            }
            pi.location = file;
            pi.loadKey = file;
            pi.stateFile = true;
            pi.kind = 0;   // CLAP_PRESET_DISCOVERY_LOCATION_FILE
            out.push_back(pi);
        }
    }
    // AAS Player: programs of its banks
    if (p == "aasplayer") {
        std::error_code ec;
        for (auto &e : fs::directory_iterator("/Library/Application Support/Applied Acoustics Systems/AAS Player/Banks", ec)) {
            if (e.path().extension() != ".aasbank") continue;
            std::string id, bank;
            std::vector<AasProgram> progs;
            if (!aasBank(e.path().string(), id, bank, progs)) continue;
            for (size_t i = 0; i < progs.size(); ++i) {
                PresetInfo pi;
                pi.name = progs[i].name;
                pi.category = progs[i].category;
                pi.stateFile = true;
                pi.location = pi.loadKey = "aas:" + e.path().string() + "#" + std::to_string(i + 1);
                out.push_back(pi);
            }
        }
    }
    // MeldaProduction plugins keep their presets in one bank file each, "<plugin>.presets" (MPowerSynth:
    // MSynthesizer), installed system-wide and copied to the user's folder, where saved presets go too
    std::string meldaBank;
    if (squash(plugin.vendor).find("melda") != std::string::npos || p == "mpowersynth") {
        const std::string file = (p == "mpowersynth" ? std::string("MSynthesizer") : plugin.name) + ".presets";
        std::error_code ec;
        for (const fs::path &dir : {fs::path(home) / "Library/Application Support/MeldaProduction", fs::path("/Library/Application Support/MeldaProduction")})
            if (fs::is_regular_file(dir / file, ec)) { meldaBank = (dir / file).string(); break; }
    }
    if (!meldaBank.empty()) {
        const std::string &bank = meldaBank;
        std::vector<MeldaPreset> presets;
        std::string e;
        if (meldaPresets(bank, presets, e))
            for (size_t i = 0; i < presets.size(); ++i) {
                PresetInfo pi;
                pi.name = presets[i].name;
                pi.category = presets[i].category;
                pi.stateFile = true;
                pi.location = pi.loadKey = "melda:" + bank + "#" + std::to_string(i);
                out.push_back(pi);
            }
    }
    // Analog Lab V: its own presets are its state (instrument presets of other Arturia engines are not)
    if (p == "analoglabv") {
        std::error_code ec;
        for (auto &e : fs::directory_iterator("/Library/Arturia/Presets/Analog Lab V/Factory/Factory", ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::string head(4096, '\0');
            in.read(&head[0], (std::streamsize)head.size());
            head.resize((size_t)in.gcount());
            if (head.rfind("22 serialization::archive ", 0) != 0 || head.find(" 15 InstrumentPart1 ") == std::string::npos) continue;
            PresetInfo pi;
            pi.name = e.path().filename().string();
            pi.category = "Analog Lab";
            pi.stateFile = true;
                pi.location = pi.loadKey = e.path().string();
            out.push_back(pi);
        }
    }
    // Dexed: every voice of every DX7 cartridge (.syx, 32 voices) is a preset: "<file>.syx#<n>"
    if (p == "dexed") {
        std::error_code ec;
        // where Dexed unpacks its cartridges on first load (JUCE's user data folder)
#if defined(__APPLE__)
        const fs::path carts = fs::path(home) / "Library/Application Support/DigitalSuburban/Dexed/Cartridges";
#elif defined(_WIN32)
        const fs::path carts = fs::path(getenv("APPDATA") ? getenv("APPDATA") : home) / "DigitalSuburban/Dexed/Cartridges";
#else
        const fs::path carts = (getenv("XDG_DATA_HOME") && *getenv("XDG_DATA_HOME") ? fs::path(getenv("XDG_DATA_HOME")) : fs::path(home) / ".local/share") /
                               "DigitalSuburban/Dexed/Cartridges";
#endif
        for (auto it = fs::recursive_directory_iterator(carts, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".syx" || !it->is_regular_file(ec)) continue;
            std::ifstream in(it->path(), std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (d.size() != 4104 || d[0] != 0xF0 || d[1] != 0x43 || d[3] != 0x09) continue;
            for (int v = 0; v < 32; ++v) {
                std::string name(reinterpret_cast<const char *>(&d[6 + v * 128 + 118]), 10);
                while (!name.empty() && (name.back() == ' ' || name.back() == 0)) name.pop_back();
                PresetInfo pi;
                pi.name = name;
                pi.category = it->path().stem().string();
                pi.stateFile = true;
                pi.location = pi.loadKey = it->path().string() + "#" + std::to_string(v);
                out.push_back(pi);
            }
        }
    }
    // Kilohearts snap-ins: presets in Kilohearts' shared folder, one subfolder and extension per
    // plugin (index.json names them: "kHs Delay" -> presets/ksdl/**/*.ksdl)
    if (squash(plugin.vendor) == "kilohearts") {
        std::ifstream in("/Library/Application Support/Kilohearts/index.json");
        const json index = in ? json::parse(in, nullptr, false) : json();
        std::string id, ext;
        if (index.is_object() && index.contains("plugins") && index["plugins"].is_array())
            for (const auto &e : index["plugins"])
                if (e.is_object() && e.value("fileName", "") == plugin.name) { id = e.value("id", ""); ext = "." + e.value("presetExtension", id); }
        std::error_code ec;
        if (!id.empty())
            for (const fs::path &root : {fs::path("/Library/Application Support/Kilohearts/presets"), fs::path(home) / "Library/Application Support/Kilohearts/presets"})
                for (auto it = fs::recursive_directory_iterator(root / id, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (!it->is_regular_file(ec) || it->path().extension().string() != ext) continue;
                    PresetInfo pi;
                    pi.name = it->path().stem().string();
                    pi.category = it->path().parent_path() == root / id ? "Presets" : it->path().parent_path().filename().string();
                    pi.stateFile = true;
                    pi.location = pi.loadKey = it->path().string();
                    out.push_back(pi);
                }
    }
    // Permut8: every program of every bank (30 each): "<bank>.p8bank#<n>"
    if (p == "permut8") {
        std::error_code ec;
        std::set<std::string> banks;
        for (const fs::path &root : {fs::path("/Library/Audio/Presets/Sonic Charge/Permut8 Banks"), fs::path(home) / "Library/Audio/Presets/Sonic Charge/Permut8 Banks"})
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".p8bank" || !it->is_regular_file(ec) || !banks.insert(it->path().filename().string()).second) continue;   // one copy per bank (All / By Package)
                std::ifstream in(it->path(), std::ios::binary);
                std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const auto names = permut8BankPrograms(d);
                for (size_t i = 0; i < names.size(); ++i) {
                    PresetInfo pi;
                    pi.name = names[i];
                    pi.category = it->path().stem().string();
                    pi.stateFile = true;
                    pi.location = pi.loadKey = it->path().string() + "#" + std::to_string(i);
                    out.push_back(pi);
                }
            }
    }
    // OB-Xd: every program of every bank (.fxb, 128 programs) is a preset: "<bank>.fxb#<n>"
    if (p == "obxd") {
        std::error_code ec;
        for (auto &e : fs::directory_iterator(fs::path(home) / "Documents/discoDSP/OB-Xd/Banks", ec)) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".fxb" || !e.is_regular_file(ec)) continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto names = obxdBankPrograms(d);
            for (size_t i = 0; i < names.size(); ++i) {
                PresetInfo pi;
                pi.name = names[i];
                pi.category = e.path().stem().string();
                pi.stateFile = true;
                pi.location = pi.loadKey = e.path().string() + "#" + std::to_string(i);
                out.push_back(pi);
            }
        }
    }
    // libraries often file one preset under several folders (All / By Category / By Creator): keep one
    // copy of each name + size, preferring a "Category" folder (its name is the category)
    std::stable_sort(out.begin(), out.end(), [](const PresetInfo &a, const PresetInfo &b) {
        const bool ca = a.location.find("Categor") != std::string::npos, cb = b.location.find("Categor") != std::string::npos;
        return ca > cb;
    });
    {
        std::set<std::pair<std::string, uintmax_t>> keep;
        std::vector<PresetInfo> unique;
        for (auto &x : out) {
            std::error_code ec;
            if (!fs::is_regular_file(x.location, ec)) { unique.push_back(x); continue; }   // bank entries, cartridge voices
            if (keep.insert({x.name, fs::file_size(x.location, ec)}).second) unique.push_back(x);
        }
        out.swap(unique);
    }
    std::sort(out.begin(), out.end(), [](const PresetInfo &a, const PresetInfo &b) {
        return a.category != b.category ? a.category < b.category : a.name < b.name;
    });
    // NKS presets for this plugin, unless a native preset file already has the name
    std::set<std::string> have;
    for (auto &x : out) have.insert(x.name);
    for (auto &x : nksPresets(plugin, false)) if (have.insert(x.name).second) out.push_back(x);
    return out;
}

} // namespace wl
