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
// `force`: every track with fallbacks plays the first available one, as on a computer without its plugin.
int applyFallbacks(nlohmann::json &job, const std::string &baseDir, std::vector<std::string> &notes, bool force = false);

// true when this computer can play `sound` (a track or a fallback entry); `why` says what is missing
bool soundAvailable(const nlohmann::json &sound, const std::string &baseDir, std::string &why);
// `track` plays `sound` instead of its own (what applyFallbacks does for the first available fallback)
void useSound(nlohmann::json &track, const nlohmann::json &sound);
// A stand-in for a track that has none, from built-in sounds: its role (bass, pad, lead, arp, keys, drums,
// strings, brass...) guessed from its name, plugin, preset and notes. {"role", "why", "fallback": [sounds]}
// (a General MIDI SoundFont program first for orchestral parts, then a builtin:synth patch); null when the
// track needs none (it is built in, or already has fallbacks).
nlohmann::json suggestFallback(const nlohmann::json &track);

} // namespace wl
