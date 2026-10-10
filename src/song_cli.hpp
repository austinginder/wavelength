#pragma once
// The song commands (docs/song-format.md): save, history, undo, redo, restore, diff, comments, riffs, pack,
// unpack, validate, migrate and fallbacks. They parse their own arguments, so main.cpp only routes them.
#include <cstdio>
#include <string>

namespace wl {

bool isSongCommand(const std::string &cmd);
int runSongCommand(int argc, char **argv, std::FILE *out);

} // namespace wl
