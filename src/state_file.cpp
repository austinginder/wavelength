#include "state_file.hpp"

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
} // namespace

bool readStateFile(const std::string &path, const std::string &format, StateFile &out, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot read state file " + path; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::string fmt = format.empty() ? "auto" : format;
    if (fmt == "auto")
        fmt = looksLikeClapPreset(data) ? "clap-preset" : isVstPreset(data) ? "vstpreset" : isNksf(data) ? "nksf"
            : endsWith(path, ".vital") ? "juce-string" : "raw";

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
    } else if (fmt == "raw") {
        out.state = std::move(data);
    } else {
        err = "unknown state format '" + fmt + "' (use auto, clap-preset, vstpreset, nksf, juce-string or raw)";
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
