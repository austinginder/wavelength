#pragma once
// Standard MIDI Files (.mid): import into a job, export a job's parts.
//
// Import: tempo map, time signature, markers, and one track per MIDI track and channel with its
// notes (sustain pedal folded into note lengths), controllers (CC7 volume -> fader, CC10 pan -> pan,
// CC11 expression -> gain rides, others -> automation.cc), pitch bend (automation.pitchbend, or
// per-note bends on sampler tracks) and channel pressure. Channel 10 plays builtin:drums; other
// channels get a General MIDI-family sound from the installed sample library (or --instrument).
//
// Export: a type 1 file with a conductor track (tempo map, time signature, markers) and one track per
// job track: notes, CC / pitch bend / pressure automation, and the program kept from an import.
#include "job.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct MidiImport {
    nlohmann::json job;                 // the job (written to outDir/job.json)
    std::vector<std::string> notes;     // what was approximated or left out, one line each
    size_t tracks = 0, noteCount = 0;
    int format = 0;                     // SMF type 0, 1 or 2
};

// `instrument`: a plugin spec for every melodic track ("" = General MIDI sounds from the sample library)
bool importMidiFile(const std::string &path, const std::string &outDir, const std::string &instrument, MidiImport &out, std::string &err);

// General MIDI program 0-127: its name, and the sound a part with it gets: {"plugin", "sampler"}. A close
// Bitwig multisample, else the General MIDI SoundFont (`samples --install-soundfont`), else a looser
// multisample; drums get the SoundFont's kit (bank 128, `program` = kit) or builtin:drums. `note`
// explains a stand-in ("" when there is none).
std::string gmProgramName(int program);
nlohmann::json gmSound(int program, bool drums, std::string &note);

// `raw` is the job JSON `job` was parsed from (programs and drum kits are read from it)
bool exportMidiFile(const Job &job, const nlohmann::json &raw, const std::string &path, std::vector<std::string> &notes, std::string &err);

} // namespace wl
