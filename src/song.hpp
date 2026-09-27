#pragma once
// A song folder (docs/song-format.md): the manifest wavelength.json, the job and the files that
// belong to it. The path rules of the spec live here, so every command applies them the same way.
#include <nlohmann/json.hpp>

#include <filesystem>
#include <ctime>
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
// opens too (hasManifest() false): `save` and `migrate` give it one.
bool openSong(const std::string &arg, Song &song, std::string &err);
// the song a job file belongs to, when its folder has a manifest (renders record history there)
// What to call a job in pictures and reports: its song's title, else its folder (job.json) or file name.
std::string titleOfJob(const std::filesystem::path &jobPath);
bool songOfJob(const std::string &jobPath, Song &song);
bool writeManifest(const Song &song, std::string &err);
// A minimal manifest for a folder that has none: an ID, a title from the folder's name, the job and
// the files that look like sources, notes and media. Nothing is written.
nlohmann::json newManifest(const std::filesystem::path &dir);

// Section 2: a relative path inside the song, "/"-separated, no "..", no ".git"; `err` says what is wrong
bool checkSongPath(const std::string &rel, std::string &err);
// A path compared the way the format compares names (docs/song-format.md section 2): Unicode simple case
// folding for Latin, Greek and Cyrillic.
std::string foldName(const std::string &path);
// JSON the format accepts (I-JSON): no byte order mark, no key twice in one object.
bool parseJsonStrict(const std::string &text, nlohmann::json &out, std::string &err);
// Whether this Wavelength can read a song with this manifest: format and minReaderVersion as numbers,
// no required extension it doesn't know.
bool readableFormat(const nlohmann::json &manifest, std::string &err);
// Files the job writes (deliver[].file), {where, path}.
std::vector<std::pair<std::string, std::string>> jobOutputRefs(const nlohmann::json &job);
// The files a revision snapshots: the job and the files whose role is source, notes or media (those
// that exist), sorted.
std::vector<std::string> trackedFiles(const Song &song);
// A file the job uses: where it is named, the path (without a "#3" program suffix), the JSON string that
// names it (to rewrite with path + suffix), the object it belongs to (a track, fallback or effect) and
// whether it is that object's "state".
struct FileRef {
    std::string where, path, suffix;
    nlohmann::json *slot, *owner;
    bool state = false;
};
// Every file reference in a job: states (tracks, fallbacks, effects on tracks, buses and the master),
// sampler files and kit folders given as paths, clips. "lib:" names are library files, not the song's.
std::vector<FileRef> fileRefs(nlohmann::json &job);
std::vector<std::pair<std::string, std::string>> jobFileRefs(const nlohmann::json &job);

std::string newUuid();
std::string nowRfc3339();
std::string rfc3339(std::time_t t);   // local time with its offset
// who is acting: WAVELENGTH_AUTHOR (and WAVELENGTH_AUTHOR_KIND), else an agent when one is detected
// (Claude Code, Codex), else the user's login name
nlohmann::json actor();
std::string readText(const std::filesystem::path &p);
bool writeText(const std::filesystem::path &p, const std::string &text, std::string &err);

} // namespace wl
