#include "bitwig.hpp"

#include "vst2_abi.hpp"
#include "zip.hpp"

#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <set>
#include <unordered_map>

namespace wl::bitwig {

namespace {

// ---- the field stream --------------------------------------------------------------------------
// After a 0x2c-byte-aligned header ("BtWg" + hex offsets) and a metadata block, the body is one
// object: u32 class id, then fields (u32 field id, u8 type, value) until a u32 0. Some classes
// write a second group of fields after that 0 (a base-class part: those are followed by one 0 byte
// after the first group), class id 1 is a reference to an earlier object, and each object (and each
// type-0x14 value) takes the next object number. The class tables below were learned from 745
// projects saved by Bitwig 4 to 6; 743 of them read completely.

struct Obj;
struct Val {
    uint8_t type = 0;
    int64_t i = 0;
    double d = 0;
    std::string s;
    Obj *obj = nullptr;              // 0x09, 0x1a
    std::vector<Obj *> list;         // 0x12
};
struct Obj {
    uint32_t cls = 0;
    uint32_t ref = 0;                // class id 1: the object numbered `ref`
    std::vector<std::pair<uint32_t, Val>> fields;
};

const std::set<uint32_t> kTwoLevel = {144, 71, 138, 1833, 109, 163, 573, 4725, 4116, 422, 238};
const std::set<uint32_t> kPadAfterFirst = {477, 144, 71, 138, 1833, 109, 163, 573, 4725, 4116, 422, 238};
const std::set<uint32_t> kPadAfterFirstV6 = {215};   // format 0xc0 and later

struct Reader {
    const std::vector<uint8_t> &d;
    size_t pos = 0;
    int format = 0;
    std::deque<Obj> pool;
    std::unordered_map<uint32_t, Obj *> byNumber;
    uint32_t next = 1;
    std::string err;
    int depth = 0;

