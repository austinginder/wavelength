#pragma once
// GarageBand and Logic patches: a .patch folder holds one channel strip (.cst) per channel and data.plist.
// A channel strip is a run of "UCuA" records, one per plugin slot (MIDI effects, the instrument, audio
// effects, sends): a 36-byte header (payload size at +0x1c) and a payload with the plugin's name at +120 (12
// bytes) and its maker at +132 ("MELC", "GAME"). The sampler plays the channels on Logic's samplers
// (Sampler, EXS24, Drum Kit Designer): their instrument is stored inside the channel strip (Sampler
// patches carry a whole EXS instrument), or named ("MELC" "PMAS" + 4 bytes + "<name>.exs"). Apple's
// synths (Alchemy, Retro Synth, ES2, Sculpture, the Vintage keyboards) run only inside GarageBand and Logic.
#include <cstddef>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace wl {

// A plug-in's saved settings: Apple parameter #n is params[n] (from the "TSPP" block: u32 size, u16 version,
// u8 big-endian flag, u8, u32 count, "GAME" "TSPP" ("EMAG" "PPST" big-endian), u32 plug-in id, count float32
// values of which the first is reserved)
struct PatchPlugin {
    std::string name;            // as the channel strip names it ("Channel EQ", "Compressor", "Tape Delay", ...)
    uint32_t id = 0;             // Apple's plug-in id (Channel EQ 236, Compressor 154, Tape Delay 147, Single Band EQ 311)
    std::vector<float> params;   // empty when the slot holds no settings block
};

struct PatchChannel {
    std::string file;            // the channel strip (.cst)
    std::string instrument;      // its instrument as the channel strip names it ("Sampler", "EXS24", "Drum Kit", "Alchemy", ...); "" = none
    std::string preset;          // the instrument's settings file as the channel strip names it ("Boutique 808.pst", "#default.pst")
    bool sampler = false;        // an instrument the sampler plays
    std::vector<uint8_t> data;   // the channel strip, read whole for sampler channels
    size_t exsAt = 0;            // where an instrument stored inside it starts (0 = none)
    std::string exs;             // else the .exs file it names ("Steinway Piano 2.exs")
    std::vector<std::string> effects;   // the other plugins on the channel, in order
    std::vector<PatchPlugin> chain;     // the same plug-ins with their settings (after the instrument)
    PatchPlugin settings;               // the instrument's own settings (Retro Synth's parameters)
};

// A send from a patch's channel to a GarageBand aux (data.plist): "Large Hall/6.6s Botta Church", its level
struct PatchSend {
    std::string channel, aux, room;   // the channel's name, the aux tag, its room ("6.6s Botta Church")
    double db = 0;                    // the send level, read as a linear gain
};

struct LogicPatch {
    std::string name, path, category;   // the folder name without ".patch", its path, the folder under Instrument it is in
    std::string instrument;             // the instrument it plays (its first sampler channel's, else the first one's)
    bool sampler = false;               // some channel plays an instrument the sampler can play
};

// Where patches are installed: GarageBand's and Logic's own, Logic's library, the user's.
std::vector<std::string> logicPatchRoots();
// Every instrument patch under the roots (cached per process; reads each channel strip's plugin names only).
const std::vector<LogicPatch> &logicPatches();
// A patch by name (any case), else nullptr.
const LogicPatch *logicPatchNamed(const std::string &name);
// A patch folder's channels (root channel first) with their instruments; sampler channels read whole.
bool readPatchChannels(const std::string &patchDir, std::vector<PatchChannel> &out, std::string &err);
// Wavelength effects for the plug-ins of a channel that have built-in counterparts: Channel EQ and Single Band EQ
// (eq bands; cuts as cascaded biquads), Compressor (compressor with its gain stages, distortion as a clip, its
// limiter) and Tape Delay (delay; a Wet of 0 is left out). `notes` names what is approximated or left out
// (Gain-Q coupling, circuit types, groove, wow and flutter, and every other plug-in). From the parameter maps
// decoded from GarageBand's patches and Logic's presets (docs in the changelog for 0.5.0).
nlohmann::json patchEffects(const std::vector<PatchPlugin> &chain, std::vector<std::string> &notes);
// A patch's sends (macOS: data.plist is a binary property list); empty when it has none or can't be read.
std::vector<PatchSend> readPatchSends(const std::string &patchDir);
// "Sampler", "EXS24", "Drum Kit" (Drum Kit Designer): instruments built on Logic's samplers.
bool isSamplerInstrument(const std::string &name);

} // namespace wl
