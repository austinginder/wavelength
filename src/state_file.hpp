#pragma once
// Plugin state files.
//
//  clap-preset  Bitwig's container, also used inside .dawproject files:
//               "clap" + u32 big-endian id length + plugin id + raw plugin state
//  juce-string  a text preset (e.g. a Vital .vital file) that a JUCE plugin stores with
//               MemoryOutputStream::writeString, i.e. the UTF-8 text plus a NUL
//  raw          the plugin's state bytes as-is
//  auto         clap-preset if the header matches, juce-string for .vital, else raw
#include <cstdint>
#include <string>
#include <vector>

namespace wl {

struct StateFile {
    std::vector<uint8_t> state;   // what the plugin's state.load() receives
    std::string pluginId;         // from a clap-preset header, if present
    std::string format;           // the format that was used
};

bool readStateFile(const std::string &path, const std::string &format, StateFile &out, std::string &err);
bool writeClapPreset(const std::string &path, const std::string &pluginId, const std::vector<uint8_t> &state, std::string &err);

} // namespace wl