    explicit Reader(const std::vector<uint8_t> &data) : d(data) {}
    bool need(size_t n) {
        if (pos + n > d.size()) { if (err.empty()) err = "unexpected end of file"; return false; }
        return true;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        const uint32_t v = (uint32_t)d[pos] << 24 | (uint32_t)d[pos + 1] << 16 | (uint32_t)d[pos + 2] << 8 | d[pos + 3];
        pos += 4;
        return v;
    }
    uint8_t u8() { return need(1) ? d[pos++] : 0; }
    uint64_t u64() { const uint64_t hi = u32(); return hi << 32 | u32(); }
    bool fail(const std::string &m) { if (err.empty()) { char b[32]; std::snprintf(b, sizeof b, " at byte %zu", pos); err = m + b; } return false; }
    bool str(std::string &out) {
        uint32_t n = u32();
        if (n & 0x80000000u) {   // UTF-16BE
            n &= 0x7fffffff;
            if (!need(2 * (size_t)n)) return false;
            out.clear();
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t c = (uint32_t)d[pos] << 8 | d[pos + 1];
                pos += 2;
                if (c < 0x80) out += (char)c;
                else if (c < 0x800) { out += (char)(0xc0 | c >> 6); out += (char)(0x80 | (c & 0x3f)); }
                else { out += (char)(0xe0 | c >> 12); out += (char)(0x80 | (c >> 6 & 0x3f)); out += (char)(0x80 | (c & 0x3f)); }
            }
            return true;
        }
        if (n > 50'000'000 || !need(n)) return fail("bad string length");
        out.assign(reinterpret_cast<const char *>(&d[pos]), n);
        pos += n;
        return true;
    }
    bool value(uint8_t t, Val &v) {
        v.type = t;
        switch (t) {
        case 0x00: case 0x0a: return true;
        case 0x01: case 0x05: v.i = u8(); return err.empty();
        case 0x02: { if (!need(2)) return false; v.i = (int16_t)(d[pos] << 8 | d[pos + 1]); pos += 2; return true; }
        case 0x03: v.i = (int32_t)u32(); return err.empty();
        case 0x04: v.i = (int64_t)u64(); return err.empty();
        case 0x06: { const uint32_t b = u32(); float f; std::memcpy(&f, &b, 4); v.d = f; return err.empty(); }
        case 0x07: { const uint64_t b = u64(); std::memcpy(&v.d, &b, 8); return err.empty(); }
        case 0x08: return str(v.s);
        case 0x09: v.obj = object(); return v.obj != nullptr;
        case 0x0b: v.i = u32(); return err.empty();
        case 0x0d: { const uint32_t n = u32(); if (!need(n)) return false; pos += n; return true; }
        case 0x12:
            for (;;) {
                const uint32_t c = u32();
                if (!err.empty()) return false;
                if (c == 3) return true;
                pos -= 4;
                Obj *o = object();
                if (!o) return false;
                v.list.push_back(o);
            }
        case 0x14: {   // a list of strings (user names) and a u32
            const uint8_t n = u8();
            for (uint8_t k = 0; k < n; ++k) { std::string s; if (!str(s)) return false; if (!k) v.s = s; }
            u32();
            ++next;
            return err.empty();
        }
        case 0x15: case 0x16: if (!need(16)) return false; pos += 16; return true;   // uuid, colour
        case 0x17: case 0x19: { const uint32_t n = u32(); if (!need(4 * (size_t)n)) return false; pos += 4 * (size_t)n; return true; }
        case 0x1a: { v.obj = object(); if (!v.obj) return false; return str(v.s); }   // object + its key
        default: return fail("unknown value type " + std::to_string(t));
        }
    }
    Obj *object() {
        if (++depth > 400) return fail("objects nested too deeply"), nullptr;
        const uint32_t cls = u32();
        if (!err.empty()) return nullptr;
        pool.emplace_back();
        Obj *o = &pool.back();
        o->cls = cls;
        if (cls == 1) { o->ref = u32(); --depth; return o; }
        if (cls == 0 || cls > 0x10000) return fail("bad class id " + std::to_string(cls)), nullptr;
        byNumber[next++] = o;
        const int levels = kTwoLevel.count(cls) ? 2 : 1;
        for (int level = 0;;) {
            const uint32_t f = u32();
            if (!err.empty()) return nullptr;
            if (f == 0) {
                if (level == 0 && (kPadAfterFirst.count(cls) || (format >= 0xc0 && kPadAfterFirstV6.count(cls)))) ++pos;
                if (++level < levels) continue;
                break;
            }
            if (f > 0x8000) return fail("bad field id"), nullptr;
            const uint8_t t = u8();
            o->fields.emplace_back(f, Val{});
            if (!value(t, o->fields.back().second)) return nullptr;
        }
        --depth;
        return o;
    }
    Obj *resolve(Obj *o) const {
        for (int k = 0; o && o->cls == 1 && k < 8; ++k) {
            auto it = byNumber.find(o->ref);
            o = it == byNumber.end() ? nullptr : it->second;
        }
        return o;
    }
};

const Val *field(const Obj *o, uint32_t f) {
    if (!o) return nullptr;
    for (auto &p : o->fields) if (p.first == f) return &p.second;
    return nullptr;
}
std::string sfield(const Obj *o, uint32_t f) { const Val *v = field(o, f); return v ? v->s : ""; }
Obj *ofield(const Reader &r, const Obj *o, uint32_t f) { const Val *v = field(o, f); return v ? r.resolve(v->obj) : nullptr; }

// Field ids (stable across Bitwig 4-6)
enum : uint32_t {
    F_DEVICE_NAME = 0x9a, F_ENABLED = 0xa3, F_NATIVE_ID = 0x99, F_CONTENTS = 0xa4, F_STATE = 0xbe5,
    F_VST_ID = 0xce4, F_CLAP_ID = 0x2ec9, F_SLOT_DEVICE = 0x197, F_PARAM_ID = 0x2b9, F_PARAM_LIST = 0x20c,
    F_NUM = 0x136, F_ENUM = 0x273, F_BOOL = 0x12f, F_SUBCHAIN = 0x8e1, F_CHAIN_BODY = 0x349, F_CHAIN_DEVICES = 0x87,
    F_PADS = 0x8e0, F_PAD_KEY = 0x8e5, F_PAD_MIXER = 0x825, F_MIX_VOLUME = 0x821, F_MIX_PAN = 0x822, F_MIX_MUTE = 0x823,
    F_MULTI_ZONES = 0x76d, F_ZONE_BODY = 0x76e, F_MULTI_NAME = 0xfb3,
    F_SAMPLE_ZONE = 0x74c, F_ZONE_SAMPLE = 0x748, F_SAMPLE_FILE = 0x129e, F_FILE_PACKAGE_PATH = 0xcd4, F_ZONE_ROOT = 0x75c,
    F_TRACKS = 0x4de, F_EFFECT_TRACKS = 0x4df, F_MASTER = 0x4e0, F_TRACK_NAME = 0x15b, F_TRACK_CHAIN = 0x164, F_CHAIN_SLOTS = 0x144,
};

bool paramValue(const Obj *p, double &out) {
    for (auto &f : p->fields) {
        if (f.first == F_PARAM_ID) continue;
        const Val &v = f.second;
        if (v.type == 0x07 || v.type == 0x06) { out = v.d; return true; }
        if (v.type == 0x01 || v.type == 0x02 || v.type == 0x03 || v.type == 0x05) { out = (double)v.i; return true; }
        return false;
    }
    return false;
}

std::vector<Device> chainDevices(const Reader &r, const Obj *chain);

bool device(const Reader &r, const Obj *o, Device &dv) {
    if (!o) return false;
    if (!field(o, F_DEVICE_NAME)) {   // a slot around the device
        o = ofield(r, o, F_SLOT_DEVICE);
        if (!o || !field(o, F_DEVICE_NAME)) return false;
    }
    dv.name = sfield(o, F_DEVICE_NAME);
    if (const Val *en = field(o, F_ENABLED)) dv.enabled = en->i != 0;
    if (const Val *st = field(o, F_STATE); st && !st->s.empty()) dv.state = "plugin-states/" + st->s;
    if (const Val *c = field(o, F_CLAP_ID)) { dv.kind = "clap"; dv.pluginId = c->s; }
    else if (const Val *v = field(o, F_VST_ID)) {
        if (v->type == 0x08) { dv.kind = "vst3"; dv.pluginId = v->s; }
        else { dv.kind = "vst2"; dv.pluginId = vst2::fourcc((int32_t)v->i); }
    } else dv.kind = "native";
    if (dv.kind != "native") return true;
    const Obj *contents = ofield(r, o, F_CONTENTS);
    const Val *list = field(contents, F_PARAM_LIST);
    if (!list) return true;
    for (Obj *pp : list->list) {
        const Obj *p = r.resolve(pp);
        if (!p) continue;
        const std::string id = sfield(p, F_PARAM_ID);
        if (const Obj *sub = ofield(r, p, F_SUBCHAIN)) { dv.chains[id] = chainDevices(r, sub); continue; }
        if (const Val *pads = field(p, F_PADS); pads && id == "DRUM_PADS") {
            for (Obj *po : pads->list) {
                const Obj *pad = r.resolve(po);
                if (!pad) continue;
                Device::Pad pd;
                if (const Val *k = field(pad, F_PAD_KEY)) pd.key = (int)k->i;
                else if (sfield(pad, F_PARAM_ID).rfind("PAD", 0) == 0) pd.key = std::atoi(sfield(pad, F_PARAM_ID).c_str() + 3);
                if (const Obj *mix = ofield(r, pad, F_PAD_MIXER)) {
                    double v;
                    if (const Obj *x = ofield(r, mix, F_MIX_VOLUME); x && paramValue(x, v)) pd.volume = v;
                    if (const Obj *x = ofield(r, mix, F_MIX_PAN); x && paramValue(x, v)) pd.pan = v;
                    if (const Obj *x = ofield(r, mix, F_MIX_MUTE); x && paramValue(x, v)) pd.mute = v != 0;
                }
                pd.devices = chainDevices(r, pad);
                dv.pads.push_back(std::move(pd));
            }
            continue;
        }
        if (id == "SAMPLE" && dv.name == "Sampler") {   // the first zone's sample
            const Obj *zone = ofield(r, p, F_SAMPLE_ZONE);
            if (const Val *zones = field(zone, F_MULTI_ZONES)) {   // a multisample: its name, and the first zone
                dv.multisample = sfield(zone, F_MULTI_NAME);
                zone = zones->list.empty() ? nullptr : ofield(r, r.resolve(zones->list[0]), F_ZONE_BODY);
            }
            const Obj *file = ofield(r, ofield(r, zone, F_ZONE_SAMPLE), F_SAMPLE_FILE);
            dv.sample = sfield(file, F_FILE_PACKAGE_PATH);
            if (dv.sample.empty() && file)   // a file of the user's own: the first absolute path in it
                for (auto &f : file->fields)
                    if (f.second.type == 0x08 && (f.second.s.rfind("/", 0) == 0 || (f.second.s.size() > 2 && f.second.s[1] == ':'))) { dv.sample = f.second.s; break; }
            if (const Val *root = field(zone, F_ZONE_ROOT)) dv.sampleRoot = (int)root->i;
            continue;
        }
        double v;
        if (!id.empty() && paramValue(p, v)) dv.params[id] = v;
    }
    return true;
}

// a chain object: 0x349 -> 0x87 list of devices (or the list directly)
std::vector<Device> chainDevices(const Reader &r, const Obj *chain) {
    std::vector<Device> out;
    const Obj *body = ofield(r, chain, F_CHAIN_BODY);
    const Val *list = field(body ? body : chain, F_CHAIN_DEVICES);
    if (!list) return out;
    for (Obj *x : list->list) {
        Device dv;
        if (device(r, r.resolve(x), dv)) out.push_back(std::move(dv));
    }
    return out;
}

Track track(const Reader &r, const Obj *t) {
    Track tr;
    tr.name = sfield(t, F_TRACK_NAME);
    const Obj *chain = ofield(r, t, F_TRACK_CHAIN);
    if (const Val *slots = field(chain, F_CHAIN_SLOTS))
        for (Obj *s : slots->list) {
            Device dv;
            if (device(r, r.resolve(s), dv)) tr.devices.push_back(std::move(dv));
        }
    return tr;
}

bool readAll(const std::string &path, std::vector<uint8_t> &d, std::string &err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    d.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

} // namespace

bool load(const std::string &path, Project &out, std::string &err) {
    std::vector<uint8_t> d;
    if (!readAll(path, d, err)) return false;
    if (d.size() < 40 || std::memcmp(d.data(), "BtWg", 4) != 0) { err = path + " is not a Bitwig project"; return false; }
    auto hex = [&](size_t at, size_t n) { return std::strtoul(std::string(d.begin() + (long)at, d.begin() + (long)(at + n)).c_str(), nullptr, 16); };
    out = Project{};
    out.path = path;
    out.format = (int)hex(12, 4);
    Reader r(d);
    r.format = out.format;
    r.pos = hex(16, 8);
    const Obj *root = r.object();
    if (!root) { err = path + ": can't read this Bitwig project (" + r.err + ")"; return false; }
    if (const Val *ts = field(root, F_TRACKS)) for (Obj *t : ts->list) if (const Obj *x = r.resolve(t)) out.tracks.push_back(track(r, x));
    if (const Val *ts = field(root, F_EFFECT_TRACKS)) for (Obj *t : ts->list) if (const Obj *x = r.resolve(t)) out.effects.push_back(track(r, x));
    if (const Val *m = field(root, F_MASTER)) {
        const Obj *mo = nullptr;
        if (m->type == 0x0b) { auto it = r.byNumber.find((uint32_t)m->i); if (it != r.byNumber.end()) mo = r.resolve(it->second); }
        else mo = r.resolve(m->obj);
        if (mo) { out.master = track(r, mo); out.hasMaster = true; }
    }
    return true;
}

bool readState(const Project &p, const std::string &entry, std::vector<uint8_t> &data, std::string &err) {
    Zip z;
    if (!z.open(p.path, err)) return false;
    return z.read(entry, data, err);
}

} // namespace wl::bitwig
