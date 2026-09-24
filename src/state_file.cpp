#include "state_file.hpp"

#include "preset_formats.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace wl {

namespace {
bool endsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool looksLikeClapPreset(const std::vector<uint8_t> &d) {
    if (d.size() < 9 || d[0] != 'c' || d[1] != 'l' || d[2] != 'a' || d[3] != 'p') return false;
    uint32_t n = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16) | (uint32_t(d[6]) << 8) | d[7];
    return n > 0 && n < 256 && 8 + n <= d.size();
}
bool isVstPreset(const std::vector<uint8_t> &d) { return d.size() >= 48 && d[0] == 'V' && d[1] == 'S' && d[2] == 'T' && d[3] == '3'; }
bool isNksf(const std::vector<uint8_t> &d) {
    return d.size() >= 12 && std::equal(d.begin(), d.begin() + 4, "RIFF") && std::equal(d.begin() + 8, d.begin() + 12, "NIKS");
}
// PCHK chunk of an NKS file, without its 4-byte version header
bool nksPluginChunk(const std::vector<uint8_t> &d, std::vector<uint8_t> &out) {
    size_t i = 12;
    while (i + 8 <= d.size()) {
        const uint32_t n = d[i + 4] | (d[i + 5] << 8) | (d[i + 6] << 16) | ((uint32_t)d[i + 7] << 24);
        if (i + 8 + n > d.size()) return false;
        if (std::equal(d.begin() + i, d.begin() + i + 4, "PCHK")) {
            if (n < 4) return false;
            out.assign(d.begin() + i + 12, d.begin() + i + 8 + n);
            return true;
        }
        i += 8 + n + (n & 1);
    }
    return false;
}
bool isFxp(const std::vector<uint8_t> &d) { return d.size() >= 28 && std::equal(d.begin(), d.begin() + 4, "CcnK"); }
uint32_t be32(const std::vector<uint8_t> &d, size_t i) { return uint32_t(d[i]) << 24 | uint32_t(d[i + 1]) << 16 | uint32_t(d[i + 2]) << 8 | d[i + 3]; }
// VST2 .fxp program / .fxb bank: the opaque chunk inside ("FPCh" program, "FBCh" bank)
bool fxpChunk(const std::vector<uint8_t> &d, std::vector<uint8_t> &out, std::string &err) {
    const std::string magic(d.begin() + 8, d.begin() + 12);
    size_t at;
    if (magic == "FPCh") at = 56;          // header + 28-byte program name, then u32 chunk size
    else if (magic == "FBCh") at = 156;    // header + 128 reserved bytes, then u32 chunk size
    else {
        err = magic == "FxCk" || magic == "FxBk"
            ? "is a VST2 parameter-list preset (" + magic + "), not a state chunk; set its values with \"params\" instead"
            : "has an unknown fxp type '" + magic + "'";
        return false;
    }
    if (d.size() < at + 4) { err = "is a truncated .fxp/.fxb file"; return false; }
    const uint32_t n = be32(d, at);
    if (at + 4 + n > d.size()) { err = "is a truncated .fxp/.fxb file"; return false; }
    out.assign(d.begin() + at + 4, d.begin() + at + 4 + n);
    return true;
}
// OB-Xf / OB-Xd program chunks keep the patch on the root element (<OB-Xf a=".." .../>); their
// plugin state wants a single-program document (<OB-Xf single-program-format="1"><program .../>)
void adaptObxProgram(std::vector<uint8_t> &chunk) {
    if (chunk.size() < 8 || !std::equal(chunk.begin(), chunk.begin() + 4, "VC2!")) return;
    std::string xml(chunk.begin() + 8, chunk.end());
    while (!xml.empty() && xml.back() == 0) xml.pop_back();
    for (const std::string tag : {"OB-Xf", "OB-Xd"}) {
        const size_t open = xml.find("<" + tag + " ");
        if (open == std::string::npos || xml.find("<program") != std::string::npos) continue;
        const size_t close = xml.find("/>", open);
        if (close == std::string::npos) return;
        size_t nameEnd = open + 1 + tag.size();
        std::string attrs = xml.substr(nameEnd, close - nameEnd), version;
        const std::string key = tag == "OB-Xf" ? "ob-xf_version=\"" : "ob-xd_version=\"";
        const size_t v = attrs.find(key);
        if (v != std::string::npos) {
            const size_t e = attrs.find('"', v + key.size());
            version = " " + attrs.substr(v, e + 1 - v);
            attrs.erase(v, e + 1 - v);
        }
        xml = xml.substr(0, open) + "<" + tag + version + " single-program-format=\"1\"><program" + attrs + "/></" + tag + ">";
        std::vector<uint8_t> body(xml.begin(), xml.end());
        body.push_back(0);
        const uint32_t n = (uint32_t)body.size();
        chunk.assign({'V', 'C', '2', '!', uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)});
        chunk.insert(chunk.end(), body.begin(), body.end());
        return;
    }
}
} // namespace

