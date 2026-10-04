#pragma once
// Binary property lists ("bplist00"), read on any platform: Apple keeps small settings in them inside channel
// strips and presets (the Arpeggiator's rhythm grid). Objects become JSON: dictionaries, arrays, strings (ASCII
// and UTF-16), integers, reals, booleans, null; dates as seconds since 2001, data as its byte count, UIDs as
// integers.
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

namespace wl {

// The property list in d[0, n) as JSON; false when it isn't a well-formed binary property list.
bool parseBinaryPlist(const uint8_t *d, size_t n, nlohmann::json &out);

} // namespace wl
