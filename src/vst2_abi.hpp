#pragma once
// The VST 2 plugin binary interface, declared from its publicly documented layout so Wavelength
// can host VST 2 plugins without Steinberg's (withdrawn) VST 2 SDK. Only what a host needs: the
// plugin struct, the dispatcher and host-callback opcodes, MIDI events and the transport struct.
// Names are Wavelength's own; the values are the de facto ABI every VST 2 host and plugin shares.
#include <cstdint>
#include <string>

namespace wl::vst2 {

struct Effect;
using HostCallback = intptr_t (*)(Effect *effect, int32_t opcode, int32_t index, intptr_t value, void *ptr, float opt);
using Dispatcher = intptr_t (*)(Effect *effect, int32_t opcode, int32_t index, intptr_t value, void *ptr, float opt);
using ProcessFn = void (*)(Effect *effect, float **inputs, float **outputs, int32_t frames);
using ProcessDoubleFn = void (*)(Effect *effect, double **inputs, double **outputs, int32_t frames);
using SetParamFn = void (*)(Effect *effect, int32_t index, float value);
using GetParamFn = float (*)(Effect *effect, int32_t index);
using EntryFn = Effect *(*)(HostCallback host);

constexpr int32_t kMagic = 0x56737450;   // 'VstP'

struct Effect {
    int32_t magic;
    Dispatcher dispatcher;
    ProcessFn processAccumulating;        // deprecated in-place accumulating process
    SetParamFn setParameter;
    GetParamFn getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs;
    int32_t flags;
    intptr_t reserved1, reserved2;
    int32_t initialDelay;                 // latency in samples
    int32_t realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueId, version;
    ProcessFn processReplacing;
    ProcessDoubleFn processDoubleReplacing;
    char future[56];
};

// Effect::flags
enum : int32_t {
    kFlagHasEditor = 1 << 0,
    kFlagCanReplacing = 1 << 4,
    kFlagProgramChunks = 1 << 5,
    kFlagIsSynth = 1 << 8,
    kFlagNoSoundInStop = 1 << 9,
    kFlagCanDoubleReplacing = 1 << 12,
};

// host -> plugin (dispatcher)
enum : int32_t {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
    effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effString2Parameter = 27,
    effGetProgramNameIndexed = 29, effGetPlugCategory = 35, effSetSpeakerArrangement = 42,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48, effGetVendorVersion = 49,
    effCanDo = 51, effGetTailSize = 52, effGetVstVersion = 58, effBeginSetProgram = 67, effEndSetProgram = 68,
    effStartProcess = 71, effStopProcess = 72, effSetProcessPrecision = 77,
};

// plugin -> host (callback)
enum : int32_t {
    hostAutomate = 0, hostVersion = 1, hostCurrentId = 2, hostIdle = 3, hostGetTime = 7, hostProcessEvents = 8,
    hostIoChanged = 13, hostSizeWindow = 15, hostGetSampleRate = 16, hostGetBlockSize = 17,
    hostGetInputLatency = 18, hostGetOutputLatency = 19, hostGetCurrentProcessLevel = 23,
    hostGetAutomationState = 24, hostGetVendorString = 32, hostGetProductString = 33, hostGetVendorVersion = 34,
    hostVendorSpecific = 35, hostCanDo = 37, hostGetLanguage = 38, hostGetDirectory = 41, hostUpdateDisplay = 42,
    hostBeginEdit = 43, hostEndEdit = 44,
};

// effGetPlugCategory results that matter to a host
enum : int32_t { kCategoryShell = 10 };

// hostGetCurrentProcessLevel results
enum : int32_t { kProcessLevelRealtime = 2, kProcessLevelOffline = 4 };

struct MidiEvent {
    int32_t type;          // 1 = MIDI
    int32_t byteSize;      // sizeof(MidiEvent)
    int32_t deltaFrames;   // sample offset in the block
    int32_t flags;         // 1 = realtime
    int32_t noteLength, noteOffset;
    char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
};

struct Events {            // variable length: numEvents pointers follow
    int32_t numEvents;
    intptr_t reserved;
    void *events[2];
};

struct TimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};

// TimeInfo::flags
enum : int32_t {
    kTransportChanged = 1, kTransportPlaying = 2, kPpqPosValid = 1 << 9, kTempoValid = 1 << 10,
    kBarsValid = 1 << 11, kTimeSigValid = 1 << 13,
};

inline std::string fourcc(int32_t id) {
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = (char)((id >> shift) & 0xff);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

} // namespace wl::vst2
