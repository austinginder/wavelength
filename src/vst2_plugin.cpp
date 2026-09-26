#include "vst2_plugin.hpp"

#include "job.hpp"
#include "platform.hpp"
#include "state_file.hpp"
#include "vst2_abi.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace wl {

using namespace vst2;

struct Vst2Plugin::Impl {
    Effect *fx = nullptr;
    double sampleRate = 48000;
    int32_t blockSize = 512;
    TimeInfo time{};
    bool rendering = false;
    intptr_t dispatch(int32_t op, int32_t index = 0, intptr_t value = 0, void *ptr = nullptr, float opt = 0.f) {
        return fx->dispatcher(fx, op, index, value, ptr, opt);
    }
};

namespace {

std::mutex gMapMutex;
std::map<Effect *, Vst2Plugin::Impl *> gInstances;
thread_local int32_t gLoadingId = 0;   // hostCurrentId while a shell plugin asks which of its plugins to make

Vst2Plugin::Impl *implOf(Effect *e) {
    std::lock_guard<std::mutex> lock(gMapMutex);
    auto it = gInstances.find(e);
    return it == gInstances.end() ? nullptr : it->second;
}

intptr_t hostCallback(Effect *effect, int32_t opcode, int32_t index, intptr_t value, void *ptr, float opt) {
    (void)index; (void)value; (void)opt;
    Vst2Plugin::Impl *im = effect ? implOf(effect) : nullptr;
    switch (opcode) {
    case hostVersion: return 2400;
    case hostCurrentId: return gLoadingId;
    case hostIdle: return 0;
    case hostGetTime: return im ? reinterpret_cast<intptr_t>(&im->time) : 0;
    case hostGetSampleRate: return im ? (intptr_t)im->sampleRate : 48000;
    case hostGetBlockSize: return im ? im->blockSize : 512;
    case hostGetCurrentProcessLevel: return im && im->rendering ? kProcessLevelRealtime : 0;
    case hostGetAutomationState: return 1;   // off
    case hostGetVendorString: if (ptr) std::strcpy(static_cast<char *>(ptr), "Wavelength"); return 1;
    case hostGetProductString: if (ptr) std::strcpy(static_cast<char *>(ptr), "Wavelength"); return 1;
    case hostGetVendorVersion: return 200;
    case hostGetLanguage: return 1;   // English
    case hostIoChanged: return 1;
    case hostUpdateDisplay: return 1;
    case hostCanDo: {
        const std::string what = ptr ? static_cast<const char *>(ptr) : "";
        for (const char *yes : {"sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents", "receiveVstMidiEvent",
                                "supportShell", "shellCategory"})
            if (what == yes) return 1;
        return 0;
    }
    default: return 0;
    }
}

// The library file inside a bundle path, or the path itself (.dll / .so)
Effect *instantiate(const std::string &path, int32_t shellId, std::string &err) {
    void *sym = platform::loadLibrarySymbol(path, "VSTPluginMain", err);
    if (!sym) {
        std::string e2;
        sym = platform::loadLibrarySymbol(path, "main_macho", e2);
        if (!sym) sym = platform::loadLibrarySymbol(path, "main", e2);
        if (!sym) return nullptr;
        err.clear();
    }
    gLoadingId = shellId;
    Effect *fx = reinterpret_cast<EntryFn>(sym)(hostCallback);
    gLoadingId = 0;
    if (!fx || fx->magic != kMagic) { err = path + " did not return a VST 2 plugin"; return nullptr; }
    return fx;
}

std::string dispatchString(Effect *fx, int32_t op, int32_t index = 0) {
    char buf[512] = {};
    fx->dispatcher(fx, op, index, 0, buf, 0.f);
    buf[sizeof buf - 1] = 0;
    std::string s(buf);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

void be32put(std::vector<uint8_t> &d, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) d.push_back((uint8_t)(v >> s));
}

} // namespace

