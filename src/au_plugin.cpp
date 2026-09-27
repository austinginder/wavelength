#include "au_plugin.hpp"

#if defined(__APPLE__)

#include "job.hpp"
#include "midi_file.hpp"
#include "platform.hpp"
#include "state_file.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

namespace wl {

namespace {

std::string fourcc(OSType v) {
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = (char)((v >> shift) & 0xff);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

bool parseFourcc(const std::string &s, OSType &v) {
    if (s.size() != 4) return false;
    v = (OSType)((uint8_t)s[0] << 24 | (uint8_t)s[1] << 16 | (uint8_t)s[2] << 8 | (uint8_t)s[3]);
    return true;
}

std::string cfString(CFStringRef s) {
    if (!s) return "";
    char buf[1024];
    if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf;
    return "";
}

// "aumu:dls :appl" (type:subtype:manufacturer) <-> a component description
std::string idOf(const AudioComponentDescription &d) {
    return fourcc(d.componentType) + ":" + fourcc(d.componentSubType) + ":" + fourcc(d.componentManufacturer);
}

bool descOf(const std::string &id, AudioComponentDescription &d) {
    d = AudioComponentDescription{};
    if (id.size() != 14 || id[4] != ':' || id[9] != ':') return false;
    return parseFourcc(id.substr(0, 4), d.componentType) && parseFourcc(id.substr(5, 4), d.componentSubType) &&
           parseFourcc(id.substr(10, 4), d.componentManufacturer);
}

std::string statusText(OSStatus st) {
    switch (st) {
    case kAudioUnitErr_InvalidProperty: return "invalid property";
    case kAudioUnitErr_InvalidParameter: return "invalid parameter";
    case kAudioUnitErr_InvalidElement: return "invalid element";
    case kAudioUnitErr_NoConnection: return "no input connection";
    case kAudioUnitErr_FailedInitialization: return "failed to initialize";
    case kAudioUnitErr_TooManyFramesToProcess: return "too many frames to process";
    case kAudioUnitErr_FormatNotSupported: return "format not supported";
    case kAudioUnitErr_Uninitialized: return "not initialized";
    case kAudioUnitErr_InvalidScope: return "invalid scope";
    case kAudioUnitErr_Unauthorized: return "not authorized (licence)";
    case kAudioComponentErr_InstanceInvalidated: return "the plugin process ended";
    default: break;
    }
    const OSType t = (OSType)st;
    const std::string four = fourcc(t);
    return "error " + std::to_string((int)st) + (std::all_of(four.begin(), four.end(), [](char c) { return std::isalnum((unsigned char)c); }) ? " '" + four + "'" : "");
}

} // namespace

std::vector<PluginInfo> auPlugins() {
    std::vector<PluginInfo> out;
    for (OSType type : {kAudioUnitType_MusicDevice, kAudioUnitType_Effect, kAudioUnitType_MusicEffect}) {
        AudioComponentDescription want{};
        want.componentType = type;
        for (AudioComponent c = AudioComponentFindNext(nullptr, &want); c; c = AudioComponentFindNext(c, &want)) {
            AudioComponentDescription d{};
            if (AudioComponentGetDescription(c, &d) != noErr) continue;
            CFStringRef cf = nullptr;
            AudioComponentCopyName(c, &cf);
            std::string full = cfString(cf);
            if (cf) CFRelease(cf);
            PluginInfo p;
            p.format = "au";
            p.id = idOf(d);
            const size_t colon = full.find(": ");
            p.vendor = colon == std::string::npos ? fourcc(d.componentManufacturer) : full.substr(0, colon);
            p.name = colon == std::string::npos ? full : full.substr(colon + 2);
            if (p.name.empty()) p.name = p.id;
            UInt32 v = 0;
            if (AudioComponentGetVersion(c, &v) == noErr && v)
                p.version = std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 0xff) + "." + std::to_string(v & 0xff);
            p.features = type == kAudioUnitType_MusicDevice ? std::vector<std::string>{"instrument"}
                       : type == kAudioUnitType_MusicEffect ? std::vector<std::string>{"audio-effect", "note-effect"}
                                                            : std::vector<std::string>{"audio-effect"};
            if (d.componentFlags & kAudioComponentFlag_IsV3AudioUnit) p.description = "AUv3";
            out.push_back(p);
        }
    }
    return out;
}

struct AuPlugin::Impl {
    AudioUnit unit = nullptr;
    OSType type = 0;
    bool outOfProcess = false;
    double sampleRate = 48000;
    UInt32 block = 512;
    UInt32 outChannels = 2, inChannels = 2, inputBuses = 0;
    // render-time data the callbacks read (worker thread)
    const Audio *input = nullptr, *sidechain = nullptr;
    int64_t pos = 0, total = 0;
    double beat = 0, tempo = 120, barStart = 0;
    int tsigNum = 4, tsigDen = 4;
    bool playing = false;

