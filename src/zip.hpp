#pragma once
// Minimal zip reader (stored or deflated entries, no zip64): Bitwig .multisample instruments and
// DAWproject files are zips.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace wl {

struct ZipEntry { std::string name; uint16_t method; uint32_t compSize, size, localOffset; };

namespace zipdetail {
inline uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
std::string lower(std::string s);
bool inflateRaw(const std::vector<uint8_t> &in, std::vector<uint8_t> &out);
}

class Zip {
public:
    // entry names, in archive order
    std::vector<std::string> names() const { std::vector<std::string> n; for (auto &e : entries_) n.push_back(e.name); return n; }
    bool open(const std::string &path, std::string &err) {
        path_ = path;
        f_.open(path, std::ios::binary);
        if (!f_) { err = "cannot open " + path; return false; }
        f_.seekg(0, std::ios::end);
        const size_t size = (size_t)f_.tellg();
        const size_t tail = std::min<size_t>(size, 65536 + 22);
        std::vector<uint8_t> buf(tail);
        f_.seekg((std::streamoff)(size - tail));
        f_.read(reinterpret_cast<char *>(buf.data()), (std::streamsize)tail);
        long eocd = -1;
        for (long i = (long)tail - 22; i >= 0; --i)
            if (zipdetail::u32(&buf[i]) == 0x06054b50) { eocd = i; break; }
        if (eocd < 0) { err = path + " is not a zip archive"; return false; }
        const uint32_t cdSize = zipdetail::u32(&buf[eocd + 12]), cdOffset = zipdetail::u32(&buf[eocd + 16]);
        std::vector<uint8_t> cd(cdSize);
        f_.seekg(cdOffset);
        f_.read(reinterpret_cast<char *>(cd.data()), cdSize);
        for (size_t p = 0; p + 46 <= cd.size() && zipdetail::u32(&cd[p]) == 0x02014b50;) {
            ZipEntry e;
            e.method = zipdetail::u16(&cd[p + 10]);
            e.compSize = zipdetail::u32(&cd[p + 20]);
            e.size = zipdetail::u32(&cd[p + 24]);
            const uint16_t nameLen = zipdetail::u16(&cd[p + 28]), extraLen = zipdetail::u16(&cd[p + 30]), commentLen = zipdetail::u16(&cd[p + 32]);
            e.localOffset = zipdetail::u32(&cd[p + 42]);
            e.name.assign(reinterpret_cast<const char *>(&cd[p + 46]), nameLen);
            entries_.push_back(e);
            p += 46 + nameLen + extraLen + commentLen;
        }
        return true;
    }
    bool read(const std::string &name, std::vector<uint8_t> &out, std::string &err) {
        const ZipEntry *e = nullptr;
        for (auto &x : entries_) if (x.name == name) { e = &x; break; }
        if (!e) for (auto &x : entries_) if (zipdetail::lower(x.name) == zipdetail::lower(name)) { e = &x; break; }
        if (!e) { err = path_ + " has no entry '" + name + "'"; return false; }
        uint8_t lh[30];
        f_.seekg(e->localOffset);
        f_.read(reinterpret_cast<char *>(lh), 30);
        if (zipdetail::u32(lh) != 0x04034b50) { err = "bad zip entry header for " + name; return false; }
        f_.seekg(e->localOffset + 30 + zipdetail::u16(lh + 26) + zipdetail::u16(lh + 28));
        std::vector<uint8_t> comp(e->compSize);
        f_.read(reinterpret_cast<char *>(comp.data()), e->compSize);
        if (e->method == 0) { out = std::move(comp); return true; }
        if (e->method != 8) { err = name + ": unsupported zip compression " + std::to_string(e->method); return false; }
        out.resize(e->size);
        if (!zipdetail::inflateRaw(comp, out)) { err = name + ": corrupt deflate data"; return false; }
        return true;
    }
private:
    std::string path_;
    std::ifstream f_;
    std::vector<ZipEntry> entries_;
};

} // namespace wl
