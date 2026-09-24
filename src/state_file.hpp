#pragma once
// Plugin state files.
//
//  clap-preset  Bitwig's container, also used inside .dawproject files:
//               "clap" + u32 big-endian id length + plugin id + raw plugin state
//  juce-string  a text preset (e.g. a Vital .vital file) that a JUCE plugin stores with
//               MemoryOutputStream::writeString, i.e. the UTF-8 text plus a NUL
//  vstpreset    a VST3 preset file ("VST3" header, component + controller chunks); passed to
//               the VST3 host whole
//  nksf         a Native Instruments NKS preset (RIFF "NIKS"); the PCHK chunk holds the
//               plugin's own state (after a 4-byte chunk version)
//  fxp          VST2 .fxp/.fxb program or bank chunk (Surge XT, OB-Xf patches)
//  serum        Serum 2 .SerumPreset, split into its processor and controller states
//  juce-valuetree  a JUCE ValueTree in binary form (Odin2 .odin), re-written as the JUCE binary
//               XML state the plugin loads
//  h2p          a u-he text preset (Zebra2, Zebralette, TripleCheese, ...)
//  raw          the plugin's state bytes as-is
//  auto         detected from the header or extension, else juce-string for .vital, else raw
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wl {

struct StateFile {
    std::vector<uint8_t> state;   // what the plugin's state.load() receives
    std::vector<uint8_t> controllerState;   // VST3 edit-controller state, when the file carries one
    // Formats that patch the plugin's own state (a DX7 voice into Dexed's): called with the
    // plugin's current state, returns the state to load. Empty = load `state` as is.
    std::function<bool(const std::vector<uint8_t> &current, std::vector<uint8_t> &out, std::string &err)> transform;
    std::string pluginId;         // from a clap-preset header or a vstpreset class id, if present
    std::string format;           // the format that was used
};

bool readStateFile(const std::string &path, const std::string &format, StateFile &out, std::string &err);
bool writeClapPreset(const std::string &path, const std::string &pluginId, const std::vector<uint8_t> &state, std::string &err);

} // namespace wl