    template <class T> OSStatus get(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, T &v) const {
        UInt32 size = sizeof(T);
        return AudioUnitGetProperty(unit, id, scope, el, &v, &size);
    }
    template <class T> OSStatus set(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const T &v) {
        return AudioUnitSetProperty(unit, id, scope, el, &v, sizeof(T));
    }
    OSType subtype = 0, manufacturer = 0;
    int program = -1;   // a General MIDI program to send before the notes (Apple's GM synths)
    bool isInstrument() const { return type == kAudioUnitType_MusicDevice; }
    // Apple's General MIDI synths pick their instrument by program change, not by preset
    bool generalMidi() const { return manufacturer == 'appl' && (subtype == 'dls ' || subtype == 'msyn'); }
    bool takesMidi() const { return type == kAudioUnitType_MusicDevice || type == kAudioUnitType_MusicEffect; }
};

namespace {

OSStatus beatAndTempo(void *user, Float64 *beat, Float64 *tempo) {
    auto *im = static_cast<AuPlugin::Impl *>(user);
    if (beat) *beat = im->beat;
    if (tempo) *tempo = im->tempo;
    return noErr;
}

OSStatus musicalTime(void *user, UInt32 *toNextBeat, Float32 *num, UInt32 *den, Float64 *downBeat) {
    auto *im = static_cast<AuPlugin::Impl *>(user);
    if (toNextBeat) {   // samples from now to the next beat
        const double frac = im->beat - std::floor(im->beat), perBeat = 60.0 / std::max(1.0, im->tempo) * im->sampleRate;
        *toNextBeat = frac > 0 ? (UInt32)std::lround((1.0 - frac) * perBeat) : 0;
    }
    if (num) *num = (Float32)im->tsigNum;
    if (den) *den = (UInt32)im->tsigDen;
    if (downBeat) *downBeat = im->barStart;
    return noErr;
}

OSStatus transportState(void *user, Boolean *playing, Boolean *recording, Boolean *changed, Float64 *sample, Boolean *cycling,
                        Float64 *cycleStart, Float64 *cycleEnd) {
    auto *im = static_cast<AuPlugin::Impl *>(user);
    if (playing) *playing = im->playing;
    if (recording) *recording = false;
    if (changed) *changed = false;
    if (sample) *sample = (Float64)std::max<int64_t>(0, im->pos);
    if (cycling) *cycling = false;
    if (cycleStart) *cycleStart = 0;
    if (cycleEnd) *cycleEnd = 0;
    return noErr;
}

// input bus 0: the effect's source; bus 1: its sidechain
OSStatus renderInput(void *user, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    auto *im = static_cast<AuPlugin::Impl *>(user);
    const Audio *feed = bus == 0 ? im->input : im->sidechain;
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b) {
        float *dst = static_cast<float *>(io->mBuffers[b].mData);
        if (!dst) continue;
        std::fill(dst, dst + frames, 0.f);
        if (!feed || im->pos < 0 || im->pos >= im->total) continue;
        const auto &src = (b % 2) ? feed->right : feed->left;
        const int64_t n = std::min<int64_t>(frames, im->total - im->pos);
        std::copy(src.begin() + im->pos, src.begin() + im->pos + n, dst);
    }
    return noErr;
}

// A buffer list of `channels` non-interleaved float buffers over `storage`
struct Buffers {
    std::vector<uint8_t> mem;
    std::vector<std::vector<float>> data;
    AudioBufferList *list() { return reinterpret_cast<AudioBufferList *>(mem.data()); }
    Buffers(UInt32 channels, UInt32 frames) : mem(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * std::max<UInt32>(1, channels)),
                                              data(channels, std::vector<float>(frames)) {
        list()->mNumberBuffers = channels;
        reset(frames);
    }
    void reset(UInt32 frames) {
        for (UInt32 c = 0; c < list()->mNumberBuffers; ++c) {
            list()->mBuffers[c].mNumberChannels = 1;
            list()->mBuffers[c].mDataByteSize = frames * sizeof(float);
            list()->mBuffers[c].mData = data[c].data();
        }
    }
};

AudioStreamBasicDescription floatFormat(double rate, UInt32 channels) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = f.mBytesPerFrame = sizeof(float);
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
}

} // namespace

