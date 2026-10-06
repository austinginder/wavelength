#include "plugin.hpp"

#include "bundle.hpp"
#include "clap_plugin.hpp"
#include "au_plugin.hpp"
#include "vst2_plugin.hpp"
#include "vst3_plugin.hpp"

#include <algorithm>
#include <thread>

namespace wl {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace

bool Plugin::findParam(const std::string &key, ParamInfo &out) const {
    const auto all = params();
    if (!key.empty() && key[0] == '#') {
        const ParamId want = (ParamId)std::stoul(key.substr(1));
        for (const auto &p : all) if (p.id == want) { out = p; return true; }
        return false;
    }
    const std::string k = lower(key);
    for (const auto &p : all) if (lower(p.name) == k) { out = p; return true; }
    for (const auto &p : all) if (lower(p.module + "/" + p.name) == k) { out = p; return true; }
    return false;
}

std::unique_ptr<Plugin> createPlugin(const PluginInfo &info, std::string &err) {
    if (info.format == "vst3") return Vst3Plugin::create(info, err);
    if (info.format == "vst2") return Vst2Plugin::create(info, err);
    if (info.format == "au") return AuPlugin::create(info, err);
    return ClapPlugin::create(info, err);
}

Pacer::Pacer(bool on, const std::vector<TimedEvent> &events, double sampleRate) : sr_(sampleRate) {
    if (!on) return;
    std::vector<const TimedEvent *> notes;
    for (const auto &e : events) if (e.kind == TimedEvent::Note) notes.push_back(&e);
    std::stable_sort(notes.begin(), notes.end(), [](const TimedEvent *a, const TimedEvent *b) { return a->frame < b->frame; });
    const int64_t before = (int64_t)(0.5 * sr_), after = (int64_t)(1.5 * sr_);
    int held = 0;
    for (const auto *e : notes) {
        if (e->on) {
            if (held++ == 0) {
                const int64_t start = std::max<int64_t>(0, e->frame - before);
                if (!windows_.empty() && start <= windows_.back().second) windows_.back().second = INT64_MAX;   // reopened
                else windows_.push_back({start, INT64_MAX});
            }
        } else if (held > 0 && --held == 0) windows_.back().second = e->frame + after;
    }
}

void Pacer::wait(int64_t pos) {
    size_t w = 0;
    while (w < windows_.size() && windows_[w].second <= pos) ++w;
    if (w == windows_.size() || pos < windows_[w].first) { current_ = SIZE_MAX; return; }
    const auto now = std::chrono::steady_clock::now();
    if (w != current_) { current_ = w; anchorPos_ = pos; anchorTime_ = now; return; }
    std::this_thread::sleep_until(anchorTime_ + std::chrono::microseconds((int64_t)((pos - anchorPos_) * 1e6 / sr_)));
}

} // namespace wl
