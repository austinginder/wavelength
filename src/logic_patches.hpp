#pragma once
// GarageBand and Logic patches: a .patch folder holds one channel strip (.cst) per channel and data.plist.
// A channel strip is a run of "UCuA" records, one per plugin slot (MIDI effects, the instrument, audio
// effects, sends): a 36-byte header (payload size at +0x1c) and a payload with the plugin's name at +120 (12
// bytes) and its maker at +132 ("MELC", "GAME"). The sampler plays the channels on Logic's samplers
// (Sampler, EXS24, Drum Kit Designer): their instrument is stored inside the channel strip (Sampler
// patches carry a whole EXS instrument), or named ("MELC" "PMAS" + 4 bytes + "<name>.exs"). Apple's
// synths (Alchemy, Retro Synth, ES2, Sculpture, the Vintage keyboards) run only inside GarageBand and Logic.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wl {

struct PatchChannel {
    std::string file;            // the channel strip (.cst)
    std::string instrument;      // its instrument as the channel strip names it ("Sampler", "EXS24", "Drum Kit", "Alchemy", ...); "" = none
    bool sampler = false;        // an instrument the sampler plays
    std::vector<uint8_t> data;   // the channel strip, read whole for sampler channels
    size_t exsAt = 0;            // where an instrument stored inside it starts (0 = none)
    std::string exs;             // else the .exs file it names ("Steinway Piano 2.exs")
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
// "Sampler", "EXS24", "Drum Kit" (Drum Kit Designer): instruments built on Logic's samplers.
bool isSamplerInstrument(const std::string &name);

} // namespace wl
