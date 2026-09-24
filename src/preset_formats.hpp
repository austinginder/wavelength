#pragma once
// Converters from plugins' own preset files to the state their plugins load.
//
//  Serum 2 .SerumPreset  "XferJson\0" + u64 LE header length + JSON header + u32 LE CBOR size
//                        + u32 version (2) + zstd(CBOR). A preset is the processor state and the
//                        controller state merged into one map; they are split apart again here.
//  JUCE ValueTree        ValueTree::writeToStream binary (Odin2 .odin): the plugin's state is
//                        the same tree as JUCE binary XML ("VC2!" + u32 LE length + XML + NUL).
//  u-he .h2p             text preset (Zebra2, Zebralette, TripleCheese, ...): the state is
//                        u32 LE length + "#pgm=<name>.h2p\n" + the preset text.
#include <cstdint>
#include <string>
#include <vector>

namespace wl {

bool isXferJson(const std::vector<uint8_t> &d);
bool serumPresetToStates(const std::vector<uint8_t> &file, std::vector<uint8_t> &processor,
                         std::vector<uint8_t> &controller, std::string &err);

bool valueTreeToJuceXml(const std::vector<uint8_t> &file, std::vector<uint8_t> &state, std::string &err);

// DX7 32-voice bulk dump (.syx, 4104 bytes) + voice index -> patch Dexed's own state: the
// cartridge goes into its sysex blob and the voice (unpacked to 155 bytes) into its edit buffer.
bool isDx7Cartridge(const std::vector<uint8_t> &d);
bool dexedWithVoice(const std::vector<uint8_t> &dexedState, const std::vector<uint8_t> &cart, int voice,
                    std::vector<uint8_t> &out, std::string &err);

// Synplant .synplant text patch -> program slot 0 of Synplant's own state (which holds 16 patches,
// zlib-compressed inside an FXB chunk); `name` becomes the patch's display name.
bool isSynplantPatch(const std::vector<uint8_t> &d);
bool synplantWithPatch(const std::vector<uint8_t> &synplantState, const std::vector<uint8_t> &patch, const std::string &name,
                       std::vector<uint8_t> &out, std::string &err);

// Cherry Audio presets (.dco106preset, .mg1preset, .sempreset, .voltagepreset): a JUCE ValueTree
// "main" holding the patch, merged into the plugin's own state ("savedState" > "pt", or for
// Voltage Modular "VoltageState" > "presetInfo" inside a VST2-style FXB wrapper).
bool isCherryPreset(const std::vector<uint8_t> &d);
bool cherryWithPreset(const std::vector<uint8_t> &pluginState, const std::vector<uint8_t> &preset,
                      std::vector<uint8_t> &out, std::string &err);

// Guitar Rig 6 rack (.ngrr): its rack XML block, wrapped in the NI SoundShell container the plugin's
// state uses (no template needed). `paidComponents` lists components outside the free edition,
// which a free licence removes on load.
std::vector<std::string> guitarRigPaid(const std::string &rackXml);   // paid components a rack uses
bool guitarRigRackState(const std::vector<uint8_t> &rack, std::vector<uint8_t> &state,
                        std::vector<std::string> &paidComponents, std::string &err);

bool looksLikeH2p(const std::vector<uint8_t> &d);
std::vector<uint8_t> h2pToState(const std::vector<uint8_t> &text, const std::string &name);

} // namespace wl
