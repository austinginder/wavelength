#include "zip.hpp"

#include <zlib.h>

#include <algorithm>

namespace wl::zipdetail {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool inflateRaw(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = const_cast<Bytef *>(in.data()); zs.avail_in = (uInt)in.size();
    zs.next_out = out.data(); zs.avail_out = (uInt)out.size();
    const int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

} // namespace wl::zipdetail
