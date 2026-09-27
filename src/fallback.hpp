#pragma once
// Stand-in sounds for jobs that travel: a track's "fallback" lists other sounds (another plugin,
// builtin:synth, a sample library) that play its part when its own plugin or sample library isn't
// on this computer. Applied to the job's JSON before it is parsed, the same way by the render and by
// its worker processes, so a song made with Serum 2 still renders on a machine without it.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

// Swaps in each track's first fallback that is available here and returns how many it swapped; `notes`
// gets a line per swap, and per track that has fallbacks but can play neither its own sound nor any of them.
int applyFallbacks(nlohmann::json &job, const std::string &baseDir, std::vector<std::string> &notes);

} // namespace wl
