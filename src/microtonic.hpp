#pragma once
// Sonic Charge Microtonic kits (.mtpreset) and drums (.mtdrum), via the plugin's own text parser.
#include "plugin.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wl {

bool isMicrotonicText(const std::vector<uint8_t> &d);

// A kit into a one-program bank built on the plugin's own state `comp` (Microtonic or Microtonic
// Multi). The kit waits for notes (36-43 = drums 1-8): its own pattern playback and mutes are off.
bool microtonicKitState(Plugin &plugin, const std::vector<uint8_t> &comp, const std::vector<uint8_t> &preset,
                        const std::string &name, std::vector<uint8_t> &out, std::string &err);

// One drum as parameter values for channel 1-8.
bool microtonicDrumParams(Plugin &plugin, const std::vector<uint8_t> &drum, int channel, std::vector<ParamValue> &values,
                          std::string &err);

} // namespace wl
