#pragma once
// A render job: the JSON document agents write. See docs/job-format.md.
#include "automation.hpp"
#include "tempo.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace wl {

struct Note {
    double start, length;   // seconds
    int key, channel;
    double velocity;        // 0..1
};

struct ParamSetting {
    std::string key;        // "#id", "Name" or "Module/Name"
    double value;           // plain value in the parameter's own range
};

struct Track {
    std::string name, plugin;
    std::string stateFile, stateFormat;
    std::vector<ParamSetting> params;
    std::vector<Note> notes;
    double gainDb = 0, pan = 0;
    double warmup = -1;                                          // seconds; < 0 = job warmup
    bool mute = false;
    nlohmann::json fx = nlohmann::json::array();               // effect chain, in order
    std::vector<std::pair<std::string, double>> sends;          // bus name → dB (post-fader)
    Envelope gainAutomation;                                     // fader dB over time (empty = none)
    std::vector<std::pair<std::string, Envelope>> paramAutomation;   // plugin parameter curves
};

struct Bus {
    std::string name;
    double gainDb = 0;
    nlohmann::json fx = nlohmann::json::array();
};

struct Marker {
    double beat, sec;
    std::string name;
};

struct Job {
    int sampleRate = 48000;
    int blockSize = 512;
    TempoMap tempo;
    int tsigNum = 4, tsigDen = 4;
    double tail = 3.0;          // seconds rendered after the last note-off
    double warmup = 0.4;        // seconds of wall-clock settling after activation
    double length = 0;          // optional fixed length in seconds (0 = auto)
    bool hasNormalize = false;
    double normalizeDb = -1.0;  // mix peak target when normalizing
    std::vector<Track> tracks;
    std::vector<Bus> buses;
    nlohmann::json masterFx = nlohmann::json::array();
    double masterGainDb = 0;
    std::vector<Marker> markers;  // sections for per-section loudness in the report
    std::string baseDir;        // relative paths resolve from here
};

bool parseJob(const nlohmann::json &j, const std::string &baseDir, Job &out, std::string &err);
int parseKey(const nlohmann::json &k);   // 60, "C4", "F#3", "Bb5" (C4 = 60)

} // namespace wl
