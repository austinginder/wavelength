#pragma once
// A song's revisions (docs/song-format.md, section 6): history/log.jsonl, one revision per line, and
// history/objects/<2>/<sha256>, each tracked file's content compressed with zlib and named by the
// SHA-256 of its bytes. Undo, redo and restore never delete: they write older files back and add a line.
#include "song.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl::history {

using json = nlohmann::json;

// every line of the log, in order (a truncated last line is skipped; `warning` says so)
bool read(const Song &song, std::vector<json> &entries, std::string &err, std::string *warning = nullptr);
// the revision the folder is at: the last entry's, or the one it restored
int current(const std::vector<json> &entries);
const json *find(const std::vector<json> &entries, int rev);

// Hashes the tracked files and stores any object not stored yet: {"path": "sha256:<hex>"}.
bool snapshot(const Song &song, json &files, std::string &err);
// Appends an entry (fills in rev, time and parent) and returns its revision, 0 on error.
int append(const Song &song, std::vector<json> &entries, json entry, std::string &err);

// A save point. With `always` false nothing is added when the files are the current revision's and
// there is no message; returns the revision the folder is at.
int save(const Song &song, const std::string &message, const json &by, bool always, std::string &err);
// undo / redo / restore <rev>: saves unsaved changes first, writes the target's files, logs the step.
// Returns the new entry's revision, 0 on error.
int undo(const Song &song, const json &by, std::string &err);
int redo(const Song &song, const json &by, std::string &err);
int restore(const Song &song, int target, const json &by, std::string &err);

// A full render of a song's job (the job's folder has a manifest): appends a `render` revision with the
// render's loudness summary, adds "song": {"revision", "job"} to `report` and writes it to
// <outDir>/report.json again. Returns the revision, 0 when the job isn't a song's.
int recordRender(const std::string &jobPath, const std::string &outDir, json &report, std::string &err);

// a tracked file's content at a revision
bool readFile(const Song &song, int rev, const std::string &path, std::string &out, std::string &err);
// the job at a revision (0: the folder as it is now)
bool jobAt(const Song &song, int rev, json &job, std::string &err);
bool readObject(const Song &song, const std::string &hash, std::string &out, std::string &err);

// Every revision as one git commit (tree = its files, parent = its parent's commit, author and date from
// the entry, message + "Wavelength-Revision: <rev>"), the same on every computer. Into a new repository
// `dir`, or a bundle file. Needs git.
bool exportGit(const Song &song, const std::string &target, bool bundle, std::string &err);

} // namespace wl::history