AuPlugin::AuPlugin() : impl_(std::make_unique<Impl>()) {}

AuPlugin::~AuPlugin() {
    if (impl_ && impl_->unit) {
        AudioUnitUninitialize(impl_->unit);
        AudioComponentInstanceDispose(impl_->unit);
    }
}

std::unique_ptr<Plugin> AuPlugin::create(const PluginInfo &info, std::string &err) {
    AudioComponentDescription d{};
    if (!descOf(info.id, d)) { err = "not an Audio Unit id: " + info.id; return nullptr; }
    AudioComponent c = AudioComponentFindNext(nullptr, &d);
    if (!c) { err = info.name + ": the Audio Unit is not installed"; return nullptr; }
    std::unique_ptr<AuPlugin> p(new AuPlugin());
    auto &im = *p->impl_;
    im.type = d.componentType;
    im.subtype = d.componentSubType;
    im.manufacturer = d.componentManufacturer;
    AudioComponentDescription full{};
    AudioComponentGetDescription(c, &full);
    OSStatus st = -1;
    if (!(full.componentFlags & kAudioComponentFlag_IsV3AudioUnit)) st = AudioComponentInstanceNew(c, &im.unit);
    if (st != noErr || !im.unit) {
        // out of process: app-extension AUv3s, and plugins whose code can't load here (Intel-only on Apple silicon)
        struct Opened { std::atomic<bool> done{false}; AudioComponentInstance unit = nullptr; OSStatus status = noErr; };
        auto opened = std::make_shared<Opened>();   // the handler may run on another thread, even after a timeout
        AudioComponentInstantiate(c, kAudioComponentInstantiation_LoadOutOfProcess, ^(AudioComponentInstance inst, OSStatus s) {
            opened->unit = inst;
            opened->status = s;
            opened->done = true;
        });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!opened->done && std::chrono::steady_clock::now() < until) platform::pumpEvents(10);
        if (!opened->done) { err = info.name + ": the Audio Unit did not open within 30 s"; return nullptr; }
        if (opened->status != noErr || !opened->unit) {
            err = info.name + ": the Audio Unit would not open (" + statusText(st != noErr && st != -1 ? st : opened->status) + ")";
            return nullptr;
        }
        im.unit = opened->unit;
        im.outOfProcess = true;
    }
    p->id_ = info.id;
    p->name_ = info.name;
    // output channels: stereo where the unit allows it
    AudioStreamBasicDescription f{};
    if (im.get(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, f) == noErr && f.mChannelsPerFrame) im.outChannels = f.mChannelsPerFrame;
    UInt32 count = 0;
    if (im.get(kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, count) == noErr) im.inputBuses = count;
    return p;
}

void AuPlugin::pump(double ms) { platform::pumpEvents(ms); }