bool scanVst2Bundle(const std::string &path, std::vector<PluginInfo> &out, std::string &err) {
    Effect *fx = instantiate(path, 0, err);
    if (!fx) return false;
    fx->dispatcher(fx, effOpen, 0, 0, nullptr, 0.f);
    const intptr_t category = fx->dispatcher(fx, effGetPlugCategory, 0, 0, nullptr, 0.f);
    if (category == kCategoryShell) {
        fx->dispatcher(fx, effClose, 0, 0, nullptr, 0.f);
        err = path + " is a VST 2 shell (several plugins behind one library); shells are not supported yet";
        return false;
    }
    PluginInfo p;
    p.format = "vst2";
    p.bundlePath = path;
    p.id = fourcc(fx->uniqueId);
    p.name = dispatchString(fx, effGetEffectName);
    if (p.name.empty()) p.name = dispatchString(fx, effGetProductString);
    if (p.name.empty()) p.name = fs::path(path).stem().string();
    p.vendor = dispatchString(fx, effGetVendorString);
    const intptr_t v = fx->dispatcher(fx, effGetVendorVersion, 0, 0, nullptr, 0.f);
    p.version = v > 0 ? std::to_string(v) : "";
    p.features = {(fx->flags & kFlagIsSynth) ? "instrument" : "audio-effect"};
    p.description = std::to_string(fx->numInputs) + " in, " + std::to_string(fx->numOutputs) + " out, " + std::to_string(fx->numParams) +
                    " parameters, " + std::to_string(fx->numPrograms) + " programs";
    fx->dispatcher(fx, effClose, 0, 0, nullptr, 0.f);
    out.push_back(p);
    return true;
}

Vst2Plugin::Vst2Plugin() : impl_(std::make_unique<Impl>()) {}

Vst2Plugin::~Vst2Plugin() {
    if (impl_ && impl_->fx) {
        impl_->dispatch(effClose);
        std::lock_guard<std::mutex> lock(gMapMutex);
        gInstances.erase(impl_->fx);
    }
}

std::unique_ptr<Plugin> Vst2Plugin::create(const PluginInfo &info, std::string &err) {
    std::unique_ptr<Vst2Plugin> p(new Vst2Plugin());
    Effect *fx = instantiate(info.bundlePath, 0, err);
    if (!fx) return nullptr;
    p->impl_->fx = fx;
    {
        std::lock_guard<std::mutex> lock(gMapMutex);
        gInstances[fx] = p->impl_.get();
    }
    p->id_ = info.id;
    p->name_ = info.name;
    p->impl_->dispatch(effOpen);
    p->impl_->dispatch(effSetSampleRate, 0, 0, nullptr, (float)p->impl_->sampleRate);
    p->impl_->dispatch(effSetBlockSize, 0, p->impl_->blockSize);
    return p;
}

void Vst2Plugin::pump(double ms) { platform::pumpEvents(ms); }

bool Vst2Plugin::loadState(const StateFile &sf, std::string &err) {
    auto &im = *impl_;
    if (!sf.fxParams.empty() && sf.state.empty()) {   // FxCk / FxBk: parameter values
        const int32_t n = std::min<int32_t>(im.fx->numParams, (int32_t)sf.fxParams.size());
        for (int32_t i = 0; i < n; ++i) im.fx->setParameter(im.fx, i, sf.fxParams[(size_t)i]);
        return true;
    }
    if (sf.state.empty()) { err = name_ + ": empty state"; return false; }
    if (!(im.fx->flags & kFlagProgramChunks)) {
        err = name_ + " does not take state chunks (it saves parameter lists: use an FxCk .fxp or \"params\")";
        return false;
    }
    const bool program = sf.fxKind == "FPCh";   // FBCh banks and raw chunks load as a bank
    std::vector<uint8_t> chunk = sf.state;
    if (program) im.dispatch(effBeginSetProgram);
    im.dispatch(effSetChunk, program ? 1 : 0, (intptr_t)chunk.size(), chunk.data());
    if (program) im.dispatch(effEndSetProgram);
    pump(50);
    return true;
}

bool Vst2Plugin::getState(std::vector<uint8_t> &out, std::string &err) {
    auto &im = *impl_;
    if (im.fx->flags & kFlagProgramChunks) {
        void *data = nullptr;
        const intptr_t n = im.dispatch(effGetChunk, 0, 0, &data);
        if (n <= 0 || !data) { err = name_ + " returned an empty chunk"; return false; }
        out.assign(static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + n);
        return true;
    }
    out.clear();   // parameter plugins: the values as big-endian floats
    for (int32_t i = 0; i < im.fx->numParams; ++i) {
        const float f = im.fx->getParameter(im.fx, i);
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        be32put(out, bits);
    }
    return true;
}

