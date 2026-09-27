#pragma once
// Comments on a song (review.json, docs/song-format.md section 7): each keeps an anchor to what was
// heard (the revision and render that were playing, the bars, time and tracks picked), replies, and
// whether it was resolved. `serve` writes them, agents read and answer them with `wavelength comments`.
#include "song.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace wl::review {

using json = nlohmann::json;

// {"comments": [...]}; a missing file is empty
json read(const std::filesystem::path &songDir);
bool write(const std::filesystem::path &songDir, const json &data, std::string &err);
// the comment in the spec's shape: legacy comments (anchor fields on the comment, `reply` a string) moved
// into anchor and replies
json normalize(const json &comment);
// A new comment's anchor: the selection the page sends (ref, bars, beats, time, tracks, notes) plus the
// revision and report hash of the render being heard (`reportRel`, relative to the song; "" = out/report.json).
json anchorFor(const Song &song, const json &selection, const std::string &reportRel);
// How the song moved since a comment: {"revision", "since": [revisions after it], "outdated", "why": [...]}
json status(const Song &song, const json &comment);

} // namespace wl::review
