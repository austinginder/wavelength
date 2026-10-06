#include "bplist.hpp"
#include "bytes.hpp"

#include <cstring>
#include <string>
#include <vector>

using nlohmann::json;

namespace wl {

namespace {
constexpr size_t kMaxVisits = 1 << 20;   // far above any real plist, small enough to fail fast

struct Reader {
    const uint8_t *d;
    size_t n, refSize = 1;
    std::vector<uint64_t> offsets;
    int depth = 0;
    bool keepData = false;   // data objects as their bytes (JSON binary), else their size
    bool markUids = false;   // UIDs as {"CF$UID": n}, else plain integers
    size_t visits = 0;       // objects read: shared references can't multiply into an exponential walk (last: the
                             // reader is built by position)

    uint64_t be(size_t at, size_t bytes) const {
        uint64_t v = 0;
        for (size_t i = 0; i < bytes; ++i) v = v << 8 | d[at + i];
        return v;
    }
    // an object's count: the low nibble, or 0xF and an integer object after the marker
    bool count(size_t &at, uint8_t marker, uint64_t &c) const {
        c = marker & 0x0F;
        if (c != 0x0F) return true;
        if (at >= n || (d[at] & 0xF0) != 0x10) return false;
        const size_t bytes = (size_t)1 << (d[at] & 0x0F);
        if (bytes > 8 || !fits(at + 1, bytes, n)) return false;
        c = be(at + 1, bytes);
        at += 1 + bytes;
        return true;
    }
    bool object(uint64_t ref, json &out) {
        if (ref >= offsets.size() || ++depth > 64 || ++visits > kMaxVisits) return false;
        size_t at = (size_t)offsets[ref];
        if (at >= n) return false;
        const uint8_t m = d[at++];
        bool ok = true;
        switch (m >> 4) {
        case 0x0: out = m == 0x08 ? json(false) : m == 0x09 ? json(true) : json(nullptr); break;
        case 0x1: {   // integer: 1, 2, 4, 8 bytes (8 bytes signed)
            const size_t bytes = (size_t)1 << (m & 0x0F);
            if (bytes > 16 || !fits(at, bytes, n)) { ok = false; break; }
            const uint64_t v = be(at + (bytes > 8 ? bytes - 8 : 0), bytes > 8 ? 8 : bytes);
            out = bytes >= 8 ? json((int64_t)v) : json(v);
            break;
        }
        case 0x2: case 0x3: {   // real, or a date (a float64 of seconds since 2001)
            const size_t bytes = (m >> 4) == 0x3 ? 8 : (size_t)1 << (m & 0x0F);
            if (!fits(at, bytes, n) || (bytes != 4 && bytes != 8)) { ok = false; break; }
            if (bytes == 4) { const uint32_t b = (uint32_t)be(at, 4); float f; std::memcpy(&f, &b, 4); out = (double)f; }
            else { const uint64_t b = be(at, 8); double f; std::memcpy(&f, &b, 8); out = f; }
            break;
        }
        case 0x4: {   // data: its size, or its bytes
            uint64_t c;
            ok = count(at, m, c) && fits(at, c, n);
            if (ok) out = keepData ? json::binary(std::vector<uint8_t>(d + at, d + at + c)) : json(c);
            break;
        }
        case 0x5: {   // ASCII string
            uint64_t c;
            ok = count(at, m, c) && fits(at, c, n);
            if (ok) out = std::string(reinterpret_cast<const char *>(d + at), (size_t)c);
            break;
        }
        case 0x6: {   // UTF-16 big-endian string
            uint64_t c;
            ok = count(at, m, c) && fitsItems(at, c, 2, n);
            if (!ok) break;
            std::string s;
            for (uint64_t i = 0; i < c; ++i) {
                uint32_t u = (uint32_t)be(at + 2 * i, 2);
                if (u >= 0xD800 && u < 0xDC00 && i + 1 < c) {
                    const uint32_t lo = (uint32_t)be(at + 2 * (i + 1), 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); ++i; }
                }
                if (u < 0x80) s += (char)u;
                else if (u < 0x800) { s += (char)(0xC0 | u >> 6); s += (char)(0x80 | (u & 0x3F)); }
                else if (u < 0x10000) { s += (char)(0xE0 | u >> 12); s += (char)(0x80 | (u >> 6 & 0x3F)); s += (char)(0x80 | (u & 0x3F)); }
                else { s += (char)(0xF0 | u >> 18); s += (char)(0x80 | (u >> 12 & 0x3F)); s += (char)(0x80 | (u >> 6 & 0x3F)); s += (char)(0x80 | (u & 0x3F)); }
            }
            out = s;
            break;
        }
        case 0x8: {   // UID
            const size_t bytes = (size_t)(m & 0x0F) + 1;
            ok = fits(at, bytes, n) && bytes <= 8;
            if (ok) out = markUids ? json{{"CF$UID", be(at, bytes)}} : json(be(at, bytes));
            break;
        }
        case 0xA: case 0xC: {   // array, set
            uint64_t c;
            ok = count(at, m, c) && fitsItems(at, c, refSize, n);
            out = json::array();
            for (uint64_t i = 0; ok && i < c; ++i) {
                json v;
                ok = object(be(at + i * refSize, refSize), v);
                out.push_back(std::move(v));
            }
            break;
        }
        case 0xD: {   // dictionary: the key refs, then the value refs
            uint64_t c;
            ok = count(at, m, c) && fitsItems(at, c, 2 * refSize, n);
            out = json::object();
            for (uint64_t i = 0; ok && i < c; ++i) {
                json k, v;
                ok = object(be(at + i * refSize, refSize), k) && object(be(at + (c + i) * refSize, refSize), v) && k.is_string();
                if (ok) out[k.get<std::string>()] = std::move(v);
            }
            break;
        }
        default: ok = false;
        }
        --depth;
        return ok;
    }
};
} // namespace