bool AuPlugin::getState(std::vector<uint8_t> &out, std::string &err) {
    CFPropertyListRef plist = nullptr;
    UInt32 size = sizeof(plist);
    const OSStatus st = AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size);
    if (st != noErr || !plist) { err = name_ + ": cannot read its state (" + statusText(st) + ")"; return false; }
    CFErrorRef cfErr = nullptr;
    CFDataRef data = CFPropertyListCreateData(nullptr, plist, kCFPropertyListXMLFormat_v1_0, 0, &cfErr);
    CFRelease(plist);
    if (!data) { if (cfErr) CFRelease(cfErr); err = name_ + ": its state is not a property list"; return false; }
    out.assign(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data));
    CFRelease(data);
    return true;
}

bool AuPlugin::loadState(const StateFile &sf, std::string &err) {
    if (sf.state.empty()) { err = name_ + ": empty state"; return false; }
    CFDataRef data = CFDataCreate(nullptr, sf.state.data(), (CFIndex)sf.state.size());
    CFErrorRef cfErr = nullptr;
    CFPropertyListRef plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, &cfErr);
    CFRelease(data);
    if (!plist) {
        if (cfErr) CFRelease(cfErr);
        err = name_ + ": an Audio Unit takes its state as a property list (.aupreset); this file is not one";
        return false;
    }
    const OSStatus st = AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof(plist));
    CFRelease(plist);
    if (st != noErr) { err = name_ + " did not accept the state (" + statusText(st) + "): is it a preset for this plugin?"; return false; }
    pump(20);
    return true;
}

bool AuPlugin::saveStateFile(const std::string &path, size_t &bytes, std::string &err) {
    std::vector<uint8_t> data;
    if (!getState(data, err)) return false;
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
    if (!o) { err = "cannot write " + path; return false; }
    bytes = data.size();
    return true;
}

std::vector<std::string> AuPlugin::programs() {
    std::vector<std::string> out;
    if (impl_->generalMidi()) {
        for (int i = 0; i < 128; ++i) out.push_back(gmProgramName(i));
        return out;
    }
    CFArrayRef presets = nullptr;
    UInt32 size = sizeof(presets);
    if (AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size) != noErr || !presets) return out;
    for (CFIndex i = 0; i < CFArrayGetCount(presets); ++i) {
        const AUPreset *p = static_cast<const AUPreset *>(CFArrayGetValueAtIndex(presets, i));
        out.push_back(p ? cfString(p->presetName) : "");
    }
    CFRelease(presets);
    return out;
}

bool AuPlugin::loadPreset(const std::string &query, std::string &loadedName, std::string &err) {
    auto lowerS = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    if (impl_->generalMidi()) {   // a General MIDI program by name
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < 128; ++i) {
                const std::string n = lowerS(gmProgramName(i));
                if (pass == 0 ? n == lowerS(query) : n.find(lowerS(query)) != std::string::npos) {
                    impl_->program = i;
                    loadedName = gmProgramName(i);
                    return true;
                }
            }
        err = name_ + " has no General MIDI program matching '" + query + "'";
        return false;
    }
    CFArrayRef presets = nullptr;
    UInt32 size = sizeof(presets);
    if (AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_FactoryPresets, kAudioUnitScope_Global, 0, &presets, &size) != noErr || !presets) {
        err = name_ + " has no factory presets";
        return false;
    }
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
    const AUPreset *hit = nullptr;
    for (int pass = 0; pass < 2 && !hit; ++pass)
        for (CFIndex i = 0; i < CFArrayGetCount(presets) && !hit; ++i) {
            const AUPreset *p = static_cast<const AUPreset *>(CFArrayGetValueAtIndex(presets, i));
            const std::string n = lower(cfString(p->presetName));
            if (pass == 0 ? n == lower(query) : n.find(lower(query)) != std::string::npos) hit = p;
        }
    if (!hit) { CFRelease(presets); err = name_ + " has no factory preset matching '" + query + "'"; return false; }
    AUPreset chosen = *hit;
    const OSStatus st = AudioUnitSetProperty(impl_->unit, kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0, &chosen, sizeof(chosen));
    loadedName = cfString(hit->presetName);
    CFRelease(presets);
    if (st != noErr) { err = name_ + " did not load preset '" + loadedName + "' (" + statusText(st) + ")"; return false; }
    pump(20);
    return true;
}

