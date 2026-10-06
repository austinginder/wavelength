#pragma once
// Bounds checks for readers of files from anywhere (songs, packages, projects, presets): sizes and
// offsets read from a file can be anything, so `at + len <= size` must never wrap around.

#include <cstddef>
#include <cstdint>
#include <limits>

namespace wl {

// [at, at + len) lies inside a buffer of `size` bytes, for any inputs.
inline bool fits(uint64_t at, uint64_t len, uint64_t size) { return at <= size && len <= size - at; }

// len = count * each, false when that overflows.
inline bool product(uint64_t count, uint64_t each, uint64_t &len) {
    if (each && count > std::numeric_limits<uint64_t>::max() / each) return false;
    len = count * each;
    return true;
}

// count items of `each` bytes at `at` lie inside the buffer.
inline bool fitsItems(uint64_t at, uint64_t count, uint64_t each, uint64_t size) {
    uint64_t len;
    return product(count, each, len) && fits(at, len, size);
}

} // namespace wl