bool parseBinaryPlist(const uint8_t *d, size_t n, json &out, bool keepData, bool markUids) {
    if (n < 8 + 32 || std::memcmp(d, "bplist0", 7)) return false;
    Reader r{d, n, 1, {}, 0, keepData, markUids};
    const uint8_t *t = d + n - 32;   // trailer: 6 unused, offset size, ref size, object count, top object, table offset
    const size_t offSize = t[6];
    r.refSize = t[7];
    const uint64_t count = r.be(n - 24, 8), top = r.be(n - 16, 8), table = r.be(n - 8, 8);
    if (!offSize || offSize > 8 || !r.refSize || r.refSize > 8 || count > n || !fitsItems(table, count, offSize, n - 32)) return false;
    for (uint64_t i = 0; i < count; ++i) r.offsets.push_back(r.be((size_t)(table + i * offSize), offSize));
    return r.object(top, out);
}

namespace {
json follow(const json &objs, const json &v, int depth, size_t &budget) {
    if (depth > 64 || budget == 0) return nullptr;
    --budget;
    if (v.is_object() && v.size() == 1 && v.contains("CF$UID")) {
        const uint64_t i = v["CF$UID"].get<uint64_t>();
        return i < objs.size() ? follow(objs, objs[i], depth + 1, budget) : json(nullptr);
    }
    if (v.is_string()) return v == "$null" ? json(nullptr) : v;
    if (v.is_array()) { json a = json::array(); for (auto &x : v) a.push_back(follow(objs, x, depth + 1, budget)); return a; }
    if (!v.is_object()) return v;
    if (v.contains("NS.keys") && v.contains("NS.objects") && v["NS.keys"].is_array() && v["NS.objects"].is_array()) {
        json o = json::object();
        for (size_t k = 0; k < v["NS.keys"].size() && k < v["NS.objects"].size(); ++k) {
            const json key = follow(objs, v["NS.keys"][k], depth + 1, budget);
            o[key.is_string() ? key.get<std::string>() : key.dump()] = follow(objs, v["NS.objects"][k], depth + 1, budget);
        }
        return o;
    }
    if (v.contains("NS.objects") && v["NS.objects"].is_array()) return follow(objs, v["NS.objects"], depth + 1, budget);
    if (v.contains("NS.string")) return follow(objs, v["NS.string"], depth + 1, budget);
    json o = json::object();
    for (auto &[k, x] : v.items()) if (k != "$class") o[k] = follow(objs, x, depth + 1, budget);
    return o;
}
} // namespace

bool parseKeyedArchive(const uint8_t *d, size_t n, json &out) {
    json p;
    if (!parseBinaryPlist(d, n, p, false, true) || !p.is_object() || !p.contains("$objects") || !p.contains("$top") || !p["$objects"].is_array()) return false;
    const json &top = p["$top"];
    size_t budget = kMaxVisits;
    out = follow(p["$objects"], top.contains("root") ? top["root"] : top, 0, budget);
    return budget > 0;   // a run-out budget: references that multiply past any real archive
}

} // namespace wl