std::vector<ParamInfo> AuPlugin::params() const {
    std::vector<ParamInfo> out;
    AudioUnit u = impl_->unit;
    UInt32 size = 0;
    Boolean writable = false;
    if (AudioUnitGetPropertyInfo(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, &writable) != noErr || !size) return out;
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size) != noErr) return out;
    for (AudioUnitParameterID id : ids) {
        AudioUnitParameterInfo info{};
        UInt32 isz = sizeof(info);
        if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &isz) != noErr) continue;
        ParamInfo p{};
        p.id = id;
        p.cookie = nullptr;
        if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) && info.cfNameString) {
            p.name = cfString(info.cfNameString);
            if (info.flags & kAudioUnitParameterFlag_CFNameRelease) CFRelease(info.cfNameString);
        } else p.name = info.name;
        if (p.name.empty()) p.name = "Param " + std::to_string(id);
        if (info.flags & kAudioUnitParameterFlag_HasClump) {
            AudioUnitParameterNameInfo clump{};
            clump.inID = info.clumpID;
            clump.inDesiredLength = kAudioUnitParameterName_Full;
            UInt32 csz = sizeof(clump);
            if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterClumpName, kAudioUnitScope_Global, 0, &clump, &csz) == noErr && clump.outName) {
                p.module = cfString(clump.outName);
                CFRelease(clump.outName);
            }
        }
        p.min = info.minValue;
        p.max = info.maxValue;
        p.def = info.defaultValue;
        AudioUnitParameterValue v = 0;
        p.value = AudioUnitGetParameter(u, id, kAudioUnitScope_Global, 0, &v) == noErr ? v : p.def;
        p.stepped = info.unit == kAudioUnitParameterUnit_Indexed || info.unit == kAudioUnitParameterUnit_Boolean;
        p.readonly = !(info.flags & kAudioUnitParameterFlag_IsWritable);
        p.hidden = false;
        std::string text;
        const_cast<AuPlugin *>(this)->textForValue(id, p.value, text);
        p.display = text;
        out.push_back(p);
    }
    return out;
}

bool AuPlugin::textForValue(ParamId id, double plain, std::string &text) {
    AudioUnit u = impl_->unit;
    AudioUnitParameterInfo info{};
    UInt32 isz = sizeof(info);
    if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &isz) != noErr) return false;
    if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) && (info.flags & kAudioUnitParameterFlag_CFNameRelease) && info.cfNameString)
        CFRelease(info.cfNameString);
    // named values (indexed parameters)
    if (info.flags & kAudioUnitParameterFlag_ValuesHaveStrings) {
        AudioUnitParameterStringFromValue sv{};
        AudioUnitParameterValue value = (AudioUnitParameterValue)plain;
        sv.inParamID = id;
        sv.inValue = &value;
        UInt32 ssz = sizeof(sv);
        if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, 0, &sv, &ssz) == noErr && sv.outString) {
            text = cfString(sv.outString);
            CFRelease(sv.outString);
            return true;
        }
    }
    if (info.unit == kAudioUnitParameterUnit_Indexed) {
        CFArrayRef names = nullptr;
        UInt32 nsz = sizeof(names);
        if (AudioUnitGetProperty(u, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &names, &nsz) == noErr && names) {
            const CFIndex i = (CFIndex)std::lround(plain - info.minValue);
            if (i >= 0 && i < CFArrayGetCount(names)) text = cfString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, i)));
            CFRelease(names);
            if (!text.empty()) return true;
        }
    }
    static const std::pair<AudioUnitParameterUnit, const char *> units[] = {
        {kAudioUnitParameterUnit_Hertz, "Hz"}, {kAudioUnitParameterUnit_Decibels, "dB"}, {kAudioUnitParameterUnit_Seconds, "s"},
        {kAudioUnitParameterUnit_Milliseconds, "ms"}, {kAudioUnitParameterUnit_Percent, "%"}, {kAudioUnitParameterUnit_Cents, "cents"},
        {kAudioUnitParameterUnit_RelativeSemiTones, "semitones"}, {kAudioUnitParameterUnit_BPM, "BPM"}, {kAudioUnitParameterUnit_Degrees, "deg"},
        {kAudioUnitParameterUnit_Beats, "beats"}, {kAudioUnitParameterUnit_Octaves, "octaves"}};
    char buf[64];
    if (info.unit == kAudioUnitParameterUnit_Boolean) { text = plain >= 0.5 ? "On" : "Off"; return true; }
    std::snprintf(buf, sizeof buf, "%.4g", plain);
    text = buf;
    for (auto &[k, label] : units) if (info.unit == k) { text += std::string(" ") + label; break; }
    if (info.unit == kAudioUnitParameterUnit_CustomUnit && info.unitName) text += " " + cfString(info.unitName);
    return true;
}

