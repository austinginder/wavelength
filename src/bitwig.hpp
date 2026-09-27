#pragma once
// Bitwig Studio project files (.bwproject): the devices a DAWproject export leaves out. Bitwig's own
// devices (Drum Machine pads, Chain, EQ+, Compressor, Multiband FX-3, ...) are only names in a
// DAWproject; the .bwproject holds their settings, the plugins inside them and those plugins' states.
// The format is private and undocumented: a typed field stream read here from its observed layout
// (Bitwig 4 to 6). It also holds the arrangement (tempo, mixer, note clips, automation), so a project
// imports without a DAWproject export (see importBitwig in dawproject.hpp).
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wl::bitwig {

struct Device {
    std::string kind;       // "vst3", "clap", "vst2" or "native" (Bitwig's own)
    std::string name;       // as shown in Bitwig
    std::string pluginId;   // VST3 class id (hex), CLAP id, VST2 four-character id
    std::string state;      // zip entry of the plugin's saved state ("" when none)
    bool enabled = true;
    std::map<std::string, double> params;                 // native: parameter id -> stored value
    std::map<std::string, std::vector<Device>> chains;    // native containers: chain id -> devices
    std::string sample;     // Sampler: the sample (a Bitwig package path "Vendor/Package:ver/samples/..." or a file path)
    int sampleRoot = 60;    // Sampler: the key the sample plays at its own pitch
    std::string multisample;   // Sampler with key zones: the multisample's name ("808 BD (Round-Robin)"); `sample` is its first zone
    struct Pad { int key = 36; double volume = 1, pan = 0; bool mute = false; std::vector<Device> devices; };
    std::vector<Pad> pads;                                // Drum Machine
};

struct Note {
    double time = 0, duration = 0;   // beats, in the clip's content time
    int key = 60;
    double velocity = 0.8, release = 0.8;
};

// An arranger clip. Content time `playStart` sounds at `time`; a looping clip then repeats
// loopStart..loopEnd until `time + duration` (DAWproject's Clip attributes, same meaning).
struct Clip {
    double time = 0, duration = 0, playStart = 0;
    bool loop = false;
    double loopStart = 0, loopEnd = 0;
    std::vector<Note> notes;
};

// An arranger automation lane. Values as Bitwig stores them: volume as amplitude^(1/3), pan -1..1,
// a plugin parameter normalized 0..1.
struct Automation {
    std::string kind;       // "volume", "pan", "param" (a parameter of a plugin at the top of the track) or "other"
    std::string device, parameter;   // as Bitwig names them
    std::string paramId;    // "param": the plugin's parameter id (CLAP/VST3, decimal) or VST2 index
    std::vector<std::pair<double, double>> points;   // beat, value
};

struct Send {
    int effect = -1;        // index into Project::effects
    double amount = 0;      // amplitude
};

struct Track {
    std::string name;       // often empty: Bitwig then shows the instrument's name
    std::vector<Device> devices;
    double volume = 1, pan = 0;   // fader as amplitude, pan -1..1
    bool mute = false;
    std::vector<Send> sends;
    std::vector<Clip> clips;      // note clips in the arranger
    size_t audioClips = 0;        // audio clips in the arranger (not read yet)
    std::vector<Automation> automation;
    size_t unreadAutomation = 0;  // Bitwig 6 automation clips (not read yet)
};

struct Project {
    std::string path;
    int format = 0;                  // file format revision (0xba = Bitwig 5, 0xc0 = Bitwig 6)
    std::vector<Track> tracks;       // instrument/audio tracks in arranger order (groups flattened)
    std::vector<Track> effects;      // effect (return) tracks
    Track master;
    bool hasMaster = false;
    double tempo = 120;
    int numerator = 4, denominator = 4;
    bool meterChecked = true;        // false: a time signature other than 4/4 (its encoding is inferred)
};

bool load(const std::string &path, Project &out, std::string &err);
// the bytes of a plugin state named by Device::state
bool readState(const Project &p, const std::string &entry, std::vector<uint8_t> &data, std::string &err);

// Bitwig stores frequencies as MIDI pitch (69 = 440 Hz)
inline double pitchToHz(double pitch) { return 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0); }

} // namespace wl::bitwig
