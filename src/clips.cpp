#include "clips.hpp"

#include "apple_loops.hpp"
#include "audio_file.hpp"
#include "dsp.hpp"
#include "effects.hpp"
#include "harmony.hpp"
#include "sampler.hpp"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

using nlohmann::json;
namespace fs = std::filesystem;

namespace wl {

namespace {

struct Clip {
    Audio audio;            // source audio at the job's sample rate, trimmed and reversed
    double startSec = 0;    // where it lands
    double speed = 1;       // > 1 plays faster (shorter)
    double pitch = 0;       // semitones
    bool stretch = true;    // speed without changing pitch (else resample: pitch follows speed)
    double gainDb = 0, fadeInMs = 2, fadeOutMs = 5;
    size_t outFrames() const { return (size_t)std::llround((double)audio.frames() / speed); }
};

std::map<std::string, std::pair<Audio, int>> &fileCache() {
    static std::map<std::string, std::pair<Audio, int>> cache;
    return cache;
}

bool loadFile(const std::string &path, Audio &a, int &sr, std::string &err) {
    auto &c = fileCache();
    auto it = c.find(path);
    if (it != c.end()) { a = it->second.first; sr = it->second.second; return true; }
    if (!readAudio(path, a, sr, err)) return false;
    c[path] = {a, sr};
    return true;
}

// linear-interpolation resample by `ratio` (output frames = input / ratio)
Audio resample(const Audio &in, double ratio) {
    Audio out;
    const size_t n = (size_t)std::floor((double)in.frames() / ratio);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const double pos = (double)i * ratio;
        const size_t i0 = (size_t)pos;
        const double f = pos - (double)i0;
        const size_t i1 = std::min(i0 + 1, in.frames() - 1);
        out.left[i] = (float)(in.left[i0] * (1 - f) + in.left[i1] * f);
        out.right[i] = (float)(in.right[i0] * (1 - f) + in.right[i1] * f);
    }
    return out;
}

bool renderSpec(const json &f, ClipRender &r, double &tail, std::string &err) {
    static const std::set<std::string> known = {"render", "tracks", "tail", "fx"};
    for (auto &[k, v] : f.items())
        if (!known.count(k)) { err = "clip: unknown \"file\" setting '" + k + "' (a rendered file takes render, tracks, tail, fx)"; return false; }
    const json &rg = f.contains("render") ? f["render"] : json();
    if (!rg.is_array() || rg.size() != 2 || !rg[0].is_number() || !rg[1].is_number() || rg[1].get<double>() <= rg[0].get<double>()) {
        err = "clip: \"file\": {\"render\": [fromBeat, toBeat]} needs two beats, the second after the first";
        return false;
    }
    r.fromBeat = rg[0].get<double>();
    r.toBeat = rg[1].get<double>();
    r.tracks.clear();
    if (f.contains("tracks")) {
        if (!f["tracks"].is_array()) { err = "clip: \"tracks\" must be a list of track names"; return false; }
        for (auto &t : f["tracks"]) r.tracks.push_back(t.get<std::string>());
    }
    tail = std::clamp(f.value("tail", 0.0), 0.0, 60.0);
    return true;
}

bool parseClip(const Job &job, const json &c, Clip &clip, std::string &err, const Audio *rendered = nullptr) {
    static const std::set<std::string> known = {"file", "beat", "endAt", "bpm", "speed", "pitch", "stretch", "start", "length",
                                                "beats", "reverse", "gain", "fadeIn", "fadeOut", "repeat", "key"};
    for (auto &[k, v] : c.items())
        if (!known.count(k)) { err = "clip: unknown setting '" + k + "'"; return false; }
    Audio src;
    int sr = 0;
    std::string file;
    const AppleLoop *loop = nullptr;   // an Apple Loop knows its tempo and key
    if (c.contains("file") && c["file"].is_object()) {
        // the song's own audio, captured from the render graph, plus the tail, through the clip's own fx
        const json &f = c["file"];
        ClipRender r;
        double tail = 0;
        if (!renderSpec(f, r, tail, err)) return false;
        file = "render [" + std::to_string(r.fromBeat) + ", " + std::to_string(r.toBeat) + "]";
        sr = job.sampleRate;
        const size_t n = (size_t)std::llround((job.tempo.beatToSec(r.toBeat) - job.tempo.beatToSec(r.fromBeat) + tail) * sr);
        if (rendered) src = *rendered;
        src.left.resize(n, 0.f);   // the tail (or, while sizing the song, silence of the right length)
        src.right.resize(n, 0.f);
        if (rendered && f.contains("fx")) {
            const FxContext ctx{job, false, nullptr};
            for (size_t i = 0; i < f["fx"].size(); ++i) {
                if (f["fx"][i].is_object() && f["fx"][i].value("bypass", false)) continue;
                auto fx = makeEffect(f["fx"][i], job, "clip render fx[" + std::to_string(i) + "]", err);
                if (!fx || !fx->process(src, ctx, err)) return false;
            }
        }
    } else {
        file = c.value("file", "");
        const std::string path = resolveSampleFile(file, job.baseDir);
        if (path.empty()) { err = "clip: cannot find audio file '" + file + "'"; return false; }
        if (!loadFile(path, src, sr, err)) return false;
        std::string ext = fs::u8path(path).extension().string();
        for (auto &ch : ext) ch = (char)std::tolower((unsigned char)ch);
        if (ext == ".caf") loop = appleLoopAt(path);
    }
    // trim in the file's own time: start / length in seconds, or beats at the clip's own bpm (an Apple
    // Loop's own tempo unless "bpm" or "speed" is given: it follows the song's tempo, as in GarageBand)
    const double srcBpm = c.value("bpm", loop && loop->bpm > 0 && !c.contains("speed") ? loop->bpm : 0.0);
    const double start = std::max(0.0, c.value("start", 0.0));
    double length = c.value("length", 0.0);
    if (c.contains("beats")) {
        if (srcBpm <= 0) { err = "clip: \"beats\" needs the file's \"bpm\""; return false; }
        length = c["beats"].get<double>() * 60.0 / srcBpm;
    }
    const size_t a = std::min(src.frames(), (size_t)(start * sr));
    const size_t b = length > 0 ? std::min(src.frames(), a + (size_t)(length * sr)) : src.frames();
    Audio cut;
    cut.left.assign(src.left.begin() + (long)a, src.left.begin() + (long)b);
    cut.right.assign(src.right.begin() + (long)a, src.right.begin() + (long)b);
    if (c.value("reverse", false)) { std::reverse(cut.left.begin(), cut.left.end()); std::reverse(cut.right.begin(), cut.right.end()); }
    const int repeat = c.value("repeat", 1);
    if (repeat < 1 || repeat > 1000) { err = "clip: \"repeat\" is how many times it plays back to back (1-1000)"; return false; }
    if (repeat > 1) {
        const size_t n = cut.frames();
        cut.left.reserve(n * (size_t)repeat);
        cut.right.reserve(n * (size_t)repeat);
        for (int k = 1; k < repeat; ++k) {
            cut.left.insert(cut.left.end(), cut.left.begin(), cut.left.begin() + (long)n);
            cut.right.insert(cut.right.end(), cut.right.begin(), cut.right.begin() + (long)n);
        }
    }
    clip.audio = sr == job.sampleRate ? std::move(cut) : resample(cut, (double)sr / job.sampleRate);
    if (clip.audio.frames() == 0) { err = "clip: '" + file + "' is empty after trimming"; return false; }

    clip.pitch = c.value("pitch", 0.0);
    clip.stretch = c.value("stretch", true);
    clip.gainDb = c.value("gain", 0.0);
    clip.fadeInMs = c.value("fadeIn", 2.0);
    clip.fadeOutMs = c.value("fadeOut", 5.0);
    const bool hasBeat = c.contains("beat"), hasEnd = c.contains("endAt");
    if (hasBeat == hasEnd) { err = "clip: give either \"beat\" (where it starts) or \"endAt\" (the beat where it ends)"; return false; }
    const double anchorBeat = hasBeat ? c["beat"].get<double>() : c["endAt"].get<double>();
    // "key": an Apple Loop moved into a key ("D minor") or into the job's own key at the clip ("song")
    if (c.contains("key")) {
        if (!c["key"].is_string()) { err = "clip: \"key\" is a key name (\"D minor\") or \"song\""; return false; }
        if (!loop) { err = "clip: \"key\" moves an Apple Loop into a key; '" + file + "' doesn't say its own key (use \"pitch\")"; return false; }
        const std::string k = c["key"].get<std::string>();
        int shift = 0;
        if (k == "song") {
            const KeyMark *in = nullptr;
            for (auto &m : job.keys) if (m.beat <= anchorBeat + 1e-9) in = &m;
            if (!in && !job.keys.empty()) in = &job.keys.front();
            if (!in) { err = "clip: \"key\": \"song\" needs the job's \"keys\" ([{\"bar\": 1, \"key\": \"D minor\"}])"; return false; }
            if (appleLoopShift(*loop, in->tonic, in->minor, shift)) clip.pitch += shift;
        } else if (!loop->key.empty()) {
            if (!appleLoopShift(*loop, k, shift, err)) { err = "clip: \"key\": " + err; return false; }
            clip.pitch += shift;
        }
    }
    // speed: the file's tempo fitted to the song's (at the anchor), or an explicit factor
    clip.speed = c.value("speed", 1.0);
    if (srcBpm > 0 && !c.contains("speed")) clip.speed = job.tempo.bpmAtBeat(anchorBeat) / srcBpm;
    if (clip.speed <= 0.01) { err = "clip: speed must be positive"; return false; }
    const double durSec = (double)clip.outFrames() / job.sampleRate;
    clip.startSec = hasBeat ? job.tempo.beatToSec(anchorBeat) : job.tempo.beatToSec(anchorBeat) - durSec;
    if (clip.startSec < -job.tempo.originSec() - 1e-9) { err = "clip: '" + file + "' would start before the song (endAt is too early for its length)"; return false; }
    return true;
}

} // namespace

