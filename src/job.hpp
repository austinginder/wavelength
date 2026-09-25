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
    std::vector<std::pair<double, double>> bend;   // (seconds after the note start, semitones); empty = none
};

struct ParamSetting {
    std::string key;        // "#id", "Name" or "Module/Name"
    double value;           // plain value in the parameter's own range
    std::string text;       // or the plugin's display text ("800 Hz"), parsed by the plugin
};

struct Track {
    std::string name, plugin;
    std::string stateFile, stateFormat, preset;
    std::vector<ParamSetting> params;
    std::vector<Note> notes;
    double gainDb = 0, pan = 0;
    double warmup = -1;                                          // seconds; < 0 = job warmup
    bool mute = false;
    nlohmann::json fx = nlohmann::json::array();               // effect chain, in order
    std::vector<std::pair<std::string, double>> sends;          // bus name → dB (post-fader)
    Envelope gainAutomation;                                     // fader dB over time (empty = none)
    Envelope panAutomation;                                      // pan -1..1 over time (empty = static pan)
    std::vector<std::pair<int, Envelope>> ccAutomation;          // MIDI CC number → value 0..127 over time
    Envelope bendAutomation;                                     // pitch bend in semitones over time
    Envelope pressureAutomation;                                 // channel pressure 0..127 over time
    double bendRange = 2;                                        // the plugin's pitch-bend range, semitones
    std::vector<std::pair<std::string, Envelope>> paramAutomation;   // plugin parameter curves
    nlohmann::json sampler;                                      // builtin:sampler settings
    nlohmann::json clips = nlohmann::json::array();              // builtin:audio clips
    std::string output;                                          // bus to feed instead of the master ("" = master)
    std::vector<std::pair<std::string, Envelope>> sendAutomation;   // bus name → send dB over time
    std::vector<std::string> warnings;                           // found while parsing (range, articulations)
};

struct Bus {
    std::string name;
    double gainDb = 0;
    nlohmann::json fx = nlohmann::json::array();
    std::string output;             // another bus to feed ("" = master)
    Envelope gainAutomation;        // dB added to the bus gain over time
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
    Envelope masterGainAutomation;  // dB added to the master gain over time (fades)
    bool hasMasterLoudness = false;
    double masterLoudness = -14;    // target integrated LUFS after the master chain (gain into the chain is found)
    int stemBits = 32;              // 32 float, 24, 16, or 0 = no stem files
    std::vector<Marker> markers;  // sections for per-section loudness in the report
    std::string baseDir;        // relative paths resolve from here
    std::string sourcePath;     // the job file (worker processes re-read it); empty = render in process
    int parallel = -1;          // plugin tracks rendered at once in worker processes; 0 = all in this process; <0 = auto
};

bool parseJob(const nlohmann::json &j, const std::string &baseDir, Job &out, std::string &err);
int parseKey(const nlohmann::json &k);   // 60, "C4", "F#3", "Bb5" (C4 = 60)

} // namespace wl
