#include "state_file.hpp"

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
} // namespace

bool readStateFile(const std::string &path, const std::string &format, StateFile &out, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot read state file " + path; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::string fmt = format.empty() ? "auto" : format;
    if (fmt == "auto") fmt = looksLikeClapPreset(data) ? "clap-preset" : endsWith(path, ".vital") ? "juce-string" : "raw";

    out.format = fmt;
    if (fmt == "clap-preset") {
        if (!looksLikeClapPreset(data)) { err = path + " is not a .clap-preset file"; return false; }
        uint32_t n = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) | (uint32_t(data[6]) << 8) | data[7];
        out.pluginId.assign(data.begin() + 8, data.begin() + 8 + n);
        out.state.assign(data.begin() + 8 + n, data.end());
    } else if (fmt == "juce-string") {
        out.state = std::move(data);
        if (out.state.empty() || out.state.back() != 0) out.state.push_back(0);
    } else if (fmt == "raw") {
        out.state = std::move(data);
    } else {
        err = "unknown state format '" + fmt + "' (use auto, clap-preset, juce-string or raw)";
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
