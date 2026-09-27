#pragma once
// A song folder (docs/song-format.md): the manifest wavelength.json, the job and the files that
// belong to it. The path rules of the spec live here, so every command applies them the same way.
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace wl {

constexpr const char *kSongFormat = "wavelength.song";
constexpr const char *kSongFormatVersion = "1.0";
constexpr const char *kSongMediaType = "application/vnd.wavelength.song+zip";

struct Song {
    std::filesystem::path dir;
    nlohmann::json manifest = nlohmann::json::object();   // {} when the folder has no wavelength.json
    bool hasManifest() const { return manifest.contains("format"); }
    std::string jobFile() const { return manifest.value("job", std::string("job.json")); }
    std::filesystem::path jobPath() const { return dir / jobFile(); }
    std::string title() const;
};

// A song from a folder, its job file or its wavelength.json. A folder with a job but no manifest
// opens too (hasManifest() false): `save` and `upgrade` give it one.
bool openSong(const std::string &arg, Song &song, std::string &err);
// the song a job file belongs to, when its folder has a manifest (renders record history there)
bool songOfJob(const std::string &jobPath, Song &song);
bool writeManifest(const Song &song, std::string &err);
// A minimal manifest for a folder that has none: an ID, a title from the folder's name, the job and
// the files that look like sources, notes and media. Nothing is written.
nlohmann::json newManifest(const std::filesystem::path &dir);

// Section 2: a relative path inside the song, "/"-separated, no "..", no ".git"; `err` says what is wrong
bool checkSongPath(const std::string &rel, std::string &err);
// The files a revision snapshots: the job and the files whose role is source, notes or media (those
// that exist), sorted.
std::vector<std::string> trackedFiles(const Song &song);
// every file reference in a job ({where, path}): states, sampler files, clips
std::vector<std::pair<std::string, std::string>> jobFileRefs(const nlohmann::json &job);

std::string newUuid();
std::string nowRfc3339();
// who is acting: WAVELENGTH_AUTHOR (and WAVELENGTH_AUTHOR_KIND), else an agent when one is detected
// (Claude Code, Codex), else the user's login name
nlohmann::json actor();
std::string readText(const std::filesystem::path &p);
bool writeText(const std::filesystem::path &p, const std::string &text, std::string &err);

} // namespace wl