bool readStateFile(const std::string &path, const std::string &format, StateFile &out, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot read state file " + path; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::string fmt = format.empty() ? "auto" : format;
    if (fmt == "auto")
        fmt = looksLikeClapPreset(data) ? "clap-preset" : isVstPreset(data) ? "vstpreset" : isNksf(data) ? "nksf"
            : isFxp(data) ? "fxp" : isXferJson(data) ? "serum"
            : endsWith(path, ".odin") ? "juce-valuetree" : looksLikeH2p(data) || endsWith(path, ".h2p") ? "h2p" : endsWith(path, ".vital") ? "juce-string" : "raw";

    out.format = fmt;
    if (fmt == "clap-preset") {
        if (!looksLikeClapPreset(data)) { err = path + " is not a .clap-preset file"; return false; }
        uint32_t n = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) | (uint32_t(data[6]) << 8) | data[7];
        out.pluginId.assign(data.begin() + 8, data.begin() + 8 + n);
        out.state.assign(data.begin() + 8 + n, data.end());
    } else if (fmt == "juce-string") {
        out.state = std::move(data);
        if (out.state.empty() || out.state.back() != 0) out.state.push_back(0);
    } else if (fmt == "vstpreset") {
        if (!isVstPreset(data)) { err = path + " is not a .vstpreset file"; return false; }
        out.pluginId.assign(data.begin() + 8, data.begin() + 40);   // class id, 32 hex characters
        out.state = std::move(data);
    } else if (fmt == "nksf") {
        if (!isNksf(data) || !nksPluginChunk(data, out.state)) { err = path + " is not an NKS preset with a plugin chunk"; return false; }
    } else if (fmt == "fxp") {
        if (!isFxp(data)) { err = path + " is not a .fxp/.fxb file"; return false; }
        if (!fxpChunk(data, out.state, err)) { err = path + " " + err; return false; }
        adaptObxProgram(out.state);
    } else if (fmt == "serum") {
        if (!serumPresetToStates(data, out.state, out.controllerState, err)) { err = path + ": " + err; return false; }
    } else if (fmt == "juce-valuetree") {
        if (!valueTreeToJuceXml(data, out.state, err)) { err = path + ": " + err; return false; }
    } else if (fmt == "h2p") {
        std::string name = path.substr(path.find_last_of("/\\") + 1);
        if (endsWith(name, ".h2p")) name.resize(name.size() - 4);
        out.state = h2pToState(data, name);
    } else if (fmt == "raw") {
        out.state = std::move(data);
    } else {
        err = "unknown state format '" + fmt + "' (use auto, clap-preset, vstpreset, nksf, fxp, serum, juce-valuetree, h2p, juce-string or raw)";
        return false;
    }
    return true;
}

bool writeClapPreset(const std::string &path, const std::string &pluginId, const std::vector<uint8_t> &state, std::string &err) {
    std::ofstream out(path, std::ios::binary);
    if (!out) { err = "cannot write " + path; return false; }
    uint32_t n = (uint32_t)pluginId.size();
    uint8_t header[8] = {'c', 'l', 'a', 'p', uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
    out.write(reinterpret_cast<const char *>(header), 8);
    out.write(pluginId.data(), n);
    out.write(reinterpret_cast<const char *>(state.data()), (std::streamsize)state.size());
    return (bool)out;
}

} // namespace wl