bool clipRenders(const Track &track, std::vector<ClipRender> &out, std::string &err) {
    for (size_t i = 0; i < track.clips.size(); ++i) {
        const json &c = track.clips[i];
        if (!c.is_object() || !c.contains("file") || !c["file"].is_object()) continue;
        ClipRender r;
        double tail;
        if (!renderSpec(c["file"], r, tail, err)) { err = "track '" + track.name + "' clip " + std::to_string(i + 1) + ": " + err; return false; }
        r.clip = i;
        out.push_back(r);
    }
    return true;
}

double clipsEndSeconds(const Job &job, const Track &track) {
    double end = 0;
    std::string err;
    for (auto &c : track.clips) {
        Clip clip;
        if (parseClip(job, c, clip, err)) end = std::max(end, clip.startSec + (double)clip.outFrames() / job.sampleRate);
    }
    return end;
}

bool renderClips(const Job &job, const Track &track, Audio &out, std::vector<std::string> &warnings, std::string &err,
                 const std::map<size_t, Audio> *rendered) {
    if (track.clips.empty()) { warnings.push_back("builtin:audio track has no \"clips\""); return true; }
    const double sr = job.sampleRate;
    for (size_t ci = 0; ci < track.clips.size(); ++ci) {
        Clip clip;
        const Audio *captured = nullptr;
        if (track.clips[ci].is_object() && track.clips[ci].contains("file") && track.clips[ci]["file"].is_object()) {
            if (rendered && rendered->count(ci)) captured = &rendered->at(ci);
            if (!captured) {
                err = "track '" + track.name + "' clip " + std::to_string(ci + 1) + ": a \"render\" clip only plays inside a song render";
                return false;
            }
            if (measure(*captured).silent)
                warnings.push_back("clip " + std::to_string(ci + 1) + ": the rendered beats captured silence (are the tracks playing there, and not muted?)");
        }
        if (!parseClip(job, track.clips[ci], clip, err, captured)) { err = "track '" + track.name + "' clip " + std::to_string(ci + 1) + ": " + err; return false; }
        Audio body;
        const size_t outN = clip.outFrames();
        if (!clip.stretch) {   // tape-style: speed and pitch move together
            body = resample(clip.audio, clip.speed * std::pow(2.0, clip.pitch / 12));
        } else if (std::fabs(clip.speed - 1) < 1e-6 && std::fabs(clip.pitch) < 1e-6) {
            body = clip.audio;
        } else {
            signalsmith::stretch::SignalsmithStretch<float> stretch;
            stretch.presetDefault(2, (float)sr);
            stretch.setTransposeSemitones((float)clip.pitch);
            std::vector<std::vector<float>> in = {clip.audio.left, clip.audio.right}, o(2, std::vector<float>(outN, 0.f));
            if (!stretch.exact(in, (int)clip.audio.frames(), o, (int)outN)) {
                err = "track '" + track.name + "' clip " + std::to_string(ci + 1) + ": too short to stretch (under about 0.2 s)";
                return false;
            }
            body.left = std::move(o[0]);
            body.right = std::move(o[1]);
        }
        const double g = dsp::dbToLin(clip.gainDb);
        const size_t fi = (size_t)(clip.fadeInMs * 0.001 * sr), fo = (size_t)(clip.fadeOutMs * 0.001 * sr);
        // a clip can start before a render window (render --from): play it from where the window begins
        const int64_t at0 = std::llround(clip.startSec * sr);
        const size_t n = body.frames(), skip = at0 < 0 ? (size_t)(-at0) : 0, at = at0 < 0 ? 0 : (size_t)at0;
        for (size_t i = skip; i < n && at + i - skip < out.frames(); ++i) {
            double e = g;
            if (fi && i < fi) e *= (double)i / fi;
            if (fo && i + fo > n) e *= (double)(n - i) / fo;
            out.left[at + i - skip] += (float)(body.left[i] * e);
            out.right[at + i - skip] += (float)(body.right[i] * e);
        }
    }
    return true;
}

} // namespace wl