bool Vst2Plugin::saveStateFile(const std::string &path, size_t &bytes, std::string &err) {
    auto &im = *impl_;
    std::vector<uint8_t> body, file;
    const bool chunks = im.fx->flags & kFlagProgramChunks;
    if (!getState(body, err)) return false;
    // .fxb: "CcnK", size, "FBCh" (bank chunk) or "FxCk" (one program's parameters), version, id, version, count
    const std::string kind = chunks ? "FBCh" : "FxCk";
    std::vector<uint8_t> head;
    for (char c : kind) head.push_back((uint8_t)c);
    be32put(head, 1);
    be32put(head, (uint32_t)im.fx->uniqueId);
    be32put(head, (uint32_t)im.fx->version);
    be32put(head, (uint32_t)(chunks ? im.fx->numPrograms : im.fx->numParams));
    if (chunks) {
        head.insert(head.end(), 128, 0);
        be32put(head, (uint32_t)body.size());
    } else {
        std::string pname = "Wavelength";
        pname.resize(28, '\0');
        head.insert(head.end(), pname.begin(), pname.end());
    }
    head.insert(head.end(), body.begin(), body.end());
    for (char c : std::string("CcnK")) file.push_back((uint8_t)c);
    be32put(file, (uint32_t)head.size());
    file.insert(file.end(), head.begin(), head.end());
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char *>(file.data()), (std::streamsize)file.size());
    if (!o) { err = "cannot write " + path; return false; }
    bytes = file.size();
    return true;
}

std::vector<std::string> Vst2Plugin::programs() {
    std::vector<std::string> out;
    auto &im = *impl_;
    for (int32_t i = 0; i < im.fx->numPrograms; ++i) {
        std::string n = dispatchString(im.fx, effGetProgramNameIndexed, i);
        if (n.empty()) break;
        out.push_back(n);
    }
    return out;
}

bool Vst2Plugin::loadPreset(const std::string &query, std::string &loadedName, std::string &err) {
    const auto names = programs();
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    int hit = -1;
    for (size_t i = 0; i < names.size() && hit < 0; ++i) if (lower(names[i]) == lower(query)) hit = (int)i;
    for (size_t i = 0; i < names.size() && hit < 0; ++i) if (lower(names[i]).find(lower(query)) != std::string::npos) hit = (int)i;
    if (hit < 0) { err = name_ + " has no program matching '" + query + "' (" + std::to_string(names.size()) + " programs)"; return false; }
    impl_->dispatch(effSetProgram, 0, hit);
    loadedName = names[(size_t)hit];
    return true;
}

std::vector<ParamInfo> Vst2Plugin::params() const {
    std::vector<ParamInfo> out;
    Effect *fx = impl_->fx;
    for (int32_t i = 0; i < fx->numParams; ++i) {
        ParamInfo p{};
        p.id = (ParamId)i;
        p.cookie = nullptr;
        p.name = dispatchString(fx, effGetParamName, i);
        if (p.name.empty()) p.name = "Param " + std::to_string(i);
        p.min = 0; p.max = 1;
        p.value = fx->getParameter(fx, i);
        p.def = p.value;
        p.stepped = p.readonly = p.hidden = false;
        const std::string disp = dispatchString(fx, effGetParamDisplay, i), label = dispatchString(fx, effGetParamLabel, i);
        p.display = label.empty() ? disp : disp + " " + label;
        out.push_back(p);
    }
    return out;
}

bool Vst2Plugin::valueFromText(ParamId id, const std::string &text, double &plain) {
    Effect *fx = impl_->fx;
    const float before = fx->getParameter(fx, (int32_t)id);
    std::string t = text;
    if (!fx->dispatcher(fx, effString2Parameter, (int32_t)id, 0, t.data(), 0.f)) return false;
    plain = fx->getParameter(fx, (int32_t)id);
    fx->setParameter(fx, (int32_t)id, before);
    return true;
}

bool Vst2Plugin::textForValue(ParamId id, double plain, std::string &text) {
    Effect *fx = impl_->fx;
    if ((int32_t)id >= fx->numParams) return false;
    const float before = fx->getParameter(fx, (int32_t)id);
    fx->setParameter(fx, (int32_t)id, (float)std::clamp(plain, 0.0, 1.0));
    const std::string disp = dispatchString(fx, effGetParamDisplay, (int32_t)id), label = dispatchString(fx, effGetParamLabel, (int32_t)id);
    fx->setParameter(fx, (int32_t)id, before);
    text = label.empty() ? disp : disp + " " + label;
    return true;
}

