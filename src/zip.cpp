#include "zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

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

bool deflateRaw(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out.resize(deflateBound(&zs, (uLong)in.size()));
    zs.next_in = const_cast<Bytef *>(in.data()); zs.avail_in = (uInt)in.size();
    zs.next_out = out.data(); zs.avail_out = (uInt)out.size();
    const int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return rc == Z_STREAM_END;
}

uint32_t crc32(const std::vector<uint8_t> &data) { return (uint32_t)::crc32(0L, data.data(), (uInt)data.size()); }

} // namespace wl::zipdetail

namespace wl {

bool ZipWriter::write(const std::string &path, std::string &err) const {
    std::vector<uint8_t> out, central;
    auto u16 = [](std::vector<uint8_t> &v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); };
    auto u32 = [](std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); };
    for (const auto &f : files_) {
        std::vector<uint8_t> comp;
        uint16_t method = 8;
        if (!zipdetail::deflateRaw(f.data, comp) || comp.size() >= f.data.size()) { comp = f.data; method = 0; }   // store what doesn't shrink
        const uint32_t crc = zipdetail::crc32(f.data), offset = (uint32_t)out.size();
        auto header = [&](std::vector<uint8_t> &v, bool centralEntry) {
            u32(v, centralEntry ? 0x02014b50 : 0x04034b50);
            if (centralEntry) u16(v, 20);   // made by
            u16(v, 20); u16(v, 0x0800);     // version needed; flags: UTF-8 names
            u16(v, method); u16(v, 0); u16(v, 0x21);   // time 00:00, date 1980-01-01
            u32(v, crc); u32(v, (uint32_t)comp.size()); u32(v, (uint32_t)f.data.size());
            u16(v, (uint16_t)f.name.size()); u16(v, 0);
            if (centralEntry) { u16(v, 0); u16(v, 0); u16(v, 0); u32(v, 0); u32(v, offset); }
            v.insert(v.end(), f.name.begin(), f.name.end());
        };
        header(out, false);
        out.insert(out.end(), comp.begin(), comp.end());
        header(central, true);
    }
    const uint32_t cdOffset = (uint32_t)out.size();
    out.insert(out.end(), central.begin(), central.end());
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, (uint16_t)files_.size()); u16(out, (uint16_t)files_.size());
    u32(out, (uint32_t)central.size()); u32(out, cdOffset); u16(out, 0);
    std::ofstream o(std::filesystem::path(path), std::ios::binary);
    if (!o) { err = "cannot write " + path; return false; }
    o.write(reinterpret_cast<const char *>(out.data()), (std::streamsize)out.size());
    if (!o) { err = "write failed for " + path + " (disk full?)"; return false; }
    return true;
}

} // namespace wl
