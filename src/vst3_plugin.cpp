#include "vst3_plugin.hpp"

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstpresetfile.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace wl {

namespace {

// One host context for the process, as the SDK expects.
FUnknown *hostContext() {
    static Vst::HostApplication *app = [] {
        auto *a = new Vst::HostApplication();
        PluginContextFactory::instance().setPluginContext(a);
        return a;
    }();
    return app;
}

// Modules stay loaded for the life of the process (unloading plugin libraries crashes hosts).
VST3::Hosting::Module::Ptr loadModule(const std::string &path, std::string &err) {
    static std::map<std::string, VST3::Hosting::Module::Ptr> modules;
    if (auto it = modules.find(path); it != modules.end()) return it->second;
    std::string why;
    auto m = VST3::Hosting::Module::create(path, why);
    if (!m) { err = "could not load VST3 module " + path + (why.empty() ? "" : ": " + why); return nullptr; }
    modules[path] = m;
    return m;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string uidString(const VST3::UID &uid) { return uid.toString(); }

void pumpFor(double ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((int64_t)(ms * 1000));
    do {
#ifdef __APPLE__
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
    } while (std::chrono::steady_clock::now() < until);
}

} // namespace

// ---- scanning ----------------------------------------------------------------------------
bool scanVst3Bundle(const std::string &path, std::vector<PluginInfo> &out, std::string &err) {
    hostContext();
    auto module = loadModule(path, err);
    if (!module) return false;
    const auto factory = module->getFactory();
    const std::string factoryVendor = factory.info().vendor();
    for (const auto &ci : factory.classInfos()) {
        if (ci.category() != kVstAudioEffectClass) continue;
        PluginInfo p;
        p.id = uidString(ci.ID());
        p.name = ci.name();
        p.vendor = ci.vendor().empty() ? factoryVendor : ci.vendor();
        p.version = ci.version();
        p.description = ci.subCategoriesString();
        p.bundlePath = path;
        p.format = "vst3";
        bool instrument = false;
        for (const auto &sub : ci.subCategories()) {
            const std::string s = lower(sub);
            if (std::find(p.features.begin(), p.features.end(), s) == p.features.end()) p.features.push_back(s);
            instrument |= s == "instrument";
        }
        if (!instrument) p.features.push_back("audio-effect");
        out.push_back(std::move(p));
    }
    return true;
}

// ---- instance ----------------------------------------------------------------------------
struct Vst3Plugin::Impl {
    VST3::Hosting::Module::Ptr module;
    VST3::Hosting::ClassInfo classInfo;
    IPtr<PlugProvider> provider;
    IPtr<IComponent> component;
    IPtr<IEditController> controller;
    FUnknownPtr<IAudioProcessor> processor{nullptr};
    std::vector<std::pair<ParamID, Vst::ParamValue>> pending;   // normalized values for the first block

    Vst::ParamValue toNorm(ParamID id, double plain) const {
        return controller ? controller->plainParamToNormalized(id, plain) : plain;
    }
};

Vst3Plugin::Vst3Plugin() : impl_(new Impl()) {}
Vst3Plugin::~Vst3Plugin() {
    if (!impl_) return;
    impl_->processor = nullptr;
    impl_->controller = nullptr;
    impl_->component = nullptr;
    impl_->provider = nullptr;   // terminates and releases component + controller
}

std::unique_ptr<Plugin> Vst3Plugin::create(const PluginInfo &info, std::string &err) {
    hostContext();
    auto module = loadModule(info.bundlePath, err);
    if (!module) return nullptr;
    const auto factory = module->getFactory();
    std::unique_ptr<Vst3Plugin> p(new Vst3Plugin());
    bool found = false;
    for (const auto &ci : factory.classInfos())
        if (ci.category() == kVstAudioEffectClass && lower(uidString(ci.ID())) == lower(info.id)) { p->impl_->classInfo = ci; found = true; }
    if (!found) { err = "no VST3 class " + info.id + " in " + info.bundlePath; return nullptr; }

    auto &im = *p->impl_;
    im.module = module;
    im.provider = owned(new PlugProvider(factory, im.classInfo, true));
    if (!im.provider->initialize()) { err = info.name + ": the plugin failed to initialise"; return nullptr; }
    im.component = im.provider->getComponentPtr();
    im.controller = im.provider->getControllerPtr();
    if (!im.component) { err = info.name + ": no audio component"; return nullptr; }
    im.processor = FUnknownPtr<IAudioProcessor>(im.component);
    if (!im.processor) { err = info.name + ": the component is not an audio processor"; return nullptr; }
    if (im.processor->canProcessSampleSize(kSample32) != kResultTrue) { err = info.name + ": does not support 32-bit processing"; return nullptr; }
    p->id_ = info.id;
    p->name_ = info.name;
    return p;
}

void Vst3Plugin::pump(double ms) { pumpFor(ms); }

// ---- state -------------------------------------------------------------------------------
bool Vst3Plugin::loadState(const StateFile &sf, std::string &err) {
    auto &im = *impl_;
    if (sf.format == "clap-preset") { err = "a .clap-preset is CLAP state; " + name_ + " is a VST3 plugin"; return false; }
    std::vector<uint8_t> data = sf.state;   // MemoryStream reads from a mutable buffer
    if (sf.format == "vstpreset") {
        if (!sf.pluginId.empty() && lower(sf.pluginId) != lower(id_)) {
            err = "the .vstpreset belongs to class " + sf.pluginId + ", not " + name_ + " (" + id_ + ")";
            return false;
        }
        MemoryStream stream(data.data(), (TSize)data.size());
        if (!PresetFile::loadPreset(&stream, FUID::fromTUID(im.classInfo.ID().data()), im.component, im.controller)) {
            err = name_ + " rejected the .vstpreset";
            return false;
        }
        return true;
    }
    // nksf / juce-string / raw: the component's own state bytes
    auto tryLoad = [&](std::vector<uint8_t> &bytes) {
        MemoryStream stream(bytes.data(), (TSize)bytes.size());
        return im.component->setState(&stream) == kResultOk;
    };
    bool ok = tryLoad(data);
    if (!ok && sf.format == "nksf") {   // NKS chunks of wrapped VST2 plugins (DUNE 3) lack the u32 length prefix
        std::vector<uint8_t> pre(4);
        for (int i = 0; i < 4; ++i) pre[(size_t)i] = (uint8_t)(sf.state.size() >> (8 * i));
        pre.insert(pre.end(), sf.state.begin(), sf.state.end());
        data = pre;
        ok = tryLoad(data);
    }
    if (!ok) { err = name_ + " rejected the state (" + std::to_string(sf.state.size()) + " bytes)"; return false; }
    if (im.controller) {
        MemoryStream stream(data.data(), (TSize)data.size());
        im.controller->setComponentState(&stream);
        if (!sf.controllerState.empty()) {   // the controller's own half (Serum 2 presets carry both)
            std::vector<uint8_t> ctl = sf.controllerState;
            MemoryStream cs(ctl.data(), (TSize)ctl.size());
            im.controller->setState(&cs);
        }
    }
    return true;
}

bool Vst3Plugin::saveStateFile(const std::string &path, size_t &bytes, std::string &err) {
    auto &im = *impl_;
    MemoryStream stream;
    if (!PresetFile::savePreset(&stream, FUID::fromTUID(im.classInfo.ID().data()), im.component, im.controller)) {
        err = name_ + " failed to save its state";
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) { err = "cannot write " + path; return false; }
    out.write(stream.getData(), (std::streamsize)stream.getSize());
    bytes = (size_t)stream.getSize();
    return (bool)out;
}

bool Vst3Plugin::getState(std::vector<uint8_t> &out, std::string &err) {
    MemoryStream stream;
    if (impl_->component->getState(&stream) != kResultOk) { err = name_ + " failed to save its state"; return false; }
    out.assign(stream.getData(), stream.getData() + stream.getSize());
    return true;
}

// ---- parameters --------------------------------------------------------------------------
std::vector<ParamInfo> Vst3Plugin::params() const {
    std::vector<ParamInfo> out;
    auto &ctl = impl_->controller;
    if (!ctl) return out;
    const int32 n = ctl->getParameterCount();
    for (int32 i = 0; i < n; ++i) {
        ParameterInfo pi{};
        if (ctl->getParameterInfo(i, pi) != kResultOk) continue;
        ParamInfo p;
        p.id = pi.id;
        p.cookie = nullptr;
        p.name = Steinberg::Vst::StringConvert::convert(pi.title);
        p.module = "";
        p.min = ctl->normalizedParamToPlain(pi.id, 0.0);
        p.max = ctl->normalizedParamToPlain(pi.id, 1.0);
        p.def = ctl->normalizedParamToPlain(pi.id, pi.defaultNormalizedValue);
        const Vst::ParamValue norm = ctl->getParamNormalized(pi.id);
        p.value = ctl->normalizedParamToPlain(pi.id, norm);
        p.stepped = pi.stepCount > 0;
        p.readonly = pi.flags & ParameterInfo::kIsReadOnly;
        p.hidden = pi.flags & ParameterInfo::kIsHidden;
        String128 text{};
        if (ctl->getParamStringByValue(pi.id, norm, text) == kResultOk) {
            p.display = Steinberg::Vst::StringConvert::convert(text);
            const std::string units = Steinberg::Vst::StringConvert::convert(pi.units);
            if (!units.empty()) p.display += " " + units;
        }
        out.push_back(std::move(p));
    }
    return out;
}

bool Vst3Plugin::setParams(const std::vector<ParamValue> &values, std::string &err) {
    auto &im = *impl_;
    if (!values.empty() && !im.controller) { err = name_ + " has no edit controller, so its parameters can't be set"; return false; }
    for (const auto &v : values) {
        const Vst::ParamValue norm = im.toNorm(v.id, v.value);
        im.controller->setParamNormalized(v.id, norm);
        im.pending.push_back({v.id, norm});
    }
    return true;
}

// ---- factory programs (VST3 program lists, selected through the program-change parameter) ----
namespace {
struct ProgramList { ParamID param = 0; int32 count = 0; Vst::ProgramListID list = -1; };
ProgramList findProgramList(IEditController *ctl) {
    ProgramList out;
    if (!ctl) return out;
    FUnknownPtr<Vst::IUnitInfo> units(ctl);
    for (int32 i = 0, n = ctl->getParameterCount(); i < n; ++i) {
        Vst::ParameterInfo pi{};
        if (ctl->getParameterInfo(i, pi) != kResultOk || !(pi.flags & Vst::ParameterInfo::kIsProgramChange)) continue;
        out.param = pi.id;
        out.count = pi.stepCount + 1;
        if (units) {   // the program list of the parameter's unit names the programs
            for (int32 u = 0, un = units->getUnitCount(); u < un; ++u) {
                Vst::UnitInfo ui{};
                if (units->getUnitInfo(u, ui) == kResultOk && ui.id == pi.unitId) out.list = ui.programListId;
            }
            if (out.list == Vst::kNoProgramListId && units->getProgramListCount() > 0) {
                Vst::ProgramListInfo li{};
                if (units->getProgramListInfo(0, li) == kResultOk) out.list = li.id;
            }
        }
        if (pi.unitId == Vst::kRootUnitId) break;   // prefer the root unit's program change
    }
    return out;
}
} // namespace

std::vector<std::string> Vst3Plugin::programs() {
    std::vector<std::string> names;
    auto &im = *impl_;
    const ProgramList pl = findProgramList(im.controller);
    if (!pl.param || pl.count < 1) return names;
    FUnknownPtr<Vst::IUnitInfo> units(im.controller);
    for (int32 i = 0; i < pl.count; ++i) {
        Vst::String128 name{};
        std::string s;
        if (units && pl.list != Vst::kNoProgramListId && units->getProgramName(pl.list, i, name) == kResultOk)
            s = Steinberg::Vst::StringConvert::convert(name);
        names.push_back(s.empty() ? "Program " + std::to_string(i + 1) : s);
    }
    // a list of placeholders ("Prog 1", "Program 0", "Default") is not a preset library
    auto generic = [](std::string n) {
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        for (const char *w : {"program", "prog", "preset", "patch", "default", "init"})
            if (n.rfind(w, 0) == 0) { n.erase(0, strlen(w)); break; }
        return n.find_first_not_of(" 0123456789") == std::string::npos;
    };
    if (std::all_of(names.begin(), names.end(), generic)) names.clear();
    if (names.size() > 1 && std::all_of(names.begin(), names.end(), [&](const std::string &n) { return n == names[0]; })) names.clear();
    return names;
}

bool Vst3Plugin::loadPreset(const std::string &query, std::string &loadedName, std::string &err) {
    auto &im = *impl_;
    const ProgramList pl = findProgramList(im.controller);
    const auto names = programs();
    if (!pl.param || names.empty()) { err = name_ + " lists no factory programs; load a state file instead"; return false; }
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    int found = -1;
    std::vector<int> partial;
    for (size_t i = 0; i < names.size() && found < 0; ++i) if (names[i] == query) found = (int)i;
    for (size_t i = 0; i < names.size() && found < 0; ++i) if (lower(names[i]) == lower(query)) found = (int)i;
    if (found < 0) for (size_t i = 0; i < names.size(); ++i) if (lower(names[i]).find(lower(query)) != std::string::npos) partial.push_back((int)i);
    if (found < 0 && partial.size() == 1) found = partial[0];
    if (found < 0) {
        err = partial.empty() ? "no program named '" + query + "' on " + name_ : "'" + query + "' matches " + std::to_string(partial.size()) + " programs";
        for (size_t i = 0; i < partial.size() && i < 6; ++i) err += (i ? ", " : ": ") + names[(size_t)partial[i]];
        err += " (run `wavelength presets \"" + name_ + "\"`)";
        return false;
    }
    const Vst::ParamValue norm = pl.count > 1 ? (double)found / (pl.count - 1) : 0;
    im.controller->setParamNormalized(pl.param, norm);
    // plugins switch programs inside process(): commit it now so the state and parameters follow
    if (!commitParams({{pl.param, nullptr, im.controller->normalizedParamToPlain(pl.param, norm)}}, 48000, 512, err)) return false;
    loadedName = names[(size_t)found];
    return true;
}

bool Vst3Plugin::commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) {
    if (values.empty()) return true;
    Job job;
    job.sampleRate = (int)sampleRate;
    job.blockSize = (int)block;
    job.warmup = 0.1;
    Audio silence;
    silence.resize(block * 8);
    std::vector<std::string> warnings;
    return render(job, {}, values, {}, nullptr, silence, warnings, err);
}

// ---- rendering ---------------------------------------------------------------------------
bool Vst3Plugin::render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                        const std::vector<AutoParam> &autos, const Audio *input, Audio &out,
                        std::vector<std::string> &warnings, std::string &err) {
    auto &im = *impl_;
    const int32 block = job.blockSize;
    const double sr = job.sampleRate;

    ProcessSetup setup{kOffline, kSample32, block, sr};
    if (im.processor->setupProcessing(setup) != kResultOk) { err = name_ + " refused the processing setup"; return false; }
    const int32 outBuses = im.component->getBusCount(kAudio, kOutput), inBuses = im.component->getBusCount(kAudio, kInput);
    if (outBuses < 1) { err = name_ + " has no audio output"; return false; }
    if (input && inBuses < 1) { err = name_ + " has no audio input, so it can't be used as an effect"; return false; }
    im.component->activateBus(kAudio, kOutput, 0, true);
    if (inBuses > 0) im.component->activateBus(kAudio, kInput, 0, true);
    if (im.component->getBusCount(kEvent, kInput) > 0) im.component->activateBus(kEvent, kInput, 0, true);
    if (im.component->setActive(true) != kResultOk) { err = name_ + " refused to activate"; return false; }
    pumpFor((warmup >= 0 ? warmup : job.warmup) * 1000.0);

    HostProcessData data;
    if (!data.prepare(*im.component, block, kSample32)) { im.component->setActive(false); err = name_ + ": could not allocate process buffers"; return false; }
    EventList eventList(1024);   // fixed capacity: extra events would be dropped silently
    ParameterChanges changes, outChanges;
    EventList outEvents;
    ProcessContext ctx{};
    data.processMode = kOffline;
    data.inputEvents = &eventList;
    data.outputEvents = &outEvents;
    data.inputParameterChanges = &changes;
    data.outputParameterChanges = &outChanges;
    data.processContext = &ctx;

    // parameter values: the setParams() backlog plus `initial` go in the first block;
    // automation is converted to normalized values here, on the main thread
    std::vector<std::pair<ParamID, Vst::ParamValue>> first = im.pending;
    for (const auto &v : initial) first.push_back({v.id, im.toNorm(v.id, v.value)});
    im.pending.clear();
    const int64_t total = (int64_t)out.frames(), blocks = (total + block - 1) / block;
    std::vector<std::vector<float>> autoNorm(autos.size());
    for (size_t a = 0; a < autos.size(); ++a) {
        autoNorm[a].resize((size_t)blocks);
        for (int64_t b = 0; b < blocks; ++b) autoNorm[a][(size_t)b] = (float)im.toNorm(autos[a].id, autos[a].env.at(b * (double)block / sr));
    }

    // MIDI CC / pitch bend / pressure reach a VST3 plugin as the parameters it maps them to
    std::map<std::pair<int, int>, ParamID> ctlMap;   // (channel, controller) -> parameter
    std::set<int> unmapped;
    {
        FUnknownPtr<Vst::IMidiMapping> mm(im.controller);
        for (const auto &e : events) {
            if (e.kind == TimedEvent::Note) continue;
            const int ctl = e.kind == TimedEvent::CC ? e.number : e.kind == TimedEvent::PitchBend ? Vst::kPitchBend : Vst::kAfterTouch;
            const auto key = std::make_pair(e.channel, ctl);
            if (ctlMap.count(key) || unmapped.count(ctl)) continue;
            ParamID id = 0;
            if (mm && mm->getMidiControllerAssignment(0, (int16)e.channel, (Vst::CtrlNumber)ctl, id) == kResultTrue) ctlMap[key] = id;
            else unmapped.insert(ctl);
        }
        for (int ctl : unmapped)
            warnings.push_back(name_ + " does not map " + (ctl == Vst::kPitchBend ? std::string("pitch bend") : ctl == Vst::kAfterTouch ? std::string("channel pressure") : "MIDI CC " + std::to_string(ctl)) +
                               " to a parameter; that automation is ignored");
    }

    std::atomic<bool> done{false};
    std::string audioErr;
    int failures = 0;
    std::thread worker([&] {
        im.processor->setProcessing(true);
        const int64_t warm = (int64_t)(0.1 * sr);
        size_t next = 0;
        std::vector<float> lastAuto(autos.size(), NAN);
        bool firstBlock = true;
        for (int64_t pos = -warm; pos < total;) {
            const int32 n = (int32)std::min<int64_t>(block, pos < 0 ? -pos : total - pos);
            data.numSamples = n;
            eventList.clear();
            outEvents.clear();
            changes.clearQueue();
            outChanges.clearQueue();
            auto addParam = [&](ParamID id, Vst::ParamValue v, int32 offset = 0) {
                int32 qi = 0, pi = 0;
                if (auto *q = changes.addParameterData(id, qi)) q->addPoint(offset, v, pi);
            };
            if (firstBlock) { for (auto &[id, v] : first) addParam(id, v); firstBlock = false; }
            // again when audio starts: some plugins (Surge XT) apply a loaded state in their first
            // blocks and would otherwise overwrite these values
            if (pos >= 0 && pos < block) for (const auto &v : initial) addParam(v.id, im.toNorm(v.id, v.value));
            if (pos >= 0) {
                const size_t b = (size_t)(pos / block);
                for (size_t a = 0; a < autos.size(); ++a)
                    if (b < autoNorm[a].size() && autoNorm[a][b] != lastAuto[a]) { addParam(autos[a].id, autoNorm[a][b]); lastAuto[a] = autoNorm[a][b]; }
                while (next < events.size() && events[next].frame < pos + n) {
                    const auto &e = events[next++];
                    if (e.kind != TimedEvent::Note) {
                        const int ctl = e.kind == TimedEvent::CC ? e.number : e.kind == TimedEvent::PitchBend ? Vst::kPitchBend : Vst::kAfterTouch;
                        auto it = ctlMap.find({e.channel, ctl});
                        if (it != ctlMap.end())
                            addParam(it->second, e.kind == TimedEvent::PitchBend ? (e.value + 1) * 0.5 : e.value,
                                     (int32)std::max<int64_t>(0, e.frame - pos));
                        continue;
                    }
                    Event ev{};
                    ev.busIndex = 0;
                    ev.sampleOffset = (int32)std::max<int64_t>(0, e.frame - pos);
                    ev.flags = Event::kIsLive;
                    if (e.on) {
                        ev.type = Event::kNoteOnEvent;
                        ev.noteOn.channel = (int16)e.channel;
                        ev.noteOn.pitch = (int16)e.key;
                        ev.noteOn.velocity = (float)e.velocity;
                        ev.noteOn.noteId = -1;
                    } else {
                        ev.type = Event::kNoteOffEvent;
                        ev.noteOff.channel = (int16)e.channel;
                        ev.noteOff.pitch = (int16)e.key;
                        ev.noteOff.velocity = (float)e.velocity;
                        ev.noteOff.noteId = -1;
                    }
                    eventList.addEvent(ev);
                }
            }
            // transport
            const double sec = std::max<int64_t>(0, pos) / sr, beat = job.tempo.secToBeat(sec);
            const double barBeats = job.tsigNum * 4.0 / job.tsigDen;
            ctx.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid |
                        ProcessContext::kBarPositionValid | (pos >= 0 ? ProcessContext::kPlaying : 0);
            ctx.sampleRate = sr;
            ctx.projectTimeSamples = std::max<int64_t>(0, pos);
            ctx.continousTimeSamples = pos + warm;
            ctx.projectTimeMusic = beat;
            ctx.barPositionMusic = std::floor(beat / barBeats) * barBeats;
            ctx.tempo = job.tempo.bpmAtBeat(beat);
            ctx.timeSigNumerator = job.tsigNum;
            ctx.timeSigDenominator = job.tsigDen;

            // inputs: silence, or the effect's source audio on bus 0; outputs cleared
            for (int32 b = 0; b < data.numInputs; ++b)
                for (int32 c = 0; c < data.inputs[b].numChannels; ++c) {
                    float *dst = data.inputs[b].channelBuffers32[c];
                    if (b == 0 && input && pos >= 0) {
                        const auto &src = (c % 2) ? input->right : input->left;
                        std::copy(src.begin() + pos, src.begin() + pos + n, dst);
                    } else std::fill(dst, dst + n, 0.f);
                }
            for (int32 b = 0; b < data.numOutputs; ++b)
                for (int32 c = 0; c < data.outputs[b].numChannels; ++c) std::fill(data.outputs[b].channelBuffers32[c], data.outputs[b].channelBuffers32[c] + n, 0.f);

            if (im.processor->process(data) != kResultOk) ++failures;
            if (pos >= 0) {
                const auto &bus = data.outputs[0];
                if (bus.numChannels > 0) {
                    const float *l = bus.channelBuffers32[0], *r = bus.numChannels > 1 ? bus.channelBuffers32[1] : l;
                    std::copy(l, l + n, out.left.begin() + pos);
                    std::copy(r, r + n, out.right.begin() + pos);
                }
            }
            pos += n;
        }
        im.processor->setProcessing(false);
        done = true;
    });
    while (!done) pumpFor(2);
    worker.join();
    im.component->setActive(false);
    if (failures) warnings.push_back(name_ + ": process() reported an error in " + std::to_string(failures) + " block(s)");
    if (!audioErr.empty()) { err = audioErr; return false; }
    return true;
}

} // namespace wl