bool AuPlugin::valueFromText(ParamId id, const std::string &text, double &plain) {
    AudioUnitParameterValueFromString vs{};
    vs.inParamID = id;
    vs.inString = CFStringCreateWithCString(nullptr, text.c_str(), kCFStringEncodingUTF8);
    UInt32 size = sizeof(vs);
    const OSStatus st = AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_ParameterValueFromString, kAudioUnitScope_Global, 0, &vs, &size);
    CFRelease(vs.inString);
    if (st == noErr) { plain = vs.outValue; return true; }
    // indexed parameters: match the value names
    CFArrayRef names = nullptr;
    UInt32 nsz = sizeof(names);
    AudioUnitParameterInfo info{};
    UInt32 isz = sizeof(info);
    if (AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &info, &isz) != noErr) return false;
    if (AudioUnitGetProperty(impl_->unit, kAudioUnitProperty_ParameterValueStrings, kAudioUnitScope_Global, id, &names, &nsz) != noErr || !names) return false;
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(names) && !found; ++i) {
        std::string n = cfString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, i))), t = text;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
        if (n == t) { plain = info.minValue + (double)i; found = true; }
    }
    CFRelease(names);
    return found;
}

bool AuPlugin::setParams(const std::vector<ParamValue> &values, std::string &err) {
    (void)err;
    for (const auto &v : values) AudioUnitSetParameter(impl_->unit, v.id, kAudioUnitScope_Global, 0, (AudioUnitParameterValue)v.value, 0);
    return true;
}

bool AuPlugin::commitParams(const std::vector<ParamValue> &values, double sampleRate, uint32_t block, std::string &err) {
    (void)sampleRate; (void)block;
    return setParams(values, err);   // Audio Unit parameters apply at once
}

