#pragma once
// `wavelength __parse <format> <file>...` (internal): one file reader over each file, a JSON line per file.

#include <cstdio>
#include <string>
#include <vector>

namespace wl {

int parseCheck(const std::vector<std::string> &args, std::FILE *out);

} // namespace wl
