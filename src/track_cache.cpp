#include "track_cache.hpp"

#include "platform.hpp"
#include "sha256.hpp"
#include "song.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

namespace fs = std::filesystem;

namespace wl::trackcache {

namespace {

using json = nlohmann::json;

constexpr int kFormat = 1;   // bump when what a cached entry holds changes

fs::path dir() { return platform::cacheDir() / "tracks"; }

// a file's identity for the key: size and modification time (0 when missing)
json stamp(const fs::path &p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    const long long mt = ec ? 0 : (long long)t.time_since_epoch().count();
    const auto size = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0;
    return json::array({(long long)(ec ? 0 : size), mt});
}

// this binary: a rebuild (a DSP change during development) must not reuse older audio
const json &engineStamp() {
    static const json s = [] {
        json e = {{"version", WAVELENGTH_VERSION}, {"format", kFormat}};
        const std::string self = platform::selfExecutable();
        if (!self.empty()) e["binary"] = stamp(self);
        return e;
    }();
    return s;
}

// the track without what only the mixer uses: its fader, pan, mute, sends, output bus and their curves
json soundOf(const json &track) {
    json t = track;
    if (!t.is_object()) return t;
    for (const char *k : {"gain", "pan", "panLaw", "mute", "solo", "sends", "output", "stem", "harmony", "range"}) t.erase(k);
    if (t.contains("automation") && t["automation"].is_object())
        for (const char *k : {"gain", "rides", "pan", "sends"}) t["automation"].erase(k);
    return t;
}

// names of the tracks whose notes a duck or gate on this track follows
std::set<std::string> triggersOf(const json &track) {
    std::set<std::string> out;
    if (track.is_object() && track.contains("fx") && track["fx"].is_array())
        for (auto &fx : track["fx"])
            if (fx.is_object() && fx.contains("trigger") && fx["trigger"].is_string()) out.insert(fx["trigger"].get<std::string>());
    return out;
}

} // namespace

std::vector<std::string> keys(const Job &job, const std::vector<std::set<size_t>> &deps, const std::set<size_t> &uncached, size_t frames) {
    const size_t n = job.tracks.size();
    std::map<std::string, size_t> byName;
    for (size_t i = 0; i < n; ++i) byName[job.tracks[i].name] = i;
    std::vector<std::string> out(n);
    std::vector<int> state(n, 0);   // 0 not yet, 1 in progress, 2 done
    std::function<const std::string &(size_t)> keyOf = [&](size_t i) -> const std::string & {
        if (state[i] == 2 || state[i] == 1) return out[i];   // a loop can't happen (render checks), but never recurse forever
        state[i] = 1;
        const Track &t = job.tracks[i];
        bool ok = !uncached.count(i);
        json k = {{"engine", engineStamp()}, {"settings", job.settings}, {"frames", (unsigned long long)frames}, {"track", soundOf(t.source)}};
        json files = json::array();
        for (auto &[where, path] : jobFileRefs(json{{"tracks", json::array({t.source})}})) {
            const fs::path p = fs::u8path(path);
            files.push_back({path, stamp(p.is_absolute() ? p : fs::u8path(job.baseDir) / p)});
        }
        k["files"] = files;
        json dk = json::array();
        for (size_t d : deps[i]) {
            const std::string &x = keyOf(d);
            if (x.empty()) ok = false;
            dk.push_back(x);
        }
        k["deps"] = dk;
        json tk = json::array();
        for (const std::string &name : triggersOf(t.source)) {
            auto it = byName.find(name);
            if (it != byName.end()) tk.push_back(soundOf(job.tracks[it->second].source));
        }
        k["triggers"] = tk;
        out[i] = ok ? sha256Hex(k.dump(-1, ' ', false, json::error_handler_t::replace)) : "";
        state[i] = 2;
        return out[i];
    };
    for (size_t i = 0; i < n; ++i) keyOf(i);
    return out;
}

bool load(const std::string &key, size_t frames, Audio &audio, json &result) {
    if (key.empty()) return false;
    const fs::path pcm = dir() / (key + ".pcm"), js = dir() / (key + ".json");
    std::error_code ec;
    if (fs::file_size(pcm, ec) != frames * 2 * sizeof(float) || ec) return false;
    std::ifstream jin(js, std::ios::binary);
    result = jin ? json::parse(jin, nullptr, false) : json();
    if (!result.is_object() || !result.value("ok", false)) return false;
    audio.resize(frames);
    std::ifstream pin(pcm, std::ios::binary);
    pin.read(reinterpret_cast<char *>(audio.left.data()), (std::streamsize)(frames * sizeof(float)));
    pin.read(reinterpret_cast<char *>(audio.right.data()), (std::streamsize)(frames * sizeof(float)));
    if (!pin) return false;
    const auto now = fs::file_time_type::clock::now();   // recently used: pruned last
    fs::last_write_time(pcm, now, ec);
    fs::last_write_time(js, now, ec);
    return true;
}

namespace {

// write through a temporary name, so a render in another process never reads half a file
bool publish(const fs::path &tmp, const fs::path &to) {
    std::error_code ec;
    fs::rename(tmp, to, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

std::string tmpSuffix() { return ".tmp" + std::to_string(platform::processId()); }

} // namespace

void store(const std::string &key, const Audio &audio, const json &result) {
    if (key.empty()) return;
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path pcm = dir() / (key + ".pcm"), js = dir() / (key + ".json");
    {
        std::ofstream o(pcm.string() + tmpSuffix(), std::ios::binary);
        o.write(reinterpret_cast<const char *>(audio.left.data()), (std::streamsize)(audio.frames() * sizeof(float)));
        o.write(reinterpret_cast<const char *>(audio.right.data()), (std::streamsize)(audio.frames() * sizeof(float)));
        if (!o) { o.close(); fs::remove(pcm.string() + tmpSuffix(), ec); return; }
    }
    {
        std::ofstream o(js.string() + tmpSuffix(), std::ios::binary);
        o << result.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    publish(js.string() + tmpSuffix(), js);   // the JSON first: load() checks the audio's size, then reads both
    publish(pcm.string() + tmpSuffix(), pcm);
}

void storeFiles(const std::string &key, const std::string &prefix) {
    if (key.empty()) return;
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path pcm = dir() / (key + ".pcm"), js = dir() / (key + ".json");
    fs::copy_file(prefix + ".json", js.string() + tmpSuffix(), fs::copy_options::overwrite_existing, ec);
    if (ec || !publish(js.string() + tmpSuffix(), js)) return;
    fs::rename(prefix + ".pcm", pcm.string() + tmpSuffix(), ec);   // same volume: a move; else copy
    if (ec) { ec.clear(); fs::copy_file(prefix + ".pcm", pcm.string() + tmpSuffix(), fs::copy_options::overwrite_existing, ec); }
    if (!ec) publish(pcm.string() + tmpSuffix(), pcm);
}

void prune() {
    double capGb = 4;
    if (const char *e = std::getenv("WAVELENGTH_TRACK_CACHE_GB")) capGb = std::max(0.0, std::atof(e));
    const uintmax_t cap = (uintmax_t)(capGb * 1e9);
    struct Entry { fs::file_time_type t; uintmax_t size; std::string key; };
    std::map<std::string, Entry> entries;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir(), ec)) {
        const std::string name = e.path().filename().string();
        const size_t dot = name.find('.');
        if (dot == std::string::npos || name.find(".tmp") != std::string::npos) continue;
        std::error_code e2;
        Entry &x = entries[name.substr(0, dot)];
        x.key = name.substr(0, dot);
        x.size += e.file_size(e2);
        x.t = std::max(x.t, e.last_write_time(e2));
    }
    std::vector<Entry> list;
    uintmax_t total = 0;
    for (auto &[k, e] : entries) { list.push_back(e); total += e.size; }
    std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) { return a.t < b.t; });
    for (auto &e : list) {
        if (total <= cap) break;
        fs::remove(dir() / (e.key + ".pcm"), ec);
        fs::remove(dir() / (e.key + ".json"), ec);
        total -= std::min(total, e.size);
    }
}

} // namespace wl::trackcache