bool AuPlugin::render(const Job &job, const std::vector<TimedEvent> &events, const std::vector<ParamValue> &initial,
                      const std::vector<AutoParam> &autos, const Audio *input, Audio &out,
                      std::vector<std::string> &warnings, std::string &err) {
    auto &im = *impl_;
    AudioUnit u = im.unit;
    const UInt32 block = (UInt32)job.blockSize;
    const double sr = job.sampleRate;
    if (input && im.inputBuses < 1) { err = name_ + " has no audio input, so it can't be used as an effect"; return false; }
    if (!input && !im.isInstrument() && im.inputBuses > 0 && im.type == kAudioUnitType_Effect) {
        err = name_ + " is an effect: use it in a track's \"fx\", not as its instrument";
        return false;
    }
    im.sampleRate = sr;
    im.block = block;
    AudioUnitUninitialize(u);
    // stereo float at the job's rate on the output, and on the inputs of an effect
    const UInt32 outCh = im.outChannels >= 2 ? 2 : im.outChannels;
    AudioStreamBasicDescription fmt = floatFormat(sr, outCh);
    OSStatus st = im.set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, fmt);
    if (st != noErr) { fmt = floatFormat(sr, im.outChannels); st = im.set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, fmt); }
    if (st != noErr) { err = name_ + " does not take a " + std::to_string((int)sr) + " Hz float output (" + statusText(st) + ")"; return false; }
    const UInt32 renderCh = fmt.mChannelsPerFrame;
    im.input = input;
    im.sidechain = sidechain;
    sidechainConnected = false;
    for (UInt32 bus = 0; bus < std::min<UInt32>(im.inputBuses, 2); ++bus) {
        AudioStreamBasicDescription inFmt = floatFormat(sr, 2);
        if (im.set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, bus, inFmt) != noErr) {
            inFmt = floatFormat(sr, 1);
            im.set(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, bus, inFmt);
        }
        AURenderCallbackStruct cb{renderInput, &im};
        if (im.set(kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, bus, cb) == noErr && bus == 1 && sidechain) sidechainConnected = true;
    }
    im.set(kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, block);
    HostCallbackInfo host{};
    host.hostUserData = &im;
    host.beatAndTempoProc = beatAndTempo;
    host.musicalTimeLocationProc = musicalTime;
    host.transportStateProc2 = transportState;
    im.set(kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, host);
    const UInt32 offline = 1;
    im.set(kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, offline);
    st = AudioUnitInitialize(u);
    if (st != noErr) { err = name_ + " failed to start (" + statusText(st) + ")"; return false; }
    pump((warmup >= 0 ? warmup : job.warmup) * 1000.0);
    setParams(initial, err);

    Float64 latency = 0;
    im.get(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, latency);
    const int64_t total = (int64_t)out.frames();
    const int64_t lat = std::clamp<int64_t>((int64_t)std::llround(latency * sr), 0, (int64_t)(10 * sr));
    latencySamples = (uint32_t)lat;
    im.total = total;
    Buffers bufs(renderCh, block);
    std::atomic<bool> done{false};
    std::string renderErr;
    std::thread worker([&] {
        const int64_t warm = (int64_t)(0.1 * sr), end = total + lat;
        size_t next = 0;
        std::vector<float> lastAuto(autos.size(), NAN);
        const double barBeats = job.tsigNum * 4.0 / job.tsigDen;
        for (int64_t pos = -warm; pos < end;) {
            const UInt32 n = (UInt32)std::min<int64_t>(block, pos < 0 ? -pos : end - pos);
            im.pos = pos;
            const double sec = std::max<int64_t>(0, pos) / sr;
            im.beat = job.tempo.secToBeat(sec);
            im.tempo = job.tempo.bpmAtBeat(im.beat);
            im.barStart = std::floor(im.beat / barBeats) * barBeats;
            im.tsigNum = job.tsigNum;
            im.tsigDen = job.tsigDen;
            im.playing = pos >= 0;
            if (pos >= 0 && pos < block) for (const auto &v : initial) AudioUnitSetParameter(u, v.id, kAudioUnitScope_Global, 0, (AudioUnitParameterValue)v.value, 0);
            if (pos >= 0)
                for (size_t a = 0; a < autos.size(); ++a) {
                    const float v = (float)autos[a].env.at(pos / sr);
                    if (v != lastAuto[a]) { AudioUnitSetParameter(u, autos[a].id, kAudioUnitScope_Global, 0, v, 0); lastAuto[a] = v; }
                }
            if (im.program >= 0 && pos == -warm)   // the GM program, before the warm-up so it has loaded
                for (UInt32 ch = 0; ch < 16; ++ch) if (ch != 9) MusicDeviceMIDIEvent(u, 0xC0 | ch, (UInt32)im.program, 0, 0);
            // MIDI for this block, at its offset
            if (pos >= 0 && im.takesMidi())
                while (next < events.size() && events[next].frame < pos + n) {
                    const auto &e = events[next++];
                    const UInt32 off = (UInt32)std::max<int64_t>(0, e.frame - pos);
                    const UInt32 ch = (UInt32)std::clamp(e.channel, 0, 15);
                    if (e.kind == TimedEvent::Note)
                        MusicDeviceMIDIEvent(u, (e.on ? 0x90 : 0x80) | ch, (UInt32)std::clamp(e.key, 0, 127),
                                             (UInt32)std::clamp((int)std::lround(e.velocity * 127), e.on ? 1 : 0, 127), off);
                    else if (e.kind == TimedEvent::CC)
                        MusicDeviceMIDIEvent(u, 0xB0 | ch, (UInt32)std::clamp(e.number, 0, 127), (UInt32)std::clamp((int)std::lround(e.value * 127), 0, 127), off);
                    else if (e.kind == TimedEvent::PitchBend) {
                        const int v = std::clamp((int)std::lround((e.value + 1) * 8192), 0, 16383);
                        MusicDeviceMIDIEvent(u, 0xE0 | ch, (UInt32)(v & 0x7f), (UInt32)(v >> 7), off);
                    } else MusicDeviceMIDIEvent(u, 0xD0 | ch, (UInt32)std::clamp((int)std::lround(e.value * 127), 0, 127), 0, off);
                }
            bufs.reset(n);
            AudioTimeStamp ts{};
            ts.mSampleTime = (Float64)(pos + warm);
            ts.mFlags = kAudioTimeStampSampleTimeValid;
            AudioUnitRenderActionFlags flags = 0;
            const OSStatus rs = AudioUnitRender(u, &flags, &ts, 0, n, bufs.list());
            if (rs != noErr) { renderErr = name_ + " failed while rendering (" + statusText(rs) + ")"; break; }
            if (pos >= 0) {
                const float *l = static_cast<const float *>(bufs.list()->mBuffers[0].mData);
                const float *r = renderCh > 1 ? static_cast<const float *>(bufs.list()->mBuffers[1].mData) : l;
                for (UInt32 i = 0; i < n; ++i) {
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
    while (!done) pump(2);
    worker.join();
    im.input = im.sidechain = nullptr;
    AudioUnitReset(u, kAudioUnitScope_Global, 0);
    if (!renderErr.empty()) { err = renderErr; return false; }
    (void)warnings;
    return true;
}

} // namespace wl

#else   // not macOS: no Audio Units

namespace wl {
struct AuPlugin::Impl {};
std::vector<PluginInfo> auPlugins() { return {}; }
AuPlugin::AuPlugin() {}
AuPlugin::~AuPlugin() {}
std::unique_ptr<Plugin> AuPlugin::create(const PluginInfo &info, std::string &err) { err = info.name + ": Audio Units exist only on macOS"; return nullptr; }
void AuPlugin::pump(double) {}
bool AuPlugin::loadState(const StateFile &, std::string &err) { err = "no Audio Units here"; return false; }
bool AuPlugin::loadPreset(const std::string &, std::string &, std::string &err) { err = "no Audio Units here"; return false; }
std::vector<std::string> AuPlugin::programs() { return {}; }
bool AuPlugin::saveStateFile(const std::string &, size_t &, std::string &err) { err = "no Audio Units here"; return false; }
bool AuPlugin::getState(std::vector<uint8_t> &, std::string &err) { err = "no Audio Units here"; return false; }
bool AuPlugin::valueFromText(ParamId, const std::string &, double &) { return false; }
bool AuPlugin::textForValue(ParamId, double, std::string &) { return false; }
std::vector<ParamInfo> AuPlugin::params() const { return {}; }
bool AuPlugin::setParams(const std::vector<ParamValue> &, std::string &) { return false; }
bool AuPlugin::commitParams(const std::vector<ParamValue> &, double, uint32_t, std::string &) { return false; }
bool AuPlugin::render(const Job &, const std::vector<TimedEvent> &, const std::vector<ParamValue> &, const std::vector<AutoParam> &,
                      const Audio *, Audio &, std::vector<std::string> &, std::string &err) { err = "no Audio Units here"; return false; }
} // namespace wl

#endif
