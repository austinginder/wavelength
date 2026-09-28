#pragma once
// Patch cards: how a preset sounds, for agents that can see images but can't hear. Each patch plays
// one probe phrase (a held C4, C2-C5, a C minor chord, a 16th run) in a child render; the card shows
// the held note zoomed (spectrum with note names and harmonics, level, attack and release), its
// pitch over time and waveform, the four octaves, the chord and the run, with the measurements and
// flags (sounds an octave down, long tail, slow attack, silent at C2, noise before the first note).
// One patch draws a full card; several draw a contact sheet of compact cards to compare.
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace wl {

struct CardItem {
    std::string plugin;   // id, name or path, as for render
    std::string preset;   // empty: the plugin's default sound
    std::string state;    // a state file instead of a preset
};

struct CardOptions {
    std::vector<CardItem> items;
    std::string out;      // the PNG
    std::string keep;     // keep the probe render (job, stems, report) in this folder
    int width = 1400;
    int jobs = -1;        // render workers (-1: render's default)
    bool verbose = false;
};

// Renders the probe for every item and writes the picture; `report` gets the measurements per item.
bool makeCards(const CardOptions &opt, nlohmann::json &report, std::string &err);

} // namespace wl
