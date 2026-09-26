#pragma once
// MusicXML scores (.musicxml, .xml, compressed .mxl; score-partwise) imported into a job.
//
// One track per part. Repeats and first/second endings are played out (D.C., D.S., Fine and Coda
// too), ties join, chords and voices keep their timing, and transposing instruments sound at
// concert pitch. Dynamics (pp..ff, sf, the dynamics attribute, <sound dynamics>) and hairpins set
// velocities; accents raise them and staccato marks shorten notes. Each note keeps its score marks
// in "marks" (staccato, accent, tenuto, fermata, ...) for mapping onto a library's articulations.
// Tempo marks become the tempo map, key signatures with a mode become "keys", rehearsal marks become
// markers. Parts get a General MIDI sound from the sample library (their MIDI program, or a guess
// from the part name), percussion parts builtin:drums with the score's MIDI keys.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct MusicXmlImport {
    nlohmann::json job;
    std::vector<std::string> notes;     // what was approximated or left out
    size_t tracks = 0, noteCount = 0, measures = 0, playedMeasures = 0;
};

bool importMusicXml(const std::string &path, const std::string &outDir, const std::string &instrument, MusicXmlImport &out, std::string &err);

} // namespace wl
