#pragma once
// `wavelength help`: a short list of commands grouped by task, `help <command>` (or `<command> --help`)
// for one command's usage and details, `help all` for everything. Built from one text, kUsage; on a
// terminal it is styled (term.hpp), in a pipe it is plain.
#include <cstdio>
#include <string>

namespace wl {

extern const char *kUsage;

// the short list (exit status 0), one command (1 when there is no such command) or everything
int printHelp(std::FILE *out, const std::string &command, const std::string &version);

} // namespace wl
