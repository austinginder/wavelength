#pragma once
// Riffs: short parts a person plays on serve's pages for an agent to build songs from. A song keeps its own in
// `riffs.json` beside its job (AGENTS.md, "Starting from a riff"). The riff library keeps riffs apart from any song:
// a folder shaped like a Sounds-page project, whose job.json and sounds.json hold its instruments (a track each, no
// notes) and whose riffs.json holds every riff, each in a numbered group ("Group 3"): a new riff joins the group of
// the day it was made, and the human moves riffs between groups by dragging. A group has no name and no entry of its
// own: it is the riffs that carry its number. `wavelength riffs` lists and shows them; `wavelength riffs use` copies
// some, with their instruments, into a song.
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace wl {

constexpr const char *kRiffLibrarySlug = "_riffs";   // how serve's pages name the library

// $WAVELENGTH_RIFFS, else Riffs/ in Wavelength's folder (platform::dataDir())
std::filesystem::path riffLibraryDir();

// A riff checked and tidied for riffs.json: {id?, name, track, group? (a number), from?, tempo, timeSignature, bars, quantize,
// note?, notes, played?}; notes are {beat, dur, key (a note name), vel} from the riff's first downbeat.
bool normalizeRiff(const nlohmann::json &in, nlohmann::json &out, std::string &err);

// A riffs.json (a song's or the library's), or an empty one: {"format", "formatVersion", "riffs": [...]}
nlohmann::json readRiffFile(const std::filesystem::path &file);
// Writes it ("today" only while its group has riffs); a file with no riffs is removed.
bool writeRiffFile(const std::filesystem::path &file, nlohmann::json rf, std::string &err);

// The library's groups. Files every riff of an older library in a numbered group: named groups (before 2026-10-11)
// take numbers in the order they were made, after any numbers in use; a riff in no group joins a group for the day
// it was made. True when it changed anything.
bool fileRiffGroups(nlohmann::json &rf);
// A riff's group number, 0 when it has none
int riffGroup(const nlohmann::json &riff);
// The group numbers riffs are in, lowest first
std::vector<int> riffGroups(const nlohmann::json &rf);
// The group a riff made now joins: today's (riffs.json "today": {day, group}) while it has riffs, else a new one
int todaysRiffGroup(nlohmann::json &rf);
// The group the file's "today" names when that is today and it has riffs, else 0
int currentDayGroup(const nlohmann::json &rf);
// One past the highest group: for riffs moved to a new group
int newRiffGroup(const nlohmann::json &rf);
// "3", "Group 3", "group3", "g3" -> 3; 0 when it is no group number
int riffGroupNumber(const std::string &text);

// The library's instruments: track name -> its sound {plugin, preset, state, params} (sounds.json over job.json).
nlohmann::json libraryInstruments(const std::filesystem::path &lib);
// "Wavelength Trance · Driven Bass · 1 knob set"
std::string soundLine(const nlohmann::json &sound);
// The riff's notes by bar: "C2 G2 | F#2 G2 Bb2 A2 Ab2 F#2" (a chord as "C4+Eb4+G4")
std::string riffNoteLine(const nlohmann::json &riff, size_t maxNotes = 32);

// Riffs of the library named by id ("riff-12"), name (any case; a name two riffs share is refused), or "group:3"
// (every riff in Group 3; "Group 3" alone too when no riff has that name). Each riff once, in the order named.
bool resolveRiffs(const nlohmann::json &rf, const std::vector<std::string> &refs, std::vector<nlohmann::json> &out, std::string &err);

// Copies library riffs into a song folder (made when it doesn't exist): riffs.json gets each riff with a new id and
// "from": "library:<id>" in place of its group (a riff already copied is skipped), sounds.json and job.json a track for each instrument they
// play on (named as in the library; a track of that name with another sound gets "<name> 2"), and preset files the
// sounds use are copied in. Returns {song, riffs: [{id, name, track, from}], skipped, tracks: [{name, sound, added}], job}.
nlohmann::json useRiffs(const std::filesystem::path &songDir, const std::vector<nlohmann::json> &riffs,
                        const std::filesystem::path &lib, std::string &err);

} // namespace wl
