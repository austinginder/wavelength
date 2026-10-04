#pragma once
// GarageBand projects (.band, GarageBand 10 for Mac). The package holds the song in
// Alternatives/000/ProjectData and its tempo, key and files in MetaData.plist. ProjectData is Logic's
// project container: a 24-byte header, then chunks back to back to the end of the file, each a 36-byte
// header (tag stored reversed, class, object id, a reference, a sub-index, payload size) and a payload.
// The format is private and undocumented: it is read here from its observed layout (GarageBand 10.2 and
// 10.4 saves). The arrangement lists the tracks; each track's object links its channel strip, whose
// records (sends, the instrument, effects) have the layout of a .patch folder's channel strip, so a track
// is written out as its own patch folder and plays the way GarageBand's patches do (logic_patches.hpp).
// Regions are sequences of MIDI-like events at 960 ticks a beat; Drummer regions hold their generated
// performance as plain notes.
#include "dawproject.hpp"

#include <string>

namespace wl {

// Reads `path` (a .band package; it is never written) into `outDir`/job.json, with each kept track's channel
// strip as `outDir`/patches/<track>.patch/#Root.cst (aux and master channels under patches/_aux and
// patches/_master.patch): tracks in order (instrument tracks on builtin:sampler or builtin:synth, audio
// tracks on builtin:audio with their effects), faders, pans, sends to buses carrying the aux channels'
// effects, the master's effects, notes of MIDI and Drummer regions, audio regions as clips, tempo, time
// signature, key, sample rate. The job's "import" object keeps where it came from and the warnings, which
// `out.notes` lists too. `allTracks` keeps tracks without regions; `copyMedia` copies audio files into
// `outDir`/media instead of naming the project's.
bool importGarageBand(const std::string &path, const std::string &outDir, DawprojectImport &out, std::string &err,
                      bool allTracks = false, bool copyMedia = false);

} // namespace wl