bool Vst2Plugin::setParams(const std::vector<ParamValue> &values, std::string &err) {
    (void)err;
    Effect *fx = impl_->fx;
    for (const auto &v : values)
        if ((int32_t)v.id < fx->numParams) fx->setParameter(fx, (int32_t)v.id, (float)std::clamp(v.value, 0.0, 1.0));
    return true;
}

bool Vst2Plugin::commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) {
    (void)sampleRate; (void)block;
    return setParams(values, err);   // VST 2 parameters apply immediately
}

bool Vst2Plugin::render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                        const std::vector<AutoParam> &autos, const Audio *input, Audio &out,
                        std::vector<std::string> &warnings, std::string &err) {
    auto &im = *impl_;
    Effect *fx = im.fx;
    const int32_t block = job.blockSize;
    const double sr = job.sampleRate;
    if (fx->numOutputs < 1) { err = name_ + " has no audio output"; return false; }
    if (input && fx->numInputs < 1) { err = name_ + " has no audio input, so it can't be used as an effect"; return false; }
    if (!(fx->flags & kFlagCanReplacing) && !fx->processAccumulating) { err = name_ + " has no process function"; return false; }
    im.sampleRate = sr;
    im.blockSize = block;
    im.dispatch(effSetSampleRate, 0, 0, nullptr, (float)sr);
    im.dispatch(effSetBlockSize, 0, block);
    im.dispatch(effSetProcessPrecision, 0, 0);   // 32-bit float
    im.dispatch(effMainsChanged, 0, 1);
    im.dispatch(effStartProcess);
    im.rendering = true;
    pump((warmup >= 0 ? warmup : job.warmup) * 1000.0);
    setParams(initial, err);

    const int32_t nin = std::max(fx->numInputs, 0), nout = std::max(fx->numOutputs, 0);
    std::vector<std::vector<float>> inBuf((size_t)nin, std::vector<float>((size_t)block)), outBuf((size_t)nout, std::vector<float>((size_t)block));
    std::vector<float *> inPtr((size_t)nin), outPtr((size_t)nout);
    for (int32_t c = 0; c < nin; ++c) inPtr[(size_t)c] = inBuf[(size_t)c].data();
    for (int32_t c = 0; c < nout; ++c) outPtr[(size_t)c] = outBuf[(size_t)c].data();
    sidechainConnected = sidechain && nin >= 4;

    // events for one block: Events header + pointer array, backed by MidiEvent storage
    constexpr size_t kMaxEvents = 1024;
    std::vector<MidiEvent> midi(kMaxEvents);
    std::vector<uint8_t> evMem(sizeof(Events) + kMaxEvents * sizeof(void *));
    Events *evs = reinterpret_cast<Events *>(evMem.data());
    void **evPtr = evs->events;
    size_t dropped = 0;

    const int64_t total = (int64_t)out.frames();
    const int64_t lat = std::clamp<int64_t>(fx->initialDelay, 0, (int64_t)(10 * sr));
    latencySamples = (uint32_t)lat;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        const int64_t warm = (int64_t)(0.1 * sr), end = total + lat;
        size_t next = 0;
        std::vector<float> lastAuto(autos.size(), NAN);
        for (int64_t pos = -warm; pos < end;) {
            const int32_t n = (int32_t)std::min<int64_t>(block, pos < 0 ? -pos : end - pos);
            // parameters at audio start (some plugins apply a state in their first blocks) and automation
            if (pos >= 0 && pos < block) for (const auto &v : initial) fx->setParameter(fx, (int32_t)v.id, (float)std::clamp(v.value, 0.0, 1.0));
            if (pos >= 0)
                for (size_t a = 0; a < autos.size(); ++a) {
                    const float v = (float)std::clamp(autos[a].env.at(pos / sr), 0.0, 1.0);
                    if (v != lastAuto[a]) { fx->setParameter(fx, (int32_t)autos[a].id, v); lastAuto[a] = v; }
                }
            // MIDI for this block: notes, CC, pitch bend, channel pressure
            size_t count = 0;
            if (pos >= 0)
                while (next < events.size() && events[next].frame < pos + n) {
                    const auto &e = events[next++];
                    if (count >= kMaxEvents) { ++dropped; continue; }
                    MidiEvent &m = midi[count];
                    m = MidiEvent{};
                    m.type = 1;
                    m.byteSize = sizeof(MidiEvent);
                    m.deltaFrames = (int32_t)std::max<int64_t>(0, e.frame - pos);
                    m.flags = 1;
                    const int ch = std::clamp(e.channel, 0, 15);
                    if (e.kind == TimedEvent::Note) {
                        m.midiData[0] = (char)((e.on ? 0x90 : 0x80) | ch);
                        m.midiData[1] = (char)std::clamp(e.key, 0, 127);
                        m.midiData[2] = (char)std::clamp((int)std::lround(e.velocity * 127), e.on ? 1 : 0, 127);
                    } else if (e.kind == TimedEvent::CC) {
                        m.midiData[0] = (char)(0xB0 | ch);
                        m.midiData[1] = (char)std::clamp(e.number, 0, 127);
                        m.midiData[2] = (char)std::clamp((int)std::lround(e.value * 127), 0, 127);
                    } else if (e.kind == TimedEvent::PitchBend) {
                        const int v = std::clamp((int)std::lround((e.value + 1) * 8192), 0, 16383);
                        m.midiData[0] = (char)(0xE0 | ch);
                        m.midiData[1] = (char)(v & 0x7f);
                        m.midiData[2] = (char)(v >> 7);
                    } else {
                        m.midiData[0] = (char)(0xD0 | ch);
                        m.midiData[1] = (char)std::clamp((int)std::lround(e.value * 127), 0, 127);
                    }
                    evPtr[count++] = &m;
                }
            evs->numEvents = (int32_t)count;
            evs->reserved = 0;
            // transport, read by the plugin through hostGetTime
            const double sec = std::max<int64_t>(0, pos) / sr, beat = job.tempo.secToBeat(sec);
            const double barBeats = job.tsigNum * 4.0 / job.tsigDen;
            TimeInfo &t = im.time;
            t = TimeInfo{};
            t.samplePos = (double)std::max<int64_t>(0, pos);
            t.sampleRate = sr;
            t.ppqPos = beat;
            t.tempo = job.tempo.bpmAtBeat(beat);
            t.barStartPos = std::floor(beat / barBeats) * barBeats;
            t.timeSigNumerator = job.tsigNum;
            t.timeSigDenominator = job.tsigDen;
            t.flags = kPpqPosValid | kTempoValid | kBarsValid | kTimeSigValid | (pos >= 0 ? kTransportPlaying : 0);
            if (count) fx->dispatcher(fx, effProcessEvents, 0, 0, evs, 0.f);
            // inputs: silence, the effect's source on 0/1 and its sidechain on 2/3
            for (int32_t c = 0; c < nin; ++c) {
                float *dst = inPtr[(size_t)c];
                std::fill(dst, dst + n, 0.f);
                const Audio *feed = c < 2 ? input : (c < 4 && sidechainConnected ? sidechain : nullptr);
                if (feed && pos >= 0 && pos < total) {
                    const auto &src = (c % 2) ? feed->right : feed->left;
                    std::copy(src.begin() + pos, src.begin() + std::min<int64_t>(pos + n, total), dst);
                }
            }
            for (int32_t c = 0; c < nout; ++c) std::fill(outPtr[(size_t)c], outPtr[(size_t)c] + n, 0.f);
            if (fx->flags & kFlagCanReplacing) fx->processReplacing(fx, inPtr.data(), outPtr.data(), n);
            else fx->processAccumulating(fx, inPtr.data(), outPtr.data(), n);
            if (pos >= 0) {
                const float *l = outPtr[0], *r = nout > 1 ? outPtr[1] : outPtr[0];
                for (int32_t i = 0; i < n; ++i) {
                    const int64_t at = pos + i - lat;
                    if (at < 0 || at >= total) continue;
                    out.left[(size_t)at] = l[i];
                    out.right[(size_t)at] = r[i];
                }
            }
            pos += n;
        }
        done = true;
    });
    while (!done) {
        pump(2);
        fx->dispatcher(fx, 53 /* effIdle */, 0, 0, nullptr, 0.f);
    }
    worker.join();
    im.dispatch(effStopProcess);
    im.dispatch(effMainsChanged, 0, 0);
    im.rendering = false;
    if (dropped) warnings.push_back(name_ + ": " + std::to_string(dropped) + " MIDI events over 1024 in one block were dropped");
    (void)err;
    return true;
}

} // namespace wl
