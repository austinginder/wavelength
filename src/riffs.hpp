#pragma once
// Riffs: short parts a person plays on serve's pages for an agent to build songs from. A song keeps its own in
// `riffs.json` beside its job (AGENTS.md, "Starting from a riff"). The riff library keeps riffs apart from any song:
// a folder shaped like a Sounds-page project, whose job.json and sounds.json hold its instruments (a track each, no
// notes) and whose riffs.json holds every riff and the groups they are filed in. `wavelength riffs` lists and shows
// them; `wavelength riffs use` copies some, with their instruments, into a song.
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace wl {

constexpr const char *kRiffLibrarySlug = "_riffs";   // how serve's pages name the library

// $WAVELENGTH_RIFFS, else Riffs/ in Wavelength's folder (platform::dataDir())
std::filesystem::path riffLibraryDir();

// A riff checked and tidied for riffs.json: {id?, name, track, group?, from?, tempo, timeSignature, bars, quantize,
// note?, notes, played?}; notes are {beat, dur, key (a note name), vel} from the riff's first downbeat.
bool normalizeRiff(const nlohmann::json &in, nlohmann::json &out, std::string &err);

// A riffs.json (a song's or the library's), or an empty one: {"format", "formatVersion", "riffs": [...]}
nlohmann::json readRiffFile(const std::filesystem::path &file);
// Writes it (without "groups" when there are none); a file with no riffs and no groups is removed.
bool writeRiffFile(const std::filesystem::path &file, nlohmann::json rf, std::string &err);
// The groups in their order: the file's "groups", then any a riff names that the list misses.
std::vector<std::string> riffGroups(const nlohmann::json &rf);

// The library's instruments: track name -> its sound {plugin, preset, state, params} (sounds.json over job.json).
nlohmann::json libraryInstruments(const std::filesystem::path &lib);
// "Wavelength Trance · Driven Bass · 1 knob set"
std::string soundLine(const nlohmann::json &sound);
// The riff's notes by bar: "C2 G2 | F#2 G2 Bb2 A2 Ab2 F#2" (a chord as "C4+Eb4+G4")
std::string riffNoteLine(const nlohmann::json &riff, size_t maxNotes = 32);

// Riffs of the library named by id ("riff-12"), name (any case; a name two riffs share is refused), or "group:NAME"
// (every riff in the group). Each riff once, in the order named.
bool resolveRiffs(const nlohmann::json &rf, const std::vector<std::string> &refs, std::vector<nlohmann::json> &out, std::string &err);

// Copies library riffs into a song folder (made when it doesn't exist): riffs.json gets each riff with a new id and
// "from": "library:<id>" in place of its group (a riff already copied is skipped), sounds.json and job.json a track for each instrument they
// play on (named as in the library; a track of that name with another sound gets "<name> 2"), and preset files the
// sounds use are copied in. Returns {song, riffs: [{id, name, track, from}], skipped, tracks: [{name, sound, added}], job}.
nlohmann::json useRiffs(const std::filesystem::path &songDir, const std::vector<nlohmann::json> &riffs,
                        const std::filesystem::path &lib, std::string &err);

} // namespace wl
