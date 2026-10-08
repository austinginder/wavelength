#pragma once
// Sounds: `sounds.json` beside a job (docs/job-format.md, "Sounds") gives tracks their instrument (plugin,
// preset, state, params) on top of whatever wrote the job, the way edits.json changes its notes. serve's
// Sounds page writes it while a person tunes each instrument by ear; an agent then writes the notes, and the
// sounds stay as they were set even when its make-job.py writes job.json again.
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace wl {

// The keys an entry sets on its track (besides "track", the track's name, and "note", words for whoever writes the part).
extern const char *const kSoundEntryKeys[4];

// The job with each sounds.json entry applied to the track it names: the entry replaces the track's preset, state and
// params (a different plugin replaces its whole sound, the way a fallback does). An entry whose sound isn't on this
// computer is skipped, so the track keeps its own sound and fallbacks. `unmatched` gets (track, why) for entries that
// didn't apply. Applying it twice gives the same job. Returns the job unchanged when there is no sounds.json.
nlohmann::json applySounds(const nlohmann::json &job, const std::string &baseDir,
                           std::vector<std::pair<std::string, std::string>> *unmatched = nullptr);

// The job with each sounds.json entry as one more track ("sounds.json: <track>"), for listing the files and plugins a song
// uses (packages, validate, purge): every entry counts, whether this computer has its plugin or not.
nlohmann::json jobWithSounds(const nlohmann::json &job, const std::string &baseDir);

} // namespace wl
