#pragma once
// Minimal zip reader (stored or deflated entries, no zip64): Bitwig .multisample instruments and
// DAWproject files are zips. ZipWriter writes deflated entries (DAWproject export).
#include "bytes.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace wl {

struct ZipEntry { std::string name; uint16_t method; uint32_t compSize, size, localOffset; uint16_t flags = 0; uint32_t externalAttr = 0; uint32_t crc = 0; };

namespace zipdetail {
inline uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
std::string lower(std::string s);
bool inflateRaw(const std::vector<uint8_t> &in, std::vector<uint8_t> &out);
bool deflateRaw(const std::vector<uint8_t> &in, std::vector<uint8_t> &out);
uint32_t crc32(const std::vector<uint8_t> &data);
}

// Entries are collected in memory, then written as one archive (no zip64: under 4 GB).
class ZipWriter {
public:
    // store: keep the entry uncompressed (a `mimetype` entry, audio and images); otherwise deflated when that makes it smaller
    void add(const std::string &name, std::vector<uint8_t> data, bool store = false) { files_.push_back({name, std::move(data), store}); }
    void add(const std::string &name, const std::string &text, bool store = false) { add(name, std::vector<uint8_t>(text.begin(), text.end()), store); }
    bool write(const std::string &path, std::string &err) const;
private:
    struct File { std::string name; std::vector<uint8_t> data; bool store = false; };
    std::vector<File> files_;
};

class Zip {
public:
    // entry names, in archive order
    std::vector<std::string> names() const { std::vector<std::string> n; for (auto &e : entries_) n.push_back(e.name); return n; }
    const std::vector<ZipEntry> &entries() const { return entries_; }
    bool open(const std::string &path, std::string &err) {
        path_ = path;
        f_.open(path, std::ios::binary);
        if (!f_) { err = "cannot open " + path; return false; }
        f_.seekg(0, std::ios::end);
        const size_t size = (size_t)f_.tellg();
        size_ = size;
        const size_t tail = std::min<size_t>(size, 65536 + 22);
        std::vector<uint8_t> buf(tail);
        f_.seekg((std::streamoff)(size - tail));
        f_.read(reinterpret_cast<char *>(buf.data()), (std::streamsize)tail);
        long eocd = -1;
        for (long i = (long)tail - 22; i >= 0; --i)
            if (zipdetail::u32(&buf[i]) == 0x06054b50) { eocd = i; break; }
        if (eocd < 0) { err = path + " is not a zip archive"; return false; }
        const uint32_t cdSize = zipdetail::u32(&buf[eocd + 12]), cdOffset = zipdetail::u32(&buf[eocd + 16]);
        // a zip after other data (a Bitwig project's plugin states): offsets count from the zip's start
        const size_t eocdAt = size - tail + (size_t)eocd;
        base_ = eocdAt >= (size_t)cdSize + cdOffset ? eocdAt - cdSize - cdOffset : 0;
        if (!fits(base_ + (uint64_t)cdOffset, cdSize, eocdAt)) { err = path + ": its zip directory lies outside the file"; return false; }
        std::vector<uint8_t> cd(cdSize);
        f_.seekg((std::streamoff)(base_ + cdOffset));
        f_.read(reinterpret_cast<char *>(cd.data()), cdSize);
        if (!f_) { err = path + ": the zip ends inside its directory"; return false; }
        for (size_t p = 0; fits(p, 46, cd.size()) && zipdetail::u32(&cd[p]) == 0x02014b50;) {
            ZipEntry e;
            e.flags = zipdetail::u16(&cd[p + 8]);
            e.method = zipdetail::u16(&cd[p + 10]);
            e.crc = zipdetail::u32(&cd[p + 16]);
            e.externalAttr = zipdetail::u32(&cd[p + 38]);
            e.compSize = zipdetail::u32(&cd[p + 20]);
            e.size = zipdetail::u32(&cd[p + 24]);
            const uint16_t nameLen = zipdetail::u16(&cd[p + 28]), extraLen = zipdetail::u16(&cd[p + 30]), commentLen = zipdetail::u16(&cd[p + 32]);
            e.localOffset = zipdetail::u32(&cd[p + 42]);
            if (!fits(p + 46, (uint64_t)nameLen + extraLen + commentLen, cd.size())) { err = path + ": a zip directory entry runs past the directory"; return false; }
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
        uint8_t lh[30] = {};
        f_.seekg((std::streamoff)(base_ + e->localOffset));
        f_.read(reinterpret_cast<char *>(lh), 30);
        if (zipdetail::u32(lh) != 0x04034b50) { err = "bad zip entry header for " + name; return false; }
        // the local header must name the same file as the central directory (readers disagree otherwise)
        std::string localName(zipdetail::u16(lh + 26), '\0');
        f_.read(localName.data(), (std::streamsize)localName.size());
        if (localName != e->name) { err = "zip entry '" + e->name + "' is named '" + localName + "' in its local header"; return false; }
        const uint64_t dataAt = base_ + (uint64_t)e->localOffset + 30 + zipdetail::u16(lh + 26) + zipdetail::u16(lh + 28);
        if (!fits(dataAt, e->compSize, size_)) { err = name + ": the zip ends inside this entry"; return false; }
        // deflate expands at most about 1032:1, so a larger size in the header is a lie (or a zip bomb)
        if (e->method == 8 && e->size > (uint64_t)e->compSize * 1032 + 1024) { err = name + ": its zip header claims more data than it can hold"; return false; }
        f_.seekg((std::streamoff)dataAt);
        std::vector<uint8_t> comp(e->compSize);
        f_.read(reinterpret_cast<char *>(comp.data()), e->compSize);
        if (!f_) { err = name + ": the zip ends inside this entry"; return false; }
        if (e->method == 0) out = std::move(comp);
        else if (e->method != 8) { err = name + ": unsupported zip compression " + std::to_string(e->method); return false; }
        else {
            out.resize(e->size);
            if (!zipdetail::inflateRaw(comp, out)) { err = name + ": corrupt deflate data"; return false; }
        }
        if (zipdetail::crc32(out) != e->crc) { err = name + ": its CRC-32 doesn't match (a damaged zip)"; return false; }
        return true;
    }
private:
    std::string path_;
    std::ifstream f_;
    size_t base_ = 0, size_ = 0;
    std::vector<ZipEntry> entries_;
};

} // namespace wl
